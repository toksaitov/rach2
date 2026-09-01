/**
 * Verify that no set-id clear ever reaches the root node, whose spare mode bit
 * is the dirty-mount flag.
 *
 * `KRFFS_MODE_DIRTY` and `KRFFS_MODE_SUID` are the same bit, 0004000. The flag
 * lives only on the root node, which nobody can exec and whose mode the
 * fixed-identity paths never expose, so the collision is safe exactly as long
 * as nothing writes the root's mode. The set-id clear is the first rule that
 * would: `chown("/", 0, 0)` reaches `krffs_chown_node` from any uid, since the
 * empty name resolves to the mounted root and a chown that changes nothing
 * needs no right. An unguarded clear appended there is an unprivileged clear of
 * the crash evidence `fsck.krffs` reads. The guard is by node identity and not
 * by `KRFFS_IS_DIR`, because `mkfs` stamps the root a bare 0755 with no type
 * bits, so the directory test reads false on it.
 *
 * This cannot be proved from the RAM image, where the flag is never set, nor by
 * stat, which reports the root with a fixed `S_IFDIR | 0755` and would show a
 * clean mode on a corrupted image. So the probe runs under the rescue shell on
 * a disk-backed mount, hammers every path that reaches the root node with a
 * mode-adjacent operation, and the crash suite kills QEMU afterwards and
 * requires offline `fsck.krffs` still to report "not cleanly unmounted".
 *
 * The `sync(2)` after each attempt is the whole proof. The page cache is
 * write-back, so a spurious clear would sit in a dirty frame and never reach
 * the platter before QEMU is killed, and the phase would go green on a broken
 * kernel. Each attempt is flushed before the next one runs.
 *
 * The six calls and the answers this kernel gives a root caller, none of which
 * this phase changes:
 *
 *   chown("/", -1, -1)          0, the no-change chown any caller may make
 *   chown("/", 0, 0)            0, the root node is already owned 0:0
 *   chmod("/", 0755)            0, and the stored mode is deliberately not
 *                               written; an unprivileged caller gets -EPERM
 *   open("/", O_WRONLY|O_TRUNC) -EISDIR, a directory takes neither
 *   creat("/", 0644)            -EINVAL, the canonical root leaves no name
 *   write() on a descriptor
 *   opened on "/"               -EBADF, a directory descriptor has no writer
 *
 * `fchmod` and `fchown` on that same descriptor are absent on purpose: a
 * descriptor on the root has a NULL node, so both take the fixed-metadata
 * branch and never reach the node at all.
 *
 * Prints "DIRTYPROBE OK" and exits 0 only when every answer held; any failure
 * names the step and the answer it got, and exits 1. Freestanding; direct
 * int 0x80.
 */

typedef unsigned int u32;

#define SYS_EXIT    1
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_CREAT   8
#define SYS_CHMOD   15
#define SYS_SYNC    36
#define SYS_CHOWN32 212

#define STDOUT 1

#define EBADF  9
#define EISDIR 21
#define EINVAL 22

#define O_RDONLY 0
#define O_WRONLY 1
#define O_TRUNC  01000

/* The chown sentinel that keeps a field: (uid_t)-1 as an unsigned word. */
#define KEEP 0xFFFFFFFFu

#define ROOT "/"

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

static void fail(const char *step, int got)
{
    puts_("DIRTYPROBE FAIL: ");
    puts_(step);
    puts_(" (got ");
    putd(got);
    puts_(")\n");
    syscall1(SYS_EXIT, 1);
}

/* Flush the page cache, then assert the answer. The flush comes first because
 * it is the assertion that matters: a mode the kernel wrote into a cached frame
 * has to reach the platter for the offline fsck to see it, and QEMU is killed
 * without a clean unmount. */
static void want_eq(const char *step, int got, int want)
{
    int rc = syscall1(SYS_SYNC, 0);

    if (rc != 0) {
        fail("sync", rc);
    }
    if (got != want) {
        fail(step, got);
    }
}

void _start(void)
{
    char byte = 'x';
    int fd;

    /* Two chowns that stop at the change gate, and one that does not. The
     * first changes nothing and needs no right; the second names the ids the
     * root node already has, so it too moves no field. Neither enters the
     * clear at all, which means neither can show whether the clear would spare
     * the root: the gate answers first. The third moves the group, so it is the
     * only call in this probe that reaches the helper with the root node, and
     * it is therefore the whole of the guard's proof. The group goes out and
     * comes straight back, so the image is handed on as it was found. */
    want_eq("chown(-1, -1) on the root", syscall3(SYS_CHOWN32, (int)ROOT, (int)KEEP, (int)KEEP), 0);
    want_eq("chown(0, 0) on the root", syscall3(SYS_CHOWN32, (int)ROOT, 0, 0), 0);
    want_eq("chown(-1, 1) on the root", syscall3(SYS_CHOWN32, (int)ROOT, (int)KEEP, 1), 0);
    want_eq("restore the root's group", syscall3(SYS_CHOWN32, (int)ROOT, (int)KEEP, 0), 0);

    /* The chmod rule answers for the root but stores nothing. */
    want_eq("chmod the root", syscall3(SYS_CHMOD, (int)ROOT, 0755, 0), 0);

    /* The two truncating opens, each refused before it can reach a clear. */
    want_eq("open the root for writing with O_TRUNC",
            syscall3(SYS_OPEN, (int)ROOT, O_WRONLY | O_TRUNC, 0), -EISDIR);
    want_eq("creat the root", syscall3(SYS_CREAT, (int)ROOT, 0644, 0), -EISDIR);

    /* A descriptor on the root exists and reads; writing through it is refused
     * by the descriptor's own type, so the write clear is never reached. */
    fd = syscall3(SYS_OPEN, (int)ROOT, O_RDONLY, 0);
    if (fd < 0) {
        fail("open the root for reading", fd);
    }
    want_eq("write through a descriptor on the root", syscall3(SYS_WRITE, fd, (int)&byte, 1),
            -EBADF);
    want_eq("close the descriptor on the root", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    puts_("DIRTYPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
