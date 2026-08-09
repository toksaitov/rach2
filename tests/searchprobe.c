/**
 * Verify that search (x) is charged on every directory a path enters, not
 * only on the target and on its immediate parent.
 *
 * The rule under test: a process that may not search a directory reaches
 * nothing below it, whatever the bits on the leaf say, and cannot learn
 * whether a name below it exists. The fixture is one root-owned tree whose
 * modes the probe's root parent flips between three uid-1000 children, so that
 * a single denied directory is the only difference between a call that works
 * and the same call that does not.
 *
 * A denial is 0777 turned into 0666, one bit of the three the caller is judged
 * by. Read and write stay granted, so nothing but the withheld execute bit can
 * account for a refusal. The obvious 0755 turned into 0700 would take the read
 * bit away in the same stroke, and a kernel that charged read where it must
 * charge execute would pass unremarked.
 *
 * The baseline child runs first, while the tree is 0777 throughout, and does
 * every read and every mutation the denied child will be refused. Without it a
 * later -EACCES could be coming from the leaf's own bits or from the directory
 * the name lands in, and the probe would prove nothing.
 *
 * The denied child then runs with `/sp_denied` at 0666 root and asserts exact
 * errnos, one level down and two, across open, stat, access, chdir, execve,
 * unlink, chmod, chown, utimensat, link, O_CREAT, creat, mkdir, rmdir and
 * rename in both directions, plus the *at forms this tree implements
 * (openat, fstatat64, faccessat, fchmodat, fchownat, utimensat, linkat; there
 * is no unlinkat, renameat, or mkdirat here). A name that does not exist under
 * the denied directory must answer -EACCES and not -ENOENT, or a caller probes
 * for existence through a directory it may not search. An openat through a
 * dirfd opened while the ancestor was still searchable must be refused too: a
 * dirfd here is a name, not a capability.
 *
 * Its mutations run in two rounds, and the two charge different directories.
 * The first aims inside `/sp_denied/shared`, which stays 0777: the directory
 * the name lands in grants write to everyone, so only an ancestor above it can
 * refuse. The second aims directly into `/sp_denied`, which grants write and
 * withholds search, so the only thing left that can refuse is the immediate
 * parent's own execute bit. That second round is what reaches each syscall's
 * own full-path resolve, since the write check walks as far as the parent and
 * stops, and a walk never charges the directory it ends on: a kernel that
 * charged every ancestor but not the parent would answer the first round
 * correctly and let the second one through.
 *
 * The third child runs after the parent moves the denial one level down, so
 * the answer follows the directory that denies rather than a decision cached
 * from the first walk.
 *
 * Two opens must keep working throughout. `/sp_denied/../sp_up.txt` resolves,
 * because `..` collapses lexically before resolution and the denied directory
 * is never entered; `/` resolves, because the root is always searchable.
 * access(2) is charged by the real ids, so the denied child asks it twice,
 * once holding a real uid of 0 under an effective 1000 and once the other way
 * round, and the two answers must differ: that is the view a set-id program
 * asks for.
 *
 * chdir is charged twice over, and only one of the two charges is this probe's
 * subject: the walk enters every directory above the target, and chdir then
 * demands execute on the target itself. `chdir /sp_denied` in the denied child
 * and `chdir /sp_denied/deep` in the third one name the denied directory as
 * their own target, so chdir's own check refuses them whatever the walk
 * charged. They are kept as coverage of that second rule; the two-level chdir
 * and the chdir into the reachable 0777 directory below are what carry the
 * search claim.
 *
 * The fixtures split two ways. `/sp_denied`, `/sp_denied/leaf.txt`,
 * `/sp_denied/deep/leaf.txt`, `/sp_denied/shared` and the program
 * `/sp_denied/prog` are staged by `krffs/manifests/test.manifest`: a
 * freestanding probe cannot mint an ELF, the execve cases need a real program
 * under the denied ancestor, and staging it drags in the directories that hold
 * it. Everything the mutations aim at, `/sp_out` outside the tree for the
 * renames that leave it, and `/sp_up.txt` for the lexical `..`, the root parent
 * mints and removes instead, so the probe hands the image back exactly as the
 * manifest staged it, runs twice in one boot, and rests on no file another
 * probe owns. The parent is root throughout, and its override is what lets it
 * stage and unstage a tree it has just made unreachable to everyone else.
 *
 * After the denied child the parent asserts that nothing that child attempted
 * was carried out: the created names absent, the removed ones present, the
 * chmod'd file's mode unchanged. That is the only way to catch a refusal
 * reported after the work was already done, which is what an unchecked
 * `krffs_create` under a checked `open` would look like. Each rename is given a
 * source of its own, so that an absent destination cannot mean merely that an
 * earlier case had already taken the source away. linkat is the one call with
 * no such check: this file system has no hard links, so the link it was refused
 * could not have been made on any kernel, and its -EACCES against the
 * baseline's -EPERM is the whole of the evidence.
 *
 * Every wait checks WIFEXITED before WEXITSTATUS: a signal death carries the
 * signal in the low bits and would otherwise decode as exit 0, scoring a
 * crashed child as a passing one. Prints "SEARCHPROBE OK" and exits 0 only
 * when every case passed; any failure names the step and the answer it got,
 * and exits 1. Freestanding; direct int 0x80.
 */

typedef unsigned int u32;

#define SYS_EXIT        1
#define SYS_FORK        2
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_WAITPID     7
#define SYS_CREAT       8
#define SYS_UNLINK      10
#define SYS_EXECVE      11
#define SYS_CHDIR       12
#define SYS_CHMOD       15
#define SYS_ACCESS      33
#define SYS_RENAME      38
#define SYS_MKDIR       39
#define SYS_RMDIR       40
#define SYS_STAT        106
#define SYS_STAT64      195
#define SYS_SETRESUID32 208
#define SYS_SETRESGID32 210
#define SYS_CHOWN32     212
#define SYS_OPENAT      295
#define SYS_FCHOWNAT    298
#define SYS_FSTATAT64   300
#define SYS_LINKAT      303
#define SYS_FCHMODAT    306
#define SYS_FACCESSAT   307
#define SYS_UTIMENSAT   320

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

/* i386 struct stat64: the 32-bit mode field is at offset 16. */
#define ST_MODE 16

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death carries the signal in the low 7 bits. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

/* Staged by krffs/manifests/test.manifest, every directory 0777. */
#define DENIED    "/sp_denied"
#define LEAF      "/sp_denied/leaf.txt"
#define DEEP      "/sp_denied/deep"
#define DEEP_LEAF "/sp_denied/deep/leaf.txt"
#define SHARED    "/sp_denied/shared"
#define PROG      "/sp_denied/prog"

/* Minted and removed by the root parent: what the first round of mutations
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

/* Names that do not exist: the denial must beat the absence. */
#define NOSUCH      "/sp_denied/nosuch"
#define DEEP_NOSUCH "/sp_denied/deep/nosuch"

/* The two openable paths a denied ancestor must never cost the caller. */
#define UPWARD "/sp_denied/../sp_up.txt"
#define ROOT   "/"

static int syscall1(int num, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(num), "b"(a) : "memory");
    return r;
}

static int syscall3(int num, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int syscall4(int num, int a, int b, int c, int d)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d)
                         : "memory");
    return r;
}

static int syscall5(int num, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(num), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (int)s, (int)slen(s));
}

/* Print a signed decimal, so a failing line carries the answer it got. */
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

/* Print an unsigned octal with a leading zero: the only readable shape for a
 * mode. */
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

static void fail(const char *step, int got)
{
    puts_("SEARCHPROBE FAIL: ");
    puts_(step);
    puts_(" (got ");
    putd(got);
    puts_(")\n");
    syscall1(SYS_EXIT, 1);
}

/* The answer must be exactly `want`: an errno family, not merely a sign. */
static void want_eq(const char *step, int got, int want)
{
    if (got != want) {
        fail(step, got);
    }
}

/* The call must have succeeded (open returns a descriptor, so not just 0). */
static void want_ok(const char *step, int got)
{
    if (got < 0) {
        fail(step, got);
    }
}

/* Open `path` read-only and close it again, reporting open's own answer. */
static int open_ro(const char *path)
{
    int fd = syscall3(SYS_OPEN, (int)path, O_RDONLY, 0);

    if (fd >= 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
    }
    return fd;
}

/* As open_ro, but relative to a directory descriptor. */
static int openat_ro(int dirfd, const char *name)
{
    int fd = syscall4(SYS_OPENAT, dirfd, (int)name, O_RDONLY, 0);

    if (fd >= 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
    }
    return fd;
}

/* The one stat64 buffer: the kernel writes it, stat_mode reads it back. */
static char statbuf[144];

/* stat64's own answer, for the existence assertions. */
static int stat_rc(const char *path)
{
    return syscall3(SYS_STAT64, (int)path, (int)statbuf, 0);
}

/* The legacy stat(2) (106), which has its own resolve site. musl builds on
 * stat64, so nothing but a freestanding caller reaches this one, and a mutation
 * that collapsed its walk error went uncaught until this case existed. */
static int old_stat_rc(const char *path)
{
    return syscall3(SYS_STAT, (int)path, (int)statbuf, 0);
}

/* fstatat64's own answer, the *at form of the same question. */
static int fstatat_rc(int dirfd, const char *path)
{
    return syscall4(SYS_FSTATAT64, dirfd, (int)path, (int)statbuf, 0);
}

/* The permission bits of `path`; a stat that fails is a setup failure. */
static u32 stat_mode(const char *path)
{
    int rc = stat_rc(path);

    if (rc != 0) {
        fail("stat64 of a fixture", rc);
    }
    return *(u32 *)(statbuf + ST_MODE) & 07777u;
}

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

/* Exec `path` in a fresh child and report what became of it: 0 when the
 * program ran and exited 0, or 128 + errno when the exec was refused. The fork
 * is what makes the two answers distinguishable, since a successful exec
 * leaves nothing behind to report it. */
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
        e = syscall3(SYS_EXECVE, (int)path, (int)argv, 0);
        syscall1(SYS_EXIT, 128 + ((-e) & 0x7F));
    }
    reaped = syscall3(SYS_WAITPID, pid, (int)&status, 0);
    if (reaped != pid) {
        fail("waitpid for exec", reaped);
    }
    if (!WIFEXITED(status)) {
        fail("exec child died by signal", status & 0x7F);
    }
    return WEXITSTATUS(status);
}

/* Become uid and gid 1000 outright, with no saved id to swing back on. The
 * gids go first: once the effective uid is 1000 the gid calls are refused. */
static void drop_to_1000(void)
{
    want_eq("drop the gids", syscall3(SYS_SETRESGID32, 1000, 1000, 1000), 0);
    want_eq("drop the uids", syscall3(SYS_SETRESUID32, 1000, 1000, 1000), 0);
}

/* Dirfds the parent opens while the ancestor is still searchable, inherited by
 * every child across the fork. */
static int dfd_denied = -1;
static int dfd_shared = -1;

/* Everything the denied child is refused, done here while the whole tree is
 * 0777, so that a later -EACCES cannot be the leaf's bits or the bits of the
 * directory the name lands in talking. */
static void baseline_child(void)
{
    int fd;

    drop_to_1000();

    want_ok("baseline: open the one-level leaf", open_ro(LEAF));
    want_ok("baseline: open the two-level leaf", open_ro(DEEP_LEAF));
    want_eq("baseline: stat the two-level leaf", stat_rc(DEEP_LEAF), 0);
    want_eq("baseline: legacy stat the one-level leaf", old_stat_rc(LEAF), 0);
    want_eq("baseline: fstatat the two-level leaf", fstatat_rc(AT_FDCWD, DEEP_LEAF), 0);
    want_eq("baseline: access the one-level leaf", syscall3(SYS_ACCESS, (int)LEAF, R_OK, 0), 0);
    want_eq("baseline: faccessat the two-level leaf",
            syscall4(SYS_FACCESSAT, AT_FDCWD, (int)DEEP_LEAF, R_OK, 0), 0);
    want_ok("baseline: openat through the denied dirfd", openat_ro(dfd_denied, "leaf.txt"));
    want_ok("baseline: openat through the shared dirfd", openat_ro(dfd_shared, "victim"));

    want_eq("baseline: chdir one level", syscall3(SYS_CHDIR, (int)DENIED, 0, 0), 0);
    want_eq("baseline: chdir two levels", syscall3(SYS_CHDIR, (int)DEEP, 0, 0), 0);
    want_eq("baseline: chdir back to the root", syscall3(SYS_CHDIR, (int)ROOT, 0, 0), 0);

    want_eq("baseline: exec the staged program", run_exec(PROG), 0);
    want_eq("baseline: exec a missing name", run_exec(NOSUCH), 128 + ENOENT);

    /* The first round's ground: what uid 1000 may do inside the 0777
     * directory. */
    fd = syscall3(SYS_CREAT, (int)BL, 0644, 0);
    want_ok("baseline: creat in the 0777 directory", fd);
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("baseline: chmod its own file", syscall3(SYS_CHMOD, (int)BL, 0600, 0), 0);
    want_eq("baseline: fchmodat its own file", syscall4(SYS_FCHMODAT, AT_FDCWD, (int)BL, 0644, 0),
            0);
    want_eq("baseline: fchownat with no change",
            syscall5(SYS_FCHOWNAT, AT_FDCWD, (int)BL, -1, -1, 0), 0);
    want_eq("baseline: utimensat through the other-write bit",
            syscall4(SYS_UTIMENSAT, AT_FDCWD, (int)VICTIM, 0, 0), 0);
    want_eq("baseline: linkat reaches the no-hard-links answer",
            syscall5(SYS_LINKAT, AT_FDCWD, (int)VICTIM, AT_FDCWD, (int)LNK, 0), -EPERM);
    want_eq("baseline: mkdir in the 0777 directory", syscall3(SYS_MKDIR, (int)BLDIR, 0755, 0), 0);
    want_eq("baseline: rmdir in the 0777 directory", syscall3(SYS_RMDIR, (int)BLDIR, 0, 0), 0);
    want_eq("baseline: rename out of the tree", syscall3(SYS_RENAME, (int)BL, (int)OUTBL, 0), 0);
    want_eq("baseline: rename back into the tree", syscall3(SYS_RENAME, (int)OUTBL, (int)BL, 0), 0);
    want_eq("baseline: unlink in the 0777 directory", syscall3(SYS_UNLINK, (int)BL, 0, 0), 0);

    /* The second round's ground: the same rights directly inside /sp_denied,
     * which the denied child will find at 0666. Between the two children that
     * directory loses its execute bit and keeps its write bit, so a refusal
     * there can be nothing but the search charge. */
    fd = syscall3(SYS_CREAT, (int)PBL, 0644, 0);
    want_ok("baseline: creat directly under /sp_denied", fd);
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("baseline: mkdir directly under /sp_denied", syscall3(SYS_MKDIR, (int)PBLDIR, 0755, 0),
            0);
    want_eq("baseline: rmdir directly under /sp_denied", syscall3(SYS_RMDIR, (int)PBLDIR, 0, 0), 0);
    want_eq("baseline: rename out of /sp_denied", syscall3(SYS_RENAME, (int)PBL, (int)POUTBL, 0),
            0);
    want_eq("baseline: rename back into /sp_denied", syscall3(SYS_RENAME, (int)POUTBL, (int)PBL, 0),
            0);
    want_eq("baseline: unlink directly under /sp_denied", syscall3(SYS_UNLINK, (int)PBL, 0, 0), 0);

    syscall1(SYS_EXIT, 0);
}

/* The same calls with /sp_denied at 0666 root. Every call that enters the
 * directory must answer -EACCES; the paths that never enter it (a lexical
 * `..`, the root) and the questions asked while the real uid is still 0 must
 * go on succeeding, or the rule would be denying more than search. */
static void denied_child(void)
{
    /* The effective id alone is 1000: the walk is charged by it, and access(2)
     * is charged by the real id, which is still 0. */
    want_eq("drop the gids", syscall3(SYS_SETRESGID32, 1000, 1000, 1000), 0);
    want_eq("drop only the effective uid", syscall3(SYS_SETRESUID32, 0, 1000, 0), 0);
    want_eq("denied: open is charged by the effective id", open_ro(LEAF), -EACCES);
    want_eq("denied: access is charged by the real id (root)",
            syscall3(SYS_ACCESS, (int)LEAF, R_OK, 0), 0);

    /* Now the set-id program's own view: real 1000, effective 0. The answers
     * must swap, which is what makes access(2) worth asking. */
    want_eq("take the effective id back", syscall3(SYS_SETRESUID32, 1000, 0, 0), 0);
    want_ok("denied: open under the effective root", open_ro(LEAF));
    want_eq("denied: access is charged by the real id (1000)",
            syscall3(SYS_ACCESS, (int)LEAF, R_OK, 0), -EACCES);

    want_eq("drop the uids", syscall3(SYS_SETRESUID32, 1000, 1000, 1000), 0);

    /* The reads, one level and two, and a name that is not there. */
    want_eq("denied: open the one-level leaf", open_ro(LEAF), -EACCES);
    want_eq("denied: open the two-level leaf", open_ro(DEEP_LEAF), -EACCES);
    want_eq("denied: open a missing one-level name", open_ro(NOSUCH), -EACCES);
    want_eq("denied: open a missing two-level name", open_ro(DEEP_NOSUCH), -EACCES);
    want_eq("denied: stat the one-level leaf", stat_rc(LEAF), -EACCES);
    want_eq("denied: stat a missing name", stat_rc(NOSUCH), -EACCES);
    want_eq("denied: legacy stat the one-level leaf", old_stat_rc(LEAF), -EACCES);
    want_eq("denied: legacy stat a missing name", old_stat_rc(NOSUCH), -EACCES);
    want_eq("denied: fstatat the two-level leaf", fstatat_rc(AT_FDCWD, DEEP_LEAF), -EACCES);
    want_eq("denied: access the one-level leaf", syscall3(SYS_ACCESS, (int)LEAF, R_OK, 0), -EACCES);
    want_eq("denied: access a missing name", syscall3(SYS_ACCESS, (int)NOSUCH, F_OK, 0), -EACCES);
    want_eq("denied: faccessat the two-level leaf",
            syscall4(SYS_FACCESSAT, AT_FDCWD, (int)DEEP_LEAF, R_OK, 0), -EACCES);

    /* A dirfd is a name, not a capability: both were opened while the ancestor
     * was still searchable. */
    want_eq("denied: openat through the denied dirfd", openat_ro(dfd_denied, "leaf.txt"), -EACCES);
    want_eq("denied: openat through the shared dirfd", openat_ro(dfd_shared, "victim"), -EACCES);

    /* The two-level chdir and the chdir into the 0777 directory are the ones
     * the walk decides; the one-level chdir names the denied directory as its
     * own target, which chdir's own execute check refuses on its own. */
    want_eq("denied: chdir one level", syscall3(SYS_CHDIR, (int)DENIED, 0, 0), -EACCES);
    want_eq("denied: chdir two levels", syscall3(SYS_CHDIR, (int)DEEP, 0, 0), -EACCES);
    want_eq("denied: chdir into the 0777 directory", syscall3(SYS_CHDIR, (int)SHARED, 0, 0),
            -EACCES);

    want_eq("denied: exec the staged program", run_exec(PROG), 128 + EACCES);
    want_eq("denied: exec a missing name", run_exec(NOSUCH), 128 + EACCES);

    /* First round: inside the 0777 directory the baseline child has just
     * proved writable by uid 1000, so only the ancestor above it can refuse. */
    want_eq("denied: creat", syscall3(SYS_CREAT, (int)NEWFILE, 0644, 0), -EACCES);
    want_eq("denied: open with O_CREAT", syscall3(SYS_OPEN, (int)CREATED, O_WRONLY | O_CREAT, 0644),
            -EACCES);
    want_eq("denied: mkdir", syscall3(SYS_MKDIR, (int)NEWDIR, 0755, 0), -EACCES);
    want_eq("denied: rmdir", syscall3(SYS_RMDIR, (int)RMME, 0, 0), -EACCES);
    want_eq("denied: unlink", syscall3(SYS_UNLINK, (int)VICTIM, 0, 0), -EACCES);
    want_eq("denied: rename out of the tree", syscall3(SYS_RENAME, (int)MOVER, (int)MOVED, 0),
            -EACCES);
    want_eq("denied: rename into the tree", syscall3(SYS_RENAME, (int)OUTSRC, (int)LANDED, 0),
            -EACCES);
    want_eq("denied: chmod its own file", syscall3(SYS_CHMOD, (int)MINE, 0600, 0), -EACCES);
    want_eq("denied: fchmodat its own file", syscall4(SYS_FCHMODAT, AT_FDCWD, (int)MINE, 0600, 0),
            -EACCES);
    want_eq("denied: fchownat with no change",
            syscall5(SYS_FCHOWNAT, AT_FDCWD, (int)MINE, -1, -1, 0), -EACCES);
    want_eq("denied: utimensat", syscall4(SYS_UTIMENSAT, AT_FDCWD, (int)VICTIM, 0, 0), -EACCES);
    want_eq("denied: linkat", syscall5(SYS_LINKAT, AT_FDCWD, (int)VICTIM, AT_FDCWD, (int)LNK, 0),
            -EACCES);

    /* Second round: directly inside /sp_denied, which grants write to everyone
     * and withholds search. The write check walks only as far as this
     * directory and a walk never charges the one it ends on, so each of these
     * rests on the syscall's own full-path resolve and on nothing else. */
    want_eq("denied: creat under the denied parent", syscall3(SYS_CREAT, (int)PNEWFILE, 0644, 0),
            -EACCES);
    want_eq("denied: open with O_CREAT under the denied parent",
            syscall3(SYS_OPEN, (int)PCREATED, O_WRONLY | O_CREAT, 0644), -EACCES);
    want_eq("denied: mkdir under the denied parent", syscall3(SYS_MKDIR, (int)PNEWDIR, 0755, 0),
            -EACCES);
    want_eq("denied: rmdir under the denied parent", syscall3(SYS_RMDIR, (int)PRMME, 0, 0),
            -EACCES);
    want_eq("denied: unlink under the denied parent", syscall3(SYS_UNLINK, (int)PVICTIM, 0, 0),
            -EACCES);
    want_eq("denied: rename out of the denied parent",
            syscall3(SYS_RENAME, (int)PMOVER, (int)PMOVED, 0), -EACCES);
    want_eq("denied: rename into the denied parent",
            syscall3(SYS_RENAME, (int)POUTSRC, (int)PLANDED, 0), -EACCES);

    /* What the denial must not cost: `..` collapses lexically, so the denied
     * directory is transited and never entered, and the root is searchable by
     * everyone. */
    want_ok("denied: open through a lexical ..", open_ro(UPWARD));
    want_ok("denied: open the root", open_ro(ROOT));
    want_eq("denied: search the root", syscall3(SYS_ACCESS, (int)ROOT, X_OK, 0), 0);

    syscall1(SYS_EXIT, 0);
}

/* With the denial moved from /sp_denied down to /sp_denied/deep: the answer
 * follows the directory that denies, not a decision cached from the first
 * walk. As in the denied child, the chdir into the denier itself is refused by
 * chdir's own execute check whatever the walk charged; the open and the stat
 * below it are what the walk decides. */
static void moved_child(void)
{
    int fd;

    drop_to_1000();

    want_ok("moved: open the one-level leaf", open_ro(LEAF));
    want_ok("moved: openat through the denied dirfd", openat_ro(dfd_denied, "leaf.txt"));
    want_eq("moved: open under the new denier", open_ro(DEEP_LEAF), -EACCES);
    want_eq("moved: stat a missing name under the new denier", stat_rc(DEEP_NOSUCH), -EACCES);
    want_eq("moved: chdir into the new denier", syscall3(SYS_CHDIR, (int)DEEP, 0, 0), -EACCES);
    want_eq("moved: chdir into its searchable parent", syscall3(SYS_CHDIR, (int)DENIED, 0, 0), 0);
    want_eq("moved: chdir back to the root", syscall3(SYS_CHDIR, (int)ROOT, 0, 0), 0);

    fd = syscall3(SYS_CREAT, (int)THIRD, 0644, 0);
    want_ok("moved: creat in the reachable 0777 directory", fd);
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("moved: unlink it again", syscall3(SYS_UNLINK, (int)THIRD, 0, 0), 0);

    syscall1(SYS_EXIT, 0);
}

/* Run one child to completion and require a clean exit 0. WIFEXITED comes
 * first: a signal death carries the signal in the low bits and would otherwise
 * decode as exit 0. The child has already named the case it failed. */
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
        /* Unreachable: every body exits itself. Non-zero so that a body that
         * ever grew a path out the bottom would fail rather than pass. */
        syscall1(SYS_EXIT, 127);
    }
    reaped = syscall3(SYS_WAITPID, pid, (int)&status, 0);
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

/* Create `path` with `mode`, pinned against the boot shell's umask. */
static void mint_file(const char *path, u32 mode)
{
    int fd = syscall3(SYS_CREAT, (int)path, (int)mode, 0);

    if (fd < 0) {
        fail("mint a fixture file", fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("chmod a fixture file", syscall3(SYS_CHMOD, (int)path, (int)mode, 0), 0);
}

static void mint_dir(const char *path, u32 mode)
{
    want_eq("mint a fixture directory", syscall3(SYS_MKDIR, (int)path, (int)mode, 0), 0);
    want_eq("chmod a fixture directory", syscall3(SYS_CHMOD, (int)path, (int)mode, 0), 0);
}

void _start(void)
{
    /* The staged tree is what the manifest declares, before anything flips
     * a mode: a probe that assumed its fixtures would prove nothing about
     * them. */
    want_mode("staged /sp_denied", DENIED, 0777);
    want_mode("staged /sp_denied/deep", DEEP, 0777);
    want_mode("staged /sp_denied/shared", SHARED, 0777);
    want_mode("staged /sp_denied/leaf.txt", LEAF, 0644);
    want_mode("staged /sp_denied/deep/leaf.txt", DEEP_LEAF, 0644);
    want_mode("staged /sp_denied/prog", PROG, 0755);

    /* The targets the mutations aim at. `victim` and the two movers are
     * root-owned and 0666, so uid 1000 holds write on them but not ownership;
     * `mine` is uid 1000's own, so the chmod cases are refused by the ancestor
     * and by nothing else. Each rename has a source of its own, so that no
     * later "the destination is not there" can rest on an earlier case having
     * removed the source. */
    mint_file(VICTIM, 0666);
    mint_file(MOVER, 0666);
    mint_file(MINE, 0644);
    want_eq("give mine to uid 1000", syscall3(SYS_CHOWN32, (int)MINE, 1000, 1000), 0);
    mint_dir(RMME, 0755);
    mint_file(PVICTIM, 0666);
    mint_file(PMOVER, 0666);
    mint_dir(PRMME, 0755);
    mint_dir(OUTDIR, 0777);
    mint_file(OUTSRC, 0666);
    mint_file(POUTSRC, 0666);
    mint_file(UPFILE, 0644);

    dfd_denied = syscall3(SYS_OPEN, (int)DENIED, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open a dirfd on /sp_denied", dfd_denied);
    dfd_shared = syscall3(SYS_OPEN, (int)SHARED, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open a dirfd on /sp_denied/shared", dfd_shared);

    run_child(baseline_child, "the baseline child");

    /* One bit down from the staged 0777: read and write stay, search goes. */
    want_eq("close /sp_denied to 0666", syscall3(SYS_CHMOD, (int)DENIED, 0666, 0), 0);
    run_child(denied_child, "the denied child");

    /* Refused and nothing done: the check an errno alone cannot make. linkat
     * is not among them, since a file system with no hard links could not have
     * made the link on any kernel. */
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
    want_eq("reopen /sp_denied to 0777", syscall3(SYS_CHMOD, (int)DENIED, 0777, 0), 0);
    want_eq("close /sp_denied/deep to 0666", syscall3(SYS_CHMOD, (int)DEEP, 0666, 0), 0);
    run_child(moved_child, "the moved-denial child");
    want_eq("reopen /sp_denied/deep to 0777", syscall3(SYS_CHMOD, (int)DEEP, 0777, 0), 0);

    /* Hand the image back the way the manifest staged it. */
    syscall3(SYS_CLOSE, dfd_denied, 0, 0);
    syscall3(SYS_CLOSE, dfd_shared, 0, 0);
    want_eq("remove the victim", syscall3(SYS_UNLINK, (int)VICTIM, 0, 0), 0);
    want_eq("remove the mover", syscall3(SYS_UNLINK, (int)MOVER, 0, 0), 0);
    want_eq("remove mine", syscall3(SYS_UNLINK, (int)MINE, 0, 0), 0);
    want_eq("remove the rmdir target", syscall3(SYS_RMDIR, (int)RMME, 0, 0), 0);
    want_eq("remove the second victim", syscall3(SYS_UNLINK, (int)PVICTIM, 0, 0), 0);
    want_eq("remove the second mover", syscall3(SYS_UNLINK, (int)PMOVER, 0, 0), 0);
    want_eq("remove the second rmdir target", syscall3(SYS_RMDIR, (int)PRMME, 0, 0), 0);
    want_eq("remove the outside source", syscall3(SYS_UNLINK, (int)OUTSRC, 0, 0), 0);
    want_eq("remove the second outside source", syscall3(SYS_UNLINK, (int)POUTSRC, 0, 0), 0);
    want_eq("remove the outside directory", syscall3(SYS_RMDIR, (int)OUTDIR, 0, 0), 0);
    want_eq("remove the lexical-.. target", syscall3(SYS_UNLINK, (int)UPFILE, 0, 0), 0);

    puts_("SEARCHPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
