/**
 * openat2probe: openat2(2) and its RESOLVE_ flags, on a tree in a tmpfs, as
 * Linux 6.12 answers them.
 *
 * The tree is D/a, D/a/f, D/a/b, and the links D/a/up -> "..",
 * D/a/lnk -> "f", D/a/abs -> "/a/f" and D/a/out -> "../../..", where D is
 * the root of a tmpfs: the directory `dir=PATH` names, or o2probe.d in the
 * working directory, on which the probe mounts one of its own.
 *
 * * size: a struct open_how of 23 bytes is EINVAL and one past a page E2BIG;
 *   40 bytes open with zeros past the 24, and are E2BIG with a byte that is
 *   not.
 * * flags: an unknown open flag, a mode without O_CREAT, a mode bit above
 *   07777, an unknown RESOLVE_ flag, BENEATH with IN_ROOT, O_PATH with
 *   O_RDWR and O_CREAT with O_DIRECTORY are EINVAL, and CACHED with O_CREAT
 *   EAGAIN.
 * * nosymlinks: NO_SYMLINKS is ELOOP for a link at the end and on the way,
 *   and gives the link itself to O_PATH with O_NOFOLLOW.
 * * nomagic: NO_MAGICLINKS is ELOOP for /proc/self/fd/N at the end and on
 *   the way, and lets an ordinary link through.
 * * noxdev: NO_XDEV is EXDEV for `..` out of the tmpfs and for /proc from
 *   the root, and opens a file on the same mount.
 * * beneath: BENEATH from D/a opens f and b/../f, and is EXDEV for ../a/f,
 *   an absolute path, `..` at D/a through a link, and an absolute link.
 * * inroot: IN_ROOT from D reaches D/a/f by /a/f, ../../a/f, the absolute
 *   link and the link that climbs past D, and finds no /proc in D.
 * * cached: CACHED opens ".", a file just looked up and a directory.
 * * create: O_CREAT under BENEATH makes b/new, ../new is EXDEV and makes
 *   nothing, and O_CREAT with O_EXCL under IN_ROOT makes /a/b/new2 in D.
 * * cleanup: the tree is removed, and the probe's own tmpfs unmounted.
 *
 * A rename that races a walk under BENEATH or IN_ROOT, which is EAGAIN, has
 * no row: no probe can time a rename inside one call's walk.
 *
 * Prints "OPENAT2PROBE OK" or "OPENAT2PROBE FAIL: why". etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for each
 * group above in order; the first failure fails its assertion and leaves the
 * rest not run. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_DIRECTORY 0200000
#define O_NOFOLLOW  0400000
#define O_PATH      010000000

#define RESOLVE_NO_XDEV       0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS   0x04
#define RESOLVE_BENEATH       0x08
#define RESOLVE_IN_ROOT       0x10
#define RESOLVE_CACHED        0x20

#define ENOENT 2
#define E2BIG  7
#define EAGAIN 11
#define EXDEV  18
#define EINVAL 22
#define ELOOP  40

#define AT_FDCWD   (-100)
#define MNT_DETACH 2

/* Linux's struct open_how, 24 bytes on both architectures, with room past
 * it for the size rows. */
struct open_how {
    unsigned long long flags;
    unsigned long long mode;
    unsigned long long resolve;
};

static union {
    struct open_how how;
    unsigned char bytes[40];
} big;

static const char *dir = "o2probe.d";
static int mounted, made_dir, made_tree;
static unsigned long long ino_f;

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
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

/** Write `s` and a newline to standard output, in one write. */
static void puts(const char *s)
{
    char line[160];
    size_t n = strlen(s);

    if (n > sizeof(line) - 1) {
        n = sizeof(line) - 1;
    }
    copy(line, s, n);
    line[n] = '\n';
    syscall3(SYS_WRITE, 1, (long)line, (long)n + 1);
}

/** The path `name` under the tree's root, in a static buffer. */
static const char *in_dir(const char *name)
{
    static char path[256];
    size_t n = strlen(dir), m = strlen(name);

    copy(path, dir, n);
    path[n] = '/';
    copy(path + n + 1, name, m + 1);
    return path;
}

/** Write the decimal `v` into `out`, ended with a NUL; return the end. */
static char *decimal(char *out, unsigned long v)
{
    char digits[24];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *out++ = digits[--n];
    }
    *out = '\0';
    return out;
}

/** Remove what the probe made; 0, or -1 when a removal failed. */
static int cleanup(void)
{
    static const char *const files[] = {"a/b/new", "a/b/new2", "a/f",   "a/up",
                                        "a/lnk",   "a/abs",    "a/out", "new"};
    int bad = 0;
    int i;

    if (made_tree) {
        for (i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
            syscall1(SYS_UNLINK, (long)in_dir(files[i]));
        }
        bad |= syscall1(SYS_RMDIR, (long)in_dir("a/b")) != 0;
        bad |= syscall1(SYS_RMDIR, (long)in_dir("a")) != 0;
        made_tree = 0;
    }
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

/** Print the failure `why`, end the report, remove the tree and exit 1. */
static void fail(const char *why)
{
    static const char head[] = "OPENAT2PROBE FAIL: ";
    char line[160];
    size_t n = sizeof(head) - 1, m = strlen(why);

    copy(line, head, n);

    if (n + m > sizeof(line) - 1) {
        m = sizeof(line) - 1 - n;
    }
    copy(line + n, why, m);
    line[n + m] = '\0';
    puts(line);
    report_stop(why);
    cleanup();
    syscall1(SYS_EXIT, 1);
    __builtin_unreachable();
}

/** openat2 of `path` from `dfd` with `flags`, `mode` and `resolve`, and a
 * struct of its own size. */
static long o2(int dfd, const char *path, unsigned long long flags, unsigned long long mode,
               unsigned long long resolve)
{
    struct open_how how;

    how.flags = flags;
    how.mode = mode;
    how.resolve = resolve;
    return syscall4(SYS_OPENAT2, dfd, (long)path, (long)&how, sizeof(how));
}

/** Fail with `why` unless `got` is the error `err`. */
static void want_err(long got, int err, const char *why)
{
    if (got >= 0) {
        syscall1(SYS_CLOSE, got);
    }
    if (got != -err) {
        char line[160];
        size_t n = strlen(why);

        if (n > sizeof(line) - 24) {
            n = sizeof(line) - 24;
        }
        copy(line, why, n);
        copy(line + n, ": got ", 6);
        n += 6;
        if (got < 0) {
            line[n++] = '-';
            got = -got;
        }
        decimal(line + n, (unsigned long)got);
        fail(line);
    }
}

/** The inode number of the open file `fd`, or 0 when fstat fails. */
static unsigned long long ino_of(int fd)
{
    unsigned char st[KSTAT_SIZE];
    unsigned long long ino;

    if (syscall2(SYS_FSTAT, fd, (long)st) != 0) {
        return 0;
    }
    copy(&ino, st + KSTAT_INO, sizeof(ino));
    return ino;
}

/** Fail with `why` unless `got` is a descriptor, of the file D/a/f when
 * `is_f`; close it. */
static void want_open(long got, int is_f, const char *why)
{
    unsigned long long ino;

    if (got < 0) {
        fail(why);
    }
    ino = ino_of((int)got);
    syscall1(SYS_CLOSE, got);
    if (is_f && ino != ino_f) {
        fail(why);
    }
}

/** Make the tree, and return the descriptors of D in `*dfd` and D/a in
 * `*afd`. */
static void make_tree(int *dfd, int *afd)
{
    long fd;

    if (syscall2(SYS_MKDIR, (long)in_dir("a"), 0755) != 0 ||
        syscall2(SYS_MKDIR, (long)in_dir("a/b"), 0755) != 0) {
        fail("make D/a and D/a/b");
    }
    made_tree = 1;
    fd = syscall3(SYS_OPEN, (long)in_dir("a/f"), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"x", 1) != 1) {
        fail("make D/a/f");
    }
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_SYMLINK, (long)"..", (long)in_dir("a/up")) != 0 ||
        syscall2(SYS_SYMLINK, (long)"f", (long)in_dir("a/lnk")) != 0 ||
        syscall2(SYS_SYMLINK, (long)"/a/f", (long)in_dir("a/abs")) != 0 ||
        syscall2(SYS_SYMLINK, (long)"../../..", (long)in_dir("a/out")) != 0) {
        fail("make the links in D/a");
    }
    *dfd = (int)syscall3(SYS_OPEN, (long)dir, O_RDONLY | O_DIRECTORY, 0);
    *afd = (int)syscall3(SYS_OPEN, (long)in_dir("a"), O_RDONLY | O_DIRECTORY, 0);
    fd = syscall3(SYS_OPEN, (long)in_dir("a/f"), O_RDONLY, 0);
    if (*dfd < 0 || *afd < 0 || fd < 0) {
        fail("open D, D/a and D/a/f");
    }
    ino_f = ino_of((int)fd);
    syscall1(SYS_CLOSE, fd);
}

/** The size rows, from D/a. */
static void size_rows(int afd)
{
    long r;

    big.how.flags = O_RDONLY;
    want_err(syscall4(SYS_OPENAT2, afd, (long)"f", (long)&big, 23), EINVAL,
             "size: 23 bytes was not EINVAL");
    want_err(syscall4(SYS_OPENAT2, afd, (long)"f", (long)&big, 4097), E2BIG,
             "size: a page and a byte was not E2BIG");
    r = syscall4(SYS_OPENAT2, afd, (long)"f", (long)&big, sizeof(big));
    want_open(r, 1, "size: 40 bytes with zeros past 24 did not open D/a/f");
    big.bytes[30] = 1;
    want_err(syscall4(SYS_OPENAT2, afd, (long)"f", (long)&big, sizeof(big)), E2BIG,
             "size: a byte past 24 that is not zero was not E2BIG");
    big.bytes[30] = 0;
}

/** The checks of the flags and the mode, from D/a. */
static void flag_rows(int afd)
{
    want_err(o2(afd, "f", 1ull << 40, 0, 0), EINVAL, "flags: an unknown flag was not EINVAL");
    want_err(o2(afd, "f", O_RDONLY, 0644, 0), EINVAL,
             "flags: a mode without O_CREAT was not EINVAL");
    want_err(o2(afd, "n", O_WRONLY | O_CREAT, 010644, 0), EINVAL,
             "flags: a mode bit above 07777 was not EINVAL");
    want_err(o2(afd, "f", O_RDONLY, 0, 0x40), EINVAL,
             "flags: an unknown RESOLVE_ flag was not EINVAL");
    want_err(o2(afd, "f", O_RDONLY, 0, RESOLVE_BENEATH | RESOLVE_IN_ROOT), EINVAL,
             "flags: BENEATH with IN_ROOT was not EINVAL");
    want_err(o2(afd, "f", O_PATH | O_RDWR, 0, 0), EINVAL,
             "flags: O_PATH with O_RDWR was not EINVAL");
    want_err(o2(afd, "n", O_CREAT | O_DIRECTORY, 0644, 0), EINVAL,
             "flags: O_CREAT with O_DIRECTORY was not EINVAL");
    want_err(o2(afd, "n", O_WRONLY | O_CREAT, 0644, RESOLVE_CACHED), EAGAIN,
             "flags: CACHED with O_CREAT was not EAGAIN");
}

/** Write "/proc/self/fd/" and the decimal `fd`, then `tail`, into `out`. */
static void fd_path(char *out, int fd, const char *tail)
{
    static const char head[] = "/proc/self/fd/";
    char digits[12];
    int n = 0;

    copy(out, head, sizeof(head) - 1);
    out += sizeof(head) - 1;
    do {
        digits[n++] = (char)('0' + fd % 10);
        fd /= 10;
    } while (fd);
    while (n) {
        *out++ = digits[--n];
    }
    copy(out, tail, strlen(tail) + 1);
}

/** The rows of NO_SYMLINKS and NO_MAGICLINKS. */
static void link_rows(int afd)
{
    char path[48];

    want_err(o2(afd, "lnk", O_RDONLY, 0, RESOLVE_NO_SYMLINKS), ELOOP,
             "nosymlinks: a link at the end was not ELOOP");
    want_err(o2(afd, "up/a/f", O_RDONLY, 0, RESOLVE_NO_SYMLINKS), ELOOP,
             "nosymlinks: a link on the way was not ELOOP");
    want_open(o2(afd, "lnk", O_PATH | O_NOFOLLOW, 0, RESOLVE_NO_SYMLINKS), 0,
              "nosymlinks: O_PATH with O_NOFOLLOW did not open the link");
    want_open(o2(afd, "f", O_RDONLY, 0, RESOLVE_NO_SYMLINKS), 1,
              "nosymlinks: a path with no link did not open");
    report_held();

    fd_path(path, afd, "");
    want_err(o2(AT_FDCWD, path, O_RDONLY | O_DIRECTORY, 0, RESOLVE_NO_MAGICLINKS), ELOOP,
             "nomagic: /proc/self/fd/N at the end was not ELOOP");
    fd_path(path, afd, "/f");
    want_err(o2(AT_FDCWD, path, O_RDONLY, 0, RESOLVE_NO_MAGICLINKS), ELOOP,
             "nomagic: /proc/self/fd/N on the way was not ELOOP");
    want_open(o2(afd, "lnk", O_RDONLY, 0, RESOLVE_NO_MAGICLINKS), 1,
              "nomagic: an ordinary link was not followed");
}

/** The rows of NO_XDEV, BENEATH and IN_ROOT. */
static void scope_rows(int dfd, int afd)
{
    want_err(o2(dfd, "..", O_RDONLY | O_DIRECTORY, 0, RESOLVE_NO_XDEV), EXDEV,
             "noxdev: .. out of the tmpfs was not EXDEV");
    want_err(o2(AT_FDCWD, "/proc/self/status", O_RDONLY, 0, RESOLVE_NO_XDEV), EXDEV,
             "noxdev: /proc from the root was not EXDEV");
    want_open(o2(dfd, "a/f", O_RDONLY, 0, RESOLVE_NO_XDEV), 1,
              "noxdev: a file on the same mount did not open");
    report_held();

    want_open(o2(afd, "f", O_RDONLY, 0, RESOLVE_BENEATH), 1, "beneath: f did not open");
    want_open(o2(afd, "b/../f", O_RDONLY, 0, RESOLVE_BENEATH), 1, "beneath: b/../f did not open");
    want_err(o2(afd, "../a/f", O_RDONLY, 0, RESOLVE_BENEATH), EXDEV,
             "beneath: ../a/f was not EXDEV");
    want_err(o2(afd, "/a/f", O_RDONLY, 0, RESOLVE_BENEATH), EXDEV,
             "beneath: an absolute path was not EXDEV");
    want_err(o2(afd, "up/a/f", O_RDONLY, 0, RESOLVE_BENEATH), EXDEV,
             "beneath: .. through a link was not EXDEV");
    want_err(o2(afd, "abs", O_RDONLY, 0, RESOLVE_BENEATH), EXDEV,
             "beneath: an absolute link was not EXDEV");
    report_held();

    want_open(o2(dfd, "/a/f", O_RDONLY, 0, RESOLVE_IN_ROOT), 1, "inroot: /a/f did not reach D/a/f");
    want_open(o2(dfd, "../../a/f", O_RDONLY, 0, RESOLVE_IN_ROOT), 1,
              "inroot: ../../a/f did not reach D/a/f");
    want_open(o2(dfd, "a/abs", O_RDONLY, 0, RESOLVE_IN_ROOT), 1,
              "inroot: the absolute link did not reach D/a/f");
    want_open(o2(dfd, "a/out/a/f", O_RDONLY, 0, RESOLVE_IN_ROOT), 1,
              "inroot: a link that climbs past D did not stay at D");
    want_err(o2(dfd, "/proc/self/status", O_RDONLY, 0, RESOLVE_IN_ROOT), ENOENT,
             "inroot: /proc was found in D");
}

/** The rows of CACHED and of O_CREAT under BENEATH and IN_ROOT. */
static void create_rows(int dfd, int afd)
{
    unsigned char st[KSTAT_SIZE];

    syscall2(SYS_STAT, (long)in_dir("a/f"), (long)st);
    want_open(o2(afd, ".", O_RDONLY | O_DIRECTORY, 0, RESOLVE_CACHED), 0, "cached: . did not open");
    want_open(o2(afd, "f", O_RDONLY, 0, RESOLVE_CACHED), 1,
              "cached: a file just looked up did not open");
    want_open(o2(afd, "b", O_RDONLY | O_DIRECTORY, 0, RESOLVE_CACHED), 0,
              "cached: a directory did not open");
    report_held();

    want_open(o2(afd, "b/new", O_WRONLY | O_CREAT, 0600, RESOLVE_BENEATH), 0,
              "create: b/new under BENEATH was not made");
    want_open(syscall3(SYS_OPEN, (long)in_dir("a/b/new"), O_RDONLY, 0), 0,
              "create: D/a/b/new is not there");
    want_err(o2(afd, "../new", O_WRONLY | O_CREAT, 0600, RESOLVE_BENEATH), EXDEV,
             "create: ../new under BENEATH was not EXDEV");
    want_err(syscall3(SYS_OPEN, (long)in_dir("new"), O_RDONLY, 0), ENOENT,
             "create: the refused ../new was made");
    want_open(o2(dfd, "/a/b/new2", O_WRONLY | O_CREAT | O_EXCL, 0600, RESOLVE_IN_ROOT), 0,
              "create: /a/b/new2 under IN_ROOT was not made");
    want_open(syscall3(SYS_OPEN, (long)in_dir("a/b/new2"), O_RDONLY, 0), 0,
              "create: D/a/b/new2 is not there");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Take `dir=PATH` from the arguments (probe_args.h), make the tree, run
 * the groups in order and remove the tree. `_start` calls it with the
 * initial stack.
 */
void probe_main(unsigned long *sp)
{
    static const char *const plan[] = {"size",    "flags",  "nosymlinks", "nomagic", "noxdev",
                                       "beneath", "inroot", "cached",     "create",  "cleanup"};
    char **argv = (char **)(sp + 1);
    int dfd = -1, afd = -1;
    int i;

    report_begin(report_case(argv + sp[0] + 1, "guest.default.openat2probe"));
    for (i = 0; i < (int)(sizeof(plan) / sizeof(plan[0])); i++) {
        report_plan(plan[i]);
    }
    probe_args((int)sp[0], argv);
    dir = probe_arg("dir", dir);
    if (!probe_arg("dir", 0)) {
        if (syscall2(SYS_MKDIR, (long)dir, 0755) != 0) {
            fail("make o2probe.d");
        }
        made_dir = 1;
        if (syscall5(SYS_MOUNT, (long)"tmpfs", (long)dir, (long)"tmpfs", 0, 0) != 0) {
            fail("mount a tmpfs on o2probe.d");
        }
        mounted = 1;
    }
    make_tree(&dfd, &afd);

    size_rows(afd);
    report_held();
    flag_rows(afd);
    report_held();
    link_rows(afd);
    report_held();
    scope_rows(dfd, afd);
    report_held();
    create_rows(dfd, afd);
    report_held();

    syscall1(SYS_CLOSE, dfd);
    syscall1(SYS_CLOSE, afd);
    if (cleanup() != 0) {
        fail("cleanup: the tree or the tmpfs was not removed");
    }
    report_held();
    report_end();
    puts("OPENAT2PROBE OK");
    syscall1(SYS_EXIT, 0);
    __builtin_unreachable();
}
