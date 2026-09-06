/**
 * Two disks on one AHCI controller, both in the registry and both usable.
 *
 * The suite attaches a 64 MiB KRFFS disk on port 0 and a 20 MiB disk with
 * one FAT32 partition on port 1, and boots with `rach2.diskdiag` so the
 * whole-disk nodes exist. This requires `/proc/disk` to list both disks with
 * their ports and sizes, the nodes of both to exist and the missing
 * partitions' nodes not to, and a partition of each to mount and take a
 * write that reads back: the first disk's KRFFS volume at `/mnt0`, the
 * second's FAT32 volume at `/mnt1`. The suite then reads the FAT32 file from
 * the host with mtools, so the write reached the second disk and not the
 * first.
 *
 * Prints "DISKSPROBE OK" or "DISKSPROBE FAIL: reason". Freestanding; int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_MOUNT   21
#define SYS_MKDIR   39
#define SYS_UMOUNT2 52
#define SYS_STAT64  195

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

#define DISK1_SECTORS 40960u /* the suite's second disk, 20 MiB */

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
    sys3(SYS_WRITE, 1, (int)(size_t)"DISKSPROBE FAIL: ", 17);
    puts(why);
    sys2(SYS_EXIT, 1, 0);
}

static int xstat(const char *path)
{
    char st[104];

    st[0] = 0;
    return sys2(SYS_STAT64, (int)(size_t)path, (int)(size_t)st);
}

static int starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/* The sector count on the `/proc/disk` line `disk N TRANSPORT PORT SECTORS`,
   for the disk numbered `n`, or 0 when there is no such line. */
static unsigned disk_sectors(const char *text, int n, const char *transport, int port)
{
    char want[24];
    int k = 0;
    const char *p;

    want[k++] = 'd';
    want[k++] = 'i';
    want[k++] = 's';
    want[k++] = 'k';
    want[k++] = ' ';
    want[k++] = (char)('0' + n);
    want[k++] = ' ';
    while (*transport) {
        want[k++] = *transport++;
    }
    want[k++] = ' ';
    want[k++] = (char)('0' + port);
    want[k++] = ' ';
    want[k] = 0;
    for (p = text; *p; p++) {
        if ((p == text || p[-1] == '\n') && starts(p, want)) {
            unsigned v = 0;

            p += k;
            while (*p >= '0' && *p <= '9') {
                v = v * 10u + (unsigned)(*p - '0');
                p++;
            }
            return v;
        }
    }
    return 0;
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

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    static char text[2048];
    int fd, n;
    unsigned s0, s1;

    (void)sp;
    fd = sys3(SYS_OPEN, (int)(size_t)"/proc/disk", O_RDONLY, 0);
    if (fd < 0) {
        fail("/proc/disk");
    }
    n = sys3(SYS_READ, fd, (int)(size_t)text, (int)sizeof(text) - 1);
    sys2(SYS_CLOSE, fd, 0);
    if (n <= 0) {
        fail("reading /proc/disk");
    }
    text[n] = 0;
    s0 = disk_sectors(text, 0, "ahci", 0);
    s1 = disk_sectors(text, 1, "ahci", 1);
    if (s0 == 0) {
        fail("/proc/disk has no line for disk 0 on AHCI port 0");
    }
    if (s1 != DISK1_SECTORS) {
        fail("/proc/disk has no line for disk 1 on AHCI port 1 with its 40960 sectors");
    }
    if (s0 <= s1) {
        fail("disk 0 is not the larger disk");
    }
    if (disk_sectors(text, 2, "ahci", 2) != 0) {
        fail("/proc/disk lists a third disk");
    }

    if (xstat("/dev/disk0") != 0 || xstat("/dev/disk0p1") != 0) {
        fail("the nodes of disk 0");
    }
    if (xstat("/dev/disk1") != 0 || xstat("/dev/disk1p1") != 0) {
        fail("the nodes of disk 1");
    }
    if (xstat("/dev/disk1p2") == 0 || xstat("/dev/disk2") == 0) {
        fail("a node for a partition or a disk that is not there");
    }

    sys2(SYS_MKDIR, (int)(size_t)"/mnt0", 0755);
    sys2(SYS_MKDIR, (int)(size_t)"/mnt1", 0755);
    if (sys5(SYS_MOUNT, (int)(size_t)"/dev/disk0p1", (int)(size_t)"/mnt0", (int)(size_t)"krffs", 0,
             0) != 0) {
        fail("mounting disk 0's KRFFS partition");
    }
    if (sys5(SYS_MOUNT, (int)(size_t)"/dev/disk1p1", (int)(size_t)"/mnt1", (int)(size_t)"vfat", 0,
             0) != 0) {
        fail("mounting disk 1's FAT32 partition");
    }
    write_and_read_back("/mnt0/zero.txt", "disk zero says hello\n");
    write_and_read_back("/mnt1/one.txt", "disk one says hello\n");
    if (sys2(SYS_UMOUNT2, (int)(size_t)"/mnt1", 0) != 0 ||
        sys2(SYS_UMOUNT2, (int)(size_t)"/mnt0", 0) != 0) {
        fail("unmounting");
    }
    puts("DISKSPROBE OK");
    sys2(SYS_EXIT, 0, 0);
}
