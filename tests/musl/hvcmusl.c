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
 * Prints `HVCMUSL OK` and exits 0 on success, and prints `HVCMUSL FAIL
 * (detail)` and exits 1 otherwise. etude's report (probe_report.h), for the
 * case ETUDE_CASE names, has the assertions `named`, `bound`, `node`,
 * `listed`, `terminal` and `log` with `vport0p1`, and `named` and `absent`
 * with `ttyS0`. The first failure fails its assertion and leaves the rest
 * not run.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

/* The kernel's log, read whole: the ring is 128 KiB. */
static char ring[131072 + 1];

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    char why[96];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("HVCMUSL FAIL (%s)\n", why);
    report_stop(why);
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

/** Run the rows of the console `argv[1]` names; 0 when each held, 2 for
 * another argument. */
int main(int argc, char **argv)
{
    static const char *const port_plan[] = {"named", "bound", "node", "listed", "terminal", "log"};
    static const char *const uart_plan[] = {"named", "absent"};
    int port = argc == 2 && strcmp(argv[1], "vport0p1") == 0;
    unsigned i;

    if (!port && !(argc == 2 && strcmp(argv[1], "ttyS0") == 0)) {
        fprintf(stderr, "usage: hvcmusl vport0p1|ttyS0\n");
        return 2;
    }
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
