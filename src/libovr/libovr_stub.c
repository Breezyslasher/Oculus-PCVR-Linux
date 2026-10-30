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

#include "../common/log.h"
#include "iat_hook.h"
#include "ovr_types.h"
#include "sigcheck.h"

#define EXPORT __declspec(dllexport)
#define LOG(...) stub_log("libovr", __VA_ARGS__)

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

/* ---- Lifetime ---------------------------------------------------------- */

EXPORT ovrResult ovr_Initialize(const ovrInitParams *params)
{
    LOG("ovr_Initialize(flags=0x%x, minor=%u)", params ? params->Flags : 0,
        params ? params->RequestedMinorVersion : 0);
    g_initialized = 1;
    return ovrSuccess;
}

EXPORT void ovr_Shutdown(void)
{
    LOG("ovr_Shutdown");
    g_initialized = 0;
}

EXPORT void ovr_GetLastErrorInfo(ovrErrorInfo *info)
{
    if (info)
        *info = g_last_error;
}

EXPORT const char *ovr_GetVersionString(void)
{
    return "1.0.0-ovrstub";
}

EXPORT int ovr_TraceMessage(int level, const char *message)
{
    LOG("trace[%d]: %s", level, message ? message : "");
    return 0;
}

EXPORT ovrResult ovr_IdentifyClient(const char *identity)
{
    LOG("ovr_IdentifyClient: %s", identity ? identity : "");
    return ovrSuccess;
}

/* ---- HMD / session ----------------------------------------------------- */

EXPORT ovrHmdDesc ovr_GetHmdDesc(ovrSession session)
{
    ovrHmdDesc desc;
    (void)session;
    memset(&desc, 0, sizeof(desc));
    desc.Type = ovrHmd_None;
    return desc;
}

EXPORT ovrResult ovr_Create(ovrSession *session, ovrGraphicsLuid *luid)
{
    (void)luid;
    if (session)
        *session = NULL;
    if (!g_initialized)
        return set_error(ovrError_NotInitialized, "ovr_Initialize not called");
    LOG("ovr_Create -> ovrError_NoHmd");
    return set_error(ovrError_NoHmd, "No HMD (ovrstub milestone 2)");
}

EXPORT void ovr_Destroy(ovrSession session)
{
    (void)session;
}

EXPORT ovrResult ovr_GetSessionStatus(ovrSession session, ovrSessionStatus *status)
{
    (void)session;
    if (status)
        memset(status, 0, sizeof(*status));
    return set_error(ovrError_InvalidSession, "no session");
}

EXPORT unsigned ovr_GetTrackerCount(ovrSession session)
{
    (void)session;
    return 0;
}

EXPORT ovrTrackerDesc ovr_GetTrackerDesc(ovrSession session, unsigned index)
{
    ovrTrackerDesc d;
    (void)session; (void)index;
    memset(&d, 0, sizeof(d));
    return d;
}

EXPORT ovrTrackerPose ovr_GetTrackerPose(ovrSession session, unsigned index)
{
    ovrTrackerPose p;
    (void)session; (void)index;
    memset(&p, 0, sizeof(p));
    return p;
}

EXPORT ovrTrackingState ovr_GetTrackingState(ovrSession session, double absTime,
                                             ovrBool latencyMarker)
{
    ovrTrackingState s;
    (void)session; (void)absTime; (void)latencyMarker;
    memset(&s, 0, sizeof(s));
    s.HeadPose.ThePose.Orientation.w = 1.0f;
    s.CalibratedOrigin.Orientation.w = 1.0f;
    return s;
}

EXPORT ovrTouchHapticsDesc ovr_GetTouchHapticsDesc(ovrSession session, int type)
{
    ovrTouchHapticsDesc d;
    (void)session; (void)type;
    memset(&d, 0, sizeof(d));
    return d;
}

EXPORT ovrHmdColorDesc ovr_GetHmdColorDesc(ovrSession session)
{
    ovrHmdColorDesc d;
    (void)session;
    memset(&d, 0, sizeof(d));
    return d;
}

EXPORT ovrSizei ovr_GetFovTextureSize(ovrSession session, ovrEyeType eye,
                                      ovrFovPort fov, float pixelsPerDisplayPixel)
{
    ovrSizei s = {0, 0};
    (void)session; (void)eye; (void)fov; (void)pixelsPerDisplayPixel;
    return s;
}

EXPORT ovrEyeRenderDescV1 ovr_GetRenderDesc(ovrSession session, ovrEyeType eye,
                                            ovrFovPort fov)
{
    ovrEyeRenderDescV1 d;
    (void)session;
    memset(&d, 0, sizeof(d));
    d.Eye = eye;
    d.Fov = fov;
    return d;
}

EXPORT ovrEyeRenderDesc ovr_GetRenderDesc2(ovrSession session, ovrEyeType eye,
                                           ovrFovPort fov)
{
    ovrEyeRenderDesc d;
    (void)session;
    memset(&d, 0, sizeof(d));
    d.Eye = eye;
    d.Fov = fov;
    d.HmdToEyePose.Orientation.w = 1.0f;
    return d;
}

/* Session-taking calls that report a result code. Their real argument lists
 * differ, but with no live session every one of them fails the same way, and
 * on Win64 the caller owns argument cleanup, so ignoring arguments is safe. */
#define NO_SESSION(name)                                     \
    EXPORT ovrResult name(void)                              \
    {                                                        \
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
NO_SESSION(ovr_GetTextureSwapChainLength)
NO_SESSION(ovr_GetTextureSwapChainCurrentIndex)
NO_SESSION(ovr_GetTextureSwapChainDesc)
NO_SESSION(ovr_CommitTextureSwapChain)
NO_SESSION(ovr_CreateTextureSwapChainDX)
NO_SESSION(ovr_GetTextureSwapChainBufferDX)
NO_SESSION(ovr_CreateMirrorTextureDX)
NO_SESSION(ovr_CreateMirrorTextureWithOptionsDX)
NO_SESSION(ovr_GetMirrorTextureBufferDX)
NO_SESSION(ovr_CreateTextureSwapChainGL)
NO_SESSION(ovr_GetTextureSwapChainBufferGL)
NO_SESSION(ovr_CreateMirrorTextureGL)
NO_SESSION(ovr_CreateMirrorTextureWithOptionsGL)
NO_SESSION(ovr_GetMirrorTextureBufferGL)
NO_SESSION(ovr_WaitToBeginFrame)
NO_SESSION(ovr_BeginFrame)
NO_SESSION(ovr_EndFrame)
NO_SESSION(ovr_SubmitFrame)
NO_SESSION(ovr_SubmitFrame2)
NO_SESSION(ovr_GetPerfStats)
NO_SESSION(ovr_ResetPerfStats)

EXPORT int ovr_GetTrackingOriginType(ovrSession session)
{
    (void)session;
    return 0;
}

EXPORT void ovr_ClearShouldRecenterFlag(ovrSession session)
{
    (void)session;
}

EXPORT unsigned ovr_GetConnectedControllerTypes(ovrSession session)
{
    (void)session;
    return 0;
}

EXPORT void ovr_DestroyTextureSwapChain(ovrSession session, ovrTextureSwapChain chain)
{
    (void)session; (void)chain;
}

EXPORT void ovr_DestroyMirrorTexture(ovrSession session, ovrMirrorTexture tex)
{
    (void)session; (void)tex;
}

/* ---- Timing ------------------------------------------------------------ */

EXPORT double ovr_GetTimeInSeconds(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)g_qpc_freq.QuadPart;
}

EXPORT double ovr_GetPredictedDisplayTime(ovrSession session, long long frameIndex)
{
    (void)session; (void)frameIndex;
    return ovr_GetTimeInSeconds();
}

/* ---- Properties: always return the caller's default -------------------- */

EXPORT ovrBool ovr_GetBool(ovrSession s, const char *name, ovrBool def)
{
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetBool(ovrSession s, const char *name, ovrBool v)
{
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

EXPORT int ovr_GetInt(ovrSession s, const char *name, int def)
{
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetInt(ovrSession s, const char *name, int v)
{
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

EXPORT float ovr_GetFloat(ovrSession s, const char *name, float def)
{
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetFloat(ovrSession s, const char *name, float v)
{
    (void)s; (void)name; (void)v;
    return ovrFalse;
}

EXPORT unsigned ovr_GetFloatArray(ovrSession s, const char *name, float *v,
                                  unsigned cap)
{
    (void)s; (void)name; (void)v; (void)cap;
    return 0;
}

EXPORT ovrBool ovr_SetFloatArray(ovrSession s, const char *name, const float *v,
                                 unsigned n)
{
    (void)s; (void)name; (void)v; (void)n;
    return ovrFalse;
}

EXPORT const char *ovr_GetString(ovrSession s, const char *name, const char *def)
{
    (void)s; (void)name;
    return def;
}

EXPORT ovrBool ovr_SetString(ovrSession s, const char *name, const char *v)
{
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
