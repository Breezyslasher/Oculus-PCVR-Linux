/*
 * Hooks WinVerifyTrust so Meta's loaders accept our unsigned stub DLLs.
 * See sigcheck.c. Call after the hook table is ready and before
 * redirect_install() patches import tables.
 */
#ifndef OVRSTUB_SIGCHECK_H
#define OVRSTUB_SIGCHECK_H

void sigcheck_install(void);

#endif
