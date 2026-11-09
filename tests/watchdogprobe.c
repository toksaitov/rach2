/* Exercise /dev/watchdog, then leave its live descriptor unfed for QMP. */
#include "probe.h"

/** Write one fixed diagnostic or progress token to the console. */
static void say(const char *text)
{
    unsigned n = 0;

    while (text[n]) {
        n++;
    }
    syscall3(SYS_WRITE, 1, (long)text, n);
}

/** Halt the timer on a failed check so the failure remains readable. */
static void failed(long fd)
{
    if (fd >= 0) {
        syscall3(SYS_WRITE, fd, (long)"V", 1);
        syscall1(SYS_CLOSE, fd);
    }
    say("WATCHDOGPROBE FAIL\n");
    syscall1(SYS_EXIT, 1);
    for (;;) {
    }
}

void _start(void);
/** Check exclusivity, support, timeout and feed, then retain the open
 * descriptor while sleeping. The host sets QEMU's watchdog action to pause. */
PROBE_START void _start(void)
{
    struct {
        unsigned options, firmware;
        char name[32];
    } info;
    long fd = syscall3(SYS_OPEN, (long)"/dev/watchdog", 1, 0);
    long second;
    int seconds = 3, left = -1;

    if (fd < 0) {
        failed(fd);
    }
    second = syscall3(SYS_OPEN, (long)"/dev/watchdog", 1, 0);
    if (second >= 0) {
        syscall1(SYS_CLOSE, second);
        failed(fd);
    }
    if (second != -16 || syscall3(SYS_IOCTL, fd, 0x80285700u, (long)&info) != 0 ||
        info.options != 0x8180 || info.firmware != 2 ||
        syscall3(SYS_IOCTL, fd, 0xC0045706u, (long)&seconds) != 0 || seconds != 3 ||
        syscall3(SYS_IOCTL, fd, 0x80045705u, 0) != 0 ||
        syscall3(SYS_IOCTL, fd, 0x8004570Au, (long)&left) != 0 || left < 0 || left > 3) {
        failed(fd);
    }
    say("WATCHDOGPROBE ARMED\n");
    for (;;) {
        syscall0(SYS_PAUSE);
    }
}
