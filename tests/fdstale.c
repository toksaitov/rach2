/**
 * Verify that a descriptor open on a regular file names that file from open to
 * close: across an unlink, across a rename onto its name, and across a write
 * that relocates it.
 *
 * The probe asserted the inverse until this milestone. A KRFFS node was freed
 * the moment its name went, so every operation revalidated the cached pointer
 * and answered -EBADF to keep off the freed free-list metadata, and this file
 * asserted that read, lseek, _llseek and fchmod each did. The node is now kept
 * alive by the descriptor tables and freed by the last close, so those same
 * four calls succeed and report the file the descriptor opened. The credential
 * readers are what rest on it: each is several syscalls long (musl's
 * getspnam_r opens /etc/shadow and then reads it through fgets), the kernel
 * preempts between two syscalls, and a replacement landing in that window used
 * to make su, login and dropbear refuse a correct password.
 *
 * The control comes first, so that no later result can be credited to a
 * mis-staged fixture: a descriptor with nothing happening to it reads its file
 * and then reads end of file.
 *
 * Unlink under an open descriptor is the case whose assertion inverted. The
 * read returns the original bytes in full, lseek and _llseek to SEEK_END and
 * fstat64 all report the original size, fchmod succeeds, and a fresh open of
 * the name returns -ENOENT. That last one is what separates a lifetime from a
 * lookup that merely forgot to fail.
 *
 * The orphan is hidden from the scans that enumerate by parent id, not only
 * from the resolver. After such an unlink, getdents64 on the parent directory
 * lists neither the name nor a nameless entry, and rmdir of a directory whose
 * only child is an orphan this probe still has succeeds, which is what POSIX
 * says. Both fail on an orphan that keeps its parent id and nothing else
 * changes, the second with -ENOTEMPTY.
 *
 * A rename that fails after its destination was resolved must leave that
 * destination alone, and it is asserted in both the shapes this kernel has.
 * With a descriptor open on the file, renaming a directory onto its name
 * returns -EPERM and the name stays readable, through the held descriptor and
 * through a fresh open alike. An implementation that orphans before it refuses
 * cannot pass that. The other shape refuses for the destination, not the
 * source: renaming a file onto an open directory returns -EISDIR, which this
 * kernel decides after it takes the orphan, so the directory's name survives
 * only because a directory descriptor is not a holder. The assertion is what
 * keeps the answer right if one ever becomes one.
 *
 * The rename onto the reader's name is the case the whole change exists for,
 * and it runs across two processes, since a holder scan that looked only at
 * the calling process would pass a single-process version of it. The child
 * opens two files and blocks; the parent writes a replacement, renames it over
 * the first name, unlinks the second name and creates a different file at it,
 * and only then releases the child. Both descriptors must still read what they
 * opened, whole: not a mixture, which is what re-resolving the path would give
 * a reader that had already consumed part of the old file, and not an error.
 * The parent then reads both names itself and gets the replacements. No sleep
 * anywhere: each side blocks on a pipe the other writes, and a syscall on this
 * kernel is not preempted, so the interleaving is the one named here. Each
 * child signals the moment its opens have returned and before it judges them,
 * so a failed open reports itself through the exit status instead of hanging
 * the parent on a byte that never comes.
 *
 * Two descriptors on one orphan, and a child that has the last one, cover
 * the release itself: closing one of a dup'd pair leaves the other reading, so
 * the release counts descriptors and not files, and a child that _exits
 * without closing proves release_fd_table reaches the same release sys_close
 * does.
 *
 * Relocation is forced, not guessed at. krffs_resize_reserved_node
 * extends a node in place when the node that follows it is free and at least
 * `difference` bytes, and moves the file otherwise, so the trigger is the
 * neighbour and never a byte count. Two empty files created back to back put a
 * reserved node behind the first: a creat with no data takes the first free
 * node in the image whatever its size, since the allocator is first-fit and no
 * free node is ever smaller than a header, and the second creat then takes the
 * remainder split off right behind the first file. Where there was no
 * remainder to split, the first file absorbed its whole free node, and what
 * followed that node was already reserved, because the free list never has
 * two adjacent free nodes. Either way the first write past the capacity must
 * move the file, and 1024 bytes is past it: a creat leaves room for no data at
 * all beyond the slack too small to split off, which is under one 357-byte
 * node header. It is asserted twice, once with both descriptors in one table
 * and once with the holder in another process, for the reason the rename case
 * is two processes: a repointing that walked only the writer's own table would
 * pass the first and still leave every other table on the freed node.
 *
 * The leak check is the only free-space assertion a probe can make on a file
 * system with no statfs, and it is made in all three shapes, because the
 * release lives at the last close and only a descriptor ever reaches it. First
 * 256 rounds of open, unlink under the descriptor, close, on a 64 KB file: a
 * kernel that orphans and never releases needs 16 MB of a 10 MB image and
 * fails at or before round 160, where a correct one uses one extent 256 times.
 * Then the same count of the two shapes that must take no orphan at all: the
 * removal nothing is holding, which is almost every removal, and the write of
 * a new file renamed onto a name, which is what every credential replacement
 * actually does. Neither may exhaust the image, and between them they are what
 * the condition on the orphan is judged by, since an orphan taken at either
 * call site with no holder is one nobody is left to close.
 *
 * Directories stay out of scope, as a scope line and not an oversight: rmdir
 * still frees the node under an open directory descriptor, and getdents64 and
 * a relative path through that descriptor both answer -EBADF. A directory
 * descriptor in Rach2 is a name, and an orphan directory has no name for a
 * relative path to join. It is asserted last and asserts nothing between the
 * rmdir and the two answers, since a freed node the allocator hands out again
 * would be a live node under that descriptor.
 *
 * Every wait checks WIFEXITED before WEXITSTATUS: a signal death has the
 * signal in the low bits and would otherwise decode as exit 0, scoring a
 * crashed child as a passing one. Every fixture is minted and removed here, so
 * the probe hands the image back as it found it and runs twice in one boot.
 *
 * Prints "FDSTALE OK" and exits 0 only when every case passed; any failure
 * names the step and the answer it got, and exits 1. Freestanding; direct
 * int 0x80.
 */

typedef unsigned int u32;

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_CREAT      8
#define SYS_UNLINK     10
#define SYS_LSEEK      19
#define SYS_RENAME     38
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_DUP        41
#define SYS_PIPE       42
#define SYS_FCHMOD     94
#define SYS_LLSEEK     140
#define SYS_FSTAT64    197
#define SYS_GETDENTS64 220
#define SYS_OPENAT     295

#define STDOUT 1

#define EPERM   1
#define ENOTDIR 20
#define ENOENT  2
#define EBADF   9
#define EISDIR  21

#define O_RDONLY    0
#define O_RDWR      2
#define O_CREAT     0100
#define O_DIRECTORY 0200000
#define SEEK_END    2

/* i386 struct stat64: the 64-bit size field is at offset 44. */
#define ST_SIZE 44

/* linux_dirent64: d_reclen is at offset 16, the name begins at 19. */
#define D_RECLEN 16
#define D_NAME   19

/* Linux wait-status decode: a normal exit has the low 7 bits clear and the
 * code in bits 8-15; a signal death has the signal in the low 7 bits. */
#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

#define CTL   "/fdstale.ctl"  /* the control, which nothing happens to */
#define HELD  "/fdstale.held" /* unlinked under the descriptor */
#define ODIR  "/fdstale.odir" /* the directory whose only child is orphaned */
#define KID   "/fdstale.odir/kid"
#define KEEP  "/fdstale.keep" /* the destination a refused rename must spare */
#define RDIR  "/fdstale.rdir" /* the directory both refusals rest on */
#define OLD   "/fdstale.old"  /* renamed over while the child has it */
#define NEW   "/fdstale.new"  /* the replacement, before the rename */
#define GONE  "/fdstale.gone" /* re-created at its own name under the child */
#define EXITF "/fdstale.exit" /* held by a child that _exits without closing */
#define DUPF  "/fdstale.dup"  /* two descriptors on one orphan */
#define GROW  "/fdstale.grow" /* the file a write must relocate */
#define WALL  "/fdstale.wall" /* the reserved node that leaves it nowhere to go */
#define XGROW "/fdstale.xgrw" /* the same, moved by another process */
#define XWALL "/fdstale.xwal" /* and its wall */
#define LEAK  "/fdstale.leak" /* the unlink-under-a-descriptor leak loop */
#define SOLO  "/fdstale.solo" /* the loop that removes what nothing has */
#define STAGE "/fdstale.stg"  /* the rename loop's new file */
#define REPL  "/fdstale.repl" /* the name it lands on, which nobody has */
#define DDIR  "/fdstale.ddir" /* rmdir'd under an open directory descriptor */

/* The pairs a torn read would betray are the same length, so that only their
 * bytes, and not a byte count, can tell them apart. */
#define OLD_LINE   "old:x:0:0:the file the rename replaced\n"
#define NEW_LINE   "new:x:0:0:the file the rename produced\n"
#define GONE_LINE  "held:x:0:0:the file the reader opened\n"
#define FRESH_LINE "made:x:0:0:the file the writer minted\n"

#define CTL_LINE  "the control, which nothing happens to\n"
#define KID_LINE  "the only child of a directory to be removed\n"
#define HELD_LINE "the file whose name went while it was open\n"
#define KEEP_LINE "the file a refused rename must not touch\n"
#define EXIT_LINE "the file whose last holder never closed it\n"
#define DUP_LINE  "the file two descriptors share\n"

/* The relocating write: past the capacity a creat leaves, which is under one
 * 357-byte node header. */
#define GROW_BYTES 1024

/* 256 rounds of 64 KB is 16 MB, which a 10 MB image cannot hold twice over. */
#define LEAK_ROUNDS 256
#define LEAK_CHUNK  4096
#define LEAK_CHUNKS 16

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

static void fail(const char *step, int got)
{
    puts_("FDSTALE FAIL: ");
    puts_(step);
    puts_(" (got ");
    putd(got);
    puts_(")\n");
    syscall1(SYS_EXIT, 1);
}

/* A round-numbered failure: the leak loops are judged by where they stop. */
static void fail_round(const char *step, int round, int got)
{
    puts_("FDSTALE FAIL: ");
    puts_(step);
    puts_(" (round ");
    putd(round);
    puts_(", got ");
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

/* The bytes every filling write emits, and what the relocation case reads
 * back; no NUL, so a compare never ends early. */
static char blob[LEAK_CHUNK];

static char rdbuf[LEAK_CHUNK];

/* One stat64 buffer: the kernel writes it, want_fd_size reads it back. */
static char statbuf[144];

static int streq(const char *a, const char *b)
{
    u32 i;

    for (i = 0; a[i] != '\0' && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

/* Read the rest of the file through `fd` and require exactly `len` bytes equal
 * to `want`, then end of file. A short read, a long one, or one byte out of
 * place is a different file or a mixture of two. */
static void want_bytes(const char *step, int fd, const char *want, int len)
{
    int n = syscall3(SYS_READ, fd, (int)rdbuf, (int)sizeof(rdbuf));
    int i;

    if (n != len) {
        fail(step, n);
    }
    for (i = 0; i < len; i++) {
        if (rdbuf[i] != want[i]) {
            puts_("FDSTALE FAIL: ");
            puts_(step);
            puts_(" (byte ");
            putd(i);
            puts_(")\n");
            syscall1(SYS_EXIT, 1);
        }
    }
    n = syscall3(SYS_READ, fd, (int)rdbuf, (int)sizeof(rdbuf));
    if (n != 0) {
        fail(step, n);
    }
}

/* Open `path` read-only, read it whole, close it: the view by name, which the
 * held descriptor's view is judged against. */
static void want_named_bytes(const char *step, const char *path, const char *want, int len)
{
    int fd = syscall3(SYS_OPEN, (int)path, O_RDONLY, 0);

    want_ok(step, fd);
    want_bytes(step, fd, want, len);
    want_eq(step, syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

/* The size fstat64 reports through the descriptor itself. */
static void want_fd_size(const char *step, int fd, int size)
{
    int got;

    want_eq(step, syscall3(SYS_FSTAT64, fd, (int)statbuf, 0), 0);
    got = *(int *)(statbuf + ST_SIZE);
    if (got != size) {
        fail(step, got);
    }
}

/* Create `path` holding `text`, whatever was there before. */
static void mint(const char *path, const char *text)
{
    int fd = syscall3(SYS_CREAT, (int)path, 0644, 0);
    int len = (int)slen(text);

    want_ok("mint a fixture", fd);
    want_eq("fill a fixture", syscall3(SYS_WRITE, fd, (int)text, len), len);
    want_eq("close a fixture", syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

/* Count the entries of `dirpath`, refusing a nameless one and refusing
 * `forbidden`. An orphan that kept its parent id and nothing else shows up
 * here as one or the other. One call drains the directories this probe builds,
 * which hold one child at most. */
static int dir_entries(const char *step, const char *dirpath, const char *forbidden)
{
    static char dbuf[1024];
    int fd = syscall3(SYS_OPEN, (int)dirpath, O_RDONLY | O_DIRECTORY, 0);
    int seen = 0;
    int pos = 0;
    int n;

    want_ok(step, fd);
    n = syscall3(SYS_GETDENTS64, fd, (int)dbuf, (int)sizeof(dbuf));
    if (n < 0) {
        fail(step, n);
    }
    while (pos < n) {
        int reclen = *(unsigned short *)(dbuf + pos + D_RECLEN);
        const char *name = dbuf + pos + D_NAME;

        if (reclen <= 0) {
            fail(step, reclen);
        }
        if (name[0] == '\0' || streq(name, forbidden)) {
            fail(step, pos);
        }
        pos += reclen;
        /* A root that keeps `.` and `..` on the disk returns them; the boot
         * root synthesizes neither. What this counts is the children, so the
         * two are stepped over on whichever root they appear. */
        if (streq(name, ".") || streq(name, "..")) {
            continue;
        }
        seen++;
    }
    want_eq(step, syscall3(SYS_CLOSE, fd, 0, 0), 0);
    return seen;
}

/* The rendezvous: up has the child's "I am holding it", down the parent's
 * "I have replaced it". */
static int up[2];
static int down[2];

static void notify(const char *step, int fd)
{
    want_eq(step, syscall3(SYS_WRITE, fd, (int)"x", 1), 1);
}

static void wait_for(const char *step, int fd)
{
    char c;

    want_eq(step, syscall3(SYS_READ, fd, (int)&c, 1), 1);
}

static int spawn(void (*body)(void), const char *what)
{
    int pid = syscall1(SYS_FORK, 0);

    if (pid < 0) {
        fail(what, pid);
    }
    if (pid == 0) {
        body();
        /* Unreachable: every body exits itself. Non-zero so that a body that
         * ever grew a path out the bottom would fail, not pass. */
        syscall1(SYS_EXIT, 127);
    }
    return pid;
}

/* Reap one child and require a clean exit 0. WIFEXITED comes first: a signal
 * death would otherwise decode as exit 0. The child has already named the case
 * it failed. */
static void reap(int pid, const char *what)
{
    int status = 0;
    int reaped = syscall3(SYS_WAITPID, pid, (int)&status, 0);

    if (reaped != pid) {
        fail(what, reaped);
    }
    if (!WIFEXITED(status)) {
        puts_("FDSTALE FAIL: ");
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

/* Nothing happens to this descriptor, so what it reads is what every later
 * case is judged against. */
static void control_case(void)
{
    int fd;

    mint(CTL, CTL_LINE);
    fd = syscall3(SYS_OPEN, (int)CTL, O_RDONLY, 0);
    want_ok("the control open", fd);
    want_bytes("the control read", fd, CTL_LINE, (int)slen(CTL_LINE));
    want_eq("the control close", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("remove the control", syscall3(SYS_UNLINK, (int)CTL, 0, 0), 0);
}

/* The case whose assertion inverted: the four calls that used to answer -EBADF
 * through a descriptor whose name is gone. */
static void unlink_case(void)
{
    int len = (int)slen(HELD_LINE);
    int fd;

    mint(HELD, HELD_LINE);
    fd = syscall3(SYS_OPEN, (int)HELD, O_RDONLY, 0);
    want_ok("open the file to be unlinked", fd);
    want_eq("unlink under the descriptor", syscall3(SYS_UNLINK, (int)HELD, 0, 0), 0);

    want_bytes("read through the unlinked descriptor", fd, HELD_LINE, len);
    want_eq("lseek SEEK_END through the unlinked descriptor", syscall3(SYS_LSEEK, fd, 0, SEEK_END),
            len);
    {
        /* _llseek is the path musl's lseek takes: EBX fd, ECX/EDX offset high
         * and low, ESI result, EDI whence. */
        long long res = -1;
        int r;

        __asm__ __volatile__("int $0x80"
                             : "=a"(r)
                             : "a"(SYS_LLSEEK), "b"(fd), "c"(0), "d"(0), "S"(&res), "D"(SEEK_END)
                             : "memory");
        want_eq("_llseek SEEK_END through the unlinked descriptor", r, 0);
        want_eq("_llseek's result through the unlinked descriptor", (int)res, len);
    }
    want_fd_size("fstat64 through the unlinked descriptor", fd, len);
    want_eq("fchmod through the unlinked descriptor", syscall3(SYS_FCHMOD, fd, 0600, 0), 0);

    /* The lifetime is the descriptor's, not the name's: the name is gone. */
    want_eq("open the unlinked name", syscall3(SYS_OPEN, (int)HELD, O_RDONLY, 0), -ENOENT);
    want_eq("close the unlinked descriptor", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("open the unlinked name after the last close",
            syscall3(SYS_OPEN, (int)HELD, O_RDONLY, 0), -ENOENT);
}

/* The orphan is unreachable by name; it must be invisible to the scans that
 * enumerate by parent id as well. */
static void orphan_scan_case(void)
{
    int fd;

    want_eq("mkdir the orphan's directory", syscall3(SYS_MKDIR, (int)ODIR, 0755, 0), 0);
    mint(KID, KID_LINE);
    want_eq("count the directory's children", dir_entries("list the directory", ODIR, ""), 1);

    fd = syscall3(SYS_OPEN, (int)KID, O_RDONLY, 0);
    want_ok("open the child", fd);
    want_eq("unlink the child under the descriptor", syscall3(SYS_UNLINK, (int)KID, 0, 0), 0);

    want_eq("count the children past the orphan", dir_entries("list past the orphan", ODIR, "kid"),
            0);
    want_eq("rmdir over the orphan", syscall3(SYS_RMDIR, (int)ODIR, 0, 0), 0);

    want_bytes("read the orphan after its directory went", fd, KID_LINE, (int)slen(KID_LINE));
    want_eq("close the orphan", syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

/* A refusal must destroy nothing, in both the shapes this kernel has: the
 * directory source, refused before the destination is resolved, and the
 * directory destination, refused after the orphan is taken. The second name
 * survives only because a directory descriptor is not a holder, so it is
 * asserted, not assumed. */
/* Whether this root performs a directory's rename. The boot root refuses every
 * one of them with EPERM, since its children name their parent by id; ext2
 * moves a directory and answers a rename onto a file with ENOTDIR, which is
 * what POSIX asks for. The row below takes whichever answer its root owes. */
static int root_moves_directories(void)
{
    static char mounts[1024];
    int fd = syscall3(SYS_OPEN, (int)"/proc/mounts", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (int)mounts, (int)sizeof(mounts) - 1);
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (n <= 0) {
        return 0;
    }
    mounts[n] = 0;
    for (i = 0; i + 8 < n; i++) {
        if (mounts[i] == ' ' && mounts[i + 1] == '/' && mounts[i + 2] == ' ') {
            return !(mounts[i + 3] == 'k' && mounts[i + 4] == 'r' && mounts[i + 5] == 'f');
        }
    }
    return 0;
}

static void failed_rename_case(void)
{
    int len = (int)slen(KEEP_LINE);
    int fd;
    int dirfd;
    int again;

    mint(KEEP, KEEP_LINE);
    want_eq("mkdir the refused source", syscall3(SYS_MKDIR, (int)RDIR, 0755, 0), 0);
    fd = syscall3(SYS_OPEN, (int)KEEP, O_RDONLY, 0);
    want_ok("open the refused rename's destination", fd);

    want_eq("rename a directory onto a held name", syscall3(SYS_RENAME, (int)RDIR, (int)KEEP, 0),
            root_moves_directories() ? -ENOTDIR : -EPERM);
    want_bytes("read the spared file through the held descriptor", fd, KEEP_LINE, len);
    want_named_bytes("read the spared file by name", KEEP, KEEP_LINE, len);

    dirfd = syscall3(SYS_OPEN, (int)RDIR, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open the directory destination", dirfd);
    want_eq("rename a file onto a held directory", syscall3(SYS_RENAME, (int)KEEP, (int)RDIR, 0),
            -EISDIR);
    again = syscall3(SYS_OPEN, (int)RDIR, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open the spared directory by name", again);
    want_eq("close the re-opened directory", syscall3(SYS_CLOSE, again, 0, 0), 0);
    want_eq("close the directory destination", syscall3(SYS_CLOSE, dirfd, 0, 0), 0);

    want_eq("close the spared file", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_eq("remove the refused source", syscall3(SYS_RMDIR, (int)RDIR, 0, 0), 0);
    want_eq("remove the spared file", syscall3(SYS_UNLINK, (int)KEEP, 0, 0), 0);
}

/* The child of the cross-process case: it opens both names and then blocks,
 * so the parent's replacement lands squarely between its open and its read. */
static void reader_child(void)
{
    int fd_old = syscall3(SYS_OPEN, (int)OLD, O_RDONLY, 0);
    int fd_gone = syscall3(SYS_OPEN, (int)GONE, O_RDONLY, 0);

    /* Before the opens are judged: a failed open must report itself through
     * the exit status, not hang the parent on a byte that never comes. */
    notify("the child signals its opens", up[1]);
    want_ok("the child opens the renamed-over name", fd_old);
    want_ok("the child opens the re-created name", fd_gone);
    wait_for("the child waits for the replacement", down[0]);

    want_bytes("the child reads across a rename onto its name", fd_old, OLD_LINE,
               (int)slen(OLD_LINE));
    want_bytes("the child reads across a re-created name", fd_gone, GONE_LINE,
               (int)slen(GONE_LINE));
    want_eq("the child closes the renamed-over file", syscall3(SYS_CLOSE, fd_old, 0, 0), 0);
    want_eq("the child closes the re-created file", syscall3(SYS_CLOSE, fd_gone, 0, 0), 0);
    syscall1(SYS_EXIT, 0);
}

/* The case the change exists for, and the one a single-process version of
 * would not reach: the holder is another process. */
static void cross_process_case(void)
{
    int pid;

    mint(OLD, OLD_LINE);
    mint(GONE, GONE_LINE);

    pid = spawn(reader_child, "fork the reader");
    wait_for("the parent waits for the child's opens", up[0]);

    mint(NEW, NEW_LINE);
    want_eq("rename the replacement onto the held name",
            syscall3(SYS_RENAME, (int)NEW, (int)OLD, 0), 0);
    want_eq("unlink the re-created name", syscall3(SYS_UNLINK, (int)GONE, 0, 0), 0);
    mint(GONE, FRESH_LINE);

    notify("the parent releases the child", down[1]);
    reap(pid, "the reader child");

    /* What the names hold now is the writer's, whatever the reader saw. */
    want_named_bytes("the renamed-onto name has the replacement", OLD, NEW_LINE,
                     (int)slen(NEW_LINE));
    want_named_bytes("the re-created name has the new file", GONE, FRESH_LINE,
                     (int)slen(FRESH_LINE));
    want_eq("remove the renamed-onto name", syscall3(SYS_UNLINK, (int)OLD, 0, 0), 0);
    want_eq("remove the re-created name", syscall3(SYS_UNLINK, (int)GONE, 0, 0), 0);
}

/* The child of the release case: it has the only descriptor on an orphan and
 * exits without closing it. */
static void holder_child(void)
{
    int fd = syscall3(SYS_OPEN, (int)EXITF, O_RDONLY, 0);

    notify("the holder signals its open", up[1]);
    want_ok("the holder opens the file", fd);
    wait_for("the holder waits for the unlink", down[0]);
    want_bytes("the holder reads the file whose name went", fd, EXIT_LINE, (int)slen(EXIT_LINE));
    syscall1(SYS_EXIT, 0);
}

static void exit_release_case(void)
{
    int pid;

    mint(EXITF, EXIT_LINE);
    pid = spawn(holder_child, "fork the holder");
    wait_for("the parent waits for the holder's open", up[0]);
    want_eq("unlink under the holder", syscall3(SYS_UNLINK, (int)EXITF, 0, 0), 0);
    notify("the parent releases the holder", down[1]);
    reap(pid, "the holder child");
    want_eq("open the name the holder let go", syscall3(SYS_OPEN, (int)EXITF, O_RDONLY, 0),
            -ENOENT);
}

/* The child of the cross-process relocation case: it opens the file before the
 * write that moves it and reads it after. */
static void moved_child(void)
{
    int fd = syscall3(SYS_OPEN, (int)XGROW, O_RDONLY, 0);

    notify("the moved-file child signals its open", up[1]);
    want_ok("the moved-file child opens the file", fd);
    wait_for("the moved-file child waits for the write", down[0]);

    want_eq("the moved-file child seeks to the new end", syscall3(SYS_LSEEK, fd, 0, SEEK_END),
            GROW_BYTES);
    want_eq("the moved-file child rewinds", syscall3(SYS_LSEEK, fd, 0, 0), 0);
    want_bytes("the moved-file child reads what another process grew", fd, blob, GROW_BYTES);
    want_eq("the moved-file child closes the moved file", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    syscall1(SYS_EXIT, 0);
}

/* Relocation with the holder in another process. The two-descriptor case below
 * keeps both in one table, so a repointing that walked only the writer's own
 * process would pass it and still leave every other table on the node the
 * allocator freed. That is the same hole the rename case is two processes for,
 * in the half of the contract a write owns. */
static void cross_reloc_case(void)
{
    int pid, fd, writer;

    fd = syscall3(SYS_CREAT, (int)XGROW, 0644, 0);
    want_ok("mint the file another process moves", fd);
    want_eq("close the file another process moves", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    fd = syscall3(SYS_CREAT, (int)XWALL, 0644, 0);
    want_ok("mint the wall behind the shared file", fd);
    want_eq("close the wall behind the shared file", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    pid = spawn(moved_child, "fork the moved-file reader");
    wait_for("the parent waits for the moved-file child's open", up[0]);

    writer = syscall3(SYS_OPEN, (int)XGROW, O_RDWR, 0);
    want_ok("open the writer that moves the file", writer);
    want_eq("the write that must move the file under another process",
            syscall3(SYS_WRITE, writer, (int)blob, GROW_BYTES), GROW_BYTES);
    want_eq("close the writer that moved the file", syscall3(SYS_CLOSE, writer, 0, 0), 0);

    notify("the parent releases the moved-file child", down[1]);
    reap(pid, "the moved-file child");
    want_eq("remove the shared moved file", syscall3(SYS_UNLINK, (int)XGROW, 0, 0), 0);
    want_eq("remove the wall behind it", syscall3(SYS_UNLINK, (int)XWALL, 0, 0), 0);
}

/* The release counts descriptors, not files: a dup'd pair has one orphan and
 * the survivor goes on reading. */
static void dup_case(void)
{
    int len = (int)slen(DUP_LINE);
    int fd, dupfd;

    mint(DUPF, DUP_LINE);
    fd = syscall3(SYS_OPEN, (int)DUPF, O_RDONLY, 0);
    want_ok("open the file to be dup'd", fd);
    dupfd = syscall1(SYS_DUP, fd);
    want_ok("dup the descriptor", dupfd);
    want_eq("unlink under the dup'd pair", syscall3(SYS_UNLINK, (int)DUPF, 0, 0), 0);
    want_eq("close one of the pair", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    want_bytes("read through the surviving descriptor", dupfd, DUP_LINE, len);
    want_eq("close the survivor", syscall3(SYS_CLOSE, dupfd, 0, 0), 0);
}

/* A write through one descriptor moves the file; the other must follow it.
 * See the header for why the wall is what forces the move. */
static void reloc_case(void)
{
    int fd, reader, writer;

    fd = syscall3(SYS_CREAT, (int)GROW, 0644, 0);
    want_ok("mint the file to be moved", fd);
    want_eq("close the file to be moved", syscall3(SYS_CLOSE, fd, 0, 0), 0);
    fd = syscall3(SYS_CREAT, (int)WALL, 0644, 0);
    want_ok("mint the wall behind it", fd);
    want_eq("close the wall", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    reader = syscall3(SYS_OPEN, (int)GROW, O_RDONLY, 0);
    want_ok("open the reader before the move", reader);
    writer = syscall3(SYS_OPEN, (int)GROW, O_RDWR, 0);
    want_ok("open the writer", writer);

    want_eq("the write that must move the file", syscall3(SYS_WRITE, writer, (int)blob, GROW_BYTES),
            GROW_BYTES);

    want_eq("lseek SEEK_END through the moved-from descriptor",
            syscall3(SYS_LSEEK, reader, 0, SEEK_END), GROW_BYTES);
    want_eq("rewind the moved-from descriptor", syscall3(SYS_LSEEK, reader, 0, 0), 0);
    want_bytes("read through the moved-from descriptor", reader, blob, GROW_BYTES);
    want_fd_size("fstat64 through the moved-from descriptor", reader, GROW_BYTES);

    want_eq("close the reader", syscall3(SYS_CLOSE, reader, 0, 0), 0);
    want_eq("close the writer", syscall3(SYS_CLOSE, writer, 0, 0), 0);
    want_eq("remove the moved file", syscall3(SYS_UNLINK, (int)GROW, 0, 0), 0);
    want_eq("remove the wall", syscall3(SYS_UNLINK, (int)WALL, 0, 0), 0);
}

/* Fill `fd` to 64 KB, the size at which 256 rounds outgrow the image. */
static void fill_round(const char *step, int fd, int round)
{
    int i;

    for (i = 0; i < LEAK_CHUNKS; i++) {
        int n = syscall3(SYS_WRITE, fd, (int)blob, LEAK_CHUNK);

        if (n != LEAK_CHUNK) {
            fail_round(step, round, n);
        }
    }
}

/* An orphan released at the last close costs one extent however often it is
 * made; one that is never released costs 16 MB of a 10 MB image. */
static void leak_by_close_case(void)
{
    int round;

    for (round = 0; round < LEAK_ROUNDS; round++) {
        int fd = syscall3(SYS_OPEN, (int)LEAK, O_RDWR | O_CREAT, 0644);
        int rc;

        if (fd < 0) {
            fail_round("open in the unlink leak loop", round, fd);
        }
        fill_round("fill in the unlink leak loop", fd, round);
        rc = syscall3(SYS_UNLINK, (int)LEAK, 0, 0);
        if (rc != 0) {
            fail_round("unlink in the unlink leak loop", round, rc);
        }
        rc = syscall3(SYS_CLOSE, fd, 0, 0);
        if (rc != 0) {
            fail_round("close in the unlink leak loop", round, rc);
        }
    }
}

/* The removal nothing is reading, which is almost every removal. The loop above
 * cannot reach it: it always unlinks under a descriptor, and that descriptor is
 * what releases the orphan. Here there is none, so an orphan taken anyway is
 * never closed and never released. */
static void leak_by_unlink_case(void)
{
    int round;

    for (round = 0; round < LEAK_ROUNDS; round++) {
        int fd = syscall3(SYS_CREAT, (int)SOLO, 0644, 0);
        int rc;

        if (fd < 0) {
            fail_round("creat in the unheld unlink loop", round, fd);
        }
        fill_round("fill in the unheld unlink loop", fd, round);
        rc = syscall3(SYS_CLOSE, fd, 0, 0);
        if (rc != 0) {
            fail_round("close in the unheld unlink loop", round, rc);
        }
        rc = syscall3(SYS_UNLINK, (int)SOLO, 0, 0);
        if (rc != 0) {
            fail_round("unlink in the unheld unlink loop", round, rc);
        }
    }
}

/* The shape every credential replacement has, onto a name no descriptor has.
 * An orphan made unconditionally is never closed and never released, so this
 * is the loop that catches it. */
static void leak_by_rename_case(void)
{
    int round;

    for (round = 0; round < LEAK_ROUNDS; round++) {
        int fd = syscall3(SYS_CREAT, (int)STAGE, 0644, 0);
        int rc;

        if (fd < 0) {
            fail_round("creat in the rename leak loop", round, fd);
        }
        fill_round("fill in the rename leak loop", fd, round);
        rc = syscall3(SYS_CLOSE, fd, 0, 0);
        if (rc != 0) {
            fail_round("close in the rename leak loop", round, rc);
        }
        rc = syscall3(SYS_RENAME, (int)STAGE, (int)REPL, 0);
        if (rc != 0) {
            fail_round("rename in the rename leak loop", round, rc);
        }
    }
    want_eq("remove the replaced name", syscall3(SYS_UNLINK, (int)REPL, 0, 0), 0);
}

/* The scope line, asserted: a directory descriptor is a name, so rmdir still
 * takes the directory out from under it. Nothing is allocated between the
 * rmdir and the two answers, since a freed node handed out again would be a
 * live node under this descriptor. */
static void directory_scope_case(void)
{
    static char dbuf[256];
    int fd;

    want_eq("mkdir the directory to be removed", syscall3(SYS_MKDIR, (int)DDIR, 0755, 0), 0);
    fd = syscall3(SYS_OPEN, (int)DDIR, O_RDONLY | O_DIRECTORY, 0);
    want_ok("open the directory", fd);
    want_eq("rmdir under the directory descriptor", syscall3(SYS_RMDIR, (int)DDIR, 0, 0), 0);

    want_eq("getdents64 through the removed directory",
            syscall3(SYS_GETDENTS64, fd, (int)dbuf, (int)sizeof(dbuf)), -EBADF);
    want_eq("a relative path through the removed directory",
            syscall4(SYS_OPENAT, fd, (int)"kid", O_RDONLY, 0), -EBADF);
    want_eq("close the removed directory", syscall3(SYS_CLOSE, fd, 0, 0), 0);
}

void _start(void)
{
    int i;

    for (i = 0; i < LEAK_CHUNK; i++) {
        blob[i] = (char)('a' + (i % 26));
    }

    control_case();
    unlink_case();
    orphan_scan_case();
    failed_rename_case();

    want_eq("pipe for the child's signal", syscall3(SYS_PIPE, (int)up, 0, 0), 0);
    want_eq("pipe for the parent's signal", syscall3(SYS_PIPE, (int)down, 0, 0), 0);
    cross_process_case();
    exit_release_case();
    cross_reloc_case();
    want_eq("close the child's signal read end", syscall3(SYS_CLOSE, up[0], 0, 0), 0);
    want_eq("close the child's signal write end", syscall3(SYS_CLOSE, up[1], 0, 0), 0);
    want_eq("close the parent's signal read end", syscall3(SYS_CLOSE, down[0], 0, 0), 0);
    want_eq("close the parent's signal write end", syscall3(SYS_CLOSE, down[1], 0, 0), 0);

    dup_case();
    reloc_case();
    leak_by_close_case();
    leak_by_unlink_case();
    leak_by_rename_case();
    directory_scope_case();

    puts_("FDSTALE OK\n");
    syscall1(SYS_EXIT, 0);
}
