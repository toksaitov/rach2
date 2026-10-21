/**
 * permprobe: the nine permission bits, checked by each call that reads them.
 *
 * A root parent stages the fixtures (a 0600 root file, a 0666 root file, a
 * 0700 root directory, a 0700 directory owned by uid 1000), then a child
 * drops to 1000 and is denied everything the bits deny: writing a 0644
 * root file, reading a 0600 root file, creating or unlinking or renaming in
 * the root directory, chdir into a 0700 root directory, exec of a file with
 * no execute bit, and setting explicit utimensat times on a root file it can
 * only write; while its own directory grants creation and entry, the
 * other-write bit grants the utimensat touch to "now", and access(2) answers
 * with the real ids, which it shows first with real uid 1000 under effective
 * uid 0. The root parent then removes every fixture through its override,
 * which is itself the override's proof.
 *
 * The fixtures are at the root, beside root's 0644 `/hello.txt`. An argument
 * `ids=OLD:NEW,...` gives the id that stands for 1000 in a native run
 * (probe_args.h), which runs the probe in a private copy of the test image's
 * root; without it the id is 1000.
 *
 * Prints "PERMPROBE OK" and exits 0 on success; prints "PERMPROBE FAIL: "
 * and the failing step and exits 1 otherwise. Writes etude's report records
 * (probe_report.h) for the case ETUDE_CASE names, an assertion for each check
 * `steps` lists. The child cannot write them, since only the process that
 * began the report may, so it exits with the number of the check that
 * failed, and the parent reports it; a failure leaves the checks after it
 * not run.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int u32;

/* The id a native run gives in place of `id` (probe_args.h). */
#define ID(id) ((u32)probe_id(id))

#define STDOUT   1
#define EPERM    1
#define EACCES   13
#define EBADF    9
#define O_RDONLY 0
#define O_WRONLY 1
#define R_OK     4
#define W_OK     2
#define AT_FDCWD (-100)

/* utimensat tv_nsec sentinels (each timespec is {sec, nsec}, [atime,
 * mtime]). */
#define UTIME_NOW  ((1 << 30) - 1)
#define UTIME_OMIT ((1 << 30) - 2)

static int fixture_owner;

/** Remove partially staged fixtures in the root parent after a failure. */
static void cleanup(void)
{
    if (syscall1(SYS_GETPID, 0) != fixture_owner) {
        return;
    }
    syscall1(SYS_UNLINK, (long)"/perm.wo");
    syscall1(SYS_UNLINK, (long)"/perm600");
    syscall1(SYS_UNLINK, (long)"/perm.rw");
    syscall1(SYS_UNLINK, (long)"/perm.own/mine.txt");
    syscall1(SYS_RMDIR, (long)"/perm.own");
    syscall1(SYS_RMDIR, (long)"/perm.priv");
}

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output, with no newline. */
static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

/* The checks, one assertion each, in the order the probe makes them: the
 * parent's up to `FORK`, the child's from `REAL_IDS` to `ACCESS`, and the
 * parent's `CLEANUP`. */
enum {
    ROOT_EXEC,
    CREAT_WO,
    READ_WO,
    STAGE_600,
    STAGE_DIRS,
    STAGE_RW,
    FORK,
    REAL_IDS,
    READ_EFFECTIVE,
    ACCESS_EFFECTIVE,
    DROP,
    READ_644,
    WRITE_644,
    READ_600,
    CREATE_ROOT,
    UNLINK_ROOT,
    RENAME_ROOT,
    MKDIR_ROOT,
    CHDIR_PRIV,
    EXEC,
    CHDIR_OWN,
    CREATE_OWN,
    TOUCH_OTHER_WRITE,
    EXPLICIT_NON_OWNER,
    MIXED_NON_OWNER,
    TOUCH_NO_WRITE,
    BOTH_OMIT,
    EXPLICIT_OWNER,
    ACCESS,
    CLEANUP,
    STEPS
};

static const struct {
    const char *id;
    const char *text;
} steps[STEPS] = {
    {"root-exec", "root exec of non-executable not EACCES"},
    {"creat-wo", "creat write-only"},
    {"read-wo", "read through creat fd not EBADF"},
    {"stage-600", "stage perm600"},
    {"stage-dirs", "stage directories"},
    {"stage-rw", "stage perm.rw"},
    {"fork", "fork"},
    {"real-ids", "real uid 1000 under effective uid 0"},
    {"read-effective", "read 0600 as effective root"},
    {"access-effective", "access by real uid 1000 to a 0600 root file not EACCES"},
    {"drop", "drop to 1000"},
    {"read-644", "read 0644 as other"},
    {"write-644", "write 0644 as other not EACCES"},
    {"read-600", "read 0600 as other not EACCES"},
    {"create-root", "create in / not EACCES"},
    {"unlink-root", "unlink in / not EACCES"},
    {"rename-root", "rename out of / not EACCES"},
    {"mkdir-root", "mkdir in / not EACCES"},
    {"chdir-priv", "chdir into 0700 root dir not EACCES"},
    {"exec", "exec of non-executable not EACCES"},
    {"chdir-own", "chdir into own dir"},
    {"create-own", "create in own dir"},
    {"touch-other-write", "touch through other-write"},
    {"explicit-non-owner", "explicit times as non-owner not EPERM"},
    {"mixed-non-owner", "mixed NOW/OMIT as non-owner not EPERM"},
    {"touch-no-write", "touch without write not EACCES"},
    {"both-omit", "both-OMIT needs no right"},
    {"explicit-owner", "explicit times as owner"},
    {"access", "access real-id answers"},
    {"cleanup", "root override cleanup"},
};

/** Report each check from the next of the planned path up to `step` as
 * held. */
static void report_upto(int step)
{
    while (report_path_at < step) {
        report_held();
    }
}

/** Print "PERMPROBE FAIL: " and `why`, and in the root parent fail the check
 * `step` with `why`, remove the fixtures, and exit 1. */
static void fail_why(int step, const char *why)
{
    puts_("PERMPROBE FAIL: ");
    puts_(why);
    puts_("\n");
    report_upto(step);
    report_stop(why);
    cleanup();
    syscall3(SYS_EXIT, 1, 0, 0);
}

/** Fail the check `step`: the child exits with `step`, for the parent to
 * report, after printing its line; the parent fails it as `fail_why` does. */
static void fail(int step)
{
    if (syscall1(SYS_GETPID, 0) != fixture_owner) {
        puts_("PERMPROBE FAIL: ");
        puts_(steps[step].text);
        puts_("\n");
        syscall3(SYS_EXIT, step, 0, 0);
    }
    fail_why(step, steps[step].text);
}

/**
 * access(2) answers with the real ids. The child takes gid 1000 and real uid
 * 1000 while its effective uid stays 0, so open reads the 0600 root file
 * and access, which asks for real uid 1000, may not; a kernel that answered
 * access with the effective ids would grant it.
 */
static void real_id_rows(void)
{
    int fd;

    if (syscall3(SYS_SETRESGID, ID(1000), ID(1000), ID(1000)) != 0 ||
        syscall3(SYS_SETRESUID, ID(1000), 0, 0) != 0) {
        fail(REAL_IDS);
    }
    fd = syscall3(SYS_OPEN, (long)"/perm600", O_RDONLY, 0);
    if (fd < 0) {
        fail(READ_EFFECTIVE);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (syscall3(SYS_ACCESS, (long)"/perm600", R_OK, 0) != -EACCES) {
        fail(ACCESS_EFFECTIVE);
    }
}

/**
 * The child's half: drop to uid and gid 1000 and make each denied call and
 * each granted one. It exits 0 when every answer is right, and 1 through
 * `fail` at the first that is not.
 *
 * utimensat splits its rights: write permission grants a touch to "now"; an
 * explicit time, or any pair not both UTIME_NOW, is the owner's or root's
 * alone; and a request with both UTIME_OMIT changes nothing and needs no
 * right.
 */
static void child_main(void)
{
    static char *exec_argv[2];
    int fd;

    real_id_rows();
    if (syscall3(SYS_SETRESUID, ID(1000), ID(1000), ID(1000)) != 0) {
        fail(DROP);
    }

    /* Other may read a 0644 file and may not write it. */
    fd = syscall3(SYS_OPEN, (long)"/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail(READ_644);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (syscall3(SYS_OPEN, (long)"/hello.txt", O_WRONLY, 0) != -EACCES) {
        fail(WRITE_644);
    }
    if (syscall3(SYS_OPEN, (long)"/perm600", O_RDONLY, 0) != -EACCES) {
        fail(READ_600);
    }

    /* The root directory is 0755: no creation, removal, or renaming by 1000. */
    if (syscall3(SYS_CREAT, (long)"/denied.txt", 0644, 0) != -EACCES) {
        fail(CREATE_ROOT);
    }
    if (syscall3(SYS_UNLINK, (long)"/hello.txt", 0, 0) != -EACCES) {
        fail(UNLINK_ROOT);
    }
    if (syscall3(SYS_RENAME, (long)"/hello.txt", (long)"/perm.own/h", 0) != -EACCES) {
        fail(RENAME_ROOT);
    }
    if (syscall3(SYS_MKDIR, (long)"/denied.d", 0755, 0) != -EACCES) {
        fail(MKDIR_ROOT);
    }

    /* Search of a directory and exec of a file each need the x bit. */
    if (syscall3(SYS_CHDIR, (long)"/perm.priv", 0, 0) != -EACCES) {
        fail(CHDIR_PRIV);
    }
    exec_argv[0] = (char *)"/hello.txt";
    exec_argv[1] = 0;
    if (syscall3(SYS_EXECVE, (long)"/hello.txt", (long)exec_argv, 0) != -EACCES) {
        fail(EXEC);
    }

    /* The child's own directory grants entry and creation. */
    if (syscall3(SYS_CHDIR, (long)"/perm.own", 0, 0) != 0) {
        fail(CHDIR_OWN);
    }
    fd = syscall3(SYS_CREAT, (long)"/perm.own/mine.txt", 0600, 0);
    if (fd < 0) {
        fail(CREATE_OWN);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);

    /* The utimensat split of rights. */
    {
        static const long expl[4] = {5, 0, 5, 0};
        static const long mixed[4] = {0, UTIME_NOW, 0, UTIME_OMIT};
        static const long omit[4] = {0, UTIME_OMIT, 0, UTIME_OMIT};

        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.rw", 0, 0) != 0) {
            fail(TOUCH_OTHER_WRITE);
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.rw", (long)expl, 0) != -EPERM) {
            fail(EXPLICIT_NON_OWNER);
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.rw", (long)mixed, 0) != -EPERM) {
            fail(MIXED_NON_OWNER);
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/hello.txt", 0, 0) != -EACCES) {
            fail(TOUCH_NO_WRITE);
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/hello.txt", (long)omit, 0) != 0) {
            fail(BOTH_OMIT);
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.own/mine.txt", (long)expl, 0) != 0) {
            fail(EXPLICIT_OWNER);
        }
    }

    /* access(2) answers with the real ids. */
    if (syscall3(SYS_ACCESS, (long)"/hello.txt", R_OK, 0) != 0 ||
        syscall3(SYS_ACCESS, (long)"/hello.txt", W_OK, 0) != -EACCES) {
        fail(ACCESS);
    }

    syscall3(SYS_EXIT, 0, 0, 0);
}

/** Fail the child's walk, which ended with the wait `status`: a child that
 * exited with the number of one of its checks fails that check, and one that
 * ended otherwise its first, as the "denied walk"; exits 1. */
static void walk_failed(int status)
{
    int code = (status >> 8) & 0xFF;

    if ((status & 0x7F) == 0 && code > FORK && code < CLEANUP) {
        report_upto(code);
        report_stop(steps[code].text);
    }
    fail_why(REAL_IDS, "denied walk");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe: root's own two refusals, the fixtures, the child's walk, and
 * root's removal of the fixtures. /perm.rw is world-writable and not the
 * child's, so the child has write permission on it without ownership, the
 * pair the utimensat split tells apart; chmod sets its mode again after the
 * creat, which applies the umask.
 */
void probe_main(unsigned long *sp)
{
    static char *exec_argv[2];
    int pid, status;
    int fd, i;

    probe_args((int)sp[0], (char **)(sp + 1));
    fixture_owner = syscall1(SYS_GETPID, 0);
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.permprobe"));
    for (i = 0; i < STEPS; i++) {
        report_plan(steps[i].id);
    }
    /* Exec of a file with no execute bit is -EACCES even for root. */
    exec_argv[0] = (char *)"/hello.txt";
    exec_argv[1] = 0;
    if (syscall3(SYS_EXECVE, (long)"/hello.txt", (long)exec_argv, 0) != -EACCES) {
        fail(ROOT_EXEC);
    }

    /* creat's descriptor is write-only, so a read is -EBADF even for root. */
    fd = syscall3(SYS_CREAT, (long)"/perm.wo", 0644, 0);
    if (fd < 0) {
        fail(CREAT_WO);
    }
    {
        char c;
        if (syscall3(SYS_READ, fd, (long)&c, 1) != -EBADF) {
            fail(READ_WO);
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    syscall3(SYS_UNLINK, (long)"/perm.wo", 0, 0);

    /* Root stages the fixtures. */
    fd = syscall3(SYS_CREAT, (long)"/perm600", 0600, 0);
    if (fd < 0) {
        fail(STAGE_600);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (syscall3(SYS_MKDIR, (long)"/perm.priv", 0700, 0) != 0 ||
        syscall3(SYS_MKDIR, (long)"/perm.own", 0700, 0) != 0 ||
        syscall3(SYS_CHOWN, (long)"/perm.own", ID(1000), ID(1000)) != 0) {
        fail(STAGE_DIRS);
    }
    /* The 0666 fixture, its mode set again past the umask. */
    fd = syscall3(SYS_CREAT, (long)"/perm.rw", 0666, 0);
    if (fd < 0 || syscall3(SYS_CLOSE, fd, 0, 0) != 0 ||
        syscall3(SYS_CHMOD, (long)"/perm.rw", 0666, 0) != 0) {
        fail(STAGE_RW);
    }

    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail(FORK);
    }
    if (pid == 0) {
        child_main();
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        walk_failed(status);
    }

    /* Root removes every fixture, uid 1000's included, through its override. */
    if (syscall3(SYS_UNLINK, (long)"/perm.own/mine.txt", 0, 0) != 0 ||
        syscall3(SYS_RMDIR, (long)"/perm.own", 0, 0) != 0 ||
        syscall3(SYS_RMDIR, (long)"/perm.priv", 0, 0) != 0 ||
        syscall3(SYS_UNLINK, (long)"/perm600", 0, 0) != 0 ||
        syscall3(SYS_UNLINK, (long)"/perm.rw", 0, 0) != 0) {
        fail(CLEANUP);
    }

    report_upto(STEPS);
    report_end();
    puts_("PERMPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
