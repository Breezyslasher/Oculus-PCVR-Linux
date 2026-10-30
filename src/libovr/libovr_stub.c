/*
 * LibOVRRT64_1.dll shim, Milestone 2 stage: "runtime present, no HMD".
 *
 * ovr_Initialize succeeds, ovr_GetHmdDesc reports ovrHmd_None and ovr_Create
 * fails with ovrError_NoHmd, which is how a real Oculus install without a
 * headset behaves. Games that support it then fall back to the flat screen,
 * which lets us test the Platform stub and boot-to-menu before any rendering
 * or tracking work (Milestone 3 replaces these with OpenVR-backed versions).
 *
 * Session-taking functions are exported so the game's LibOVR loader can
 * resolve its full symbol table; since no session is ever created they just
 * fail with ovrError_InvalidSession or return zeroed structs.
 */
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include "../common/log.h"
#include "iat_hook.h"
#include "ovr_types.h"
#include "sigcheck.h"
#include "swapchain_d3d.h"
#include "vr_backend.h"

/* The one opaque session object. OpenVR is a process-wide singleton, so a
 * single session is enough and a game only ever creates one. */
struct ovrHmdStruct {
    int active;
};
static struct ovrHmdStruct g_hmd;

#define EXPORT __declspec(dllexport)
#define LOG(...) stub_log("libovr", __VA_ARGS__)
/* Log the first call of each entry point, so when a game crashes the last
 * line in the log names the call it was in (or the one just before). */
#define TRACE_FIRST()                                        \
    do {                                                     \
        static LONG once_;                                   \
        if (!InterlockedExchange(&once_, 1))                 \
            LOG("-> %s", __func__);                          \
    } while (0)

/* Lets `ovrl` recognize our own stub DLLs and never mistake one for a
 * game's genuine file (e.g. when backing up before install). */
EXPORT const char ovrstub_build_marker[] = "OVRSTUB-BUILD-MARKER-v1";

static ovrErrorInfo g_last_error;
static HANDLE g_hmd_event;
static int g_initialized;
static LARGE_INTEGER g_qpc_freq;

static ovrResult set_error(ovrResult r, const char *msg)
{
    g_last_error.Result = r;
    strncpy(g_last_error.ErrorString, msg, sizeof(g_last_error.ErrorString) - 1);
    return r;
}

/* A session handle the game may still use: ours, and not yet destroyed. */
static int session_live(ovrSession session)
{
    return session == &g_hmd && g_hmd.active && vr_backend_active();
}

/* Bring OpenVR up without a session, for calls that report the HMD before
 * (or between) sessions. Retries are throttled so a game polling while
 * SteamVR is off doesn't flood the log with init failures. */
static int backend_up(void)
{
    static DWORD last_try;
    if (vr_backend_active())
        return 1;
    if (!g_initialized)
        return 0;
    DWORD now = GetTickCount();
    if (last_try && now - last_try < 2000)
        return 0;
    last_try = now;
    return vr_backend_init();
}

/* ---- Lifetime ---------------------------------------------------------- */

EXPORT ovrResult ovr_Initialize(const ovrInitParams *params)
{
    TRACE_FIRST();
    LOG("ovr_Initialize(flags=0x%x, minor=%u)", params ? params->Flags : 0,
        params ? params->RequestedMinorVersion : 0);
    g_initialized = 1;
    return ovrSuccess;
}

EXPORT void ovr_Shutdown(void)
{
    TRACE_FIRST();
    LOG("ovr_Shutdown");
    g_hmd.active = 0;
    vr_backend_shutdown();
    g_initialized = 0;
}

EXPORT void ovr_GetLastErrorInfo(ovrErrorInfo *info)
{
    TRACE_FIRST();
    if (info)
        *info = g_last_error;
}

EXPORT const char *ovr_GetVersionString(void)
{
    TRACE_FIRST();
    return "1.0.0-ovrstub";
}

EXPORT int ovr_TraceMessage(int level, const char *message)
{
    TRACE_FIRST();
    LOG("trace[%d]: %s", level, message ? message : "");
    return 0;
}

EXPORT ovrResult ovr_IdentifyClient(const char *identity)
{
    TRACE_FIRST();
    LOG("ovr_IdentifyClient: %s", identity ? identity : "");
    return ovrSuccess;
}

/* ---- HMD / session ----------------------------------------------------- */

EXPORT ovrHmdDesc ovr_GetHmdDesc(ovrSession session)
{
    TRACE_FIRST();
    ovrHmdDesc desc;
    (void)session;
    memset(&desc, 0, sizeof(desc));
    /* Games call this with a NULL session to ask whether a headset is
     * connected before they create one, so answer from OpenVR directly. */
    if (!backend_up()) {
        desc.Type = ovrHmd_None;
        return desc;
    }
    desc.Type = ovrHmd_CV1;
    strcpy(desc.ProductName, "Oculus Rift via ovrstub");
    strcpy(desc.Manufacturer, "ovrstub");
    desc.DefaultEyeFov[ovrEye_Left] = desc.MaxEyeFov[ovrEye_Left] =
        vr_backend_fov(ovrEye_Left);
    desc.DefaultEyeFov[ovrEye_Right] = desc.MaxEyeFov[ovrEye_Right] =
        vr_backend_fov(ovrEye_Right);
    int w, h;
    vr_backend_render_target_size(&w, &h);
    desc.Resolution.w = w * 2;   /* both eyes side by side */
    desc.Resolution.h = h;
    desc.DisplayRefreshRate = 90.0f;
    desc.AvailableHmdCaps = desc.DefaultHmdCaps = 0;
    desc.AvailableTrackingCaps = desc.DefaultTrackingCaps = 0;
    static LONG logged;
    if (!InterlockedExchange(&logged, 1))
        LOG("ovr_GetHmdDesc -> CV1 %dx%d, fov L %.2f/%.2f/%.2f/%.2f",
            desc.Resolution.w, desc.Resolution.h, desc.DefaultEyeFov[0].UpTan,
            desc.DefaultEyeFov[0].DownTan, desc.DefaultEyeFov[0].LeftTan,
            desc.DefaultEyeFov[0].RightTan);
    return desc;
}

EXPORT ovrResult ovr_Create(ovrSession *session, ovrGraphicsLuid *luid)
{
    TRACE_FIRST();
    if (luid)
        memset(luid, 0, sizeof(*luid));
    if (session)
        *session = NULL;
    if (!g_initialized)
        return set_error(ovrError_NotInitialized, "ovr_Initialize not called");
    if (!vr_backend_active() && !vr_backend_init()) {
        LOG("ovr_Create -> ovrError_NoHmd (no SteamVR backend)");
        return set_error(ovrError_NoHmd, "No HMD / SteamVR not available");
    }
    g_hmd.active = 1;
    if (session)
        *session = &g_hmd;
    LOG("ovr_Create -> session via OpenVR");
    return ovrSuccess;
}

EXPORT void ovr_Destroy(ovrSession session)
{
    TRACE_FIRST();
    /* Only the session ends here. OpenVR stays connected until
     * ovr_Shutdown: games commonly create and destroy a temporary session
     * (UE4 does, to read the audio device ids) and then probe for the HMD
     * with ovr_GetHmdDesc(NULL) before creating their real one. */
    if (session == &g_hmd && g_hmd.active) {
        g_hmd.active = 0;
        LOG("ovr_Destroy");
    }
}

EXPORT ovrResult ovr_GetSessionStatus(ovrSession session, ovrSessionStatus *status)
{
    TRACE_FIRST();
    if (!status)
        return set_error(ovrError_InvalidParameter, "null status");
    memset(status, 0, sizeof(*status));
    if (!session_live(session))
        return set_error(ovrError_InvalidSession, "no session");
    status->IsVisible = ovrTrue;
    status->HmdPresent = ovrTrue;
    status->HmdMounted = ovrTrue;
    return ovrSuccess;
}

EXPORT unsigned ovr_GetTrackerCount(ovrSession session)
{
    TRACE_FIRST();
    (void)session;
    return 0;
}

EXPORT ovrTrackerDesc ovr_GetTrackerDesc(ovrSession session, unsigned index)
{
    TRACE_FIRST();
    ovrTrackerDesc d;
    (void)session; (void)index;
    memset(&d, 0, sizeof(d));
    return d;
}

EXPORT ovrTrackerPose ovr_GetTrackerPose(ovrSession session, unsigned index)
{
    TRACE_FIRST();
    ovrTrackerPose p;
    (void)session; (void)index;
    memset(&p, 0, sizeof(p));
    return p;
}

EXPORT ovrTrackingState ovr_GetTrackingState(ovrSession session, double absTime,
                                             ovrBool latencyMarker)
{
    TRACE_FIRST();
    ovrTrackingState s;
    (void)absTime; (void)latencyMarker;
    memset(&s, 0, sizeof(s));
    s.HeadPose.ThePose.Orientation.w = 1.0f;
    s.CalibratedOrigin.Orientation.w = 1.0f;
    if (session_live(session)) {
        s.HeadPose = vr_backend_head_pose();
        s.StatusFlags = 0x0001 | 0x0002;  /* orientation + position tracked */
    }
    return s;
}

/* Exports some SDK versions require in their symbol table (Meta's loader
 * unloads the runtime if any is missing). Minimal, safe behavior. */

EXPORT ovrResult ovr_Lookup(const char *name, void **data)
{
    TRACE_FIRST();
    LOG("ovr_Lookup(%s)", name ? name : "(null)");
    if (data)
        *data = NULL;
    return ovrError_Unsupported;
}

EXPORT unsigned ovr_GetEnabledCaps(ovrSession session)
{
    TRACE_FIRST();
    (void)session;
    return 0;
}

EXPORT void ovr_SetEnabledCaps(ovrSession session, unsigned caps)
{
    TRACE_FIRST();
    (void)session; (void)caps;
}

EXPORT unsigned ovr_GetTrackingCaps(ovrSession session)
{
    TRACE_FIRST();
    (void)session;
    return 0;
}

EXPORT ovrTrackingState ovr_GetTrackingStateWithSensorData(ovrSession session,
                                                           double absTime,
                                                           ovrBool latencyMarker,
                                                           ovrSensorData *sensor)
{
    TRACE_FIRST();
    if (sensor)
        memset(sensor, 0, sizeof(*sensor));
    return ovr_GetTrackingState(session, absTime, latencyMarker);
}

EXPORT ovrResult ovr_GetExternalCameras(ovrSession session, void *cameras,
                                        unsigned *inoutCameraCount)
{
    TRACE_FIRST();
    (void)session; (void)cameras;
    if (inoutCameraCount)
        *inoutCameraCount = 0;
    return ovrSuccess;
}

EXPORT ovrResult ovr_SetExternalCameraProperties(ovrSession session, const char *name,
                                                 const void *intrinsics,
                                                 const void *extrinsics)
{
    TRACE_FIRST();
    (void)session; (void)name; (void)intrinsics; (void)extrinsics;
    return ovrSuccess;
}

/* Audio device queries: report the system default (WAVE_MAPPER / empty). */
/* Audio device queries. Unlike nearly every other entry point these take no
 * session (the real runtime just reports the Rift's headphones and mic), so
 * the signatures must match exactly or the caller's buffer is misread. We
 * report the system's default devices, as Revive does, so the game plays
 * through whatever output the user has selected. */
#define OVR_AUDIO_MAX_DEVICE_STR_SIZE 128

static const GUID kClsidMMDeviceEnumerator =
    {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID kIidIMMDeviceEnumerator =
    {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
/* DirectSound's default playback/capture device ids. */
static const GUID kDsDefaultPlayback =
    {0xdef00000, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};
static const GUID kDsDefaultCapture =
    {0xdef00001, 0x9c6d, 0x47ed, {0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03}};

/* Copy the default endpoint's id (eRender = 0, eCapture = 1; role eConsole
 * = 0) into buf; leaves buf empty if there is no such device. */
static void default_endpoint_id(int flow, wchar_t *buf)
{
    buf[0] = 0;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    if (SUCCEEDED(CoCreateInstance(&kClsidMMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &kIidIMMDeviceEnumerator, (void **)&en))) {
        IMMDevice *dev = NULL;
        if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, (EDataFlow)flow,
                                                          eConsole, &dev))) {
            LPWSTR id = NULL;
            if (SUCCEEDED(dev->lpVtbl->GetId(dev, &id)) && id) {
                wcsncpy(buf, id, OVR_AUDIO_MAX_DEVICE_STR_SIZE - 1);
                buf[OVR_AUDIO_MAX_DEVICE_STR_SIZE - 1] = 0;
                CoTaskMemFree(id);
            }
            dev->lpVtbl->Release(dev);
        }
        en->lpVtbl->Release(en);
    }
    if (SUCCEEDED(init))
        CoUninitialize();
}

EXPORT ovrResult ovr_GetAudioDeviceOutWaveId(unsigned *deviceOutId)
{
    TRACE_FIRST();
    if (!deviceOutId)
        return set_error(ovrError_InvalidParameter, "null deviceOutId");
    *deviceOutId = WAVE_MAPPER;
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetAudioDeviceInWaveId(unsigned *deviceInId)
{
    TRACE_FIRST();
    if (!deviceInId)
        return set_error(ovrError_InvalidParameter, "null deviceInId");
    *deviceInId = WAVE_MAPPER;
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetAudioDeviceOutGuidStr(wchar_t *buffer)
{
    TRACE_FIRST();
    if (!buffer)
        return set_error(ovrError_InvalidParameter, "null buffer");
    default_endpoint_id(eRender, buffer);
    LOG("ovr_GetAudioDeviceOutGuidStr -> \"%ls\"", buffer);
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetAudioDeviceInGuidStr(wchar_t *buffer)
{
    TRACE_FIRST();
    if (!buffer)
        return set_error(ovrError_InvalidParameter, "null buffer");
    default_endpoint_id(eCapture, buffer);
    LOG("ovr_GetAudioDeviceInGuidStr -> \"%ls\"", buffer);
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetAudioDeviceOutGuid(GUID *guid)
{
    TRACE_FIRST();
    if (!guid)
        return set_error(ovrError_InvalidParameter, "null guid");
    *guid = kDsDefaultPlayback;
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetAudioDeviceInGuid(GUID *guid)
{
    TRACE_FIRST();
    if (!guid)
        return set_error(ovrError_InvalidParameter, "null guid");
    *guid = kDsDefaultCapture;
    return ovrSuccess;
}

EXPORT ovrResult ovr_SetSynchronizationQueueVk(ovrSession session, void *queue)
{
    TRACE_FIRST();
    (void)session; (void)queue;
    return ovrSuccess;
}

/* Misspelled variant that shipped in some SDK versions. */
EXPORT ovrResult ovr_SetSynchonizationQueueVk(ovrSession session, void *queue)
{
    TRACE_FIRST();
    return ovr_SetSynchronizationQueueVk(session, queue);
}

EXPORT ovrTouchHapticsDesc ovr_GetTouchHapticsDesc(ovrSession session, int type)
{
    TRACE_FIRST();
    ovrTouchHapticsDesc d;
    (void)session; (void)type;
    memset(&d, 0, sizeof(d));
    return d;
}

EXPORT ovrHmdColorDesc ovr_GetHmdColorDesc(ovrSession session)
{
    TRACE_FIRST();
    ovrHmdColorDesc d;
    (void)session;
    memset(&d, 0, sizeof(d));
    return d;
}

EXPORT ovrSizei ovr_GetFovTextureSize(ovrSession session, ovrEyeType eye,
                                      ovrFovPort fov, float pixelsPerDisplayPixel)
{
    TRACE_FIRST();
    ovrSizei s = {0, 0};
    (void)session; (void)eye; (void)fov;
    if (vr_backend_active()) {
        int w, h;
        vr_backend_render_target_size(&w, &h);
        float scale = pixelsPerDisplayPixel > 0.0f ? pixelsPerDisplayPixel : 1.0f;
        s.w = (int)(w * scale);
        s.h = (int)(h * scale);
    }
    return s;
}

EXPORT ovrEyeRenderDescV1 ovr_GetRenderDesc(ovrSession session, ovrEyeType eye,
                                            ovrFovPort fov)
{
    TRACE_FIRST();
    ovrEyeRenderDescV1 d;
    (void)session;
    memset(&d, 0, sizeof(d));
    d.Eye = eye;
    d.Fov = fov;
    int w, h;
    vr_backend_render_target_size(&w, &h);
    d.DistortedViewport.Size.w = w;
    d.DistortedViewport.Size.h = h;
    if (vr_backend_active()) {
        ovrPosef p = vr_backend_eye_pose(eye);
        d.HmdToEyeOffset = p.Position;  /* pre-1.17 used an offset vector */
    }
    return d;
}

EXPORT ovrEyeRenderDesc ovr_GetRenderDesc2(ovrSession session, ovrEyeType eye,
                                           ovrFovPort fov)
{
    TRACE_FIRST();
    ovrEyeRenderDesc d;
    (void)session;
    memset(&d, 0, sizeof(d));
    d.Eye = eye;
    d.Fov = fov;
    d.HmdToEyePose.Orientation.w = 1.0f;
    int w, h;
    vr_backend_render_target_size(&w, &h);
    d.DistortedViewport.Size.w = w;
    d.DistortedViewport.Size.h = h;
    if (vr_backend_active())
        d.HmdToEyePose = vr_backend_eye_pose(eye);
    return d;
}

/* Session-taking calls that report a result code. Their real argument lists
 * differ, but with no live session every one of them fails the same way, and
 * on Win64 the caller owns argument cleanup, so ignoring arguments is safe. */
#define NO_SESSION(name)                                     \
    EXPORT ovrResult name(void)                              \
    {                                                        \
        TRACE_FIRST();                                       \
        return set_error(ovrError_InvalidSession, #name ": no session"); \
    }

NO_SESSION(ovr_SetTrackingOriginType)
NO_SESSION(ovr_RecenterTrackingOrigin)
NO_SESSION(ovr_SpecifyTrackingOrigin)
NO_SESSION(ovr_GetDevicePoses)
NO_SESSION(ovr_GetInputState)
NO_SESSION(ovr_SetControllerVibration)
NO_SESSION(ovr_SubmitControllerVibration)
NO_SESSION(ovr_GetControllerVibrationState)
NO_SESSION(ovr_TestBoundary)
NO_SESSION(ovr_TestBoundaryPoint)
NO_SESSION(ovr_SetBoundaryLookAndFeel)
NO_SESSION(ovr_ResetBoundaryLookAndFeel)
NO_SESSION(ovr_GetBoundaryGeometry)
NO_SESSION(ovr_GetBoundaryDimensions)
NO_SESSION(ovr_GetBoundaryVisible)
NO_SESSION(ovr_RequestBoundaryVisible)
NO_SESSION(ovr_CreateTextureSwapChainGL)
NO_SESSION(ovr_GetTextureSwapChainBufferGL)
NO_SESSION(ovr_CreateMirrorTextureGL)
NO_SESSION(ovr_CreateMirrorTextureWithOptionsGL)
NO_SESSION(ovr_GetMirrorTextureBufferGL)
NO_SESSION(ovr_GetPerfStats)
NO_SESSION(ovr_ResetPerfStats)

/* ---- D3D11 texture swap chains, mirror, and frame submission (3b) ------- */

EXPORT ovrResult ovr_CreateTextureSwapChainDX(ovrSession session, void *d3dDevice,
                                              const ovrTextureSwapChainDesc *desc,
                                              ovrTextureSwapChain *out)
{
    TRACE_FIRST();
    if (out)
        *out = NULL;
    if (!session_live(session))
        return set_error(ovrError_InvalidSession, "no session");
    if (!d3dDevice || !desc || !out)
        return set_error(ovrError_InvalidParameter, "null arg");
    LOG("ovr_CreateTextureSwapChainDX(type=%d fmt=%d %dx%d array=%d mips=%d "
        "samples=%d static=%d misc=0x%x bind=0x%x)", desc->Type, desc->Format,
        desc->Width, desc->Height, desc->ArraySize, desc->MipLevels,
        desc->SampleCount, desc->StaticImage, desc->MiscFlags, desc->BindFlags);
    ovrTextureSwapChain c = swapchain_d3d_create(d3dDevice, desc);
    if (!c) {
        LOG("  -> swap chain creation FAILED");
        return set_error(ovrError_RuntimeException, "swap chain creation failed");
    }
    LOG("  -> chain %p, %d buffers", (void *)c, swapchain_d3d_length(c));
    *out = c;
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetTextureSwapChainLength(ovrSession session,
                                               ovrTextureSwapChain chain, int *out)
{
    TRACE_FIRST();
    (void)session;
    if (!chain || !out)
        return set_error(ovrError_InvalidParameter, "null arg");
    *out = swapchain_d3d_length(chain);
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetTextureSwapChainCurrentIndex(ovrSession session,
                                                     ovrTextureSwapChain chain, int *out)
{
    TRACE_FIRST();
    (void)session;
    if (!chain || !out)
        return set_error(ovrError_InvalidParameter, "null arg");
    *out = swapchain_d3d_current_index(chain);
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetTextureSwapChainDesc(ovrSession session,
                                             ovrTextureSwapChain chain,
                                             ovrTextureSwapChainDesc *out)
{
    TRACE_FIRST();
    (void)session;
    if (!chain || !out)
        return set_error(ovrError_InvalidParameter, "null arg");
    int w, h;
    swapchain_d3d_size(chain, &w, &h);
    memset(out, 0, sizeof(*out));
    out->Width = w;
    out->Height = h;
    out->ArraySize = 1;
    out->MipLevels = 1;
    out->SampleCount = 1;
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetTextureSwapChainBufferDX(ovrSession session,
                                                 ovrTextureSwapChain chain, int index,
                                                 const void *iid, void **out)
{
    TRACE_FIRST();
    (void)session;
    if (!chain || !out)
        return set_error(ovrError_InvalidParameter, "null arg");
    void *buf = swapchain_d3d_buffer(chain, index, iid);
    if (!buf)
        return set_error(ovrError_InvalidParameter, "bad swap chain buffer");
    *out = buf;
    return ovrSuccess;
}

EXPORT ovrResult ovr_CommitTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
    TRACE_FIRST();
    (void)session;
    if (!chain)
        return set_error(ovrError_InvalidParameter, "null chain");
    swapchain_d3d_commit(chain);
    return ovrSuccess;
}

EXPORT void ovr_DestroyTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
    TRACE_FIRST();
    (void)session;
    swapchain_d3d_destroy(chain);
}

/* Mirror textures (the desktop preview) are optional for VR output; create a
 * plain texture so the game's calls succeed, but nothing is drawn into it. */
EXPORT ovrResult ovr_CreateMirrorTextureDX(ovrSession session, void *d3dDevice,
                                           const ovrMirrorTextureDesc *desc,
                                           ovrMirrorTexture *out)
{
    TRACE_FIRST();
    if (out)
        *out = NULL;
    if (session != &g_hmd || !d3dDevice || !desc || !out)
        return set_error(ovrError_InvalidParameter, "mirror: bad arg");
    ovrTextureSwapChainDesc sc;
    memset(&sc, 0, sizeof(sc));
    sc.Format = desc->Format;
    sc.Width = desc->Width;
    sc.Height = desc->Height;
    sc.ArraySize = 1;
    sc.MipLevels = 1;
    sc.SampleCount = 1;
    sc.BindFlags = ovrTextureBind_DX_RenderTarget;
    sc.StaticImage = ovrTrue;
    ovrTextureSwapChain c = swapchain_d3d_create(d3dDevice, &sc);
    if (!c)
        return set_error(ovrError_RuntimeException, "mirror creation failed");
    *out = (ovrMirrorTexture)c;
    return ovrSuccess;
}

EXPORT ovrResult ovr_CreateMirrorTextureWithOptionsDX(ovrSession session, void *d3dDevice,
                                                      const ovrMirrorTextureDesc *desc,
                                                      ovrMirrorTexture *out)
{
    TRACE_FIRST();
    return ovr_CreateMirrorTextureDX(session, d3dDevice, desc, out);
}

EXPORT ovrResult ovr_GetMirrorTextureBufferDX(ovrSession session, ovrMirrorTexture mirror,
                                              const void *iid, void **out)
{
    TRACE_FIRST();
    (void)session;
    if (!mirror || !out)
        return set_error(ovrError_InvalidParameter, "null arg");
    void *buf = swapchain_d3d_buffer((ovrTextureSwapChain)mirror, 0, iid);
    if (!buf)
        return set_error(ovrError_InvalidParameter, "bad mirror buffer");
    *out = buf;
    return ovrSuccess;
}

/* Submit one frame's layers to the compositor and present. We handle the
 * EyeFov layer (what head-mounted rendering uses); others are ignored. The
 * caller has already blocked for render poses (via begin_frame). */
static ovrResult submit_layers(const ovrLayerHeader *const *layers, unsigned count)
{
    int submitted = 0;
    for (unsigned i = 0; i < count; i++) {
        const ovrLayerHeader *h = layers ? layers[i] : NULL;
        if (!h || h->Type != ovrLayerType_EyeFov)
            continue;
        const ovrLayerEyeFov *l = (const ovrLayerEyeFov *)h;
        for (int eye = 0; eye < ovrEye_Count; eye++) {
            ovrTextureSwapChain chain = l->ColorTexture[eye]
                ? l->ColorTexture[eye] : l->ColorTexture[0];
            if (!chain)
                continue;
            void *tex = swapchain_d3d_submit_texture(chain);
            int w, h2;
            swapchain_d3d_size(chain, &w, &h2);
            /* Normalize the eye's viewport into texture UV bounds. */
            ovrRecti vp = l->Viewport[eye];
            float uMin = w ? (float)vp.Pos.x / w : 0.0f;
            float vMin = h2 ? (float)vp.Pos.y / h2 : 0.0f;
            float uMax = w ? (float)(vp.Pos.x + vp.Size.w) / w : 1.0f;
            float vMax = h2 ? (float)(vp.Pos.y + vp.Size.h) / h2 : 1.0f;
            if (vp.Size.w == 0 || vp.Size.h == 0) {
                uMin = vMin = 0.0f;
                uMax = vMax = 1.0f;
            }
            if (vr_backend_submit_eye((ovrEyeType)eye, tex, uMin, vMin, uMax, vMax))
                submitted++;
        }
    }
    vr_backend_present();
    static LONG frames_;
    LONG f = InterlockedIncrement(&frames_);
    if (f <= 3 || f % 900 == 0)
        LOG("frame %ld: %u layer(s), %d eye(s) submitted", f, count, submitted);
    return submitted ? ovrSuccess
        : set_error(ovrSuccess_NotVisible, "no eye layer submitted");
}

EXPORT ovrResult ovr_SubmitFrame(ovrSession session, long long frameIndex,
                                 const ovrViewScaleDesc *viewScaleDesc,
                                 const ovrLayerHeader *const *layerPtrList,
                                 unsigned layerCount)
{
    TRACE_FIRST();
    (void)frameIndex; (void)viewScaleDesc;
    if (!session_live(session))
        return set_error(ovrError_InvalidSession, "no session");
    /* Old single-call frame model: block for poses, submit, present. */
    vr_backend_begin_frame();
    return submit_layers(layerPtrList, layerCount);
}

EXPORT ovrResult ovr_SubmitFrame2(ovrSession session, long long frameIndex,
                                  const ovrViewScaleDesc *viewScaleDesc,
                                  const ovrLayerHeader *const *layerPtrList,
                                  unsigned layerCount)
{
    TRACE_FIRST();
    return ovr_SubmitFrame(session, frameIndex, viewScaleDesc, layerPtrList, layerCount);
}

EXPORT ovrResult ovr_WaitToBeginFrame(ovrSession session, long long frameIndex)
{
    TRACE_FIRST();
    (void)frameIndex;
    if (!session_live(session))
        return set_error(ovrError_InvalidSession, "no session");
    return vr_backend_begin_frame() ? ovrSuccess
        : set_error(ovrError_RuntimeException, "WaitGetPoses failed");
}

EXPORT ovrResult ovr_BeginFrame(ovrSession session, long long frameIndex)
{
    TRACE_FIRST();
    (void)frameIndex;
    if (!session_live(session))
        return set_error(ovrError_InvalidSession, "no session");
    return ovrSuccess;
}

EXPORT ovrResult ovr_EndFrame(ovrSession session, long long frameIndex,
                              const ovrViewScaleDesc *viewScaleDesc,
                              const ovrLayerHeader *const *layerPtrList,
                              unsigned layerCount)
{
    TRACE_FIRST();
    (void)frameIndex; (void)viewScaleDesc;
    if (!session_live(session))
        return set_error(ovrError_InvalidSession, "no session");
    /* Split frame model: ovr_WaitToBeginFrame already blocked for poses. */
    return submit_layers(layerPtrList, layerCount);
}

EXPORT int ovr_GetTrackingOriginType(ovrSession session)
{
    TRACE_FIRST();
    (void)session;
    return 0;
}

EXPORT void ovr_ClearShouldRecenterFlag(ovrSession session)
{
    TRACE_FIRST();
    (void)session;
}

EXPORT unsigned ovr_GetConnectedControllerTypes(ovrSession session)
{
    TRACE_FIRST();
    (void)session;
    return 0;
}

EXPORT void ovr_DestroyMirrorTexture(ovrSession session, ovrMirrorTexture tex)
{
    TRACE_FIRST();
    (void)session;
    swapchain_d3d_destroy((ovrTextureSwapChain)tex);
}

/* ---- Timing ------------------------------------------------------------ */

EXPORT double ovr_GetTimeInSeconds(void)
{
    TRACE_FIRST();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)g_qpc_freq.QuadPart;
}

EXPORT double ovr_GetPredictedDisplayTime(ovrSession session, long long frameIndex)
{
    TRACE_FIRST();
    (void)session; (void)frameIndex;
    return ovr_GetTimeInSeconds();
}

/* ---- Properties: always return the caller's default -------------------- */

EXPORT ovrBool ovr_GetBool(ovrSession s, const char *name, ovrBool def)
{
    TRACE_FIRST();
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetBool(ovrSession s, const char *name, ovrBool v)
{
    TRACE_FIRST();
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

EXPORT int ovr_GetInt(ovrSession s, const char *name, int def)
{
    TRACE_FIRST();
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetInt(ovrSession s, const char *name, int v)
{
    TRACE_FIRST();
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

EXPORT float ovr_GetFloat(ovrSession s, const char *name, float def)
{
    TRACE_FIRST();
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetFloat(ovrSession s, const char *name, float v)
{
    TRACE_FIRST();
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

EXPORT unsigned ovr_GetFloatArray(ovrSession s, const char *name, float *v,
                                  unsigned cap)
{
    TRACE_FIRST();
    (void)s; (void)name; (void)v; (void)cap;
    return 0;
}

EXPORT ovrBool ovr_SetFloatArray(ovrSession s, const char *name, const float *v,
                                 unsigned n)
{
    TRACE_FIRST();
    (void)s; (void)name; (void)v; (void)n;
    return ovrFalse;
}

EXPORT const char *ovr_GetString(ovrSession s, const char *name, const char *def)
{
    TRACE_FIRST();
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetString(ovrSession s, const char *name, const char *v)
{
    TRACE_FIRST();
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

/*
 * The Oculus service creates a named event, "OculusHMDConnected", and signals
 * it while a headset is attached. Unreal's Oculus plugin opens it before
 * touching LibOVR: no event means "Oculus isn't installed", so the runtime is
 * never loaded. We create it ourselves, signaled unless OVRSTUB_HMD_CONNECTED=0.
 * The handle stays open for the life of the process.
 */
static void create_hmd_connected_event(void)
{
    const char *v = getenv("OVRSTUB_HMD_CONNECTED");
    BOOL connected = !(v && v[0] == '0');
    g_hmd_event = CreateEventW(NULL, TRUE, connected, L"OculusHMDConnected");
    if (!g_hmd_event)
        LOG("could not create OculusHMDConnected event (error %lu)", GetLastError());
    else
        LOG("OculusHMDConnected event created (%s)",
            connected ? "signaled: HMD present" : "not signaled: no HMD");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        QueryPerformanceFrequency(&g_qpc_freq);
        LOG("loaded (no-HMD stub)");
        create_hmd_connected_event();
        sigcheck_install();
        redirect_install(inst);
    }
    return TRUE;
}
