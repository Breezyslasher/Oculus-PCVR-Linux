/*
 * ovrstub_xrlayer.dll: a small OpenXR API layer for Rift-store games that
 * run on SteamVR's OpenXR runtime (through Proton) instead of Meta's.
 *
 * Some of these games turn on OpenXR extensions that behave differently on
 * SteamVR than on Meta's runtime. BONELAB, for example, enables
 * XR_EXT_hand_interaction; over Steam Link, SteamVR then drives the hands
 * through that profile and the game's Oculus Touch actions get no poses
 * (hands stuck at the player's sides). This layer removes the extensions
 * named in OVRSTUB_XR_STRIP from the game's xrCreateInstance call, so the
 * runtime never enables them:
 *
 *   OVRSTUB_XR_STRIP=XR_EXT_hand_interaction %command% ...
 *
 * Names are separated by commas, spaces or semicolons. The game still
 * believes the extension is on; its calls that depend on it fail cleanly
 * (e.g. suggesting bindings for that profile returns PATH_UNSUPPORTED).
 * Not every game copes with that: BONELAB crashes at start without
 * XR_EXT_hand_tracking. For hand tracking there is a gentler switch,
 *
 *   OVRSTUB_XR_NO_HAND_JOINTS=1
 *
 * which keeps the extension but reports every hand as not tracked
 * (xrLocateHandJointsEXT isActive = false), so a game falls back to its
 * controllers instead of freezing its hands on joint data that never moves
 * (SteamVR over Steam Link offers hand joints even with hand tracking off).
 *
 * The shim registers this layer as an implicit layer in the game's Wine
 * prefix (see register_xr_layer in libovr_stub.c); its manifest sets
 * enable_environment to OVRSTUB_XR_STRIP, so the loader only loads it for
 * games launched with that variable. The types below are written from the
 * OpenXR loader/layer interface specification.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../common/log.h"

#define LOG(...) stub_log("xr-layer", __VA_ARGS__)
#define EXPORT __declspec(dllexport)
#define XRAPI_CALL __stdcall

typedef int32_t XrResult;
typedef uint64_t XrVersion;
typedef uint64_t XrInstance;
typedef int32_t XrStructureType;
typedef void (XRAPI_CALL *PFN_xrVoidFunction)(void);

#define XR_SUCCESS 0
#define XR_ERROR_VALIDATION_FAILURE (-1)
#define XR_ERROR_INITIALIZATION_FAILED (-6)
#define XR_ERROR_FUNCTION_UNSUPPORTED (-7)
#define XR_ERROR_HANDLE_INVALID (-12)

typedef XrResult (XRAPI_CALL *PFN_xrGetInstanceProcAddr)(XrInstance, const char *,
                                                        PFN_xrVoidFunction *);

typedef struct {
    char applicationName[128];
    uint32_t applicationVersion;
    char engineName[128];
    uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct {
    XrStructureType type;
    const void *next;
    uint64_t createFlags;
    XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char *const *enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *enabledExtensionNames;
} XrInstanceCreateInfo;

/* openxr_loader_negotiation.h */
enum {
    XR_LOADER_INTERFACE_STRUCT_LOADER_INFO = 1,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST = 2,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO = 4,
    XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO = 5,
};
#define XR_CURRENT_LOADER_API_LAYER_VERSION 1
#define XR_API_LAYER_INFO_STRUCT_VERSION 1
#define XR_MAX_API_LAYER_NAME_SIZE 256
#define XR_API_LAYER_MAX_SETTINGS_PATH_SIZE 512
#define XR_MAKE_VERSION(ma, mi, pa) \
    ((((uint64_t)(ma) & 0xffff) << 48) | (((uint64_t)(mi) & 0xffff) << 32) | ((pa) & 0xffffffff))

typedef struct XrApiLayerCreateInfo XrApiLayerCreateInfo;
typedef XrResult (XRAPI_CALL *PFN_xrCreateApiLayerInstance)(
    const XrInstanceCreateInfo *, const XrApiLayerCreateInfo *, XrInstance *);

typedef struct {
    int32_t structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    XrVersion minApiVersion;
    XrVersion maxApiVersion;
} XrNegotiateLoaderInfo;

typedef struct {
    int32_t structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t layerInterfaceVersion;
    XrVersion layerApiVersion;
    PFN_xrGetInstanceProcAddr getInstanceProcAddr;
    PFN_xrCreateApiLayerInstance createApiLayerInstance;
} XrNegotiateApiLayerRequest;

typedef struct XrApiLayerNextInfo {
    int32_t structType;
    uint32_t structVersion;
    size_t structSize;
    char layerName[XR_MAX_API_LAYER_NAME_SIZE];
    PFN_xrGetInstanceProcAddr nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance nextCreateApiLayerInstance;
    struct XrApiLayerNextInfo *next;
} XrApiLayerNextInfo;

struct XrApiLayerCreateInfo {
    int32_t structType;
    uint32_t structVersion;
    size_t structSize;
    void *loaderInstance;
    char settings_file_location[XR_API_LAYER_MAX_SETTINGS_PATH_SIZE];
    XrApiLayerNextInfo *nextInfo;
};

static PFN_xrGetInstanceProcAddr g_next_gipa;

/* XR_EXT_hand_tracking: only the fields we touch. */
typedef uint64_t XrHandTrackerEXT;
typedef struct {
    XrStructureType type;
    void *next;
    uint32_t isActive;      /* XrBool32 */
    uint32_t jointCount;
    void *jointLocations;
} XrHandJointLocationsEXT;
typedef XrResult (XRAPI_CALL *PFN_xrLocateHandJointsEXT)(XrHandTrackerEXT, const void *,
                                                         XrHandJointLocationsEXT *);
static PFN_xrLocateHandJointsEXT g_next_locate_joints;

static int no_hand_joints(void)
{
    const char *v = getenv("OVRSTUB_XR_NO_HAND_JOINTS");
    return v && *v && *v != '0';
}

static XrResult XRAPI_CALL layer_locate_hand_joints(XrHandTrackerEXT tracker,
                                                    const void *info,
                                                    XrHandJointLocationsEXT *locations)
{
    XrResult r = g_next_locate_joints(tracker, info, locations);
    if (r == XR_SUCCESS && locations && locations->isActive) {
        static int logged;
        if (!logged) {
            logged = 1;
            LOG("reporting hand joints as not tracked (OVRSTUB_XR_NO_HAND_JOINTS)");
        }
        locations->isActive = 0;
    }
    return r;
}

/* Whether `name` appears in the OVRSTUB_XR_STRIP list. */
static int stripped(const char *name)
{
    const char *list = getenv("OVRSTUB_XR_STRIP");
    if (!list || !name)
        return 0;
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ';' || *p == ' ')
            p++;
        const char *end = p;
        while (*end && *end != ',' && *end != ';' && *end != ' ')
            end++;
        if ((size_t)(end - p) == n && !strncmp(p, name, n))
            return 1;
        p = end;
    }
    return 0;
}

static XrResult XRAPI_CALL layer_gipa(XrInstance instance, const char *name,
                                      PFN_xrVoidFunction *function)
{
    if (!g_next_gipa) {
        if (function)
            *function = NULL;
        return instance ? XR_ERROR_HANDLE_INVALID : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    XrResult r = g_next_gipa(instance, name, function);
    if (r == XR_SUCCESS && function && *function && name &&
        !strcmp(name, "xrLocateHandJointsEXT") && no_hand_joints()) {
        g_next_locate_joints = (PFN_xrLocateHandJointsEXT)(void *)*function;
        *function = (PFN_xrVoidFunction)(void *)layer_locate_hand_joints;
    }
    return r;
}

static XrResult XRAPI_CALL layer_create_instance(const XrInstanceCreateInfo *info,
                                                 const XrApiLayerCreateInfo *layer_info,
                                                 XrInstance *instance)
{
    if (!info || !layer_info || !layer_info->nextInfo)
        return XR_ERROR_INITIALIZATION_FAILED;
    XrApiLayerNextInfo *next = layer_info->nextInfo;
    g_next_gipa = next->nextGetInstanceProcAddr;

    XrApiLayerCreateInfo down = *layer_info;
    down.nextInfo = next->next;

    XrInstanceCreateInfo ci = *info;
    const char **names = NULL;
    if (info->enabledExtensionCount) {
        names = calloc(info->enabledExtensionCount, sizeof(*names));
        if (!names)
            return XR_ERROR_INITIALIZATION_FAILED;
        uint32_t kept = 0;
        for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
            const char *e = info->enabledExtensionNames[i];
            if (stripped(e))
                LOG("hiding %s from the runtime", e);
            else
                names[kept++] = e;
        }
        ci.enabledExtensionCount = kept;
        ci.enabledExtensionNames = names;
    }
    XrResult r = next->nextCreateApiLayerInstance(&ci, &down, instance);
    LOG("xrCreateInstance(\"%s\") -> %d", info->applicationInfo.applicationName, (int)r);
    free(names);
    return r;
}

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loader, const char *layer_name,
    XrNegotiateApiLayerRequest *request)
{
    (void)layer_name;
    if (!loader || !request ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = XR_MAKE_VERSION(1, 0, 0);
    request->getInstanceProcAddr = layer_gipa;
    request->createApiLayerInstance = layer_create_instance;
    LOG("loaded; hiding: %s%s", getenv("OVRSTUB_XR_STRIP") ? getenv("OVRSTUB_XR_STRIP") : "(nothing)",
        no_hand_joints() ? "; hand joints reported as not tracked" : "");
    return XR_SUCCESS;
}
