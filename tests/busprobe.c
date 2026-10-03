/**
 * Every IDE drive, and a disk past 128 GiB.
 *
 * Mode `four`: the suite attaches a disk of a different size on each of
 * the four IDE positions, each with one FAT32 partition. This requires
 * `/proc/disk` to list them as `ata` 0 to 3 with their sizes, and mounts
 * each disk's partition, writes a file and reads it back.
 *
 * Mode `big`: a sparse disk of 130 GiB on the primary master with one
 * FAT32 partition starting past sector 2^28, which the 28-bit commands
 * cannot address. The disk must be listed at its full size, the partition
 * mounted, a file written and read back, so the 48-bit commands moved it.
 *
 * Prints "BUSPROBE OK" or "BUSPROBE FAIL: reason". Freestanding; direct system
 * calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

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
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"BUSPROBE FAIL: ", 15);
    puts(why);
    syscall2(SYS_EXIT, 1, 0);
}

static int has_line(const char *text, const char *line)
{
    const char *p;

    for (p = text; *p; p++) {
        const char *a = p, *b = line;

        if (p != text && p[-1] != '\n') {
            continue;
        }
        while (*b && *a == *b) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return 1;
        }
    }
    return 0;
}

static int slurp(const char *path, char *buf, int cap)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n, total = 0;

    if (fd < 0) {
        return -1;
    }
    while (total < cap - 1) {
        n = syscall3(SYS_READ, fd, (long)(buf + total), cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    syscall2(SYS_CLOSE, fd, 0);
    buf[total] = 0;
    return total;
}

static void write_and_read_back(const char *path, const char *text)
{
    static char back[64];
    int fd, n = (int)strlen(text), got;

    fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail(path);
    }
    if (syscall3(SYS_WRITE, fd, (long)text, n) != n) {
        fail(path);
    }
    syscall2(SYS_CLOSE, fd, 0);
    fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        fail(path);
    }
    got = syscall3(SYS_READ, fd, (long)back, (int)sizeof(back));
    syscall2(SYS_CLOSE, fd, 0);
    if (got != n) {
        fail(path);
    }
    for (fd = 0; fd < n; fd++) {
        if (back[fd] != text[fd]) {
            fail(path);
        }
    }
}

/* Mount `node` at `at` as FAT32, write `text` as `name` there, unmount. */
static void mount_write(const char *node, const char *at, const char *name, const char *text)
{
    static char path[64];
    int i = 0, k;

    syscall2(SYS_MKDIR, (long)at, 0755);
    if (syscall5(SYS_MOUNT, (long)node, (long)at, (long)"vfat", 0, 0) != 0) {
        fail(node);
    }
    for (k = 0; at[k]; k++) {
        path[i++] = at[k];
    }
    path[i++] = '/';
    for (k = 0; name[k]; k++) {
        path[i++] = name[k];
    }
    path[i] = 0;
    write_and_read_back(path, text);
    if (syscall2(SYS_UMOUNT2, (long)at, 0) != 0) {
        fail(at);
    }
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    static char text[8192];
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int big = argc >= 2 && argv[1][0] == 'b';

    if (slurp("/proc/disk", text, (int)sizeof(text)) <= 0) {
        fail("/proc/disk");
    }
    if (big) {
        if (!has_line(text, "disk 0 ata 0 272629760 mbr ")) {
            fail("the 130 GiB disk is not listed at its size");
        }
        if (!has_line(text, "part 0 1 268437504 65536 mbr:0c")) {
            fail("the partition past 2^28 sectors");
        }
        mount_write("/dev/disk0p1", "/mnt0", "far.txt", "written past 128 GiB\n");
        puts("BUSPROBE OK");
        syscall2(SYS_EXIT, 0, 0);
    }
    if (!has_line(text, "disk 0 ata 0 32768 mbr ") || !has_line(text, "disk 1 ata 1 40960 mbr ") ||
        !has_line(text, "disk 2 ata 2 49152 mbr ") || !has_line(text, "disk 3 ata 3 57344 mbr ")) {
        fail("the four IDE drives with their sizes");
    }
    mount_write("/dev/disk0p1", "/mnt0", "drive.txt", "primary master\n");
    mount_write("/dev/disk1p1", "/mnt1", "drive.txt", "primary slave\n");
    mount_write("/dev/disk2p1", "/mnt2", "drive.txt", "secondary master\n");
    mount_write("/dev/disk3p1", "/mnt3", "drive.txt", "secondary slave\n");
    puts("BUSPROBE OK");
    syscall2(SYS_EXIT, 0, 0);
}
