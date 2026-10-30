#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static CRITICAL_SECTION g_log_lock;
static INIT_ONCE g_log_once = INIT_ONCE_STATIC_INIT;
static FILE *g_log_file;

/* Logs past this size start over, so a log kept across many launches can't
 * grow without bound. */
#define LOG_MAX_BYTES (20L * 1024 * 1024)

static FILE *open_log(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL big = GetFileAttributesExW(path, GetFileExInfoStandard, &a) &&
               (a.nFileSizeHigh || a.nFileSizeLow > LOG_MAX_BYTES);
    return _wfopen(path, big ? L"w" : L"a");
}

static BOOL CALLBACK log_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_log_lock);
    /* OVRSTUB_LOG picks the file; "0" turns the file off. Without it, log to
     * ovrstub.log beside this DLL (the game's exe folder when launched
     * through Steam), so a launch never goes unrecorded just because the
     * launch options lost the variable. */
    const char *env = getenv("OVRSTUB_LOG");
    wchar_t path[MAX_PATH + 16];
    if (env && !strcmp(env, "0"))
        return TRUE;
    if (env && *env) {
        if (MultiByteToWideChar(CP_ACP, 0, env, -1, path, MAX_PATH))
            g_log_file = open_log(path);
        return TRUE;
    }
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)log_init, &self))
        return TRUE;
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return TRUE;
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash)
        return TRUE;
    wcscpy(slash + 1, L"ovrstub.log");
    g_log_file = open_log(path);
    return TRUE;
}

void stub_log(const char *component, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    InitOnceExecuteOnce(&g_log_once, log_init, NULL, NULL);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_lock);
    fprintf(stderr, "[%s] %s\n", component, msg);
    fflush(stderr);
    if (g_log_file) {
        fprintf(g_log_file, "[%s] %s\n", component, msg);
        fflush(g_log_file);
    }
    LeaveCriticalSection(&g_log_lock);
}
