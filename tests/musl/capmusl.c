/**
 * Capabilities, read and given up with capget and capset.
 *
 * The kernel keeps them whole. As root the probe has every one, 0 to 40,
 * effective and permitted, and none inheritable. An unknown version is
 * EINVAL, and with no data 0, with version 3 written back into the header;
 * a set between none and all is EINVAL. A child that gives them up, as
 * Chromium's zygote does in each process it forks, has none, in capget and
 * in /proc/self/status, and so has a child it forks; it may not bind a port
 * below 1024 nor read a file only another user may, and may not take them
 * back. An exec gives them back, except under no_new_privs.
 *
 * The calls go through syscall by number, since musl has no wrapper.
 *
 * Prints "CAPMUSL OK" on success and "CAPMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define VERSION_3 0x20080522u
#define HIGH_WORD 0x1FFu /* capabilities 32 to 40 */
#define SECRET    "/tmp/capmusl.secret"

struct cap_header {
    unsigned version;
    int pid;
};

struct cap_data {
    unsigned effective;
    unsigned permitted;
    unsigned inheritable;
};

/* What the child of `dropped` found, by its exit status. */
static const char *const dropped_rows[] = {
    NULL,
    "a child could not report",
    "capset with every set empty failed",
    "a thread that gave up its capabilities still has them",
    "/proc/self/status did not show the capabilities given up",
    "a thread without capabilities bound a port below 1024",
    "a thread without capabilities read a file only another user may",
    "a thread without capabilities took them back",
    "a fork of a thread without capabilities has them",
};

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("CAPMUSL FAIL (%s)\n", what);

    return 1;
}

/** Read the caller's two sets into `d`; 0, or -1 with errno set. */
static int get_caps(struct cap_data *d)
{
    struct cap_header h = {VERSION_3, 0};

    memset(d, 0xAA, 2 * sizeof(*d));
    return (int)syscall(SYS_capget, &h, d);
}

/** Set the caller's two sets to `d`; 0, or -1 with errno set. */
static int set_caps(const struct cap_data *d)
{
    struct cap_header h = {VERSION_3, 0};

    return (int)syscall(SYS_capset, &h, d);
}

/** Whether `d` is every capability, effective and permitted. */
static int all_of(const struct cap_data *d)
{
    return d[0].effective == 0xFFFFFFFFu && d[0].permitted == 0xFFFFFFFFu &&
           d[0].inheritable == 0 && d[1].effective == HIGH_WORD && d[1].permitted == HIGH_WORD &&
           d[1].inheritable == 0;
}

/** Whether `d` is no capability at all. */
static int none_of(const struct cap_data *d)
{
    return (d[0].effective | d[0].permitted | d[0].inheritable | d[1].effective | d[1].permitted |
            d[1].inheritable) == 0;
}

/** Give up every capability; 0, or -1 with errno set. */
static int give_up(void)
{
    struct cap_data d[2];

    memset(d, 0, sizeof(d));
    return set_caps(d);
}

/** Whether /proc/self/status gives an empty effective set. */
static int status_has_none(void)
{
    char buf[2048];
    const char *line;
    int fd = open("/proc/self/status", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, buf, sizeof(buf) - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';
    line = strstr(buf, "CapEff:\t");
    return line && strncmp(line + 8, "0000000000000000\n", 17) == 0;
}

/** Whether a TCP socket may be bound to port 81 of the loopback, 1 if so,
 * 0 for EACCES and -1 for anything else. */
static int binds_low_port(void)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int rc;

    if (s < 0) {
        return -1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(81);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rc = bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 ? 1 : errno == EACCES ? 0 : -1;
    close(s);
    return rc;
}

/**
 * The rows of a child that gave up its capabilities: its exit status is 0,
 * or the index in `dropped_rows` of the row that failed.
 */
static int dropped(void)
{
    struct cap_data d[2];
    pid_t pid;
    int fd, status;

    if (give_up() != 0) {
        return 2;
    }
    if (get_caps(d) != 0 || !none_of(d)) {
        return 3;
    }
    if (!status_has_none()) {
        return 4;
    }
    if (binds_low_port() != 0) {
        return 5;
    }
    fd = open(SECRET, O_RDONLY);
    if (fd >= 0 || errno != EACCES) {
        return 6;
    }
    d[0].effective = d[0].permitted = 0xFFFFFFFFu;
    d[1].effective = d[1].permitted = HIGH_WORD;
    d[0].inheritable = d[1].inheritable = 0;
    if (set_caps(d) != -1 || errno != EPERM) {
        return 7;
    }
    pid = fork();
    if (pid == 0) {
        _exit(get_caps(d) == 0 && none_of(d) ? 0 : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return 8;
    }
    return 0;
}

/** Run `dropped` in a child; 0, or the index of the row that failed. */
static int in_dropped_child(void)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        _exit(dropped());
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 1;
    }
    return WEXITSTATUS(status) < (int)(sizeof(dropped_rows) / sizeof(dropped_rows[0]))
               ? WEXITSTATUS(status)
               : 1;
}

/**
 * Give up every capability in a child, under no_new_privs when `nnp` says
 * so, and exec this program to judge its sets with `mode`, "all" or
 * "none"; whether that image found them so.
 */
static int exec_finds(int nnp, const char *mode)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        if ((nnp && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) || give_up() != 0) {
            _exit(2);
        }
        execl("/proc/self/exe", "capmusl", mode, (char *)NULL);
        _exit(3);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

/** Make the file only user 1000 may read. */
static int make_secret(void)
{
    int fd = open(SECRET, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0 && write(fd, "secret\n", 7) == 7 && fchown(fd, 1000, 1000) == 0;

    if (fd >= 0) {
        close(fd);
    }
    return ok;
}

int main(int argc, char **argv)
{
    struct cap_header h = {0x12345678u, 0};
    struct cap_data d[2];
    int row;

    if (argc > 1) {
        if (get_caps(d) != 0) {
            return 1;
        }
        return strcmp(argv[1], "all") == 0 ? !all_of(d) : !none_of(d);
    }
    if (get_caps(d) != 0 || !all_of(d)) {
        return fail("root does not have every capability");
    }
    if (syscall(SYS_capget, &h, NULL) != 0 || h.version != VERSION_3) {
        return fail("an unknown version with no data was not 0 with version 3 written back");
    }
    h.version = 0x12345678u;
    if (syscall(SYS_capget, &h, d) != -1 || errno != EINVAL) {
        return fail("an unknown version was not EINVAL");
    }
    memset(d, 0, sizeof(d));
    d[0].effective = d[0].permitted = 1;
    if (set_caps(d) != -1 || errno != EINVAL) {
        return fail("a set between none and all was not EINVAL");
    }
    if (!make_secret()) {
        return fail("the file only user 1000 may read");
    }
    row = in_dropped_child();
    unlink(SECRET);
    if (row != 0) {
        return fail(dropped_rows[row]);
    }
    if (!exec_finds(0, "all")) {
        return fail("an exec as root did not give the capabilities back");
    }
    if (!exec_finds(1, "none")) {
        return fail("an exec under no_new_privs gave the capabilities back");
    }

    printf("CAPMUSL OK\n");

    return 0;
}
