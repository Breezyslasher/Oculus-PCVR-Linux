#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static CRITICAL_SECTION g_log_lock;
static INIT_ONCE g_log_once = INIT_ONCE_STATIC_INIT;
static FILE *g_log_file;

static BOOL CALLBACK log_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_log_lock);
    const char *path = getenv("OVRSTUB_LOG");
    if (path && *path)
        g_log_file = fopen(path, "a");
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
