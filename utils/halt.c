/**
 * halt: stop the machine.
 *
 * `halt [-p|-r]`: power off by default, `-p` to say so, `-r` to restart
 * instead. Root's, because `reboot(2)` is.
 *
 * It exists for the images that carry no busybox. os-simple stages the `utils`
 * sweep and the `ish` shell and nothing else, so without this it would be the
 * one shipped image with a system call to stop the machine and no program that
 * makes it. os-real does not stage it: busybox's standalone shell dispatches
 * `halt`, `poweroff` and `reboot` as applets before it looks at the file
 * system, so a binary of the same name there would be one nobody reaches, and a
 * second implementation of the same command is worse for a reader than none.
 *
 * There is nothing to tear down first. The kernel flushes the page cache and
 * clears the dirty-mount flag inside the call, with scheduling already off, so
 * a program that tried to stop its neighbours politely would only be adding a
 * scheduling policy of its own to the last moment of the machine's life.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/reboot.h>

/**
 * Ask the kernel to stop the machine. Returns only on failure.
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
