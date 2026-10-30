/*
 * Minimal LibOVR 1.x type definitions for the LibOVRRT shim.
 *
 * Written from the public LibOVR API (cross-checked against the ovr-sys
 * bindings for LibOVR 1.15), not copied from the Oculus SDK headers. Only the
 * types the shim needs are defined. Layouts target 64-bit Windows.
 *
 * ABI note: any struct a function returns by value must match the real size
 * class. On Win64, structs of 1/2/4/8 bytes come back in RAX; everything else
 * goes through a hidden caller-allocated pointer. Getting that wrong corrupts
 * the caller's stack, so the static_asserts below pin the sizes.
 */
#ifndef OVRSTUB_OVR_TYPES_H
#define OVRSTUB_OVR_TYPES_H

#include <assert.h>
#include <stdint.h>

typedef int32_t ovrResult;
typedef char ovrBool;
#define ovrFalse 0
#define ovrTrue 1

typedef struct ovrHmdStruct *ovrSession;
typedef struct ovrTextureSwapChainData *ovrTextureSwapChain;
typedef struct ovrMirrorTextureData *ovrMirrorTexture;

enum {
    ovrSuccess = 0,
    ovrSuccess_NotVisible = 1000,
    ovrError_MemoryAllocationFailure = -1000,
    ovrError_InvalidSession = -1002,
    ovrError_NotInitialized = -1004,
    ovrError_InvalidParameter = -1005,
    ovrError_NoHmd = -1007,
    ovrError_Unsupported = -1009,
    ovrError_RuntimeException = -1016,
    ovrError_Initialize = -3000,
};

typedef enum { ovrHmd_None = 0, ovrHmd_CV1 = 14 } ovrHmdType;
typedef enum { ovrEye_Left = 0, ovrEye_Right = 1, ovrEye_Count = 2 } ovrEyeType;

typedef void (*ovrLogCallback)(uintptr_t userData, int level, const char *message);

typedef struct {
    uint32_t Flags;
    uint32_t RequestedMinorVersion;
    ovrLogCallback LogCallback;
    uintptr_t UserData;
    uint32_t ConnectionTimeoutMS;
    uint8_t _pad0[4];
} ovrInitParams;

typedef struct {
    ovrResult Result;
    char ErrorString[512];
} ovrErrorInfo;

typedef struct { char Reserved[8]; } ovrGraphicsLuid;
typedef struct { int w, h; } ovrSizei;
typedef struct { int x, y; } ovrVector2i;
typedef struct { ovrVector2i Pos; ovrSizei Size; } ovrRecti;
typedef struct { float x, y; } ovrVector2f;
typedef struct { float x, y, z; } ovrVector3f;
typedef struct { float x, y, z, w; } ovrQuatf;
typedef struct { ovrQuatf Orientation; ovrVector3f Position; } ovrPosef;
typedef struct { float UpTan, DownTan, LeftTan, RightTan; } ovrFovPort;

typedef struct {
    ovrPosef ThePose;
    ovrVector3f AngularVelocity;
    ovrVector3f LinearVelocity;
    ovrVector3f AngularAcceleration;
    ovrVector3f LinearAcceleration;
    uint8_t _pad0[4];
    double TimeInSeconds;
} ovrPoseStatef;

typedef struct {
    ovrHmdType Type;
    uint8_t _pad0[4];
    char ProductName[64];
    char Manufacturer[64];
    short VendorId;
    short ProductId;
    char SerialNumber[24];
    short FirmwareMajor;
    short FirmwareMinor;
    unsigned AvailableHmdCaps;
    unsigned DefaultHmdCaps;
    unsigned AvailableTrackingCaps;
    unsigned DefaultTrackingCaps;
    ovrFovPort DefaultEyeFov[ovrEye_Count];
    ovrFovPort MaxEyeFov[ovrEye_Count];
    ovrSizei Resolution;
    float DisplayRefreshRate;
    uint8_t _pad1[4];
} ovrHmdDesc;

typedef struct {
    ovrBool IsVisible;
    ovrBool HmdPresent;
    ovrBool HmdMounted;
    ovrBool DisplayLost;
    ovrBool ShouldQuit;
    ovrBool ShouldRecenter;
    /* Later SDKs append more ovrBool fields. The shim only writes these six,
     * which every 1.x version has, so older callers' buffers stay safe. */
} ovrSessionStatus;

typedef struct {
    float FrustumHFovInRadians;
    float FrustumVFovInRadians;
    float FrustumNearZInMeters;
    float FrustumFarZInMeters;
} ovrTrackerDesc;

typedef struct {
    unsigned TrackerFlags;
    ovrPosef Pose;
    ovrPosef LeveledPose;
    uint8_t _pad0[4];
} ovrTrackerPose;

typedef struct {
    ovrPoseStatef HeadPose;
    unsigned StatusFlags;
    ovrPoseStatef HandPoses[2];
    unsigned HandStatusFlags[2];
    ovrPosef CalibratedOrigin;
} ovrTrackingState;

/* ovr_GetRenderDesc (pre-1.17): eye offset as a vector. */
typedef struct {
    ovrEyeType Eye;
    ovrFovPort Fov;
    ovrRecti DistortedViewport;
    ovrVector2f PixelsPerTanAngleAtCenter;
    ovrVector3f HmdToEyeOffset;
} ovrEyeRenderDescV1;

/* ovr_GetRenderDesc2 (1.17+): eye offset as a full pose. */
typedef struct {
    ovrEyeType Eye;
    ovrFovPort Fov;
    ovrRecti DistortedViewport;
    ovrVector2f PixelsPerTanAngleAtCenter;
    ovrPosef HmdToEyePose;
} ovrEyeRenderDesc;

typedef struct {
    int SampleRateHz;
    int SampleSizeInBytes;
    int QueueMinSizeToAvoidStarvation;
    int SubmitMinSamples;
    int SubmitMaxSamples;
    int SubmitOptimalSamples;
} ovrTouchHapticsDesc;

/* ---- Texture swap chains, layers and submission (Milestone 3b) ---------- */

typedef int ovrTextureType;   /* ovrTexture_2D = 0 is all we handle */
typedef int ovrTextureFormat;
enum {
    OVR_FORMAT_UNKNOWN = 0,
    OVR_FORMAT_R8G8B8A8_UNORM = 4,
    OVR_FORMAT_R8G8B8A8_UNORM_SRGB = 5,
    OVR_FORMAT_B8G8R8A8_UNORM = 6,
    OVR_FORMAT_B8G8R8A8_UNORM_SRGB = 7,
    OVR_FORMAT_B8G8R8X8_UNORM = 8,
    OVR_FORMAT_B8G8R8X8_UNORM_SRGB = 9,
    OVR_FORMAT_R16G16B16A16_FLOAT = 10,
};

enum {
    ovrTextureBind_DX_RenderTarget = 0x0001,
    ovrTextureBind_DX_UnorderedAccess = 0x0002,
    ovrTextureBind_DX_DepthStencil = 0x0004,
};
enum {
    ovrTextureMisc_DX_Typeless = 0x0001,
    ovrTextureMisc_AllowGenerateMips = 0x0002,
};

typedef struct {
    ovrTextureType Type;
    ovrTextureFormat Format;
    int ArraySize;
    int Width;
    int Height;
    int MipLevels;
    int SampleCount;
    ovrBool StaticImage;
    unsigned MiscFlags;
    unsigned BindFlags;
} ovrTextureSwapChainDesc;

typedef struct {
    ovrTextureFormat Format;
    int Width;
    int Height;
    unsigned MiscFlags;
    unsigned MirrorOptions;
} ovrMirrorTextureDesc;

typedef enum {
    ovrLayerType_Disabled = 0,
    ovrLayerType_EyeFov = 1,
    ovrLayerType_Quad = 3,
    ovrLayerType_EyeMatrix = 5,
} ovrLayerType;

typedef struct {
    ovrLayerType Type;
    unsigned Flags;
} ovrLayerHeader;

typedef struct {
    ovrLayerHeader Header;
    ovrTextureSwapChain ColorTexture[ovrEye_Count];
    ovrRecti Viewport[ovrEye_Count];
    ovrFovPort Fov[ovrEye_Count];
    ovrPosef RenderPose[ovrEye_Count];
    double SensorSampleTime;
} ovrLayerEyeFov;

typedef struct {
    ovrVector3f HmdToEyeOffset[ovrEye_Count];
    float HmdSpaceToWorldScaleInMeters;
} ovrViewScaleDesc;

static_assert(sizeof(ovrTextureSwapChainDesc) == 40, "ovrTextureSwapChainDesc");
static_assert(sizeof(ovrLayerEyeFov) == 152, "ovrLayerEyeFov");

typedef struct {
    ovrBool IsOculusServiceRunning;
    ovrBool IsOculusHMDConnected;
    uint8_t _pad0[6];
} ovrDetectResult;

typedef struct {
    int ColorSpace;
    uint8_t _pad0[4];
} ovrHmdColorDesc;

static_assert(sizeof(ovrInitParams) == 32, "ovrInitParams");
static_assert(sizeof(ovrErrorInfo) == 516, "ovrErrorInfo");
static_assert(sizeof(ovrPoseStatef) == 88, "ovrPoseStatef");
static_assert(sizeof(ovrHmdDesc) == 264, "ovrHmdDesc");
static_assert(sizeof(ovrSizei) == 8, "ovrSizei is returned in RAX");
static_assert(sizeof(ovrDetectResult) == 8, "ovrDetectResult is returned in RAX");
static_assert(sizeof(ovrHmdColorDesc) == 8, "ovrHmdColorDesc is returned in RAX");

#endif
