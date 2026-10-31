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
#include <wchar.h>
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>

#include "../common/log.h"
#include "digicert_root_der.h"
#include "iat_hook.h"
#include "sigcheck.h"

#define LOG(...) stub_log("libovr-sigcheck", __VA_ARGS__)

static __typeof__(&WinVerifyTrust) real_WinVerifyTrust;
/* Diagnostics: the two accessors the game uses to inspect the signature, so
 * we can see the provider-data shape Wine built for the genuine Meta file. */
typedef CRYPT_PROVIDER_DATA *(WINAPI *WTHelperProvDataFromStateData_t)(HANDLE);
typedef CRYPT_PROVIDER_SGNR *(WINAPI *WTHelperGetProvSignerFromChain_t)(
    CRYPT_PROVIDER_DATA *, DWORD, BOOL, DWORD);
static WTHelperProvDataFromStateData_t real_ProvDataFromStateData;
static WTHelperGetProvSignerFromChain_t real_GetProvSignerFromChain;
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

static int is_libovr_named(LPCWSTR path)
{
    if (!path)
        return 0;
    LPCWSTR base = path;
    for (LPCWSTR p = path; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    return _wcsnicmp(base, L"LibOVRRT64_1", 12) == 0 ||
           _wcsnicmp(base, L"LibOVRPlatform64_1", 18) == 0;
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
    LONG r = real_WinVerifyTrust(hwnd, action, pdata);
    /* Log the real result for a genuine LibOVR file so we can see whether
     * Wine accepted Meta's certificate (0 = trusted). */
    if (is_libovr_named(file) && data && data->dwStateAction == WTD_STATEACTION_VERIFY)
        LOG("real WinVerifyTrust(%ls) -> 0x%08lx%s", file, (unsigned long)r,
            r == 0 ? " (trusted)" : "");
    return r;
}

static CRYPT_PROVIDER_DATA *WINAPI hook_ProvDataFromStateData(HANDLE h)
{
    CRYPT_PROVIDER_DATA *cpd = real_ProvDataFromStateData(h);
    if (cpd)
        LOG("WTHelperProvDataFromStateData -> csSigners=%lu",
            (unsigned long)cpd->csSigners);
    else
        LOG("WTHelperProvDataFromStateData -> NULL");
    return cpd;
}

/* Is the signer one of the names Meta signs its runtime with? */
static int signed_by_meta(CRYPT_PROVIDER_SGNR *cps)
{
    static const wchar_t *const names[] = {
        L"Oculus VR, LLC", L"Facebook Technologies, LLC",
        L"Meta Platforms Technologies, LLC",
    };
    if (!cps->csCertChain || !cps->pasCertChain || !cps->pasCertChain[0].pCert)
        return 0;
    wchar_t subject[128];
    if (!CertGetNameStringW(cps->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                            0, NULL, subject, 128))
        return 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!wcscmp(subject, names[i]))
            return 1;
    return 0;
}

/* Is this signer's chain a genuine Meta-signed LibOVR file's, validated by
 * CryptoAPI with no errors? Decided from the provider data itself rather
 * than from a WinVerifyTrust call we saw: some loaders (Unity's OVRPlugin)
 * reach WinVerifyTrust in a way our hook doesn't intercept. The file is
 * recognised by name when the provider data carries it, else by its signer. */
static int chain_is_trusted_libovr(CRYPT_PROVIDER_DATA *cpd, CRYPT_PROVIDER_SGNR *cps)
{
    if (!cpd || !cps)
        return 0;
    LPCWSTR file = verified_file(cpd->pWintrustData);
    if (!is_libovr_named(file) && !signed_by_meta(cps)) {
        LOG("not fixing: file %ls is not a Meta-signed LibOVR file",
            file ? file : L"(unknown)");
        return 0;
    }
    if (!cps->pChainContext) {
        LOG("not fixing: no certificate chain for %ls", file ? file : L"(unknown)");
        return 0;
    }
    /* Revocation that couldn't be checked offline doesn't make a chain
     * untrusted (WinVerifyTrust accepts it too); anything else does. */
    DWORD status = cps->pChainContext->TrustStatus.dwErrorStatus &
        ~(DWORD)(CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION);
    if (status != CERT_TRUST_NO_ERROR)
        LOG("not fixing: LibOVR signer chain has errors (status 0x%lx)",
            (unsigned long)cps->pChainContext->TrustStatus.dwErrorStatus);
    return status == CERT_TRUST_NO_ERROR;
}

static CRYPT_PROVIDER_SGNR *WINAPI hook_GetProvSignerFromChain(CRYPT_PROVIDER_DATA *cpd,
                                                               DWORD idxSigner,
                                                               BOOL counterSigner,
                                                               DWORD idxCounterSigner)
{
    CRYPT_PROVIDER_SGNR *cps =
        real_GetProvSignerFromChain(cpd, idxSigner, counterSigner, idxCounterSigner);
    if (!cps) {
        LOG("WTHelperGetProvSignerFromChain(signer %lu) -> NULL",
            (unsigned long)idxSigner);
        return cps;
    }
    LOG("WTHelperGetProvSignerFromChain(signer %lu) -> csCertChain=%lu "
        "(game wants exactly 3)", (unsigned long)idxSigner,
        (unsigned long)cps->csCertChain);
    /* The game's ValidateCertificateContents walks pasCertChain and rejects
     * any test cert or untrusted self-signed root. Wine fills in only some
     * of each entry's fields (it never sets fTestCert, for one), and the
     * rest can hold leftover heap data: the same genuine runtime that passes
     * one check then reads as a "test cert" on the next (seen as
     * fTestCert=4096 dwError=0x10 in Lost, Google Earth VR and Minecraft).
     * For a LibOVR file whose chain CryptoAPI built with no errors, set those
     * fields to what Windows reports for that chain: not a test or
     * commercial-test cert, not revoked, no per-cert error, and self-signed
     * only where the subject is its own issuer. The certificates themselves
     * and the trust decision are Wine's, unchanged. */
    int fix = g_enabled && chain_is_trusted_libovr(cpd, cps);
    for (DWORD i = 0; i < cps->csCertChain && cps->pasCertChain; i++) {
        CRYPT_PROVIDER_CERT *pc = &cps->pasCertChain[i];
        if (fix && pc->pCert && pc->pCert->pCertInfo) {
            if (pc->fTestCert || pc->dwError || pc->fCommercial || pc->dwRevokedReason)
                LOG("  cert[%lu]: clearing uninitialized fields Wine left set "
                    "(fTestCert=%d dwError=0x%lx)", (unsigned long)i,
                    (int)pc->fTestCert, (unsigned long)pc->dwError);
            pc->fCommercial = FALSE;
            pc->fTestCert = FALSE;
            pc->dwRevokedReason = 0;
            pc->dwError = 0;
            pc->fSelfSigned = CertCompareCertificateName(X509_ASN_ENCODING,
                &pc->pCert->pCertInfo->Subject, &pc->pCert->pCertInfo->Issuer);
        }
        LOG("  cert[%lu]: fSelfSigned=%d fTrustedRoot=%d fTestCert=%d "
            "dwError=0x%lx dwConfidence=0x%lx", (unsigned long)i,
            (int)pc->fSelfSigned, (int)pc->fTrustedRoot, (int)pc->fTestCert,
            (unsigned long)pc->dwError, (unsigned long)pc->dwConfidence);
    }
    return cps;
}

/* Ensure the DigiCert Assured ID Root CA is in the prefix's trusted-root
 * store. Proton prefixes often lack it, so Wine builds the genuine Meta
 * runtime's chain with an untrusted root (CERT_E_UNTRUSTEDROOT) and the
 * game's own check refuses to load the runtime. Adding this standard public
 * root -- the same one every real Windows install trusts -- lets the real
 * signature validate. Set OVRSTUB_INSTALL_ROOT_CA=0 to skip. */
static void install_trusted_root(void)
{
    const char *skip = getenv("OVRSTUB_INSTALL_ROOT_CA");
    if (skip && skip[0] == '0')
        return;
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT");
    if (!store) {
        LOG("could not open LOCAL_MACHINE Root store (error %lu); the real "
            "runtime's root CA may read as untrusted", GetLastError());
        return;
    }
    if (CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING,
            digicert_assured_id_root_ca, sizeof(digicert_assured_id_root_ca),
            CERT_STORE_ADD_REPLACE_EXISTING, NULL))
        LOG("DigiCert Assured ID Root CA present in the prefix Root store");
    else
        LOG("failed to add DigiCert root to the Root store (error %lu)",
            GetLastError());
    CertCloseStore(store, 0);
}

int file_is_our_stub(LPCWSTR path)
{
    return is_our_stub_file(path);
}

void sigcheck_install(void)
{
    install_trusted_root();
    const char *v = getenv("OVRSTUB_FAKE_SIGNATURE");
    if (v && v[0] == '0') {
        g_enabled = 0;
        LOG("disabled (OVRSTUB_FAKE_SIGNATURE=0); stub DLLs must be signed");
        return;
    }
    hook_register("WinVerifyTrust", (void *)hook_WinVerifyTrust,
                  (void **)&real_WinVerifyTrust);
    hook_register("WTHelperProvDataFromStateData", (void *)hook_ProvDataFromStateData,
                  (void **)&real_ProvDataFromStateData);
    hook_register("WTHelperGetProvSignerFromChain", (void *)hook_GetProvSignerFromChain,
                  (void **)&real_GetProvSignerFromChain);
    LOG("will report LibOVRRT64_1.dll and LibOVRPlatform64_1.dll as trusted");
}
