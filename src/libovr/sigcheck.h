/*
 * Hooks WinVerifyTrust so Meta's loaders accept our unsigned stub DLLs.
 * See sigcheck.c. Call after the hook table is ready and before
 * redirect_install() patches import tables.
 */
#ifndef OVRSTUB_SIGCHECK_H
#define OVRSTUB_SIGCHECK_H

#include <windows.h>

void sigcheck_install(void);

/* Whether the file at `path` is one of our stub DLLs (carries our marker). */
int file_is_our_stub(LPCWSTR path);

#endif
