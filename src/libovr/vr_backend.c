#include "vr_backend.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

/* openvr_capi.h redefines EXTERN_C (winnt.h already defined it) and, via a
 * `__WIN32` typo, redeclares bool when stdbool.h has already provided it.
 * Clear both so its own definitions apply cleanly. */
#undef EXTERN_C
#undef __WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "openvr_capi.h"
#pragma GCC diagnostic pop

#include "../common/log.h"

#define LOG(...) stub_log("vr-backend", __VA_ARGS__)

/* openvr_api.dll exports, resolved at runtime. */
typedef intptr_t (*VR_InitInternal_t)(EVRInitError *, EVRApplicationType);
typedef void (*VR_ShutdownInternal_t)(void);
typedef bool (*VR_IsHmdPresent_t)(void);
typedef intptr_t (*VR_GetGenericInterface_t)(const char *, EVRInitError *);
typedef const char *(*VR_GetErrDesc_t)(EVRInitError);

static VR_InitInternal_t p_InitInternal;
static VR_ShutdownInternal_t p_ShutdownInternal;
static VR_IsHmdPresent_t p_IsHmdPresent;
static VR_GetGenericInterface_t p_GetGenericInterface;
static VR_GetErrDesc_t p_GetErrDesc;

static struct VR_IVRSystem_FnTable *g_system;
static struct VR_IVRCompositor_FnTable *g_compositor;
static int g_active;

static void *resolve(HMODULE m, const char *name)
{
    return (void *)GetProcAddress(m, name);
}

/* Load openvr_api.dll. Launched via `proton run` (not as a Steam VR app),
 * Proton doesn't put it on the DLL search path, so after the bare name we
 * also try a copy next to this shim, which ovrl prefix-setup places there. */
static HMODULE load_openvr_api(void)
{
    HMODULE m = LoadLibraryW(L"openvr_api.dll");
    if (m)
        return m;

    HMODULE self = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)&load_openvr_api, &self);
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/')
        n--;
    path[n] = 0;
    if (wcslen(path) + 15 < MAX_PATH) {
        wcscat(path, L"openvr_api.dll");
        m = LoadLibraryW(path);
        if (m)
            LOG("loaded openvr_api.dll from %ls", path);
    }
    return m;
}

bool vr_backend_init(void)
{
    if (g_active)
        return true;

    HMODULE ovr = load_openvr_api();
    if (!ovr) {
        LOG("openvr_api.dll not found (error %lu); no SteamVR backend. "
            "Run `ovrl prefix-setup` with SteamVR installed so it is copied "
            "into the prefix.", GetLastError());
        return false;
    }
    p_InitInternal = (VR_InitInternal_t)resolve(ovr, "VR_InitInternal");
    p_ShutdownInternal = (VR_ShutdownInternal_t)resolve(ovr, "VR_ShutdownInternal");
    p_IsHmdPresent = (VR_IsHmdPresent_t)resolve(ovr, "VR_IsHmdPresent");
    p_GetGenericInterface = (VR_GetGenericInterface_t)resolve(ovr, "VR_GetGenericInterface");
    p_GetErrDesc = (VR_GetErrDesc_t)resolve(ovr, "VR_GetVRInitErrorAsEnglishDescription");
    if (!p_InitInternal || !p_GetGenericInterface || !p_ShutdownInternal) {
        LOG("openvr_api.dll missing expected exports");
        return false;
    }

    EVRInitError err = EVRInitError_VRInitError_None;
    p_InitInternal(&err, EVRApplicationType_VRApplication_Scene);
    if (err != EVRInitError_VRInitError_None) {
        LOG("VR_InitInternal failed: %s",
            p_GetErrDesc ? p_GetErrDesc(err) : "unknown error");
        return false;
    }

    /* The FnTable: prefix asks OpenVR for a C function table rather than a
     * C++ vtable. The version strings come from the vendored header; if the
     * installed runtime only offers another version this returns NULL. */
    char name[128];
    err = EVRInitError_VRInitError_None;
    snprintf(name, sizeof(name), "FnTable:%s", IVRSystem_Version);
    g_system = (struct VR_IVRSystem_FnTable *)p_GetGenericInterface(name, &err);
    EVRInitError err2 = EVRInitError_VRInitError_None;
    snprintf(name, sizeof(name), "FnTable:%s", IVRCompositor_Version);
    g_compositor = (struct VR_IVRCompositor_FnTable *)p_GetGenericInterface(name, &err2);
    if (!g_system || !g_compositor) {
        LOG("could not get OpenVR interfaces (%s / %s); system=%p compositor=%p",
            IVRSystem_Version, IVRCompositor_Version, (void *)g_system,
            (void *)g_compositor);
        p_ShutdownInternal();
        return false;
    }

    /* Our poses are in standing space; make the compositor's match, since
     * it interprets submitted render poses in its own tracking space. */
    if (g_compositor->SetTrackingSpace)
        g_compositor->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseStanding);
    g_active = 1;
    LOG("OpenVR backend up (%s, %s)", IVRSystem_Version, IVRCompositor_Version);
    return true;
}

void vr_backend_shutdown(void)
{
    if (g_active && p_ShutdownInternal)
        p_ShutdownInternal();
    g_active = 0;
    g_system = NULL;
    g_compositor = NULL;
}

bool vr_backend_active(void)
{
    return g_active != 0;
}

bool vr_backend_hmd_present(void)
{
    return p_IsHmdPresent ? p_IsHmdPresent() : false;
}

int vr_backend_dxgi_adapter_index(void)
{
    int32_t index = -1;
    if (g_active && g_system && g_system->GetDXGIOutputInfo)
        g_system->GetDXGIOutputInfo(&index);
    return index;
}

void vr_backend_render_target_size(int *width, int *height)
{
    uint32_t w = 0, h = 0;
    if (g_active)
        g_system->GetRecommendedRenderTargetSize(&w, &h);
    if (!w || !h) {   /* a sane default if the runtime declines to answer */
        w = 1344;
        h = 1600;
    }
    *width = (int)w;
    *height = (int)h;
}

ovrFovPort vr_backend_fov(ovrEyeType eye)
{
    ovrFovPort fov = {1.0f, 1.0f, 1.0f, 1.0f};
    if (!g_active)
        return fov;
    /* OpenVR gives raw projection tangents (left/right/top/bottom).
     * Despite the names, "top" and "bottom" are y-down: building the
     * projection from them (OpenVR's ComposeProjection) maps a direction
     * with tangent `b` to the top of the image, so `b` is the UP extent and
     * `t` (negative) the DOWN one. Reading them the other way round told
     * games the Quest 3 sees more up than down (1.43 vs 0.97 when it's the
     * reverse); the frame then didn't match the compositor's projection
     * and the world zoomed and swam with head motion. */
    float l = 0, r = 0, t = 0, b = 0;
    g_system->GetProjectionRaw((EVREye)eye, &l, &r, &t, &b);
    fov.LeftTan = fabsf(l);
    fov.RightTan = fabsf(r);
    fov.UpTan = fabsf(b);
    fov.DownTan = fabsf(t);
    return fov;
}

/* Convert an OpenVR 3x4 row-major rigid transform to a LibOVR pose. */
static ovrPosef pose_from_matrix(const HmdMatrix34_t *m)
{
    ovrPosef p;
    memset(&p, 0, sizeof(p));
    p.Position.x = m->m[0][3];
    p.Position.y = m->m[1][3];
    p.Position.z = m->m[2][3];

    float trace = m->m[0][0] + m->m[1][1] + m->m[2][2];
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        p.Orientation.w = 0.25f * s;
        p.Orientation.x = (m->m[2][1] - m->m[1][2]) / s;
        p.Orientation.y = (m->m[0][2] - m->m[2][0]) / s;
        p.Orientation.z = (m->m[1][0] - m->m[0][1]) / s;
    } else if (m->m[0][0] > m->m[1][1] && m->m[0][0] > m->m[2][2]) {
        float s = sqrtf(1.0f + m->m[0][0] - m->m[1][1] - m->m[2][2]) * 2.0f;
        p.Orientation.w = (m->m[2][1] - m->m[1][2]) / s;
        p.Orientation.x = 0.25f * s;
        p.Orientation.y = (m->m[0][1] + m->m[1][0]) / s;
        p.Orientation.z = (m->m[0][2] + m->m[2][0]) / s;
    } else if (m->m[1][1] > m->m[2][2]) {
        float s = sqrtf(1.0f + m->m[1][1] - m->m[0][0] - m->m[2][2]) * 2.0f;
        p.Orientation.w = (m->m[0][2] - m->m[2][0]) / s;
        p.Orientation.x = (m->m[0][1] + m->m[1][0]) / s;
        p.Orientation.y = 0.25f * s;
        p.Orientation.z = (m->m[1][2] + m->m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + m->m[2][2] - m->m[0][0] - m->m[1][1]) * 2.0f;
        p.Orientation.w = (m->m[1][0] - m->m[0][1]) / s;
        p.Orientation.x = (m->m[0][2] + m->m[2][0]) / s;
        p.Orientation.y = (m->m[1][2] + m->m[2][1]) / s;
        p.Orientation.z = 0.25f * s;
    }
    return p;
}

ovrPosef vr_backend_eye_pose(ovrEyeType eye)
{
    ovrPosef p;
    memset(&p, 0, sizeof(p));
    p.Orientation.w = 1.0f;
    if (!g_active)
        return p;
    HmdMatrix34_t m = g_system->GetEyeToHeadTransform((EVREye)eye);
    return pose_from_matrix(&m);
}

float vr_backend_display_hz(void)
{
    if (!g_active)
        return 90.0f;
    ETrackedPropertyError err = ETrackedPropertyError_TrackedProp_Success;
    float hz = g_system->GetFloatTrackedDeviceProperty(
        k_unTrackedDeviceIndex_Hmd, ETrackedDeviceProperty_Prop_DisplayFrequency_Float, &err);
    return (err == ETrackedPropertyError_TrackedProp_Success && hz > 1.0f) ? hz : 90.0f;
}

/* Valve's recipe for "seconds to photons": the rest of this frame, plus the
 * display's vsync-to-photons latency. */
double vr_backend_seconds_to_photons(void)
{
    float hz = vr_backend_display_hz();
    if (!g_active)
        return 1.0 / hz;
    float since_vsync = 0.0f;
    uint64_t frame = 0;
    g_system->GetTimeSinceLastVsync(&since_vsync, &frame);
    ETrackedPropertyError err = ETrackedPropertyError_TrackedProp_Success;
    float to_photons = g_system->GetFloatTrackedDeviceProperty(
        k_unTrackedDeviceIndex_Hmd, ETrackedDeviceProperty_Prop_SecondsFromVsyncToPhotons_Float,
        &err);
    if (err != ETrackedPropertyError_TrackedProp_Success || to_photons < 0.0f)
        to_photons = 0.0f;
    double s = 1.0 / hz - since_vsync + to_photons;
    return s > 0.0 ? s : 0.0;
}

ovrPoseStatef vr_backend_head_pose(float predict_seconds)
{
    ovrPoseStatef s;
    memset(&s, 0, sizeof(s));
    s.ThePose.Orientation.w = 1.0f;
    if (!g_active)
        return s;

    TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
    memset(poses, 0, sizeof(poses));
    /* Standing-space poses, predicted to the time the caller asked about
     * (usually when its frame will be displayed). */
    g_system->GetDeviceToAbsoluteTrackingPose(
        ETrackingUniverseOrigin_TrackingUniverseStanding, predict_seconds, poses,
        k_unMaxTrackedDeviceCount);

    const TrackedDevicePose_t *hmd = &poses[k_unTrackedDeviceIndex_Hmd];
    if (hmd->bPoseIsValid) {
        s.ThePose = pose_from_matrix(&hmd->mDeviceToAbsoluteTracking);
        s.AngularVelocity.x = hmd->vAngularVelocity.v[0];
        s.AngularVelocity.y = hmd->vAngularVelocity.v[1];
        s.AngularVelocity.z = hmd->vAngularVelocity.v[2];
        s.LinearVelocity.x = hmd->vVelocity.v[0];
        s.LinearVelocity.y = hmd->vVelocity.v[1];
        s.LinearVelocity.z = hmd->vVelocity.v[2];
    }
    return s;
}

static void fill_pose_state(ovrPoseStatef *s, const TrackedDevicePose_t *p)
{
    s->ThePose = pose_from_matrix(&p->mDeviceToAbsoluteTracking);
    s->AngularVelocity.x = p->vAngularVelocity.v[0];
    s->AngularVelocity.y = p->vAngularVelocity.v[1];
    s->AngularVelocity.z = p->vAngularVelocity.v[2];
    s->LinearVelocity.x = p->vVelocity.v[0];
    s->LinearVelocity.y = p->vVelocity.v[1];
    s->LinearVelocity.z = p->vVelocity.v[2];
}

bool vr_backend_controller(int hand, struct vr_controller *out, float predict_seconds)
{
    memset(out, 0, sizeof(*out));
    out->pose.ThePose.Orientation.w = 1.0f;
    if (!g_active)
        return false;
    TrackedDeviceIndex_t idx = g_system->GetTrackedDeviceIndexForControllerRole(
        hand ? ETrackedControllerRole_TrackedControllerRole_RightHand
             : ETrackedControllerRole_TrackedControllerRole_LeftHand);
    if (idx == k_unTrackedDeviceIndexInvalid || idx >= k_unMaxTrackedDeviceCount)
        return false;
    VRControllerState_t st;
    memset(&st, 0, sizeof(st));
    if (!g_system->GetControllerState(idx, &st, sizeof(st)))
        return false;
    out->connected = 1;
    out->pressed = st.ulButtonPressed;
    out->touched = st.ulButtonTouched;
    for (int i = 0; i < 5; i++) {
        out->axis[i][0] = st.rAxis[i].x;
        out->axis[i][1] = st.rAxis[i].y;
    }
    TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
    memset(poses, 0, sizeof(poses));
    g_system->GetDeviceToAbsoluteTrackingPose(
        ETrackingUniverseOrigin_TrackingUniverseStanding, predict_seconds, poses,
        k_unMaxTrackedDeviceCount);
    if (poses[idx].bPoseIsValid) {
        fill_pose_state(&out->pose, &poses[idx]);
        out->pose_valid = 1;
    }
    return true;
}

bool vr_backend_begin_frame(void)
{
    if (!g_active)
        return false;
    /* Block until the compositor hands us this frame's render poses. We don't
     * use the returned poses directly yet (the game queried them via
     * ovr_GetTrackingState); the call also paces rendering to the display. */
    TrackedDevicePose_t render[k_unMaxTrackedDeviceCount];
    EVRCompositorError e = g_compositor->WaitGetPoses(render, k_unMaxTrackedDeviceCount,
                                                      NULL, 0);
    if (e != EVRCompositorError_VRCompositorError_None) {
        static int logged;
        if (logged++ < 5)
            LOG("compositor WaitGetPoses failed: %d", (int)e);
    }
    return e == EVRCompositorError_VRCompositorError_None;
}

/* Rigid transform helpers on OpenVR's 3x4 row-major matrices. */
static HmdMatrix34_t matrix_from_pose(const ovrPosef *p)
{
    float x = p->Orientation.x, y = p->Orientation.y, z = p->Orientation.z,
          w = p->Orientation.w;
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n > 0.0f) {
        x /= n; y /= n; z /= n; w /= n;
    }
    HmdMatrix34_t m;
    m.m[0][0] = 1 - 2 * (y * y + z * z);
    m.m[0][1] = 2 * (x * y - z * w);
    m.m[0][2] = 2 * (x * z + y * w);
    m.m[1][0] = 2 * (x * y + z * w);
    m.m[1][1] = 1 - 2 * (x * x + z * z);
    m.m[1][2] = 2 * (y * z - x * w);
    m.m[2][0] = 2 * (x * z - y * w);
    m.m[2][1] = 2 * (y * z + x * w);
    m.m[2][2] = 1 - 2 * (x * x + y * y);
    m.m[0][3] = p->Position.x;
    m.m[1][3] = p->Position.y;
    m.m[2][3] = p->Position.z;
    return m;
}

static HmdMatrix34_t matrix_inverse_rigid(const HmdMatrix34_t *a)
{
    HmdMatrix34_t r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i][j] = a->m[j][i];
    for (int i = 0; i < 3; i++)
        r.m[i][3] = -(r.m[i][0] * a->m[0][3] + r.m[i][1] * a->m[1][3] +
                      r.m[i][2] * a->m[2][3]);
    return r;
}

static HmdMatrix34_t matrix_mul(const HmdMatrix34_t *a, const HmdMatrix34_t *b)
{
    HmdMatrix34_t r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] +
                        a->m[i][2] * b->m[2][j];
        }
        r.m[i][3] += a->m[i][3];
    }
    return r;
}

bool vr_backend_submit_eye(ovrEyeType eye, void *d3d11_texture,
                           float uMin, float vMin, float uMax, float vMax,
                           const ovrPosef *render_pose)
{
    if (!g_active || !d3d11_texture)
        return false;
    struct Texture_t tex;
    tex.handle = d3d11_texture;
    tex.eType = ETextureType_TextureType_DirectX;
    /* OVRSTUB_COLORSPACE=gamma|linear overrides Auto, for typeless or sRGB
     * textures the compositor may otherwise misread. */
    static int cs = -1;
    if (cs < 0) {
        const char *v = getenv("OVRSTUB_COLORSPACE");
        cs = !v ? EColorSpace_ColorSpace_Auto
           : !strcmp(v, "gamma") ? EColorSpace_ColorSpace_Gamma
           : !strcmp(v, "linear") ? EColorSpace_ColorSpace_Linear
           : EColorSpace_ColorSpace_Auto;
    }
    tex.eColorSpace = (EColorSpace)cs;
    struct VRTextureBounds_t bounds = {uMin, vMin, uMax, vMax};
    /* Tell the compositor which head pose the game rendered this eye from.
     * Without it, reprojection assumes WaitGetPoses' pose, which differs
     * from the one the game predicted, and the view warps and shifts as
     * the head moves. LibOVR's RenderPose is the eye's pose; the compositor
     * wants the HMD's, so remove the eye-to-head offset. */
    const ovrQuatf *q = render_pose ? &render_pose->Orientation : NULL;
    if (q && q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w > 0.5f) {
        struct VRTextureWithPose_t tp;
        tp.handle = tex.handle;
        tp.eType = tex.eType;
        tp.eColorSpace = tex.eColorSpace;
        HmdMatrix34_t eye_world = matrix_from_pose(render_pose);
        HmdMatrix34_t eye_to_head = g_system->GetEyeToHeadTransform((EVREye)eye);
        HmdMatrix34_t head_to_eye = matrix_inverse_rigid(&eye_to_head);
        tp.mDeviceToAbsoluteTracking = matrix_mul(&eye_world, &head_to_eye);
        EVRCompositorError e = g_compositor->Submit(
            (EVREye)eye, (struct Texture_t *)&tp, &bounds,
            EVRSubmitFlags_Submit_TextureWithPose);
        if (e == EVRCompositorError_VRCompositorError_None)
            return true;
        static int warned;
        if (warned++ < 3)
            LOG("compositor Submit(eye %d, with pose) failed: %d; retrying without",
                (int)eye, (int)e);
    }
    EVRCompositorError e = g_compositor->Submit((EVREye)eye, &tex, &bounds,
                                                EVRSubmitFlags_Submit_Default);
    if (e != EVRCompositorError_VRCompositorError_None)
        LOG("compositor Submit(eye %d) failed: %d", (int)eye, (int)e);
    return e == EVRCompositorError_VRCompositorError_None;
}

void vr_backend_log_compositor_state(long frame)
{
    if (!g_active)
        return;
    struct Compositor_FrameTiming t;
    memset(&t, 0, sizeof(t));
    t.m_nSize = sizeof(t);
    bool have = g_compositor->GetFrameTiming && g_compositor->GetFrameTiming(&t, 0);
    LOG("frame %ld: compositor CanRenderScene=%d sceneFocusPid=%u lastRendererPid=%u "
        "(our win pid %lu)%s presents=%u dropped=%u mispresented=%u",
        frame, g_compositor->CanRenderScene ? (int)g_compositor->CanRenderScene() : -1,
        g_compositor->GetCurrentSceneFocusProcess
            ? g_compositor->GetCurrentSceneFocusProcess() : 0,
        g_compositor->GetLastFrameRenderer ? g_compositor->GetLastFrameRenderer() : 0,
        (unsigned long)GetCurrentProcessId(), have ? "" : " (no timing)",
        t.m_nNumFramePresents, t.m_nNumDroppedFrames, t.m_nNumMisPresented);
    /* Totals since we connected: how often SteamVR had to show an old
     * frame (dropped) or synthesize one (reprojected) because ours was late. */
    if (g_compositor->GetCumulativeStats) {
        struct Compositor_CumulativeStats c;
        memset(&c, 0, sizeof(c));
        g_compositor->GetCumulativeStats(&c, sizeof(c));
        LOG("frame %ld: totals presents=%u dropped=%u reprojected=%u timed-out=%u "
            "submits=%u", frame, c.m_nNumFramePresents, c.m_nNumDroppedFrames,
            c.m_nNumReprojectedFrames, c.m_nNumTimedOut, c.m_nNumFrameSubmits);
    }
}

void vr_backend_present(void)
{
    if (g_active)
        g_compositor->PostPresentHandoff();
}
