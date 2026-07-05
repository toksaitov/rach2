/**
 * Verify file ownership: creation stamping, the umask, and the chmod/chown
 * and kill permission rules.
 *
 * A new file belongs to its creator's effective ids with the file-creation
 * mask applied; chmod is the owner's or root's; a uid change by chown is
 * root's alone while an owner may chgrp only to a group it belongs to; the fd
 * variants obey the same rules through fchmod/fchown; and kill answers EPERM,
 * not ESRCH, for a live process the sender does not own. All state the probe
 * creates is removed and the umask restored, so the shared boot stays clean
 * for the probes that follow.
 *
 * Prints "OWNPROBE OK" and exits 0 on success; prints the failing step and
 * exits 1 otherwise. Freestanding; direct int 0x80.
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
#define SYS_CHMOD       15
#define SYS_KILL        37
#define SYS_MKDIR       39
#define SYS_RMDIR       40
#define SYS_UMASK       60
#define SYS_FCHMOD      94
#define SYS_GETPPID     64
#define SYS_GETPID      20
#define SYS_STAT64      195
#define SYS_SETREUID32  203
#define SYS_SETGROUPS32 206
#define SYS_FCHOWN32    207
#define SYS_SETRESUID32 208
#define SYS_SETRESGID32 210
#define SYS_CHOWN32     212

#define STDOUT 1
#define EPERM  1
#define KEEP   0xFFFFFFFFu

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

static void fail(const char *step)
{
    puts_("OWNPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall3(SYS_EXIT, 1, 0, 0);
}

/* The stat64 fields this probe reads: mode at offset 16, uid at 24, gid at 28
 * (the i386 struct stat64 layout the kernel fills). */
static u32 stat_field(const char *path, int offset)
{
    static char st[144];

    if (syscall3(SYS_STAT64, (int)path, (int)st, 0) != 0) {
        fail("stat");
    }
    return *(u32 *)(st + offset);
}

#define ST_MODE 16
#define ST_UID  24
#define ST_GID  28

/* The identity-dependent walk runs in a child so the parent stays root. */
static void child_main(void)
{
    u32 groups[1];
    int fd;

    /* Creation applies the mask and stamps the creator: umask 027 turns 0666
     * into 0640 and 0777 into 0750, owned by the effective ids. */
    syscall3(SYS_UMASK, 027, 0, 0);
    fd = syscall3(SYS_CREAT, (int)"/own.txt", 0666, 0);
    if (fd < 0) {
        fail("creat");
    }
    if ((stat_field("/own.txt", ST_MODE) & 0777) != 0640) {
        fail("umask on creat");
    }
    if (stat_field("/own.txt", ST_UID) != 0 || stat_field("/own.txt", ST_GID) != 0) {
        fail("creation owner not root");
    }
    if (syscall3(SYS_MKDIR, (int)"/own.d", 0777, 0) != 0 ||
        (stat_field("/own.d", ST_MODE) & 0777) != 0750) {
        fail("umask on mkdir");
    }

    /* chown: a uid change is root's; the stored ids come back from stat. */
    if (syscall3(SYS_CHOWN32, (int)"/own.txt", 1000, 1000) != 0 ||
        stat_field("/own.txt", ST_UID) != 1000 || stat_field("/own.txt", ST_GID) != 1000) {
        fail("chown as root");
    }
    if (syscall3(SYS_FCHOWN32, fd, 1000, 2000) != 0 || stat_field("/own.txt", ST_GID) != 2000) {
        fail("fchown as root");
    }
    if (syscall3(SYS_CHOWN32, (int)"/own.d", 1000, 1000) != 0) {
        fail("chown dir as root");
    }

    /* Creation stamps the EFFECTIVE id: with ruid 0 and euid 1000, the new
     * file belongs to 1000, still under the 027 mask. chmod then sets 0666
     * exactly: the mask applies at creation, never to chmod. */
    if (syscall3(SYS_SETREUID32, (int)KEEP, 1000, 0) != 0) {
        fail("excursion out");
    }
    if (syscall3(SYS_CREAT, (int)"/own.d/e.txt", 0666, 0) < 0 ||
        stat_field("/own.d/e.txt", ST_UID) != 1000 ||
        (stat_field("/own.d/e.txt", ST_MODE) & 0777) != 0640) {
        fail("creation stamps euid under umask");
    }
    if (syscall3(SYS_CHMOD, (int)"/own.d/e.txt", 0666, 0) != 0 ||
        (stat_field("/own.d/e.txt", ST_MODE) & 0777) != 0666) {
        fail("chmod is not masked");
    }
    if (syscall3(SYS_SETREUID32, (int)KEEP, 0, 0) != 0) {
        fail("excursion back");
    }

    /* Drop to 1000 with supplementary group 3000 for the rules below. */
    groups[0] = 3000;
    if (syscall3(SYS_SETGROUPS32, 1, (int)groups, 0) != 0 ||
        syscall3(SYS_SETRESGID32, 1000, 1000, 1000) != 0 ||
        syscall3(SYS_SETRESUID32, 1000, 1000, 1000) != 0) {
        fail("drop to 1000");
    }

    /* chmod: the owner may; fchmod agrees; a uid change stays root's. */
    if (syscall3(SYS_CHMOD, (int)"/own.txt", 0600, 0) != 0 ||
        (stat_field("/own.txt", ST_MODE) & 0777) != 0600) {
        fail("chmod as owner");
    }
    if (syscall3(SYS_FCHMOD, fd, 0640, 0) != 0 ||
        (stat_field("/own.txt", ST_MODE) & 0777) != 0640) {
        fail("fchmod as owner");
    }
    if (syscall3(SYS_CHOWN32, (int)"/own.txt", 0, KEEP) != -EPERM) {
        fail("chown uid as non-root not EPERM");
    }
    /* chgrp: the owner, to an own group only. */
    if (syscall3(SYS_CHOWN32, (int)"/own.txt", KEEP, 3000) != 0 ||
        stat_field("/own.txt", ST_GID) != 3000) {
        fail("owner chgrp to own group");
    }
    if (syscall3(SYS_CHOWN32, (int)"/own.txt", KEEP, 4000) != -EPERM) {
        fail("owner chgrp to foreign group not EPERM");
    }
    /* A non-owner may not chmod: /hello.txt is root's and we are 1000. */
    if (syscall3(SYS_CHMOD, (int)"/hello.txt", 0777, 0) != -EPERM) {
        fail("chmod as non-owner not EPERM");
    }
    /* The fd variants agree for a directory descriptor: /own.d is ours. */
    {
        int dfd = syscall3(SYS_OPEN, (int)"/own.d", 0, 0);
        if (dfd < 0) {
            fail("open own dir");
        }
        if (syscall3(SYS_FCHMOD, dfd, 0700, 0) != 0 ||
            (stat_field("/own.d", ST_MODE) & 0777) != 0700) {
            fail("fchmod on dir fd");
        }
        syscall3(SYS_CLOSE, dfd, 0, 0);
    }

    /* kill: the probe's parent is root; a 1000 sender gets EPERM, not
     * silence, and may still probe itself. */
    if (syscall3(SYS_KILL, syscall3(SYS_GETPPID, 0, 0, 0), 0, 0) != -EPERM) {
        fail("kill root parent not EPERM");
    }
    if (syscall3(SYS_KILL, syscall3(SYS_GETPID, 0, 0, 0), 0, 0) != 0) {
        fail("kill self probe");
    }

    syscall3(SYS_CLOSE, fd, 0, 0);
    syscall3(SYS_EXIT, 0, 0, 0);
}

void _start(void)
{
    int pid, status;

    syscall3(SYS_UMASK, 022, 0, 0);
    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        child_main();
    }
    if (syscall3(SYS_WAITPID, pid, (int)&status, 0) != pid || status != 0) {
        fail("child walk");
    }

    /* The parent is still root: remove the probe's droppings and restore the
     * mask, so the shared boot stays clean for the next probe. */
    if (syscall3(SYS_UNLINK, (int)"/own.txt", 0, 0) != 0 ||
        syscall3(SYS_UNLINK, (int)"/own.d/e.txt", 0, 0) != 0 ||
        syscall3(SYS_RMDIR, (int)"/own.d", 0, 0) != 0) {
        fail("cleanup");
    }
    syscall3(SYS_UMASK, 022, 0, 0);

    puts_("OWNPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
