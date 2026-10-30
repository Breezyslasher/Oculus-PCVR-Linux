#include "iat_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PSAPI_VERSION 2
#include <psapi.h>
#include <wincrypt.h>

#include "../common/log.h"

#define LOG(...) stub_log("libovr-redirect", __VA_ARGS__)
#define TRACE(...) stub_log("libovr-trace", __VA_ARGS__)
/* For hooks a game hits every frame (it polls the OculusHMDConnected event
 * ~10x per frame): log the first few calls only. Writing and flushing a log
 * line per call from the render thread caused visible stutter. */
#define TRACE_FEW(...)                                       \
    do {                                                     \
        static LONG n_;                                      \
        if (InterlockedIncrement(&n_) <= 8)                  \
            TRACE(__VA_ARGS__);                              \
    } while (0)

static HMODULE g_self;
static WCHAR g_self_path[MAX_PATH];
/* Our Platform SDK stub, if it sits next to this DLL. */
static WCHAR g_platform_path[MAX_PATH];
static WCHAR g_system_dir[MAX_PATH];
static size_t g_system_dir_len;

/* ---- Hook table ----------------------------------------------------------
 * Each entry is a function the game may import. Its export address in every
 * loaded system DLL that has it is recorded, so an IAT slot pointing at any
 * of them (directly or through a resolved forwarder) is recognized. `real`
 * is the implementation the hook calls; DLLs are tried in order, kernelbase
 * first, since that is where kernel32's forwarders lead. */
#define MAX_ADDRS 6
struct hook {
    const char *name;
    void *hook;
    void **real;
    void *addrs[MAX_ADDRS];
};

static const WCHAR *const g_source_dlls[] = {
    L"kernelbase.dll", L"kernel32.dll", L"advapi32.dll", L"ucrtbase.dll", L"msvcrt.dll",
    L"wintrust.dll", L"crypt32.dll", L"version.dll",
};

/* Pointers to the real implementations, typed from the SDK declarations. */
#define REAL(name) static __typeof__(&name) real_##name
REAL(LoadLibraryW);
REAL(LoadLibraryExW);
REAL(LoadLibraryA);
REAL(LoadLibraryExA);
REAL(OpenEventW);
REAL(OpenEventA);
REAL(OpenMutexW);
REAL(OpenMutexA);
REAL(GetEnvironmentVariableW);
REAL(GetEnvironmentVariableA);
REAL(getenv);
REAL(_wgetenv);
REAL(RegOpenKeyExW);
REAL(RegOpenKeyExA);
REAL(CreateFileW);
REAL(GetFileAttributesW);
REAL(GetFileAttributesExW);
REAL(GetProcAddress);
REAL(CertGetNameStringW);
REAL(CertGetNameStringA);
REAL(GetFileVersionInfoSizeW);
REAL(GetFileVersionInfoW);

static LPCWSTR base_name(LPCWSTR path)
{
    LPCWSTR base = path;
    for (LPCWSTR p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    return base;
}

/* Case-insensitive "does s contain any of the needles". */
static int contains_w(LPCWSTR s, const WCHAR *const *needles)
{
    if (!s)
        return 0;
    for (; *needles; needles++) {
        size_t n = wcslen(*needles);
        for (LPCWSTR p = s; *p; p++)
            if (_wcsnicmp(p, *needles, n) == 0)
                return 1;
    }
    return 0;
}

static int contains_a(LPCSTR s, const WCHAR *const *needles)
{
    WCHAR wide[1024];
    if (!s || !MultiByteToWideChar(CP_ACP, 0, s, -1, wide, 1024))
        return 0;
    return contains_w(wide, needles);
}

static const WCHAR *const k_ovr_words[] = {L"ovr", L"oculus", NULL};

/* ---- LoadLibrary redirect ---------------------------------------------- */

static void patch_all_modules(void);

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

static HMODULE redirect(LPCWSTR requested, LPCWSTR target)
{
    LOG("redirecting load of %ls to %ls", requested, target);
    /* For our own module this just bumps the reference count, so the
     * caller's FreeLibrary stays balanced. */
    return real_LoadLibraryW(target);
}

static HMODULE loaded(LPCWSTR name, HMODULE m)
{
    if (contains_w(name, k_ovr_words))
        TRACE("LoadLibrary(%ls) -> %p (error %lu)", name, (void *)m, m ? 0 : GetLastError());
    if (m)
        patch_all_modules();
    return m;
}

static HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name)
{
    LPCWSTR target = redirect_target_w(name);
    if (target)
        return redirect(name, target);
    return loaded(name, real_LoadLibraryW(name));
}

static HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    LPCWSTR target = redirect_target_w(name);
    if (target && !(flags & LOAD_LIBRARY_AS_DATAFILE))
        return redirect(name, target);
    return loaded(name, real_LoadLibraryExW(name, file, flags));
}

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR name)
{
    WCHAR wide[MAX_PATH] = L"";
    LPCWSTR target = redirect_target_a(name, wide);
    if (target)
        return redirect(wide, target);
    return loaded(wide, real_LoadLibraryA(name));
}

static HMODULE WINAPI hook_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    WCHAR wide[MAX_PATH] = L"";
    LPCWSTR target = redirect_target_a(name, wide);
    if (target && !(flags & LOAD_LIBRARY_AS_DATAFILE))
        return redirect(wide, target);
    return loaded(wide, real_LoadLibraryExA(name, file, flags));
}

/* ---- Diagnostic hooks: log what the game probes, change nothing ---------- */

static HANDLE WINAPI hook_OpenEventW(DWORD access, BOOL inherit, LPCWSTR name)
{
    HANDLE h = real_OpenEventW(access, inherit, name);
    TRACE_FEW("OpenEventW(%ls) -> %s", name ? name : L"(null)", h ? "found" : "not found");
    return h;
}

static HANDLE WINAPI hook_OpenEventA(DWORD access, BOOL inherit, LPCSTR name)
{
    HANDLE h = real_OpenEventA(access, inherit, name);
    TRACE_FEW("OpenEventA(%s) -> %s", name ? name : "(null)", h ? "found" : "not found");
    return h;
}

static HANDLE WINAPI hook_OpenMutexW(DWORD access, BOOL inherit, LPCWSTR name)
{
    HANDLE h = real_OpenMutexW(access, inherit, name);
    TRACE_FEW("OpenMutexW(%ls) -> %s", name ? name : L"(null)", h ? "found" : "not found");
    return h;
}

static HANDLE WINAPI hook_OpenMutexA(DWORD access, BOOL inherit, LPCSTR name)
{
    HANDLE h = real_OpenMutexA(access, inherit, name);
    TRACE_FEW("OpenMutexA(%s) -> %s", name ? name : "(null)", h ? "found" : "not found");
    return h;
}

static DWORD WINAPI hook_GetEnvironmentVariableW(LPCWSTR name, LPWSTR buf, DWORD size)
{
    DWORD r = real_GetEnvironmentVariableW(name, buf, size);
    if (contains_w(name, k_ovr_words))
        TRACE_FEW("GetEnvironmentVariableW(%ls) -> %s", name,
              r && r < size ? "set" : (r ? "set (buffer too small)" : "not set"));
    return r;
}

static DWORD WINAPI hook_GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    DWORD r = real_GetEnvironmentVariableA(name, buf, size);
    if (contains_a(name, k_ovr_words))
        TRACE_FEW("GetEnvironmentVariableA(%s) -> %s%s", name,
              r && r < size ? "\"" : (r ? "set (buffer too small)" : "not set"),
              r && r < size ? buf : "");
    return r;
}

static char *hook_getenv(const char *name)
{
    char *v = real_getenv(name);
    if (contains_a(name, k_ovr_words))
        TRACE_FEW("getenv(%s) -> %s", name, v ? v : "(not set)");
    return v;
}

static wchar_t *hook__wgetenv(const wchar_t *name)
{
    wchar_t *v = real__wgetenv(name);
    if (contains_w(name, k_ovr_words))
        TRACE_FEW("_wgetenv(%ls) -> %ls", name, v ? v : L"(not set)");
    return v;
}

static LONG WINAPI hook_RegOpenKeyExW(HKEY root, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    LONG r = real_RegOpenKeyExW(root, sub, opt, sam, out);
    if (contains_w(sub, k_ovr_words))
        TRACE("RegOpenKeyExW(%ls, sam 0x%lx) -> %ld", sub, (unsigned long)sam, r);
    return r;
}

static LONG WINAPI hook_RegOpenKeyExA(HKEY root, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out)
{
    LONG r = real_RegOpenKeyExA(root, sub, opt, sam, out);
    if (contains_a(sub, k_ovr_words))
        TRACE("RegOpenKeyExA(%s, sam 0x%lx) -> %ld", sub, (unsigned long)sam, r);
    return r;
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                                      HANDLE tmpl)
{
    HANDLE h = real_CreateFileW(path, access, share, sa, disp, flags, tmpl);
    if (contains_w(path, k_ovr_words))
        TRACE("CreateFileW(%ls) -> %s", path,
              h != INVALID_HANDLE_VALUE ? "opened" : "failed");
    return h;
}

static DWORD WINAPI hook_GetFileAttributesW(LPCWSTR path)
{
    DWORD a = real_GetFileAttributesW(path);
    if (contains_w(path, k_ovr_words))
        TRACE("GetFileAttributesW(%ls) -> %s", path,
              a != INVALID_FILE_ATTRIBUTES ? "exists" : "missing");
    return a;
}

static BOOL WINAPI hook_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS lvl, LPVOID out)
{
    BOOL ok = real_GetFileAttributesExW(path, lvl, out);
    if (contains_w(path, k_ovr_words))
        TRACE("GetFileAttributesExW(%ls) -> %s", path, ok ? "exists" : "missing");
    return ok;
}

/* Diagnostics for the runtime's post-signature checks: the signer-name read
 * and the version-resource read. Log only, pass results through. */
static DWORD WINAPI hook_CertGetNameStringW(PCCERT_CONTEXT cert, DWORD type, DWORD flags,
                                            void *para, LPWSTR name, DWORD cch)
{
    DWORD r = real_CertGetNameStringW(cert, type, flags, para, name, cch);
    if (name && cch > 1 && r > 1)
        TRACE("CertGetNameStringW(type=%lu) -> \"%ls\"", (unsigned long)type, name);
    else
        TRACE("CertGetNameStringW(type=%lu) -> %lu chars", (unsigned long)type,
              (unsigned long)r);
    return r;
}

static DWORD WINAPI hook_CertGetNameStringA(PCCERT_CONTEXT cert, DWORD type, DWORD flags,
                                            void *para, LPSTR name, DWORD cch)
{
    DWORD r = real_CertGetNameStringA(cert, type, flags, para, name, cch);
    if (name && cch > 1 && r > 1)
        TRACE("CertGetNameStringA(type=%lu) -> \"%s\"", (unsigned long)type, name);
    return r;
}

static DWORD WINAPI hook_GetFileVersionInfoSizeW(LPCWSTR path, LPDWORD handle)
{
    DWORD r = real_GetFileVersionInfoSizeW(path, handle);
    if (contains_w(path, k_ovr_words))
        TRACE("GetFileVersionInfoSizeW(%ls) -> %lu", path, (unsigned long)r);
    return r;
}

static BOOL WINAPI hook_GetFileVersionInfoW(LPCWSTR path, DWORD handle, DWORD len, LPVOID data)
{
    BOOL ok = real_GetFileVersionInfoW(path, handle, len, data);
    if (contains_w(path, k_ovr_words))
        TRACE("GetFileVersionInfoW(%ls) -> %s", path, ok ? "ok" : "fail");
    return ok;
}

static int is_system_module(HMODULE mod);
static FARPROC WINAPI hook_GetProcAddress(HMODULE mod, LPCSTR name);

#define HOOK(fn) {#fn, (void *)hook_##fn, (void **)&real_##fn, {0}}
#define MAX_HOOKS 64
static struct hook g_hooks[MAX_HOOKS] = {
    HOOK(LoadLibraryW), HOOK(LoadLibraryExW), HOOK(LoadLibraryA), HOOK(LoadLibraryExA),
    HOOK(OpenEventW), HOOK(OpenEventA), HOOK(OpenMutexW), HOOK(OpenMutexA),
    HOOK(GetEnvironmentVariableW), HOOK(GetEnvironmentVariableA), HOOK(getenv), HOOK(_wgetenv),
    HOOK(RegOpenKeyExW), HOOK(RegOpenKeyExA),
    HOOK(CreateFileW), HOOK(GetFileAttributesW), HOOK(GetFileAttributesExW),
    HOOK(GetProcAddress),
    HOOK(CertGetNameStringW), HOOK(CertGetNameStringA),
    HOOK(GetFileVersionInfoSizeW), HOOK(GetFileVersionInfoW),
};
/* Entries end at the first empty slot. */
static size_t count_hooks(void)
{
    size_t n = 0;
    while (n < MAX_HOOKS && g_hooks[n].name)
        n++;
    return n;
}
#define NHOOKS count_hooks()

void hook_register(const char *name, void *hook, void **real)
{
    size_t n = count_hooks();
    if (n >= MAX_HOOKS) {
        LOG("hook table full; %s not hooked", name);
        return;
    }
    g_hooks[n] = (struct hook){name, hook, real, {0}};
}

/* Functions the game resolves at run time (delay-load, or LoadLibrary +
 * GetProcAddress) never appear in an import table, so hand out our hooks
 * here too. Only system DLLs' exports are swapped. */
/* Stand-in for LibOVR entry points the shim doesn't implement. Newer SDKs'
 * loaders resolve their whole function list and reject the runtime if any
 * is missing, though games only call most of them when the feature exists.
 * Nearly all of these return ovrResult, so answer ovrError_Unsupported
 * (-1009, which is the low 32 bits the caller reads as its int result); a
 * caller expecting void ignores it. On Win64 the caller cleans up
 * the arguments, so ignoring them is safe. */
static INT_PTR WINAPI unsupported_ovr_entry(void)
{
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 20)
        LOG("an unimplemented ovr_* entry point was called -> ovrError_Unsupported");
    return -1009;
}

static INT_PTR WINAPI empty_platform_entry(void)
{
    static LONG calls;
    if (InterlockedIncrement(&calls) <= 20)
        LOG("an unimplemented Platform SDK function was called -> 0/NULL");
    return 0;
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC p = real_GetProcAddress(mod, name);
    /* Meta's CAPI shim resolves every LibOVR export after loading the
     * runtime; a single missing one makes it unload and retry forever. For
     * lookups on our own runtime, hand back the stand-in instead, and log
     * the name so a function that matters can be implemented properly. */
    if (((ULONG_PTR)name >> 16) != 0 && !p &&
        (name[0] == 'o' || name[0] == 'O') &&
        (name[1] == 'v' || name[1] == 'V')) {
        if (mod == g_self && !strncmp(name, "ovr_", 4)) {
            TRACE("GetProcAddress(%s) -> not implemented, using stand-in", name);
            return (FARPROC)unsupported_ovr_entry;
        }
        /* Same for our Platform SDK stub (it carries our build marker).
         * Platform functions return handles, strings, counts and flags, so
         * its stand-in answers 0: NULL / "" -> null / false. Unity's
         * Platform wrapper resolves these lazily (and retries with an "A"
         * suffix), and a failed lookup throws inside its message loop. */
        if (mod != g_self && !strncmp(name, "ovr_", 4) &&
            real_GetProcAddress(mod, "ovrstub_build_marker")) {
            TRACE("GetProcAddress(%s) -> not in the Platform stub, using empty stand-in",
                  name);
            return (FARPROC)empty_platform_entry;
        }
        TRACE("GetProcAddress(%s) -> MISSING", name);
    }
    if (!p || ((ULONG_PTR)name >> 16) == 0 || !is_system_module(mod))
        return p;
    for (size_t i = 0, n = NHOOKS; i < n; i++) {
        struct hook *h = &g_hooks[i];
        if (strcmp(h->name, name) != 0)
            continue;
        if (!*h->real)
            *h->real = (void *)p;
        return (FARPROC)h->hook;
    }
    return p;
}

/* Record export addresses from any source DLL loaded since the last pass. */
static void resolve_hooks(void)
{
    for (size_t d = 0; d < sizeof(g_source_dlls) / sizeof(g_source_dlls[0]); d++) {
        HMODULE m = GetModuleHandleW(g_source_dlls[d]);
        if (!m)
            continue;
        for (size_t i = 0, n = NHOOKS; i < n; i++) {
            struct hook *h = &g_hooks[i];
            void *addr = (void *)GetProcAddress(m, h->name);
            if (!addr)
                continue;
            if (!*h->real)
                *h->real = addr;
            for (int a = 0; a < MAX_ADDRS; a++) {
                if (h->addrs[a] == addr)
                    break;
                if (!h->addrs[a]) {
                    h->addrs[a] = addr;
                    break;
                }
            }
        }
    }
}

/* System DLLs are never patched: their internal calls (e.g. kernel32's
 * LoadLibraryW calling kernelbase through its own IAT) would loop back into
 * our hooks, and only the game's own imports matter here. */
static int is_system_module(HMODULE mod)
{
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(mod, path, MAX_PATH))
        return 1;
    return g_system_dir_len && _wcsnicmp(path, g_system_dir, g_system_dir_len) == 0;
}

static void patch_module(HMODULE mod)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (mod == g_self || dos->e_magic != IMAGE_DOS_SIGNATURE || is_system_module(mod))
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
            for (size_t i = 0, n = NHOOKS; i < n; i++) {
                struct hook *h = &g_hooks[i];
                int match = 0;
                for (int a = 0; a < MAX_ADDRS && h->addrs[a]; a++)
                    match |= cur == h->addrs[a];
                if (!match)
                    continue;
                DWORD old;
                if (VirtualProtect(&thunk->u1.Function, sizeof(void *),
                                   PAGE_READWRITE, &old)) {
                    thunk->u1.Function = (ULONG_PTR)h->hook;
                    VirtualProtect(&thunk->u1.Function, sizeof(void *), old, &old);
                }
                break;
            }
        }
    }
}

static void patch_all_modules(void)
{
    HMODULE mods[1024];
    DWORD needed;
    resolve_hooks();
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
    g_self = self;
    GetModuleFileNameW(self, g_self_path, MAX_PATH);
    wcscpy(g_platform_path, g_self_path);
    WCHAR *slash = (WCHAR *)base_name(g_platform_path);
    *slash = 0;
    if (wcslen(g_platform_path) + 32 < MAX_PATH)
        wcscat(g_platform_path, L"LibOVRPlatform64_1.dll");
    if (GetFileAttributesW(g_platform_path) == INVALID_FILE_ATTRIBUTES)
        g_platform_path[0] = 0;

    /* GetSystemDirectory gives ...\system32; match its parent (C:\windows\)
     * so syswow64 and winsxs DLLs count as system too. */
    UINT n = GetSystemDirectoryW(g_system_dir, MAX_PATH);
    if (n && n < MAX_PATH) {
        WCHAR *last = (WCHAR *)base_name(g_system_dir);
        *last = 0;
        g_system_dir_len = wcslen(g_system_dir);
    }

    patch_all_modules();
    LOG("installed LoadLibrary redirect for LibOVRRT64_1.dll%s",
        g_platform_path[0] ? " and LibOVRPlatform64_1.dll" : "");
}
