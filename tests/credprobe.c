/**
 * Verify the process credential model: the V7 rules with POSIX saved IDs.
 *
 * The probe starts as root (every process is, until something drops). A child
 * walks the id lifecycle a login server performs: set the supplementary
 * groups, make the temporary effective-id excursion and return (the
 * authorized_keys idiom), drop the gid then the uid for good, and then prove
 * the shed identity cannot be regained by any setter. A grandchild proves the
 * dropped credentials are inherited across fork. The parent stays root so the
 * kernel shell gets its console back unchanged.
 *
 * Prints "CREDPROBE OK" and exits 0 on success; prints the failing step and
 * exits 1 otherwise. Freestanding; direct system calls (probe.h), the i386 *32
 * id variants.
 */

#include "probe.h"

typedef unsigned int u32;

#define STDOUT 1
#define EPERM  1
#define EINVAL 22
#define KEEP   0xFFFFFFFFu

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
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

static void fail(const char *step)
{
    puts_("CREDPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall3(SYS_EXIT, 1, 0, 0);
}

/* The dropped-identity walk, run in a child so the parent stays root. */
static void child_main(void)
{
    u32 groups[2];
    u32 back[3];
    int pid, status;

    /* Root sets the supplementary list and reads it back. */
    groups[0] = 1000;
    groups[1] = 2000;
    if (syscall3(SYS_SETGROUPS, 2, (long)groups, 0) != 0) {
        fail("setgroups as root");
    }
    if (syscall3(SYS_GETGROUPS, 2, (long)back, 0) != 2 || back[0] != 1000 || back[1] != 2000) {
        fail("getgroups round trip");
    }
    if (syscall3(SYS_GETGROUPS, 0, 0, 0) != 2) {
        fail("getgroups count query");
    }
    if (syscall3(SYS_GETGROUPS, 1, (long)back, 0) != -EINVAL) {
        fail("getgroups short list not EINVAL");
    }
    if (syscall3(SYS_SETGROUPS, 9, (long)groups, 0) != -EINVAL) {
        fail("setgroups oversize not EINVAL");
    }

    /* The login-server excursion by raw setreuid, which pins the saved-id
     * rule: setting the effective id away from the real id saves the new
     * effective id, and restoring it back to the real id saves nothing. */
    if (syscall3(SYS_SETREUID, (int)KEEP, 1000, 0) != 0 || syscall3(SYS_GETEUID, 0, 0, 0) != 1000 ||
        syscall3(SYS_GETUID, 0, 0, 0) != 0) {
        fail("effective-id excursion out");
    }
    if (syscall3(SYS_GETRESUID, (long)&back[0], (long)&back[1], (long)&back[2]) != 0 ||
        back[0] != 0 || back[1] != 1000 || back[2] != 1000) {
        fail("saved id after excursion out");
    }
    /* Privilege keys on the effective id: with the real id still 0 but the
     * effective id out at 1000, a privileged-only call must refuse. */
    if (syscall3(SYS_SETGROUPS, 1, (long)groups, 0) != -EPERM) {
        fail("privilege not keyed on effective id");
    }
    if (syscall3(SYS_SETREUID, (int)KEEP, 0, 0) != 0 || syscall3(SYS_GETEUID, 0, 0, 0) != 0) {
        fail("effective-id excursion back");
    }
    if (syscall3(SYS_GETRESUID, (long)&back[0], (long)&back[1], (long)&back[2]) != 0 ||
        back[0] != 0 || back[1] != 0 || back[2] != 1000) {
        fail("saved id after excursion back");
    }

    /* A distinct triple, so a getter that swapped or duplicated fields
     * shows. */
    if (syscall3(SYS_SETRESGID, 1000, 2000, 3000) != 0 ||
        syscall3(SYS_GETRESGID, (long)&back[0], (long)&back[1], (long)&back[2]) != 0 ||
        back[0] != 1000 || back[1] != 2000 || back[2] != 3000) {
        fail("distinct setresgid triple");
    }

    /* The real drop: gid first (once the uid goes, setgid is unprivileged),
     * then all three uids. */
    if (syscall3(SYS_SETRESGID, 1000, 1000, 1000) != 0) {
        fail("setresgid drop");
    }
    /* Park in a distinct uid triple, then pin the unprivileged permitted
     * sets: setreuid draws the real id from {real, effective} only, never
     * saved, while setresuid draws each id from all three. */
    if (syscall3(SYS_SETRESUID, 1000, 1001, 1002) != 0) {
        fail("distinct setresuid park");
    }
    if (syscall3(SYS_SETREUID, 1002, (int)KEEP, 0) != -EPERM) {
        fail("setreuid real from saved not EPERM");
    }
    if (syscall3(SYS_SETRESUID, (int)KEEP, (int)KEEP, 1000) != 0) {
        fail("setresuid saved from real");
    }
    if (syscall3(SYS_SETREUID, 1001, (int)KEEP, 0) != 0) {
        fail("setreuid real from effective");
    }
    if (syscall3(SYS_GETRESUID, (long)&back[0], (long)&back[1], (long)&back[2]) != 0 ||
        back[0] != 1001 || back[1] != 1001 || back[2] != 1001) {
        fail("triple after unprivileged walk");
    }

    /* A shed identity cannot be regained, by any setter. */
    if (syscall3(SYS_SETUID, 0, 0, 0) != -EPERM) {
        fail("setuid 0 after drop");
    }
    if (syscall3(SYS_SETREUID, (int)KEEP, 0, 0) != -EPERM) {
        fail("setreuid 0 after drop");
    }
    if (syscall3(SYS_SETRESUID, 0, (int)KEEP, (int)KEEP) != -EPERM) {
        fail("setresuid 0 after drop");
    }
    if (syscall3(SYS_SETGROUPS, 0, 0, 0) != -EPERM) {
        fail("setgroups after drop");
    }

    /* Swapping to an id we already hold stays legal. */
    if (syscall3(SYS_SETUID, 1001, 0, 0) != 0) {
        fail("setuid to own ruid");
    }

    /* The dropped identity is inherited across fork. */
    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("grandchild fork");
    }
    if (pid == 0) {
        syscall3(SYS_EXIT, syscall3(SYS_GETUID, 0, 0, 0) == 1001 ? 0 : 1, 0, 0);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("grandchild inheritance");
    }

    syscall3(SYS_EXIT, 0, 0, 0);
}

PROBE_START void _start(void)
{
    int pid, status;

    if (syscall3(SYS_GETUID, 0, 0, 0) != 0 || syscall3(SYS_GETEUID, 0, 0, 0) != 0) {
        fail("boot identity not root");
    }

    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        child_main();
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("child walk");
    }

    puts_("CREDPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
