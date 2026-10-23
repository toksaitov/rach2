/* The native equivalent of the build-time OVMF pair search. */
#ifndef RACH2_HOST_OVMF_H
#define RACH2_HOST_OVMF_H
#ifdef _WIN32
#include "win/files.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int host_ovmf_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, R_OK) == 0;
}

#ifdef _WIN32
/** Find the firmware pair beside the native QEMU executable. */
static inline const char *host_ovmf_native(char *out, size_t size)
{
    static const char *const directories[] = {"share", "../share/qemu"};
    char program[4096], vars[4096], *slash, *backslash;
    size_t i;
    if (etude_win_program("qemu-system-x86_64", program, sizeof(program)) != 0)
        return NULL;
    slash = strrchr(program, '/');
    backslash = strrchr(program, '\\');
    if (slash == NULL || (backslash != NULL && backslash > slash))
        slash = backslash;
    if (slash == NULL)
        return NULL;
    *slash = '\0';
    for (i = 0; i < sizeof(directories) / sizeof(directories[0]); i++)
        if (snprintf(out, size, "%s/%s/edk2-x86_64-code.fd", program, directories[i]) < (int)size &&
            snprintf(vars, sizeof(vars), "%s/%s/edk2-i386-vars.fd", program, directories[i]) <
                (int)sizeof(vars) &&
            host_ovmf_file(out) && host_ovmf_file(vars))
            return out;
    return NULL;
}
#endif

static inline const char *host_ovmf_code(char *out, size_t size)
{
#ifndef _WIN32
    static const char *const code[] = {"/usr/share/OVMF/OVMF_CODE_4M.fd",
                                       "/usr/share/OVMF/OVMF_CODE.fd"};
    static const char *const vars[] = {"/usr/share/OVMF/OVMF_VARS_4M.fd",
                                       "/usr/share/OVMF/OVMF_VARS.fd"};
    const char *path = getenv("PATH"), *end;
    char store[4096];
    size_t i, length;
#endif
    const char *override = getenv("RACH2_OVMF_CODE");

    if (override != NULL && *override != '\0') {
        if (snprintf(out, size, "%s", override) < (int)size && host_ovmf_file(out))
            return out;
        return NULL;
    }
#ifdef _WIN32
    return host_ovmf_native(out, size);
#else
    while (path != NULL && *path) {
        end = strchr(path, ':');
        length = end != NULL ? (size_t)(end - path) : strlen(path);
        if (length > 0 &&
            snprintf(out, size, "%.*s/../share/qemu/edk2-x86_64-code.fd", (int)length, path) <
                (int)size &&
            host_ovmf_file(out)) {
            if (snprintf(store, sizeof store, "%.*s/../share/qemu/edk2-i386-vars.fd", (int)length,
                         path) < (int)sizeof store &&
                host_ovmf_file(store))
                return out;
            /* The shell search chooses the first QEMU data directory. */
            break;
        }
        path = end != NULL ? end + 1 : NULL;
    }
    for (i = 0; i < sizeof code / sizeof code[0]; i++) {
        if (host_ovmf_file(code[i]) && host_ovmf_file(vars[i]) &&
            snprintf(out, size, "%s", code[i]) < (int)size)
            return out;
    }
    return NULL;
#endif
}
#endif
