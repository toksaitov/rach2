/**
 * oldfsprobe: sysfs(2) and ustat(2), the file system calls Linux keeps for
 * old programs, as Linux 6.12 answers them.
 *
 * * count: sysfs option 3 gives the number of lines of /proc/filesystems.
 * * names: option 2 gives each line's name, with its NUL, for its index.
 * * indexes: option 1 gives each name's index.
 * * sysfs-errors: an unknown name and an index past the last are EINVAL, an
 *   empty name ENOENT, a name or a buffer outside memory EFAULT, and options
 *   0 and 4 EINVAL.
 * * ustat-counts: ustat of a tmpfs's st_dev gives the free blocks and
 *   inodes that statfs(2) gives for it, cut to the width of each member.
 * * ustat-names: f_fname and f_fpack are zero.
 * * ustat-errors: a device number no mount has is EINVAL, and a buffer
 *   outside memory EFAULT.
 * * cleanup: the probe's own tmpfs is unmounted and its directory removed.
 *
 * The tmpfs is the directory `dir=PATH` names (probe_args.h), or one the
 * probe mounts on oldfs.d in the working directory. Prints "OLDFSPROBE OK"
 * or "OLDFSPROBE FAIL: why". etude's report (probe_report.h), for the case
 * ETUDE_CASE names, has an assertion for each row above in order; the first
 * failure fails its assertion and leaves the rest not run. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define O_RDONLY 0
#define ENOENT   2
#define EFAULT   14
#define EINVAL   22

#define MNT_DETACH 2
#define MAX_TYPES  32

/* The offset of f_bfree, a 64-bit count, in the struct probe_statfs fills:
 * x86-64's statfs or i386's statfs64. */
#if defined __x86_64__
#define KSTATFS_BFREE 24
#else
#define KSTATFS_BFREE 16
#endif

/* Linux's struct ustat: 20 bytes on i386, 32 on x86-64. */
struct ustat {
    int f_tfree;
    unsigned long f_tinode;
    char f_fname[6];
    char f_fpack[6];
};

static const char *dir = "oldfs.d";
static int mounted, made_dir;

/* The names of /proc/filesystems, in its order. */
static char text[2048];
static const char *names[MAX_TYPES];
static int ntypes;

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Zero the `n` bytes at `p`, a buffer the kernel fills. */
static void clear(void *p, size_t n)
{
    char *d = p;

    while (n--) {
        *d++ = 0;
    }
}

/** Copy `n` bytes from `src` to `dst`. */
static void copy(void *dst, const void *src, size_t n)
{
    char *d = dst;
    const char *s = src;

    while (n--) {
        *d++ = *s++;
    }
}

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Write `head`, `s` and a newline to standard output, in one write. */
static void say(const char *head, const char *s)
{
    char line[160];
    size_t n = strlen(head), m = strlen(s);

    if (n + m > sizeof(line) - 1) {
        m = sizeof(line) - 1 - n;
    }
    copy(line, head, n);
    copy(line + n, s, m);
    line[n + m] = '\n';
    syscall3(SYS_WRITE, 1, (long)line, (long)(n + m + 1));
}

/** Unmount and remove the probe's own tmpfs; 0, or -1 when that failed. */
static int cleanup(void)
{
    int bad = 0;

    if (mounted) {
        bad |= syscall2(SYS_UMOUNT2, (long)dir, MNT_DETACH) != 0;
        mounted = 0;
    }
    if (made_dir) {
        bad |= syscall1(SYS_RMDIR, (long)dir) != 0;
        made_dir = 0;
    }
    return bad ? -1 : 0;
}

/** Print the failure `why`, end the report, clean up and exit 1. */
static void fail(const char *why)
{
    say("OLDFSPROBE FAIL: ", why);
    report_stop(why);
    cleanup();
    syscall1(SYS_EXIT, 1);
    __builtin_unreachable();
}

/** Fail with `why` unless `got` is `want_value`. */
static void want(long got, long want_value, const char *why)
{
    if (got != want_value) {
        fail(why);
    }
}

/** Read /proc/filesystems and split it into `names`: each line is "nodev"
 * or nothing, a tab, and the name. */
static void read_types(void)
{
    long fd = syscall3(SYS_OPEN, (long)"/proc/filesystems", O_RDONLY, 0);
    int len = 0;
    long n = 0;
    char *p, *line;

    if (fd < 0) {
        fail("open /proc/filesystems");
    }
    while (len < (int)sizeof(text) - 1 &&
           (n = syscall3(SYS_READ, fd, (long)(text + len), (long)sizeof(text) - 1 - len)) > 0) {
        len += (int)n;
    }
    syscall1(SYS_CLOSE, fd);
    if (n < 0) {
        fail("read /proc/filesystems");
    }
    if (len == (int)sizeof(text) - 1) {
        fail("/proc/filesystems reached the probe's 2047-byte limit");
    }
    text[len] = '\0';
    for (line = text; *line; line = p + 1) {
        if (ntypes == MAX_TYPES) {
            fail("/proc/filesystems passed the probe's 32-type limit");
        }
        for (p = line; *p && *p != '\t'; p++) {
        }
        if (!*p) {
            break;
        }
        names[ntypes++] = ++p;
        while (*p && *p != '\n') {
            p++;
        }
        if (!*p) {
            break;
        }
        *p = '\0';
    }
    if (ntypes == 0) {
        fail("/proc/filesystems has no types");
    }
}

/** The sysfs rows. */
static void sysfs_rows(void)
{
    char name[64];
    int i;

    want(syscall1(SYS_SYSFS, 3), ntypes, "count: option 3 is not the number of types");
    report_held();
    for (i = 0; i < ntypes; i++) {
        name[0] = 'X';
        name[strlen(names[i]) + 1] = 'X';
        if (syscall3(SYS_SYSFS, 2, i, (long)name) != 0 || !same(name, names[i]) ||
            name[strlen(names[i]) + 1] != 'X') {
            fail("names: option 2 did not give a line's name");
        }
    }
    report_held();
    for (i = 0; i < ntypes; i++) {
        want(syscall2(SYS_SYSFS, 1, (long)names[i]), i, "indexes: option 1 did not give an index");
    }
    report_held();
    want(syscall2(SYS_SYSFS, 1, (long)"nosuchfs"), -EINVAL,
         "sysfs-errors: an unknown name was not EINVAL");
    want(syscall2(SYS_SYSFS, 1, (long)""), -ENOENT, "sysfs-errors: an empty name was not ENOENT");
    want(syscall2(SYS_SYSFS, 1, 0), -EFAULT, "sysfs-errors: a NULL name was not EFAULT");
    want(syscall3(SYS_SYSFS, 2, ntypes, (long)name), -EINVAL,
         "sysfs-errors: the index past the last was not EINVAL");
    want(syscall3(SYS_SYSFS, 2, 0, 0), -EFAULT, "sysfs-errors: a NULL buffer was not EFAULT");
    want(syscall1(SYS_SYSFS, 0), -EINVAL, "sysfs-errors: option 0 was not EINVAL");
    want(syscall1(SYS_SYSFS, 4), -EINVAL, "sysfs-errors: option 4 was not EINVAL");
    report_held();
}

/** The ustat rows, on the tmpfs. */
static void ustat_rows(void)
{
    unsigned char st[KSTAT_SIZE], fs[KSTATFS_SIZE];
    unsigned long long bfree, ffree;
    unsigned int dev;
    struct ustat u;
    int i;

    clear(st, sizeof(st));
    if (syscall2(SYS_STAT, (long)dir, (long)st) != 0 || probe_statfs(dir, fs) != 0) {
        fail("ustat-counts: stat or statfs of the tmpfs failed");
    }
    copy(&dev, st + KSTAT_DEV, sizeof(dev));
    copy(&bfree, fs + KSTATFS_BFREE, sizeof(bfree));
    copy(&ffree, fs + KSTATFS_FFREE, sizeof(ffree));
    for (i = 0; i < (int)sizeof(u); i++) {
        ((unsigned char *)&u)[i] = 0xA5;
    }
    want(syscall2(SYS_USTAT, dev, (long)&u), 0, "ustat-counts: ustat of the tmpfs failed");
    want((unsigned int)u.f_tfree == (unsigned int)bfree, 1,
         "ustat-counts: f_tfree is not statfs's f_bfree");
    want(u.f_tinode == (unsigned long)ffree, 1, "ustat-counts: f_tinode is not statfs's f_ffree");
    report_held();
    for (i = 0; i < 6; i++) {
        if (u.f_fname[i] != 0 || u.f_fpack[i] != 0) {
            fail("ustat-names: f_fname or f_fpack is not zero");
        }
    }
    report_held();
    want(syscall2(SYS_USTAT, 0x7fff0000, (long)&u), -EINVAL,
         "ustat-errors: a device no mount has was not EINVAL");
    want(syscall2(SYS_USTAT, dev, 1), -EFAULT, "ustat-errors: a bad buffer was not EFAULT");
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Run the rows in order. `_start` calls it with the initial stack.
 */
void probe_main(unsigned long *sp)
{
    static const char *const plan[] = {"count",        "names",        "indexes",
                                       "sysfs-errors", "ustat-counts", "ustat-names",
                                       "ustat-errors", "cleanup"};
    int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.oldfsprobe"));
    for (i = 0; i < (int)(sizeof(plan) / sizeof(plan[0])); i++) {
        report_plan(plan[i]);
    }
    probe_args((int)sp[0], (char **)(sp + 1));
    dir = probe_arg("dir", dir);
    read_types();
    sysfs_rows();

    if (!probe_arg("dir", 0)) {
        if (syscall2(SYS_MKDIR, (long)dir, 0755) != 0) {
            fail("make oldfs.d");
        }
        made_dir = 1;
        if (syscall5(SYS_MOUNT, (long)"tmpfs", (long)dir, (long)"tmpfs", 0, 0) != 0) {
            fail("mount a tmpfs on oldfs.d");
        }
        mounted = 1;
    }
    ustat_rows();

    if (cleanup() != 0) {
        fail("cleanup: the tmpfs was not removed");
    }
    report_held();
    report_end();
    say("OLDFSPROBE OK", "");
    syscall1(SYS_EXIT, 0);
    __builtin_unreachable();
}
