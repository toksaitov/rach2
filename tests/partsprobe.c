/**
 * Four disks with four kinds of table, all read by the partition service.
 *
 * The suite attaches, on one AHCI controller: an MBR disk whose entries are
 * a KRFFS partition, a FAT32 one, an extended one holding three logical
 * partitions, and a fourth primary; a GPT disk with six partitions and
 * labels; a copy of that GPT whose primary header is damaged; and a blank
 * disk. It boots with `rach2.diskdiag` so `/dev/diskN` exists for each.
 * This requires `/proc/disk` to describe every disk and partition as
 * built, the nodes to exist for the partitions and not for the extended
 * entry or an absent one, the backup GPT to have served for the damaged
 * disk, a logical FAT32 partition and a GPT one to mount and take a write,
 * and the whole-disk node's `BLKRRPART` to be refused while a partition is
 * mounted and to read the table again once it is not.
 *
 * Prints "PARTSPROBE OK" or "PARTSPROBE FAIL: reason". Freestanding; int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_MOUNT   21
#define SYS_MKDIR   39
#define SYS_IOCTL   54
#define SYS_UMOUNT2 52
#define SYS_STAT64  195

#define O_RDONLY  0
#define O_WRONLY  1
#define O_CREAT   0100
#define O_TRUNC   01000
#define EBUSY     16
#define BLKRRPART 0x125F

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
    sys3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    sys3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static void fail(const char *why)
{
    sys3(SYS_WRITE, 1, (int)(size_t)"PARTSPROBE FAIL: ", 17);
    puts(why);
    sys2(SYS_EXIT, 1, 0);
}

static int xstat(const char *path)
{
    char st[104];

    st[0] = 0;
    return sys2(SYS_STAT64, (int)(size_t)path, (int)(size_t)st);
}

/* Whether `text` has a line beginning with `line`. */
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

/* Whether `text` contains `needle` anywhere. */
static int has_text(const char *text, const char *needle)
{
    const char *p;

    for (p = text; *p; p++) {
        const char *a = p, *b = needle;

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
    int fd = sys3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0);
    int n, total = 0;

    if (fd < 0) {
        return -1;
    }
    while (total < cap - 1) {
        n = sys3(SYS_READ, fd, (int)(size_t)(buf + total), cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    sys2(SYS_CLOSE, fd, 0);
    buf[total] = 0;
    return total;
}

static void write_and_read_back(const char *path, const char *text)
{
    static char back[64];
    int fd, n = (int)strlen(text), got;

    fd = sys3(SYS_OPEN, (int)(size_t)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail(path);
    }
    if (sys3(SYS_WRITE, fd, (int)(size_t)text, n) != n) {
        fail(path);
    }
    sys2(SYS_CLOSE, fd, 0);
    fd = sys3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0);
    if (fd < 0) {
        fail(path);
    }
    got = sys3(SYS_READ, fd, (int)(size_t)back, (int)sizeof(back));
    sys2(SYS_CLOSE, fd, 0);
    if (got != n) {
        fail(path);
    }
    for (fd = 0; fd < n; fd++) {
        if (back[fd] != text[fd]) {
            fail(path);
        }
    }
}

static int mount_vfat(const char *src, const char *at)
{
    sys2(SYS_MKDIR, (int)(size_t)at, 0755);
    return sys5(SYS_MOUNT, (int)(size_t)src, (int)(size_t)at, (int)(size_t)"vfat", 0, 0);
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
    static char log[65536];
    int fd;

    (void)sp;
    if (slurp("/proc/disk", text, (int)sizeof(text)) <= 0) {
        fail("/proc/disk");
    }

    /* Disk 0: the MBR as built, numbered by entry, the extended one absent. */
    if (!has_line(text, "disk 0 ahci 0 131072 mbr ")) {
        fail("disk 0 is not the MBR disk");
    }
    if (!has_line(text, "part 0 1 2048 98304 mbr:7f") ||
        !has_line(text, "part 0 2 100352 8192 mbr:0c") ||
        !has_line(text, "part 0 4 122880 4096 mbr:83")) {
        fail("disk 0's primary partitions");
    }
    if (has_text(text, "part 0 3 ")) {
        fail("the extended entry is listed as a partition");
    }
    if (!has_line(text, "part 0 5 108545 4096 mbr:83") ||
        !has_line(text, "part 0 6 112642 4096 mbr:0c") ||
        !has_line(text, "part 0 7 116739 4096 mbr:83")) {
        fail("disk 0's logical partitions");
    }
    if (has_text(text, "part 0 8 ")) {
        fail("a logical partition past the chain");
    }

    /* Disk 1: the GPT with its six entries, GUIDs and labels. */
    if (!has_line(text, "disk 1 ahci 1 65536 gpt ")) {
        fail("disk 1 is not the GPT disk");
    }
    if (!has_line(text, "part 1 1 64 2048 ") || !has_line(text, "part 1 6 50176 1024 ")) {
        fail("disk 1's first and sixth partitions");
    }
    if (!has_text(text, "part 1 3 8192 8192 6a696867-6c6b-6e6d-6f70-717273747576 DATA3\n")) {
        fail("disk 1's third partition with its GUID and label");
    }
    if (has_text(text, "part 1 7 ")) {
        fail("a seventh partition on disk 1");
    }

    /* Disk 2: the damaged copy, read from its backup. */
    if (!has_line(text, "disk 2 ahci 2 65536 gpt ") ||
        !has_text(text, "part 2 3 8192 8192 6a696867-6c6b-6e6d-6f70-717273747576 DATA3\n")) {
        fail("disk 2's table from the backup header");
    }
    if (slurp("/dev/bootlog", log, (int)sizeof(log)) <= 0) {
        fail("/dev/bootlog");
    }
    if (!has_text(log, "disk2: the primary GPT is damaged; the backup served")) {
        fail("the record that the backup served for disk 2");
    }
    if (has_text(log, "disk1: the primary GPT is damaged")) {
        fail("disk 1's primary was taken for damaged");
    }

    /* Disk 3: blank. */
    if (!has_line(text, "disk 3 ahci 3 16384 none ")) {
        fail("disk 3 is not the blank disk");
    }
    if (has_text(text, "part 3 ")) {
        fail("a partition on the blank disk");
    }

    /* The nodes. */
    if (xstat("/dev/disk0p1") != 0 || xstat("/dev/disk0p2") != 0 || xstat("/dev/disk0p4") != 0 ||
        xstat("/dev/disk0p5") != 0 || xstat("/dev/disk0p6") != 0 || xstat("/dev/disk0p7") != 0) {
        fail("disk 0's partition nodes");
    }
    if (xstat("/dev/disk0p3") == 0 || xstat("/dev/disk0p8") == 0) {
        fail("a node for the extended entry or past the chain");
    }
    if (xstat("/dev/disk1p6") != 0 || xstat("/dev/disk1p7") == 0) {
        fail("disk 1's sixth node, and no seventh");
    }
    if (xstat("/dev/disk2p6") != 0 || xstat("/dev/disk3") != 0 || xstat("/dev/disk3p1") == 0) {
        fail("disk 2's nodes, the blank disk's node and no partition on it");
    }

    /* A logical partition and a GPT partition, mounted and written. */
    if (mount_vfat("/dev/disk0p6", "/mnt6") != 0) {
        fail("mounting the logical FAT32 partition");
    }
    if (mount_vfat("/dev/disk1p3", "/mnt3") != 0) {
        fail("mounting the GPT FAT32 partition");
    }
    write_and_read_back("/mnt6/logical.txt", "logical six says hello\n");
    write_and_read_back("/mnt3/gpt.txt", "gpt three says hello\n");

    /* The table read again: refused under a mount, done once it is gone. */
    fd = sys3(SYS_OPEN, (int)(size_t)"/dev/disk1", O_RDONLY, 0);
    if (fd < 0) {
        fail("opening /dev/disk1");
    }
    if (sys3(SYS_IOCTL, fd, BLKRRPART, 0) != -EBUSY) {
        fail("BLKRRPART was not refused under a mounted partition");
    }
    if (sys2(SYS_UMOUNT2, (int)(size_t)"/mnt3", 0) != 0) {
        fail("unmounting the GPT partition");
    }
    if (sys3(SYS_IOCTL, fd, BLKRRPART, 0) != 0) {
        fail("BLKRRPART after the unmount");
    }
    sys2(SYS_CLOSE, fd, 0);
    if (xstat("/dev/disk1p3") != 0 || xstat("/dev/disk1p6") != 0 || xstat("/dev/disk1p7") == 0) {
        fail("the nodes after the table was read again");
    }
    if (sys2(SYS_UMOUNT2, (int)(size_t)"/mnt6", 0) != 0) {
        fail("unmounting the logical partition");
    }
    puts("PARTSPROBE OK");
    sys2(SYS_EXIT, 0, 0);
}
