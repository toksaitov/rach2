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
 * exits 1 otherwise. Writes etude's report records (probe_report.h) for the
 * case ETUDE_CASE names, an assertion for each step `steps` lists. The child
 * cannot write them, since only the process that began the report may, so it
 * exits with the number of the step that failed, and the parent reports it;
 * a failure leaves the steps after it not run. Freestanding; direct system
 * calls (probe.h), the 32-bit id calls on i386.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

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

/* The checks, one assertion each, in the order the probe makes them: the
 * first two in the parent, the rest in the child. */
enum {
    ROOT,
    FORK,
    SETGROUPS,
    GETGROUPS,
    GETGROUPS_COUNT,
    GETGROUPS_SHORT,
    SETGROUPS_OVERSIZE,
    EXCURSION_OUT,
    SAVED_OUT,
    PRIVILEGE,
    EXCURSION_BACK,
    SAVED_BACK,
    RESGID_TRIPLE,
    RESGID_DROP,
    RESUID_PARK,
    REUID_FROM_SAVED,
    RESUID_FROM_REAL,
    REUID_FROM_EFFECTIVE,
    WALK_TRIPLE,
    SETUID_ZERO,
    SETREUID_ZERO,
    SETRESUID_ZERO,
    SETGROUPS_DROPPED,
    SETUID_OWN,
    GRANDCHILD_FORK,
    INHERIT,
    STEPS
};

/* Each check's assertion and the step that `fail` prints. */
static const struct {
    const char *id;
    const char *text;
} steps[STEPS] = {
    {"root", "boot identity not root"},
    {"fork", "fork"},
    {"setgroups", "setgroups as root"},
    {"getgroups", "getgroups round trip"},
    {"getgroups-count", "getgroups count query"},
    {"getgroups-short", "getgroups short list not EINVAL"},
    {"setgroups-oversize", "setgroups oversize not EINVAL"},
    {"excursion-out", "effective-id excursion out"},
    {"saved-out", "saved id after excursion out"},
    {"privilege", "privilege not keyed on effective id"},
    {"excursion-back", "effective-id excursion back"},
    {"saved-back", "saved id after excursion back"},
    {"resgid-triple", "distinct setresgid triple"},
    {"resgid-drop", "setresgid drop"},
    {"resuid-park", "distinct setresuid park"},
    {"reuid-from-saved", "setreuid real from saved not EPERM"},
    {"resuid-from-real", "setresuid saved from real"},
    {"reuid-from-effective", "setreuid real from effective"},
    {"walk-triple", "triple after unprivileged walk"},
    {"setuid-zero", "setuid 0 after drop"},
    {"setreuid-zero", "setreuid 0 after drop"},
    {"setresuid-zero", "setresuid 0 after drop"},
    {"setgroups-dropped", "setgroups after drop"},
    {"setuid-own", "setuid to own ruid"},
    {"grandchild-fork", "grandchild fork"},
    {"inherit", "grandchild inheritance"},
};

/** Print "CREDPROBE FAIL: " and the text of the check `step`, and exit: the
 * parent fails that check and exits 1, and the child exits with `step`, for
 * the parent to report. */
static void fail(int step)
{
    puts_("CREDPROBE FAIL: ");
    puts_(steps[step].text);
    puts_("\n");
    if (report_getpid() != report_owner) {
        syscall3(SYS_EXIT, step, 0, 0);
    }
    report_stop(steps[step].text);
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
        fail(SETGROUPS);
    }
    if (syscall3(SYS_GETGROUPS, 2, (long)back, 0) != 2 || back[0] != id_1000 ||
        back[1] != id_2000) {
        fail(GETGROUPS);
    }
    if (syscall3(SYS_GETGROUPS, 0, 0, 0) != 2) {
        fail(GETGROUPS_COUNT);
    }
    if (syscall3(SYS_GETGROUPS, 1, (long)back, 0) != -EINVAL) {
        fail(GETGROUPS_SHORT);
    }
    if (syscall3(SYS_SETGROUPS, 9, (long)groups, 0) != -EINVAL) {
        fail(SETGROUPS_OVERSIZE);
    }

    /* The excursion by setreuid, out and back, with the saved id. */
    if (syscall3(SYS_SETREUID, (int)KEEP, id_1000, 0) != 0 ||
        syscall3(SYS_GETEUID, 0, 0, 0) != (long)id_1000 || syscall3(SYS_GETUID, 0, 0, 0) != 0) {
        fail(EXCURSION_OUT);
    }
    if (!resuid_is(0, id_1000, id_1000)) {
        fail(SAVED_OUT);
    }
    /* Privilege follows the effective id, id_1000 here. */
    if (syscall3(SYS_SETGROUPS, 1, (long)groups, 0) != -EPERM) {
        fail(PRIVILEGE);
    }
    if (syscall3(SYS_SETREUID, (int)KEEP, 0, 0) != 0 || syscall3(SYS_GETEUID, 0, 0, 0) != 0) {
        fail(EXCURSION_BACK);
    }
    if (!resuid_is(0, 0, id_1000)) {
        fail(SAVED_BACK);
    }

    /* A distinct triple. */
    if (syscall3(SYS_SETRESGID, id_1000, id_2000, id_3000) != 0 ||
        syscall3(SYS_GETRESGID, (long)&back[0], (long)&back[1], (long)&back[2]) != 0 ||
        back[0] != id_1000 || back[1] != id_2000 || back[2] != id_3000) {
        fail(RESGID_TRIPLE);
    }

    /* The drop: the gids first, since without root setgid is refused. */
    if (syscall3(SYS_SETRESGID, id_1000, id_1000, id_1000) != 0) {
        fail(RESGID_DROP);
    }
    /* A distinct uid triple, then the unprivileged rules of each setter. */
    if (syscall3(SYS_SETRESUID, id_1000, id_1001, id_1002) != 0) {
        fail(RESUID_PARK);
    }
    if (syscall3(SYS_SETREUID, id_1002, (int)KEEP, 0) != -EPERM) {
        fail(REUID_FROM_SAVED);
    }
    if (syscall3(SYS_SETRESUID, (int)KEEP, (int)KEEP, id_1000) != 0) {
        fail(RESUID_FROM_REAL);
    }
    if (syscall3(SYS_SETREUID, id_1001, (int)KEEP, 0) != 0) {
        fail(REUID_FROM_EFFECTIVE);
    }
    if (!resuid_is(id_1001, id_1001, id_1001)) {
        fail(WALK_TRIPLE);
    }

    /* A shed identity cannot be regained, by any setter. */
    if (syscall3(SYS_SETUID, 0, 0, 0) != -EPERM) {
        fail(SETUID_ZERO);
    }
    if (syscall3(SYS_SETREUID, (int)KEEP, 0, 0) != -EPERM) {
        fail(SETREUID_ZERO);
    }
    if (syscall3(SYS_SETRESUID, 0, (int)KEEP, (int)KEEP) != -EPERM) {
        fail(SETRESUID_ZERO);
    }
    if (syscall3(SYS_SETGROUPS, 0, 0, 0) != -EPERM) {
        fail(SETGROUPS_DROPPED);
    }

    /* Setting an id the process already holds succeeds. */
    if (syscall3(SYS_SETUID, id_1001, 0, 0) != 0) {
        fail(SETUID_OWN);
    }

    /* The dropped identity is inherited across fork. */
    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail(GRANDCHILD_FORK);
    }
    if (pid == 0) {
        syscall3(SYS_EXIT, syscall3(SYS_GETUID, 0, 0, 0) == (long)id_1001 ? 0 : 1, 0, 0);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail(INHERIT);
    }

    syscall3(SYS_EXIT, 0, 0, 0);
}

/** Report the child's checks from its wait `status`: those before the check
 * it exited with held, and that check failed. A child that ended otherwise
 * fails its first check as the "child walk". */
static void report_child(int status)
{
    int code = (status >> 8) & 0xFF, i;

    if ((status & 0x7F) != 0 || code <= FORK || code >= STEPS) {
        return;
    }
    for (i = FORK + 1; i < code; i++) {
        report_held();
    }
    report_stop(steps[code].text);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: root at the start, then
 * `child_main` in a child. */
void probe_main(unsigned long *sp)
{
    int pid, status, i;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.credprobe"));
    for (i = 0; i < STEPS; i++) {
        report_plan(steps[i].id);
    }
    id_1000 = (u32)probe_id(1000);
    id_1001 = (u32)probe_id(1001);
    id_1002 = (u32)probe_id(1002);
    id_2000 = (u32)probe_id(2000);
    id_3000 = (u32)probe_id(3000);

    if (syscall3(SYS_GETUID, 0, 0, 0) != 0 || syscall3(SYS_GETEUID, 0, 0, 0) != 0) {
        fail(ROOT);
    }
    report_held();

    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail(FORK);
    }
    if (pid == 0) {
        child_main();
    }
    report_held();
    status = -1;
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        report_child(status);
        puts_("CREDPROBE FAIL: child walk\n");
        report_stop("child walk");
        syscall3(SYS_EXIT, 1, 0, 0);
    }
    for (i = FORK + 1; i < STEPS; i++) {
        report_held();
    }
    report_end();

    puts_("CREDPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
