#include "vr_backend.h"

#include <math.h>
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
    /* OpenVR gives raw projection tangents (left/right/top/bottom), with up
     * negative. LibOVR wants positive up/down/left/right half-tangents. */
    float l = 0, r = 0, t = 0, b = 0;
    g_system->GetProjectionRaw((EVREye)eye, &l, &r, &t, &b);
    fov.LeftTan = fabsf(l);
    fov.RightTan = fabsf(r);
    fov.UpTan = fabsf(t);
    fov.DownTan = fabsf(b);
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

ovrPoseStatef vr_backend_head_pose(void)
{
    ovrPoseStatef s;
    memset(&s, 0, sizeof(s));
    s.ThePose.Orientation.w = 1.0f;
    if (!g_active)
        return s;

    TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
    memset(poses, 0, sizeof(poses));
    /* Standing-space poses, predicted to "now". The compositor supplies more
     * precise render poses during the frame loop; this is for polling. */
    g_system->GetDeviceToAbsoluteTrackingPose(
        ETrackingUniverseOrigin_TrackingUniverseStanding, 0.0f, poses,
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
    return e == EVRCompositorError_VRCompositorError_None;
}

bool vr_backend_submit_eye(ovrEyeType eye, void *d3d11_texture,
                           float uMin, float vMin, float uMax, float vMax)
{
    if (!g_active || !d3d11_texture)
        return false;
    struct Texture_t tex;
    tex.handle = d3d11_texture;
    tex.eType = ETextureType_TextureType_DirectX;
    tex.eColorSpace = EColorSpace_ColorSpace_Auto;
    struct VRTextureBounds_t bounds = {uMin, vMin, uMax, vMax};
    EVRCompositorError e = g_compositor->Submit((EVREye)eye, &tex, &bounds,
                                                EVRSubmitFlags_Submit_Default);
    if (e != EVRCompositorError_VRCompositorError_None)
        LOG("compositor Submit(eye %d) failed: %d", (int)eye, (int)e);
    return e == EVRCompositorError_VRCompositorError_None;
}

void vr_backend_present(void)
{
    if (g_active)
        g_compositor->PostPresentHandoff();
}
