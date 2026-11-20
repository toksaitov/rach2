/**
 * loopdevprobe: a file system in a file, through a loop device.
 *
 * The disk's one partition is a FAT32 volume with the images: a FAT32, an ext2
 * and a KRFFS one, each with a file of known bytes in it, and for the refusals
 * a second FAT32 image and a file whose length is not whole sectors. The probe
 * mounts that volume, attaches an image to /dev/loop0 through
 * /dev/loop-control, mounts the device, reads what is in it, first through a
 * private mapping, which a page fault reads through the device, writes a file,
 * unmounts, detaches, attaches again and reads back what it wrote, which is
 * what says the write reached the file and not only the cache. On a vfat
 * image it also exchanges two directories across parents with
 * RENAME_EXCHANGE, whose four writes go down through the device into the
 * volume's file system, and reads the exchanged names back, and mounts the
 * image with sys_immutable, under which a file given the system bit takes
 * no change (`immutable_rows`).
 *
 * It then asks for everything the attachment refuses: the same file twice, a
 * write of an attached file, a detach with a mount on it, an unmount of the
 * volume holding the file, a directory, and a file whose length is not whole
 * sectors. A truncation of an attached file goes through, as on Linux, on
 * every file system but KRFFS: one to the file's own length, and one of a
 * small file on a device, past whose new end the device reads zeros.
 *
 * Last come Linux's loop ioctls, the way util-linux's mount -o loop makes
 * them: /dev/loop-control's LOOP_CTL_GET_FREE names a free device, whose node
 * is a block device of major 7; LOOP_CONFIGURE attaches the image open on a
 * descriptor, set to clear itself; LOOP_GET_STATUS64 names the file; the node
 * reads the image's first sector; opened with O_DIRECT, it refuses a write
 * at its end with ENOSPC before it judges the buffer, and cuts a write that
 * crosses the end to the sectors before it; and the device, mounted while
 * its node is still open, is detached by the unmount after the node is
 * closed.
 * LOOP_SET_FD of a file open for reading attaches it read-only, and
 * LOOP_CLR_FD detaches it.
 *
 * `loopdevprobe pull` is the stick's row: the image lives on a volume of a
 * stick, and the scenario pulls the stick while the image is mounted. The
 * row reads a file of the image, yielding between reads, until a read or
 * the open before it fails, which is the removal witnessed, however many
 * reads that takes; the case's watchdog ends a run whose reads never fail.
 * The row then requires an error from the read in flight, an unmount of the
 * mount over the device that has gone, and an unmount of the volume the
 * file was on, since the attachment let go of it when its medium went.
 *
 * `loopdevprobe durable` makes DURABLE_WRITES writes to a file in a KRFFS
 * image. KRFFS writes its dirty blocks to the device at a sync point that
 * ends each write and then flushes the device, so the device's write count
 * and its sync count, read from /proc/loop before the first and after the
 * last, must each grow by at least one a write: every sync point synced the
 * backing file before it returned. The counts are taken after the mount, so
 * the mount's own write of the volume's dirty flag is not among them.
 * `loopdevprobe ext2back` attaches an image that lives on an ext2 volume, and
 * refuses it a second device under a second name.
 *
 * `loopdevprobe vfat`, `ext2` and `krffs` name the type inside the image,
 * `vfat` with no argument. Prints "LOOPPROBE OK (type)", the type or the
 * mode, and exits 0, or "LOOPPROBE FAIL: " and the reason, unmounts what it
 * mounted, detaches what it attached and exits 1.
 *
 * The probe works in /m, where it mounts the volume, in /l and /r, where it
 * mounts the images, and on the root, where it puts the copy. A native run
 * (probe_args.h) has no volume of images: `images=DIR` names a directory,
 * such as a tmpfs, into which the probe copies the images the workspace has,
 * and the probe neither mounts nor unmounts it, so that a device on a file of
 * an image's volume has one device under it, not two, and is allowed;
 * `mount=DIR` and `ro=DIR` name the mount points; and `copy=DIR` a directory
 * on which the probe mounts a tmpfs of its own for the copy, which has no
 * device under it, as the root of the test image has none; the tmpfs allows
 * an unlink of the attached copy and a rename over it, which the KRFFS root
 * refuses. The run uses the first two devices that /proc/loop does not name
 * in place of loop0 and loop1, and leaves the machine's own alone. A relative
 * path is taken from the working directory.
 */

#include "probe.h"
#include "probe_args.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_DIRECT 040000

#define MS_RDONLY 1

#define AT_FDCWD        (-100)
#define RENAME_EXCHANGE 2
#if defined __x86_64__
#define SYS_RENAMEAT2_ 316
#else
#define SYS_RENAMEAT2_ 353
#endif

#define SEEK_END 2

#define PROT_READ   1
#define MAP_PRIVATE 2

/* vfat's attribute requests, _IOR and _IOW of 'r', 0x10 and 0x11, a u32,
 * and the attribute bits the rows give: archive, and system. */
#define FAT_IOCTL_GET_ATTRIBUTES 0x80047210L
#define FAT_IOCTL_SET_ATTRIBUTES 0x40047211L
#define FAT_ARCHIVE              0x20u
#define FAT_SYSTEM               0x04u

/* The writes the durable mode makes, each a sync point of KRFFS. */
#define DURABLE_WRITES 128

#define EPERM  1
#define ENOENT 2
#define ENXIO  6
#define ENOSPC 28
#define EBUSY  16
#define EINVAL 22
#define EROFS  30
#define ELOOP  40

/** End the process with `status`; it does not return. */
static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/* The two devices the rows attach, A and B: loop0 and loop1 on the test
 * image, and in a native run the first two that /proc/loop does not name,
 * so that the machine's own attachments are left alone. `choose_devices`
 * sets the digit of each string below. */
static char attach_a[] = "attach 0 ", detach_a[] = "detach 0", node_a[] = "/dev/loop0",
            line_a[] = "loop0 ", detach_junk[] = "detach 0junk", detach_word[] = "detach 0 junk";
static char attach_b[] = "attach 1 ", attach_ro_b[] = "attach ro 1 ", detach_b[] = "detach 1",
            node_b[] = "/dev/loop1", line_b[] = "loop1 ";

/* What a failed check undoes: the places the probe has mounted, oldest
 * first, a null pointer for one unmounted since, and the devices it has
 * attached, a bit each. */
#define MOUNTS_CAP 8
static const char *mounts[MOUNTS_CAP];
static int mount_count;
static unsigned attached;

/** mount(2), which keeps `target` for `undo` when it succeeds; the errno,
 * or 0. `target` is one of the probe's static paths. */
static int mount_at(long source, long target, long type, long flags, long data)
{
    int rc = syscall5(SYS_MOUNT, source, target, type, flags, data);

    if (rc == 0 && mount_count < MOUNTS_CAP) {
        mounts[mount_count++] = (const char *)target;
    }
    return rc;
}

/** umount2(2) of `target`, which `undo` then leaves alone; the errno, or
 * 0. */
static int unmount(long target, long flags)
{
    int rc = syscall2(SYS_UMOUNT2, target, flags);
    int i;

    for (i = mount_count - 1; rc == 0 && i >= 0; i--) {
        if (mounts[i] == (const char *)target) {
            mounts[i] = 0;
            break;
        }
    }
    return rc;
}

/**
 * Take back what the probe made on the machine: unmount its mounts, newest
 * first, detach its devices, and unmount again what a device on a file of it
 * kept busy the first time. Errors are passed over, since this runs only
 * after a check has failed.
 */
static void undo(void)
{
    char line[] = "detach 0";
    int pass, fd, i;

    for (pass = 0; pass < 2; pass++) {
        for (i = mount_count - 1; i >= 0; i--) {
            if (mounts[i] != 0 && syscall2(SYS_UMOUNT2, (long)mounts[i], 0) == 0) {
                mounts[i] = 0;
            }
        }
        fd = syscall3(SYS_OPEN, (long)"/dev/loop-control", O_WRONLY, 0);
        for (i = 0; fd >= 0 && i < 4; i++) {
            line[7] = (char)('0' + i);
            if ((attached & 1u << i) && syscall3(SYS_WRITE, fd, (long)line, 8) >= 0) {
                attached &= ~(1u << i);
            }
        }
        if (fd >= 0) {
            syscall1(SYS_CLOSE, fd);
        }
    }
}

/** Print "LOOPPROBE FAIL: " and `why`, undo the probe's mounts and
 * attachments, and exit 1. */
static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"LOOPPROBE FAIL: ", 16);
    puts(why);
    undo();
    sys_exit(1);
}

/** Print "LOOPPROBE FAIL: ", `why` and the number `n` it got, undo the
 * probe's mounts and attachments, and exit 1. */
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
    syscall3(SYS_WRITE, 1, (long)"LOOPPROBE FAIL: ", 16);
    syscall3(SYS_WRITE, 1, (long)why, (int)strlen(why));
    syscall3(SYS_WRITE, 1, (long)" (got ", 6);
    syscall3(SYS_WRITE, 1, (long)(digits + i), (int)strlen(digits + i));
    puts(")");
    undo();
    sys_exit(1);
}

/** Whether the strings `a` and `b` are equal. */
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

/** Whether the string `s` begins with the string `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

/** One command to /dev/loop-control, whose attach or detach `undo` then
 * knows of when it succeeds; the errno, or 0. */
static int control(const char *line)
{
    int fd = syscall3(SYS_OPEN, (long)"/dev/loop-control", O_WRONLY, 0);
    const char *index = line + 7;
    int rc;

    if (fd < 0) {
        fail_with("opening /dev/loop-control", fd);
    }
    rc = syscall3(SYS_WRITE, fd, (long)line, (int)strlen(line));
    syscall1(SYS_CLOSE, fd);
    if (rc < 0) {
        return rc;
    }
    if (starts(line, "attach ro ")) {
        index += 3;
    }
    if (starts(line, "attach ") && *index >= '0' && *index <= '3') {
        attached |= 1u << (*index - '0');
    }
    if (starts(line, "detach ") && *index >= '0' && *index <= '3') {
        attached &= ~(1u << (*index - '0'));
    }
    return 0;
}

/** What /proc/loop says, into `buffer` of `cap` bytes and NUL-ended; the
 * count read. A failed open or read fails the probe. */
static int loop_lines(char *buffer, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)"/proc/loop", O_RDONLY, 0);
    int n;

    if (fd < 0) {
        fail_with("opening /proc/loop", fd);
    }
    n = syscall3(SYS_READ, fd, (long)buffer, cap - 1);
    syscall1(SYS_CLOSE, fd);
    if (n < 0) {
        fail_with("reading /proc/loop", n);
    }
    buffer[n] = '\0';
    return n;
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

/** The line of the text of /proc/loop, `lines`, that begins with `name`,
 * such as "loop0 ", or a null pointer when no line does. */
static const char *device_line(const char *lines, const char *name)
{
    while (*lines) {
        if (starts(lines, name)) {
            return lines;
        }
        while (*lines && *lines != '\n') {
            lines++;
        }
        if (*lines) {
            lines++;
        }
    }
    return 0;
}

/** Whether `line` of /proc/loop, a null pointer for none, has `text`
 * before its end. */
static int line_has(const char *line, const char *text)
{
    for (; line != 0 && *line && *line != '\n'; line++) {
        if (starts(line, text)) {
            return 1;
        }
    }
    return 0;
}

/** The `writes` and `syncs` counts /proc/loop gives device A, into `writes`
 * and `syncs`; a count it does not give is left -1. */
static void loop_counts(int *writes, int *syncs)
{
    char lines[512];
    const char *line;

    *writes = -1;
    *syncs = -1;
    loop_lines(lines, (int)sizeof(lines));
    for (line = device_line(lines, line_a); line != 0 && *line && *line != '\n'; line++) {
        if (*writes < 0 && starts(line, " writes ")) {
            *writes = read_number(line + 8);
        }
        if (*syncs < 0 && starts(line, " syncs ")) {
            *syncs = read_number(line + 7);
        }
    }
}

/**
 * The number after `name` in /proc/disk, which is how a row counts what the
 * drivers did without a device of its own to watch. A /proc/disk without
 * `name` fails the probe.
 */
static int proc_disk_number(const char *name)
{
    char buffer[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/disk", O_RDONLY, 0);
    int n;
    int i;
    int j;

    if (fd < 0) {
        fail_with("opening /proc/disk", fd);
    }
    /* Read to the end, since the count is past the first record. */
    n = 0;
    for (;;) {
        int got = syscall3(SYS_READ, fd, (long)(buffer + n), (int)sizeof(buffer) - 1 - n);

        if (got <= 0) {
            break;
        }
        n += got;
        if (n >= (int)sizeof(buffer) - 1) {
            break;
        }
    }
    syscall1(SYS_CLOSE, fd);
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
    syscall3(SYS_WRITE, 1, (long)buffer, n > 200 ? 200 : n);
    fail("a name /proc/disk does not have");
    return -1;
}

/** Fail unless the file `path` begins with the bytes of `want`. */
static void expect_file(const char *path, const char *want)
{
    char buffer[256];
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n;
    int i;

    if (fd < 0) {
        fail_with("opening a file in the image", fd);
    }
    n = syscall3(SYS_READ, fd, (long)buffer, (int)sizeof(buffer) - 1);
    syscall1(SYS_CLOSE, fd);
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

/** Fail unless a private mapping of the file `path` begins with the bytes of
 * `want`. */
static void expect_mapped(const char *path, const char *want)
{
    const char *m;
    long addr;
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int i;

    if (fd < 0) {
        fail_with("opening a file in the image to map it", fd);
    }
    addr = probe_mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    syscall1(SYS_CLOSE, fd);
    if (sys_failed(addr)) {
        fail_with("a private mapping of a file in the image", (int)addr);
    }
    m = (const char *)addr;
    for (i = 0; want[i]; i++) {
        if (m[i] != want[i]) {
            fail_with("a byte of a private mapping of a file in the image", i);
        }
    }
    if (syscall2(SYS_MUNMAP, addr, 4096) != 0) {
        fail("unmapping a file in the image");
    }
}

/* The longest path the probe makes. */
#define PATH_CAP 192

/* Where the probe works, each an absolute path: the directory of the images,
 * /m with the volume /dev/disk0p1 mounted on it or the one `images=` names;
 * the mount points of the images, /l and /r or those `mount=` and `ro=`
 * name; and the directory of the copy, the root, or `copy=` with a tmpfs the
 * probe mounts on it. `image` is the image of the type rows, and `shown` its
 * first 63 bytes, as much of its name as /proc/loop and LOOP_GET_STATUS64
 * keep. */
static char images_dir[PATH_CAP], mount_dir[PATH_CAP], ro_dir[PATH_CAP], copy_dir[PATH_CAP];
static char image[PATH_CAP], shown[64];
/* Whether `images=` named the images' directory, which then holds the images
 * of the workspace and no volume of the probe's own. */
static int native_images;

/** Write `dir`, a slash and `name` into `out`, of PATH_CAP bytes, and return
 * `out`. */
static const char *join(char *out, const char *dir, const char *name)
{
    int n = 0;

    while (*dir && n < PATH_CAP - 2) {
        out[n++] = *dir++;
    }
    out[n++] = '/';
    while (*name && n < PATH_CAP - 1) {
        out[n++] = *name++;
    }
    out[n] = '\0';
    return out;
}

/** Copy the path `arg` into `out`, of PATH_CAP bytes, made absolute against
 * the working directory when it is relative. */
static void absolute(char *out, const char *arg)
{
    int n = 0;

    if (arg[0] != '/') {
        if (syscall2(SYS_GETCWD, (long)out, PATH_CAP) < 0) {
            fail("reading the working directory");
        }
        n = (int)strlen(out);
        if (n > 1) {
            out[n++] = '/';
        }
    }
    while (*arg && n < PATH_CAP - 1) {
        out[n++] = *arg++;
    }
    out[n] = '\0';
}

/** One command to /dev/loop-control, `verb`, such as "attach 0 ", and then
 * `path`; the errno, or 0. */
static int control_on(const char *verb, const char *path)
{
    char line[PATH_CAP + 16];
    int n = 0;

    while (*verb && n < 15) {
        line[n++] = *verb++;
    }
    while (*path && n < (int)sizeof(line) - 1) {
        line[n++] = *path++;
    }
    line[n] = '\0';
    return control(line);
}

/** Copy the file `from` to `to`, made or emptied; fails with `open_why` when
 * either does not open, `write_why` when a write is short and `read_why`
 * when a read fails. */
static void copy_file(const char *from, const char *to, const char *open_why, const char *write_why,
                      const char *read_why)
{
    char block[4096];
    int in = syscall3(SYS_OPEN, (long)from, O_RDONLY, 0);
    int out = syscall3(SYS_OPEN, (long)to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int n;

    if (in < 0 || out < 0) {
        fail(open_why);
    }
    while ((n = syscall3(SYS_READ, in, (long)block, (int)sizeof(block))) > 0) {
        if (syscall3(SYS_WRITE, out, (long)block, n) != n) {
            fail(write_why);
        }
    }
    syscall1(SYS_CLOSE, in);
    syscall1(SYS_CLOSE, out);
    if (n < 0) {
        fail_with(read_why, n);
    }
}

/**
 * Put the images of a native run in the images' directory under the names
 * the volume of a QEMU run gives them: `loopdev-fat` of the workspace as
 * fat.img and as odd2.img, `loopdev-ext2` as ext2.img and `loopdev-krffs` as
 * krffs.img, and beside them odd.bin, a file of 100 bytes.
 */
static void stage_images(void)
{
    static const char *const from[] = {"loopdev-fat", "loopdev-fat", "loopdev-ext2",
                                       "loopdev-krffs"};
    static const char *const to[] = {"fat.img", "odd2.img", "ext2.img", "krffs.img"};
    char path[PATH_CAP], zero[100];
    int i, fd;

    for (i = 0; i < 4; i++) {
        copy_file(from[i], join(path, images_dir, to[i]), "opening an image to stage it",
                  "staging an image", "reading an image to stage it");
    }
    for (i = 0; i < (int)sizeof(zero); i++) {
        zero[i] = 0;
    }
    fd = syscall3(SYS_OPEN, (long)join(path, images_dir, "odd.bin"), O_WRONLY | O_CREAT | O_TRUNC,
                  0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)zero, (int)sizeof(zero)) != (int)sizeof(zero)) {
        fail("making the file whose length is not whole sectors");
    }
    syscall1(SYS_CLOSE, fd);
}

/** Mount /dev/disk0p1, the volume of the images, on their directory as
 * `fs`, failing with `why`; or, in a native run, stage the images there. */
static void mount_images(const char *fs, const char *why)
{
    int rc;

    if (native_images) {
        stage_images();
        return;
    }
    rc = mount_at((long)"/dev/disk0p1", (long)images_dir, (long)fs, 0, 0);
    if (rc != 0) {
        fail_with(why, rc);
    }
}

/** Unmount the volume of the images, which a native run does not have;
 * the errno, or 0. */
static int unmount_images(void)
{
    return native_images ? 0 : unmount((long)images_dir, 0);
}

PROBE_SP_ENTRY;

/** The struct loop_info64 of the device open on `dev`, into `info`; the
 * errno, or 0. */
static int loop_status(int dev, unsigned char *info)
{
    return syscall3(SYS_IOCTL, dev, LOOP_GET_STATUS64, (long)info);
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
    return syscall3(SYS_OPEN, (long)node, flags, 0);
}

/**
 * The rows after the unmount of the device LOOP_CONFIGURE attached, `index`:
 * the device set to clear itself has gone, LOOP_SET_FD of `image` open for
 * reading attaches it read-only, and LOOP_CLR_FD detaches it.
 */
static void set_fd_rows(int index)
{
    unsigned char info[232];
    int dev = open_loop(index, O_RDWR);
    int file, rc;

    if (loop_status(dev, info) != -ENXIO) {
        fail("the device set to clear itself was still attached after the unmount");
    }
    attached &= ~(1u << index);
    file = syscall3(SYS_OPEN, (long)image, O_RDONLY, 0);
    rc = syscall3(SYS_IOCTL, dev, LOOP_SET_FD, file);
    if (rc != 0) {
        fail_with("LOOP_SET_FD of an image open for reading", rc);
    }
    attached |= 1u << index;
    if (loop_status(dev, info) != 0 || info_flags(info) != LO_FLAGS_READ_ONLY) {
        fail("LOOP_SET_FD of a file open for reading did not attach it read-only");
    }
    if (syscall3(SYS_IOCTL, dev, LOOP_CLR_FD, 0) != 0 || loop_status(dev, info) != -ENXIO) {
        fail("LOOP_CLR_FD did not detach the device");
    }
    attached &= ~(1u << index);
    syscall1(SYS_CLOSE, dev);
    syscall1(SYS_CLOSE, file);
}

/* The buffer of the O_DIRECT rows, on a page, so that it and its sectors
 * are aligned and the byte after its start is not. */
static unsigned char direct_buf[1024] __attribute__((aligned(4096)));

/**
 * The O_DIRECT rows of the node of device `index`, whose image is `size`
 * bytes, in the order of Linux's `blkdev_write_iter`, which cuts a write
 * at the device's end before `blkdev_dio_invalid` judges it: a write at the
 * end from a buffer off the sector is ENOSPC, and one of 513 bytes a sector
 * before the end is cut to the last sector, which it writes back as the
 * read before it found it.
 */
static void direct_end_rows(int index, long long size)
{
    int dev = open_loop(index, O_RDWR | O_DIRECT);
    long long at;
    long rc;

    if (dev < 0) {
        fail_with("opening the loop node with O_DIRECT", dev);
    }
    if (probe_llseek(dev, size, 0, &at) != 0) {
        fail("seeking to the end of the loop node");
    }
    rc = syscall3(SYS_WRITE, dev, (long)(direct_buf + 1), 512);
    if (rc != -ENOSPC) {
        fail_with("an O_DIRECT write at the node's end from a buffer off the sector is not ENOSPC",
                  (int)rc);
    }
    if (probe_pread(dev, (char *)direct_buf, 512, size - 512) != 512 ||
        probe_llseek(dev, size - 512, 0, &at) != 0) {
        fail("reading the loop node's last sector with O_DIRECT");
    }
    rc = syscall3(SYS_WRITE, dev, (long)direct_buf, 513);
    if (rc != 512) {
        fail_with("an O_DIRECT write of 513 bytes a sector before the node's end is not cut to 512",
                  (int)rc);
    }
    syscall1(SYS_CLOSE, dev);
}

/**
 * The rows of Linux's loop ioctls over `image`, which has a file system of
 * `type`, as the file's opening comment describes them. The node's mode has
 * the block device's type bits, and st_rdev has the major and the minor in
 * its low two bytes, as Linux encodes them.
 */
static void ioctl_rows(const char *type)
{
    unsigned char config[304], info[232], first[512], sector[512], st[KSTAT_SIZE];
    char node[] = "/dev/loop0";
    char path[PATH_CAP];
    long long size;
    int ctl, index, file, dev, rc, i;

    ctl = syscall3(SYS_OPEN, (long)"/dev/loop-control", O_RDWR, 0);
    index = ctl < 0 ? ctl : syscall3(SYS_IOCTL, ctl, LOOP_CTL_GET_FREE, 0);
    if (index < 0 || index > 3) {
        fail_with("LOOP_CTL_GET_FREE named no free device", index);
    }
    syscall1(SYS_CLOSE, ctl);
    node[9] = (char)('0' + index);
    /* A block device of major 7, the minor the index. */
    if (syscall2(SYS_STAT, (long)node, (long)st) != 0 || (st[KSTAT_MODE + 1] & 0xF0) != 0x60 ||
        st[KSTAT_RDEV + 1] != 7 || st[KSTAT_RDEV] != index) {
        fail("a loop device's node is not a block device of major 7");
    }
    file = syscall3(SYS_OPEN, (long)image, O_RDWR, 0);
    dev = open_loop(index, O_RDWR);
    if (file < 0 || dev < 0 || probe_llseek(file, 0, 2, &size) != 0 ||
        probe_pread(file, (char *)first, 512, 0) != 512) {
        fail("opening and reading the image and opening a free loop device");
    }
    for (i = 0; i < (int)sizeof(config); i++) {
        config[i] = 0;
    }
    config[0] = (unsigned char)file;
    config[CONFIG_INFO + INFO_FLAGS] = LO_FLAGS_AUTOCLEAR;
    rc = syscall3(SYS_IOCTL, dev, LOOP_CONFIGURE, (long)config);
    if (rc != 0) {
        fail_with("LOOP_CONFIGURE of an image open on a descriptor", rc);
    }
    attached |= 1u << index;
    if (loop_status(dev, info) != 0 || !streq((const char *)info + INFO_NAME, shown) ||
        info_flags(info) != LO_FLAGS_AUTOCLEAR) {
        fail("LOOP_GET_STATUS64 did not name the file and its flags");
    }
    if (syscall3(SYS_READ, dev, (long)sector, 512) != 512) {
        fail("reading the first sector of the device");
    }
    for (i = 0; i < 512 && first[i] == sector[i]; i++) {
    }
    if (i != 512) {
        fail("the device's first sector is not the image's");
    }
    direct_end_rows(index, size);
    rc = mount_at((long)node, (long)mount_dir, (long)type, 0, 0);
    if (rc != 0) {
        fail_with("mounting a device LOOP_CONFIGURE attached", rc);
    }
    syscall1(SYS_CLOSE, dev);
    syscall1(SYS_CLOSE, file);
    expect_file(join(path, mount_dir, "hello.txt"), "hello from inside an image");
    rc = unmount((long)mount_dir, 0);
    if (rc != 0) {
        fail_with("unmounting the device LOOP_CONFIGURE attached", rc);
    }
    set_fd_rows(index);
}

/**
 * `durable`: a sync point of a file system on a loop device syncs the
 * backing file before it returns, as /proc/loop counts the device's writes
 * and syncs.
 */
static void durable_mode(void)
{
    char path[PATH_CAP];
    int filler, block_at, rc;
    int writes_before, syncs_before, writes, syncs;

    mount_images("vfat", "mounting the volume that holds the image");
    rc = control_on(attach_a, join(path, images_dir, "krffs.img"));
    if (rc != 0) {
        fail_with("attaching the image", rc);
    }
    rc = mount_at((long)node_a, (long)mount_dir, (long)"krffs", 0, 0);
    if (rc != 0) {
        fail_with("mounting the image", rc);
    }
    (void)proc_disk_number;
    filler = syscall3(SYS_OPEN, (long)join(path, mount_dir, "filler.bin"),
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (filler < 0) {
        fail_with("the file written through the image", filler);
    }
    loop_counts(&writes_before, &syncs_before);
    if (writes_before < 0 || syncs_before < 0) {
        fail("/proc/loop gives the device no write and sync counts");
    }
    for (block_at = 0; block_at < DURABLE_WRITES; block_at++) {
        char block[1024];
        int k;

        for (k = 0; k < (int)sizeof(block); k++) {
            block[k] = (char)(block_at + k);
        }
        if (syscall3(SYS_WRITE, filler, (long)block, (int)sizeof(block)) != (int)sizeof(block)) {
            fail("writing the file through the image");
        }
    }
    loop_counts(&writes, &syncs);
    syscall1(SYS_CLOSE, filler);
    if (writes - writes_before < DURABLE_WRITES) {
        fail_with("the device writes the image's sync points made", writes - writes_before);
    }
    if (syncs - syncs_before < DURABLE_WRITES) {
        fail_with("the syncs of the backing file the image's sync points made",
                  syncs - syncs_before);
    }
    if (unmount((long)mount_dir, 0) != 0) {
        fail("unmounting the image after writing it");
    }
    if (control(detach_a) != 0) {
        fail("detaching after writing it");
    }
    if (unmount_images() != 0) {
        fail("unmounting the volume after writing it");
    }
    puts("LOOPPROBE OK (durable)");
    sys_exit(0);
}

/**
 * `ext2back`: the volume the file lives on need not be a FAT one, and a file
 * is named by its mount and its node rather than by a path, so a hard link
 * to an attached image is the same file and a second device over it is
 * refused.
 */
static void ext2back_mode(void)
{
    char path[PATH_CAP], alias[PATH_CAP];
    int made, rc;

    mount_images("ext2", "mounting the ext2 volume that holds the image");
    join(path, images_dir, "krffs.img");
    if (syscall2(SYS_LINK, (long)path, (long)join(alias, images_dir, "alias.img")) != 0) {
        fail("a second name for the image");
    }
    rc = control_on(attach_a, path);
    if (rc != 0) {
        fail_with("attaching an image on an ext2 volume", rc);
    }
    if (control_on(attach_b, alias) != -EBUSY) {
        fail("the same file was attached twice under two names");
    }
    rc = mount_at((long)node_a, (long)mount_dir, (long)"krffs", 0, 0);
    if (rc != 0) {
        fail_with("mounting the image on the ext2 volume", rc);
    }
    expect_file(join(path, mount_dir, "hello.txt"), "hello from inside an image");
    made = syscall3(SYS_OPEN, (long)join(path, mount_dir, "on-ext2.txt"),
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (made < 0 || syscall3(SYS_WRITE, made, (long)"on ext2\n", 8) != 8) {
        fail("writing the image on an ext2 volume");
    }
    syscall1(SYS_FSYNC, made);
    syscall1(SYS_CLOSE, made);
    if (unmount((long)mount_dir, 0) != 0) {
        fail("unmounting the image on an ext2 volume");
    }
    if (control(detach_a) != 0) {
        fail("detaching the image on an ext2 volume");
    }
    if (syscall1(SYS_UNLINK, (long)alias) != 0) {
        fail("removing the second name");
    }
    if (unmount_images() != 0) {
        fail("unmounting the ext2 volume");
    }
    puts("LOOPPROBE OK (ext2back)");
    sys_exit(0);
}

/**
 * `pull` reads until the stick is not there any more; `LOOPPROBE HOLDING`
 * is printed after the first read, so the pull lands on a process reading.
 * The device's slot is still taken after the pull, since a mount may still
 * be standing on the device and an attachment would put another file under
 * it.
 */
static void pull_mode(void)
{
    char path[PATH_CAP], hello[PATH_CAP];
    int opens = 0, rc;

    mount_images("vfat", "mounting the stick's volume");
    rc = control_on(attach_a, join(path, images_dir, "fat.img"));
    if (rc != 0) {
        fail_with("attaching an image on a stick", rc);
    }
    rc = mount_at((long)node_a, (long)mount_dir, (long)"vfat", 0, 0);
    if (rc != 0) {
        fail_with("mounting an image on a stick", rc);
    }
    expect_file(join(hello, mount_dir, "hello.txt"), "hello from inside an image");
    for (;;) {
        char one[8];
        int fd = syscall3(SYS_OPEN, (long)hello, O_RDONLY, 0), got;

        if (fd < 0) {
            break;
        }
        if (opens++ == 1) {
            puts("LOOPPROBE HOLDING");
        }
        got = syscall3(SYS_READ, fd, (long)one, (int)sizeof(one));
        syscall1(SYS_CLOSE, fd);
        if (got < 0) {
            break;
        }
        syscall0(SYS_SCHED_YIELD);
    }
    /* The slot is still taken. */
    if (control_on(attach_a, path) != -EBUSY) {
        fail("a device whose stick went took another file");
    }
    rc = unmount((long)mount_dir, 0);
    if (rc != 0) {
        fail_with("unmounting an image whose stick has gone", rc);
    }
    rc = control(detach_a);
    if (rc != 0 && rc != -ENOENT) {
        fail_with("detaching a device whose stick has gone", rc);
    }
    rc = unmount_images();
    if (rc != 0) {
        fail_with("unmounting the volume of a stick that has gone", rc);
    }
    puts("LOOPPROBE OK (pull)");
    sys_exit(0);
}

/** Attach `image` to device A and mount it as `type`, with /proc/loop not
 * naming the device before and naming the image after, and read its file
 * through a private mapping and then with read(2). */
static void attach_rows(const char *type)
{
    char lines[512], path[PATH_CAP];
    int rc;

    loop_lines(lines, (int)sizeof(lines));
    if (device_line(lines, line_a) != 0) {
        fail("/proc/loop names the device before it is attached");
    }
    rc = control_on(attach_a, image);
    if (rc != 0) {
        fail_with("attaching the image", rc);
    }
    loop_lines(lines, (int)sizeof(lines));
    if (!line_has(device_line(lines, line_a), shown)) {
        fail("/proc/loop does not name what was attached");
    }
    rc = mount_at((long)node_a, (long)mount_dir, (long)type, 0, 0);
    if (rc != 0) {
        fail_with("mounting the image through the device", rc);
    }
    expect_mapped(join(path, mount_dir, "hello.txt"), "hello from inside an image");
    expect_file(path, "hello from inside an image");
}

/** Everything the attachment of `image` refuses, as the file's opening
 * comment lists it, and the commands the control node does not take. */
static void refusal_rows(void)
{
    char path[PATH_CAP];
    int fd, rc;

    /* The same file, asked for on another device. */
    if (control_on(attach_b, image) != -EBUSY) {
        fail("the same file was attached twice");
    }
    /* Another file, on the device that is taken. */
    if (control_on(attach_a, join(path, images_dir, "odd2.img")) != -EBUSY) {
        fail("the device that is taken took another file");
    }
    fd = syscall3(SYS_OPEN, (long)image, O_WRONLY, 0);
    if (fd < 0) {
        fail_with("opening a file that backs a device", fd);
    }
    rc = syscall3(SYS_WRITE, fd, (long)"x", 1);
    if (rc != -EBUSY) {
        fail_with("writing a file that backs a device", rc);
    }
    /* Linux lets the length of a loop's file change; here every file
     * system but KRFFS does, and the images' volume is FAT32 or a tmpfs. */
    rc = syscall3(SYS_LSEEK, fd, 0, SEEK_END);
    if (rc <= 0 || syscall2(SYS_FTRUNCATE, fd, rc) != 0) {
        fail_with("truncating a file that backs a device to its own length", rc);
    }
    syscall1(SYS_CLOSE, fd);
    if (control(detach_a) != -EBUSY) {
        fail("a device with a mount on it was detached");
    }
    if (unmount((long)images_dir, 0) != -EBUSY) {
        fail("the volume holding an attached file was unmounted");
    }
    if (control_on(attach_b, images_dir) != -EINVAL) {
        fail("a directory was attached");
    }
    if (control_on(attach_b, join(path, images_dir, "odd.bin")) != -EINVAL) {
        fail("a file whose length is not whole sectors was attached");
    }
    /* A device on a file on a device on a disk is ELOOP; on a tmpfs, which
     * has no device, the chain is one device long, which is allowed. */
    rc = control_on(attach_b, join(path, mount_dir, "inner.bin"));
    if (!native_images && rc != -ELOOP) {
        fail("a device was made to stand on another one");
    }
    if (native_images && (rc != 0 || control(detach_b) != 0)) {
        fail_with("a device on a file of a device on a tmpfs", rc);
    }

    /* The control node refuses malformed commands. */
    join(path, images_dir, "fat.img");
    if (control("detach x") != -EINVAL) {
        fail("a detach with no number was taken");
    }
    if (control(detach_junk) != -EINVAL) {
        fail("a detach with something after the number was taken");
    }
    if (control(detach_word) != -EINVAL) {
        fail("a detach with a word after the number was taken");
    }
    if (control_on("attach 9 ", path) != -EINVAL) {
        fail("a device that does not exist was attached");
    }
    if (control_on("attach 4294967296 ", path) != -EINVAL) {
        fail("a number larger than any device was attached");
    }
    if (control_on("nonsense 0 ", path) != -EINVAL) {
        fail("a command that is not one was taken");
    }
}

/** A read-only device makes a read-only mount, and /proc/loop says so. */
static void read_only_rows(void)
{
    char lines[512], path[PATH_CAP];
    int rc;

    if (control_on(attach_ro_b, join(path, images_dir, "odd2.img")) != 0) {
        fail("attaching a device that will not write");
    }
    loop_lines(lines, (int)sizeof(lines));
    if (!line_has(device_line(lines, line_b), " ro ")) {
        fail("/proc/loop does not say the device will not write");
    }
    syscall2(SYS_MKDIR, (long)ro_dir, 0755);
    rc = mount_at((long)node_b, (long)ro_dir, (long)"vfat", 0, 0);
    if (rc != 0) {
        fail_with("mounting a device that will not write", rc);
    }
    rc = syscall3(SYS_OPEN, (long)join(path, ro_dir, "made.txt"), O_WRONLY | O_CREAT, 0644);
    if (rc != -EROFS) {
        fail_with("creating a file on a device that will not write", rc);
    }
    rc = unmount((long)ro_dir, 0);
    if (rc != 0) {
        fail_with("unmounting the device that will not write", rc);
    }
    if (control(detach_b) != 0) {
        fail("detaching the device that will not write");
    }
}

/* The bytes of each filler file of the exchange rows: 8 KiB, two blocks of
 * the buffer cache, so that the directory made after a filler lies in a
 * block of the cache apart from the one made before it. */
static char exchange_filler[8192];

/** Make the file `name` of the directory `dir` with the bytes of `text`. */
static void make_text(const char *dir, const char *name, const char *text, int len)
{
    char path[PATH_CAP];
    int fd = syscall3(SYS_OPEN, (long)join(path, dir, name), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        fail_with("creating a file for the exchange", fd);
    }
    if (syscall3(SYS_WRITE, fd, (long)text, len) != len) {
        fail("writing a file for the exchange");
    }
    syscall1(SYS_CLOSE, fd);
}

/** Make the directory `name` of `dir` into `out`, of PATH_CAP bytes. */
static void make_dir(char *out, const char *dir, const char *name)
{
    int rc = syscall2(SYS_MKDIR, (long)join(out, dir, name), 0755);

    if (rc != 0) {
        fail_with("making a directory for the exchange", rc);
    }
}

/**
 * RENAME_EXCHANGE of two directories across parents on the FAT image
 * mounted through the device, whose file lives on the FAT volume: D1 in the
 * root and A/D2, with a filler before each, so that both short entries and
 * both `..` entries lie in four blocks of the cache. The exchange changes
 * the four, and each write goes down through the loop device into the file
 * system of the volume, which takes buffers of its own while the exchange
 * goes on; the read-back rows find each directory's file under the other's
 * name after the image is mounted again (`exchanged_names`).
 */
static void exchange_rows(void)
{
    char a[PATH_CAP], d1[PATH_CAP], d2[PATH_CAP];
    int rc;

    make_text(mount_dir, "F0.BIN", exchange_filler, (int)sizeof(exchange_filler));
    make_dir(a, mount_dir, "A");
    make_text(mount_dir, "F1.BIN", exchange_filler, (int)sizeof(exchange_filler));
    make_dir(d1, mount_dir, "D1");
    make_text(a, "F2.BIN", exchange_filler, (int)sizeof(exchange_filler));
    make_dir(d2, a, "D2");
    make_text(d1, "one.txt", "one", 3);
    make_text(d2, "two.txt", "two", 3);
    syscall0(SYS_SYNC);
    rc = syscall5(SYS_RENAMEAT2_, AT_FDCWD, (long)d1, AT_FDCWD, (long)d2, RENAME_EXCHANGE);
    if (rc != 0) {
        fail_with("RENAME_EXCHANGE of two directories across parents on the image", rc);
    }
}

/**
 * sys_immutable, on the image mounted again with it: root, which holds
 * CAP_LINUX_IMMUTABLE, gives a file the system bit with
 * FAT_IOCTL_SET_ATTRIBUTES, which FAT_IOCTL_GET_ATTRIBUTES reads back; an
 * open of the file for writing, a chmod, an unlink and a rename are then
 * EPERM, as Linux's S_IMMUTABLE makes them, and once the bit is cleared the
 * unlink goes through. The image stays mounted with the option.
 */
static void immutable_rows(void)
{
    char path[PATH_CAP], moved[PATH_CAP];
    unsigned int attr = FAT_ARCHIVE | FAT_SYSTEM;
    int fd, rc;

    rc = unmount((long)mount_dir, 0);
    if (rc != 0) {
        fail_with("unmounting the image to mount it with sys_immutable", rc);
    }
    rc = mount_at((long)node_a, (long)mount_dir, (long)"vfat", 0, (long)"sys_immutable");
    if (rc != 0) {
        fail_with("mounting the image with sys_immutable", rc);
    }
    make_text(mount_dir, "SYS.BIN", "system", 6);
    join(path, mount_dir, "SYS.BIN");
    join(moved, mount_dir, "MOVED.BIN");
    fd = syscall2(SYS_OPEN, (long)path, O_RDONLY);
    if (fd < 0) {
        fail_with("opening the file to give it the system bit", fd);
    }
    rc = syscall3(SYS_IOCTL, fd, FAT_IOCTL_SET_ATTRIBUTES, (long)&attr);
    attr = 0;
    if (rc == 0) {
        rc = syscall3(SYS_IOCTL, fd, FAT_IOCTL_GET_ATTRIBUTES, (long)&attr);
    }
    if (rc != 0 || attr != (FAT_ARCHIVE | FAT_SYSTEM)) {
        fail_with("FAT_IOCTL_SET_ATTRIBUTES giving a file the system bit", rc);
    }
    if (syscall2(SYS_OPEN, (long)path, O_WRONLY) != -EPERM ||
        syscall2(SYS_CHMOD, (long)path, 0444) != -EPERM ||
        syscall1(SYS_UNLINK, (long)path) != -EPERM ||
        syscall2(SYS_RENAME, (long)path, (long)moved) != -EPERM) {
        fail("a system file under sys_immutable takes a write, chmod, unlink or rename");
    }
    attr = FAT_ARCHIVE;
    rc = syscall3(SYS_IOCTL, fd, FAT_IOCTL_SET_ATTRIBUTES, (long)&attr);
    syscall1(SYS_CLOSE, fd);
    if (rc != 0) {
        fail_with("clearing the system bit", rc);
    }
    rc = syscall1(SYS_UNLINK, (long)path);
    if (rc != 0) {
        fail_with("unlinking the file once it has no system bit", rc);
    }
}

/** The names the exchange rows left, on the image mounted again. */
static void exchanged_names(void)
{
    char a[PATH_CAP], d[PATH_CAP], path[PATH_CAP];

    expect_file(join(path, join(d, mount_dir, "D1"), "two.txt"), "two");
    expect_file(join(path, join(d, join(a, mount_dir, "A"), "D2"), "one.txt"), "one");
}

/** Whether the `count` bytes at `p` are each `want`. */
static int all_bytes(const char *p, int count, char want)
{
    int i;

    for (i = 0; i < count; i++) {
        if (p[i] != want) {
            return 0;
        }
    }
    return 1;
}

/**
 * A truncation of a file a device stands on, as Linux takes it: cut.img, 8
 * sectors of 'Z' on the images' volume, is attached to B and cut by a
 * sector, and the device, whose size stays what it was, reads the one
 * request of its 8 sectors as zeros whole, the 7 the file still has among
 * them, as Linux 6.12's loop zero-fills a request whose read of its file
 * came up short. A
 * truncate by path and an open with O_TRUNC go through as well. The device
 * is read once, after the cut, so that no cached block of it predates it.
 */
static void length_rows(void)
{
    char path[PATH_CAP], block[4096];
    int fd, rc;

    for (fd = 0; fd < (int)sizeof(block); fd++) {
        block[fd] = 'Z';
    }
    fd = syscall3(SYS_OPEN, (long)join(path, images_dir, "cut.img"), O_WRONLY | O_CREAT | O_TRUNC,
                  0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block)) != (int)sizeof(block)) {
        fail("making the file to cut");
    }
    syscall1(SYS_CLOSE, fd);
    if (control_on(attach_b, path) != 0) {
        fail("attaching the file to cut");
    }
    fd = syscall3(SYS_OPEN, (long)path, O_WRONLY, 0);
    rc = fd < 0 ? fd : syscall2(SYS_FTRUNCATE, fd, 3584);
    if (rc != 0) {
        fail_with("cutting a sector off a file that backs a device", rc);
    }
    syscall1(SYS_CLOSE, fd);
    fd = syscall3(SYS_OPEN, (long)node_b, O_RDONLY, 0);
    rc = fd < 0 ? fd : syscall3(SYS_READ, fd, (long)block, (int)sizeof(block));
    if (rc != (int)sizeof(block)) {
        fail_with("reading a device whose file was cut", rc);
    }
    syscall1(SYS_CLOSE, fd);
    if (!all_bytes(block, (int)sizeof(block), 0)) {
        fail("the device does not read a request past its file's new end as zeros whole");
    }
    rc = syscall2(SYS_TRUNCATE, (long)path, 4096);
    if (rc != 0) {
        fail_with("truncating a file that backs a device by its path", rc);
    }
    fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        fail_with("an open that truncates a file that backs a device", fd);
    }
    syscall1(SYS_CLOSE, fd);
    if (control(detach_b) != 0) {
        fail("detaching the file that was cut");
    }
    if (syscall1(SYS_UNLINK, (long)path) != 0) {
        fail("removing the file that was cut");
    }
}

/**
 * The root of a mounted KRFFS image keeps what chmod, chown and utimensat
 * give it, as any directory does: its mode but set-user-ID, whose place
 * holds the volume's dirty flag, its owner and its times. Each is put back,
 * so that the read-back rows and the host's fsck find the image as it was.
 */
static void krffs_root_rows(void)
{
    unsigned char st[KSTAT_SIZE];
    long times[4];
    int rc;

    rc = syscall3(SYS_CHMOD, (long)mount_dir, 05710, 0);
    if (rc == 0) {
        rc = syscall2(SYS_STAT, (long)mount_dir, (long)st);
    }
    if (rc != 0 || *(unsigned int *)(st + KSTAT_MODE) != (040000u | 01710u)) {
        fail_with("a KRFFS volume's root does not keep its mode but set-user-ID", rc);
    }
    rc = syscall3(SYS_CHOWN, (long)mount_dir, 1000, 1001);
    if (rc == 0) {
        rc = syscall2(SYS_STAT, (long)mount_dir, (long)st);
    }
    if (rc != 0 || *(unsigned int *)(st + KSTAT_UID) != 1000u ||
        *(unsigned int *)(st + KSTAT_GID) != 1001u) {
        fail_with("a KRFFS volume's root does not keep its owner", rc);
    }
    times[0] = 1000000;
    times[1] = 0;
    times[2] = 2000000;
    times[3] = 0;
    rc = syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)mount_dir, (long)times, 0);
    if (rc == 0) {
        rc = syscall2(SYS_STAT, (long)mount_dir, (long)st);
    }
    if (rc != 0 || *(unsigned int *)(st + KSTAT_MTIME) != 2000000u) {
        fail_with("a KRFFS volume's root does not keep its times", rc);
    }
    if (syscall3(SYS_CHOWN, (long)mount_dir, 0, 0) != 0 ||
        syscall3(SYS_CHMOD, (long)mount_dir, 0755, 0) != 0) {
        fail("putting the KRFFS volume's root back");
    }
}

/** A write inside the image mounted as `type`, read back after a detach and
 * a fresh attachment, after which the image is an ordinary file again. */
static void read_back_rows(const char *type)
{
    char lines[512], made[PATH_CAP];
    int fd, rc;

    fd = syscall3(SYS_OPEN, (long)join(made, mount_dir, "made.txt"), O_WRONLY | O_CREAT | O_TRUNC,
                  0644);
    if (fd < 0) {
        fail_with("creating a file inside the image", fd);
    }
    if (syscall3(SYS_WRITE, fd, (long)"written through a loop device\n", 30) != 30) {
        fail("writing inside the image");
    }
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync inside the image");
    }
    syscall1(SYS_CLOSE, fd);
    if (streq(type, "vfat")) {
        exchange_rows();
        immutable_rows();
    }
    rc = unmount((long)mount_dir, 0);
    if (rc != 0) {
        fail_with("unmounting the image", rc);
    }
    rc = control(detach_a);
    if (rc != 0) {
        fail_with("detaching", rc);
    }
    loop_lines(lines, (int)sizeof(lines));
    if (device_line(lines, line_a) != 0) {
        fail("/proc/loop still names the device after the detach");
    }
    rc = control_on(attach_a, image);
    if (rc != 0) {
        fail_with("attaching the image again", rc);
    }
    rc = mount_at((long)node_a, (long)mount_dir, (long)type, MS_RDONLY, 0);
    if (rc != 0) {
        fail_with("mounting the image again", rc);
    }
    expect_file(made, "written through a loop device\n");
    if (streq(type, "vfat")) {
        exchanged_names();
    }
    rc = unmount((long)mount_dir, 0);
    if (rc != 0) {
        fail_with("unmounting the image the second time", rc);
    }
    if (control(detach_a) != 0) {
        fail("detaching the second time");
    }
    /* The file the device stood on is an ordinary file again. */
    fd = syscall3(SYS_OPEN, (long)image, O_WRONLY, 0);
    if (fd < 0) {
        fail_with("opening the image after the detach", fd);
    }
    syscall1(SYS_CLOSE, fd);
}

/** Read the attached image after a change to its name. */
static void attached_contents(const char *type)
{
    char path[PATH_CAP];
    int rc = mount_at((long)node_a, (long)mount_dir, (long)type, MS_RDONLY, 0);

    if (rc != 0) {
        fail_with("mounting the attached image after unlink", rc);
    }
    expect_file(join(path, mount_dir, "hello.txt"), "hello from inside an image");
    rc = unmount((long)mount_dir, 0);
    if (rc != 0) {
        fail_with("unmounting the attached image after unlink", rc);
    }
}

/** A tmpfs keeps the attached inode through unlink and replacement. */
static void native_copy_names(const char *type, const char *ram)
{
    char spare[PATH_CAP];
    int fd, rc;

    rc = syscall1(SYS_UNLINK, (long)ram);
    if (rc != 0) {
        fail_with("unlinking the attached tmpfs image", rc);
    }
    attached_contents(type);
    if (control(detach_a) != 0) {
        fail("detaching the unlinked tmpfs image");
    }
    copy_file(image, ram, "opening the image to copy it again", "copying the image again",
              "reading the image to copy it again");
    rc = control_on(attach_a, ram);
    if (rc != 0) {
        fail_with("attaching the second tmpfs image", rc);
    }
    fd = syscall3(SYS_OPEN, (long)join(spare, copy_dir, "spare.img"), O_WRONLY | O_CREAT | O_TRUNC,
                  0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"x", 1) != 1 || syscall1(SYS_CLOSE, fd) != 0) {
        fail("making a file to replace the attached tmpfs image");
    }
    rc = syscall2(SYS_RENAME, (long)spare, (long)ram);
    if (rc != 0) {
        fail_with("renaming over the attached tmpfs image", rc);
    }
}

/**
 * The image copied into the copy's directory, the root, which is memory and
 * has no device under it, or a tmpfs mounted on `copy=`, mounted as `type`.
 * On the KRFFS root an open that truncates the file under the device, an
 * unlink of it and a rename over it are refused while the device stands on
 * it; a tmpfs allows the last two, as Linux does.
 */
static void copy_rows(const char *type)
{
    char ram[PATH_CAP], spare[PATH_CAP], path[PATH_CAP];
    int fd, rc;

    if (copy_dir[0] != '\0') {
        syscall2(SYS_MKDIR, (long)copy_dir, 0755);
        rc = mount_at((long)"tmpfs", (long)copy_dir, (long)"tmpfs", 0, (long)"size=8388608");
        if (rc != 0) {
            fail_with("mounting the tmpfs of the copy", rc);
        }
    }
    copy_file(image, join(ram, copy_dir, "ram.img"), "opening the image to copy it onto the root",
              "copying the image onto the root", "reading the image to copy it");
    rc = control_on(attach_a, ram);
    if (rc != 0) {
        fail_with("attaching an image that lives in memory", rc);
    }
    /* On the KRFFS root a change of length may move the file, and an open
     * that truncates is one; a tmpfs would take it, as Linux's does. */
    if (copy_dir[0] == '\0' && syscall3(SYS_OPEN, (long)ram, O_WRONLY | O_TRUNC, 0) != -EBUSY) {
        fail("an attached image on the KRFFS root was truncated by an open");
    }
    if (copy_dir[0] == '\0') {
        if (syscall1(SYS_UNLINK, (long)ram) != -EBUSY) {
            fail("the file under an attached device was unlinked");
        }
        fd = syscall3(SYS_OPEN, (long)join(spare, copy_dir, "spare.img"),
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fail("making a file to rename over the attached one");
        }
        syscall3(SYS_WRITE, fd, (long)"x", 1);
        syscall1(SYS_CLOSE, fd);
        if (syscall2(SYS_RENAME, (long)spare, (long)ram) != -EBUSY) {
            fail("the file under an attached device was renamed over");
        }
        if (syscall1(SYS_UNLINK, (long)spare) != 0) {
            fail("removing the spare file");
        }
    } else {
        native_copy_names(type, ram);
    }
    rc = mount_at((long)node_a, (long)mount_dir, (long)type, MS_RDONLY, 0);
    if (rc != 0) {
        fail_with("mounting an image that lives in memory", rc);
    }
    expect_file(join(path, mount_dir, "hello.txt"), "hello from inside an image");
    rc = unmount((long)mount_dir, 0);
    if (rc != 0) {
        fail_with("unmounting the image in memory", rc);
    }
    if (control(detach_a) != 0) {
        fail("detaching the image in memory");
    }
    if (syscall1(SYS_UNLINK, (long)ram) != 0) {
        fail("removing the copy from the root");
    }
    if (copy_dir[0] != '\0' && unmount((long)copy_dir, 0) != 0) {
        fail("unmounting the tmpfs of the copy");
    }
}

/**
 * The type rows, on the image of `type`. The same file asked for on another
 * device is refused, one file, one device, whatever it is called; and
 * another file on the device that is taken is refused, since the slot is
 * taken. A device on a file that lives on a device on a file is deeper than
 * the four buffers a process may hold can serve, and is ELOOP. The control
 * node refuses a command with no number, one whose number is not a device,
 * one with something after the number, and a command it does not know. A
 * device attached read-only makes the mount over it read-only whatever the
 * mount asked for, and /proc/loop says so. The image is then copied, and a
 * loop device reads its file through the file system that has it, so where
 * the file lives does not matter. The root keeps no count on a node of its
 * own: its rule for a file somebody holds is the descriptor tables, and an
 * attachment is not one, so the copy is refused every change while the
 * device stands on it.
 */
static void type_mode(const char *type)
{
    int rc, i;

    join(image, images_dir,
         streq(type, "ext2")    ? "ext2.img"
         : streq(type, "krffs") ? "krffs.img"
                                : "fat.img");
    for (i = 0; i < 63 && image[i]; i++) {
        shown[i] = image[i];
    }
    shown[i] = '\0';
    mount_images("vfat", "mounting the volume that holds the images");
    attach_rows(type);
    refusal_rows();
    read_only_rows();
    length_rows();
    if (streq(type, "krffs")) {
        krffs_root_rows();
    }
    read_back_rows(type);
    copy_rows(type);
    ioctl_rows(type);
    rc = unmount_images();
    if (rc != 0) {
        fail_with("unmounting the volume that holds the images", rc);
    }
    probe_line("LOOPPROBE OK (", type, ")");
    sys_exit(0);
}

/** Set devices A and B, as the comment of their strings says. A native
 * machine with fewer than two devices free fails the probe. */
static void choose_devices(void)
{
    char lines[512], name[] = "loop0 ", pick[2] = {'0', '1'};
    int found = 0, i;

    if (native_images) {
        loop_lines(lines, (int)sizeof(lines));
        for (i = 0; i < 4 && found < 2; i++) {
            name[4] = (char)('0' + i);
            if (device_line(lines, name) == 0) {
                pick[found++] = name[4];
            }
        }
        if (found < 2) {
            fail("fewer than two loop devices are free");
        }
    }
    attach_a[7] = detach_a[7] = node_a[9] = line_a[4] = pick[0];
    detach_junk[7] = detach_word[7] = pick[0];
    attach_b[7] = attach_ro_b[10] = detach_b[7] = node_b[9] = line_b[4] = pick[1];
}

/** The probe, entered with `sp` at argc: the mode or the type the first
 * argument without `=` names, `vfat` with none, in the places the
 * arguments name. */
void probe_main(unsigned long *sp)
{
    const char *type, *copy;

    probe_args((int)sp[0], (char **)(sp + 1));
    type = probe_word(1) != 0 ? probe_word(1) : "vfat";
    native_images = probe_arg("images", 0) != 0;
    absolute(images_dir, probe_arg("images", "/m"));
    absolute(mount_dir, probe_arg("mount", "/l"));
    absolute(ro_dir, probe_arg("ro", "/r"));
    copy = probe_arg("copy", "");
    if (copy[0] != '\0') {
        absolute(copy_dir, copy);
    }
    choose_devices();
    syscall2(SYS_MKDIR, (long)images_dir, 0755);
    syscall2(SYS_MKDIR, (long)mount_dir, 0755);
    if (streq(type, "durable")) {
        durable_mode();
    }
    if (streq(type, "ext2back")) {
        ext2back_mode();
    }
    if (streq(type, "pull")) {
        pull_mode();
    }
    type_mode(type);
}
