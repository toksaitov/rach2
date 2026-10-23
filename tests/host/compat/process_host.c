/* Enumerate command arguments without invoking ps or another program. */
#ifdef _WIN32
#include "win/files.h"
#include <tlhelp32.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "process_host.h"

#if defined(_WIN32)
/* Normalize the native command line to the argument spelling used by the
 * POSIX ownership queries. Windows quotes every generated argument. */
static char *plain_arguments(const wchar_t *line)
{
    typedef wchar_t **(WINAPI * split_fn)(const wchar_t *, int *);
    HMODULE module = LoadLibraryW(L"shell32.dll");
    split_fn split;
    wchar_t **words;
    char *word, *out = NULL, *more;
    size_t used = 0, length;
    int count, i;
    if (module == NULL)
        return NULL;
    split = (split_fn)(void *)GetProcAddress(module, "CommandLineToArgvW");
    words = split != NULL ? split(line, &count) : NULL;
    if (words != NULL) {
        for (i = 0; i < count; i++) {
            word = etude_win_utf8(words[i]);
            if (word == NULL) {
                free(out);
                out = NULL;
                break;
            }
            length = strlen(word);
            more = realloc(out, used + length + 2);
            if (more == NULL) {
                free(word);
                free(out);
                out = NULL;
                break;
            }
            out = more;
            if (used)
                out[used++] = ' ';
            memcpy(out + used, word, length + 1);
            used += length;
            free(word);
        }
        LocalFree(words);
    }
    FreeLibrary(module);
    return out;
}

/* ProcessCommandLineInformation reads the command line without a helper or
 * remote memory-layout assumptions. Processes may end during enumeration. */
static char *arguments(long pid)
{
    typedef LONG(WINAPI * query_fn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    struct native_string {
        USHORT length, capacity;
        PWSTR text;
    };
    query_fn query = (query_fn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                                      "NtQueryInformationProcess");
    struct native_string *value;
    HANDLE process;
    ULONG size = 0;
    char *out = NULL;
    if (query == NULL)
        return NULL;
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (process == NULL)
        return NULL;
    query(process, 60, NULL, 0, &size);
    value = size > sizeof(*value) ? calloc(1, size + sizeof(wchar_t)) : NULL;
    if (value != NULL && query(process, 60, value, size, &size) >= 0 && value->text != NULL &&
        value->length % sizeof(wchar_t) == 0) {
        value->text[value->length / sizeof(wchar_t)] = L'\0';
        out = plain_arguments(value->text);
    }
    free(value);
    CloseHandle(process);
    return out;
}

int host_process_arguments(int (*visit)(long, const char *, void *), void *ctx)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry;
    char *args;
    int result = 0;
    if (snapshot == INVALID_HANDLE_VALUE)
        return -1;
    memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return -1;
    }
    do {
        args = arguments((long)entry.th32ProcessID);
        if (args != NULL) {
            result = visit((long)entry.th32ProcessID, args, ctx);
            free(args);
        }
    } while (result == 0 && Process32NextW(snapshot, &entry));
    if (result == 0 && GetLastError() != ERROR_NO_MORE_FILES)
        result = -1;
    CloseHandle(snapshot);
    return result;
}
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>

/* KERN_PROCARGS2 has argc, the executable path, padding, then argv and env. */
static char *arguments(long pid)
{
    int mib[] = {CTL_KERN, KERN_PROCARGS2, (int)pid}, argc, limit;
    size_t n = sizeof limit, used = 0;
    char *bytes, *out, *p, *end, *zero;

    if (sysctlbyname("kern.argmax", &limit, &n, NULL, 0) != 0 || limit <= 0)
        return NULL;
    bytes = malloc((size_t)limit);
    out = malloc((size_t)limit);
    if (bytes == NULL || out == NULL) {
        free(bytes);
        free(out);
        return NULL;
    }
    n = (size_t)limit;
    if (sysctl(mib, 3, bytes, &n, NULL, 0) != 0 || n < sizeof argc)
        goto failed;
    memcpy(&argc, bytes, sizeof argc);
    p = bytes + sizeof argc;
    end = bytes + n;
    zero = memchr(p, '\0', (size_t)(end - p));
    if (argc < 0 || zero == NULL)
        goto failed;
    p = zero + 1;
    while (p < end && *p == '\0')
        p++;
    while (argc-- > 0) {
        zero = memchr(p, '\0', (size_t)(end - p));
        if (zero == NULL || used + (size_t)(zero - p) + 2 > (size_t)limit)
            goto failed;
        if (used)
            out[used++] = ' ';
        memcpy(out + used, p, (size_t)(zero - p));
        used += (size_t)(zero - p);
        p = zero + 1;
    }
    out[used] = '\0';
    free(bytes);
    return out;
failed:
    free(bytes);
    free(out);
    return NULL;
}

int host_process_arguments(int (*visit)(long, const char *, void *), void *ctx)
{
    int capacity = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0), n, i, result = 0;
    pid_t *pids;
    char *args;

    if (capacity <= 0)
        return -1;
    /* A second query observes children created between the two calls too. */
    capacity += 256 * (int)sizeof *pids;
    pids = malloc((size_t)capacity);
    if (pids == NULL)
        return -1;
    n = proc_listpids(PROC_ALL_PIDS, 0, pids, capacity);
    if (n <= 0 || n >= capacity) {
        free(pids);
        return -1;
    }
    for (i = 0; i < n / (int)sizeof *pids; i++) {
        if (pids[i] <= 0)
            continue;
        args = arguments((long)pids[i]);
        if (args == NULL)
            continue; /* Processes may end or belong to another account. */
        result = visit((long)pids[i], args, ctx);
        free(args);
        if (result != 0)
            break;
    }
    free(pids);
    return result;
}
#elif defined(__linux__) || defined(__rach2__)
#include <dirent.h>

static char *arguments(long pid)
{
    char path[64], *bytes, *more;
    FILE *f;
    size_t used = 0, capacity = 4096, n, i;

    snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    bytes = malloc(capacity + 1);
    if (bytes == NULL) {
        fclose(f);
        return NULL;
    }
    while ((n = fread(bytes + used, 1, capacity - used, f)) != 0) {
        used += n;
        if (used == capacity) {
            capacity *= 2;
            more = realloc(bytes, capacity + 1);
            if (more == NULL) {
                free(bytes);
                fclose(f);
                return NULL;
            }
            bytes = more;
        }
    }
    if (ferror(f)) {
        free(bytes);
        fclose(f);
        return NULL;
    }
    fclose(f);
    while (used && bytes[used - 1] == '\0')
        used--;
    for (i = 0; i < used; i++)
        if (bytes[i] == '\0')
            bytes[i] = ' ';
    bytes[used] = '\0';
    return bytes;
}

int host_process_arguments(int (*visit)(long, const char *, void *), void *ctx)
{
    DIR *directory = opendir("/proc");
    struct dirent *entry;
    char *end, *args;
    long pid;
    int result = 0;

    if (directory == NULL)
        return -1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        pid = strtol(entry->d_name, &end, 10);
        if (pid > 0 && *end == '\0') {
            args = arguments(pid);
            if (args != NULL) {
                result = visit(pid, args, ctx);
                free(args);
                if (result != 0)
                    break;
            }
        }
        errno = 0;
    }
    if (errno != 0)
        result = -1;
    closedir(directory);
    return result;
}
#else
#error Native process enumeration is required for this host.
#endif
