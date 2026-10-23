/**
 * Exercises the rename syscall: rename a file, confirm the old name is gone
 * and the new name resolves, reject renaming a missing source, refuse the root
 * as a source with EBUSY, as Linux does, and overwrite an existing
 * destination, which then has the source's bytes and not its own.
 *
 * The negative cases assert the exact errno, not the sign. The KRFFS core
 * answers every refusal of a rename with a bare -1, so the system call tells
 * the cases apart before it calls the core (syscall_fs.c), and the two
 * directory cases are the ones that matter: a directory as the source is
 * -EPERM, the errno POSIX reserves for a rename the file system cannot
 * perform, while a directory as the destination under a plain source is
 * -EISDIR. A test that accepted any negative return could not tell them
 * apart. A directory source is refused with -EPERM on KRFFS, whose children
 * name their parent by id, and performed on ext2, which rewrites `..`; the
 * probe reads the root's type by statfs(2) of `/`, prints it, and fails the
 * row on a root that is neither.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each test: `create`, `exists`, `rename`,
 * `old-gone`, `new-exists`, `enoent`, `dir-rename`, the answer the root's
 * type owes, `eisdir`, `ebusy-root` and `overwrite`; a failed first creation
 * leaves the others not run. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

/* The errno values the negative cases name. */
#define ENOENT 2
#define EPERM  1
#define EBUSY  16
#define EISDIR 21

/* Open flags */
#define O_RDONLY 0

/* File mode bits */
#define S_IRUSR 0400
#define S_IWUSR 0200

/** write(2): the bytes written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** read(2): the bytes read, or a negated errno. */
static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** creat(2): a descriptor, or a negated errno. */
static int sys_creat(const char *path, unsigned int mode)
{
    return syscall2(SYS_CREAT, (long)path, (long)mode);
}

/** open(2) with no mode: a descriptor, or a negated errno. */
static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

/** close(2): 0, or a negated errno. */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** unlink(2): 0, or a negated errno. */
static int sys_unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (long)path);
}

/** mkdir(2): 0, or a negated errno. */
static int sys_mkdir(const char *path, int mode)
{
    return syscall2(SYS_MKDIR, (long)path, (long)mode);
}

/** rmdir(2): 0, or a negated errno. */
static int sys_rmdir(const char *path)
{
    return syscall1(SYS_RMDIR, (long)path);
}

/** rename(2): 0, or a negated errno. */
static int sys_rename(const char *oldpath, const char *newpath)
{
    return syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/* The f_type statfs(2) reports for each root the directory row knows: the
 * KRFFS boot root, a KRFFS volume mounted by mount(2), and ext2. */
#define STATFS_MAGIC_KRFFS        0x4B524646u
#define STATFS_MAGIC_KRFFS_VOLUME 0x4B52u
#define STATFS_MAGIC_EXT2         0xEF53u

/**
 * Whether the root under the probe is ext2, read by statfs(2) of `/` rather
 * than assumed: the two roots answer a directory's rename differently and both
 * answers are right for their file system. 1 for ext2 and 0 for KRFFS; -1 for
 * another type, which fails the directory row, or when the call fails.
 * statfs and not `/proc/mounts`, whose lines name mount points from the
 * machine's root, so that a run in a private root learns that root's type.
 */
static int root_is_ext2(void)
{
    static char buf[KSTATFS_SIZE];

    if (probe_statfs("/", buf) != 0) {
        return -1;
    }
    if (*(unsigned *)buf == STATFS_MAGIC_EXT2) {
        return 1;
    }
    if (*(unsigned *)buf == STATFS_MAGIC_KRFFS || *(unsigned *)buf == STATFS_MAGIC_KRFFS_VOLUME) {
        return 0;
    }
    return -1;
}

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** Write `n`, from 0 to 99, in decimal. */
static void print_num(int n)
{
    if (n >= 10) {
        char tens = '0' + (n / 10);
        sys_write(STDOUT_FILENO, &tens, 1);
    }
    char ones = '0' + (n % 10);
    sys_write(STDOUT_FILENO, &ones, 1);
}

/* The counts of the tests that passed and failed. */
static int passed, failed;

/** Print `good` and count a test that passed when `ok`; otherwise print
 * `bad`, count a test that failed, and fail the open part for `why`. */
static void verdict(int ok, const char *good, const char *bad, const char *why)
{
    if (ok) {
        puts(good);
        passed++;
    } else {
        puts(bad);
        report_miss(why);
        failed++;
    }
}

/**
 * Test 7: a rename over an existing file replaces it. The destination is
 * made with bytes of its own, and after the rename it must read back the
 * source's five bytes, "moved", with the source's name gone. Both names are
 * removed afterwards.
 */
static void overwrite_row(void)
{
    char got[16];
    int fd, result, n = -1;

    report_part("overwrite");
    print("Test 7: create \"mvtest_dest.tmp\"... ");
    fd = sys_creat("mvtest_dest.tmp", S_IRUSR | S_IWUSR);
    if (fd < 0) {
        verdict(0, "", "FAILED (can't create dest file)", "creat of mvtest_dest.tmp failed");
        sys_unlink("mvtest_new.tmp");
        return;
    }
    sys_write(fd, "stale bytes", 11);
    sys_close(fd);
    print("OK, rename to overwrite... ");
    result = sys_rename("mvtest_new.tmp", "mvtest_dest.tmp");
    fd = sys_open("mvtest_dest.tmp", O_RDONLY);
    if (fd >= 0) {
        n = sys_read(fd, got, sizeof(got));
        sys_close(fd);
    }
    fd = sys_open("mvtest_new.tmp", O_RDONLY);
    if (fd >= 0) {
        sys_close(fd);
    }
    sys_unlink("mvtest_new.tmp");
    sys_unlink("mvtest_dest.tmp");
    verdict(result == 0 && n == 5 && got[0] == 'm' && got[1] == 'o' && got[2] == 'v' &&
                got[3] == 'e' && got[4] == 'd' && fd < 0,
            "OK (the destination has the source's bytes)",
            "FAILED (the destination is not the source, or both names remain)",
            "the destination is not the source, or both names remain");
}

/** Tests 2 to 5: the created file opens, is renamed, and is found by its
 * new name and not by its old one. */
static void name_rows(void)
{
    int fd;

    report_part("exists");
    print("Test 2: open(\"mvtest_old.tmp\") to verify... ");
    fd = sys_open("mvtest_old.tmp", O_RDONLY);
    if (fd >= 0) {
        sys_close(fd);
    }
    verdict(fd >= 0, "OK (file exists)", "FAILED (file not found)",
            "open of mvtest_old.tmp failed before the rename");

    report_part("rename");
    print("Test 3: rename(\"mvtest_old.tmp\", \"mvtest_new.tmp\")... ");
    verdict(sys_rename("mvtest_old.tmp", "mvtest_new.tmp") == 0, "OK", "FAILED",
            "rename of mvtest_old.tmp to mvtest_new.tmp failed");

    report_part("old-gone");
    print("Test 4: open(\"mvtest_old.tmp\") should fail... ");
    fd = sys_open("mvtest_old.tmp", O_RDONLY);
    if (fd >= 0) {
        sys_close(fd);
    }
    verdict(fd < 0, "OK (old name gone)", "FAILED (old name still exists)",
            "the old name still opened after the rename");

    report_part("new-exists");
    print("Test 5: open(\"mvtest_new.tmp\")... ");
    fd = sys_open("mvtest_new.tmp", O_RDONLY);
    if (fd >= 0) {
        sys_close(fd);
    }
    verdict(fd >= 0, "OK (new name exists)", "FAILED (new name not found)",
            "the new name did not open after the rename");
}

/** Test 6b: a directory's rename gives the answer of the root's type
 * `root`, as `root_is_ext2` gives it. */
static void dir_row(int root)
{
    int result;

    report_part("dir-rename");
    print("Test 6b: rename of a directory, the answer this root owes... ");
    if (root == -1) {
        verdict(0, "", "FAILED (the root is neither krffs nor ext2)",
                "the root is neither krffs nor ext2");
    } else if (sys_mkdir("mvtest.dir", 0755) == 0) {
        int dir_rename_refused = !root;

        result = sys_rename("mvtest.dir", "mvtest.dir2");
        verdict(dir_rename_refused ? (result == -EPERM) : (result == 0), "OK",
                "FAILED (wrong errno)",
                dir_rename_refused ? "rename of a directory on krffs was not EPERM"
                                   : "rename of a directory on ext2 failed");
        sys_rmdir("mvtest.dir");
        sys_rmdir("mvtest.dir2");
    } else {
        verdict(0, "", "FAILED (mkdir fixture failed)", "mkdir of mvtest.dir failed");
    }
}

/** Tests 6 to 6d: a missing source is ENOENT, a directory's rename gives
 * the answer of the root's type `root`, a directory under a plain source is
 * EISDIR, and the root as a source EBUSY. */
static void errno_rows(int root)
{
    int fd, result;

    report_part("enoent");
    print("Test 6: rename(\"nonexistent\", \"other\") == -ENOENT... ");
    result = sys_rename("nonexistent", "other");
    if (result != -ENOENT) {
        sys_unlink("other");
    }
    verdict(result == -ENOENT, "OK", "FAILED (wrong errno)",
            "rename of a missing source was not ENOENT");

    dir_row(root);

    report_part("eisdir");
    print("Test 6c: rename(file, a directory) == -EISDIR... ");
    if (sys_mkdir("mvtest.dst", 0755) == 0) {
        fd = sys_creat("mvtest.src", S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            sys_close(fd);
            result = sys_rename("mvtest.src", "mvtest.dst");
            verdict(result == -EISDIR, "OK", "FAILED (wrong errno)",
                    "rename of a file over a directory was not EISDIR");
            sys_unlink("mvtest.src");
        } else {
            verdict(0, "", "FAILED (creat fixture failed)", "creat of mvtest.src failed");
        }
        sys_rmdir("mvtest.dst");
    } else {
        verdict(0, "", "FAILED (mkdir fixture failed)", "mkdir of mvtest.dst failed");
    }

    report_part("ebusy-root");
    print("Test 6d: rename(\"/\", \"mvtest.x\") == -EBUSY... ");
    verdict(sys_rename("/", "mvtest.x") == -EBUSY, "OK", "FAILED (wrong errno)",
            "rename of / was not EBUSY");
}

/* The assertions after `create`, which a failed creation leaves not run. */
static const char *const after_create[] = {"exists",     "rename",     "old-gone",
                                           "new-exists", "enoent",     "dir-rename",
                                           "eisdir",     "ebusy-root", "overwrite"};

/** Report the assertion `create` failed and those after it not run. */
static void report_no_file(void)
{
    unsigned int i;

    report_fail("create", "creat or write of mvtest_old.tmp failed");
    for (i = 0; i < sizeof(after_create) / sizeof(after_create[0]); i++) {
        report_notrun(after_create[i], "the test file was not created");
    }
    report_end();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, which `_start` calls with the initial stack: run the rename
 * tests and exit 0 only if all pass. A row removes the names it made, and a
 * failed row the names a wrong answer may have made, such as the destination
 * of a rename whose source was missing.
 */
void probe_main(unsigned long *sp)
{
    int fd;
    int root = root_is_ext2();

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.mvtest"));
    puts("Rename syscall test starting...");
    if (root == 1) {
        puts("mvtest: the root is ext2");
    } else if (root == 0) {
        puts("mvtest: the root is krffs");
    } else {
        puts("mvtest: the root is neither krffs nor ext2");
    }
    puts("");

    print("Test 1: creat(\"mvtest_old.tmp\")... ");
    fd = sys_creat("mvtest_old.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0 && sys_write(fd, "moved", 5) == 5) {
        puts("OK");
        sys_close(fd);
        passed++;
        report_pass("create");
    } else {
        puts("FAILED (can't create test file)");
        failed++;
        report_no_file();
        goto results;
    }

    name_rows();
    errno_rows(root);
    overwrite_row();
    report_finish();

results:
    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All rename syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
