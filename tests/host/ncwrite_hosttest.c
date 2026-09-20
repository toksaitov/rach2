/** Check both nc output paths with short writes, interruptions and errors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

static int Failed, Input_read, Reply_read, Write_calls[2], Fault_fd, Fault_kind;
static char Written[2][1024], Input[1024], Reply[1024];
static size_t Used[2];

/** Return one full chunk from each input. */
static ssize_t nc_read(int fd, void *buf, size_t size)
{
    int *seen = fd == 0 ? &Input_read : &Reply_read;
    if ((*seen)++) {
        return 0;
    }
    if (size != 1024) {
        exit(2);
    }
    memcpy(buf, fd == 0 ? Input : Reply, size);
    return (ssize_t)size;
}

/** Accept at most seven bytes, with a selected error on either output. */
static ssize_t nc_write(int fd, const void *buf, size_t n)
{
    int index = fd == 1 ? 1 : 0;
    Write_calls[index]++;
    if (index == Fault_fd && Write_calls[index] == 2) {
        if (Fault_kind == 1) {
            errno = EINTR;
            return -1;
        }
        if (Fault_kind == 2) {
            return 0;
        }
        if (Fault_kind == 3) {
            errno = EIO;
            return -1;
        }
    }
    if (n > 7) {
        n = 7;
    }
    if (n > sizeof(Written[index]) - Used[index]) {
        exit(2);
    }
    memcpy(Written[index] + Used[index], buf, n);
    Used[index] += n;
    return (ssize_t)n;
}

/** The test exchanges bytes without using a host network connection. */
static int nc_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return 7;
}
static int nc_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    (void)fd;
    (void)addr;
    (void)len;
    return 0;
}
static int nc_close(int fd)
{
    (void)fd;
    return 0;
}
int resolve_host(const char *host, unsigned int *out)
{
    (void)host;
    *out = 0x7f000001;
    return 0;
}

#define read    nc_read
#define write   nc_write
#define socket  nc_socket
#define connect nc_connect
#define close   nc_close
#define main    nc_main
#include "nc.c"
#undef main
#undef read
#undef write
#undef socket
#undef connect
#undef close

/** Run the real main and compare every byte received by both outputs. */
static void run_case(int fd, int kind)
{
    char *args[] = {(char *)"nc", (char *)"127.0.0.1", (char *)"80", NULL};
    int result;

    Input_read = Reply_read = 0;
    memset(Write_calls, 0, sizeof(Write_calls));
    memset(Used, 0, sizeof(Used));
    Fault_fd = fd;
    Fault_kind = kind;
    result = nc_main(3, args);
    if (kind <= 1) {
        if (result != 0 || Used[0] != sizeof(Input) || Used[1] != sizeof(Reply) ||
            memcmp(Written[0], Input, sizeof(Input)) || memcmp(Written[1], Reply, sizeof(Reply))) {
            fprintf(stderr, "ncwrite-hosttest: fail: complete partial writes on both outputs\n");
            Failed++;
        }
    } else if (result != 1 || Used[fd] != 7) {
        fprintf(stderr, "ncwrite-hosttest: fail: failed write stops transfer\n");
        Failed++;
    }
}

/** Require complete writes and a nonzero result after either output fails. */
int main(void)
{
    size_t i;
    int fd, kind;

    for (i = 0; i < sizeof(Input); i++) {
        Input[i] = (char)(i % 251);
        Reply[i] = (char)(250 - i % 251);
    }
    run_case(-1, 0);
    for (fd = 0; fd < 2; fd++) {
        for (kind = 1; kind <= 3; kind++) {
            run_case(fd, kind);
        }
    }
    printf("ncwrite-hosttest: result: %s\n", Failed ? "fail" : "pass");
    return Failed ? 1 : 0;
}
