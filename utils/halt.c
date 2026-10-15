/**
 * halt: stop the machine.
 *
 * `halt [-p|-r]`: power off, as `-p` also asks, or restart with `-r`. Only
 * root can use it, since `reboot(2)` returns `EPERM` to anyone else.
 *
 * It is for the images without BusyBox. os-simple is built from this
 * project's own code alone and stages it; without it, that image would have
 * the system call that stops the machine and no program that makes it. The
 * BusyBox images do not stage it: BusyBox's standalone shell runs `halt`,
 * `poweroff` and `reboot` as applets before it searches the file system, so a
 * binary of the same name there would never run.
 *
 * It stops no process first: the kernel does the shutdown inside the call,
 * with scheduling off (`kernel_shutdown` in rach2/kernel.c). It writes back a
 * KRFFS root on a disk and clears its dirty-mount flag, unmounts a root of
 * another type, or writes back the page cache, and then stops the USB
 * controller and the network cards.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/reboot.h>

/**
 * Stop the machine with `reboot(2)`, which does not return when it succeeds.
 * The status is 1 on a usage error or a failed call, which it reports.
 */
int main(int argc, char **argv)
{
    int command = RB_POWER_OFF;

    if (argc > 2) {
        fprintf(stderr, "usage: halt [-p|-r]\n");

        return 1;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "-r") == 0) {
            command = RB_AUTOBOOT;
        } else if (strcmp(argv[1], "-p") != 0) {
            fprintf(stderr, "usage: halt [-p|-r]\n");

            return 1;
        }
    }

    reboot(command);
    fprintf(stderr, "halt: %s\n", strerror(errno));

    return 1;
}
