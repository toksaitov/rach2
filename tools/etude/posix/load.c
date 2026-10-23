/*
 * load.c: the native load fixture that runs beside a repeated case. It spins
 * processors, holds written memory, or writes and synchronizes a file, until
 * it is killed.
 */
#ifdef _WIN32
#include "../win/files.h"
#include <windows.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "etude/repeat.h"

/* The most processes a processor load starts. */
#define LOAD_CPU_MAX 1024

/* The bytes a disk load writes with one write(2). */
#define LOAD_CHUNK 65536

/* The bytes between two writes that keep an allocation resident. */
#define LOAD_PAGE 4096

static const char usage_text[] = "usage: etude stress cpu|memory|disk AMOUNT\n";

/** Say that the load is in effect, with one write(2). */
static int ready(void)
{
    const char *text = ETUDE_LOAD_READY;

    return write(1, text, strlen(text)) == (ssize_t)strlen(text) ? 0 : -1;
}

/** Spin until killed. */
static void spin(void)
{
    volatile unsigned long n = 0;

    for (;;)
        n = n + 1;
}

#ifdef _WIN32
static DWORD WINAPI thread_spin(void *argument)
{
    (void)argument;
    spin();
    return 0;
}
#endif
/** Spin on `count` processors within the executor's owned process tree. */
static int load_cpu(uint64_t count)
{
    uint64_t i;
#ifdef _WIN32
    for (i = 1; i < count; i++) {
        HANDLE thread = CreateThread(NULL, 65536, thread_spin, NULL, 0, NULL);
        if (!thread)
            return 1;
        CloseHandle(thread);
    }
#else
    pid_t pids[LOAD_CPU_MAX];
    for (i = 1; i < count; i++) {
        pids[i] = fork();
        if (pids[i] == 0)
            spin();
        if (pids[i] < 0) {
            fprintf(stderr, "etude stress: fork: %s\n", strerror(errno));
            while (--i > 0)
                kill(pids[i], SIGKILL);
            return 1;
        }
    }
#endif
    if (ready() != 0)
        return 1;
    spin();
    return 1;
}

/** Hold `bytes` of memory, each page written, until killed. */
static int load_memory(uint64_t bytes)
{
    volatile unsigned char *m = bytes <= SIZE_MAX ? malloc((size_t)bytes) : NULL;
    uint64_t at;

    if (m == NULL) {
        fprintf(stderr, "etude stress: cannot allocate %llu bytes\n", (unsigned long long)bytes);
        return 1;
    }
    for (at = 0; at < bytes; at += LOAD_PAGE)
        m[at] = (unsigned char)at;
    if (ready() != 0)
        return 1;
    for (;;) {
#ifdef _WIN32
        Sleep(INFINITE);
#else
        pause();
#endif
    }
}

/**
 * Write `bytes` to a file of this process's, unlinked once it is open, and
 * synchronize it, again and again until killed.
 */
#ifdef _WIN32
static int load_disk(uint64_t bytes)
{
    static unsigned char chunk[LOAD_CHUNK];
    char name[64];
    snprintf(name, sizeof(name), "etude-stress.%lu", (unsigned long)GetCurrentProcessId());
    HANDLE file =
        CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_DELETE, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 1;
    memset(chunk, 0xa5, sizeof(chunk));
    int announced = 0;
    for (;;) {
        LARGE_INTEGER at;
        at.QuadPart = 0;
        if (!SetFilePointerEx(file, at, NULL, FILE_BEGIN))
            break;
        for (uint64_t written = 0; written < bytes;) {
            DWORD n = bytes - written < LOAD_CHUNK ? (DWORD)(bytes - written) : LOAD_CHUNK;
            DWORD done;
            if (!WriteFile(file, chunk, n, &done, NULL) || done != n) {
                CloseHandle(file);
                return 1;
            }
            written += done;
        }
        if (!FlushFileBuffers(file))
            break;
        if (!announced) {
            if (ready())
                break;
            announced = 1;
        }
    }
    CloseHandle(file);
    return 1;
}
#else
static int load_disk(uint64_t bytes)
{
    static unsigned char chunk[LOAD_CHUNK];
    char name[64];
    uint64_t at;
    size_t n;
    int fd;

    snprintf(name, sizeof(name), "etude-stress.%ld", (long)getpid());
    fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || unlink(name) != 0) {
        fprintf(stderr, "etude stress: %s: %s\n", name, strerror(errno));
        return 1;
    }
    memset(chunk, 0xa5, sizeof(chunk));
    int announced = 0;
    for (;;) {
        for (at = 0; at < bytes; at += n) {
            n = bytes - at < LOAD_CHUNK ? (size_t)(bytes - at) : LOAD_CHUNK;
            if (pwrite(fd, chunk, n, (off_t)at) != (ssize_t)n) {
                fprintf(stderr, "etude stress: write: %s\n", strerror(errno));
                return 1;
            }
        }
        if (fsync(fd) != 0) {
            fprintf(stderr, "etude stress: fsync: %s\n", strerror(errno));
            return 1;
        }
        if (!announced) {
            if (ready())
                return 1;
            announced = 1;
        }
    }
}

#endif

int etude_load_main(int argc, char **argv)
{
    unsigned long long amount;
    char *end;

    if (argc != 3 || argv[2][0] < '0' || argv[2][0] > '9') {
        fputs(usage_text, stderr);
        return 2;
    }
    errno = 0;
    amount = strtoull(argv[2], &end, 10);
    if (errno != 0 || *end != '\0' || amount == 0) {
        fputs(usage_text, stderr);
        return 2;
    }
    if (strcmp(argv[1], "cpu") == 0 && amount <= LOAD_CPU_MAX)
        return load_cpu(amount);
    if (strcmp(argv[1], "memory") == 0)
        return load_memory(amount);
    if (strcmp(argv[1], "disk") == 0)
        return load_disk(amount);
    fputs(usage_text, stderr);
    return 2;
}
