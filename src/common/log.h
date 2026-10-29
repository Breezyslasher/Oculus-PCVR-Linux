/*
 * Minimal logging shared by the stub DLLs.
 *
 * Output goes to stderr (visible in the Proton/Wine terminal log) and, when the
 * OVRSTUB_LOG environment variable names a file, is appended to that file too.
 */
#ifndef OVRSTUB_LOG_H
#define OVRSTUB_LOG_H

void stub_log(const char *component, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
