/**
 * Verify the sticky bit: in a directory that has it, write permission is no
 * longer enough to take another user's entry out.
 *
 * The rule under test: with the bit set on a directory, an entry may be removed
 * or renamed only by the entry's owner, by the directory's owner, or by a
 * privileged caller, and the refusal is -EPERM. Four call sites answer it,
 * unlink, rmdir, and both ends of rename, and the destination end is the one
 * whose omission leaves the attack open while a probe that only tries unlink
 * passes.
 *
 * Every denial is paired against the same call in a non-sticky control
 * directory, which is what makes it mean anything: a lone -EPERM could be
 * coming from ownership or from a missing write bit, not from the sticky
 * bit. `/st_plain` has the same fixtures at the same modes and the same
 * owners as `/st_sticky` and differs from it in exactly one bit, so the four
 * denials and the four permissions below are the same four calls asked twice.
 *
 * The victims are 0666, so the caller has write permission on every file it
 * is refused. That is deliberate: the SVR4 relaxation, which also permits the
 * removal when the caller can write the file, would let all four denials
 * through, and this kernel does not adopt it.
 *
 * The two ends of rename are told apart by giving the caller a rename it may
 * perform. `/st_sticky/myrn` is its own file and renaming it inside the sticky
 * directory succeeds, so when the rename of its own `/st_sticky/mysrc` over
 * root's `/st_sticky/target` is refused, the source end has already been shown
 * to permit it and the entry being replaced is the only thing left that can
 * refuse. Without that case a kernel that denied every rename in a sticky
 * directory would pass the destination case for the wrong reason.
 *
 * The directory-owner clause runs in `/st_owned`, a 01777 directory owned by
 * uid 1000: the same caller that could not take root's entry out of
 * `/st_sticky` takes root's entry out of this one, because it owns the
 * directory. A third uid is then refused in both sticky directories, which is
 * what keeps that clause about ownership, not about the second directory
 * being the more permissive one; it creates and removes a file of its own in
 * each first, so its refusal cannot be a missing write bit either, and that
 * pair of removals is the entry-owner clause under a uid that owns neither
 * directory.
 *
 * `/st_locked` is 01755 and root's: a caller with no write permission there
 * must see -EACCES and not -EPERM. The sticky rule is asked only after the
 * directory's write check, so the bit never turns an access failure into a
 * permission one, and this is the case that checks that order.
 *
 * The root parent's own cleanup is the privileged clause: it removes from
 * `/st_sticky` every entry the two children were refused.
 *
 * The fixtures split two ways. Only `/st_sticky` and `/st_plain` are staged, by
 * `krffs/manifests/test.manifest`, and only because the manifest's `dir` verb
 * is how the shipped `1777 /tmp` gets its bit and nothing else in the tree
 * reads that bit back: the probe asserts both staged modes before it touches
 * anything, so a staging tool that dropped 01000 fails here, not quietly
 * shipping an inert /tmp. Everything else the root parent mints and removes,
 * the other two sticky directories included, since chmod and chown can make
 * those directly and a probe that leaves nothing behind rests on no other
 * probe's file and can be run again in the same boot.
 *
 * Exact errnos throughout, never a sign. The KRFFS core answers every refusal
 * of an unlink or a rename with a bare -1, and the system calls tell the cases
 * apart before they call it (syscall_fs.c), so a refusal and a missing file
 * answer differently and -EPERM is the sticky rule's answer.
 *
 * An argument `ids=OLD:NEW,...` gives the ids that stand for 1000 and 1001
 * in a native run (probe_args.h), which runs the probe in a private copy of
 * the test image's root; without it the ids are 1000 and 1001.
 *
 * Each denial is checked to have done nothing and each permission to have
 * been carried out, since an errno alone cannot tell a refusal from a
 * refusal reported after the work was done. The destination victims are
 * checked by mode, not by existence, because a rename that clobbered
 * one would leave the name in place carrying the source's 0640. Every wait
 * checks WIFEXITED before WEXITSTATUS: a signal death has the signal in
 * the low bits and would otherwise decode as exit 0. Prints "STICKYPROBE OK"
 * and exits 0 only when every case passed; any failure names the step and
 * the answer it got, and exits 1.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each check `checks` lists. A child cannot write
 * them, since only the process that began the report may, so it gives the
 * check it failed to the parent on a pipe; a failure leaves the checks after
 * it not run. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

typedef unsigned int u32;

/* The id a native run gives in place of `id` (probe_args.h). */
#define ID(id) ((u32)probe_id(id))

#define STDOUT 1

#define EPERM  1
#define ENOENT 2
#define EACCES 13

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death has the signal in the low 7 bits. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

/* The pair, staged by krffs/manifests/test.manifest: one bit apart. */
#define STICKY "/st_sticky"
#define PLAIN  "/st_plain"

/* The two the root parent mints: a sticky directory owned by uid 1000, for the
 * directory-owner clause, and a sticky directory nobody but root may write, for
 * the order of the write check and the sticky check. */
#define OWNED  "/st_owned"
#define LOCKED "/st_locked"

/* Inside the sticky directory. The root-owned entries are 0666, so the caller
 * holds write on each; `mine`, `myrn` and `mysrc` are uid 1000's own. */
#define S_VICTIM "/st_sticky/victim"
#define S_MOVER  "/st_sticky/mover"
#define S_MOVED  "/st_sticky/moved"
#define S_RMME   "/st_sticky/rmme"
#define S_MINE   "/st_sticky/mine"
#define S_MYRN   "/st_sticky/myrn"
#define S_MYRN2  "/st_sticky/renamed"
#define S_MYSRC  "/st_sticky/mysrc"
#define S_TARGET "/st_sticky/target"
#define S_OTHER  "/st_sticky/other"
#define S_OTHOWN "/st_sticky/otherown"

/* The same five fixtures in the non-sticky control, at the same modes and
 * the same owners: the four calls the sticky directory refuses must all work
 * here. */
#define P_VICTIM "/st_plain/victim"
#define P_MOVER  "/st_plain/mover"
#define P_MOVED  "/st_plain/moved"
#define P_RMME   "/st_plain/rmme"
#define P_MYSRC  "/st_plain/mysrc"
#define P_TARGET "/st_plain/target"

/* The directory-owner clause's victim, and the third uid's pair. */
#define OWN_VICTIM "/st_owned/victim"
#define OWN_OTHER  "/st_owned/other"
#define OWN_OTHOWN "/st_owned/otherown"

/* The victim behind a directory that grants nobody but root write. */
#define LOCK_VICTIM "/st_locked/victim"

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output. */
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

/** Print an unsigned octal with a leading zero: the only readable shape for
 * a mode. */
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

/* The checks, one assertion each, in the order the probe makes them. A row
 * whose step is NULL stands for several steps: `fixtures` for those of the
 * helpers that make the fixtures, the two drops for "drop the gids" and "drop
 * the uids" in each child, and `root-cleanup` for every "root removes". */
enum {
    STAGED_STICKY,
    STAGED_PLAIN,
    FIXTURES,
    DROP_1000,
    USER_CHILD = 16,
    DROP_1001 = 34,
    OTHER_CHILD = 39,
    ROOT_CLEANUP = 44,
    CHECKS
};

static const struct {
    const char *id;
    const char *step;
} checks[CHECKS] = {
    {"staged-sticky", "staged /st_sticky"},
    {"staged-plain", "staged /st_plain"},
    {"fixtures", 0},
    {"drop-1000", 0},
    {"sticky-unlink", "sticky: unlink root's file"},
    {"plain-unlink", "plain: unlink root's file"},
    {"sticky-rename", "sticky: rename root's file"},
    {"plain-rename", "plain: rename root's file"},
    {"sticky-rmdir", "sticky: rmdir root's directory"},
    {"plain-rmdir", "plain: rmdir root's directory"},
    {"sticky-unlink-own", "sticky: unlink its own file"},
    {"sticky-rename-own", "sticky: rename its own file"},
    {"sticky-rename-over", "sticky: rename its own file over root's"},
    {"plain-rename-over", "plain: rename its own file over root's"},
    {"owned-unlink", "owned: unlink root's file"},
    {"locked-unlink", "locked: unlink root's file"},
    {"user-child", "the uid-1000 child"},
    {"sticky-victim-kept", "sticky: the victim is still there"},
    {"sticky-mover-kept", "sticky: the mover is still there"},
    {"sticky-moved-absent", "sticky: the rename's destination"},
    {"sticky-rmdir-kept", "sticky: the rmdir target is still there"},
    {"sticky-source-kept", "sticky: the destination case's source"},
    {"sticky-target-kept", "sticky: the destination case's victim"},
    {"locked-victim-kept", "locked: the victim is still there"},
    {"plain-victim-gone", "plain: the victim is gone"},
    {"plain-mover-gone", "plain: the mover is gone"},
    {"plain-moved-there", "plain: the rename's destination"},
    {"plain-rmdir-gone", "plain: the rmdir target is gone"},
    {"sticky-own-gone", "sticky: the entry owner's file is gone"},
    {"sticky-own-source-gone", "sticky: the entry owner's rename source is gone"},
    {"sticky-own-renamed", "sticky: the entry owner's rename destination"},
    {"plain-source-gone", "plain: the destination case's source is gone"},
    {"plain-target-replaced", "plain: the destination case's victim was replaced"},
    {"owned-victim-gone", "owned: the directory owner's victim is gone"},
    {"drop-1001", 0},
    {"sticky-third-own", "sticky: a third uid's own file"},
    {"owned-third-own", "owned: a third uid's own file"},
    {"sticky-third-unlink", "sticky: a third uid unlinks root's file"},
    {"owned-third-unlink", "owned: a third uid unlinks root's file"},
    {"other-child", "the uid-1001 child"},
    {"sticky-third-kept", "sticky: the third uid's victim is still there"},
    {"owned-third-kept", "owned: the third uid's victim is still there"},
    {"sticky-third-gone", "sticky: the third uid's own file is gone"},
    {"owned-third-gone", "owned: the third uid's own file is gone"},
    {"root-cleanup", 0},
};

/* The drop row of the child running, which its fork inherits, and the write
 * end of the pipe on which a child gives its parent the check it failed, -1
 * in the parent. */
static int child_drop = DROP_1000;
static int failed_fd = -1;

/** Whether the string `s` begins with `prefix`. */
static int begins(const char *s, const char *prefix)
{
    while (*prefix != '\0' && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

/** The check whose step is `step`. */
static int check_of(const char *step)
{
    int i;

    if (begins(step, "drop the ")) {
        return child_drop;
    }
    if (begins(step, "root removes ")) {
        return ROOT_CLEANUP;
    }
    for (i = 0; i < CHECKS; i++) {
        if (checks[i].step != 0 && begins(step, checks[i].step) && begins(checks[i].step, step)) {
            return i;
        }
    }
    return FIXTURES;
}

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with `why`, those after it as not run, and the
 * report's end; with `check` CHECKS, every check held. */
static void report_at(int check, const char *why)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, why);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Fail the check of `step`: in the parent report it, and in a child give
 * its number to the parent on `failed_fd`. */
static void report_step(const char *step)
{
    char check = (char)check_of(step);

    if (failed_fd >= 0) {
        syscall3(SYS_WRITE, failed_fd, (long)&check, 1);
    }
    report_at(check, step);
}

/** Print "STICKYPROBE FAIL: ", the step and the answer it got, fail the
 * step's check, and exit 1. */
static void fail(const char *step, int got)
{
    puts_("STICKYPROBE FAIL: ");
    puts_(step);
    puts_(" (got ");
    putd(got);
    puts_(")\n");
    report_step(step);
    syscall1(SYS_EXIT, 1);
}

/** Fail at `step` unless the answer `got` is exactly `want`: an errno, not
 * merely a sign. */
static void want_eq(const char *step, int got, int want)
{
    if (got != want) {
        fail(step, got);
    }
}

/* The one stat buffer, of the larger struct of the two architectures, x86-64's
 * (probe.h): the kernel writes it, the readers below take it apart. */
static char statbuf[144];

/** stat(2) of `path` into `statbuf`: 0, or a negated errno. */
static int stat_rc(const char *path)
{
    return syscall3(SYS_STAT, (long)path, (long)statbuf, 0);
}

/** Fail at `step` unless the name `path` is still there: the check a
 * refused call's errno cannot make. */
static void want_there(const char *step, const char *path)
{
    want_eq(step, stat_rc(path), 0);
}

/** Fail at `step` unless the name `path` is gone: what a permitted removal
 * leaves behind. */
static void want_absent(const char *step, const char *path)
{
    want_eq(step, stat_rc(path), -ENOENT);
}

/** Fail at `step` unless the permission bits of `path` read back as `mode`.
 * A rename that clobbered a victim would leave the name in place under the
 * source's mode, which existence alone cannot see. */
static void want_mode(const char *step, const char *path, u32 mode)
{
    u32 got;
    int rc = stat_rc(path);

    if (rc != 0) {
        fail(step, rc);
    }
    got = *(u32 *)(statbuf + KSTAT_MODE) & 07777u;
    if (got != mode) {
        puts_("STICKYPROBE FAIL: ");
        puts_(step);
        puts_(" (mode ");
        puto(got);
        puts_(" not ");
        puto(mode);
        puts_(")\n");
        report_step(step);
        syscall1(SYS_EXIT, 1);
    }
}

/** Create `path` with `mode`, set again by chmod so that the boot shell's
 * umask leaves no mark on it. */
static void mint_file(const char *path, u32 mode)
{
    int fd = syscall3(SYS_CREAT, (long)path, (int)mode, 0);

    if (fd < 0) {
        fail("mint a fixture file", fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq("chmod a fixture file", syscall3(SYS_CHMOD, (long)path, (int)mode, 0), 0);
}

/** Make the directory `path` with `mode`, set again by chmod. */
static void mint_dir(const char *path, u32 mode)
{
    want_eq("mint a fixture directory", syscall3(SYS_MKDIR, (long)path, (int)mode, 0), 0);
    want_eq("chmod a fixture directory", syscall3(SYS_CHMOD, (long)path, (int)mode, 0), 0);
}

/** Hand a fixture to `uid`, which only the root parent may do. */
static void give_to(const char *path, int uid)
{
    want_eq("chown a fixture", syscall3(SYS_CHOWN, (long)path, uid, uid), 0);
}

/** Become `uid` outright, with no saved id to return to. The gids go first:
 * once the effective uid is not 0 the gid calls are refused. */
static void drop_to(int uid)
{
    want_eq("drop the gids", syscall3(SYS_SETRESGID, uid, uid, uid), 0);
    want_eq("drop the uids", syscall3(SYS_SETRESUID, uid, uid, uid), 0);
}

/** Create a file of the caller's own at `path` and take it away again. In a
 * sticky directory that pair proves two things at once: the caller has write
 * permission there, so nothing below can be refused for want of it, and the
 * entry-owner clause lets a caller remove what it owns. */
static void own_round_trip(const char *step, const char *path)
{
    int fd = syscall3(SYS_CREAT, (long)path, 0644, 0);

    if (fd < 0) {
        fail(step, fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq(step, syscall3(SYS_UNLINK, (long)path, 0, 0), 0);
}

/** Uid 1000: the four denials in the sticky directory, the same four calls
 * permitted in the non-sticky one, and the two clauses that let this caller
 * through. Exits 0, or 1 at the first case that fails. */
static void user_child(void)
{
    drop_to(ID(1000));

    want_eq("sticky: unlink root's file", syscall3(SYS_UNLINK, (long)S_VICTIM, 0, 0), -EPERM);
    want_eq("plain: unlink root's file", syscall3(SYS_UNLINK, (long)P_VICTIM, 0, 0), 0);

    want_eq("sticky: rename root's file", syscall3(SYS_RENAME, (long)S_MOVER, (long)S_MOVED, 0),
            -EPERM);
    want_eq("plain: rename root's file", syscall3(SYS_RENAME, (long)P_MOVER, (long)P_MOVED, 0), 0);

    want_eq("sticky: rmdir root's directory", syscall3(SYS_RMDIR, (long)S_RMME, 0, 0), -EPERM);
    want_eq("plain: rmdir root's directory", syscall3(SYS_RMDIR, (long)P_RMME, 0, 0), 0);

    want_eq("sticky: unlink its own file", syscall3(SYS_UNLINK, (long)S_MINE, 0, 0), 0);
    want_eq("sticky: rename its own file", syscall3(SYS_RENAME, (long)S_MYRN, (long)S_MYRN2, 0), 0);

    want_eq("sticky: rename its own file over root's",
            syscall3(SYS_RENAME, (long)S_MYSRC, (long)S_TARGET, 0), -EPERM);
    want_eq("plain: rename its own file over root's",
            syscall3(SYS_RENAME, (long)P_MYSRC, (long)P_TARGET, 0), 0);

    want_eq("owned: unlink root's file", syscall3(SYS_UNLINK, (long)OWN_VICTIM, 0, 0), 0);

    want_eq("locked: unlink root's file", syscall3(SYS_UNLINK, (long)LOCK_VICTIM, 0, 0), -EACCES);

    syscall1(SYS_EXIT, 0);
}

/** Uid 1001: neither directory's owner and neither entry's owner, so both
 * sticky directories refuse it, the one uid 1000 owns included. Exits 0, or 1
 * at the first case that fails. */
static void other_child(void)
{
    drop_to(ID(1001));

    own_round_trip("sticky: a third uid's own file", S_OTHOWN);
    own_round_trip("owned: a third uid's own file", OWN_OTHOWN);

    want_eq("sticky: a third uid unlinks root's file", syscall3(SYS_UNLINK, (long)S_OTHER, 0, 0),
            -EPERM);
    want_eq("owned: a third uid unlinks root's file", syscall3(SYS_UNLINK, (long)OWN_OTHER, 0, 0),
            -EPERM);

    syscall1(SYS_EXIT, 0);
}

/** Report the checks of a child that failed: the one it gave on the pipe
 * `rd`, or else its own row `what`, since a child that died or returned gave
 * none. */
static void report_child(int rd, const char *what)
{
    char check = 0;

    if (syscall3(SYS_READ, rd, (long)&check, 1) == 1 && check > child_drop - 1 && check < CHECKS) {
        report_at(check, checks[(int)check].step != 0 ? checks[(int)check].step
                                                      : "the drop to the child's ids");
    }
    report_step(what);
}

/** Run one child to completion and require a clean exit 0. WIFEXITED comes
 * first: a signal death has the signal in the low bits and would otherwise
 * decode as exit 0. The child has already named the case it failed, and
 * gives its check to the parent on a pipe; `drop` is its drop row. */
static void run_child(void (*body)(void), const char *what, int drop)
{
    int pid, status = 0;
    int reaped;
    int pipefd[2];

    child_drop = drop;
    if (syscall1(SYS_PIPE, (long)pipefd) != 0) {
        pipefd[0] = pipefd[1] = -1;
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        fail(what, pid);
    }
    if (pid == 0) {
        failed_fd = pipefd[1];
        body();
        /* Every body exits itself; 127 fails one that returns. */
        syscall1(SYS_EXIT, 127);
    }
    syscall1(SYS_CLOSE, pipefd[1]);
    reaped = syscall3(SYS_WAITPID, pid, (long)&status, 0);
    if (reaped != pid) {
        fail(what, reaped);
    }
    if (status != 0) {
        report_child(pipefd[0], what);
    }
    syscall1(SYS_CLOSE, pipefd[0]);
    if (!WIFEXITED(status)) {
        puts_("STICKYPROBE FAIL: ");
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

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Check the two staged modes, make the fixtures, run the two children, and
 * require each refusal to have done nothing and each permitted call to have
 * been carried out; then remove, as root, every entry the children were
 * refused. The root-owned fixtures are 0666, so uid 1000 has write on each
 * file it is refused. Each case has a fixture of its own, so that no later
 * "the name is gone" can rest on an earlier case having taken it.
 */
void probe_main(unsigned long *sp)
{
    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.stickyprobe"));
    want_mode("staged /st_sticky", STICKY, 01777);
    want_mode("staged /st_plain", PLAIN, 0777);

    mint_file(S_VICTIM, 0666);
    mint_file(S_MOVER, 0666);
    mint_file(S_TARGET, 0666);
    mint_file(S_OTHER, 0666);
    mint_dir(S_RMME, 0755);
    mint_file(S_MINE, 0644);
    give_to(S_MINE, ID(1000));
    mint_file(S_MYRN, 0644);
    give_to(S_MYRN, ID(1000));
    mint_file(S_MYSRC, 0640);
    give_to(S_MYSRC, ID(1000));

    mint_file(P_VICTIM, 0666);
    mint_file(P_MOVER, 0666);
    mint_file(P_TARGET, 0666);
    mint_dir(P_RMME, 0755);
    mint_file(P_MYSRC, 0640);
    give_to(P_MYSRC, ID(1000));

    mint_dir(OWNED, 01777);
    give_to(OWNED, ID(1000));
    want_mode("the minted /st_owned", OWNED, 01777);
    mint_file(OWN_VICTIM, 0666);
    mint_file(OWN_OTHER, 0666);

    mint_dir(LOCKED, 01755);
    mint_file(LOCK_VICTIM, 0666);

    run_child(user_child, "the uid-1000 child", DROP_1000);

    want_there("sticky: the victim is still there", S_VICTIM);
    want_there("sticky: the mover is still there", S_MOVER);
    want_absent("sticky: the rename's destination", S_MOVED);
    want_there("sticky: the rmdir target is still there", S_RMME);
    want_there("sticky: the destination case's source", S_MYSRC);
    want_mode("sticky: the destination case's victim", S_TARGET, 0666);
    want_there("locked: the victim is still there", LOCK_VICTIM);

    want_absent("plain: the victim is gone", P_VICTIM);
    want_absent("plain: the mover is gone", P_MOVER);
    want_there("plain: the rename's destination", P_MOVED);
    want_absent("plain: the rmdir target is gone", P_RMME);
    want_absent("sticky: the entry owner's file is gone", S_MINE);
    want_absent("sticky: the entry owner's rename source is gone", S_MYRN);
    want_there("sticky: the entry owner's rename destination", S_MYRN2);
    want_absent("plain: the destination case's source is gone", P_MYSRC);
    want_mode("plain: the destination case's victim was replaced", P_TARGET, 0640);
    want_absent("owned: the directory owner's victim is gone", OWN_VICTIM);

    run_child(other_child, "the uid-1001 child", DROP_1001);

    want_there("sticky: the third uid's victim is still there", S_OTHER);
    want_there("owned: the third uid's victim is still there", OWN_OTHER);
    want_absent("sticky: the third uid's own file is gone", S_OTHOWN);
    want_absent("owned: the third uid's own file is gone", OWN_OTHOWN);

    want_eq("root removes the victim", syscall3(SYS_UNLINK, (long)S_VICTIM, 0, 0), 0);
    want_eq("root removes the mover", syscall3(SYS_UNLINK, (long)S_MOVER, 0, 0), 0);
    want_eq("root removes the third uid's victim", syscall3(SYS_UNLINK, (long)S_OTHER, 0, 0), 0);
    want_eq("root removes the rmdir target", syscall3(SYS_RMDIR, (long)S_RMME, 0, 0), 0);
    want_eq("root removes the renamed file", syscall3(SYS_UNLINK, (long)S_MYRN2, 0, 0), 0);
    want_eq("root removes the destination case's source", syscall3(SYS_UNLINK, (long)S_MYSRC, 0, 0),
            0);
    want_eq("root removes the destination case's victim",
            syscall3(SYS_UNLINK, (long)S_TARGET, 0, 0), 0);
    want_eq("root removes the plain rename's destination",
            syscall3(SYS_UNLINK, (long)P_MOVED, 0, 0), 0);
    want_eq("root removes the plain destination case's victim",
            syscall3(SYS_UNLINK, (long)P_TARGET, 0, 0), 0);
    want_eq("root removes the third uid's owned victim",
            syscall3(SYS_UNLINK, (long)OWN_OTHER, 0, 0), 0);
    want_eq("root removes the owned directory", syscall3(SYS_RMDIR, (long)OWNED, 0, 0), 0);
    want_eq("root removes the locked victim", syscall3(SYS_UNLINK, (long)LOCK_VICTIM, 0, 0), 0);
    want_eq("root removes the locked directory", syscall3(SYS_RMDIR, (long)LOCKED, 0, 0), 0);

    report_at(CHECKS, 0);
    puts_("STICKYPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
