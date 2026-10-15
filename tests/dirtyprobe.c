/**
 * dirtyprobe: no set-id clear reaches the root node of a disk-backed KRFFS
 * root, whose spare mode bit is the dirty-mount flag.
 *
 * `KRFFS_MODE_DIRTY` and `KRFFS_MODE_SUID` are the same bit, 0004000. The flag
 * lives only on the root node, which nobody can exec and whose mode the
 * fixed-identity paths never expose, so the collision is safe as long as
 * nothing writes the root's mode. The set-id clear of chown would write it:
 * `cred_kill_setid` runs when a chown changes a field, and chown("/") reaches
 * `krffs_chown_node` with the root node, since the empty name resolves to the
 * mounted root. The clear skips the root by node identity and not by
 * `KRFFS_IS_DIR`, because `mkfs` stamps the root a bare 0755 with no type
 * bits, so the directory test reads false on it.
 *
 * The RAM image never sets the flag, and stat reports the root with a fixed
 * `S_IFDIR | 0755`, so neither can show a cleared flag. The probe runs on a
 * disk-backed root booted dirty and makes each call below that comes near
 * the root node's mode; the crash suite (tests/system/suites/crash.sh) then
 * kills QEMU and requires the offline `fsck.krffs` still to report "not
 * cleanly unmounted".
 *
 * Each call is followed by sync(2). The page cache is write-back, so a
 * clear left in a dirty frame when QEMU is killed would not reach the disk,
 * and the suite would pass on a kernel that clears the flag.
 *
 * The calls, and the answers the kernel gives a root caller:
 *
 *   chown("/", -1, -1)          0, the no-change chown any caller may make
 *   chown("/", 0, 0)            0, the root node is already owned 0:0
 *   chown("/", -1, 1)           0, the one call that reaches the clear
 *   chown("/", -1, 0)           0, which puts the group back
 *   chmod("/", 0755)            0, and the stored mode is not written; an
 *                               unprivileged caller gets -EPERM
 *   open("/", O_WRONLY|O_TRUNC) -EISDIR, a directory takes neither
 *   creat("/", 0644)            -EISDIR, the open of a directory with O_CREAT
 *   write() on a descriptor
 *   opened on "/"               -EBADF, a directory descriptor has no write
 *
 * `fchmod` and `fchown` on that descriptor are left out: a descriptor on the
 * root has a NULL node, so neither reaches `krffs_chmod_node` or
 * `krffs_chown_node`, and a change through either is -EROFS from
 * `vfs_setattr`, since the KRFFS root has no setattr.
 *
 * Prints "DIRTYPROBE OK" and exits 0 when every answer is as listed;
 * otherwise it prints "DIRTYPROBE FAIL: ", the step and the answer it got,
 * and exits 1.
 */

#include "probe.h"

typedef unsigned int u32;

#define STDOUT 1

#define EBADF  9
#define EISDIR 21

#define O_RDONLY 0
#define O_WRONLY 1
#define O_TRUNC  01000

/* The chown sentinel that keeps a field: (uid_t)-1 as an unsigned word. */
#define KEEP 0xFFFFFFFFu

#define ROOT "/"

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

/** Print `v` in signed decimal, so a failing line has the answer it got. */
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

/** Print "DIRTYPROBE FAIL: ", `step` and the answer `got`, and exit 1. */
static void fail(const char *step, int got)
{
    puts_("DIRTYPROBE FAIL: ");
    puts_(step);
    puts_(" (got ");
    putd(got);
    puts_(")\n");
    syscall1(SYS_EXIT, 1);
}

/** Sync, then fail at `step` unless `got` is `want`. The sync comes first, so
 * that a mode the kernel wrote into a cached frame reaches the disk for the
 * offline fsck to see, since QEMU is killed without a clean unmount; a sync
 * that fails is the failure of the step "sync". */
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

/**
 * The probe: the calls of the table in order, each through `want_eq`.
 *
 * Of the four chowns, the first changes nothing and needs no right, and the
 * second names the ids the root node already has, so neither changes a
 * field and neither reaches the set-id clear. The third moves the group, so
 * it is the one call that reaches `cred_kill_setid` with the root node, and
 * the fourth puts the group back, so the image is left as it was found.
 */
PROBE_START void _start(void)
{
    char byte = 'x';
    int fd;

    /* Only the third chown changes a field, and so reaches the clear. */
    want_eq("chown(-1, -1) on the root", syscall3(SYS_CHOWN, (long)ROOT, (int)KEEP, (int)KEEP), 0);
    want_eq("chown(0, 0) on the root", syscall3(SYS_CHOWN, (long)ROOT, 0, 0), 0);
    want_eq("chown(-1, 1) on the root", syscall3(SYS_CHOWN, (long)ROOT, (int)KEEP, 1), 0);
    want_eq("restore the root's group", syscall3(SYS_CHOWN, (long)ROOT, (int)KEEP, 0), 0);

    /* A chmod of the root succeeds and stores nothing. */
    want_eq("chmod the root", syscall3(SYS_CHMOD, (long)ROOT, 0755, 0), 0);

    /* The two truncating opens, each refused before it can reach a clear. */
    want_eq("open the root for writing with O_TRUNC",
            syscall3(SYS_OPEN, (long)ROOT, O_WRONLY | O_TRUNC, 0), -EISDIR);
    want_eq("creat the root", syscall3(SYS_CREAT, (long)ROOT, 0644, 0), -EISDIR);

    /* A directory descriptor has no write, so the write's clear is not run. */
    fd = syscall3(SYS_OPEN, (long)ROOT, O_RDONLY, 0);
    if (fd < 0) {
        fail("open the root for reading", fd);
    }
    want_eq("write through a descriptor on the root", syscall3(SYS_WRITE, fd, (long)&byte, 1),
            -EBADF);
    want_eq("close the descriptor on the root", syscall3(SYS_CLOSE, fd, 0, 0), 0);

    puts_("DIRTYPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
