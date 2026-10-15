/**
 * ncwrite_hosttest.c: the two output paths of `nc` (`utils/nc.c`) under short
 * writes, an interrupted write and failed writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

/*
 * The failures, whether each input has been read, the writes to each output,
 * the fault's output (-1 for none) and kind, and the bytes each output
 * received; an output's index is 0 for the socket and 1 for standard output.
 */
static int Failed, Input_read, Reply_read, Write_calls[2], Fault_fd, Fault_kind;
static char Written[2][1024], Input[1024], Reply[1024];
static size_t Used[2];

/**
 * The read stand-in: one chunk of 1024 bytes from standard input or from the
 * socket, then the end of file. Exits 2 when asked for another size.
 */
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

/**
 * The write stand-in: it takes at most seven bytes. The second write to the
 * output `Fault_fd` fails as `Fault_kind` says: 1 interrupted, 2 with nothing
 * written, 3 with an I/O error. Exits 2 when an output's buffer is full.
 */
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

/** The socket stand-in: descriptor 7, with no connection on the host. */
static int nc_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return 7;
}
/** The connect stand-in, which succeeds. */
static int nc_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    (void)fd;
    (void)addr;
    (void)len;
    return 0;
}
/** The close stand-in, which succeeds. */
static int nc_close(int fd)
{
    (void)fd;
    return 0;
}
/** The resolver stand-in: 127.0.0.1. */
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

/**
 * Run nc's main with the fault `kind` on the output `fd`, as at nc_write, and
 * check what both outputs received. With no fault or an interrupted write,
 * the status is 0 and each output has every byte; with a write of nothing or
 * an error, the status is 1 and the failing output has only its first seven
 * bytes.
 */
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

/**
 * Fill both inputs and run the case with no fault, then each fault on each
 * output; 0 when every case passed, 1 otherwise.
 */
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
