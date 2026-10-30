/*
 * Let Meta's loaders accept our own stub DLLs.
 *
 * Before loading LibOVRRT64_1.dll or LibOVRPlatform64_1.dll, the LibOVR
 * loaders built into each game call WinVerifyTrust on the file and refuse to
 * load it unless the call succeeds. Our stubs are unsigned, so on a real
 * Windows check they return TRUST_E_NOSIGNATURE (seen in Spider-Man:
 * Homecoming VR under Proton) and the game never loads them.
 *
 * This hook reports success from WinVerifyTrust for those two file names
 * only; every other file is passed straight to the real implementation and
 * verified normally. It is scoped to the stub files we ship ourselves, so it
 * does not weaken signature checking for anything else in the process. Set
 * OVRSTUB_FAKE_SIGNATURE=0 to turn it off.
 *
 * Some loaders additionally read the signer name and compare it with
 * "Oculus VR, LLC". If a game needs that, the trace below shows which
 * accessor it calls, and that can be handled per game.
 */
#include <stdlib.h>
#include <windows.h>
#include <wintrust.h>

#include "../common/log.h"
#include "iat_hook.h"
#include "sigcheck.h"

#define LOG(...) stub_log("libovr-sigcheck", __VA_ARGS__)

static __typeof__(&WinVerifyTrust) real_WinVerifyTrust;
static int g_enabled = 1;

/* True only for a file that is actually one of our stubs (carries the build
 * marker). A genuine Meta file at the same name is NOT a stub: it must go
 * through real verification so the loader can read its true Meta signer. */
static int is_our_stub_file(LPCWSTR path)
{
    static const char marker[] = "OVRSTUB-BUILD-MARKER-v1";
    const DWORD mlen = sizeof(marker) - 1;
    const DWORD keep = mlen - 1;  /* bytes carried between reads for overlap */
    HANDLE h;
    char buf[65536];
    DWORD carry = 0, total = 0;
    int found = 0;

    if (!path)
        return 0;
    LPCWSTR base = path;
    for (LPCWSTR p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    if (_wcsnicmp(base, L"LibOVRRT64_1", 12) != 0 &&
        _wcsnicmp(base, L"LibOVRPlatform64_1", 18) != 0)
        return 0;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    /* Scan for the marker. Each read keeps the last keep bytes of the previous
     * chunk in front of it, so a match spanning a read boundary is still
     * found. Our stubs are well under 1 MB. */
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf + carry, sizeof(buf) - carry, &got, NULL) || !got)
            break;
        DWORD avail = carry + got;
        for (DWORD i = 0; i + mlen <= avail; i++)
            if (memcmp(buf + i, marker, mlen) == 0) {
                found = 1;
                break;
            }
        total += got;
        if (found || total > (4u << 20))
            break;
        carry = avail >= keep ? keep : avail;
        memmove(buf, buf + avail - carry, carry);
    }
    CloseHandle(h);
    return found;
}

static LPCWSTR verified_file(WINTRUST_DATA *data)
{
    if (data && data->dwUnionChoice == WTD_CHOICE_FILE && data->pFile)
        return data->pFile->pcwszFilePath;
    return NULL;
}

static LONG WINAPI hook_WinVerifyTrust(HWND hwnd, GUID *action, LPVOID pdata)
{
    WINTRUST_DATA *data = pdata;
    LPCWSTR file = verified_file(data);

    if (g_enabled && is_our_stub_file(file)) {
        /* Only our own unsigned stub is reported trusted. A real Meta file at
         * the same name falls through to real verification below, so the
         * loader can read its genuine signer. */
        LOG("reporting our stub %ls as trusted", file);
        /* WTD_STATEACTION_VERIFY expects provider state in hWVTStateData for
         * a later CLOSE call. We take neither action, so leave it NULL and
         * answer CLOSE (and every other state action) with success too. */
        return ERROR_SUCCESS;
    }
    return real_WinVerifyTrust(hwnd, action, pdata);
}

void sigcheck_install(void)
{
    const char *v = getenv("OVRSTUB_FAKE_SIGNATURE");
    if (v && v[0] == '0') {
        g_enabled = 0;
        LOG("disabled (OVRSTUB_FAKE_SIGNATURE=0); stub DLLs must be signed");
        return;
    }
    hook_register("WinVerifyTrust", (void *)hook_WinVerifyTrust,
                  (void **)&real_WinVerifyTrust);
    LOG("will report LibOVRRT64_1.dll and LibOVRPlatform64_1.dll as trusted");
}
