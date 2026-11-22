/**
 * hvcmusl: check what a program sees of the virtio console when one of its
 * ports is the kernel's console, and that none of it is there when the
 * console is the UART.
 *
 * The one argument is the console the boot line names: `vport0p1`, port 1 of
 * the virtio console, which etude gives a guest case's machine, or `ttyS0`,
 * with which the machine has a UART and no virtio console.
 *
 * With `vport0p1` the boot line has `console=vport0p1`; a device of
 * /sys/bus/virtio is bound to `virtio_console`; /dev/vport0p1 is the
 * character device 245:1 and /dev/hvc0, the node of a console port, is
 * absent, by name and in /dev's listing; an open of /dev/vport0p1 gives the
 * console terminal, with the modes and the size /dev/console has; and the
 * log has the driver's record of its binding and the console's of the port.
 * With `ttyS0` the boot line has `console=ttyS0`, and neither node, no bound
 * device and no record of the port are there.
 *
 * The machine case guest.hvc.ports runs the other modes from the debug
 * shell of a machine with a UART, whose boot line names hvc0 before ttyS0,
 * and whose virtio console has a generic port named org.rach2.boot with no
 * host's end at the boot; through QMP the case adds a console port, a
 * third port, org.rach2.late, and then two ports at 4 and 5 whose names
 * are of NAME_MAX bytes and differ only in their last byte. Each of `tty`,
 * `late`, `gone` and `long` first waits for the port it checks to come or
 * go. `boot`: /dev/vport0p1 and
 * /dev/virtio-ports/org.rach2.boot are one character device of major 245,
 * whose open gives the end of a file, POLLHUP and EAGAIN for a write while
 * the host's end is closed, and refuses a second open; /dev/hvc0 is absent
 * and the log says console=hvc0 names no port. `tty`: /dev/hvc0 is the
 * terminal 229:0, which takes `hvc-tty` and a newline, and /dev/vport0p0,
 * whose tcdrain returns 0 once khvcd has handed the line to the port, after
 * which TIOCOUTQ reads 0, so that the line is out before the mode exits to
 * the debug shell, which runs no thread; a drain that a signal interrupts
 * fails with EINTR, though the handler has SA_RESTART, with the output
 * still counted (`drain_rows`), and so does one whose output went out as
 * the signal came (`late_signal_row`); on a pty pair, the drain and
 * TIOCOUTQ of either end, the termio forms, a termios change that waits
 * for a blocked write to end, and a drain stopped by SIGTTOU that is
 * restarted once continued (`pty_rows`); and /dev/vport0p0, the console
 * port's vport node, refuses an open with ENXIO. `late`:
 * /dev/vport0p3 and /dev/virtio-ports/org.rach2.late are one device, which
 * takes `ping` and a newline and gives `pong` and a newline, the bytes the
 * host's end has, and polls writable. `gone`: the late port's nodes are
 * absent and the boot port's stays. `long`: /dev/virtio-ports lists each
 * long name once and whole, and the name it gives opens the port's device,
 * /dev/vport0p4 or /dev/vport0p5.
 *
 * Prints `HVCMUSL OK` and exits 0 on success, and prints `HVCMUSL FAIL
 * (detail)` and exits 1 otherwise. etude's report (probe_report.h), for the
 * case ETUDE_CASE names, has the assertions `named`, `bound`, `node`,
 * `listed`, `terminal` and `log` with `vport0p1`, and `named` and `absent`
 * with `ttyS0`. The first failure fails its assertion and leaves the rest
 * not run. The other modes report nothing, since their machine case's
 * scenario judges their exit.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "../probe_report.h"

#define PORT_NODE    "/dev/vport0p1"
#define CONSOLE_NODE "/dev/hvc0"
#define PORT_MAJOR   245u
#define PORT_MINOR   1u
#define VIRTIO_BUS   "/sys/bus/virtio/devices"

#define BOOT_NODE "/dev/vport0p1"
#define BOOT_LINK "/dev/virtio-ports/org.rach2.boot"
#define LATE_NODE "/dev/vport0p3"
#define LATE_LINK "/dev/virtio-ports/org.rach2.late"

/* The two ports of names of NAME_MAX bytes, the prefix, 244 of 'n' and a
 * last byte of 'a' or 'b', at ports 4 and 5: their aliases' paths are 273
 * bytes, and the names differ only in their last byte. */
#define PORTS_DIR   "/dev/virtio-ports"
#define LONG_PREFIX "org.rach2."
#define LONG_NAME   255

/* The kernel's log, read whole: the ring is 128 KiB. */
static char ring[131072 + 1];

/* Whether the mode reports to etude: the guest cases' modes alone. */
static int reporting;

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    char why[96];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("HVCMUSL FAIL (%s)\n", why);
    if (reporting) {
        report_stop(why);
    }
    return 1;
}

/** Read the file at `path` into `buf` of `size` bytes as a string; the
 * count read, or -1. */
static int slurp(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY), n;

    if (fd < 0) {
        return -1;
    }
    n = (int)read(fd, buf, size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return n;
}

/** Whether the boot line has the word `word`, alone between blanks. */
static int boot_word(const char *word)
{
    char line[4096], *at;
    size_t len = strlen(word);

    if (slurp("/proc/cmdline", line, sizeof(line)) < 0) {
        return 0;
    }
    for (at = strstr(line, word); at != NULL; at = strstr(at + 1, word)) {
        if ((at == line || at[-1] == ' ') &&
            (at[len] == '\0' || at[len] == ' ' || at[len] == '\n')) {
            return 1;
        }
    }
    return 0;
}

/** Count the devices of the virtio bus bound to `virtio_console`, by the
 * DRIVER of each one's uevent; 0 too when the bus has no directory. */
static int bound_consoles(void)
{
    DIR *bus = opendir(VIRTIO_BUS);
    struct dirent *e;
    char path[320], text[512];
    int count = 0;

    if (bus == NULL) {
        return 0;
    }
    while ((e = readdir(bus)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), VIRTIO_BUS "/%s/uevent", e->d_name);
        if (slurp(path, text, sizeof(text)) > 0 &&
            strstr(text, "DRIVER=virtio_console\n") != NULL) {
            count++;
        }
    }
    closedir(bus);
    return count;
}

/** Count the entries of /dev named `name`; -1 when /dev does not open. */
static int listed(const char *name)
{
    DIR *dev = opendir("/dev");
    struct dirent *e;
    int count = 0;

    if (dev == NULL) {
        return -1;
    }
    while ((e = readdir(dev)) != NULL) {
        if (strcmp(e->d_name, name) == 0) {
            count++;
        }
    }
    closedir(dev);
    return count;
}

/** Whether the kernel's log has `text`; -1 when it cannot be read. */
static int logged(const char *text)
{
    int n = klogctl(3, ring, (int)sizeof(ring) - 1);

    if (n < 0) {
        return -1;
    }
    ring[n] = '\0';
    return strstr(ring, text) != NULL;
}

/** The modes and the size of the terminal at `path`, opened without taking
 * it as the controlling terminal; 0, or -1 with errno. */
static int terminal_of(const char *path, struct termios *modes, struct winsize *size)
{
    int fd = open(path, O_RDWR | O_NOCTTY), r = -1;

    if (fd < 0) {
        return -1;
    }
    if (isatty(fd) && tcgetattr(fd, modes) == 0 && ioctl(fd, TIOCGWINSZ, size) == 0) {
        r = 0;
    }
    close(fd);
    return r;
}

/** The rows of a boot whose console is the virtio port; 0 when each held. */
static int port_rows(void)
{
    struct termios port_modes, console_modes;
    struct winsize port_size, console_size;
    struct stat st;

    if (!boot_word("console=vport0p1")) {
        return fail("the boot line has no console=vport0p1");
    }
    report_held();
    if (bound_consoles() != 1) {
        return fail("the virtio bus has not one device bound to virtio_console");
    }
    report_held();
    if (stat(PORT_NODE, &st) != 0 || !S_ISCHR(st.st_mode) || major(st.st_rdev) != PORT_MAJOR ||
        minor(st.st_rdev) != PORT_MINOR) {
        return fail(PORT_NODE " is not the character device 245:1");
    }
    errno = 0;
    if (lstat(CONSOLE_NODE, &st) == 0 || errno != ENOENT) {
        return fail(CONSOLE_NODE " is there beside a port that is no console port");
    }
    report_held();
    if (listed("vport0p1") != 1 || listed("hvc0") != 0) {
        return fail("/dev does not list vport0p1 once and hvc0 never");
    }
    report_held();
    memset(&port_modes, 0, sizeof(port_modes));
    memset(&console_modes, 0, sizeof(console_modes));
    if (terminal_of(PORT_NODE, &port_modes, &port_size) != 0 ||
        terminal_of("/dev/console", &console_modes, &console_size) != 0) {
        return fail("the port's node or /dev/console is no terminal");
    }
    if (memcmp(&port_modes, &console_modes, sizeof(port_modes)) != 0 ||
        memcmp(&port_size, &console_size, sizeof(port_size)) != 0) {
        errno = 0;
        return fail("the port's node has other modes or another size than /dev/console");
    }
    report_held();
    errno = 0;
    if (logged("virtio_console: bound: virtio") != 1 ||
        logged("console: enabled: vport0p1, port 1 of virtio") != 1) {
        return fail("the log lacks the driver's binding or the console's port");
    }
    report_held();
    return 0;
}

/** The rows of a boot whose console is the UART; 0 when each held. */
static int uart_rows(void)
{
    struct stat st;

    if (!boot_word("console=ttyS0") || boot_word("console=vport0p1")) {
        return fail("the boot line does not name ttyS0 alone");
    }
    report_held();
    errno = 0;
    if (lstat(PORT_NODE, &st) == 0 || lstat(CONSOLE_NODE, &st) == 0 || errno != ENOENT) {
        return fail("a node of a virtio console's port is there");
    }
    if (listed("vport0p1") != 0 || listed("hvc0") != 0 || bound_consoles() != 0) {
        errno = 0;
        return fail("/dev lists a port's node, or a device is bound to virtio_console");
    }
    if (logged("console: enabled: vport0p1") != 0) {
        return fail("the log has a virtio port as the console");
    }
    report_held();
    return 0;
}

/** The device number of the character device at `path` whose major is
 * `major_want`; 0, with errno, when it is none. */
static dev_t char_dev(const char *path, unsigned major_want)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISCHR(st.st_mode) || major(st.st_rdev) != major_want) {
        errno = 0;
        return 0;
    }
    return st.st_rdev;
}

/** Whether nothing is at `path`. */
static int absent(const char *path)
{
    struct stat st;

    errno = 0;
    return lstat(path, &st) != 0 && errno == ENOENT;
}

/** Wait, asleep, until something is at `path` when `present` is set, or
 * nothing is when it is clear: the debug shell that starts this program
 * runs no thread while it waits for a line, so the driver's thread, which
 * acts on the ports the host adds and removes, runs while a program
 * sleeps. The case's bound ends a wait that does not end. */
static void settle(const char *path, int present)
{
    while (absent(path) == present) {
        usleep(100000); /* interval: the node is looked at again every 100 ms */
    }
}

/** Put in `name`, of LONG_NAME + 1 bytes, the long port's name that ends
 * in `last`. */
static void long_name(char *name, char last)
{
    size_t n = strlen(LONG_PREFIX);

    memcpy(name, LONG_PREFIX, n);
    memset(name + n, 'n', LONG_NAME - 1 - n);
    name[LONG_NAME - 1] = last;
    name[LONG_NAME] = '\0';
}

/** The rows of `long`: /dev/virtio-ports lists each long port once with
 * its whole name, and the name the listing gives opens, through the walk,
 * the port's own device, /dev/vport0p4 or /dev/vport0p5. 0 when each held. */
static int long_rows(void)
{
    char want[2][LONG_NAME + 1], path[sizeof(PORTS_DIR) + LONG_NAME + 1];
    static const char *const nodes[2] = {"/dev/vport0p4", "/dev/vport0p5"};
    struct dirent *e;
    struct stat st;
    int seen[2] = {0, 0}, i, fd;
    DIR *dir;

    long_name(want[0], 'a');
    long_name(want[1], 'b');
    snprintf(path, sizeof(path), PORTS_DIR "/%s", want[1]);
    settle(path, 1);
    dir = opendir(PORTS_DIR);
    if (dir == NULL) {
        return fail(PORTS_DIR " does not open");
    }
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, LONG_PREFIX "nnnn", sizeof(LONG_PREFIX) + 3) != 0) {
            continue;
        }
        for (i = 0; i < 2 && strcmp(e->d_name, want[i]) != 0; i++) {
        }
        if (i == 2) {
            closedir(dir);
            errno = 0;
            return fail("the listing gives a long port's name cut short");
        }
        seen[i]++;
        snprintf(path, sizeof(path), PORTS_DIR "/%s", e->d_name);
        fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            closedir(dir);
            return fail("a long port's listed name does not open");
        }
        if (fstat(fd, &st) != 0 || char_dev(nodes[i], PORT_MAJOR) != st.st_rdev) {
            close(fd);
            closedir(dir);
            return fail("a long port's name opens another device than its vport node");
        }
        close(fd);
    }
    closedir(dir);
    if (seen[0] != 1 || seen[1] != 1) {
        errno = 0;
        return fail("the listing does not give each long port's name once");
    }
    return 0;
}

/** The rows of `boot`; 0 when each held. */
static int boot_rows(void)
{
    struct pollfd p;
    dev_t dev = char_dev(BOOT_NODE, PORT_MAJOR);
    char c;
    int fd, other;

    if (bound_consoles() != 1) {
        return fail("the virtio bus has not one device bound to virtio_console");
    }
    if (dev == 0 || char_dev(BOOT_LINK, PORT_MAJOR) != dev) {
        return fail(BOOT_NODE " and " BOOT_LINK " are not one device of major 245");
    }
    if (!absent(CONSOLE_NODE)) {
        return fail(CONSOLE_NODE " is there with no console port");
    }
    fd = open(BOOT_LINK, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return fail("the boot port does not open");
    }
    other = open(BOOT_NODE, O_RDWR);
    if (other >= 0 || errno != EBUSY) {
        return fail("a second open of the port is not refused with EBUSY");
    }
    p.fd = fd;
    p.events = POLLIN | POLLOUT;
    p.revents = 0;
    if (poll(&p, 1, 0) != 1 || !(p.revents & POLLHUP) || (p.revents & POLLOUT)) {
        errno = 0;
        return fail("poll does not say hung up and not writable while the host's end is closed");
    }
    if (read(fd, &c, 1) != 0) {
        return fail("a read of the port with the host's end closed is not the end of a file");
    }
    if (write(fd, "x", 1) != -1 || errno != EAGAIN) {
        return fail("a write without a wait does not fail with EAGAIN");
    }
    close(fd);
    errno = 0;
    if (logged("console: none: console=hvc0 names no port now") != 1) {
        return fail("the log does not say console=hvc0 names no port");
    }
    return 0;
}

/** Set the policy and priority of the calling thread through the system
 * call, since musl's sched_setscheduler fails with ENOSYS. */
static int set_policy(int policy, int priority)
{
    struct sched_param sp;

    sp.sched_priority = priority;
    return (int)syscall(SYS_sched_setscheduler, 0, policy, &sp);
}

/* The SIGUSR1s the interrupted drain took. */
static volatile sig_atomic_t drain_signals;

/** The interrupted drain's handler: count the signal. */
static void drain_signal(int signo)
{
    (void)signo;
    drain_signals++;
}

/** The interrupted drain's child: wait for the parent's byte on `go`, then,
 * running only once the parent's drain blocks, since both are SCHED_FIFO 99
 * on the one processor, send it SIGUSR1, drop to SCHED_FIFO 1, which hands
 * the processor back to it, and spin, which keeps khvcd, an ordinary
 * thread, off the processor until the parent kills it. */
static void drain_child(int go)
{
    volatile unsigned long spins = 0;
    char c;

    if (read(go, &c, 1) != 1) {
        _exit(1);
    }
    kill(getppid(), SIGUSR1);
    set_policy(SCHED_FIFO, 1);
    for (;;) {
        spins++;
    }
}

/**
 * The interrupted drain on the terminal `fd`, on the case's machine of one
 * processor: with this process and a child at SCHED_FIFO 99 (`drain_child`),
 * 1024 bytes written and a drain begun, the child's SIGUSR1 ends the drain
 * with EINTR, though the handler has SA_RESTART, as Linux's TCSBRK is never
 * restarted, while TIOCOUTQ still counts the bytes khvcd, kept off the
 * processor, has not handed to the port. Once the child is killed and this
 * process is SCHED_OTHER again, a drain returns 0 and TIOCOUTQ reads 0.
 * Returns 0 when each row held.
 */
static int drain_rows(int fd)
{
    struct sigaction sa;
    char burst[1024];
    int go[2], queued = 0, rc, err, status;
    pid_t child;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = drain_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || pipe(go) != 0 || set_policy(SCHED_FIFO, 99) != 0) {
        return fail("the interrupted drain cannot be set up");
    }
    child = fork();
    if (child == 0) {
        close(go[1]);
        drain_child(go[0]);
    }
    close(go[0]);
    if (child < 0) {
        return fail("the interrupted drain's child does not start");
    }
    memset(burst, 'd', sizeof(burst) - 1);
    burst[sizeof(burst) - 1] = '\n';
    if (write(go[1], "g", 1) != 1 || write(fd, burst, sizeof(burst)) != (ssize_t)sizeof(burst)) {
        err = errno;
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        errno = err;
        return fail("the terminal does not take the drain's bytes");
    }
    errno = 0;
    rc = drain_signals == 0 ? tcdrain(fd) : 0;
    err = errno;
    if (rc == -1 && err == EINTR && ioctl(fd, TIOCOUTQ, &queued) != 0) {
        queued = -1;
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    close(go[1]);
    set_policy(SCHED_OTHER, 0);
    errno = err;
    if (rc != -1 || err != EINTR || drain_signals != 1) {
        return fail("a drain that SIGUSR1 interrupts does not fail with EINTR");
    }
    if (queued <= 0) {
        errno = 0;
        return fail("TIOCOUTQ does not count the bytes of an interrupted drain");
    }
    if (tcdrain(fd) != 0) {
        return fail("tcdrain fails after the interrupted drain");
    }
    if (ioctl(fd, TIOCOUTQ, &queued) != 0 || queued != 0) {
        return fail("TIOCOUTQ does not read 0 after tcdrain");
    }
    return 0;
}

/** The pid of the thread whose comm is `name`, or -1: a kernel thread
 * such as khvcd, which /proc lists as any process. */
static pid_t pid_named(const char *name)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    char path[300], comm[32];
    pid_t found = -1;

    while (d && found < 0 && (e = readdir(d)) != NULL) {
        size_t n;

        if (e->d_name[0] < '1' || e->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        if (slurp(path, comm, sizeof(comm)) <= 0) {
            continue;
        }
        n = strlen(comm);
        if (n > 0 && comm[n - 1] == '\n') {
            comm[n - 1] = '\0';
        }
        if (strcmp(comm, name) == 0) {
            found = (pid_t)atoi(e->d_name);
        }
    }
    if (d) {
        closedir(d);
    }
    return found;
}

/** Set the policy and priority of the thread `pid` through the system
 * call; 0 or -1. */
static int set_policy_of(pid_t pid, int policy, int priority)
{
    struct sched_param sp;

    sp.sched_priority = priority;
    return (int)syscall(SYS_sched_setscheduler, pid, policy, &sp);
}

/**
 * The drain whose output reaches the port as its signal comes, on the
 * terminal `fd` of the case's machine of one processor: this process, a
 * child and khvcd all at SCHED_FIFO 99, so that each runs in the order it
 * became ready. Five bytes written and a drain begun, khvcd runs first,
 * hands them to the port and wakes the drain, whose wait is then over; the
 * child, which runs next, sees TIOCOUTQ at 0 and sends SIGUSR1 before the
 * drain runs again. The drain must fail with EINTR, as Linux's TCSBRK
 * tests for a signal after its wait, though its output is out. khvcd and
 * this process are SCHED_OTHER again after. Returns 0 when the row held.
 */
static int late_signal_row(int fd)
{
    pid_t khvcd = pid_named("khvcd"), child;
    int go[2], rc, err, status;

    if (khvcd < 0) {
        errno = 0;
        return fail("khvcd is not in /proc");
    }
    if (pipe(go) != 0 || set_policy_of(khvcd, SCHED_FIFO, 99) != 0 ||
        set_policy(SCHED_FIFO, 99) != 0) {
        err = errno;
        set_policy_of(khvcd, SCHED_OTHER, 0);
        errno = err;
        return fail("the late signal's drain cannot be set up");
    }
    drain_signals = 0;
    child = fork();
    if (child == 0) {
        int queued = 1;
        char c;

        close(go[1]);
        if (read(go[0], &c, 1) != 1) {
            _exit(1);
        }
        while (ioctl(fd, TIOCOUTQ, &queued) == 0 && queued != 0) {
            sched_yield();
        }
        kill(getppid(), SIGUSR1);
        _exit(queued == 0 ? 0 : 1);
    }
    close(go[0]);
    rc = child < 0 || write(fd, "late\n", 5) != 5 || write(go[1], "g", 1) != 1 ? -2 : tcdrain(fd);
    err = errno;
    if (child > 0) {
        waitpid(child, &status, 0);
    }
    close(go[1]);
    set_policy(SCHED_OTHER, 0);
    set_policy_of(khvcd, SCHED_OTHER, 0);
    errno = err;
    if (rc == -2) {
        return fail("the late signal's drain does not start");
    }
    if (rc != -1 || err != EINTR || drain_signals != 1) {
        return fail("a drain whose output went out as SIGUSR1 came does not fail with EINTR");
    }
    return 0;
}

/** The state letter of process `pid` in /proc/PID/stat, or 0 once it is
 * gone. */
static char proc_state(pid_t pid)
{
    char path[48], text[512];
    const char *p;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (slurp(path, text, sizeof(text)) <= 0 || (p = strrchr(text, ')')) == NULL) {
        return 0;
    }
    return p[1] == ' ' ? p[2] : 0;
}

/** Wait until process `pid` sleeps (state S) or has exited, which a reap
 * with WNOHANG tells; 1 when it sleeps, 0 when it exited, with its status
 * in `*status`. */
static int sleeps(pid_t pid, int *status)
{
    for (;;) {
        if (waitpid(pid, status, WNOHANG) == pid) {
            return 0;
        }
        if (proc_state(pid) == 'S') {
            return 1;
        }
        sched_yield();
    }
}

/* The lines of the blocked write: three ring's worth of output with ONLCR. */
#define LOCK_LINES 4096

/** Read `lines` newlines of the master `master`; the count of them a
 * carriage return came before. */
static int crs_before(int master, int lines)
{
    int newlines = 0, crs = 0, n, i;
    char in[512], prev = 0;

    while (newlines < lines && (n = (int)read(master, in, sizeof(in))) > 0) {
        for (i = 0; i < n; i++) {
            if (in[i] == '\n') {
                newlines++;
                crs += prev == '\r';
            }
            prev = in[i];
        }
    }
    return newlines == lines ? crs : -1;
}

/** Reap the child `pid`; 1 when it exited with 0. */
static int exited_well(pid_t pid)
{
    int status;

    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * A termios change against a blocked write, on the pty pair `master` and
 * `slave`: a child's write of LOCK_LINES lines of `a` and a newline, which
 * fills the master's 4096-byte ring and waits for room, holds the slave's
 * write lock; a second child's tcsetattr(TCSADRAIN) that clears ONLCR then
 * waits for the write to end, as Linux's set_termios takes the write lock,
 * so every newline of the write reaches the master as CR LF; and the next
 * write is under the new modes, a bare newline. Returns 0 when the row
 * held.
 */
static int write_lock_row(int master, int slave)
{
    static char lines[LOCK_LINES * 2];
    struct termios modes;
    pid_t writer, changer;
    int status, i, crs;
    char in[4];

    for (i = 0; i < LOCK_LINES; i++) {
        lines[2 * i] = 'a';
        lines[2 * i + 1] = '\n';
    }
    if (tcgetattr(slave, &modes) != 0 || !(modes.c_oflag & OPOST) || !(modes.c_oflag & ONLCR)) {
        return fail("the slave has no OPOST and ONLCR to begin with");
    }
    writer = fork();
    if (writer == 0) {
        _exit(write(slave, lines, sizeof(lines)) == (ssize_t)sizeof(lines) ? 0 : 1);
    }
    if (writer < 0 || !sleeps(writer, &status)) {
        return fail("the slave's writer does not wait for the master to read");
    }
    changer = fork();
    if (changer == 0) {
        modes.c_oflag &= ~(tcflag_t)ONLCR;
        _exit(tcsetattr(slave, TCSADRAIN, &modes) == 0 ? 0 : 1);
    }
    if (changer < 0 || !sleeps(changer, &status)) {
        kill(writer, SIGKILL);
        waitpid(writer, &status, 0);
        errno = 0;
        return fail("tcsetattr(TCSADRAIN) did not wait for the write in progress");
    }
    crs = crs_before(master, LOCK_LINES);
    if (!exited_well(writer) || !exited_well(changer)) {
        return fail("the writer or the termios change failed");
    }
    if (crs != LOCK_LINES) {
        errno = 0;
        return fail("the termios change landed inside the write");
    }
    if (write(slave, "b\n", 2) != 2 || read(master, in, sizeof(in)) != 2 ||
        memcmp(in, "b\n", 2) != 0) {
        return fail("the write after the change is not under the new modes");
    }
    modes.c_oflag |= ONLCR;
    return tcsetattr(slave, TCSANOW, &modes) == 0 ? 0 : fail("ONLCR does not come back");
}

/**
 * The job-control stop of a drain, on the pty `slave`: a child that makes
 * the slave its session's terminal starts a grandchild in a group of its
 * own, in the background, whose tcdrain takes SIGTTOU and stops; the child
 * gives the grandchild's group the foreground and continues it, and the
 * drain, restarted as Linux restarts tty_check_change's ERESTARTSYS,
 * returns 0. The child's exit status is the row's: 0, or the step that
 * failed. Returns 0 when the row held.
 */
static int ttou_restart_row(int slave)
{
    pid_t child = fork(), drainer;
    int status;
    sigset_t ttou;

    if (child == 0) {
        sigemptyset(&ttou);
        sigaddset(&ttou, SIGTTOU);
        if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) != 0) {
            _exit(2);
        }
        drainer = fork();
        if (drainer == 0) {
            signal(SIGTTOU, SIG_DFL);
            sigprocmask(SIG_UNBLOCK, &ttou, NULL);
            setpgid(0, 0);
            _exit(tcdrain(slave) == 0 ? 0 : errno == EINTR ? 3 : 4);
        }
        if (drainer < 0) {
            _exit(5);
        }
        setpgid(drainer, drainer);
        if (waitpid(drainer, &status, WUNTRACED) != drainer || !WIFSTOPPED(status) ||
            WSTOPSIG(status) != SIGTTOU) {
            _exit(6);
        }
        sigprocmask(SIG_BLOCK, &ttou, NULL);
        if (tcsetpgrp(slave, drainer) != 0 || kill(drainer, SIGCONT) != 0) {
            _exit(7);
        }
        if (waitpid(drainer, &status, 0) != drainer || !WIFEXITED(status)) {
            _exit(8);
        }
        _exit(WEXITSTATUS(status));
    }
    if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return fail("the job-control drain's child does not end");
    }
    if (WEXITSTATUS(status) != 0) {
        char why[96];

        snprintf(why, sizeof(why),
                 "a drain stopped by SIGTTOU and continued does not return 0: "
                 "step %d",
                 WEXITSTATUS(status));
        errno = 0;
        return fail(why);
    }
    return 0;
}

/* Linux's struct termio, of TCGETA and the TCSETA family, which musl does
 * not declare. */
struct termio_v {
    unsigned short c_iflag;
    unsigned short c_oflag;
    unsigned short c_cflag;
    unsigned short c_lflag;
    unsigned char c_line;
    unsigned char c_cc[8];
};

/**
 * The termio forms on the pty `slave` fed by `master`: TCGETA gives the low
 * 16 bits of each mode; TCSETAW with ECHO cleared installs it and keeps a
 * high bit of c_cflag, CRTSCTS, that tcsetattr set; TCSETAF also discards
 * the input the master fed. The modes are restored. Returns 0 when the row
 * held.
 */
static int termio_row(int master, int slave)
{
    struct termios orig, modes;
    struct termio_v v;
    int avail = -1;

    if (tcgetattr(slave, &orig) != 0) {
        return fail("tcgetattr of the slave fails");
    }
    modes = orig;
    modes.c_cflag |= CRTSCTS;
    if (tcsetattr(slave, TCSANOW, &modes) != 0 || ioctl(slave, TCGETA, &v) != 0) {
        return fail("TCGETA fails");
    }
    if (v.c_lflag != (unsigned short)modes.c_lflag || v.c_cflag != (unsigned short)modes.c_cflag ||
        v.c_cc[VINTR] != modes.c_cc[VINTR]) {
        errno = 0;
        return fail("TCGETA does not give the low bits of the modes");
    }
    v.c_lflag &= (unsigned short)~ECHO;
    if (ioctl(slave, TCSETAW, &v) != 0 || tcgetattr(slave, &modes) != 0) {
        return fail("TCSETAW fails");
    }
    if ((modes.c_lflag & ECHO) || !(modes.c_cflag & CRTSCTS)) {
        errno = 0;
        return fail("TCSETAW does not set the low bits and keep the high ones");
    }
    if (write(master, "flush\n", 6) != 6 || ioctl(slave, FIONREAD, &avail) != 0 || avail <= 0) {
        return fail("the slave does not have the master's line");
    }
    if (ioctl(slave, TCSETAF, &v) != 0 || ioctl(slave, FIONREAD, &avail) != 0 || avail != 0) {
        errno = 0;
        return fail("TCSETAF does not discard the input");
    }
    return tcsetattr(slave, TCSANOW, &orig) == 0 ? 0 : fail("the slave's modes do not come back");
}

/**
 * The drain's requests on a pty pair: the master and the slave count no
 * output in TIOCOUTQ and drain at once (`tcdrain`), as Linux's pty has no
 * chars_in_buffer; then `termio_row`, `write_lock_row` and
 * `ttou_restart_row`. Returns 0 when each row held.
 */
static int pty_rows(void)
{
    int master, slave, queued = -1;

    if (openpty(&master, &slave, NULL, NULL, NULL) != 0) {
        return fail("openpty fails");
    }
    if (tcdrain(master) != 0 || tcdrain(slave) != 0) {
        return fail("tcdrain of a pty pair fails");
    }
    if (ioctl(master, TIOCOUTQ, &queued) != 0 || queued != 0 ||
        ioctl(slave, TIOCOUTQ, &queued) != 0 || queued != 0) {
        return fail("TIOCOUTQ of a pty pair does not read 0");
    }
    if (termio_row(master, slave) != 0 || write_lock_row(master, slave) != 0 ||
        ttou_restart_row(slave) != 0) {
        return 1;
    }
    close(slave);
    close(master);
    return 0;
}

/** The rows of `tty`; 0 when each held. */
static int tty_rows(void)
{
    struct termios modes;
    int fd, queued;

    settle(CONSOLE_NODE, 1);
    if (char_dev(CONSOLE_NODE, 229u) != makedev(229u, 0u)) {
        return fail(CONSOLE_NODE " is not the character device 229:0");
    }
    fd = open(CONSOLE_NODE, O_RDWR | O_NOCTTY);
    if (fd < 0 || !isatty(fd) || tcgetattr(fd, &modes) != 0) {
        return fail(CONSOLE_NODE " is no terminal");
    }
    if (write(fd, "hvc-tty\n", 8) != 8) {
        return fail("the terminal does not take a line");
    }
    if (tcdrain(fd) != 0) {
        return fail("tcdrain of the terminal fails");
    }
    if (ioctl(fd, TIOCOUTQ, &queued) != 0 || queued != 0) {
        return fail("TIOCOUTQ does not read 0 after tcdrain");
    }
    if (drain_rows(fd) != 0 || late_signal_row(fd) != 0 || pty_rows() != 0) {
        return 1;
    }
    close(fd);
    fd = open("/dev/vport0p0", O_RDWR);
    if (fd >= 0 || errno != ENXIO) {
        return fail("the console port's vport node does not refuse an open with ENXIO");
    }
    return 0;
}

/** The rows of `late`; 0 when each held. */
static int late_rows(void)
{
    struct pollfd p;
    dev_t dev;
    char in[8];
    int fd, n, got = 0;

    settle(LATE_LINK, 1);
    dev = char_dev(LATE_NODE, PORT_MAJOR);
    if (dev == 0 || char_dev(LATE_LINK, PORT_MAJOR) != dev) {
        return fail(LATE_NODE " and " LATE_LINK " are not one device of major 245");
    }
    fd = open(LATE_LINK, O_RDWR);
    if (fd < 0) {
        return fail("the late port does not open");
    }
    if (write(fd, "ping\n", 5) != 5) {
        return fail("the late port does not take a line");
    }
    while (got < 5 && (n = (int)read(fd, in + got, (size_t)(5 - got))) > 0) {
        got += n;
    }
    if (got != 5 || memcmp(in, "pong\n", 5) != 0) {
        errno = 0;
        return fail("the late port does not give the host's line");
    }
    p.fd = fd;
    p.events = POLLOUT;
    p.revents = 0;
    if (poll(&p, 1, 0) != 1 || !(p.revents & POLLOUT)) {
        errno = 0;
        return fail("the late port does not poll writable");
    }
    close(fd);
    return 0;
}

/** The rows of `gone`; 0 when each held. */
static int gone_rows(void)
{
    settle(LATE_LINK, 0);
    if (!absent(LATE_NODE)) {
        return fail("a node of the late port is there after its removal");
    }
    if (char_dev(BOOT_LINK, PORT_MAJOR) == 0) {
        return fail("the boot port's node went with the late port");
    }
    return 0;
}

/** Run the rows of the machine case's mode `mode`; 0 when each held, 2
 * for another mode. */
static int machine_mode(const char *mode)
{
    int r;

    if (strcmp(mode, "boot") == 0) {
        r = boot_rows();
    } else if (strcmp(mode, "tty") == 0) {
        r = tty_rows();
    } else if (strcmp(mode, "late") == 0) {
        r = late_rows();
    } else if (strcmp(mode, "gone") == 0) {
        r = gone_rows();
    } else if (strcmp(mode, "long") == 0) {
        r = long_rows();
    } else {
        fprintf(stderr, "usage: hvcmusl vport0p1|ttyS0|boot|tty|late|gone|long\n");
        return 2;
    }
    if (r == 0) {
        printf("HVCMUSL OK\n");
    }
    return r;
}

/** Run the rows of the console or the mode `argv[1]` names; 0 when each
 * held, 2 for another argument. */
int main(int argc, char **argv)
{
    static const char *const port_plan[] = {"named", "bound", "node", "listed", "terminal", "log"};
    static const char *const uart_plan[] = {"named", "absent"};
    int port = argc == 2 && strcmp(argv[1], "vport0p1") == 0;
    unsigned i;

    if (!port && !(argc == 2 && strcmp(argv[1], "ttyS0") == 0)) {
        if (argc != 2) {
            fprintf(stderr, "usage: hvcmusl vport0p1|ttyS0|boot|tty|late|gone|long\n");
            return 2;
        }
        return machine_mode(argv[1]);
    }
    reporting = 1;
    report_begin(report_case(environ, port ? "guest.hvc.port" : "guest.hvc.uart"));
    for (i = 0; port && i < sizeof(port_plan) / sizeof(port_plan[0]); i++) {
        report_plan(port_plan[i]);
    }
    for (i = 0; !port && i < sizeof(uart_plan) / sizeof(uart_plan[0]); i++) {
        report_plan(uart_plan[i]);
    }
    if ((port ? port_rows() : uart_rows()) != 0) {
        return 1;
    }
    report_end();
    printf("HVCMUSL OK\n");
    return 0;
}
