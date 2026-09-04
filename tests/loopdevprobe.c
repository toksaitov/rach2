/**
 * loopdevprobe: a file system in a file, through a loop device.
 *
 * The disk's one partition is a FAT32 volume carrying two images: a FAT32 one
 * and an ext2 one, each with a file of known bytes in it. The probe mounts
 * that volume, attaches an image to /dev/loop0 through /dev/loop-control,
 * mounts the device, reads what is in it, writes a file, unmounts, detaches,
 * attaches again and reads back what it wrote, which is what says the write
 * reached the file and not only the cache.
 *
 * It then asks for everything the attachment refuses: the same file twice, a
 * write and a truncation of an attached file, a detach with a mount on it, an
 * unmount of the volume holding the file, a directory, and a file whose
 * length is not whole sectors.
 *
 * `loopdevprobe pull` is the stick's row: the image lives on a volume of a
 * stick, and the harness pulls the stick while the image is mounted. What the
 * machine owes then is an error from the read in flight, an unmount of the
 * mount over the device that has gone, and an unmount of the volume the file
 * was on, since the attachment let go of it when its medium went.
 *
 * `loopdevprobe vfat`, `ext2` and `krffs` name the type inside the image.
 * Prints "LOOPPROBE OK (type)" or "LOOPPROBE FAIL: reason". Freestanding;
 * int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_UNLINK    10
#define SYS_MKDIR     39
#define SYS_MOUNT     21
#define SYS_UMOUNT2   52
#define SYS_FTRUNCATE 93
#define SYS_FSYNC     118
#define SYS_STAT64    195

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define MS_RDONLY 1

#define ENOENT 2
#define EBUSY  16
#define EINVAL 22
#define EROFS  30
#define ELOOP  40

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
    sys1(SYS_EXIT, status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    sys3(SYS_WRITE, 1, (int)s, (int)strlen(s));
    sys3(SYS_WRITE, 1, (int)"\n", 1);
}

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    sys3(SYS_WRITE, 1, (int)"LOOPPROBE FAIL: ", 16);
    puts(why);
    sys_exit(1);
}

static void fail_with(const char *why, int n) __attribute__((noreturn));
static void fail_with(const char *why, int n)
{
    char digits[12];
    int i = 11;
    unsigned v = n < 0 ? (unsigned)-n : (unsigned)n;

    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    if (n < 0) {
        digits[--i] = '-';
    }
    sys3(SYS_WRITE, 1, (int)"LOOPPROBE FAIL: ", 16);
    sys3(SYS_WRITE, 1, (int)why, (int)strlen(why));
    sys3(SYS_WRITE, 1, (int)" (got ", 6);
    sys3(SYS_WRITE, 1, (int)(digits + i), (int)strlen(digits + i));
    puts(")");
    sys_exit(1);
}

static int streq(const char *a, const char *b)
{
    size_t i;

    for (i = 0; a[i] && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

/** One command to /dev/loop-control; the errno, or 0. */
static int control(const char *line)
{
    int fd = sys3(SYS_OPEN, (int)"/dev/loop-control", O_WRONLY, 0);
    int rc;

    if (fd < 0) {
        fail_with("opening /dev/loop-control", fd);
    }
    rc = sys3(SYS_WRITE, fd, (int)line, (int)strlen(line));
    sys1(SYS_CLOSE, fd);
    return rc < 0 ? rc : 0;
}

/** What /proc/loop says, into `buffer`. */
static int loop_lines(char *buffer, int cap)
{
    int fd = sys3(SYS_OPEN, (int)"/proc/loop", O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail_with("opening /proc/loop", fd);
    }
    n = sys3(SYS_READ, fd, (int)buffer, cap - 1);
    sys1(SYS_CLOSE, fd);
    if (n < 0) {
        fail_with("reading /proc/loop", n);
    }
    buffer[n] = '\0';
    return n;
}

static int contains(const char *hay, const char *needle)
{
    int i;
    int j;

    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] == needle[j]; j++) {
        }
        if (!needle[j]) {
            return 1;
        }
    }
    return 0;
}

/** Read a file whole and compare it with `want`. */
static void expect_file(const char *path, const char *want)
{
    char buffer[256];
    int fd = sys3(SYS_OPEN, (int)path, O_RDONLY, 0);
    int n;
    int i;

    if (fd < 0) {
        fail_with("opening a file in the image", fd);
    }
    n = sys3(SYS_READ, fd, (int)buffer, (int)sizeof(buffer) - 1);
    sys1(SYS_CLOSE, fd);
    if (n < 0) {
        fail_with("reading a file in the image", n);
    }
    buffer[n] = '\0';
    for (i = 0; want[i]; i++) {
        if (buffer[i] != want[i]) {
            fail_with("a byte of a file in the image", i);
        }
    }
}

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *type = argc > 1 ? argv[1] : "vfat";
    const char *image = streq(type, "ext2")    ? "/m/ext2.img"
                        : streq(type, "krffs") ? "/m/krffs.img"
                                               : "/m/fat.img";
    char attach[64];
    char lines[512];
    int fd;
    int rc;
    int i;

    sys2(SYS_MKDIR, (int)"/m", 0755);
    sys2(SYS_MKDIR, (int)"/l", 0755);

    if (streq(type, "pull")) {
        int reads = 0;

        rc = sys5(SYS_MOUNT, (int)"/dev/usb0p1", (int)"/m", (int)"vfat", 0, 0);
        if (rc != 0) {
            fail_with("mounting the stick's volume", rc);
        }
        rc = control("attach 0 /m/fat.img");
        if (rc != 0) {
            fail_with("attaching an image on a stick", rc);
        }
        rc = sys5(SYS_MOUNT, (int)"/dev/loop0", (int)"/l", (int)"vfat", 0, 0);
        if (rc != 0) {
            fail_with("mounting an image on a stick", rc);
        }
        expect_file("/l/hello.txt", "hello from inside an image");
        puts("LOOPPROBE HOLDING");

        /* Read until the stick is not there any more. */
        for (;;) {
            int fd2 = sys3(SYS_OPEN, (int)"/l/hello.txt", O_RDONLY, 0);

            if (fd2 < 0) {
                break;
            }
            {
                char one[8];
                int got = sys3(SYS_READ, fd2, (int)one, (int)sizeof(one));

                sys1(SYS_CLOSE, fd2);
                if (got < 0) {
                    break;
                }
            }
            if (++reads > 100000) {
                fail("the stick was never pulled");
            }
        }

        rc = sys2(SYS_UMOUNT2, (int)"/l", 0);
        if (rc != 0) {
            fail_with("unmounting an image whose stick has gone", rc);
        }
        rc = control("detach 0");
        if (rc != 0 && rc != -ENOENT) {
            fail_with("detaching a device whose stick has gone", rc);
        }
        rc = sys2(SYS_UMOUNT2, (int)"/m", 0);
        if (rc != 0) {
            fail_with("unmounting the volume of a stick that has gone", rc);
        }
        puts("LOOPPROBE OK (pull)");
        sys_exit(0);
    }

    rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/m", (int)"vfat", 0, 0);
    if (rc != 0) {
        fail_with("mounting the volume that holds the images", rc);
    }

    loop_lines(lines, (int)sizeof(lines));
    if (lines[0] != '\0') {
        fail("/proc/loop names a device before anything is attached");
    }

    /* "attach 0 " and the image's path. */
    for (i = 0; i < 9; i++) {
        attach[i] = "attach 0 "[i];
    }
    for (i = 0; image[i]; i++) {
        attach[9 + i] = image[i];
    }
    attach[9 + i] = '\0';

    rc = control(attach);
    if (rc != 0) {
        fail_with("attaching the image", rc);
    }
    loop_lines(lines, (int)sizeof(lines));
    if (!contains(lines, "loop0 ") || !contains(lines, image)) {
        fail("/proc/loop does not name what was attached");
    }

    rc = sys5(SYS_MOUNT, (int)"/dev/loop0", (int)"/l", (int)type, 0, 0);
    if (rc != 0) {
        fail_with("mounting the image through the device", rc);
    }
    expect_file("/l/hello.txt", "hello from inside an image");

    /* Everything the attachment refuses. */
    {
        /* The same file, asked for on another device: one file, one device,
           whatever it is called. */
        char again[64];
        int at;

        for (at = 0; at < 9; at++) {
            again[at] = "attach 1 "[at];
        }
        for (at = 0; image[at]; at++) {
            again[9 + at] = image[at];
        }
        again[9 + at] = '\0';
        if (control(again) != -EBUSY) {
            fail("the same file was attached twice");
        }
    }
    /* Another file, on the device that is taken: the slot is the reason, not
       the file. */
    if (control("attach 0 /m/odd2.img") != -EBUSY) {
        fail("the device that is taken took another file");
    }
    fd = sys3(SYS_OPEN, (int)image, O_WRONLY, 0);
    if (fd < 0) {
        fail_with("opening a file that backs a device", fd);
    }
    rc = sys3(SYS_WRITE, fd, (int)"x", 1);
    if (rc != -EBUSY) {
        fail_with("writing a file that backs a device", rc);
    }
    rc = sys2(SYS_FTRUNCATE, fd, 0);
    if (rc != -EBUSY) {
        fail_with("truncating a file that backs a device", rc);
    }
    sys1(SYS_CLOSE, fd);
    if (control("detach 0") != -EBUSY) {
        fail("a device with a mount on it was detached");
    }
    if (sys2(SYS_UMOUNT2, (int)"/m", 0) != -EBUSY) {
        fail("the volume holding an attached file was unmounted");
    }
    if (control("attach 1 /m") != -EINVAL) {
        fail("a directory was attached");
    }
    if (control("attach 1 /m/odd.bin") != -EINVAL) {
        fail("a file whose length is not whole sectors was attached");
    }
    /* A device on a file that lives on a device on a file: deeper than the
       four buffers a process may hold can serve, and refused for it. */
    if (control("attach 1 /l/inner.bin") != -ELOOP) {
        fail("a device was made to stand on another one");
    }

    /* What the control node is not: a command with no number, one with a
       number that is not a device, and one with something after it. */
    if (control("detach x") != -EINVAL) {
        fail("a detach with no number was taken");
    }
    if (control("detach 0junk") != -EINVAL) {
        fail("a detach with something after the number was taken");
    }
    if (control("attach 9 /m/fat.img") != -EINVAL) {
        fail("a device that does not exist was attached");
    }
    if (control("attach 4294967296 /m/fat.img") != -EINVAL) {
        fail("a number larger than any device was attached");
    }
    if (control("nonsense 0 /m/fat.img") != -EINVAL) {
        fail("a command that is not one was taken");
    }

    /* A device that will not write: the mount over it is read-only whatever
       the mount asked for, and /proc/loop says which. */
    if (control("attach ro 1 /m/odd2.img") != 0) {
        fail("attaching a device that will not write");
    }
    loop_lines(lines, (int)sizeof(lines));
    if (!contains(lines, "loop1 ") || !contains(lines, " ro ")) {
        fail("/proc/loop does not say the device will not write");
    }
    sys2(SYS_MKDIR, (int)"/r", 0755);
    rc = sys5(SYS_MOUNT, (int)"/dev/loop1", (int)"/r", (int)"vfat", 0, 0);
    if (rc != 0) {
        fail_with("mounting a device that will not write", rc);
    }
    rc = sys3(SYS_OPEN, (int)"/r/made.txt", O_WRONLY | O_CREAT, 0644);
    if (rc != -EROFS) {
        fail_with("creating a file on a device that will not write", rc);
    }
    rc = sys2(SYS_UMOUNT2, (int)"/r", 0);
    if (rc != 0) {
        fail_with("unmounting the device that will not write", rc);
    }
    if (control("detach 1") != 0) {
        fail("detaching the device that will not write");
    }

    /* A write through the mount, then the same bytes read back from the file
       after a detach and a fresh attachment. */
    fd = sys3(SYS_OPEN, (int)"/l/made.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("creating a file inside the image", fd);
    }
    if (sys3(SYS_WRITE, fd, (int)"written through a loop device\n", 30) != 30) {
        fail("writing inside the image");
    }
    if (sys1(SYS_FSYNC, fd) != 0) {
        fail("fsync inside the image");
    }
    sys1(SYS_CLOSE, fd);

    rc = sys2(SYS_UMOUNT2, (int)"/l", 0);
    if (rc != 0) {
        fail_with("unmounting the image", rc);
    }
    rc = control("detach 0");
    if (rc != 0) {
        fail_with("detaching", rc);
    }
    loop_lines(lines, (int)sizeof(lines));
    if (lines[0] != '\0') {
        fail("/proc/loop still names a device after the detach");
    }

    rc = control(attach);
    if (rc != 0) {
        fail_with("attaching the image again", rc);
    }
    rc = sys5(SYS_MOUNT, (int)"/dev/loop0", (int)"/l", (int)type, MS_RDONLY, 0);
    if (rc != 0) {
        fail_with("mounting the image again", rc);
    }
    expect_file("/l/made.txt", "written through a loop device\n");
    rc = sys2(SYS_UMOUNT2, (int)"/l", 0);
    if (rc != 0) {
        fail_with("unmounting the image the second time", rc);
    }
    if (control("detach 0") != 0) {
        fail("detaching the second time");
    }

    /* The file the device stood on is an ordinary file again. */
    fd = sys3(SYS_OPEN, (int)image, O_WRONLY, 0);
    if (fd < 0) {
        fail_with("opening the image after the detach", fd);
    }
    sys1(SYS_CLOSE, fd);

    /* The same image, copied onto the root, which is memory and has no
       device under it at all: a loop device reads its file through the file
       system holding it, so where that file lives does not matter. */
    {
        char block[4096];
        int from = sys3(SYS_OPEN, (int)image, O_RDONLY, 0);
        int to = sys3(SYS_OPEN, (int)"/ram.img", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int n;

        if (from < 0 || to < 0) {
            fail("opening the image to copy it onto the root");
        }
        while ((n = sys3(SYS_READ, from, (int)block, (int)sizeof(block))) > 0) {
            if (sys3(SYS_WRITE, to, (int)block, n) != n) {
                fail("copying the image onto the root");
            }
        }
        sys1(SYS_CLOSE, from);
        sys1(SYS_CLOSE, to);
        if (n < 0) {
            fail_with("reading the image to copy it", n);
        }
    }
    rc = control("attach 0 /ram.img");
    if (rc != 0) {
        fail_with("attaching an image that lives in memory", rc);
    }
    rc = sys5(SYS_MOUNT, (int)"/dev/loop0", (int)"/l", (int)type, MS_RDONLY, 0);
    if (rc != 0) {
        fail_with("mounting an image that lives in memory", rc);
    }
    expect_file("/l/hello.txt", "hello from inside an image");
    rc = sys2(SYS_UMOUNT2, (int)"/l", 0);
    if (rc != 0) {
        fail_with("unmounting the image in memory", rc);
    }
    if (control("detach 0") != 0) {
        fail("detaching the image in memory");
    }
    if (sys1(SYS_UNLINK, (int)"/ram.img") != 0) {
        fail("removing the copy from the root");
    }

    rc = sys2(SYS_UMOUNT2, (int)"/m", 0);
    if (rc != 0) {
        fail_with("unmounting the volume that holds the images", rc);
    }

    sys3(SYS_WRITE, 1, (int)"LOOPPROBE OK (", 14);
    sys3(SYS_WRITE, 1, (int)type, (int)strlen(type));
    puts(")");
    sys_exit(0);
}
