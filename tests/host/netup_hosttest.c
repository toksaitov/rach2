/**
 * Check that a DHCP claim that cannot be read blocks the static configuration
 * of utils/netup.c, and how netup reports a DHCP client's exit status.
 *
 * host-deps: utils/netcfg.c utils/ipcheck.c
 * host-cflags: -I tests/host/compat/netutil
 *
 * The client is never started: the fork reports a child and the wait returns
 * the status a row chooses.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * open_error: the errno with which opening the claim fails, or 0.
 * read_error: nonzero to report a read error on the claim. live: the claim's
 * process exists. sockets: the socket calls made. child_status: the wait
 * status of the DHCP client.
 */
static int open_error, read_error, live, sockets, failures, child_status;
static const char *claim = "123\n10.0.0.2\n"; /* the claim file's text */

#define CHECK(condition, detail)                               \
    do {                                                       \
        if (!(condition)) {                                    \
            fprintf(stderr, "netup-test: fail: %s\n", detail); \
            failures++;                                        \
        }                                                      \
    } while (0)

/** Open a temporary stream with the text of `claim`, whatever `path` is, or
 * fail with `open_error` when it is set; exit with status 2 when it cannot. */
static FILE *model_fopen(const char *path, const char *mode)
{
    FILE *f;

    (void)path;
    (void)mode;
    if (open_error) {
        errno = open_error;
        return NULL;
    }
    f = tmpfile();
    if (!f) {
        exit(2);
    }
    fputs(claim, f);
    rewind(f);
    return f;
}

/** Report the stream's error, or an error when `read_error` is set even after
 * the pid was read. */
static int model_ferror(FILE *f)
{
    return read_error || ferror(f);
}

/** Fail as kill does for a process that exists and is not the caller's, with
 * EPERM, when `live` is set, and for one that does not exist, with ESRCH. */
static int model_kill(pid_t pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = live ? EPERM : ESRCH;
    return -1;
}

/** Count a socket call and fail it with EMFILE, so that no ioctl reaches the
 * host. */
static int model_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    sockets++;
    errno = EMFILE;
    return -1;
}

/** Report a child without starting one. */
static pid_t model_fork(void)
{
    return 4242;
}

/** Return the status the row chose for the child. */
static pid_t model_waitpid(pid_t pid, int *status, int options)
{
    (void)options;
    *status = child_status;
    return pid;
}

#define fopen   model_fopen
#define ferror  model_ferror
#define kill    model_kill
#define socket  model_socket
#define fork    model_fork
#define waitpid model_waitpid
#define main    netup_main
#include "netup.c"
#undef main
#undef fopen
#undef ferror
#undef kill
#undef socket
#undef fork
#undef waitpid

/**
 * Run run_dhcp for eth0 with `status` as the client's wait status, and copy
 * what it writes to stderr into `text`, of `size` bytes, terminated.
 *
 * Returns what run_dhcp returns; exits with status 2 when stderr cannot be
 * captured.
 */
static int run_dhcp_status(int status, char *text, size_t size)
{
    struct netcfg_iface iface;
    FILE *capture = tmpfile();
    int saved = dup(2);
    int rc;
    size_t n;

    if (!capture || saved < 0) {
        exit(2);
    }
    memset(&iface, 0, sizeof(iface));
    netcfg_set_name(iface.name, "eth0");
    child_status = status;
    fflush(stderr);
    dup2(fileno(capture), 2);
    rc = run_dhcp(&iface);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(capture);
    n = fread(text, 1, size - 1, capture);
    text[n] = '\0';
    fclose(capture);
    return rc;
}

/** Run the rows of dhcp_holder, apply_static and run_dhcp; return 0 when every
 * row passes. */
int main(void)
{
    struct netcfg_iface iface;
    int errors[] = {EIO, EMFILE, EACCES};
    char text[256];
    unsigned int i;

    memset(&iface, 0, sizeof(iface));
    netcfg_set_name(iface.name, "eth0");
    iface.address = 0x0a000002UL;
    iface.netmask = 0xffffff00UL;
    open_error = ENOENT;
    CHECK(dhcp_holder("eth0") == 0, "absent claim permits static configuration");
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        open_error = errors[i];
        CHECK(dhcp_holder("eth0") == -1, "unreadable claim is busy");
        CHECK(apply_static(&iface) == -1 && sockets == 0, "unreadable claim blocks static ioctls");
    }
    open_error = 0;
    read_error = 1;
    CHECK(dhcp_holder("eth0") == -1, "claim read error is busy");
    read_error = 0;
    claim = "";
    CHECK(dhcp_holder("eth0") == -1, "empty claim is busy");
    claim = "123\n";
    live = 1;
    CHECK(dhcp_holder("eth0") == 123, "live claim is busy");
    live = 0;
    CHECK(dhcp_holder("eth0") == 0, "dead holder permits static configuration");
    CHECK(run_dhcp_status(0, text, sizeof(text)) == 0 && text[0] == '\0',
          "a client that went to the background is success");
    CHECK(run_dhcp_status(1 << 8, text, sizeof(text)) == -1 &&
              strstr(text, "exited with status 1 before reaching the background") != NULL,
          "a client that exited is reported as never reaching the background");
    CHECK(run_dhcp_status(SIGKILL, text, sizeof(text)) == -1 &&
              strstr(text, "killed by signal 9 before reaching the background") != NULL,
          "a killed client is reported as never reaching the background");
    if (failures) {
        return 1;
    }
    puts("netup-test: result: pass");
    return 0;
}
