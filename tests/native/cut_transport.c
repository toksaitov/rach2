/* cut_transport.c: cut the first pipe after TinyCC completes one build. */
#define _POSIX_C_SOURCE 200809L
#include "collection.h"
#include "wire.h"
#include "etude/frame.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
struct relay {
    int to, from, cut;
    pid_t guest;
    const char *marker;
    char tail[256];
    size_t kept;
};
static int all(int fd, const char *data, size_t bytes)
{
    while (bytes) {
        ssize_t n = write(fd, data, bytes);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        data += n;
        bytes -= (size_t)n;
    }
    return 1;
}
static int completed(struct relay *r, const char *line, size_t bytes)
{
    char *copy = malloc(bytes + 1);
    unsigned char *decoded = malloc(bytes + 1);
    struct wire_frame f;
    long n;
    int found = 0;
    if (!copy || !decoded) {
        free(copy);
        free(decoded);
        return 0;
    }
    memcpy(copy, line, bytes);
    copy[bytes] = 0;
    if (!wire_parse(copy, bytes, &f) && !strcmp(f.type, ETUDE_EV_OUT) && f.nfields == 5 &&
        !strcmp(f.field[2], "1") && (n = wire_unbase64(f.field[4], decoded)) > 0) {
        for (long i = 0; i < n; i++) {
            if (r->kept + 1 == sizeof(r->tail)) {
                memmove(r->tail, r->tail + 1, --r->kept);
            }
            r->tail[r->kept++] = (char)decoded[i];
            r->tail[r->kept] = 0;
            if (strstr(r->tail, "tccbuild: build 1\n"))
                found = 1;
        }
    }
    free(copy);
    free(decoded);
    return found;
}
static int output(struct relay *r, char *buffer, size_t *bytes)
{
    size_t start = 0;
    for (size_t n = 0; n < *bytes; n++) {
        if (buffer[n] != '\n')
            continue;
        if (!all(1, buffer + start, n - start + 1))
            return 0;
        if (completed(r, buffer + start, n - start)) {
            int marker = open(r->marker, O_WRONLY | O_TRUNC);
            if (marker < 0 || !all(marker, "cut\n", 4)) {
                if (marker >= 0)
                    close(marker);
                return 0;
            }
            close(marker);
            close(r->to);
            close(1);
            r->cut = 1;
            return 1;
        }
        start = n + 1;
    }
    memmove(buffer, buffer + start, *bytes - start);
    *bytes -= start;
    return 1;
}
static int relay(struct relay *r)
{
    char input[32768], *buffer = malloc(ETUDE_FRAME_STDIO_MAX + 1);
    size_t bytes = 0;
    int open_input = 1, ok = buffer != NULL, status;
    while (ok) {
        struct pollfd fd[2] = {{open_input ? 0 : -1, POLLIN, 0}, {r->from, POLLIN, 0}};
        ssize_t got;
        if (poll(fd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (open_input && (fd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
            got = read(0, input, sizeof(input));
            if (got <= 0) {
                open_input = 0;
                close(r->to);
            } else
                ok = all(r->to, input, (size_t)got);
        }
        if (fd[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            got = read(r->from, buffer + bytes, ETUDE_FRAME_STDIO_MAX - bytes);
            if (got <= 0)
                break;
            bytes += (size_t)got;
            if (!r->cut)
                ok = output(r, buffer, &bytes);
            else
                bytes = 0;
            if (r->cut)
                open_input = 0;
            if (bytes == ETUDE_FRAME_STDIO_MAX)
                ok = 0;
        }
    }
    if (open_input)
        close(r->to);
    close(r->from);
    free(buffer);
    while (waitpid(r->guest, &status, 0) < 0 && errno == EINTR) {
    }
    return ok ? 0 : 1;
}
int nc_cut_transport(int argc, char **argv)
{
    struct relay r;
    int to[2], from[2], marker;
    if (argc < 6 || strcmp(argv[3], "--"))
        return 2;
    marker = open(argv[2], O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (marker < 0) {
        if (errno != EEXIST)
            return 2;
        execv(argv[4], argv + 4);
        return 127;
    }
    close(marker);
    if (pipe(to))
        return 2;
    if (pipe(from)) {
        close(to[0]);
        close(to[1]);
        return 2;
    }
    memset(&r, 0, sizeof(r));
    r.marker = argv[2];
    signal(SIGPIPE, SIG_IGN);
    r.guest = fork();
    if (!r.guest) {
        if (dup2(to[0], 0) < 0 || dup2(from[1], 1) < 0)
            _exit(126);
        close(to[0]);
        close(to[1]);
        close(from[0]);
        close(from[1]);
        execv(argv[4], argv + 4);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    r.to = to[1];
    r.from = from[0];
    if (r.guest < 0) {
        close(r.to);
        close(r.from);
        return 2;
    }
    return relay(&r);
}
