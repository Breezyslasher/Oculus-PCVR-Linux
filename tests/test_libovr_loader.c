/* Mimics a game's LibOVR loader: load the runtime from the (absent) Oculus
 * install path. Run through ovrinject.exe, the load must be redirected to the
 * injected shim, which then reports "runtime present, no HMD". */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

typedef int32_t (*init_t)(const void *);
typedef int32_t (*create_t)(void **, void *);
typedef struct { int32_t Type; char rest[260]; } hmd_desc;
typedef hmd_desc (*desc_t)(void *);

int main(void)
{
    HMODULE rt = LoadLibraryW(
        L"C:\\Program Files\\Oculus\\Support\\oculus-runtime\\LibOVRRT64_1.dll");
    if (!rt) { printf("FAIL: runtime load was not redirected\n"); return 1; }

    init_t init = (init_t)(void *)GetProcAddress(rt, "ovr_Initialize");
    create_t create = (create_t)(void *)GetProcAddress(rt, "ovr_Create");
    desc_t desc = (desc_t)(void *)GetProcAddress(rt, "ovr_GetHmdDesc");
    if (!init || !create || !desc) { printf("FAIL: missing exports\n"); return 1; }

    struct { uint32_t flags, minor; void *cb; uintptr_t ud; uint32_t t, pad; } p;
    memset(&p, 0, sizeof(p));
    if (init(&p) != 0) { printf("FAIL: ovr_Initialize\n"); return 1; }
    if (desc(NULL).Type != 0) { printf("FAIL: HMD type not None\n"); return 1; }
    void *session = (void *)1;
    if (create(&session, NULL) != -1007 || session) {
        printf("FAIL: ovr_Create should return ovrError_NoHmd\n");
        return 1;
    }
    printf("PASS: libovr redirect + no-HMD\n");
    return 0;
}
