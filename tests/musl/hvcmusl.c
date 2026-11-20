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
 * the console port's vport node, refuses an open with ENXIO. `late`:
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
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

/** The rows of `tty`; 0 when each held. */
static int tty_rows(void)
{
    struct termios modes;
    int fd;

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
