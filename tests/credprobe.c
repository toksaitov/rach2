/**
 * credprobe: the process credential model, the V7 rules with POSIX saved ids.
 *
 * The probe starts as root, as every process does until something drops it.
 * A child goes through the ids as a login server does: it sets the
 * supplementary groups, moves its effective id away and back (as a server
 * reads authorized_keys), drops the gid and then the uid for good, and then
 * finds that no setter regains the identity it gave up. A grandchild shows
 * that the dropped ids are inherited across fork. The parent stays root, so
 * that the kernel shell gets its console back unchanged.
 *
 * The ids it takes are 1000, 1001, 1002, 2000 and 3000. Given
 * `ids=OLD:NEW,...` (probe_args.h), as a native run gives it, the probe takes
 * each NEW in place of its OLD, so that it uses no id of the machine's
 * accounts; without it the probe takes those ids themselves.
 *
 * Prints "CREDPROBE OK" and exits 0 on success; prints the failing step and
 * exits 1 otherwise. Freestanding; direct system calls (probe.h), the 32-bit
 * id calls on i386.
 */

#include "probe.h"
#include "probe_args.h"

typedef unsigned int u32;

#define STDOUT 1
#define EPERM  1
#define EINVAL 22
#define KEEP   0xFFFFFFFFu

/* The ids the probe takes: 1000, 1001, 1002, 2000 and 3000, or the values
 * `ids=` gives in their place. */
static u32 id_1000, id_1001, id_1002, id_2000, id_3000;

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

/** Print "CREDPROBE FAIL: `step`" and exit 1. */
static void fail(const char *step)
{
    puts_("CREDPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall3(SYS_EXIT, 1, 0, 0);
}

/** Return whether the real, effective and saved user ids are `r`, `e` and
 * `s`. */
static int resuid_is(u32 r, u32 e, u32 s)
{
    u32 back[3];

    return syscall3(SYS_GETRESUID, (long)&back[0], (long)&back[1], (long)&back[2]) == 0 &&
           back[0] == r && back[1] == e && back[2] == s;
}

/**
 * The ids from root to a dropped identity, run in a child so that the parent
 * stays root. Exits 0, or fails the probe.
 *
 * The excursion by setreuid checks the saved-id rule: setting the effective
 * id away from the real id saves the new effective id, and setting it back
 * to the real id saves nothing. Privilege follows the effective id, so with
 * the real id 0 and the effective id 1000 setgroups is EPERM. A distinct
 * triple shows a getter that swapped or duplicated fields. After the drop
 * the unprivileged rules hold: setreuid takes the real id only from the real
 * or effective id, never the saved, while setresuid takes each id from all
 * three.
 */
static void child_main(void)
{
    u32 groups[2];
    u32 back[3];
    int pid, status;

    /* Root sets the supplementary list and reads it back. */
    groups[0] = id_1000;
    groups[1] = id_2000;
    if (syscall3(SYS_SETGROUPS, 2, (long)groups, 0) != 0) {
        fail("setgroups as root");
    }
    if (syscall3(SYS_GETGROUPS, 2, (long)back, 0) != 2 || back[0] != id_1000 ||
        back[1] != id_2000) {
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

    /* The excursion by setreuid, out and back, with the saved id. */
    if (syscall3(SYS_SETREUID, (int)KEEP, id_1000, 0) != 0 ||
        syscall3(SYS_GETEUID, 0, 0, 0) != (long)id_1000 || syscall3(SYS_GETUID, 0, 0, 0) != 0) {
        fail("effective-id excursion out");
    }
    if (!resuid_is(0, id_1000, id_1000)) {
        fail("saved id after excursion out");
    }
    /* Privilege follows the effective id, id_1000 here. */
    if (syscall3(SYS_SETGROUPS, 1, (long)groups, 0) != -EPERM) {
        fail("privilege not keyed on effective id");
    }
    if (syscall3(SYS_SETREUID, (int)KEEP, 0, 0) != 0 || syscall3(SYS_GETEUID, 0, 0, 0) != 0) {
        fail("effective-id excursion back");
    }
    if (!resuid_is(0, 0, id_1000)) {
        fail("saved id after excursion back");
    }

    /* A distinct triple. */
    if (syscall3(SYS_SETRESGID, id_1000, id_2000, id_3000) != 0 ||
        syscall3(SYS_GETRESGID, (long)&back[0], (long)&back[1], (long)&back[2]) != 0 ||
        back[0] != id_1000 || back[1] != id_2000 || back[2] != id_3000) {
        fail("distinct setresgid triple");
    }

    /* The drop: the gids first, since without root setgid is refused. */
    if (syscall3(SYS_SETRESGID, id_1000, id_1000, id_1000) != 0) {
        fail("setresgid drop");
    }
    /* A distinct uid triple, then the unprivileged rules of each setter. */
    if (syscall3(SYS_SETRESUID, id_1000, id_1001, id_1002) != 0) {
        fail("distinct setresuid park");
    }
    if (syscall3(SYS_SETREUID, id_1002, (int)KEEP, 0) != -EPERM) {
        fail("setreuid real from saved not EPERM");
    }
    if (syscall3(SYS_SETRESUID, (int)KEEP, (int)KEEP, id_1000) != 0) {
        fail("setresuid saved from real");
    }
    if (syscall3(SYS_SETREUID, id_1001, (int)KEEP, 0) != 0) {
        fail("setreuid real from effective");
    }
    if (!resuid_is(id_1001, id_1001, id_1001)) {
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

    /* Setting an id the process already holds succeeds. */
    if (syscall3(SYS_SETUID, id_1001, 0, 0) != 0) {
        fail("setuid to own ruid");
    }

    /* The dropped identity is inherited across fork. */
    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("grandchild fork");
    }
    if (pid == 0) {
        syscall3(SYS_EXIT, syscall3(SYS_GETUID, 0, 0, 0) == (long)id_1001 ? 0 : 1, 0, 0);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("grandchild inheritance");
    }

    syscall3(SYS_EXIT, 0, 0, 0);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: root at the start, then
 * `child_main` in a child. */
void probe_main(unsigned long *sp)
{
    int pid, status;

    probe_args((int)sp[0], (char **)(sp + 1));
    id_1000 = (u32)probe_id(1000);
    id_1001 = (u32)probe_id(1001);
    id_1002 = (u32)probe_id(1002);
    id_2000 = (u32)probe_id(2000);
    id_3000 = (u32)probe_id(3000);

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
