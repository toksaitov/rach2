/* An older checkout's host admission and port block protocol. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "old_ports_host.h"

static void interval(void)
{
    struct timespec delay = {0, 50000000};
    /* interval: the old helper retried a conflicting record lock every 50 ms */
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static int open_lock(const char *directory, const char *name)
{
    char path[4096];
    mode_t mask = umask(0);
    int size = snprintf(path, sizeof(path), "%s/%s", directory, name);
    int fd = size >= 0 && size < (int)sizeof(path) ? open(path, O_RDWR | O_CREAT, 0666) : -1;
    umask(mask);
    return fd;
}

static int lock(int fd, short kind)
{
    struct flock request;
    memset(&request, 0, sizeof(request));
    request.l_type = kind;
    request.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLK, &request);
}

static int take(const char *directory, const char *name, short kind, int *said, const char *waiting)
{
    int fd = open_lock(directory, name);
    if (fd < 0)
        return -1;
    while (lock(fd, kind) != 0) {
        if (errno != EACCES && errno != EAGAIN) {
            close(fd);
            return -1;
        }
        if (!*said) {
            puts(waiting);
            fflush(stdout);
            *said = 1;
        }
        interval();
    }
    return fd;
}

static int host(const char *directory)
{
    const char *mode = getenv("RACH2_HOST_LOCK"), *waiting;
    int gate, said = 0, alone;
    if (mode == NULL)
        return 0;
    alone = !strcmp(mode, "alone");
    waiting = alone ? "run: host: waiting for the runs in progress to end"
                    : "run: host: waiting for a run that needs the host alone";
    unsetenv("RACH2_HOST_LOCK");
    gate = take(directory, "rach2-host.gate", alone ? F_WRLCK : F_RDLCK, &said, waiting);
    if (gate < 0 ||
        take(directory, "rach2-host.lock", alone ? F_WRLCK : F_RDLCK, &said, waiting) < 0)
        return -1;
    if (!alone)
        close(gate);
    return 0;
}

int host_old_ports(const char *directory, unsigned count, char *const command[])
{
    char name[64], offset[32];
    unsigned slot;
    int fd, waited = 0;
    if (host(directory) != 0)
        return 126;
    for (;;) {
        for (slot = 0; slot < count; slot++) {
            snprintf(name, sizeof(name), "rach2-ports.%u", slot);
            fd = open_lock(directory, name);
            if (fd < 0)
                return 126;
            if (lock(fd, F_WRLCK) == 0) {
                snprintf(offset, sizeof(offset), "%u", slot * 1000);
                if (setenv("RACH2_PORT_OFFSET", offset, 1) != 0)
                    return 126;
                execvp(command[0], command);
                return 126;
            }
            close(fd);
        }
        if (!waited) {
            printf("run: ports: all %u blocks are held by other runs; waiting\n", count);
            fflush(stdout);
            waited = 1;
        }
        interval();
    }
}
