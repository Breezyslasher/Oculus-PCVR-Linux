/*
 * Redirects the game's attempts to load the Oculus runtime to this DLL.
 *
 * Games statically link Meta's LibOVR loader, which looks up the installed
 * runtime and calls LoadLibrary on LibOVRRT64_1.dll. Once this DLL has been
 * injected (see src/injector), we patch the import tables of every loaded
 * module so their LoadLibrary* calls go through us, and hand back our own
 * module whenever the runtime DLL is requested. Revive does the same with
 * Detours; an IAT patch avoids needing a disassembler or a third-party hook
 * library. GetProcAddress is hooked too, so functions the game resolves at
 * run time (delay-load, LoadLibrary + GetProcAddress) are covered as well.
 */
#ifndef OVRSTUB_IAT_HOOK_H
#define OVRSTUB_IAT_HOOK_H

#include <windows.h>

void redirect_install(HMODULE self);

/* Add a hook for a system DLL export. `real` receives the original
 * implementation once it is found. Call before redirect_install(). */
void hook_register(const char *name, void *hook, void **real);

#endif
