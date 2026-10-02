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
 * Last come Linux's loop ioctls, the way util-linux's mount -o loop makes
 * them: /dev/loop-control's LOOP_CTL_GET_FREE names a free device, whose node
 * is a block device of major 7; LOOP_CONFIGURE attaches the image open on a
 * descriptor, set to clear itself; LOOP_GET_STATUS64 names the file; the node
 * reads the image's first sector; and the device, mounted while its node is
 * still open, is detached by the unmount after the node is closed.
 * LOOP_SET_FD of a file open for reading attaches it read-only, and
 * LOOP_CLR_FD detaches it.
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
#define SYS_LINK      9
#define SYS_UNLINK    10
#define SYS_RENAME    38
#define SYS_MKDIR     39
#define SYS_MOUNT     21
#define SYS_UMOUNT2   52
#define SYS_FTRUNCATE 93
#define SYS_FSYNC     118
#define SYS_STAT64    195
#define SYS_IOCTL     54

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define MS_RDONLY 1

#define ENOENT 2
#define ENXIO  6
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

#define LOOP_SET_FD        0x4C00
#define LOOP_CLR_FD        0x4C01
#define LOOP_GET_STATUS64  0x4C05
#define LOOP_CONFIGURE     0x4C0A
#define LOOP_CTL_GET_FREE  0x4C82
#define LO_FLAGS_READ_ONLY 1
#define LO_FLAGS_AUTOCLEAR 4

/* struct loop_info64's flags and file name, and a struct loop_config's
 * descriptor and info. */
#define INFO_FLAGS  52
#define INFO_NAME   56
#define CONFIG_INFO 8

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

/** The decimal number `text` starts with. */
static int read_number(const char *text)
{
    int value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        text++;
    }
    return value;
}

/**
 * The number after `name` in /proc/disk, which is how a row counts what the
 * drivers did without a device of its own to watch.
 */
static int proc_disk_number(const char *name)
{
    char buffer[2048];
    int fd = sys3(SYS_OPEN, (int)"/proc/disk", O_RDONLY, 0);
    int n;
    int i;
    int j;

    if (fd < 0) {
        fail_with("opening /proc/disk", fd);
    }
    /* Read to the end: /proc hands out a record at a time, and the count this
       row wants is not in the first of them. */
    n = 0;
    for (;;) {
        int got = sys3(SYS_READ, fd, (int)(buffer + n), (int)sizeof(buffer) - 1 - n);

        if (got <= 0) {
            break;
        }
        n += got;
        if (n >= (int)sizeof(buffer) - 1) {
            break;
        }
    }
    sys1(SYS_CLOSE, fd);
    if (n <= 0) {
        fail_with("reading /proc/disk", n);
    }
    buffer[n] = '\0';
    for (i = 0; i < n; i++) {
        for (j = 0; name[j] && buffer[i + j] == name[j]; j++) {
        }
        if (name[j] == '\0') {
            int value = 0;

            i += j;
            while (buffer[i] == ' ') {
                i++;
            }
            while (buffer[i] >= '0' && buffer[i] <= '9') {
                value = value * 10 + (buffer[i] - '0');
                i++;
            }
            return value;
        }
    }
    sys3(SYS_WRITE, 1, (int)buffer, n > 200 ? 200 : n);
    fail("a name /proc/disk does not have");
    return -1;
}

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

/*
 * The pull row reads until the stick is not there any more; the hold is
 * said after the first read, so the pull lands on a process reading.
 */
/** The struct loop_info64 of the device open on `dev`, into `info`; the
 * errno, or 0. */
static int loop_status(int dev, unsigned char *info)
{
    return sys3(SYS_IOCTL, dev, LOOP_GET_STATUS64, (int)info);
}

/** The flags of the struct loop_info64 at `info`. */
static unsigned int info_flags(const unsigned char *info)
{
    return (unsigned int)info[INFO_FLAGS] | (unsigned int)info[INFO_FLAGS + 1] << 8;
}

/** The node of device `index`, /dev/loopN, opened with `flags`. */
static int open_loop(int index, int flags)
{
    char node[] = "/dev/loop0";

    node[9] = (char)('0' + index);
    return sys3(SYS_OPEN, (int)node, flags, 0);
}

/**
 * The rows after the unmount of the device LOOP_CONFIGURE attached, `index`:
 * the device set to clear itself has gone, LOOP_SET_FD of `image` open for
 * reading attaches it read-only, and LOOP_CLR_FD detaches it.
 */
static void set_fd_rows(const char *image, int index)
{
    unsigned char info[232];
    int dev = open_loop(index, O_RDWR);
    int file, rc;

    if (loop_status(dev, info) != -ENXIO) {
        fail("the device set to clear itself was still attached after the unmount");
    }
    file = sys3(SYS_OPEN, (int)image, O_RDONLY, 0);
    rc = sys3(SYS_IOCTL, dev, LOOP_SET_FD, file);
    if (rc != 0) {
        fail_with("LOOP_SET_FD of an image open for reading", rc);
    }
    if (loop_status(dev, info) != 0 || info_flags(info) != LO_FLAGS_READ_ONLY) {
        fail("LOOP_SET_FD of a file open for reading did not attach it read-only");
    }
    if (sys3(SYS_IOCTL, dev, LOOP_CLR_FD, 0) != 0 || loop_status(dev, info) != -ENXIO) {
        fail("LOOP_CLR_FD did not detach the device");
    }
    sys1(SYS_CLOSE, dev);
    sys1(SYS_CLOSE, file);
}

/**
 * The rows of Linux's loop ioctls over `image`, which holds a file system of
 * `type`, as the header says.
 */
static void ioctl_rows(const char *image, const char *type)
{
    unsigned char config[304], info[232], first[512], sector[512], st[96];
    char node[] = "/dev/loop0";
    int ctl, index, file, dev, rc, i;

    ctl = sys3(SYS_OPEN, (int)"/dev/loop-control", O_RDWR, 0);
    index = ctl < 0 ? ctl : sys3(SYS_IOCTL, ctl, LOOP_CTL_GET_FREE, 0);
    if (index < 0 || index > 3) {
        fail_with("LOOP_CTL_GET_FREE named no free device", index);
    }
    sys1(SYS_CLOSE, ctl);
    node[9] = (char)('0' + index);
    if (sys2(SYS_STAT64, (int)node, (int)st) != 0 || (st[17] & 0xF0) != 0x60 || st[33] != 7 ||
        st[32] != index) {
        fail("a loop device's node is not a block device of major 7");
    }
    file = sys3(SYS_OPEN, (int)image, O_RDWR, 0);
    dev = open_loop(index, O_RDWR);
    if (file < 0 || dev < 0 || sys3(SYS_READ, file, (int)first, 512) != 512) {
        fail("opening and reading the image and opening a free loop device");
    }
    for (i = 0; i < (int)sizeof(config); i++) {
        config[i] = 0;
    }
    config[0] = (unsigned char)file;
    config[CONFIG_INFO + INFO_FLAGS] = LO_FLAGS_AUTOCLEAR;
    rc = sys3(SYS_IOCTL, dev, LOOP_CONFIGURE, (int)config);
    if (rc != 0) {
        fail_with("LOOP_CONFIGURE of an image open on a descriptor", rc);
    }
    if (loop_status(dev, info) != 0 || !streq((const char *)info + INFO_NAME, image) ||
        info_flags(info) != LO_FLAGS_AUTOCLEAR) {
        fail("LOOP_GET_STATUS64 did not name the file and its flags");
    }
    if (sys3(SYS_READ, dev, (int)sector, 512) != 512) {
        fail("reading the first sector of the device");
    }
    for (i = 0; i < 512 && first[i] == sector[i]; i++) {
    }
    if (i != 512) {
        fail("the device's first sector is not the image's");
    }
    rc = sys5(SYS_MOUNT, (int)node, (int)"/l", (int)type, 0, 0);
    if (rc != 0) {
        fail_with("mounting a device LOOP_CONFIGURE attached", rc);
    }
    sys1(SYS_CLOSE, dev);
    sys1(SYS_CLOSE, file);
    expect_file("/l/hello.txt", "hello from inside an image");
    rc = sys2(SYS_UMOUNT2, (int)"/l", 0);
    if (rc != 0) {
        fail_with("unmounting the device LOOP_CONFIGURE attached", rc);
    }
    set_fd_rows(image, index);
}

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

    /* A write through a loop device is durable when it returns, and not when
       something later flushes: the device writes the backing file and syncs
       it. The row fills the cache so that dirty blocks of the image are
       written back by eviction alone, with no sync and no unmount, and the
       drivers' flush count must move. */
    if (streq(type, "durable")) {
        int filler;
        int block_at;

        rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/m", (int)"vfat", 0, 0);
        if (rc != 0) {
            fail_with("mounting the volume that holds the image", rc);
        }
        rc = control("attach 0 /m/krffs.img");
        if (rc != 0) {
            fail_with("attaching the image", rc);
        }
        rc = sys5(SYS_MOUNT, (int)"/dev/loop0", (int)"/l", (int)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the image", rc);
        }
        (void)proc_disk_number;
        filler = sys3(SYS_OPEN, (int)"/l/filler.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (filler < 0) {
            fail_with("the file that fills the cache", filler);
        }
        for (block_at = 0; block_at < 128; block_at++) {
            char block[1024];
            int k;

            for (k = 0; k < (int)sizeof(block); k++) {
                block[k] = (char)(block_at + k);
            }
            if (sys3(SYS_WRITE, filler, (int)block, (int)sizeof(block)) != (int)sizeof(block)) {
                fail("filling the cache through the image");
            }
        }
        sys1(SYS_CLOSE, filler);
        /* Every write the device made, and every sync it owed for one. The
           blocks reached it by eviction alone: nothing here asked for a sync
           or an unmount. */
        {
            char line[256];
            int writes = 0;
            int syncs = 0;
            int i2;
            int n2 = loop_lines(line, (int)sizeof(line));

            for (i2 = 0; i2 < n2; i2++) {
                if (line[i2] == 'w' && line[i2 + 1] == 'r' && line[i2 + 2] == 'i') {
                    writes = read_number(line + i2 + 7);
                }
                if (line[i2] == 's' && line[i2 + 1] == 'y' && line[i2 + 2] == 'n') {
                    syncs = read_number(line + i2 + 6);
                }
            }
            if (writes <= 0) {
                fail_with("the writes eviction made through the device", writes);
            }
            if (syncs != writes) {
                fail_with("the writes the device did not sync", writes - syncs);
            }
        }
        if (sys2(SYS_UMOUNT2, (int)"/l", 0) != 0) {
            fail("unmounting the image after filling it");
        }
        if (control("detach 0") != 0) {
            fail("detaching after filling it");
        }
        if (sys2(SYS_UMOUNT2, (int)"/m", 0) != 0) {
            fail("unmounting the volume after filling it");
        }
        puts("LOOPPROBE OK (durable)");
        sys_exit(0);
    }

    /* The volume the file lives on need not be a FAT one, and a file is named
       by its mount and its node rather than by a path: a hard link to an
       attached image is the same file and a second device over it is
       refused. */
    if (streq(type, "ext2back")) {
        rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/m", (int)"ext2", 0, 0);
        if (rc != 0) {
            fail_with("mounting the ext2 volume that holds the image", rc);
        }
        if (sys2(SYS_LINK, (int)"/m/krffs.img", (int)"/m/alias.img") != 0) {
            fail("a second name for the image");
        }
        rc = control("attach 0 /m/krffs.img");
        if (rc != 0) {
            fail_with("attaching an image on an ext2 volume", rc);
        }
        if (control("attach 1 /m/alias.img") != -EBUSY) {
            fail("the same file was attached twice under two names");
        }
        rc = sys5(SYS_MOUNT, (int)"/dev/loop0", (int)"/l", (int)"krffs", 0, 0);
        if (rc != 0) {
            fail_with("mounting the image on the ext2 volume", rc);
        }
        expect_file("/l/hello.txt", "hello from inside an image");
        {
            int made = sys3(SYS_OPEN, (int)"/l/on-ext2.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

            if (made < 0 || sys3(SYS_WRITE, made, (int)"on ext2\n", 8) != 8) {
                fail("writing the image on an ext2 volume");
            }
            sys1(SYS_FSYNC, made);
            sys1(SYS_CLOSE, made);
        }
        if (sys2(SYS_UMOUNT2, (int)"/l", 0) != 0) {
            fail("unmounting the image on an ext2 volume");
        }
        if (control("detach 0") != 0) {
            fail("detaching the image on an ext2 volume");
        }
        if (sys1(SYS_UNLINK, (int)"/m/alias.img") != 0) {
            fail("removing the second name");
        }
        if (sys2(SYS_UMOUNT2, (int)"/m", 0) != 0) {
            fail("unmounting the ext2 volume");
        }
        puts("LOOPPROBE OK (ext2back)");
        sys_exit(0);
    }

    if (streq(type, "pull")) {
        int reads = 0;
        int opens = 0;

        rc = sys5(SYS_MOUNT, (int)"/dev/disk0p1", (int)"/m", (int)"vfat", 0, 0);
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

        for (;;) {
            int fd2 = sys3(SYS_OPEN, (int)"/l/hello.txt", O_RDONLY, 0);

            if (fd2 < 0) {
                break;
            }
            if (opens++ == 1) {
                puts("LOOPPROBE HOLDING");
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

        /* The slot is still taken: a mount may still be standing on the
           device, and an attachment here would put another file under it. */
        if (control("attach 0 /m/fat.img") != -EBUSY) {
            fail("a device whose stick went took another file");
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
    if (control("detach 0 junk") != -EINVAL) {
        fail("a detach with a word after the number was taken");
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
    /* The root keeps no count on a node of its own: its rule for a file
       somebody holds is the descriptor tables, and an attachment is not one.
       So a removal of the file under this device, by either name it can be
       removed by, is refused while the device stands on it. */
    if (sys1(SYS_UNLINK, (int)"/ram.img") != -EBUSY) {
        fail("the file under an attached device was unlinked");
    }
    {
        int spare = sys3(SYS_OPEN, (int)"/spare.img", O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (spare < 0) {
            fail("making a file to rename over the attached one");
        }
        sys3(SYS_WRITE, spare, (int)"x", 1);
        sys1(SYS_CLOSE, spare);
        if (sys2(SYS_RENAME, (int)"/spare.img", (int)"/ram.img") != -EBUSY) {
            fail("the file under an attached device was renamed over");
        }
        if (sys1(SYS_UNLINK, (int)"/spare.img") != 0) {
            fail("removing the spare file");
        }
    }
    /* And an open that truncates is a change of its length like any other. */
    if (sys3(SYS_OPEN, (int)"/ram.img", O_WRONLY | O_TRUNC, 0) != -EBUSY) {
        fail("an attached image was truncated by an open");
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
    ioctl_rows(image, type);

    rc = sys2(SYS_UMOUNT2, (int)"/m", 0);
    if (rc != 0) {
        fail_with("unmounting the volume that holds the images", rc);
    }

    sys3(SYS_WRITE, 1, (int)"LOOPPROBE OK (", 14);
    sys3(SYS_WRITE, 1, (int)type, (int)strlen(type));
    puts(")");
    sys_exit(0);
}
