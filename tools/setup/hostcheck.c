/*
 * hostcheck.c: what the host must let a test run do, tried before anything
 * is built.
 *
 * `tests/run` checks that every command a suite names is installed, and a
 * restricted host session passed that check, built the product, and then
 * lost suites to a socket bind the session was not allowed and to a disk
 * image the session could not attach (entry 17 of milestone 50). So the
 * operations are tried, not the names: a TCP and a UDP socket bound on the
 * loopback address, a Unix socket bound in the directory given, and, when
 * `--diskimage` is given on a Mac, a one-megabyte image created, attached
 * without a mount and detached with `hdiutil`. A refusal names the
 * operation; the exit status is nonzero on any.
 *
 * Usage: hostcheck DIR [--diskimage]
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

static void refused(const char *what)
{
    fprintf(stderr, "hostcheck: refused: %s\n", what);
    failed = 1;
}

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

static void try_diskimage(const char *dir)
{
    char cmd[1024];

    /* hdiutil appends `.dmg` to the name it is given; sixteen megabytes is
       the least an ExFAT image takes. */
    snprintf(cmd, sizeof(cmd),
             "rm -f '%s/hostcheck.img.dmg' && hdiutil create -quiet -size 16m -layout NONE "
             "-fs ExFAT -volname HOSTCHECK '%s/hostcheck.img' >/dev/null 2>&1 && "
             "dev=$(hdiutil attach -nomount '%s/hostcheck.img.dmg' 2>/dev/null | head -1 | "
             "awk '{print $1}') && [ -n \"$dev\" ] && hdiutil detach \"$dev\" -quiet; rc=$?; "
             "rm -f '%s/hostcheck.img.dmg'; exit $rc",
             dir, dir, dir, dir);
    if (system(cmd) != 0) {
        refused("a disk image created, attached and detached with hdiutil");
    }
}

int main(int argc, char **argv)
{
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: hostcheck DIR [--diskimage]\n");
        return 2;
    }
    try_inet(SOCK_STREAM, "a TCP socket bound on the loopback address");
    try_inet(SOCK_DGRAM, "a UDP socket bound on the loopback address");
    try_unix(argv[1]);
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--diskimage") == 0) {
            try_diskimage(argv[1]);
        }
    }
    if (failed) {
        return 1;
    }
    printf("hostcheck: ok\n");
    return 0;
}
