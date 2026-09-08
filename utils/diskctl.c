/**
 * The two things a program that has just written a whole disk needs:
 * `diskctl reread DEV` and `diskctl id DEV`.
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
 *
 * `id` prints the disk's MBR signature, the four bytes at offset 440 of its
 * first sector, as eight lowercase hexadecimal digits. That is how every
 * program here names a disk that has just been written: `growroot` takes
 * `MBRID=ID,N`, and the identity is the image's own rather than a position
 * that another disk of the machine could also have.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BLKRRPART 0x125Fu

/** Print the MBR signature of the disk open on `fd`, or report why not. */
static int print_id(int fd, const char *path)
{
    unsigned char sector[512];
    ssize_t n;
    unsigned long id;

    n = read(fd, sector, sizeof(sector));
    if (n != (ssize_t)sizeof(sector)) {
        fprintf(stderr, "diskctl: %s: its first sector did not read\n", path);
        return 1;
    }
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        fprintf(stderr, "diskctl: %s: its first sector is not a table\n", path);
        return 1;
    }
    id = (unsigned long)sector[440] | ((unsigned long)sector[441] << 8) |
         ((unsigned long)sector[442] << 16) | ((unsigned long)sector[443] << 24);
    printf("%08lx\n", id);
    return 0;
}

int main(int argc, char **argv)
{
    int fd;
    int reading;

    reading = argc == 3 && strcmp(argv[1], "id") == 0;
    if (!reading && (argc != 3 || strcmp(argv[1], "reread") != 0)) {
        fprintf(stderr, "usage: diskctl reread DEVICE | diskctl id DEVICE\n");
        return 2;
    }
    fd = open(argv[2], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "diskctl: cannot open %s: %s\n", argv[2], strerror(errno));
        return 1;
    }
    if (reading) {
        int rc = print_id(fd, argv[2]);

        close(fd);
        return rc;
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
