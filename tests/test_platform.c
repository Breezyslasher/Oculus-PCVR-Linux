/* Exercises the Platform stub the way a game does: init, request the
 * entitlement and the logged-in user, then pump the message queue. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef int (*init_t)(const char *);
typedef uint64_t (*req_t)(void);
typedef void *(*pop_t)(void);
typedef uint32_t (*type_t)(void *);
typedef bool (*iserr_t)(void *);
typedef uint64_t (*reqid_t)(void *);
typedef void *(*getuser_t)(void *);
typedef const char *(*oculusid_t)(void *);
typedef void (*free_t)(void *);

#define LOAD(var, type, name)                                  \
    type var = (type)(void *)GetProcAddress(dll, name);        \
    if (!var) { printf("FAIL: missing export %s\n", name); return 1; }

int main(void)
{
    HMODULE dll = LoadLibraryW(L"LibOVRPlatform64_1.dll");
    if (!dll) { printf("FAIL: could not load stub\n"); return 1; }

    LOAD(init, init_t, "ovr_PlatformInitializeWindows");
    LOAD(entitled, req_t, "ovr_Entitlement_GetIsViewerEntitled");
    LOAD(logged_in, req_t, "ovr_User_GetLoggedInUser");
    LOAD(pop, pop_t, "ovr_PopMessage");
    LOAD(get_type, type_t, "ovr_Message_GetType");
    LOAD(is_error, iserr_t, "ovr_Message_IsError");
    LOAD(get_reqid, reqid_t, "ovr_Message_GetRequestID");
    LOAD(get_user, getuser_t, "ovr_Message_GetUser");
    LOAD(get_oculus_id, oculusid_t, "ovr_User_GetOculusID");
    LOAD(free_msg, free_t, "ovr_FreeMessage");

    if (init("123456789") != 0) { printf("FAIL: init\n"); return 1; }
    uint64_t r1 = entitled(), r2 = logged_in();

    int seen = 0;
    for (void *m; (m = pop()); free_msg(m)) {
        uint64_t id = get_reqid(m);
        if (is_error(m)) { printf("FAIL: error reply\n"); return 1; }
        if (id == r1 && get_type(m) == 0x186B58B1u) seen |= 1;
        if (id == r2 && get_user(m) &&
            strcmp(get_oculus_id(get_user(m)), "TestUser") == 0) seen |= 2;
    }
    if (seen != 3) { printf("FAIL: replies seen mask %d\n", seen); return 1; }
    printf("PASS: platform stub\n");
    return 0;
}
