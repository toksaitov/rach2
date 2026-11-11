/**
 * Check debug-register ownership with two processors and a dropped kernel
 * trap.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * Only the privileged register instructions are replaced. A tracer clears
 * the running tracee's saved registers, as ptrace_detach does; its processor
 * must clear its hardware enables on a switch or suppress a stale hit. A
 * debug exception taken in the kernel, a watchpoint hit by a copy or delayed
 * into the system-call entry, is dropped as Linux drops it: DR6 and the
 * processor's enables are cleared, the thread's saved DR6 is kept, and the
 * return to user mode loads the enables again. debugreg.c refers to no
 * signal function, so the link proves that nothing is posted.
 */
#include "arch/x86/debugreg.h"
#include "proc/percpu.h"

extern int printf(const char *format, ...);
extern unsigned long strtoul(const char *text, char **end, int base);

unsigned int host_cpu;
struct percpu cpu_table[NR_CPUS];
/** Each processor's debug registers DR0 to DR7, by number. */
static unsigned long hardware[NR_CPUS][8];
static process_t tracee, other;
static int failures;

/* The ModR/M byte of a mov to or from DRn is 0xc0 + 8 * n. */
#define DR_READ(n, v)  ((v) = hardware[host_cpu][(strtoul(n, 0, 16) - 0xc0) / 8])
#define DR_WRITE(n, v) (hardware[host_cpu][(strtoul(n, 0, 16) - 0xc0) / 8] = (v))
#include "arch/x86/debugreg.c"

/** Record a failed row without ending the other independent rows. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("debugreg-hosttest: fail: %s\n", row);
        failures++;
    }
}

/** Load one write watchpoint on processor 1 and clear earlier results. */
static void setup(void)
{
    k_memset(&tracee, 0, sizeof(tracee));
    k_memset(&other, 0, sizeof(other));
    k_memset(hardware, 0, sizeof(hardware));
    k_memset((void *)dr7_live, 0, sizeof(dr7_live));
    host_cpu = 1;
    cpu_table[host_cpu].current = &tracee;
    tracee.debugreg[0] = USER_ADDR_MIN;
    tracee.debugreg[7] = 0x10001;
    arch_debug_switch(&tracee);
    check(hardware[1][0] == USER_ADDR_MIN && hardware[1][7] == 0x10401, "loaded registers");
}

/** Clear the saved enables as a tracer's exit on processor 0 does. */
static void detach(void)
{
    host_cpu = 0;
    k_memset(tracee.debugreg, 0, sizeof(tracee.debugreg));
    host_cpu = 1;
}

/** A kernel hit, a kernel single step and a hit at the system-call entry
 * each clear DR6 and the processor's enables and keep the thread's DR6; the
 * return to user mode loads the enables again, for the next thread after a
 * switch and on another processor after a migration. */
static void dropped_hits(void)
{
    static const unsigned long status[] = {0x1ul, 0x4000ul, 0xffff0ff1ul, 0xffff0ff0ul};
    unsigned i;

    for (i = 0; i < sizeof(status) / sizeof(status[0]); i++) {
        setup();
        tracee.debugreg[6] = 0x2;
        hardware[1][6] = status[i];
        debugreg_drop();
        check(hardware[1][6] == 0 && (hardware[1][7] & 0xff) == 0 && !debugreg_loaded(),
              "kernel trap clears DR6 and the enables");
        check(tracee.debugreg[6] == 0x2, "kernel trap keeps the thread's DR6");
        debugreg_resume();
        check(hardware[1][7] == 0x10401 && debugreg_loaded(), "return reloads the enables");
    }

    setup();
    hardware[1][6] = 1;
    debugreg_drop();
    arch_debug_switch(&other);
    cpu_table[1].current = &other;
    debugreg_resume();
    check((hardware[1][7] & 0xff) == 0, "next thread without breakpoints gets no enables");
    host_cpu = 0;
    cpu_table[0].current = &tracee;
    arch_debug_switch(&tracee);
    debugreg_resume();
    check(hardware[0][7] == 0x10401 && tracee.debugreg[6] == 0,
          "migrated thread gets its enables and no DR6");

    setup();
    hardware[1][6] = 1;
    debugreg_drop();
    detach();
    debugreg_resume();
    check((hardware[1][7] & 0xff) == 0, "detached return keeps enables clear");

    setup();
    hardware[1][6] = 1;
    check(debugreg_drop_sysenter(0) == 0 && hardware[1][6] == 1 && hardware[1][7] == 0x10401,
          "no sysenter entry outside x86-64 leaves the state unchanged");
}

/** Exercise the two stale-register paths and the dropped kernel trap. */
int main(void)
{
    setup();
    detach();
    arch_debug_switch(&other);
    check((hardware[1][7] & 0xff) == 0, "detach then switch clears hardware enables");

    setup();
    detach();
    hardware[1][6] = 1;
    check(debugreg_status() == DEBUGREG_STALE, "stale hit sends no trap");
    check(hardware[1][6] == 0 && (hardware[1][7] & 0xff) == 0, "stale hit clears registers");

    setup();
    detach();
    hardware[1][6] = 0x4001;
    check(debugreg_status() == TRAP_TRACE, "stale hit preserves simultaneous single step");

    setup();
    hardware[1][6] = 1;
    check(debugreg_status() == TRAP_HWBKPT && tracee.debugreg[6] == 1 && hardware[1][6] == 0,
          "user hit is TRAP_HWBKPT with its DR6");
    dropped_hits();
    printf("debugreg-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}
