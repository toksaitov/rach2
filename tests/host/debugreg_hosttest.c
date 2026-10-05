/**
 * Check debug-register ownership with two processors and a deferred trap.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_CPU
 *
 * Only the privileged register instructions are replaced. A tracer clears
 * the running tracee's saved registers, as ptrace_detach does; its processor
 * must clear its hardware enables on a switch or suppress a stale hit. A
 * kernel hit is deferred without posting a signal, saved on a switch, and
 * delivered only when its thread returns, including on another processor.
 */
#include "arch/x86/debugreg.h"
#include "proc/percpu.h"

extern int printf(const char *format, ...);
extern unsigned long strtoul(const char *text, char **end, int base);

unsigned int host_cpu;
struct percpu cpu_table[NR_CPUS];
static unsigned long hardware[NR_CPUS][8];
static process_t tracee, other;
static isr_params_t frame;
static int failures, signals;
static int32_t signal_code;

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

/** Supply the user frame whose address a deferred trap reports. */
isr_params_t *proc_user_frame(const process_t *t)
{
    (void)t;
    return &frame;
}

/** Record the signal; a trap must belong to the running tracee. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    check(t == &tracee && t == current_proc && signo == SIGTRAP, "trap owner");
    signal_code = info->code;
    signals++;
}

/** Load one write watchpoint on processor 1 and clear earlier results. */
static void setup(void)
{
    k_memset(&tracee, 0, sizeof(tracee));
    k_memset(&other, 0, sizeof(other));
    k_memset(hardware, 0, sizeof(hardware));
    k_memset((void *)deferred, 0, sizeof(deferred));
    k_memset((void *)dr7_live, 0, sizeof(dr7_live));
    host_cpu = 1;
    cpu_table[host_cpu].current = &tracee;
    tracee.debugreg[0] = USER_ADDR_MIN;
    tracee.debugreg[7] = 0x10001;
    signals = 0;
    arch_debug_switch(NULL, &tracee);
    check(hardware[1][0] == USER_ADDR_MIN && hardware[1][7] == 0x10401, "loaded registers");
}

/** Clear the saved enables as a tracer's exit on processor 0 does. */
static void detach(void)
{
    host_cpu = 0;
    k_memset(tracee.debugreg, 0, sizeof(tracee.debugreg));
    host_cpu = 1;
}

/** Exercise the two stale-register paths and deferred exception ownership. */
int main(void)
{
    setup();
    detach();
    arch_debug_switch(&tracee, &other);
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
    debugreg_defer();
    check(signals == 0 && hardware[1][6] == 0 && (hardware[1][7] & 0xff) == 0,
          "kernel trap only defers and disables");
    arch_debug_switch(&tracee, &other);
    cpu_table[1].current = &other;
    debugreg_deliver();
    check(signals == 0, "deferred trap does not reach next thread");
    host_cpu = 0;
    cpu_table[0].current = &tracee;
    debugreg_deliver();
    check(signals == 1 && signal_code == TRAP_HWBKPT && tracee.debugreg[6] == 1,
          "deferred trap follows thread to another processor");
    debugreg_deliver();
    check(signals == 1, "deferred trap delivered once");
    debugreg_resume();
    check(hardware[0][7] == 0x10401, "return reloads deferred enables");

    setup();
    hardware[1][6] = 1;
    debugreg_defer();
    detach();
    debugreg_deliver();
    check(signals == 0, "detach cancels deferred hardware trap");
    debugreg_resume();
    check((hardware[1][7] & 0xff) == 0, "detached return keeps enables clear");

    setup();
    hardware[1][6] = 0x4000;
    debugreg_defer();
    debugreg_deliver();
    check(signals == 0, "kernel single step does not duplicate syscall exit stop");
    printf("debugreg-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}
