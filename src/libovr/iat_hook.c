#include "iat_hook.h"

#include <string.h>
#include <wchar.h>

#define PSAPI_VERSION 2
#include <psapi.h>

#include "../common/log.h"

#define LOG(...) stub_log("libovr-redirect", __VA_ARGS__)

typedef HMODULE(WINAPI *LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI *LoadLibraryExW_t)(LPCWSTR, HANDLE, DWORD);
typedef HMODULE(WINAPI *LoadLibraryA_t)(LPCSTR);
typedef HMODULE(WINAPI *LoadLibraryExA_t)(LPCSTR, HANDLE, DWORD);

static HMODULE g_self, g_kernel32, g_kernelbase;
static WCHAR g_self_path[MAX_PATH];
/* Our Platform SDK stub, if it sits next to this DLL. */
static WCHAR g_platform_path[MAX_PATH];
static LoadLibraryW_t real_LoadLibraryW;
static LoadLibraryExW_t real_LoadLibraryExW;
static LoadLibraryA_t real_LoadLibraryA;
static LoadLibraryExA_t real_LoadLibraryExA;

/* Every address under which a LoadLibrary* function may appear in an IAT
 * (kernel32 export, kernelbase export, or a forwarder resolved to either). */
struct target {
    const char *name;
    void *addrs[2];
    void *hook;
};
static struct target g_targets[4];

static LPCWSTR base_name(LPCWSTR path)
{
    LPCWSTR base = path;
    for (LPCWSTR p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    return base;
}

/* Where a load of `path` should really go, or NULL to leave it alone. */
static LPCWSTR redirect_target_w(LPCWSTR path)
{
    if (!path)
        return NULL;
    LPCWSTR base = base_name(path);
    if (_wcsnicmp(base, L"LibOVRRT64_1", 12) == 0)
        return g_self_path;
    if (g_platform_path[0] && _wcsnicmp(base, L"LibOVRPlatform64_1", 18) == 0)
        return g_platform_path;
    return NULL;
}

static LPCWSTR redirect_target_a(LPCSTR path, WCHAR *wide)
{
    if (!path || !MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH))
        return NULL;
    return redirect_target_w(wide);
}

static void patch_all_modules(void);

static HMODULE redirect(LPCWSTR requested, LPCWSTR target)
{
    LOG("redirecting load of %ls to %ls", requested, target);
    /* For our own module this just bumps the reference count, so the
     * caller's FreeLibrary stays balanced. */
    return real_LoadLibraryW(target);
}

static HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name)
{
    LPCWSTR target = redirect_target_w(name);
    if (target)
        return redirect(name, target);
    HMODULE m = real_LoadLibraryW(name);
    if (m)
        patch_all_modules();
    return m;
}

static HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    LPCWSTR target = redirect_target_w(name);
    if (target && !(flags & LOAD_LIBRARY_AS_DATAFILE))
        return redirect(name, target);
    HMODULE m = real_LoadLibraryExW(name, file, flags);
    if (m)
        patch_all_modules();
    return m;
}

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR name)
{
    WCHAR wide[MAX_PATH];
    LPCWSTR target = redirect_target_a(name, wide);
    if (target)
        return redirect(wide, target);
    HMODULE m = real_LoadLibraryA(name);
    if (m)
        patch_all_modules();
    return m;
}

static HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    WCHAR wide[MAX_PATH];
    LPCWSTR target = redirect_target_a(name, wide);
    if (target && !(flags & LOAD_LIBRARY_AS_DATAFILE))
        return redirect(wide, target);
    HMODULE m = real_LoadLibraryExA(name, file, flags);
    if (m)
        patch_all_modules();
    return m;
}

static void patch_module(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    /* kernel32's LoadLibrary* call kernelbase through kernel32's own IAT, so
     * patching the system DLLs would send our "real" calls back into us. */
    if (mod == g_self || mod == g_kernel32 || mod == g_kernelbase ||
        dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return;

    for (IMAGE_IMPORT_DESCRIPTOR *imp =
             (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);
         imp->Name; imp++) {
        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; thunk->u1.Function; thunk++) {
            void *cur = (void *)thunk->u1.Function;
            for (size_t t = 0; t < 4; t++) {
                struct target *tg = &g_targets[t];
                if (cur != tg->addrs[0] && cur != tg->addrs[1])
                    continue;
                DWORD old;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void *),
                                   PAGE_READWRITE, &old)) {
                    thunk->u1.Function = (ULONG_PTR)tg->hook;
                    VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &old);
                }
            }
        }
    }
}

static void patch_all_modules(void)
{
    HMODULE mods[1024];
    DWORD needed;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;
    DWORD count = needed / sizeof(HMODULE);
    if (count > 1024)
        count = 1024;
    for (DWORD i = 0; i < count; i++)
        patch_module(mods[i]);
}

void redirect_install(HMODULE self)
{
    HMODULE k32 = g_kernel32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE kb = g_kernelbase = GetModuleHandleW(L"kernelbase.dll");
    static const char *names[4] = {"LoadLibraryW", "LoadLibraryExW",
                                   "LoadLibraryA", "LoadLibraryExA"};
    void *hooks[4] = {(void *)hook_LoadLibraryW, (void *)hook_LoadLibraryExW,
                      (void *)hook_LoadLibraryA, (void *)hook_LoadLibraryExA};

    g_self = self;
    GetModuleFileNameW(self, g_self_path, MAX_PATH);
    wcscpy(g_platform_path, g_self_path);
    WCHAR *slash = (WCHAR *)base_name(g_platform_path);
    *slash = 0;
    if (wcslen(g_platform_path) + 32 < MAX_PATH)
        wcscat(g_platform_path, L"LibOVRPlatform64_1.dll");
    if (GetFileAttributesW(g_platform_path) == INVALID_FILE_ATTRIBUTES)
        g_platform_path[0] = 0;

    for (int i = 0; i < 4; i++) {
        g_targets[i].name = names[i];
        g_targets[i].addrs[0] = k32 ? (void *)GetProcAddress(k32, names[i]) : NULL;
        g_targets[i].addrs[1] = kb ? (void *)GetProcAddress(kb, names[i]) : NULL;
        g_targets[i].hook = hooks[i];
    }
    /* Prefer kernelbase, where the implementations actually live. */
    int r = kb ? 1 : 0;
    real_LoadLibraryW = (LoadLibraryW_t)g_targets[0].addrs[r];
    real_LoadLibraryExW = (LoadLibraryExW_t)g_targets[1].addrs[r];
    real_LoadLibraryA = (LoadLibraryA_t)g_targets[2].addrs[r];
    real_LoadLibraryExA = (LoadLibraryExA_t)g_targets[3].addrs[r];

    patch_all_modules();
    LOG("installed LoadLibrary redirect for LibOVRRT64_1.dll%s",
        g_platform_path[0] ? " and LibOVRPlatform64_1.dll" : "");
}
