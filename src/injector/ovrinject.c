/*
 * ovrinject.exe: start a game with shim DLLs loaded before its own code runs.
 *
 *   ovrinject.exe [--wait] <dll> [<dll>...] -- <game.exe> [game args...]
 *
 * The game is created suspended, each DLL is loaded into it with a remote
 * LoadLibraryW thread, then the main thread is resumed. This is how the
 * LibOVRRT shim gets in place before the game's LibOVR loader looks for the
 * Oculus runtime (Revive's injector does the same via Detours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>
#include <shellapi.h>

static void die(const char *what)
{
    fprintf(stderr, "ovrinject: %s failed (error %lu)\n", what, GetLastError());
    exit(1);
}

/* Append one argument to a command line using CommandLineToArgvW quoting. */
static void append_arg(wchar_t *out, size_t cap, const wchar_t *arg)
{
    size_t n = wcslen(out);
    int quote = !*arg || wcspbrk(arg, L" \t\"") != NULL;

    if (n && n + 1 < cap)
        out[n++] = L' ';
    if (quote && n + 1 < cap)
        out[n++] = L'"';
    for (const wchar_t *p = arg; *p && n + 3 < cap; p++) {
        size_t backslashes = 0;
        while (*p == L'\\') {
            backslashes++;
            p++;
        }
        if (!*p) {
            /* Double trailing backslashes so they don't escape our quote. */
            for (size_t i = 0; i < backslashes * (quote ? 2 : 1) && n + 2 < cap; i++)
                out[n++] = L'\\';
            break;
        }
        if (*p == L'"') {
            for (size_t i = 0; i < backslashes * 2 + 1 && n + 2 < cap; i++)
                out[n++] = L'\\';
        } else {
            for (size_t i = 0; i < backslashes && n + 2 < cap; i++)
                out[n++] = L'\\';
        }
        out[n++] = *p;
    }
    if (quote && n + 1 < cap)
        out[n++] = L'"';
    out[n] = 0;
}

static void inject(HANDLE process, const wchar_t *dll)
{
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(dll, MAX_PATH, full, NULL))
        die("GetFullPathNameW");
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "ovrinject: %ls not found\n", full);
        exit(1);
    }

    SIZE_T size = (wcslen(full) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote)
        die("VirtualAllocEx");
    if (!WriteProcessMemory(process, remote, full, size, NULL))
        die("WriteProcessMemory");

    LPTHREAD_START_ROUTINE load = (LPTHREAD_START_ROUTINE)(void *)GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE thread = CreateRemoteThread(process, NULL, 0, load, remote, 0, NULL);
    if (!thread)
        die("CreateRemoteThread");
    WaitForSingleObject(thread, INFINITE);

    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (!code) {
        fprintf(stderr, "ovrinject: LoadLibraryW(%ls) failed in the target\n", full);
        exit(1);
    }
    fprintf(stderr, "ovrinject: loaded %ls\n", full);
}

int main(void)
{
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int wait = 0, sep = -1, first_dll = 1;

    if (argv && argc > 1 && wcscmp(argv[1], L"--wait") == 0) {
        wait = 1;
        first_dll = 2;
    }
    for (int i = first_dll; argv && i < argc; i++) {
        if (wcscmp(argv[i], L"--") == 0) {
            sep = i;
            break;
        }
    }
    if (sep < 0 || sep + 1 >= argc) {
        fprintf(stderr,
                "usage: ovrinject.exe [--wait] <dll>... -- <game.exe> [args...]\n");
        return 2;
    }

    static wchar_t cmdline[32768];
    for (int i = sep + 1; i < argc; i++)
        append_arg(cmdline, sizeof(cmdline) / sizeof(cmdline[0]), argv[i]);

    /* Run the game from its own directory, as launchers normally do. */
    wchar_t workdir[MAX_PATH], *file_part = NULL;
    if (!GetFullPathNameW(argv[sep + 1], MAX_PATH, workdir, &file_part))
        die("GetFullPathNameW");
    if (file_part)
        *file_part = 0;

    STARTUPINFOW si = {.cb = sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL,
                        workdir, &si, &pi)) {
        fprintf(stderr, "ovrinject: command line: %ls\n", cmdline);
        die("CreateProcessW");
    }

    for (int i = first_dll; i < sep; i++)
        inject(pi.hProcess, argv[i]);

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    DWORD exit_code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }
    CloseHandle(pi.hProcess);
    LocalFree(argv);
    return (int)exit_code;
}
