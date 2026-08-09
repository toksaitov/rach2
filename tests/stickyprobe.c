/**
 * Verify the sticky bit: in a directory that carries it, write permission is
 * no longer enough to take another user's entry out.
 *
 * The rule under test: with the bit set on a directory, an entry may be removed
 * or renamed only by the entry's owner, by the directory's owner, or by a
 * privileged caller, and the refusal is -EPERM. Four call sites answer it,
 * unlink, rmdir, and both ends of rename, and the destination end is the one
 * whose omission leaves the attack open while a probe that only tries unlink
 * goes green.
 *
 * Every denial is paired against the same call in a non-sticky control
 * directory, which is what makes it mean anything: a lone -EPERM could be
 * coming from ownership or from a missing write bit rather than from the sticky
 * bit. `/st_plain` holds the same fixtures at the same modes and the same
 * owners as `/st_sticky` and differs from it in exactly one bit, so the four
 * denials and the four permissions below are the same four calls asked twice.
 *
 * The victims are 0666, so the caller holds write permission on every file it
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
 * what keeps that clause about ownership rather than about the second directory
 * being the more permissive one; it creates and removes a file of its own in
 * each first, so its refusal cannot be a missing write bit either, and that
 * pair of removals is the entry-owner clause under a uid that owns neither
 * directory.
 *
 * `/st_locked` is 01755 and root's: a caller with no write permission there
 * must see -EACCES and not -EPERM. The sticky rule is asked only after the
 * directory's write check, so the bit never turns an access failure into a
 * permission one, and this is the case that pins that order.
 *
 * The root parent's own cleanup is the privileged clause: it removes from
 * `/st_sticky` every entry the two children were refused.
 *
 * The fixtures split two ways. Only `/st_sticky` and `/st_plain` are staged, by
 * `krffs/manifests/test.manifest`, and only because the manifest's `dir` verb is
 * how the shipped `1777 /tmp` gets its bit and nothing else in the tree reads
 * that bit back: the probe asserts both staged modes before it touches
 * anything, so a staging tool that dropped 01000 fails here rather than quietly
 * shipping an inert /tmp. Everything else the root parent mints and removes,
 * the other two sticky directories included, since chmod and chown make those
 * honestly and a probe that leaves nothing behind rests on no other probe's file
 * and can be run again in the same boot.
 *
 * Exact errnos throughout, never a sign. That rests on the errno translation at
 * unlink and rename: before it both returned a bare -1, which userspace reads
 * as EPERM, so a refusal and a missing file answered alike and -EPERM proved
 * nothing.
 *
 * Each denial is checked to have done nothing and each permission to have been
 * carried out, since an errno alone cannot tell a refusal from a refusal
 * reported after the work was done. The destination victims are checked by mode
 * rather than by existence, because a rename that clobbered one would leave the
 * name in place carrying the source's 0640. Every wait checks WIFEXITED before
 * WEXITSTATUS: a signal death carries the signal in the low bits and would
 * otherwise decode as exit 0. Prints "STICKYPROBE OK" and exits 0 only when
 * every case passed; any failure names the step and the answer it got, and
 * exits 1. Freestanding; direct int 0x80.
 */

typedef unsigned int u32;

#define SYS_EXIT        1
#define SYS_FORK        2
#define SYS_WRITE       4
#define SYS_CLOSE       6
#define SYS_WAITPID     7
#define SYS_CREAT       8
#define SYS_UNLINK      10
#define SYS_CHMOD       15
#define SYS_RENAME      38
#define SYS_MKDIR       39
#define SYS_RMDIR       40
#define SYS_STAT64      195
#define SYS_SETRESUID32 208
#define SYS_SETRESGID32 210
#define SYS_CHOWN32     212

#define STDOUT 1

#define EPERM  1
#define ENOENT 2
#define EACCES 13

/* i386 struct stat64: the 32-bit mode field is at offset 16. */
#define ST_MODE 16

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death carries the signal in the low 7 bits. */
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

/* The same five fixtures in the non-sticky control, at the same modes and the
 * same owners: the four calls the sticky directory refuses must all work here. */
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
    puts_("STICKYPROBE FAIL: ");
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

/* The one stat64 buffer: the kernel writes it, the readers below take it apart. */
static char statbuf[144];

static int stat_rc(const char *path)
{
    return syscall3(SYS_STAT64, (int)path, (int)statbuf, 0);
}

/* The name must still be there: the check a refused call's errno cannot make. */
static void want_there(const char *step, const char *path)
{
    want_eq(step, stat_rc(path), 0);
}

/* The name must be gone: what a permitted removal leaves behind. */
static void want_absent(const char *step, const char *path)
{
    want_eq(step, stat_rc(path), -ENOENT);
}

/* The permission bits of `path`, and the mode it must read back at. A rename
 * that clobbered a victim would leave the name in place under the source's
 * mode, which existence alone cannot see. */
static void want_mode(const char *step, const char *path, u32 mode)
{
    u32 got;
    int rc = stat_rc(path);

    if (rc != 0) {
        fail(step, rc);
    }
    got = *(u32 *)(statbuf + ST_MODE) & 07777u;
    if (got != mode) {
        puts_("STICKYPROBE FAIL: ");
        puts_(step);
        puts_(" (mode ");
        puto(got);
        puts_(" not ");
        puto(mode);
        puts_(")\n");
        syscall1(SYS_EXIT, 1);
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

/* Hand a fixture to `uid`, which only the root parent may do. */
static void give_to(const char *path, int uid)
{
    want_eq("chown a fixture", syscall3(SYS_CHOWN32, (int)path, uid, uid), 0);
}

/* Become `uid` outright, with no saved id to swing back on. The gids go first:
 * once the effective uid is not 0 the gid calls are refused. */
static void drop_to(int uid)
{
    want_eq("drop the gids", syscall3(SYS_SETRESGID32, uid, uid, uid), 0);
    want_eq("drop the uids", syscall3(SYS_SETRESUID32, uid, uid, uid), 0);
}

/* Create a file of the caller's own at `path` and take it away again. In a
 * sticky directory that pair proves two things at once: the caller holds write
 * permission there, so nothing below can be refused for want of it, and the
 * entry-owner clause lets a caller remove what it owns. */
static void own_round_trip(const char *step, const char *path)
{
    int fd = syscall3(SYS_CREAT, (int)path, 0644, 0);

    if (fd < 0) {
        fail(step, fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    want_eq(step, syscall3(SYS_UNLINK, (int)path, 0, 0), 0);
}

/* Uid 1000: the four denials in the sticky directory, the same four calls
 * permitted in the non-sticky one, and the two clauses that let this caller
 * through. */
static void user_child(void)
{
    drop_to(1000);

    /* The pair: one file, one mode, one owner, two directories, one bit. */
    want_eq("sticky: unlink root's file", syscall3(SYS_UNLINK, (int)S_VICTIM, 0, 0), -EPERM);
    want_eq("plain: unlink root's file", syscall3(SYS_UNLINK, (int)P_VICTIM, 0, 0), 0);

    /* Renaming an entry within the directory takes it out of that directory,
     * whatever it is called. */
    want_eq("sticky: rename root's file", syscall3(SYS_RENAME, (int)S_MOVER, (int)S_MOVED, 0),
            -EPERM);
    want_eq("plain: rename root's file", syscall3(SYS_RENAME, (int)P_MOVER, (int)P_MOVED, 0), 0);

    /* Both directories are empty, so a permitted rmdir has nothing else to
     * refuse it. */
    want_eq("sticky: rmdir root's directory", syscall3(SYS_RMDIR, (int)S_RMME, 0, 0), -EPERM);
    want_eq("plain: rmdir root's directory", syscall3(SYS_RMDIR, (int)P_RMME, 0, 0), 0);

    /* The entry-owner clause, and with it the source end of rename: this
     * caller's own entry leaves the sticky directory both ways. */
    want_eq("sticky: unlink its own file", syscall3(SYS_UNLINK, (int)S_MINE, 0, 0), 0);
    want_eq("sticky: rename its own file", syscall3(SYS_RENAME, (int)S_MYRN, (int)S_MYRN2, 0), 0);

    /* The destination end. The source is this caller's own and the line above
     * has just shown the source end permits it, so the refusal can only be the
     * root-owned entry the rename would replace. */
    want_eq("sticky: rename its own file over root's",
            syscall3(SYS_RENAME, (int)S_MYSRC, (int)S_TARGET, 0), -EPERM);
    want_eq("plain: rename its own file over root's",
            syscall3(SYS_RENAME, (int)P_MYSRC, (int)P_TARGET, 0), 0);

    /* The directory-owner clause: the same caller, a root-owned entry it does
     * not own, and a sticky directory it does. */
    want_eq("owned: unlink root's file", syscall3(SYS_UNLINK, (int)OWN_VICTIM, 0, 0), 0);

    /* The order of the two checks: no write permission on the directory is
     * -EACCES, and the sticky rule never gets to speak. */
    want_eq("locked: unlink root's file", syscall3(SYS_UNLINK, (int)LOCK_VICTIM, 0, 0), -EACCES);

    syscall1(SYS_EXIT, 0);
}

/* Uid 1001: neither directory's owner and neither entry's owner, so both sticky
 * directories refuse it, the one uid 1000 owns included. */
static void other_child(void)
{
    drop_to(1001);

    own_round_trip("sticky: a third uid's own file", S_OTHOWN);
    own_round_trip("owned: a third uid's own file", OWN_OTHOWN);

    want_eq("sticky: a third uid unlinks root's file", syscall3(SYS_UNLINK, (int)S_OTHER, 0, 0),
            -EPERM);
    want_eq("owned: a third uid unlinks root's file", syscall3(SYS_UNLINK, (int)OWN_OTHER, 0, 0),
            -EPERM);

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

void _start(void)
{
    /* What the manifest staged, before anything else is asked: the whole point
     * of staging these two is that `dir 1777` must store the bit the shipped
     * /tmp is written with. */
    want_mode("staged /st_sticky", STICKY, 01777);
    want_mode("staged /st_plain", PLAIN, 0777);

    /* The sticky directory's fixtures. The root-owned ones are 0666, so uid
     * 1000 holds write on the file it is refused and the SVR4 relaxation this
     * kernel does not adopt would let every refusal through. Each case is given
     * a fixture of its own, so that no later "the name is gone" can rest on an
     * earlier case having taken it. */
    mint_file(S_VICTIM, 0666);
    mint_file(S_MOVER, 0666);
    mint_file(S_TARGET, 0666);
    mint_file(S_OTHER, 0666);
    mint_dir(S_RMME, 0755);
    mint_file(S_MINE, 0644);
    give_to(S_MINE, 1000);
    mint_file(S_MYRN, 0644);
    give_to(S_MYRN, 1000);
    /* 0640, not 0644: after a permitted rename over a 0666 victim the mode is
     * what says which of the two files the surviving name now is. */
    mint_file(S_MYSRC, 0640);
    give_to(S_MYSRC, 1000);

    /* The control, staged the same way in a directory that differs by one bit. */
    mint_file(P_VICTIM, 0666);
    mint_file(P_MOVER, 0666);
    mint_file(P_TARGET, 0666);
    mint_dir(P_RMME, 0755);
    mint_file(P_MYSRC, 0640);
    give_to(P_MYSRC, 1000);

    /* The directory-owner clause needs a sticky directory owned by uid 1000,
     * holding entries it does not own. */
    mint_dir(OWNED, 01777);
    give_to(OWNED, 1000);
    want_mode("the minted /st_owned", OWNED, 01777);
    mint_file(OWN_VICTIM, 0666);
    mint_file(OWN_OTHER, 0666);

    /* 01755: sticky, and writable by nobody but root. */
    mint_dir(LOCKED, 01755);
    mint_file(LOCK_VICTIM, 0666);

    run_child(user_child, "the uid-1000 child");

    /* Refused and nothing done. */
    want_there("sticky: the victim is still there", S_VICTIM);
    want_there("sticky: the mover is still there", S_MOVER);
    want_absent("sticky: the rename's destination", S_MOVED);
    want_there("sticky: the rmdir target is still there", S_RMME);
    want_there("sticky: the destination case's source", S_MYSRC);
    want_mode("sticky: the destination case's victim", S_TARGET, 0666);
    want_there("locked: the victim is still there", LOCK_VICTIM);

    /* Permitted and carried out. */
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

    run_child(other_child, "the uid-1001 child");

    want_there("sticky: the third uid's victim is still there", S_OTHER);
    want_there("owned: the third uid's victim is still there", OWN_OTHER);
    want_absent("sticky: the third uid's own file is gone", S_OTHOWN);
    want_absent("owned: the third uid's own file is gone", OWN_OTHOWN);

    /* The privileged clause, and the way the image is handed back as the
     * manifest staged it: root takes out of the sticky directory every entry
     * the two children were refused. */
    want_eq("root removes the victim", syscall3(SYS_UNLINK, (int)S_VICTIM, 0, 0), 0);
    want_eq("root removes the mover", syscall3(SYS_UNLINK, (int)S_MOVER, 0, 0), 0);
    want_eq("root removes the third uid's victim", syscall3(SYS_UNLINK, (int)S_OTHER, 0, 0), 0);
    want_eq("root removes the rmdir target", syscall3(SYS_RMDIR, (int)S_RMME, 0, 0), 0);
    want_eq("root removes the renamed file", syscall3(SYS_UNLINK, (int)S_MYRN2, 0, 0), 0);
    want_eq("root removes the destination case's source", syscall3(SYS_UNLINK, (int)S_MYSRC, 0, 0),
            0);
    want_eq("root removes the destination case's victim", syscall3(SYS_UNLINK, (int)S_TARGET, 0, 0),
            0);
    want_eq("root removes the plain rename's destination", syscall3(SYS_UNLINK, (int)P_MOVED, 0, 0),
            0);
    want_eq("root removes the plain destination case's victim",
            syscall3(SYS_UNLINK, (int)P_TARGET, 0, 0), 0);
    want_eq("root removes the third uid's owned victim", syscall3(SYS_UNLINK, (int)OWN_OTHER, 0, 0),
            0);
    want_eq("root removes the owned directory", syscall3(SYS_RMDIR, (int)OWNED, 0, 0), 0);
    want_eq("root removes the locked victim", syscall3(SYS_UNLINK, (int)LOCK_VICTIM, 0, 0), 0);
    want_eq("root removes the locked directory", syscall3(SYS_RMDIR, (int)LOCKED, 0, 0), 0);

    puts_("STICKYPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
