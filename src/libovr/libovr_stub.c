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
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dxgi.h>
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
EXPORT double ovr_GetTimeInSeconds(void);
static unsigned g_minor;  /* SDK minor version the game was built against */
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
    g_minor = params ? params->RequestedMinorVersion : 0;
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

/* The LUID of the GPU the headset is on. Games compare it with the adapter
 * they render with (UE4 destroys the session and retries every frame on a
 * mismatch), so it must be a real adapter's LUID, not zeros. We take the
 * adapter SteamVR reports, falling back to the primary one. */
static const GUID kIidIDXGIFactory1 =
    {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

static void hmd_adapter_luid(ovrGraphicsLuid *out)
{
    memset(out, 0, sizeof(*out));
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&kIidIDXGIFactory1, (void **)&factory)))
        return;
    int index = vr_backend_dxgi_adapter_index();
    IDXGIAdapter1 *adapter = NULL;
    if (index < 0 ||
        FAILED(factory->lpVtbl->EnumAdapters1(factory, (UINT)index, &adapter))) {
        index = 0;
        adapter = NULL;
        factory->lpVtbl->EnumAdapters1(factory, 0, &adapter);
    }
    if (adapter) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->lpVtbl->GetDesc1(adapter, &desc))) {
            memcpy(out->Reserved, &desc.AdapterLuid, sizeof(out->Reserved));
            static LONG logged;
            if (!InterlockedExchange(&logged, 1))
                LOG("HMD adapter %d: \"%ls\" LUID %08lx:%08lx", index,
                    desc.Description, (unsigned long)desc.AdapterLuid.HighPart,
                    (unsigned long)desc.AdapterLuid.LowPart);
        }
        adapter->lpVtbl->Release(adapter);
    }
    factory->lpVtbl->Release(factory);
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
    if (luid)
        hmd_adapter_luid(luid);
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
    /* Later SDKs append HasInputFocus and OverlayPresent (1.21+) and
     * DepthRequested (1.38+). Without input focus a game pauses input, so
     * set it, but only write fields the caller's struct has. */
    ovrBool *extra = (ovrBool *)status + 6;
    if (g_minor >= 21) {
        extra[0] = ovrTrue;   /* HasInputFocus */
        extra[1] = ovrFalse;  /* OverlayPresent */
    }
    if (g_minor >= 38)
        extra[2] = ovrFalse;  /* DepthRequested */
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
    (void)latencyMarker;
    memset(&s, 0, sizeof(s));
    /* absTime is when the caller wants the pose for (normally the frame's
     * predicted display time); 0 means "now". Predict that far ahead. */
    double now = ovr_GetTimeInSeconds();
    float predict = absTime > 0.0 ? (float)(absTime - now) : 0.0f;
    if (predict < 0.0f)
        predict = 0.0f;
    if (predict > 0.1f)
        predict = 0.1f;
    s.HeadPose.ThePose.Orientation.w = 1.0f;
    s.CalibratedOrigin.Orientation.w = 1.0f;
    if (session_live(session)) {
        s.HeadPose = vr_backend_head_pose(predict);
        s.HeadPose.TimeInSeconds = now + predict;
        s.StatusFlags = 0x0001 | 0x0002;  /* orientation + position tracked */
        for (int hand = 0; hand < 2; hand++) {
            struct vr_controller c;
            s.HandPoses[hand].ThePose.Orientation.w = 1.0f;
            if (vr_backend_controller(hand, &c, predict) && c.pose_valid) {
                s.HandPoses[hand] = c.pose;
                s.HandPoses[hand].TimeInSeconds = now + predict;
                s.HandStatusFlags[hand] = 0x0001 | 0x0002;
            }
        }
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
    static LONG n;
    if (InterlockedIncrement(&n) <= 6)
        LOG("GetTextureSwapChainCurrentIndex(%p) -> %d", (void *)chain, *out);
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
    LOG("GetTextureSwapChainBufferDX(%p, %d) -> %p", (void *)chain, index, buf);
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
    static LONG n;
    if (InterlockedIncrement(&n) <= 6)
        LOG("CommitTextureSwapChain(%p) -> next index %d", (void *)chain,
            swapchain_d3d_current_index(chain));
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
/* The part of an EyeFov-style layer after its header. EyeFov, EyeFovDepth
 * and EyeMatrix all start with the textures and viewports; we use those
 * (RenderPose is only meaningful for EyeFov/EyeFovDepth, and only logged). */
struct eye_layer {
    ovrTextureSwapChain ColorTexture[ovrEye_Count];
    ovrRecti Viewport[ovrEye_Count];
    ovrFovPort Fov[ovrEye_Count];
    ovrPosef RenderPose[ovrEye_Count];
};

/* Read an eye layer whatever the SDK's header size. Newer SDKs append a
 * 128-byte Reserved block to ovrLayerHeader, moving every field after it.
 * Rather than guess the version, accept whichever offset holds a swap chain
 * we created, and never dereference anything else. */
static int read_eye_layer(const ovrLayerHeader *h, struct eye_layer *out)
{
    if (!h || (h->Type != 1 /* EyeFov */ && h->Type != 2 /* EyeFovDepth */ &&
               h->Type != 5 /* EyeMatrix */))
        return 0;
    static const size_t offsets[] = {sizeof(ovrLayerHeader), sizeof(ovrLayerHeader) + 128};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        memcpy(out, (const char *)h + offsets[i], sizeof(*out));
        if (swapchain_d3d_known(out->ColorTexture[0])) {
            if (!swapchain_d3d_known(out->ColorTexture[1]))
                out->ColorTexture[1] = NULL;
            static LONG logged;
            if (!InterlockedExchange(&logged, 1))
                LOG("layer type %d: fields at offset %u (header %s)", (int)h->Type,
                    (unsigned)offsets[i], i ? "with Reserved[128]" : "plain");
            return 1;
        }
    }
    static LONG warned;
    if (InterlockedIncrement(&warned) <= 5)
        LOG("layer type %d: no known swap chain at either header offset; skipped",
            (int)h->Type);
    return 0;
}

static ovrResult submit_layers(const ovrLayerHeader *const *layers, unsigned count)
{
    int submitted = 0;
    for (unsigned i = 0; i < count; i++) {
        const ovrLayerHeader *h = layers ? layers[i] : NULL;
        struct eye_layer layer, *l = &layer;
        if (!read_eye_layer(h, l))
            continue;
        for (int eye = 0; eye < ovrEye_Count; eye++) {
            ovrTextureSwapChain chain = l->ColorTexture[eye]
                ? l->ColorTexture[eye] : l->ColorTexture[0];
            if (!chain)
                continue;
            static int test_pattern = -1;
            if (test_pattern < 0) {
                const char *tp = getenv("OVRSTUB_TEST_PATTERN");
                test_pattern = tp && *tp == '1';
                if (test_pattern)
                    LOG("OVRSTUB_TEST_PATTERN: submitting solid colors "
                        "(magenta left, cyan right) instead of the game's image");
            }
            if (test_pattern)
                swapchain_d3d_fill_test_pattern(chain, eye);
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
            /* ovrLayerFlag_TextureOriginAtBottomLeft (Unity and GL games):
             * the viewport is measured from the bottom and the image is
             * stored bottom-up. Mirroring v both maps the viewport and, as
             * vMin > vMax, has the compositor flip the image upright. */
            if (h->Flags & 0x2) {
                vMin = 1.0f - vMin;
                vMax = 1.0f - vMax;
            }
            /* EyeMatrix layers keep their RenderPose elsewhere; only use it
             * from EyeFov / EyeFovDepth. */
            const ovrPosef *pose = h->Type != 5 ? &l->RenderPose[eye] : NULL;
            if (vr_backend_submit_eye((ovrEyeType)eye, tex, uMin, vMin, uMax, vMax, pose))
                submitted++;
        }
    }
    vr_backend_present();
    static int preclear = -1;
    if (preclear < 0) {
        const char *pc = getenv("OVRSTUB_PRECLEAR");
        preclear = pc && *pc == '1';
        if (preclear)
            LOG("OVRSTUB_PRECLEAR: marking the next render buffer dark green "
                "(ff004000 in the probe = the game never wrote it)");
    }
    if (preclear)
        for (unsigned i = 0; i < count; i++) {
            const ovrLayerHeader *h = layers ? layers[i] : NULL;
            struct eye_layer layer, *l = &layer;
            if (read_eye_layer(h, l)) {
                swapchain_d3d_preclear_next(l->ColorTexture[0]);
                if (l->ColorTexture[1] && l->ColorTexture[1] != l->ColorTexture[0])
                    swapchain_d3d_preclear_next(l->ColorTexture[1]);
            }
        }
    static LONG frames_;
    LONG f = InterlockedIncrement(&frames_);
    if (f <= 3 || f % 900 == 0)
        LOG("frame %ld: %u layer(s), %d eye(s) submitted", f, count, submitted);
    if (f == 1 || f == 90 || f == 300 || f % 900 == 0) {
        vr_backend_log_compositor_state(f);
        for (unsigned i = 0; i < count; i++) {
            const ovrLayerHeader *h = layers ? layers[i] : NULL;
            struct eye_layer layer, *l = &layer;
            if (!read_eye_layer(h, l))
                continue;
            LOG("frame %ld: layer %u flags 0x%x chains %p/%p viewports "
                "L(%d,%d %dx%d) R(%d,%d %dx%d) renderpose L q(%.2f %.2f %.2f %.2f) "
                "p(%.2f %.2f %.2f)", f, i, h->Flags, (void *)l->ColorTexture[0],
                (void *)l->ColorTexture[1], l->Viewport[0].Pos.x, l->Viewport[0].Pos.y,
                l->Viewport[0].Size.w, l->Viewport[0].Size.h, l->Viewport[1].Pos.x,
                l->Viewport[1].Pos.y, l->Viewport[1].Size.w, l->Viewport[1].Size.h,
                l->RenderPose[0].Orientation.x, l->RenderPose[0].Orientation.y,
                l->RenderPose[0].Orientation.z, l->RenderPose[0].Orientation.w,
                l->RenderPose[0].Position.x, l->RenderPose[0].Position.y,
                l->RenderPose[0].Position.z);
            swapchain_d3d_probe(l->ColorTexture[0], f);
            if (l->ColorTexture[1] && l->ColorTexture[1] != l->ColorTexture[0])
                swapchain_d3d_probe(l->ColorTexture[1], f);
        }
    }
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
    /* Old single-call frame model. SteamVR's loop is WaitGetPoses, render,
     * Submit; WaitGetPoses must come before the game samples its next pose.
     * So submit this frame first, then wait for the next frame's slot here,
     * before returning to the game (waiting first, as before, delivered
     * every frame a vsync late). The very first frame waits up front. */
    static LONG waited_once;
    if (!InterlockedExchange(&waited_once, 1))
        vr_backend_begin_frame();
    ovrResult r = submit_layers(layerPtrList, layerCount);
    vr_backend_begin_frame();
    return r;
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
    unsigned types = 0;
    struct vr_controller c;
    if (vr_backend_controller(0, &c, 0.0f))
        types |= ovrControllerType_LTouch;
    if (vr_backend_controller(1, &c, 0.0f))
        types |= ovrControllerType_RTouch;
    return types;
}

/* Radial thumbstick deadzone like LibOVR's, rescaled so it starts at 0. */
static ovrVector2f stick_deadzone(ovrVector2f v)
{
    const float dz = 0.2f;
    float len = sqrtf(v.x * v.x + v.y * v.y);
    ovrVector2f out = {0.0f, 0.0f};
    if (len > dz) {
        float scale = (len > 1.0f ? 1.0f : (len - dz) / (1.0f - dz)) / len;
        out.x = v.x * scale;
        out.y = v.y * scale;
    }
    return out;
}

/* SteamVR's legacy input for Touch-style controllers: A/X on k_EButton_A,
 * B/Y on ApplicationMenu, the stick on Axis0, trigger on Axis1, grip on
 * Axis2. Mapped to LibOVR's per-hand bits. */
#define VRBIT(id) (1ull << (id))
EXPORT ovrResult ovr_GetInputState(ovrSession session, int controllerType,
                                   ovrInputState *out)
{
    TRACE_FIRST();
    if (!out)
        return set_error(ovrError_InvalidParameter, "null inputState");
    ovrInputState st;
    memset(&st, 0, sizeof(st));
    st.TimeInSeconds = ovr_GetTimeInSeconds();
    if (session_live(session) &&
        (controllerType & ovrControllerType_Touch)) {
        static const unsigned btn_lo[2] = {ovrButton_X, ovrButton_A};
        static const unsigned btn_hi[2] = {ovrButton_Y, ovrButton_B};
        static const unsigned btn_stick[2] = {ovrButton_LThumb, ovrButton_RThumb};
        static const unsigned t_lo[2] = {ovrTouch_X, ovrTouch_A};
        static const unsigned t_hi[2] = {ovrTouch_Y, ovrTouch_B};
        static const unsigned t_stick[2] = {ovrTouch_LThumb, ovrTouch_RThumb};
        static const unsigned t_trig[2] = {ovrTouch_LIndexTrigger, ovrTouch_RIndexTrigger};
        static const unsigned t_point[2] = {ovrTouch_LIndexPointing, ovrTouch_RIndexPointing};
        static const unsigned t_up[2] = {ovrTouch_LThumbUp, ovrTouch_RThumbUp};
        for (int hand = 0; hand < 2; hand++) {
            unsigned type = hand ? ovrControllerType_RTouch : ovrControllerType_LTouch;
            struct vr_controller c;
            if (!(controllerType & type) || !vr_backend_controller(hand, &c, 0.0f))
                continue;
            st.ControllerType |= type;
            if (c.pressed & VRBIT(VR_BUTTON_A))
                st.Buttons |= btn_lo[hand];
            if (c.pressed & VRBIT(VR_BUTTON_APP_MENU))
                st.Buttons |= btn_hi[hand];
            if (c.pressed & VRBIT(VR_BUTTON_AXIS0))
                st.Buttons |= btn_stick[hand];
            int thumb_on = 0;
            if (c.touched & VRBIT(VR_BUTTON_A)) {
                st.Touches |= t_lo[hand];
                thumb_on = 1;
            }
            if (c.touched & VRBIT(VR_BUTTON_APP_MENU)) {
                st.Touches |= t_hi[hand];
                thumb_on = 1;
            }
            if (c.touched & VRBIT(VR_BUTTON_AXIS0)) {
                st.Touches |= t_stick[hand];
                thumb_on = 1;
            }
            if (c.touched & VRBIT(VR_BUTTON_AXIS1))
                st.Touches |= t_trig[hand];
            else
                st.Touches |= t_point[hand];
            if (!thumb_on)
                st.Touches |= t_up[hand];
            ovrVector2f stick = {c.axis[0][0], c.axis[0][1]};
            st.ThumbstickRaw[hand] = st.ThumbstickNoDeadzone[hand] = stick;
            st.Thumbstick[hand] = stick_deadzone(stick);
            st.IndexTriggerRaw[hand] = st.IndexTriggerNoDeadzone[hand] =
                st.IndexTrigger[hand] = c.axis[1][0];
            st.HandTriggerRaw[hand] = st.HandTriggerNoDeadzone[hand] =
                st.HandTrigger[hand] = c.axis[2][0];
        }
    }
    /* Write only the part of the struct the caller's SDK has. */
    size_t n = g_minor >= 17 ? sizeof(st)
             : g_minor >= 11 ? offsetof(ovrInputState, IndexTriggerRaw)
             : offsetof(ovrInputState, IndexTriggerNoDeadzone);
    memcpy(out, &st, n);
    /* Games poll each controller type separately, so log changes per type
     * (Touch, LTouch, RTouch, or any other request). */
    static unsigned last[5][2] = {{~0u, ~0u}, {~0u, ~0u}, {~0u, ~0u}, {~0u, ~0u},
                                  {~0u, ~0u}};
    int slot = controllerType == ovrControllerType_LTouch ? 1
             : controllerType == ovrControllerType_RTouch ? 2
             : controllerType == ovrControllerType_Touch ? 0
             : controllerType == -1 /* ovrControllerType_Active */ ? 3 : 4;
    if (st.Buttons != last[slot][0] || st.Touches != last[slot][1]) {
        LOG("input(request 0x%x): controllers 0x%x buttons 0x%x touches 0x%x",
            (unsigned)controllerType, st.ControllerType, st.Buttons, st.Touches);
        last[slot][0] = st.Buttons;
        last[slot][1] = st.Touches;
    }
    return session_live(session) ? ovrSuccess
        : set_error(ovrError_InvalidSession, "no session");
}

/* ---- Entry points from newer SDKs (1.2x-1.4x) ----------------------------
 * A game's LibOVR loader resolves its SDK's whole function list and rejects
 * the runtime if any is missing (OVRPlugin asks for ovr_IsExtensionSupported
 * first). None of these features are backed by SteamVR, so they report
 * "unsupported" and games fall back to the plain path. */
EXPORT ovrResult ovr_IsExtensionSupported(ovrSession session, int extension,
                                          ovrBool *supported)
{
    TRACE_FIRST();
    (void)session; (void)extension;
    if (supported)
        *supported = ovrFalse;
    return ovrSuccess;
}

EXPORT ovrResult ovr_EnableExtension(ovrSession session, int extension)
{
    TRACE_FIRST();
    (void)session;
    LOG("ovr_EnableExtension(%d) -> unsupported", extension);
    return set_error(ovrError_Unsupported, "extension not supported");
}

EXPORT ovrResult ovr_ReportClientInfo(unsigned compilerVersion, int productVersion,
                                      int majorVersion, int minorVersion,
                                      int patchVersion, int buildNumber)
{
    TRACE_FIRST();
    (void)compilerVersion; (void)productVersion; (void)majorVersion;
    (void)minorVersion; (void)patchVersion; (void)buildNumber;
    return ovrSuccess;
}

EXPORT ovrResult ovr_SetClientColorDesc(ovrSession session, const void *desc)
{
    TRACE_FIRST();
    (void)session; (void)desc;
    return ovrSuccess;
}

/* Vulkan extensions the runtime needs. OVRPlugin queries these during
 * initialisation even for D3D11 games and gives up on failure. SteamVR's
 * Vulkan path isn't wired up here, so report an empty list. A NULL buffer
 * asks for the required size. */
static ovrResult vk_extension_list(const char *fn, const char *list, char *names,
                                   uint32_t *size)
{
    if (!size)
        return set_error(ovrError_InvalidParameter, "null size");
    uint32_t need = (uint32_t)strlen(list) + 1;
    LOG("%s(buffer %u) -> \"%s\"", fn, names ? *size : 0, list);
    if (!names) {
        *size = need;
        return ovrSuccess;
    }
    if (*size < need) {
        *size = need;
        return set_error(ovrError_InsufficientArraySize, "buffer too small");
    }
    memcpy(names, list, need);
    *size = need;
    return ovrSuccess;
}

EXPORT ovrResult ovr_GetInstanceExtensionsVk(ovrGraphicsLuid luid, char *names,
                                             uint32_t *size)
{
    TRACE_FIRST();
    (void)luid;
    return vk_extension_list(__func__, "", names, size);
}

EXPORT ovrResult ovr_GetDeviceExtensionsVk(ovrGraphicsLuid luid, char *names,
                                           uint32_t *size)
{
    TRACE_FIRST();
    (void)luid;
    return vk_extension_list(__func__, "", names, size);
}

#define UNSUPPORTED(name)                                     \
    EXPORT ovrResult name(void)                               \
    {                                                         \
        TRACE_FIRST();                                        \
        return set_error(ovrError_Unsupported, #name ": unsupported"); \
    }

UNSUPPORTED(ovr_GetFovStencil)
UNSUPPORTED(ovr_GetViewportStencil)
UNSUPPORTED(ovr_GetSessionPhysicalDeviceVk)
UNSUPPORTED(ovr_CreateTextureSwapChainVk)
UNSUPPORTED(ovr_GetTextureSwapChainBufferVk)
UNSUPPORTED(ovr_CreateMirrorTextureWithOptionsVk)
UNSUPPORTED(ovr_GetMirrorTextureBufferVk)

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
    /* When a frame started now will be on screen, so games predict their
     * render poses that far ahead (returning "now" made motion lag). */
    return ovr_GetTimeInSeconds() + vr_backend_seconds_to_photons();
}

/* ---- Properties: always return the caller's default -------------------- */

/* Log the first few property lookups by name, so we know what a game asks. */
static void log_property(const char *fn, const char *name)
{
    static LONG n;
    if (InterlockedIncrement(&n) <= 40)
        LOG("%s(\"%s\")", fn, name ? name : "(null)");
}

EXPORT ovrBool ovr_GetBool(ovrSession s, const char *name, ovrBool def)
{
    TRACE_FIRST();
    (void)s;
    log_property("ovr_GetBool", name);
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
    (void)s;
    log_property("ovr_GetInt", name);
    return def;
}

EXPORT ovrBool ovr_SetInt(ovrSession s, const char *name, int v)
{
    TRACE_FIRST();
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

/* LibOVR's built-in user-profile defaults (OVR_CAPI_Keys.h). */
EXPORT float ovr_GetFloat(ovrSession s, const char *name, float def)
{
    TRACE_FIRST();
    (void)s;
    log_property("ovr_GetFloat", name);
    if (!name)
        return def;
    if (!strcmp(name, "PlayerHeight"))
        return 1.778f;
    if (!strcmp(name, "EyeHeight"))
        return 1.675f;
    if (!strcmp(name, "IPD"))
        return 0.064f;
    /* Seconds per display frame; the real runtime reports the HMD's rate. */
    if (!strcmp(name, "VsyncToNextVsync"))
        return 1.0f / vr_backend_display_hz();
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
    (void)s;
    log_property("ovr_GetFloatArray", name);
    if (!name || !v)
        return 0;
    const float *src = NULL;
    unsigned n = 0;
    static const float neck_eye[2] = {0.0805f, 0.075f};   /* horizontal, vertical */
    static const float eye_nose[2] = {0.0315f, 0.0315f};  /* left, right */
    if (!strcmp(name, "NeckEyeDistance")) {
        src = neck_eye;
        n = 2;
    } else if (!strcmp(name, "EyeToNoseDist")) {
        src = eye_nose;
        n = 2;
    }
    if (n > cap)
        n = cap;
    for (unsigned i = 0; i < n; i++)
        v[i] = src[i];
    return n;
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
    (void)s;
    log_property("ovr_GetString", name);
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

/* Log every OVRSTUB_* variable the process sees, so a log shows which
 * debug switches were actually in effect (launch options are easy to typo). */
static void log_ovrstub_env(void)
{
    wchar_t *env = GetEnvironmentStringsW();
    int n = 0;
    for (wchar_t *e = env; e && *e; e += wcslen(e) + 1)
        if (!wcsncmp(e, L"OVRSTUB_", 8)) {
            LOG("env %ls", e);
            n++;
        }
    if (!n)
        LOG("env: no OVRSTUB_* variables set");
    if (env)
        FreeEnvironmentStringsW(env);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        QueryPerformanceFrequency(&g_qpc_freq);
        LOG("loaded (build %s)", OVRSTUB_BUILD_ID);
        log_ovrstub_env();
        create_hmd_connected_event();
        sigcheck_install();
        redirect_install(inst);
    }
    return TRUE;
}
