/**
 * Verify that a modification takes the set-id bits away, and that exec's grant
 * and the clear read the same bit the same way.
 *
 * The rule under test has two halves that POSIX gives different privilege
 * rules, and the probe's job is to keep them apart. A write or a truncate
 * disarms a file unless the caller is privileged, which is the CAP_FSETID
 * analogue: without it any root process would silently disarm `/bin/su` by
 * rewriting it. A chown disarms it whoever asks, which is why `cp` and
 * `install` chown before they chmod. The assertion that separates the two is
 * the root parent's own chown: the same identity that was just exempted from
 * the write clear is not exempt here.
 *
 * Against that stands the case a clear must not reach. `chown(path, -1, -1)`
 * succeeds for any caller by design, since a value that changes nothing needs
 * no right, so a clear that ran on a no-change chown would let any uid disarm
 * a setuid-root program with one call. A uid-1000 child asks for exactly that
 * on a root-owned 04755 file and the mode must come back untouched.
 *
 * Setgid is dropped only when the group-execute bit is set with it, matching
 * the grant: `execve` honors `KRFFS_MODE_SGID` only on a mode that has
 * group-execute, so 02645 is an inert mark and survives while 02755 goes. A
 * directory keeps both bits whatever happens to it, since neither is an
 * execution grant there.
 *
 * The escalation needs a program and not just a mode, so the probe makes one:
 * the root parent copies the staged `/suidhelper` byte for byte to
 * `/mp_sgid0`, which keeps the fixture out of the manifest and out of phase
 * 3's set-id allowlist. It is chmod'd 02700 and chowned `1000:0`, in that
 * order, and the mode must survive the chown: 02700 has no group-execute,
 * so the clear spares its setgid bit. A uid-1000 child rewrites a byte of the
 * copy and stats it back at 02700, the survival the clear rule deliberately
 * allows, and a grandchild execs it with `suidhelper`'s `plain` scenario,
 * which exits 0 only if all three uids and all three gids read 1000 and
 * AT_SECURE is 0. The same child then chmods the copy 02710 and stats it back
 * at 0710, because an unprivileged caller who is not in the file's group may
 * not store setgid on a non-directory; that is the half no write clear can
 * reach, since the mode needs no write to appear. `/suidhelper.sgid`
 * (02755:0:5) is the paired positive that `suidprobe` still requires in the
 * same image, so the narrowed grant is pinned in both directions.
 *
 * Every fixture is minted and removed here, not staged. The probes
 * share one boot, and a probe that disarmed `suidprobe`'s staged fixtures
 * would buy itself a green run by breaking a later probe's premise.
 *
 * Every operation's return value is checked before its mode is asserted, and
 * every wait checks WIFEXITED before WEXITSTATUS: a signal death has the
 * signal in the low bits and would otherwise decode as exit 0, scoring a
 * crashed child as a passing one. Prints "MODEPROBE OK" and exits 0 only when
 * every case passed; any failure names the step and the answer it got, and
 * exits 1. Freestanding; direct int 0x80.
 */

typedef unsigned int u32;

#define SYS_EXIT        1
#define SYS_FORK        2
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_WAITPID     7
#define SYS_CREAT       8
#define SYS_UNLINK      10
#define SYS_EXECVE      11
#define SYS_CHMOD       15
#define SYS_MKDIR       39
#define SYS_RMDIR       40
#define SYS_STAT64      195
#define SYS_SETRESUID32 208
#define SYS_SETRESGID32 210
#define SYS_CHOWN32     212

#define STDOUT 1

#define O_RDONLY 0
#define O_WRONLY 1
#define O_TRUNC  01000

/* The chown sentinel that keeps a field: (uid_t)-1 as an unsigned word. */
#define KEEP 0xFFFFFFFFu

/* i386 struct stat64: the 32-bit mode field is at offset 16, and the 64-bit
 * size at 44. Only the low word of the size is read; a KRFFS file cannot reach
 * 4 GB, so the high word is always zero. */
#define ST_MODE 16
#define ST_SIZE 44

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death has the signal in the low 7 bits. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

/* The root parent's own fixtures, all root-owned. */
#define ROOTSUID "/mp_root"  /* the 04755 baseline and the write exemption */
#define CHOWNED  "/mp_chown" /* chowned away, so the bit must go */
#define DIRSUID  "/mp_dir"   /* a directory keeps what a file loses */
#define NOCHG    "/mp_nochg" /* the no-change chown a uid-1000 child asks for */

/* The relocation pair, minted back to back and in this order (see _start). */
#define GROW    "/mp_grow"
#define BLOCKER "/mp_block"

/* Given to uid 1000, which then modifies each one itself. */
#define INPLACE "/mp_wr"
#define TRUNCME "/mp_trunc"
#define CREATME "/mp_creat"
#define SGIDX   "/mp_sgid"    /* 02755: group-execute, so setgid goes */
#define SGIDNOX "/mp_sgid645" /* 02645: other-execute only, so setgid stays */
#define PLAIN   "/mp_plain"   /* 0755: the control */

/* The escalation: a runnable copy of the staged helper, owned 1000:0. */
#define HELPER "/suidhelper"
#define SGID0  "/mp_sgid0"

/* Bigger than one node header (357 bytes), so a write of this size can never
 * fit in the slack a `creat` left behind and must resize the node. */
#define GROW_BYTES 1024

/* The prefill every fixture the child writes in place starts with, comfortably
 * larger than the child's own 8-byte write. */
#define FILL_BYTES 64

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

/* Print a signed decimal, so a failing line has the answer it got. */
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
    puts_("MODEPROBE FAIL: ");
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

/* The one stat64 buffer: the kernel writes it, the readers below read it. */
static char statbuf[144];

static void stat_path(const char *path)
{
    int rc = syscall3(SYS_STAT64, (int)path, (int)statbuf, 0);

    if (rc != 0) {
        fail("stat64 of a fixture", rc);
    }
}

/* Assert the stored 07777 mode of `path`, always through a fresh path lookup:
 * a clear applied to a stale node pointer after a relocating write leaves the
 * real node untouched, and only a re-resolve can see that. */
static void want_mode(const char *step, const char *path, u32 mode)
{
    u32 got;

    stat_path(path);
    got = *(u32 *)(statbuf + ST_MODE) & 07777u;
    if (got != mode) {
        puts_("MODEPROBE FAIL: ");
        puts_(step);
        puts_(" (mode ");
        puto(got);
        puts_(" not ");
        puto(mode);
        puts_(")\n");
        syscall1(SYS_EXIT, 1);
    }
}

static void want_size(const char *step, const char *path, u32 size)
{
    u32 got;

    stat_path(path);
    got = *(u32 *)(statbuf + ST_SIZE);
    if (got != size) {
        fail(step, (int)got);
    }
}

/* The bytes every write in this probe emits; the content never matters, only
 * that the write moved some. */
static char blob[GROW_BYTES];

/* Write `count` bytes at offset 0 of `path`, which must already exist. */
static void write_bytes(const char *step, const char *path, u32 count)
{
    int fd = syscall3(SYS_OPEN, (int)path, O_WRONLY, 0);

    want_ok(step, fd);
    want_eq(step, syscall3(SYS_WRITE, fd, (int)blob, (int)count), (int)count);
    want_eq(step, syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

/* Become uid and gid 1000 outright, with no saved id to swing back on. The
 * gids go first: once the effective uid is 1000 the gid calls are refused. */
static void drop_to_1000(void)
{
    want_eq("drop the gids", syscall3(SYS_SETRESGID32, 1000, 1000, 1000), 0);
    want_eq("drop the uids", syscall3(SYS_SETRESUID32, 1000, 1000, 1000), 0);
}

/* Mint one fixture: create it, give it `fill` bytes of content, hand it to
 * `uid`:`gid`, and only then set its mode. The chown comes first because a
 * chown that changes a field clears the set-id bits whoever asks, so the
 * reverse order would store a mode the next call takes away again. */
static void mint(const char *path, u32 mode, u32 fill, u32 uid, u32 gid)
{
    int fd = syscall3(SYS_CREAT, (int)path, 0644, 0);

    want_ok("mint a fixture", fd);
    if (fill > 0) {
        want_eq("fill a fixture", syscall3(SYS_WRITE, fd, (int)blob, (int)fill), (int)fill);
    }
    want_eq("close a fixture", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("give a fixture away", syscall3(SYS_CHOWN32, (int)path, (int)uid, (int)gid), 0);
    want_eq("set a fixture's mode", syscall3(SYS_CHMOD, (int)path, (int)mode, 0), 0);
    want_mode("the fixture did not take its mode", path, mode);
}

/* Copy `src` to `dst` byte for byte through one read/write loop. A freestanding
 * probe cannot mint an ELF, and the escalation needs a program, not a
 * mode; copying the staged helper keeps the runnable set-id fixture out of the
 * manifest and out of phase 3's allowlist. */
static void copy_file(const char *src, const char *dst)
{
    static char buf[4096];
    int in, out, n;

    in = syscall3(SYS_OPEN, (int)src, O_RDONLY, 0);
    want_ok("open the staged helper", in);
    out = syscall3(SYS_CREAT, (int)dst, 0755, 0);
    want_ok("create the helper copy", out);
    while ((n = syscall3(SYS_READ, in, (int)buf, (int)sizeof(buf))) > 0) {
        want_eq("write the helper copy", syscall3(SYS_WRITE, out, (int)buf, n), n);
    }
    want_eq("read the staged helper", n, 0);
    want_eq("close the staged helper", syscall3(SYS_CLOSE, in, 0, 0), 0);
    want_eq("close the helper copy", syscall3(SYS_CLOSE, out, 0, 0), 0);
}

/* Exec `path arg` in a fresh child and report what became of it: the program's
 * own exit code, or 128 + errno when the exec was refused. The fork is what
 * makes the two answers distinguishable, since a successful exec leaves nothing
 * behind to report it. */
static int run_helper(const char *path, const char *arg)
{
    int pid, status = 0;
    int reaped;

    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail("fork for exec", pid);
    }
    if (pid == 0) {
        static char *argv[3];
        int e;

        argv[0] = (char *)path;
        argv[1] = (char *)arg;
        argv[2] = 0;
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

/* The no-change gate, then every clear a uid-1000 caller must suffer on its own
 * files. The fixtures were handed over by the root parent, so nothing here is
 * refused for want of a permission and a surviving bit is the kernel's answer,
 * not an unnoticed -EACCES. */
static void user_child(void)
{
    int fd;

    drop_to_1000();

    /* chown(path, -1, -1) needs no right, so it must also take none away.
     * Without the no-change gate this one call disarms /bin/su from uid
     * 1000. */
    want_eq("chown(-1, -1) on a root-owned setuid file",
            syscall3(SYS_CHOWN32, (int)NOCHG, (int)KEEP, (int)KEEP), 0);
    want_mode("a no-change chown disarmed the file", NOCHG, 04755);
    /* The same no-change chown spelled with the ids named, not with the
     * sentinel. A gate written against (uid != -1 || gid != -1) instead of
     * against a field actually moving passes the line above and fails this one,
     * and that kernel is the hole: chown("/bin/su", 0, 0) from uid 1000 needs
     * no right, since nothing changes, and would disarm the file. */
    want_eq("chown(0, 0) on a root-owned setuid file", syscall3(SYS_CHOWN32, (int)NOCHG, 0, 0), 0);
    want_mode("naming the ids it already had disarmed the file", NOCHG, 04755);

    /* A write that stays inside the node the descriptor already has. */
    write_bytes("write in place", INPLACE, 8);
    want_mode("an in-place write kept setuid", INPLACE, 0755);

    /* A write that grows the file past its node's data room. /mp_block sits
     * immediately behind it, so the node cannot extend and must move; the
     * assertion below re-resolves the path, which is the only way to see a
     * clear that was applied through the pointer the write left behind. */
    write_bytes("write past the node", GROW, GROW_BYTES);
    want_mode("a relocating write kept setuid", GROW, 0755);
    want_size("the relocating write lost bytes", GROW, GROW_BYTES);

    fd = syscall3(SYS_OPEN, (int)TRUNCME, O_WRONLY | O_TRUNC, 0);
    want_ok("open with O_TRUNC", fd);
    want_eq("close the truncated file", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_mode("O_TRUNC kept setuid", TRUNCME, 0755);
    want_size("O_TRUNC left bytes behind", TRUNCME, 0);

    /* creat(2) on an existing file is open(O_WRONLY|O_CREAT|O_TRUNC): it
     * truncates, and the clear rides that branch. */
    fd = syscall3(SYS_CREAT, (int)CREATME, 0644, 0);
    want_ok("creat an existing set-id file", fd);
    want_eq("close the recreated file", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_mode("creat kept setuid", CREATME, 0755);
    want_size("creat left bytes behind", CREATME, 0);

    /* Setgid follows group-execute, the bit exec's grant reads. */
    write_bytes("write the 02755 file", SGIDX, 8);
    want_mode("a write kept setgid with group-execute", SGIDX, 0755);
    /* The paired positive for the chmod rule below, without which a blanket
     * strip on every unprivileged chmod would pass: this caller IS in the
     * file's group (both are 1000), so the bit it asks for must land. */
    want_eq("chmod the 02755 file back", syscall3(SYS_CHMOD, (int)SGIDX, 02755, 0), 0);
    want_mode("chmod took setgid from a caller inside the group", SGIDX, 02755);
    write_bytes("write the 02645 file", SGIDNOX, 8);
    want_mode("a write took setgid without group-execute", SGIDNOX, 02645);

    /* The control: a mode with nothing to lose comes back unchanged. */
    write_bytes("write the plain file", PLAIN, 8);
    want_mode("a write disturbed a plain mode", PLAIN, 0755);

    syscall1(SYS_EXIT, 0);
}

/* The escalation the mode alone would grant. /mp_sgid0 is a runnable copy of
 * the helper, 02700 and owned 1000:0, so its executor is its owner and the
 * setgid bit is the only thing that could hand out gid 0. */
static void escalation_child(void)
{
    char byte;
    int fd;

    drop_to_1000();

    /* Rewrite one byte with the value it already has. The clear rule cares
     * that a write moved bytes, not which, and the copy has to stay a valid
     * ELF for the exec below. */
    fd = syscall3(SYS_OPEN, (int)SGID0, O_RDONLY, 0);
    want_ok("open the copy to read a byte", fd);
    want_eq("read a byte of the copy", syscall3(SYS_READ, fd, (int)&byte, 1), 1);
    want_eq("close the copy", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    fd = syscall3(SYS_OPEN, (int)SGID0, O_WRONLY, 0);
    want_ok("reopen the copy to write it back", fd);
    want_eq("rewrite a byte of the copy", syscall3(SYS_WRITE, fd, (int)&byte, 1), 1);
    want_eq("close the rewritten copy", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    want_mode("a write took setgid without group-execute", SGID0, 02700);
    want_eq("exec the 02700 copy", run_helper(SGID0, "plain"), 0);

    /* An owner outside the file's group may not store setgid on it, so the
     * mode that would have granted gid 0 never lands. No write clear can reach
     * this case: the mode needs no write to appear. */
    want_eq("chmod the copy 02710", syscall3(SYS_CHMOD, (int)SGID0, 02710, 0), 0);
    want_mode("chmod stored setgid for a caller outside the group", SGID0, 0710);
    want_eq("exec the 02710 copy", run_helper(SGID0, "plain"), 0);

    syscall1(SYS_EXIT, 0);
}

/* Run one child to completion and require a clean exit 0. WIFEXITED comes
 * first: a signal death has the signal in the low bits and would otherwise
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
         * ever grew a path out the bottom would fail, not pass. */
        syscall1(SYS_EXIT, 127);
    }
    reaped = syscall3(SYS_WAITPID, pid, (int)&status, 0);
    if (reaped != pid) {
        fail(what, reaped);
    }
    if (!WIFEXITED(status)) {
        puts_("MODEPROBE FAIL: ");
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

void _start(void)
{
    int fd;

    /* The relocation pair, first and in this order. `creat` allocates a node
     * with no room for data, so /mp_grow is followed by the remainder of the
     * free block it was cut from; allocation is first-fit from the start of
     * the image and every hole before that block was already too small to hold
     * a node header, so the next `creat` takes that remainder. Either way
     * /mp_grow ends up followed by a reserved node, and a write that outgrows
     * it must relocate, not extend in place. */
    fd = syscall3(SYS_CREAT, (int)GROW, 0644, 0);
    want_ok("mint the relocation fixture", fd);
    want_eq("close the relocation fixture", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    fd = syscall3(SYS_CREAT, (int)BLOCKER, 0644, 0);
    want_ok("mint the blocker behind it", fd);
    want_eq("close the blocker", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("give the relocation fixture away", syscall3(SYS_CHOWN32, (int)GROW, 1000, 1000), 0);
    want_eq("arm the relocation fixture", syscall3(SYS_CHMOD, (int)GROW, 04755, 0), 0);
    want_mode("the relocation fixture did not take its mode", GROW, 04755);

    mint(ROOTSUID, 04755, FILL_BYTES, 0, 0);
    mint(CHOWNED, 04755, FILL_BYTES, 0, 0);
    mint(NOCHG, 04755, FILL_BYTES, 0, 0);
    mint(INPLACE, 04755, FILL_BYTES, 1000, 1000);
    mint(TRUNCME, 04755, FILL_BYTES, 1000, 1000);
    mint(CREATME, 04755, FILL_BYTES, 1000, 1000);
    mint(SGIDX, 02755, FILL_BYTES, 1000, 1000);
    mint(SGIDNOX, 02645, FILL_BYTES, 1000, 1000);
    mint(PLAIN, 0755, FILL_BYTES, 1000, 1000);

    /* A directory is armed after it is made, and chowned after that, so the
     * chown below has a bit to take and visibly does not take it. */
    want_eq("mint the directory", syscall3(SYS_MKDIR, (int)DIRSUID, 0755, 0), 0);
    want_eq("arm the directory", syscall3(SYS_CHMOD, (int)DIRSUID, 04755, 0), 0);
    want_mode("the directory did not take its mode", DIRSUID, 04755);

    /* Root's own half. The write exemption first: a privileged writer keeps the
     * bit, or every root process would disarm the programs it maintains. */
    want_mode("the 04755 baseline", ROOTSUID, 04755);
    write_bytes("write as root", ROOTSUID, 8);
    want_mode("a privileged write took the bit", ROOTSUID, 04755);

    /* The chown has no such exemption, and the same identity asks for it. That
     * pair is the whole difference between the two rules. */
    want_eq("chown a setuid file away", syscall3(SYS_CHOWN32, (int)CHOWNED, 1000, 1000), 0);
    want_mode("a chown kept setuid", CHOWNED, 0755);
    want_eq("chown the directory away", syscall3(SYS_CHOWN32, (int)DIRSUID, 1000, 1000), 0);
    want_mode("a chown took a directory's setuid", DIRSUID, 04755);

    run_child(user_child, "the uid-1000 child");

    /* The escalation's fixture. The chmod comes before the chown on purpose:
     * 02700 has no group-execute, so the clear the chown runs must spare
     * its setgid bit, and the assertion after it says so. */
    copy_file(HELPER, SGID0);
    want_eq("arm the helper copy", syscall3(SYS_CHMOD, (int)SGID0, 02700, 0), 0);
    want_eq("give the helper copy away", syscall3(SYS_CHOWN32, (int)SGID0, 1000, 0), 0);
    want_mode("the chown took setgid without group-execute", SGID0, 02700);

    run_child(escalation_child, "the escalation child");

    /* Hand the image back the way the probe found it. */
    want_eq("remove the baseline file", syscall3(SYS_UNLINK, (int)ROOTSUID, 0, 0), 0);
    want_eq("remove the chowned file", syscall3(SYS_UNLINK, (int)CHOWNED, 0, 0), 0);
    want_eq("remove the no-change file", syscall3(SYS_UNLINK, (int)NOCHG, 0, 0), 0);
    want_eq("remove the in-place file", syscall3(SYS_UNLINK, (int)INPLACE, 0, 0), 0);
    want_eq("remove the relocation fixture", syscall3(SYS_UNLINK, (int)GROW, 0, 0), 0);
    want_eq("remove the blocker", syscall3(SYS_UNLINK, (int)BLOCKER, 0, 0), 0);
    want_eq("remove the truncated file", syscall3(SYS_UNLINK, (int)TRUNCME, 0, 0), 0);
    want_eq("remove the recreated file", syscall3(SYS_UNLINK, (int)CREATME, 0, 0), 0);
    want_eq("remove the 02755 file", syscall3(SYS_UNLINK, (int)SGIDX, 0, 0), 0);
    want_eq("remove the 02645 file", syscall3(SYS_UNLINK, (int)SGIDNOX, 0, 0), 0);
    want_eq("remove the plain file", syscall3(SYS_UNLINK, (int)PLAIN, 0, 0), 0);
    want_eq("remove the helper copy", syscall3(SYS_UNLINK, (int)SGID0, 0, 0), 0);
    want_eq("remove the directory", syscall3(SYS_RMDIR, (int)DIRSUID, 0, 0), 0);

    puts_("MODEPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
