/**
 * searchprobe: search permission (x) is charged on every directory a path
 * enters, not only on the target and its parent.
 *
 * A process that may not search a directory reaches nothing below it,
 * whatever the bits of the leaf, and cannot learn whether a name below it
 * exists. The fixture is one root-owned tree whose modes the probe's root
 * parent changes between three children of uid 1000, so that one denied
 * directory is the only difference between a call that succeeds and the same
 * call refused.
 *
 * A denial turns 0777 into 0666: read and write stay granted, so nothing but
 * the execute bit can account for a refusal. 0755 turned into 0700 would take
 * the read bit away as well, and a kernel that charged read where it must
 * charge execute would pass.
 *
 * The baseline child runs first, while the tree is 0777 throughout, and makes
 * every read and every change the denied child is refused, so that a later
 * -EACCES cannot come from the leaf's own bits or from the directory the name
 * lands in.
 *
 * The denied child runs with /sp_denied at 0666 and requires the exact errno
 * of open, stat, i386's old stat, fstatat, access, faccessat, openat, chdir,
 * execve, creat, open with O_CREAT, mkdir, rmdir, unlink, rename in both
 * directions, chmod, fchmodat, fchownat, utimensat and linkat, the reads one
 * level down and two. unlinkat, renameat and mkdirat reach the same code as
 * unlink, rename and mkdir, and are not called. A name that does not exist
 * under the denied directory is -EACCES and not -ENOENT, or a caller could
 * test for existence through a directory it may not search. An openat through
 * a directory descriptor opened while the ancestor was searchable is refused
 * too: the descriptor grants no search of its own.
 *
 * Its changes run in two rounds, which charge different directories. The
 * first aims inside /sp_denied/shared, which stays 0777 and grants write to
 * everyone, so only an ancestor above it can refuse. The second aims directly
 * into /sp_denied, which grants write and withholds search, so only that
 * parent's execute bit can refuse. The write check walks as far as the parent
 * and stops, and a walk does not charge the directory it ends on, so the
 * second round reaches each call's own resolution of the whole path: a kernel
 * that charged every ancestor but the parent passes the first round and lets
 * the second through.
 *
 * access(2) is charged by the real ids, so the denied child asks it twice,
 * with a real uid of 0 under an effective 1000 and the other way round, and
 * the two answers differ, as a set-id program sees them. `..` is physical:
 * /sp_denied/../sp_up.txt enters the denied directory before it climbs out,
 * and is refused there. / still resolves, since the root is searchable.
 *
 * The third child runs after the parent moves the denial one level down, so
 * that the answer follows the directory that denies and not a decision cached
 * from the first walk.
 *
 * chdir is charged twice: the walk enters every directory above the target,
 * and chdir then requires execute on the target itself. `chdir /sp_denied` in
 * the denied child and `chdir /sp_denied/deep` in the third name the denied
 * directory as their target, so chdir's own check refuses them whatever the
 * walk charged; the two-level chdir and the chdir into the 0777 directory
 * below it are the rows that test the walk.
 *
 * krffs/manifests/test.manifest stages /sp_denied, its two leaves, deep,
 * shared and the program prog, since a freestanding probe cannot write an ELF
 * and the execve rows need a program under the denied ancestor. The root
 * parent creates and removes the rest: the targets of the changes, /sp_out
 * outside the tree for the renames that leave it, and /sp_up.txt for the `..`
 * row. The probe leaves the image as the manifest staged it, can run twice in
 * one boot, and uses no file another probe owns. The parent is root
 * throughout, which lets it set up and remove a tree no one else can reach.
 *
 * After the denied child the parent checks that nothing the child tried was
 * done: the created names absent, the removed ones present, the chmod'd
 * file's mode unchanged. That catches a refusal reported after the work was
 * done, as an unchecked `krffs_create` under a checked `open` would be. Each
 * rename has a source of its own, so that an absent destination cannot mean
 * that an earlier case took the source away. linkat has no such check: the
 * file system has no hard links, so the link could not have been made, and
 * its -EACCES against the baseline's -EPERM is the evidence.
 *
 * Every wait checks WIFEXITED before WEXITSTATUS, since a signal death has
 * the signal in the low bits and would read as exit 0.
 *
 * Prints "SEARCHPROBE OK" and exits 0 when every case passed; a failure
 * prints "SEARCHPROBE FAIL: " with the step and the answer it got, and exits
 * 1. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int u32;

#define STDOUT 1

#define EPERM  1
#define ENOENT 2
#define EACCES 13

#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0100
#define O_DIRECTORY 0200000

#define F_OK     0
#define X_OK     1
#define R_OK     4
#define AT_FDCWD (-100)

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death has the signal in the low 7 bits. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

/* Staged by krffs/manifests/test.manifest, every directory 0777. */
#define DENIED    "/sp_denied"
#define LEAF      "/sp_denied/leaf.txt"
#define DEEP      "/sp_denied/deep"
#define DEEP_LEAF "/sp_denied/deep/leaf.txt"
#define SHARED    "/sp_denied/shared"
#define PROG      "/sp_denied/prog"

/* Created and removed by the root parent: what the first round of changes
 * aims at, inside the 0777 directory, and what the second aims at, directly
 * inside the denied one. */
#define VICTIM  "/sp_denied/shared/victim"
#define MOVER   "/sp_denied/shared/mover"
#define MINE    "/sp_denied/shared/mine"
#define RMME    "/sp_denied/shared/rmme"
#define PVICTIM "/sp_denied/pvictim"
#define PMOVER  "/sp_denied/pmover"
#define PRMME   "/sp_denied/prmme"
#define OUTDIR  "/sp_out"
#define OUTSRC  "/sp_out/src"
#define POUTSRC "/sp_out/psrc"
#define UPFILE  "/sp_up.txt"

/* Names the denied child must fail to create, and the parent then asserts
 * absent: the first round's below the 0777 directory, the second's below the
 * denied one. */
#define NEWFILE  "/sp_denied/shared/newfile"
#define NEWDIR   "/sp_denied/shared/newdir"
#define CREATED  "/sp_denied/shared/created.txt"
#define LANDED   "/sp_denied/shared/landed"
#define MOVED    "/sp_out/moved"
#define LNK      "/sp_denied/shared/lnk"
#define PNEWFILE "/sp_denied/pnewfile"
#define PNEWDIR  "/sp_denied/pnewdir"
#define PCREATED "/sp_denied/pcreated.txt"
#define PLANDED  "/sp_denied/planded"
#define PMOVED   "/sp_out/pmoved"

/* The baseline child's own scratch names, both rounds, and the third
 * child's. */
#define BL     "/sp_denied/shared/bl.txt"
#define BLDIR  "/sp_denied/shared/bl.d"
#define OUTBL  "/sp_out/bl.txt"
#define PBL    "/sp_denied/pbl.txt"
#define PBLDIR "/sp_denied/pbl.d"
#define POUTBL "/sp_out/pbl.txt"
#define THIRD  "/sp_denied/shared/third.txt"

/* Names that do not exist, which the denial answers before their absence. */
#define NOSUCH      "/sp_denied/nosuch"
#define DEEP_NOSUCH "/sp_denied/deep/nosuch"

/* Two paths of the `..` and root rows: the first enters the denied directory
 * and is refused, the second is open to everyone. */
#define UPWARD "/sp_denied/../sp_up.txt"
#define ROOT   "/"

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write the string `s` to standard output. */
static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

/** Print a signed decimal, so a failing line has the answer it got. */
static void putd(int v)
{
    char buf[13];
    int i = 12;
    unsigned int u = (v < 0) ? (unsigned int)(-v) : (unsigned int)v;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u != 0u);
    if (v < 0) {
        buf[--i] = '-';
    }
    puts_(&buf[i]);
}

/** Print an unsigned octal with a leading zero, the form a mode is read in. */
static void puto(u32 v)
{
    char buf[13];
    int i = 12;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v & 7u));
        v >>= 3;
    } while (v != 0u);
    buf[--i] = '0';
    puts_(&buf[i]);
}

/** Print "SEARCHPROBE FAIL: `step` (got `got`)" and exit 1. */
static void fail(const char *step, int got)
{
    puts_("SEARCHPROBE FAIL: ");
    puts_(step);
    puts_(" (got ");
    putd(got);
    puts_(")\n");
    syscall1(SYS_EXIT, 1);
}

/** Fail `step` unless the answer `got` is `want` itself, the errno and not
 * only the sign. */
static void want_eq(const char *step, int got, int want)
{
    if (got != want) {
        fail(step, got);
    }
}

/** Fail `step` unless the call succeeded: `got` is 0 or more, since open
 * returns a descriptor. */
static void want_ok(const char *step, int got)
{
    if (got < 0) {
        fail(step, got);
    }
}

/** Open `path` read-only and close it again: open's answer. */
static int open_ro(const char *path)
{
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);

    if (fd >= 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
    }
    return fd;
}

/** As `open_ro`, relative to the directory descriptor `dirfd`. */
static int openat_ro(int dirfd, const char *name)
{
    int fd = syscall4(SYS_OPENAT, dirfd, (long)name, O_RDONLY, 0);

    if (fd >= 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
    }
    return fd;
}

/* The stat buffer every call here fills, as large as x86-64's struct stat;
 * `stat_mode` reads it back. */
static char statbuf[144];

/** SYS_STAT's answer for `path`, for the rows on whether a name exists. */
static int stat_rc(const char *path)
{
    return syscall3(SYS_STAT, (long)path, (long)statbuf, 0);
}

#if !defined __x86_64__
/** The answer of i386's old stat(2), call 106, which resolves its path
 * separately from stat64. musl calls stat64, so only a freestanding caller
 * reaches this one. x86-64 has no such call, and its rows are i686's alone. */
static int old_stat_rc(const char *path)
{
    return syscall3(SYS_OLDSTAT, (long)path, (long)statbuf, 0);
}
#endif

/** fstatat's answer, the *at form of the same question. */
static int fstatat_rc(int dirfd, const char *path)
{
    return syscall4(SYS_FSTATAT, dirfd, (long)path, (long)statbuf, 0);
}

/** The permission bits of `path`; a stat that fails fails the probe. */
static u32 stat_mode(const char *path)
{
    int rc = stat_rc(path);

    if (rc != 0) {
        fail("stat64 of a fixture", rc);
    }
    return *(u32 *)(statbuf + KSTAT_MODE) & 07777u;
}

/** Fail `step`, printing both modes in octal, unless the permission bits of
 * `path` are `mode`. */
static void want_mode(const char *step, const char *path, u32 mode)
{
    u32 got = stat_mode(path);

    if (got != mode) {
        puts_("SEARCHPROBE FAIL: ");
        puts_(step);
        puts_(" (mode ");
        puto(got);
        puts_(" not ");
        puto(mode);
        puts_(")\n");
        syscall1(SYS_EXIT, 1);
    }
}

/** Exec `path` in a new child and report what became of it: the program's
 * exit status when it ran, 0 for the staged program, or 128 + errno when the
 * exec was refused. The fork tells the two apart, since a successful exec
 * leaves nothing behind to report it. A child killed by a signal fails the
 * probe. */
static int run_exec(const char *path)
{
    int pid, status = 0;
    int reaped;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for exec", pid);
    }
    if (pid == 0) {
        static char *argv[2];
        int e;

        argv[0] = (char *)path;
        argv[1] = 0;
        e = syscall3(SYS_EXECVE, (long)path, (long)argv, 0);
        syscall1(SYS_EXIT, 128 + ((-e) & 0x7F));
    }
    reaped = syscall3(SYS_WAITPID, pid, (long)&status, 0);
    if (reaped != pid) {
        fail("waitpid for exec", reaped);
    }
    if (!WIFEXITED(status)) {
        fail("exec child died by signal", status & 0x7F);
    }
    return WEXITSTATUS(status);
}

/** Become uid and gid 1000, real, effective and saved, so that no id can be
 * taken back. The gids go first, since once the effective uid is 1000 the gid
 * calls are refused. */
static void drop_to_1000(void)
{
    want_eq("drop the gids", syscall3(SYS_SETRESGID, 1000, 1000, 1000), 0);
    want_eq("drop the uids", syscall3(SYS_SETRESUID, 1000, 1000, 1000), 0);
}

/* Directory descriptors the parent opens while the ancestor is searchable,
 * which every child inherits across the fork. */
static int dfd_denied = -1;
static int dfd_shared = -1;

/** Everything the denied child is refused, done while the whole tree is
 * 0777, so that a later -EACCES cannot come from the leaf's bits or from the
 * bits of the directory the name lands in. Exits 0, or fails the probe. */
static void baseline_child(void)
{
    int fd;

    drop_to_1000();

    want_ok("baseline: open the one-level leaf", open_ro(LEAF));
    want_ok("baseline: open the two-level leaf", open_ro(DEEP_LEAF));
    want_eq("baseline: stat the two-level leaf", stat_rc(DEEP_LEAF), 0);
#if !defined __x86_64__
    want_eq("baseline: legacy stat the one-level leaf", old_stat_rc(LEAF), 0);
#endif
    want_eq("baseline: fstatat the two-level leaf", fstatat_rc(AT_FDCWD, DEEP_LEAF), 0);
    want_eq("baseline: access the one-level leaf", syscall3(SYS_ACCESS, (long)LEAF, R_OK, 0), 0);
    want_eq("baseline: faccessat the two-level leaf",
            syscall4(SYS_FACCESSAT, AT_FDCWD, (long)DEEP_LEAF, R_OK, 0), 0);
    want_ok("baseline: openat through the denied dirfd", openat_ro(dfd_denied, "leaf.txt"));
    want_ok("baseline: openat through the shared dirfd", openat_ro(dfd_shared, "victim"));

    want_eq("baseline: chdir one level", syscall3(SYS_CHDIR, (long)DENIED, 0, 0), 0);
    want_eq("baseline: chdir two levels", syscall3(SYS_CHDIR, (long)DEEP, 0, 0), 0);
    want_eq("baseline: chdir back to the root", syscall3(SYS_CHDIR, (long)ROOT, 0, 0), 0);

    want_eq("baseline: exec the staged program", run_exec(PROG), 0);
    want_eq("baseline: exec a missing name", run_exec(NOSUCH), 128 + ENOENT);

    /* First round: what uid 1000 may do inside the 0777 directory. */
    fd = syscall3(SYS_CREAT, (long)BL, 0644, 0);
    want_ok("baseline: creat in the 0777 directory", fd);
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("baseline: chmod its own file", syscall3(SYS_CHMOD, (long)BL, 0600, 0), 0);
    want_eq("baseline: fchmodat its own file", syscall4(SYS_FCHMODAT, AT_FDCWD, (long)BL, 0644, 0),
            0);
    want_eq("baseline: fchownat with no change",
            syscall5(SYS_FCHOWNAT, AT_FDCWD, (long)BL, -1, -1, 0), 0);
    want_eq("baseline: utimensat through the other-write bit",
            syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)VICTIM, 0, 0), 0);
    want_eq("baseline: linkat reaches the no-hard-links answer",
            syscall5(SYS_LINKAT, AT_FDCWD, (long)VICTIM, AT_FDCWD, (long)LNK, 0), -EPERM);
    want_eq("baseline: mkdir in the 0777 directory", syscall3(SYS_MKDIR, (long)BLDIR, 0755, 0), 0);
    want_eq("baseline: rmdir in the 0777 directory", syscall3(SYS_RMDIR, (long)BLDIR, 0, 0), 0);
    want_eq("baseline: rename out of the tree", syscall3(SYS_RENAME, (long)BL, (long)OUTBL, 0), 0);
    want_eq("baseline: rename back into the tree", syscall3(SYS_RENAME, (long)OUTBL, (long)BL, 0),
            0);
    want_eq("baseline: unlink in the 0777 directory", syscall3(SYS_UNLINK, (long)BL, 0, 0), 0);

    /* Second round: the same rights directly inside /sp_denied. */
    fd = syscall3(SYS_CREAT, (long)PBL, 0644, 0);
    want_ok("baseline: creat directly under /sp_denied", fd);
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("baseline: mkdir directly under /sp_denied", syscall3(SYS_MKDIR, (long)PBLDIR, 0755, 0),
            0);
    want_eq("baseline: rmdir directly under /sp_denied", syscall3(SYS_RMDIR, (long)PBLDIR, 0, 0),
            0);
    want_eq("baseline: rename out of /sp_denied", syscall3(SYS_RENAME, (long)PBL, (long)POUTBL, 0),
            0);
    want_eq("baseline: rename back into /sp_denied",
            syscall3(SYS_RENAME, (long)POUTBL, (long)PBL, 0), 0);
    want_eq("baseline: unlink directly under /sp_denied", syscall3(SYS_UNLINK, (long)PBL, 0, 0), 0);

    syscall1(SYS_EXIT, 0);
}

/** The same calls with /sp_denied at 0666 and owned by root. Every call that
 * enters the directory answers -EACCES; the root, which no path here enters
 * through it, and access(2) asked with a real uid of 0 still succeed, or the
 * rule would deny more than search. Exits 0, or fails the probe. */
static void denied_child(void)
{
    /* The walk is charged by the effective uid 1000, access by the real 0. */
    want_eq("drop the gids", syscall3(SYS_SETRESGID, 1000, 1000, 1000), 0);
    want_eq("drop only the effective uid", syscall3(SYS_SETRESUID, 0, 1000, 0), 0);
    want_eq("denied: open is charged by the effective id", open_ro(LEAF), -EACCES);
    want_eq("denied: access is charged by the real id (root)",
            syscall3(SYS_ACCESS, (long)LEAF, R_OK, 0), 0);

    /* A set-id program's view, real 1000 and effective 0: the answers swap. */
    want_eq("take the effective id back", syscall3(SYS_SETRESUID, 1000, 0, 0), 0);
    want_ok("denied: open under the effective root", open_ro(LEAF));
    want_eq("denied: access is charged by the real id (1000)",
            syscall3(SYS_ACCESS, (long)LEAF, R_OK, 0), -EACCES);

    want_eq("drop the uids", syscall3(SYS_SETRESUID, 1000, 1000, 1000), 0);

    /* The reads, one level and two, and a name that is not there. */
    want_eq("denied: open the one-level leaf", open_ro(LEAF), -EACCES);
    want_eq("denied: open the two-level leaf", open_ro(DEEP_LEAF), -EACCES);
    want_eq("denied: open a missing one-level name", open_ro(NOSUCH), -EACCES);
    want_eq("denied: open a missing two-level name", open_ro(DEEP_NOSUCH), -EACCES);
    want_eq("denied: stat the one-level leaf", stat_rc(LEAF), -EACCES);
    want_eq("denied: stat a missing name", stat_rc(NOSUCH), -EACCES);
#if !defined __x86_64__
    want_eq("denied: legacy stat the one-level leaf", old_stat_rc(LEAF), -EACCES);
    want_eq("denied: legacy stat a missing name", old_stat_rc(NOSUCH), -EACCES);
#endif
    want_eq("denied: fstatat the two-level leaf", fstatat_rc(AT_FDCWD, DEEP_LEAF), -EACCES);
    want_eq("denied: access the one-level leaf", syscall3(SYS_ACCESS, (long)LEAF, R_OK, 0),
            -EACCES);
    want_eq("denied: access a missing name", syscall3(SYS_ACCESS, (long)NOSUCH, F_OK, 0), -EACCES);
    want_eq("denied: faccessat the two-level leaf",
            syscall4(SYS_FACCESSAT, AT_FDCWD, (long)DEEP_LEAF, R_OK, 0), -EACCES);

    /* Directory descriptors opened while the ancestor was searchable. */
    want_eq("denied: openat through the denied dirfd", openat_ro(dfd_denied, "leaf.txt"), -EACCES);
    want_eq("denied: openat through the shared dirfd", openat_ro(dfd_shared, "victim"), -EACCES);

    /* chdir's own check on its target refuses the one-level chdir. */
    want_eq("denied: chdir one level", syscall3(SYS_CHDIR, (long)DENIED, 0, 0), -EACCES);
    want_eq("denied: chdir two levels", syscall3(SYS_CHDIR, (long)DEEP, 0, 0), -EACCES);
    want_eq("denied: chdir into the 0777 directory", syscall3(SYS_CHDIR, (long)SHARED, 0, 0),
            -EACCES);

    want_eq("denied: exec the staged program", run_exec(PROG), 128 + EACCES);
    want_eq("denied: exec a missing name", run_exec(NOSUCH), 128 + EACCES);

    /* First round: inside the 0777 directory, so only an ancestor refuses. */
    want_eq("denied: creat", syscall3(SYS_CREAT, (long)NEWFILE, 0644, 0), -EACCES);
    want_eq("denied: open with O_CREAT",
            syscall3(SYS_OPEN, (long)CREATED, O_WRONLY | O_CREAT, 0644), -EACCES);
    want_eq("denied: mkdir", syscall3(SYS_MKDIR, (long)NEWDIR, 0755, 0), -EACCES);
    want_eq("denied: rmdir", syscall3(SYS_RMDIR, (long)RMME, 0, 0), -EACCES);
    want_eq("denied: unlink", syscall3(SYS_UNLINK, (long)VICTIM, 0, 0), -EACCES);
    want_eq("denied: rename out of the tree", syscall3(SYS_RENAME, (long)MOVER, (long)MOVED, 0),
            -EACCES);
    want_eq("denied: rename into the tree", syscall3(SYS_RENAME, (long)OUTSRC, (long)LANDED, 0),
            -EACCES);
    want_eq("denied: chmod its own file", syscall3(SYS_CHMOD, (long)MINE, 0600, 0), -EACCES);
    want_eq("denied: fchmodat its own file", syscall4(SYS_FCHMODAT, AT_FDCWD, (long)MINE, 0600, 0),
            -EACCES);
    want_eq("denied: fchownat with no change",
            syscall5(SYS_FCHOWNAT, AT_FDCWD, (long)MINE, -1, -1, 0), -EACCES);
    want_eq("denied: utimensat", syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)VICTIM, 0, 0), -EACCES);
    want_eq("denied: linkat", syscall5(SYS_LINKAT, AT_FDCWD, (long)VICTIM, AT_FDCWD, (long)LNK, 0),
            -EACCES);

    /* Second round: in /sp_denied, which grants write and withholds search. */
    want_eq("denied: creat under the denied parent", syscall3(SYS_CREAT, (long)PNEWFILE, 0644, 0),
            -EACCES);
    want_eq("denied: open with O_CREAT under the denied parent",
            syscall3(SYS_OPEN, (long)PCREATED, O_WRONLY | O_CREAT, 0644), -EACCES);
    want_eq("denied: mkdir under the denied parent", syscall3(SYS_MKDIR, (long)PNEWDIR, 0755, 0),
            -EACCES);
    want_eq("denied: rmdir under the denied parent", syscall3(SYS_RMDIR, (long)PRMME, 0, 0),
            -EACCES);
    want_eq("denied: unlink under the denied parent", syscall3(SYS_UNLINK, (long)PVICTIM, 0, 0),
            -EACCES);
    want_eq("denied: rename out of the denied parent",
            syscall3(SYS_RENAME, (long)PMOVER, (long)PMOVED, 0), -EACCES);
    want_eq("denied: rename into the denied parent",
            syscall3(SYS_RENAME, (long)POUTSRC, (long)PLANDED, 0), -EACCES);

    /* `..` is physical: the walk enters the denied directory first. */
    want_eq("denied: open through the denied directory's ..", open_ro(UPWARD), -EACCES);
    want_ok("denied: open the root", open_ro(ROOT));
    want_eq("denied: search the root", syscall3(SYS_ACCESS, (long)ROOT, X_OK, 0), 0);

    syscall1(SYS_EXIT, 0);
}

/** With the denial moved from /sp_denied down to /sp_denied/deep, the answer
 * follows the directory that denies and not a decision cached from the first
 * walk. As in the denied child, chdir's own execute check refuses the chdir
 * into the denying directory whatever the walk charged; the open and the stat
 * below it are what the walk decides. Exits 0, or fails the probe. */
static void moved_child(void)
{
    int fd;

    drop_to_1000();

    want_ok("moved: open the one-level leaf", open_ro(LEAF));
    want_ok("moved: openat through the denied dirfd", openat_ro(dfd_denied, "leaf.txt"));
    want_eq("moved: open under the new denier", open_ro(DEEP_LEAF), -EACCES);
    want_eq("moved: stat a missing name under the new denier", stat_rc(DEEP_NOSUCH), -EACCES);
    want_eq("moved: chdir into the new denier", syscall3(SYS_CHDIR, (long)DEEP, 0, 0), -EACCES);
    want_eq("moved: chdir into its searchable parent", syscall3(SYS_CHDIR, (long)DENIED, 0, 0), 0);
    want_eq("moved: chdir back to the root", syscall3(SYS_CHDIR, (long)ROOT, 0, 0), 0);

    fd = syscall3(SYS_CREAT, (long)THIRD, 0644, 0);
    want_ok("moved: creat in the reachable 0777 directory", fd);
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("moved: unlink it again", syscall3(SYS_UNLINK, (long)THIRD, 0, 0), 0);

    syscall1(SYS_EXIT, 0);
}

/** Run `body` in a child to its end and fail the probe, as `what`, unless it
 * exits 0. WIFEXITED comes first, since a signal death has the signal in the
 * low bits and would read as exit 0. A failing child has already printed the
 * case it failed. */
static void run_child(void (*body)(void), const char *what)
{
    int pid, status = 0;
    int reaped;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail(what, pid);
    }
    if (pid == 0) {
        body();
        /* Every body exits itself; one that returns fails with 127. */
        syscall1(SYS_EXIT, 127);
    }
    reaped = syscall3(SYS_WAITPID, pid, (long)&status, 0);
    if (reaped != pid) {
        fail(what, reaped);
    }
    if (!WIFEXITED(status)) {
        puts_("SEARCHPROBE FAIL: ");
        puts_(what);
        puts_(": died by signal ");
        putd(status & 0x7F);
        puts_("\n");
        syscall1(SYS_EXIT, 1);
    }
    if (WEXITSTATUS(status) != 0) {
        fail(what, WEXITSTATUS(status));
    }
}

/** Create the file `path` with `mode`, which a chmod sets again, since the
 * umask the boot shell passed down clears bits of the mode creat is given. */
static void mint_file(const char *path, u32 mode)
{
    int fd = syscall3(SYS_CREAT, (long)path, (int)mode, 0);

    if (fd < 0) {
        fail("mint a fixture file", fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("chmod a fixture file", syscall3(SYS_CHMOD, (long)path, (int)mode, 0), 0);
}

/** The same for the directory `path`. */
static void mint_dir(const char *path, u32 mode)
{
    want_eq("mint a fixture directory", syscall3(SYS_MKDIR, (long)path, (int)mode, 0), 0);
    want_eq("chmod a fixture directory", syscall3(SYS_CHMOD, (long)path, (int)mode, 0), 0);
}

/**
 * The probe's entry, the root parent. It checks the staged modes, creates the
 * targets of the changes, and opens the two directory descriptors; runs the
 * baseline child, then the denied child with /sp_denied at 0666, and checks
 * that the denied changes left nothing done; runs the third child with the
 * denial on /sp_denied/deep; and removes what it created.
 *
 * `victim` and the two movers are root's and 0666, so uid 1000 may write
 * them without owning them; `mine` is uid 1000's own, so only the ancestor
 * can refuse its chmod.
 */
PROBE_START void _start(void)
{
    /* The staged tree is as the manifest declares it. */
    want_mode("staged /sp_denied", DENIED, 0777);
    want_mode("staged /sp_denied/deep", DEEP, 0777);
    want_mode("staged /sp_denied/shared", SHARED, 0777);
    want_mode("staged /sp_denied/leaf.txt", LEAF, 0644);
    want_mode("staged /sp_denied/deep/leaf.txt", DEEP_LEAF, 0644);
    want_mode("staged /sp_denied/prog", PROG, 0755);

    /* The targets of the changes, each rename with a source of its own. */
    mint_file(VICTIM, 0666);
    mint_file(MOVER, 0666);
    mint_file(MINE, 0644);
    want_eq("give mine to uid 1000", syscall3(SYS_CHOWN, (long)MINE, 1000, 1000), 0);
    mint_dir(RMME, 0755);
    mint_file(PVICTIM, 0666);
    mint_file(PMOVER, 0666);
    mint_dir(PRMME, 0755);
    mint_dir(OUTDIR, 0777);
    mint_file(OUTSRC, 0666);
    mint_file(POUTSRC, 0666);
    mint_file(UPFILE, 0644);

    dfd_denied = syscall3(SYS_OPEN, (long)DENIED, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open a dirfd on /sp_denied", dfd_denied);
    dfd_shared = syscall3(SYS_OPEN, (long)SHARED, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open a dirfd on /sp_denied/shared", dfd_shared);

    run_child(baseline_child, "the baseline child");

    /* One bit down from the staged 0777: read and write stay, search goes. */
    want_eq("close /sp_denied to 0666", syscall3(SYS_CHMOD, (long)DENIED, 0666, 0), 0);
    run_child(denied_child, "the denied child");

    /* Refused and nothing done; linkat is not checked, having no hard links. */
    want_eq("creat left a file behind", stat_rc(NEWFILE), -ENOENT);
    want_eq("O_CREAT left a file behind", stat_rc(CREATED), -ENOENT);
    want_eq("mkdir left a directory behind", stat_rc(NEWDIR), -ENOENT);
    want_eq("rename into the tree landed", stat_rc(LANDED), -ENOENT);
    want_eq("rename out of the tree landed", stat_rc(MOVED), -ENOENT);
    want_eq("unlink took the victim away", stat_rc(VICTIM), 0);
    want_eq("rename took the mover away", stat_rc(MOVER), 0);
    want_eq("rmdir removed the directory", stat_rc(RMME), 0);
    want_eq("rename took the outside source away", stat_rc(OUTSRC), 0);
    want_mode("chmod got through the denied ancestor", MINE, 0644);

    want_eq("creat under the denied parent left a file behind", stat_rc(PNEWFILE), -ENOENT);
    want_eq("O_CREAT under the denied parent left a file behind", stat_rc(PCREATED), -ENOENT);
    want_eq("mkdir under the denied parent left a directory behind", stat_rc(PNEWDIR), -ENOENT);
    want_eq("rename into the denied parent landed", stat_rc(PLANDED), -ENOENT);
    want_eq("rename out of the denied parent landed", stat_rc(PMOVED), -ENOENT);
    want_eq("unlink took the denied parent's victim away", stat_rc(PVICTIM), 0);
    want_eq("rename took the denied parent's mover away", stat_rc(PMOVER), 0);
    want_eq("rmdir removed the denied parent's directory", stat_rc(PRMME), 0);
    want_eq("rename took the second outside source away", stat_rc(POUTSRC), 0);

    /* Move the denial one level down and ask again. */
    want_eq("reopen /sp_denied to 0777", syscall3(SYS_CHMOD, (long)DENIED, 0777, 0), 0);
    want_eq("close /sp_denied/deep to 0666", syscall3(SYS_CHMOD, (long)DEEP, 0666, 0), 0);
    run_child(moved_child, "the moved-denial child");
    want_eq("reopen /sp_denied/deep to 0777", syscall3(SYS_CHMOD, (long)DEEP, 0777, 0), 0);

    /* Hand the image back the way the manifest staged it. */
    syscall3(SYS_CLOSE, dfd_denied, 0, 0);
    syscall3(SYS_CLOSE, dfd_shared, 0, 0);
    want_eq("remove the victim", syscall3(SYS_UNLINK, (long)VICTIM, 0, 0), 0);
    want_eq("remove the mover", syscall3(SYS_UNLINK, (long)MOVER, 0, 0), 0);
    want_eq("remove mine", syscall3(SYS_UNLINK, (long)MINE, 0, 0), 0);
    want_eq("remove the rmdir target", syscall3(SYS_RMDIR, (long)RMME, 0, 0), 0);
    want_eq("remove the second victim", syscall3(SYS_UNLINK, (long)PVICTIM, 0, 0), 0);
    want_eq("remove the second mover", syscall3(SYS_UNLINK, (long)PMOVER, 0, 0), 0);
    want_eq("remove the second rmdir target", syscall3(SYS_RMDIR, (long)PRMME, 0, 0), 0);
    want_eq("remove the outside source", syscall3(SYS_UNLINK, (long)OUTSRC, 0, 0), 0);
    want_eq("remove the second outside source", syscall3(SYS_UNLINK, (long)POUTSRC, 0, 0), 0);
    want_eq("remove the outside directory", syscall3(SYS_RMDIR, (long)OUTDIR, 0, 0), 0);
    want_eq("remove the upward target", syscall3(SYS_UNLINK, (long)UPFILE, 0, 0), 0);

    puts_("SEARCHPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
