/*
 * hostcheck.c: what the host must let a test run do, tried before anything
 * is built.
 *
 * `tests/run` checks that every command a suite names is installed, but a
 * restricted host session can pass that check, build the product, and then
 * lose suites to a socket bind the session is not allowed or a disk image
 * it cannot attach. So the operations are tried, not the names: unless
 * `--no-sockets` is given, a TCP and a UDP socket bound on the loopback
 * address and a Unix socket bound in the directory given, and, when
 * `--diskimage` is given on a Mac, a sixteen-megabyte image created,
 * attached without a mount and detached with `hdiutil`. A refusal names the
 * operation; the exit status is nonzero on any. The directory's name goes
 * into a shell command, so an apostrophe in it is quoted as the shell wants.
 *
 * Usage: hostcheck DIR [--diskimage] [--no-sockets]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int failed;

/** Record a refused operation by name. */
static void refused(const char *what)
{
    fprintf(stderr, "hostcheck: refused: %s\n", what);
    failed = 1;
}

/** Bind a socket of `type` on the loopback address, or record the refusal. */
static void try_inet(int type, const char *name)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, type, 0);

    if (s < 0) {
        refused(name);
        return;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        refused(name);
    } else if (type == SOCK_STREAM && listen(s, 1) != 0) {
        refused(name);
    }
    close(s);
}

/** Bind a Unix socket in `dir`, or record the refusal. */
static void try_unix(const char *dir)
{
    struct sockaddr_un a;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    if (s < 0) {
        refused("a Unix socket");
        return;
    }
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (snprintf(a.sun_path, sizeof(a.sun_path), "%s/hostcheck.sock", dir) >=
        (int)sizeof(a.sun_path)) {
        refused("a Unix socket: the directory's name is too long for one");
        close(s);
        return;
    }
    unlink(a.sun_path);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        refused("a Unix socket bound in the work directory");
    }
    unlink(a.sun_path);
    close(s);
}

/**
 * Write `dir` into `out`, of `size` bytes, inside shell single quotes, with
 * each apostrophe written as `'\''`. Return 0, or -1 when the quoted form
 * does not fit.
 */
static int shell_quote(char *out, size_t size, const char *dir)
{
    size_t n = 0;
    const char *p;

    if (n + 1 >= size) {
        return -1;
    }
    out[n++] = '\'';
    for (p = dir; *p; p++) {
        const char *piece = *p == '\'' ? "'\\''" : NULL;
        size_t len = piece ? strlen(piece) : 1;

        if (n + len + 2 >= size) {
            return -1;
        }
        if (piece) {
            memcpy(out + n, piece, len);
        } else {
            out[n] = *p;
        }
        n += len;
    }
    out[n++] = '\'';
    out[n] = '\0';
    return 0;
}

/**
 * Create, attach and detach a sixteen-megabyte image in `dir` with hdiutil,
 * or record the refusal. hdiutil appends `.dmg` to the name it is given, and
 * sixteen megabytes is the least an ExFAT image takes.
 */
static void try_diskimage(const char *dir)
{
    char q[512];
    char cmd[3072];
    int n;

    if (shell_quote(q, sizeof(q), dir) != 0) {
        refused("a directory name short enough to quote for hdiutil");
        return;
    }
    n = snprintf(cmd, sizeof(cmd),
                 "rm -f %s/hostcheck.img.dmg && hdiutil create -quiet -size 16m -layout NONE "
                 "-fs ExFAT -volname HOSTCHECK %s/hostcheck.img >/dev/null 2>&1 && "
                 "dev=$(hdiutil attach -nomount %s/hostcheck.img.dmg 2>/dev/null | head -1 | "
                 "awk '{print $1}') && [ -n \"$dev\" ] && hdiutil detach \"$dev\" -quiet; rc=$?; "
                 "rm -f %s/hostcheck.img.dmg; exit $rc",
                 q, q, q, q);
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        refused("a directory name short enough for the hdiutil command");
        return;
    }
    if (system(cmd) != 0) {
        refused("a disk image created, attached and detached with hdiutil");
    }
}

/**
 * Try the operations the options choose. The exit status is 0 when all
 * succeed, 1 on a refusal and 2 on a usage error.
 */
int main(int argc, char **argv)
{
    int i, sockets = 1, diskimage = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: hostcheck DIR [--diskimage] [--no-sockets]\n");
        return 2;
    }
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--diskimage") == 0) {
            diskimage = 1;
        } else if (strcmp(argv[i], "--no-sockets") == 0) {
            sockets = 0;
        } else {
            fprintf(stderr, "hostcheck: unknown option %s\n", argv[i]);
            fprintf(stderr, "usage: hostcheck DIR [--diskimage] [--no-sockets]\n");
            return 2;
        }
    }
    if (sockets) {
        try_inet(SOCK_STREAM, "a TCP socket bound on the loopback address");
        try_inet(SOCK_DGRAM, "a UDP socket bound on the loopback address");
        try_unix(argv[1]);
    }
    if (diskimage) {
        try_diskimage(argv[1]);
    }
    if (failed) {
        return 1;
    }
    printf("hostcheck: ok\n");
    return 0;
}
