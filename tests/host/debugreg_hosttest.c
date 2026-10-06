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
 * posted before its thread checks pending signals or returns, including on
 * another processor. A user hit delayed across system-call entry must
 * interrupt a wait without being posted, then be forced once at the syscall
 * return even when the wait restores a mask that blocks SIGTRAP. Intel's
 * delayed hit has no DR6 status bit; delivery must not invent one.
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
static struct k_sigaction actions[KSIG_MAX + 1];
static int failures, signals, forced;
static int32_t signal_code;

#define DR_READ(n, v)  ((v) = hardware[host_cpu][(strtoul(n, 0, 16) - 0xc0) / 8])
#define DR_WRITE(n, v) (hardware[host_cpu][(strtoul(n, 0, 16) - 0xc0) / 8] = (v))
#include "arch/x86/debugreg.c"

/** Supply the i686 gate's address for the shared entry comparison. */
void isr128(void)
{}

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

/** Record the signal and its pending bit as signal_post_thread does.
 * A trap must belong to the running tracee. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    check(t == &tracee && t == current_proc && signo == SIGTRAP, "trap owner");
    if (!(t->sig_pending & KSIG_BIT(signo))) {
        t->sig_info[signo] = *info;
    }
    t->sig_pending |= KSIG_BIT(signo);
    signal_code = info->code;
    signals++;
}

/** Model signal_force's mask and disposition changes before posting. */
void signal_force(process_t *t, int signo, const struct k_siginfo *info)
{
    struct k_sigaction *sa = &t->sigacts[signo];

    if (sa->handler == KSIG_IGN || (t->sig_blocked & KSIG_BIT(signo))) {
        sa->handler = KSIG_DFL;
        t->sig_blocked &= ~KSIG_BIT(signo);
    }
    forced++;
    signal_post_thread(t, signo, info);
}

/** Load one write watchpoint on processor 1 and clear earlier results. */
static void setup(void)
{
    k_memset(&tracee, 0, sizeof(tracee));
    k_memset(&other, 0, sizeof(other));
    k_memset(&frame, 0, sizeof(frame));
    k_memset(actions, 0, sizeof(actions));
    k_memset(hardware, 0, sizeof(hardware));
    k_memset((void *)deferred, 0, sizeof(deferred));
    k_memset((void *)dr7_live, 0, sizeof(dr7_live));
    host_cpu = 1;
    cpu_table[host_cpu].current = &tracee;
    tracee.sigacts = actions;
    tracee.debugreg[0] = USER_ADDR_MIN;
    tracee.debugreg[7] = 0x10001;
    signals = 0;
    forced = 0;
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

/** Pending checks post hardware hits only for their owner, including after
 * migration. Detached hits and kernel single steps post no signal. */
static void pending_hits(void)
{
    setup();
    hardware[1][6] = 1;
    debugreg_defer();
    check(!arch_debug_pending(), "kernel hit does not force a wait interruption");
    arch_debug_deliver();
    check(signals == 1 && tracee.sig_pending == KSIG_BIT(SIGTRAP) &&
              tracee.sig_info[SIGTRAP].code == TRAP_HWBKPT,
          "deferred hit is posted before a wait");

    setup();
    hardware[1][6] = 1;
    debugreg_defer();
    arch_debug_switch(&tracee, &other);
    cpu_table[1].current = &other;
    arch_debug_deliver();
    check(signals == 0 && other.sig_pending == 0, "next thread posts no deferred hit");
    host_cpu = 0;
    cpu_table[0].current = &tracee;
    arch_debug_deliver();
    check(signals == 1 && tracee.sig_pending == KSIG_BIT(SIGTRAP) &&
              tracee.sig_info[SIGTRAP].code == TRAP_HWBKPT,
          "saved hit is posted after migration");

    setup();
    hardware[1][6] = 1;
    debugreg_defer();
    arch_debug_switch(&tracee, &tracee);
    hardware[1][6] = 1;
    debugreg_defer();
    detach();
    arch_debug_deliver();
    check(signals == 0 && tracee.sig_pending == 0, "detached hit posts no signal");
    tracee.debugreg[7] = 0x10001;
    arch_debug_deliver();
    check(signals == 0 && tracee.sig_pending == 0, "detached hit is cleared before rearming");

    setup();
    hardware[1][6] = 0x4000;
    debugreg_defer();
    arch_debug_deliver();
    check(signals == 0 && tracee.sig_pending == 0, "kernel step posts no signal");
}

/** A dequeue gets a deferred hit's siginfo, before and after migration.
 * Taking the pending bit must leave no second post for another wait or the
 * user return, even when the return reloads the hardware enables. */
static void dequeue_hits(void)
{
    int migrated;

    for (migrated = 0; migrated <= 1; migrated++) {
        setup();
        tracee.sig_blocked = KSIG_BIT(SIGTRAP);
        FRAME_PC(&frame) = USER_ADDR_MIN + 32;
        hardware[1][6] = 1;
        debugreg_defer();
        if (migrated) {
            arch_debug_switch(&tracee, &other);
            cpu_table[1].current = &other;
            host_cpu = 0;
            cpu_table[0].current = &tracee;
        }
        arch_debug_deliver();
        check(signals == 1 && tracee.sig_pending == KSIG_BIT(SIGTRAP) &&
                  tracee.sig_info[SIGTRAP].code == TRAP_HWBKPT &&
                  tracee.sig_info[SIGTRAP].addr == FRAME_PC(&frame) &&
                  tracee.sig_blocked == KSIG_BIT(SIGTRAP),
              "deferred hit is posted for dequeue with siginfo");
        tracee.sig_pending &= ~KSIG_BIT(SIGTRAP);
        arch_debug_deliver();
        debugreg_deliver();
        debugreg_resume();
        debugreg_deliver();
        check(signals == 1 && tracee.sig_pending == 0,
              "dequeued hit is not posted again at user return");
    }
}

/** Put a delayed user hit at the system-call entry, then leave it on this
 * processor (location 0), move it to the thread by a switch (1), or add a
 * kernel hit beside it (2). */
static void place_user_hit(unsigned long status, int location)
{
    hardware[1][6] = 0xffff0ff0ul | status;
    debugreg_defer_user((uintptr_t)&isr128);
    if (location == 1) {
        arch_debug_switch(&tracee, &other);
        cpu_table[1].current = &other;
        check(!arch_debug_pending(), "next thread has no forced wait interruption");
        arch_debug_deliver();
        check(signals == 0, "next thread does not post delayed user hit");
        host_cpu = 0;
        cpu_table[0].current = &tracee;
    } else if (location == 2) {
        arch_debug_switch(&tracee, &tracee);
        hardware[1][6] = 2;
        debugreg_defer();
    }
}

/** The user return after a wait: the hit is forced once, through the mask
 * the wait restored, with its status and the interrupted call's frame. */
static void return_user_hit(unsigned long status, int location)
{
    debugreg_deliver();
    check(forced == 1 && signals == 1 && tracee.sig_pending == KSIG_BIT(SIGTRAP) &&
              tracee.sig_blocked == KSIG_BIT(SIGUSR1) && actions[SIGTRAP].handler == KSIG_DFL,
          status ? "user return forces delayed hit after wait restores mask"
                 : "user return forces status-less hit after wait restores mask");
    check(tracee.debugreg[6] == (status | (location == 2 ? 2ul : 0ul)) &&
              tracee.sig_info[SIGTRAP].code == TRAP_HWBKPT &&
              tracee.sig_info[SIGTRAP].addr == USER_ADDR_MIN + 32 &&
              FRAME_PC(&frame) == USER_ADDR_MIN + 32 && SC_RET(&frame) == -EINTR,
          status ? "user return keeps delayed status and interrupted syscall frame"
                 : "user return keeps status-less hit and interrupted syscall frame");
    check(!arch_debug_pending(), "delivered user hit no longer interrupts wait");
    tracee.sig_pending = 0;
    arch_debug_deliver();
    debugreg_resume();
    debugreg_deliver();
    check(forced == 1 && signals == 1 && tracee.sig_pending == 0,
          status ? "user hit checked during wait is forced only once"
                 : "status-less user hit checked during wait is forced only once");
}

/** A wait whose set holds SIGTRAP or not: the pending check neither posts
 * the hit nor changes the mask, yet reports it as interrupting. */
static void wait_user_hit(unsigned long status, int location, int in_set)
{
    unsigned long cpu_pending, thread_pending;
    uint64_t saved = KSIG_BIT(SIGTRAP) | KSIG_BIT(SIGUSR1);
    uint64_t wait_set = in_set ? KSIG_BIT(SIGTRAP) : KSIG_BIT(SIGUSR1);

    setup();
    tracee.sig_blocked = saved;
    actions[SIGTRAP].handler = KSIG_IGN;
    FRAME_PC(&frame) = USER_ADDR_MIN + 32;
    SC_RET(&frame) = -EINTR;
    tracee.debugreg[6] = 0xf;
    place_user_hit(status, location);
    cpu_pending = deferred[host_cpu];
    thread_pending = tracee.debug_pending;
    tracee.sig_blocked = saved & ~wait_set;
    arch_debug_deliver();
    arch_debug_deliver();
    check(signals == 0 && forced == 0 && tracee.sig_pending == 0 &&
              tracee.sig_blocked == (saved & ~wait_set) && actions[SIGTRAP].handler == KSIG_IGN,
          status ? "pending check keeps delayed user hit out of signal waits"
                 : "pending check keeps status-less user hit out of signal waits");
    check(arch_debug_pending() == 1,
          status ? "delayed user hit interrupts wait regardless of mask and disposition"
                 : "status-less user hit interrupts wait regardless of mask and disposition");
    check(deferred[host_cpu] == cpu_pending && tracee.debug_pending == thread_pending,
          "pending check preserves both copies of delayed user status");
    tracee.sig_blocked = saved;
    return_user_hit(status, location);
}

/** A pending check cannot post or consume a user hit, even for a signal
 * wait that unblocks SIGTRAP. It interrupts with either wait set, leaving
 * user return to force the trap once after the saved mask comes back.
 * Status is B0 or zero, for a hit with or without a DR6 status bit. */
static void pending_user_hits(unsigned long status)
{
    int location, in_set;

    for (location = 0; location < 3; location++) {
        for (in_set = 0; in_set <= 1; in_set++) {
            wait_user_hit(status, location, in_set);
        }
    }

    setup();
    hardware[1][6] = 0xffff0ff0ul | status;
    debugreg_defer_user((uintptr_t)&isr128);
    cpu_table[1].current = NULL;
    check(!arch_debug_pending(), "no current thread means no forced wait interruption");
    arch_debug_deliver();
    check(signals == 0 && deferred[1] == 0, "pending check without thread clears deferred hit");
}

/** A delayed user hit stops before the next user instruction even with
 * SIGTRAP blocked, as musl's raise leaves it when a tracer redirects the PC.
 * Migration keeps that origin, delivery consumes it, and detach cancels it.
 * Status is B0 or zero, for a hit with or without a DR6 status bit.
 * The existing dequeue rows check that kernel-copy hits keep their mask. */
static void delayed_user_hits(unsigned long status)
{
    int migrated;

    for (migrated = 0; migrated <= 1; migrated++) {
        setup();
        tracee.sig_blocked = KSIG_BIT(SIGTRAP) | KSIG_BIT(SIGUSR1);
        actions[SIGTRAP].handler = KSIG_IGN;
        FRAME_PC(&frame) = USER_ADDR_MIN + 32;
        SC_RET(&frame) = 2;
        tracee.debugreg[6] = 0xf;
        hardware[1][6] = 0xffff0ff0ul | status;
        debugreg_defer_user((uintptr_t)&isr128);
        check(signals == 0 && hardware[1][6] == 0 && (hardware[1][7] & 0xff) == 0,
              "delayed user hit only defers and disables on interrupt stack");
        if (migrated) {
            arch_debug_switch(&tracee, &other);
            cpu_table[1].current = &other;
            debugreg_deliver();
            check(signals == 0, "next thread does not force delayed user hit");
            host_cpu = 0;
            cpu_table[0].current = &tracee;
        }
        debugreg_deliver();
        check(forced == 1 && signals == 1 &&
                  (tracee.sig_pending & ~tracee.sig_blocked) == KSIG_BIT(SIGTRAP) &&
                  tracee.sig_blocked == KSIG_BIT(SIGUSR1) && actions[SIGTRAP].handler == KSIG_DFL,
              status ? "delayed user hit is deliverable at syscall return"
                     : "status-less user hit is deliverable at syscall return");
        check(tracee.debugreg[6] == status && tracee.sig_info[SIGTRAP].code == TRAP_HWBKPT &&
                  tracee.sig_info[SIGTRAP].addr == USER_ADDR_MIN + 32 &&
                  FRAME_PC(&frame) == USER_ADDR_MIN + 32 && SC_RET(&frame) == 2,
              status ? "delayed user hit keeps status and syscall frame"
                     : "status-less user hit keeps status and syscall frame");
        tracee.sig_pending = 0;
        debugreg_resume();
        debugreg_deliver();
        check(forced == 1 && signals == 1 && tracee.sig_pending == 0,
              status ? "delayed user hit is forced only once"
                     : "status-less user hit is forced only once");
        tracee.sig_blocked = KSIG_BIT(SIGTRAP);
        hardware[host_cpu][6] = 1;
        debugreg_defer();
        debugreg_deliver();
        check(forced == 1 && tracee.sig_blocked == KSIG_BIT(SIGTRAP),
              "later kernel hit does not inherit user origin");
    }

    setup();
    tracee.sig_blocked = KSIG_BIT(SIGTRAP);
    hardware[1][6] = 0xffff0ff0ul | status;
    debugreg_defer_user((uintptr_t)&isr128);
    arch_debug_switch(&tracee, &tracee);
    hardware[1][6] = 0xffff0ff0ul | status;
    debugreg_defer_user((uintptr_t)&isr128);
    detach();
    check(!arch_debug_pending(), "detached user hit does not interrupt wait");
    arch_debug_deliver();
    check(deferred[1] == 0 && tracee.debug_pending == 0,
          "pending check clears detached user hit from both copies");
    debugreg_deliver();
    check(forced == 0 && signals == 0 && tracee.sig_blocked == KSIG_BIT(SIGTRAP),
          status ? "detach cancels delayed user hit without changing mask"
                 : "detach cancels status-less user hit without changing mask");
    tracee.debugreg[7] = 0x10001;
    check(!arch_debug_pending(), "rearming does not revive detached user hit");
}

/** Only the gate's first instruction identifies a delayed user hit.
 * An adjacent address must leave DR6 for the ordinary kernel-hit path.
 * Status is B0 or zero, for a hit with or without a DR6 status bit. */
static void syscall_entry_address(unsigned long status)
{
    uintptr_t entry = (uintptr_t)&isr128;

    setup();
    tracee.sig_blocked = KSIG_BIT(SIGTRAP);
    hardware[1][6] = 0xffff0ff0ul | status;
    check(debugreg_defer_user(entry - 1) == 0 && debugreg_defer_user(entry + 1) == 0 &&
              hardware[1][6] == (0xffff0ff0ul | status) && hardware[1][7] == 0x10401 &&
              deferred[1] == 0 && signals == 0,
          status ? "adjacent gate addresses leave kernel hit unchanged"
                 : "adjacent gate addresses leave status-less exception unchanged");
    debugreg_defer();
    debugreg_deliver();
    check(forced == 0 && signals == (status ? 1 : 0) && tracee.sig_blocked == KSIG_BIT(SIGTRAP),
          "non-entry exception retains kernel-copy delivery");
    setup();
    tracee.sig_blocked = KSIG_BIT(SIGTRAP);
    hardware[1][6] = 0xffff0ff0ul | status;
    check(debugreg_defer_user(entry) == 1 && hardware[1][6] == 0 && (hardware[1][7] & 0xff) == 0 &&
              signals == 0,
          "exact gate entry defers user hit");
    debugreg_deliver();
    check(forced == 1 && signals == 1 && tracee.sig_blocked == 0,
          "exact gate entry forces blocked trap");
}

/** A status-less entry exception with no saved enables has no user hit.
 * Rearming after it cannot create a hit, and no thread means no owner. */
static void stale_statusless_entry(void)
{
    setup();
    tracee.sig_blocked = KSIG_BIT(SIGTRAP);
    actions[SIGTRAP].handler = KSIG_IGN;
    detach();
    hardware[1][6] = 0xffff0ff0ul;
    debugreg_defer_user((uintptr_t)&isr128);
    check(deferred[1] == 0 && !arch_debug_pending(), "stale status-less entry records no user hit");
    debugreg_deliver();
    check(forced == 0 && signals == 0 && tracee.sig_blocked == KSIG_BIT(SIGTRAP) &&
              actions[SIGTRAP].handler == KSIG_IGN,
          "stale status-less entry posts no signal");
    tracee.debugreg[7] = 0x10001;
    arch_debug_deliver();
    debugreg_deliver();
    check(forced == 0 && signals == 0 && !arch_debug_pending(),
          "rearming after status-less entry creates no hit");

    setup();
    cpu_table[1].current = NULL;
    hardware[1][6] = 0xffff0ff0ul;
    debugreg_defer_user((uintptr_t)&isr128);
    check(deferred[1] == 0 && !arch_debug_pending(),
          "status-less entry without thread records no user hit");
    debugreg_deliver();
    check(forced == 0 && signals == 0, "status-less entry without thread posts no signal");
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
    pending_hits();
    dequeue_hits();
    pending_user_hits(1);
    pending_user_hits(0);
    delayed_user_hits(1);
    delayed_user_hits(0);
    syscall_entry_address(1);
    syscall_entry_address(0);
    stale_statusless_entry();
    printf("debugreg-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}
