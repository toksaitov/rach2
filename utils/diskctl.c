/**
 * Ask the kernel to read a disk's partition table again: `diskctl reread DEV`.
 *
 * A program that writes a whole disk changes the table under a kernel that
 * read the old one at boot, and the nodes for the partitions it just made do
 * not exist until the kernel is told. That is `BLKRRPART` on the whole-disk
 * node, and the kernel refuses it while anything is mounted from the disk or
 * another descriptor is open on it, which is what keeps a reread from moving
 * the windows under a file system.
 *
 * `growroot` does this as one of its steps, and an installer that does not
 * grow needs it on its own; this is that step by itself and nothing else.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BLKRRPART 0x125Fu

int main(int argc, char **argv)
{
    int fd;

    if (argc != 3 || strcmp(argv[1], "reread") != 0) {
        fprintf(stderr, "usage: diskctl reread DEVICE\n");
        return 2;
    }
    fd = open(argv[2], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "diskctl: cannot open %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    if (ioctl(fd, BLKRRPART, 0) != 0) {
        int e = errno;

        close(fd);
        if (e == EBUSY) {
            fprintf(stderr,
                    "diskctl: %s is in use: unmount everything on it and close "
                    "every program holding it\n",
                    argv[2]);
        } else if (e == ENOTTY) {
            fprintf(stderr, "diskctl: %s is not a whole disk\n", argv[2]);
        } else {
            fprintf(stderr, "diskctl: %s: %s\n", argv[2], strerror(e));
        }
        return 1;
    }
    close(fd);
    printf("diskctl: %s: the table was read again\n", argv[2]);
    return 0;
}
