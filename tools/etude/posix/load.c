/*
 * load.c: the load fixture that a repetition runs beside its case, on POSIX
 * systems. It spins processors, holds written memory, or writes and
 * synchronizes a file, until it is killed.
 */
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

static const char usage_text[] = "usage: etude-load cpu|memory|disk AMOUNT\n";

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
        n++;
}

/** Spin on `count` processors: this process and count - 1 children. */
static int load_cpu(uint64_t count)
{
    pid_t pids[LOAD_CPU_MAX];
    uint64_t i;

    for (i = 1; i < count; i++) {
        pids[i] = fork();
        if (pids[i] == 0)
            spin();
        if (pids[i] < 0) {
            fprintf(stderr, "etude-load: fork: %s\n", strerror(errno));
            while (--i > 0)
                kill(pids[i], SIGKILL);
            return 1;
        }
    }
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
        fprintf(stderr, "etude-load: cannot allocate %llu bytes\n", (unsigned long long)bytes);
        return 1;
    }
    for (at = 0; at < bytes; at += LOAD_PAGE)
        m[at] = (unsigned char)at;
    if (ready() != 0)
        return 1;
    for (;;)
        pause();
}

/**
 * Write `bytes` to a file of this process's, unlinked once it is open, and
 * synchronize it, again and again until killed.
 */
static int load_disk(uint64_t bytes)
{
    static unsigned char chunk[LOAD_CHUNK];
    char name[64];
    uint64_t at;
    size_t n;
    int fd;

    snprintf(name, sizeof(name), "etude-load.%ld", (long)getpid());
    fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || unlink(name) != 0) {
        fprintf(stderr, "etude-load: %s: %s\n", name, strerror(errno));
        return 1;
    }
    memset(chunk, 0xa5, sizeof(chunk));
    if (ready() != 0)
        return 1;
    for (;;) {
        for (at = 0; at < bytes; at += n) {
            n = bytes - at < LOAD_CHUNK ? (size_t)(bytes - at) : LOAD_CHUNK;
            if (pwrite(fd, chunk, n, (off_t)at) != (ssize_t)n) {
                fprintf(stderr, "etude-load: write: %s\n", strerror(errno));
                return 1;
            }
        }
        if (fsync(fd) != 0) {
            fprintf(stderr, "etude-load: fsync: %s\n", strerror(errno));
            return 1;
        }
    }
}

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
