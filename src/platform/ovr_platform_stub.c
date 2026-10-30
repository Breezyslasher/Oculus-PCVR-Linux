/*
 * LibOVRPlatform64_1.dll stub.
 *
 * Replaces the Platform SDK DLL that ships inside each Rift Store game folder.
 * The real DLL talks to the Oculus service; this one answers locally:
 *   - initialization always succeeds
 *   - the entitlement check always passes
 *   - the logged-in user is a fixed local identity
 * Every request is answered through the normal message queue, so games that
 * poll ovr_PopMessage() see the same flow as with the real runtime.
 *
 * Environment:
 *   OVRSTUB_USER_ID     numeric user ID   (default 1)
 *   OVRSTUB_USER_NAME   Oculus ID / display name (default "LinuxPlayer")
 *   OVRSTUB_LOCALE      locale string     (default "en_US")
 *   OVRSTUB_MSGTYPE_*   override a message type ID, e.g.
 *                       OVRSTUB_MSGTYPE_ENTITLEMENT=0x186B58B1
 *   OVRSTUB_LOG         also append the log to this file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../common/log.h"
#include "platform_types.h"

#define EXPORT __declspec(dllexport)
#define LOG(...) stub_log("ovr-platform", __VA_ARGS__)

struct stub_user {
    ovrID id;
    char oculus_id[64];
    char display_name[64];
    char image_url[1];
};

struct stub_error {
    int code;
    int http_code;
    char message[128];
};

struct stub_message {
    struct stub_message *next;
    ovrMessageType type;
    ovrRequest request_id;
    bool is_error;
    ovrUserHandle user;
    ovrErrorHandle error;
    ovrPlatformInitializeResult init_result;
};

static CRITICAL_SECTION g_lock;
static struct stub_message *g_queue_head, *g_queue_tail;
static ovrRequest g_next_request = 1;
static bool g_initialized;
static struct stub_user g_user;
static char g_locale[32] = "en_US";

static ovrMessageType message_type(const char *name, ovrMessageType fallback)
{
    char var[96];
    snprintf(var, sizeof(var), "OVRSTUB_MSGTYPE_%s", name);
    const char *v = getenv(var);
    return (v && *v) ? (ovrMessageType)strtoul(v, NULL, 0) : fallback;
}

static void load_identity(void)
{
    const char *v;

    g_user.id = 1;
    strcpy(g_user.oculus_id, "LinuxPlayer");
    if ((v = getenv("OVRSTUB_USER_ID")) && *v)
        g_user.id = _strtoui64(v, NULL, 0);
    if ((v = getenv("OVRSTUB_USER_NAME")) && *v)
        snprintf(g_user.oculus_id, sizeof(g_user.oculus_id), "%s", v);
    strcpy(g_user.display_name, g_user.oculus_id);
    if ((v = getenv("OVRSTUB_LOCALE")) && *v)
        snprintf(g_locale, sizeof(g_locale), "%s", v);
}

/* Queue a successful reply and return its request ID. */
static ovrRequest post_reply(ovrMessageType type, ovrUserHandle user,
                             const char *what)
{
    struct stub_message *m = calloc(1, sizeof(*m));
    if (!m)
        return invalidRequestID;

    EnterCriticalSection(&g_lock);
    m->type = type;
    m->request_id = g_next_request++;
    m->user = user;
    m->init_result = ovrPlatformInitialize_Success;
    if (g_queue_tail)
        g_queue_tail->next = m;
    else
        g_queue_head = m;
    g_queue_tail = m;
    LeaveCriticalSection(&g_lock);

    if (type == 0)
        LOG("WARNING: %s reply has unknown message type 0; the game will not "
            "recognize it (set the matching OVRSTUB_MSGTYPE_* override)", what);
    LOG("%s -> request %llu (type 0x%08X)", what,
        (unsigned long long)m->request_id, type);
    return m->request_id;
}

static ovrPlatformInitializeResult do_init(const char *fn, const char *app_id)
{
    LOG("%s(appId=%s) -> Success", fn, app_id ? app_id : "(null)");
    g_initialized = true;
    return ovrPlatformInitialize_Success;
}

static ovrRequest do_init_async(const char *fn, const char *app_id)
{
    do_init(fn, app_id);
    return post_reply(message_type("PLATFORM_INITIALIZE_WINDOWS_ASYNC",
                                   MSGTYPE_PLATFORM_INITIALIZE_WINDOWS_ASYNC),
                      NULL, fn);
}

/* ---- Initialization ---------------------------------------------------- */

EXPORT ovrPlatformInitializeResult ovr_PlatformInitializeWindows(const char *appId)
{
    return do_init(__func__, appId);
}

EXPORT ovrPlatformInitializeResult ovr_PlatformInitializeWindowsEx(
    const char *appId, int productVersion, int majorVersion)
{
    LOG("%s: SDK version %d.%d", __func__, productVersion, majorVersion);
    return do_init(__func__, appId);
}

EXPORT ovrRequest ovr_PlatformInitializeWindowsAsynchronous(const char *appId)
{
    return do_init_async(__func__, appId);
}

EXPORT ovrRequest ovr_PlatformInitializeWindowsAsynchronousEx(
    const char *appId, int productVersion, int majorVersion)
{
    LOG("%s: SDK version %d.%d", __func__, productVersion, majorVersion);
    return do_init_async(__func__, appId);
}

/* Entry points used by the Unity Oculus.Platform wrapper. */
EXPORT void ovr_UnityInitGlobals(void *loggingCallback)
{
    (void)loggingCallback;
    LOG("%s", __func__);
}

EXPORT bool ovr_UnityInitWrapper(const char *appId)
{
    return do_init(__func__, appId) == ovrPlatformInitialize_Success;
}

EXPORT bool ovr_UnityInitWrapperWindows(const char *appId, void *loggingCallback)
{
    (void)loggingCallback;
    return do_init(__func__, appId) == ovrPlatformInitialize_Success;
}

EXPORT ovrRequest ovr_UnityInitWrapperAsynchronous(const char *appId)
{
    return do_init_async(__func__, appId);
}

EXPORT ovrRequest ovr_UnityInitWrapperWindowsAsynchronous(const char *appId,
                                                         void *loggingCallback)
{
    (void)loggingCallback;
    return do_init_async(__func__, appId);
}

EXPORT bool ovr_IsPlatformInitialized(void)
{
    return g_initialized;
}

EXPORT void ovr_SetDeveloperAccessToken(const char *accessToken)
{
    (void)accessToken;
    LOG("%s (ignored)", __func__);
}

/* ---- Entitlement & user ------------------------------------------------ */

EXPORT ovrRequest ovr_Entitlement_GetIsViewerEntitled(void)
{
    return post_reply(message_type("ENTITLEMENT",
                                   MSGTYPE_ENTITLEMENT_GET_IS_VIEWER_ENTITLED),
                      NULL, __func__);
}

EXPORT ovrRequest ovr_User_GetLoggedInUser(void)
{
    return post_reply(message_type("USER_GET_LOGGED_IN_USER",
                                   MSGTYPE_USER_GET_LOGGED_IN_USER),
                      &g_user, __func__);
}

EXPORT ovrRequest ovr_User_Get(ovrID userID)
{
    LOG("%s(%llu): returning the local user", __func__,
        (unsigned long long)userID);
    return post_reply(message_type("USER_GET", MSGTYPE_USER_GET), &g_user,
                      __func__);
}

EXPORT ovrID ovr_GetLoggedInUserID(void)
{
    return g_user.id;
}

EXPORT const char *ovr_GetLoggedInUserLocale(void)
{
    return g_locale;
}

EXPORT ovrID ovr_User_GetID(ovrUserHandle u)
{
    return u ? u->id : 0;
}

EXPORT const char *ovr_User_GetOculusID(ovrUserHandle u)
{
    return u ? u->oculus_id : "";
}

EXPORT const char *ovr_User_GetDisplayName(ovrUserHandle u)
{
    return u ? u->display_name : "";
}

EXPORT const char *ovr_User_GetImageUrl(ovrUserHandle u)
{
    return u ? u->image_url : "";
}

/* ---- Message queue ----------------------------------------------------- */

EXPORT ovrMessageHandle ovr_PopMessage(void)
{
    EnterCriticalSection(&g_lock);
    struct stub_message *m = g_queue_head;
    if (m) {
        g_queue_head = m->next;
        if (!g_queue_head)
            g_queue_tail = NULL;
        m->next = NULL;
    }
    LeaveCriticalSection(&g_lock);
    return m;
}

EXPORT void ovr_FreeMessage(ovrMessageHandle m)
{
    free(m);
}

EXPORT ovrMessageType ovr_Message_GetType(ovrMessageHandle m)
{
    return m ? m->type : 0;
}

EXPORT bool ovr_Message_IsError(ovrMessageHandle m)
{
    return m ? m->is_error : true;
}

EXPORT ovrRequest ovr_Message_GetRequestID(ovrMessageHandle m)
{
    return m ? m->request_id : invalidRequestID;
}

EXPORT ovrUserHandle ovr_Message_GetUser(ovrMessageHandle m)
{
    return m ? m->user : NULL;
}

EXPORT ovrErrorHandle ovr_Message_GetError(ovrMessageHandle m)
{
    return m ? m->error : NULL;
}

EXPORT const char *ovr_Message_GetString(ovrMessageHandle m)
{
    (void)m;
    return "";
}

EXPORT ovrPlatformInitializeHandle ovr_Message_GetPlatformInitialize(ovrMessageHandle m)
{
    return m;
}

EXPORT ovrPlatformInitializeResult ovr_PlatformInitialize_GetResult(
    ovrPlatformInitializeHandle h)
{
    return h ? h->init_result : ovrPlatformInitialize_Uninitialized;
}

EXPORT int ovr_Error_GetCode(ovrErrorHandle e)
{
    return e ? e->code : 0;
}

EXPORT int ovr_Error_GetHttpCode(ovrErrorHandle e)
{
    return e ? e->http_code : 0;
}

EXPORT const char *ovr_Error_GetMessage(ovrErrorHandle e)
{
    return e ? e->message : "";
}

EXPORT const char *ovr_Error_GetDisplayableMessage(ovrErrorHandle e)
{
    return e ? e->message : "";
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_lock);
        load_identity();
        LOG("loaded (stub; user %llu \"%s\")",
            (unsigned long long)g_user.id, g_user.oculus_id);
    }
    return TRUE;
}
