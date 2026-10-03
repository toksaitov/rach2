/**
 * Exercise completed connects, framebuffer ownership, the clocks, their
 * rollover and resolutions, and the sleeps and the interval timer: their
 * deadlines, masks, remainders, clamps and argument checks.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * Unused syscall exports are private so the native compiler removes their
 * hardware paths. User pointers are offsets into a checked byte array.
 */
#include "isr.h"
#include "network/socket/socket.h"
#include "fs/fd.h"
struct tty;
static struct tty *fd_tty(fd_entry_t *entry);
static void init_syscall(void);
static int32_t raw_seek_resolve(uint64_t cur, uint64_t size, long long delta, int whence,
                                uint64_t *out);
static int pgrp_in_session(int pgid, int sid);
static int user_access_ok(uintptr_t addr, size_t len);
static int user_buffer_ok(uintptr_t addr, size_t len, int write);
static long sys_reboot(isr_params_t *params);
static long sys_clock_settime(isr_params_t *params);
static long sys_clock_settime64(isr_params_t *params);
static long sys_adjtimex(isr_params_t *params);
static long sys_clock_adjtime(isr_params_t *params);
static long sys_clock_adjtime64(isr_params_t *params);
static long sys_settimeofday(isr_params_t *params);
static long sys_stime(isr_params_t *params);
static long sys_getrandom(isr_params_t *params);
static long sys_ioctl(isr_params_t *params);
static long sys_sysinfo(isr_params_t *params);
static long sys_syslog(isr_params_t *params);
static long sys_socketcall(isr_params_t *params);
static long sys_nanosleep(isr_params_t *params);
static long sys_clock_getres(isr_params_t *params);
static long sys_clock_nanosleep(isr_params_t *params);
static long sys_setitimer(isr_params_t *params);
static long sys_getitimer(isr_params_t *params);
static int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port,
                          const uint8_t *buf, uint16_t len);
static int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec);
static long sys_accept4(isr_params_t *params);
static long sys_bind(isr_params_t *params);
static long sys_connect(isr_params_t *params);
static long sys_getpeername(isr_params_t *params);
static long sys_getsockname(isr_params_t *params);
static long sys_getsockopt(isr_params_t *params);
static long sys_listen(isr_params_t *params);
static long sys_recvfrom(isr_params_t *params);
static long sys_recvmmsg(isr_params_t *params);
static long sys_recvmsg(isr_params_t *params);
static long sys_sendmmsg(isr_params_t *params);
static long sys_sendmsg(isr_params_t *params);
static long sys_sendto(isr_params_t *params);
static long sys_setsockopt(isr_params_t *params);
static long sys_shutdown(isr_params_t *params);
static long sys_socket(isr_params_t *params);
static long sys_socketpair(isr_params_t *params);
#include "syscall.c"
#include "syscall_net.c"
#include "ns_first.h"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

static process_t proc;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc}};

/** Every socket here is in the first network namespace. */
int socket_outside(const struct socket *s)
{
    (void)s;
    return 0;
}
volatile uint32_t kernel_ticks;
volatile uint32_t kernel_uptime_seconds;
volatile uint32_t kernel_second_ticks;
gfx_info_t gfx_current;
uint32_t net_ip_addr = 0x0a00020fu;
static struct socket sock;
static struct tcb control;
static fd_entry_t entry;
static struct open_file description;
static uint8_t user[1024];
static int failures, enters, exits, palettes, masked, saves;
static int completion, immediate, waits;
static int pending, pending_masked, blocked_masked, sleeps, cut_short;
static uint32_t blocked_until;
static int post_at_block, wake_at_deadline, tick_signals, wake_unacted;
static int state_at_unmask = -1, unblocked_unmasks;
static int halts, unmasked_halts, blocks;
static uint32_t first_blocked_until, wakeup_at_unmask;
static int unarmed_unmasks, switch_back, tick_at_save, tick_after_yield, halt_without_tick;
static int sibling_runs;

#define CHECK(c, detail)                                    \
    do {                                                    \
        if (!(c)) {                                         \
            dprintf(2, "syscall-test: fail: %s\n", detail); \
            failures++;                                     \
        }                                                   \
    } while (0)

/** Refuse every range outside the model's user memory. */
int user_range_ok(uintptr_t addr, size_t len)
{
    return addr && addr <= sizeof(user) && len <= sizeof(user) - addr;
}

/** Model mapped user pages after the range check. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    (void)write;
    return user_range_ok(addr, len);
}

/** Copy the syscall result without truncating a native pointer. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    uint32_t at = (uint32_t)(unsigned long)dst;
    if (!user_range_ok(at, len))
        return -EFAULT;
    k_memcpy(user + at, src, len);
    return 0;
}

/** Copy a socket address from the same arena. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    uint32_t at = (uint32_t)(unsigned long)src;
    if (!user_range_ok(at, len))
        return -EFAULT;
    k_memcpy(dst, user + at, len);
    return 0;
}

static void tick(void);

/**
 * Take the interrupt mask, as the clock read, the sleep and the timer do. With
 * `tick_at_save` armed, a tick comes in the moment before the mask, as the
 * timer's interrupt can.
 */
uint32_t kirq_save(void)
{
    uint32_t prior = (uint32_t)masked;

    if (tick_at_save && !masked) {
        tick_at_save = 0;
        tick();
    }
    masked = 1;
    saves++;
    return prior;
}

/**
 * Restore the interrupt state, noting the process's state and deadline when
 * it unmasks.
 */
void kirq_restore(uint32_t flags)
{
    CHECK(masked, "interrupts are masked before they are restored");
    masked = (int)flags;
    if (!masked && current_proc) {
        state_at_unmask = (int)current_proc->state;
        wakeup_at_unmask = current_proc->wakeup_tick;
    }
}

/* The real time the model reads, a distinct value to check clock selection
 * unless a row sets another. */
static int64_t real_sec = 123;
static int64_t real_nsec = 450000000;

/** Supply the model's real time. */
void get_current_time(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = real_sec;
    *nanoseconds = real_nsec;
}

/** The real time at boot, which coarse real time and a real-time deadline
 * are counted from. */
int64_t rtc_boot_epoch(void)
{
    return 1000000;
}

/** Coarse real time as the clock keeps it: the boot's epoch plus the uptime
 * at the tick. */
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = rtc_boot_epoch() + (int64_t)kernel_uptime_seconds;
    *nanoseconds = (int64_t)kernel_second_ticks * 10000000;
}

/* The PIT's place within the tick, which the model's time since boot adds. */
static int64_t mono_phase_ns;

/** Time since boot as the clock keeps it: the uptime and its ticks, with the
 * place within the tick, read with interrupts masked. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();

    *seconds = (int64_t)kernel_uptime_seconds;
    *nanoseconds = (int64_t)kernel_second_ticks * 10000000 + mono_phase_ns;
    kirq_restore(flags);
}

/** The tick count and the place within the tick, as a sleep reads them. */
void rtc_tick_phase(uint32_t *ticks, uint32_t *phase_ns)
{
    *ticks = kernel_ticks;
    *phase_ns = (uint32_t)mono_phase_ns;
}

/** The model's one other process, pid 9, whose CPU clock a caller may read. */
static process_t other;
static struct tgroup group, other_group;

/** Find the model's processes: this one and pid 9. */
/** Each process here is one thread, whose CPU time is the process's. */
void proc_cpu_time(const process_t *p, uint32_t *user, uint32_t *sys)
{
    *user = p->utime;
    *sys = p->stime;
}

process_t *get_process(uint32_t pid)
{
    if (pid == proc.pid) {
        return &proc;
    }
    return pid == 9 ? &other : NULL;
}

/** Record the actual request to enter graphics mode. */
uint8_t *fb_enter(void)
{
    enters++;
    gfx_current.mode = GFX_MODE_VGA_13H;
    return NULL;
}

/** Record the request that would restore the text display. */
void fb_exit(void)
{
    exits++;
    gfx_current.mode = GFX_MODE_NONE;
}

/** Record palette writes without accessing a hardware port. */
void fb_set_palette(const uint8_t *data)
{
    (void)data;
    palettes++;
}

/** Resolve the only socket descriptor in the model. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    return fd == 3 ? &entry : NULL;
}

/** No network interrupt runs in this model. */
uint32_t net_lock(void)
{
    return 0;
}

/** Balance the network critical section. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** Datagram allocation is not used by these stream rows. */
uint16_t udp_bind_ephemeral(struct socket *s)
{
    (void)s;
    return 0;
}

/** Start a connection with completion before or during the wait. */
int tcp_connect(struct socket *s, uint32_t ip, uint16_t port)
{
    (void)ip;
    (void)port;
    s->tcb = &control;
    control.state = immediate ? completion : TCP_SYN_SENT;
    return 0;
}

/** Deliver the final state before the caller next evaluates its predicate. */
int wq_wait_event(wait_queue_t *q, int (*ready)(void *), void *arg)
{
    (void)q;
    waits++;
    control.state = completion;
    CHECK(ready(arg), "completed handshake wakes caller");
    return ready(arg) ? 0 : -EINTR;
}

/** A FIN before scheduling must pass both the wait and the return check. */
static void connects(void)
{
    int i;
    int states[] = {TCP_ESTABLISHED, TCP_CLOSE_WAIT, TCP_CLOSED};
    sock.family = AF_INET;
    sock.type = SOCK_STREAM;
    entry.type = FD_TYPE_SOCKET;
    entry.sock = &sock;
    entry.ofd = &description;
    user[16] = AF_INET;
    user[20] = 127;
    user[23] = 1;
    for (i = 0; i < 3; i++) {
        completion = states[i];
        sock.tcb = NULL;
        CHECK(do_connect(3, 16, 16) == (i == 2 ? -ECONNREFUSED : 0), "blocking connect result");
    }
    CHECK(waits == 3, "blocking connects use wait predicate");
    description.status_flags = OFD_NONBLOCK;
    completion = TCP_CLOSE_WAIT;
    immediate = 1;
    sock.tcb = NULL;
    CHECK(do_connect(3, 16, 16) == 0, "completed nonblocking connect");
    immediate = 0;
    sock.tcb = NULL;
    CHECK(do_connect(3, 16, 16) == -EINPROGRESS, "pending nonblocking connect");
    CHECK(!connect_done(&sock), "pending handshake stays asleep");
    sock.tcb = NULL;
    CHECK(connect_done(&sock), "removed connection wakes caller");
}

/** A stale descriptor cannot alter another process's mode or palette. */
static void framebuffer(void)
{
    proc.pid = proc.tgid = 1;
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0, "acquire framebuffer");
    CHECK(enters == 1 && fb_owner_pid == 1 && gfx_is_active(), "graphics entered");
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0 && enters == 1, "mode request is idempotent");
    CHECK(framebuffer_ioctl(FB_MODE_TEXT, 0) == 0 && !fb_owner_pid, "release framebuffer");
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0, "reacquire framebuffer");
    CHECK(enters == 2 && gfx_is_active() && fb_owner_pid == 1, "graphics reentered");
    CHECK(framebuffer_ioctl(FB_MODE_TEXT, 0) == 0, "release before second owner");
    proc.pid = proc.tgid = 2;
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == 0, "second owner acquires");
    proc.pid = proc.tgid = 1;
    CHECK(framebuffer_ioctl(FB_MODE_TEXT, 0) == -EBUSY, "stale text request refused");
    CHECK(framebuffer_ioctl(FB_MODE_13H, 0) == -EBUSY, "stale graphics request refused");
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 16) == -EBUSY, "stale palette request refused");
    CHECK(fb_owner_pid == 2 && gfx_is_active() && enters == 3 && exits == 2 && !palettes,
          "second owner's display unchanged");
    proc.pid = proc.tgid = 2;
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 0) == -EFAULT, "null palette refused");
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 900) == -EFAULT, "short palette refused");
    CHECK(framebuffer_ioctl(FB_SETPALETTE, 16) == 0 && palettes == 1, "owner sets palette");
    CHECK(framebuffer_ioctl(0, 0) == -ENOTTY, "unknown framebuffer request refused");
}

/**
 * The pending state the sleep asks about, and the mask of its first question,
 * which must be on: the sleep asks at the top of each turn, masked, whether a
 * signal it acts on is pending.
 */
int signal_pending(void)
{
    if (pending_masked < 0) {
        pending_masked = masked;
    }
    return pending;
}

/**
 * Wake a blocked sleeper as the kernel does: the timer at its deadline, or
 * `signal_post` with a signal, both clearing its timed wake.
 */
static void wake(int with_signal)
{
    current_proc->wakeup_tick = 0;
    current_proc->state = PROC_READY;
    if (with_signal) {
        pending = 1;
    }
}

/** One tick of the model's clock, and the timer's wake of a due sleeper; with
 * `sibling_runs`, another thread of the process runs in the tick, which the
 * timer charges to the process. */
static void tick(void)
{
    kernel_ticks++;
    if (sibling_runs) {
        proc.utime++;
    }
    if (current_proc->state == PROC_BLOCKED && current_proc->wakeup_tick != 0 &&
        (int32_t)(kernel_ticks - current_proc->wakeup_tick) >= 0) {
        wake(0);
    }
}

/** Move the model's clock forward to `tick`, never back. */
static void clock_to(uint32_t tick)
{
    if ((int32_t)(tick - kernel_ticks) > 0) {
        kernel_ticks = tick;
    }
}

/**
 * A yield: the sleeper is BLOCKED already, and the mask lifted already. The
 * model's clock moves forward: to the tick before the deadline, with the
 * deadline's tick coming just before the sleep masks again, once for
 * `tick_after_yield`; one tick with a signal for `tick_signals`; to the
 * deadline for `wake_at_deadline`, the timer waking the sleeper there, with a
 * signal for `post_at_block`; to the tick before the deadline with a signal
 * for `post_at_block` alone; one tick, waking the sleeper with nothing
 * pending as a blocked or ignored signal does, once for `wake_unacted`; and
 * otherwise to two ticks short of the deadline, leaving the sleeper blocked
 * for the halts to carry to it. A woken sleeper is READY, or RUNNING for
 * `switch_back`, as the real `schedule` leaves it when it switches back.
 *
 * No row yields a thousand times. A sleep that does, one that returns before
 * its clock moves to a caller that sleeps again until it has, is a fault the
 * model ends by posting a signal, so that its row fails and the rest run.
 */
void schedule(void)
{
    blocked_masked = masked;
    if (state_at_unmask != PROC_BLOCKED) {
        unblocked_unmasks++;
    }
    if (wakeup_at_unmask == 0) {
        unarmed_unmasks++;
    }
    if (++sleeps == 1000) {
        CHECK(0, "a sleep ends within a thousand yields");
        pending = 1;
    }
    if (current_proc->state != PROC_BLOCKED) {
        return;
    }
    blocked_until = current_proc->wakeup_tick;
    if (blocks++ == 0) {
        first_blocked_until = blocked_until;
    }
    if (tick_after_yield) {
        tick_after_yield = 0;
        clock_to(current_proc->wakeup_tick - 1);
        tick_at_save = 1;
        return;
    }
    if (tick_signals) {
        kernel_ticks++;
        wake(1);
    } else if (wake_at_deadline) {
        clock_to(current_proc->wakeup_tick);
        wake(post_at_block);
    } else if (post_at_block) {
        clock_to(current_proc->wakeup_tick - 1);
        wake(1);
    } else if (wake_unacted) {
        wake_unacted = 0;
        kernel_ticks++;
        wake(0);
    } else {
        clock_to(current_proc->wakeup_tick - 2);
        return;
    }
    if (switch_back) {
        current_proc->state = PROC_RUNNING;
    }
}

/**
 * The masked halt: one tick of the model's clock, and the timer's wake of a
 * sleeper whose deadline that tick reaches. With `halt_without_tick` armed
 * and the deadline a tick away, the halt ends once on another interrupt, and
 * the deadline's tick comes just before the sleep masks again.
 */
void idle_wait_irq_off(void)
{
    halts++;
    if (!masked) {
        unmasked_halts++;
    }
    if (halt_without_tick && current_proc->wakeup_tick == kernel_ticks + 1) {
        halt_without_tick = 0;
        tick_at_save = 1;
        return;
    }
    tick();
}

/**
 * The tick an alarm is due at, as the kernel's `proc_alarm_at` gives it; the
 * timer is armed with interrupts masked, since the tick rearms it.
 */
uint32_t proc_alarm_at(uint32_t now, uint32_t ticks)
{
    uint32_t at = now + ticks;

    CHECK(masked, "the timer is armed with interrupts masked");
    return at != 0 ? at : 1u;
}

/** Count a sleep a signal ended early, as the kernel's own counter does. */
void process_count_sleep_cut_short(void)
{
    cut_short++;
}

/** Put a sleep of `sec` s and `nsec` ns where the call reads it. */
static void ask_sleep(isr_params_t *p, int32_t sec, int32_t nsec)
{
    struct kernel_timespec req;

    req.tv_sec = sec;
    req.tv_nsec = nsec;
    k_memcpy(user + 32, &req, sizeof(req));
    k_memset(user + 64, 0xA5, sizeof(req));
    k_memset(p, 0, sizeof(*p));
    p->ebx = 32;
    p->ecx = 64;
}

/**
 * Call the sleep, and require it to return with the mask lifted and the
 * process running with no deadline, whatever the call returned.
 */
static int32_t sleep_call(isr_params_t *p)
{
    int32_t rc = sys_nanosleep(p);

    CHECK(!masked, "the sleep returns with the mask lifted");
    CHECK(current_proc->state == PROC_RUNNING && current_proc->wakeup_tick == 0,
          "the sleep returns running with no deadline");
    return rc;
}

/** Start the model at tick `tick`, nothing pending. */
static void model_at(uint32_t tick)
{
    kernel_ticks = tick;
    pending = 0;
    post_at_block = wake_at_deadline = tick_signals = wake_unacted = 0;
    switch_back = tick_at_save = tick_after_yield = halt_without_tick = sibling_runs = 0;
    sleeps = cut_short = halts = unmasked_halts = blocks = unblocked_unmasks = 0;
    unarmed_unmasks = 0;
    blocked_until = first_blocked_until = 0;
    current_proc->state = PROC_RUNNING;
    current_proc->wakeup_tick = 0;
}

/* A word of the 0xA5 bytes a call's buffer is filled with before it runs. */
#define UNWRITTEN ((long)(~0UL / 0xFFu * 0xA5u))

/** The remainder the last call wrote. */
static struct kernel_timespec remainder(void)
{
    struct kernel_timespec rem;

    k_memcpy(&rem, user + 64, sizeof(rem));
    return rem;
}

/**
 * The sleep's pending check and its block are one masked step.
 *
 * A signal posted between the two would be recorded and not acted on, since
 * `signal_post` wakes only a process already blocked, so the sleeper would
 * wait out its whole deadline with the signal already there. What says the
 * two are one step is that the check runs masked and the process is already
 * BLOCKED when the mask is lifted before the yield. The halt after a yield
 * that ran nothing is masked too, so the tick that ends it is not lost. The
 * count the kernel keeps of the sleeps a signal cut short rises only for one
 * that ended early.
 *
 * A sleep ends at the first tick at or after its end, so one of three ticks
 * begun as tick 1000 starts blocks until 1003 and returns there, not before.
 * A signal already there leaves the whole request as the remainder.
 */
static void sleeps_masked(void)
{
    isr_params_t p;
    struct kernel_timespec rem;

    ask_sleep(&p, 0, 30000000);
    model_at(1000);
    pending_masked = blocked_masked = -1;
    state_at_unmask = -1;
    CHECK(sleep_call(&p) == 0 && kernel_ticks == 1003,
          "a sleep with no signal runs to its deadline");
    CHECK(blocked_until == 1003, "a sleep of three ticks from tick 1000 blocks until 1003");
    CHECK(pending_masked == 1, "the pending check runs with interrupts masked");
    CHECK(blocked_masked == 0, "the yield runs with the mask lifted");
    CHECK(sleeps > 0 && unblocked_unmasks == 0,
          "and the mask is lifted with the sleeper already blocked");
    CHECK(unarmed_unmasks == 0, "and with its deadline stored");
    CHECK(halts > 0 && unmasked_halts == 0, "the sleep halts with interrupts masked");
    CHECK(sleeps > 0, "the sleep yields");
    CHECK(cut_short == 0, "a sleep that ran its length is not counted as cut short");

    ask_sleep(&p, 0, 30000000);
    model_at(2000);
    pending = 1;
    CHECK(sleep_call(&p) == -EINTR, "a signal already there ends the sleep at once");
    CHECK(sleeps == 0, "with no yield");
    CHECK(cut_short == 1, "and the count of sleeps a signal cut short rises");
    rem = remainder();
    CHECK(rem.tv_sec == 0 && rem.tv_nsec == 30000000, "and the whole request is left");
}

/**
 * The sleep's edges: its last tick, its deadline, the wakes that do not end
 * it, and deadlines at the wrap.
 *
 * A sleep of three ticks from 3000 blocks until 3003. A signal as its last
 * tick begins leaves that tick as the remainder, the time to the deadline
 * less the place within the tick; a signal at 3003 ends nothing, and the
 * call returns 0. A wake by a signal the sleep does not act on, blocked or
 * ignored, which `signal_post` makes with the deadline cleared, sends it back
 * to sleep with its deadline armed again. A remainder of 199 ticks is 1 s and
 * 990 ms. A sleep resumed for its remainder after a signal at every tick ends
 * within four calls, no earlier than the first sleep's deadline. The count is
 * clamped so that the longest sleep begun at 0x80000000, 4 ms into the tick,
 * still lies ahead of it, seconds past the clamp are shortened to it rather
 * than wrapped, and a deadline that wraps to 0, the "not sleeping" value, is
 * stored as 1. A sleep of zero does not block.
 */
static void sleep_edges(void)
{
    isr_params_t p;
    struct kernel_timespec rem;
    int rc, calls;

    ask_sleep(&p, 0, 30000000);
    model_at(3000);
    post_at_block = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && rem.tv_sec == 0 && rem.tv_nsec == 10000000,
          "a signal as the last tick begins leaves that tick");
    CHECK(cut_short == 1, "and the sleep is counted as one a signal cut short");

    ask_sleep(&p, 0, 30000000);
    model_at(3000);
    post_at_block = wake_at_deadline = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == 0 && cut_short == 0 && rem.tv_sec == 0 && rem.tv_nsec == 0,
          "a signal at the deadline ends nothing");

    ask_sleep(&p, 0, 30000000);
    model_at(7000);
    wake_unacted = 1;
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocks >= 2 && blocked_until == 7003 && kernel_ticks == 7003,
          "a sleep woken by a signal it does not act on blocks again with its deadline armed");

    ask_sleep(&p, 2, 0);
    model_at(6000);
    tick_signals = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && rem.tv_sec == 1 && rem.tv_nsec == 990000000,
          "a remainder of 1.99 s is 1 s and 990 ms");

    ask_sleep(&p, 0, 30000000);
    model_at(5000);
    tick_signals = 1;
    for (calls = 1; calls <= 20; calls++) {
        pending = 0;
        current_proc->state = PROC_RUNNING;
        if (sleep_call(&p) == 0) {
            break;
        }
        k_memcpy(user + 32, user + 64, sizeof(struct kernel_timespec));
    }
    CHECK(calls <= 4 && kernel_ticks >= 5003,
          "a sleep resumed after a signal at every tick ends within four calls");

    mono_phase_ns = 4000000;
    ask_sleep(&p, 21474836, 469999999);
    model_at(0x80000000u);
    rc = sleep_call(&p);
    CHECK(rc == 0 && sleeps > 0, "the longest sleep begun at tick 0x80000000 blocks");
    mono_phase_ns = 0;

    ask_sleep(&p, 42949673, 0);
    model_at(1400);
    rc = sleep_call(&p);
    CHECK(rc == 0 && (int32_t)(first_blocked_until - 1400u) > 2000000000,
          "a sleep of 42949673 s is shortened to the limit, not wrapped to four ticks");

    ask_sleep(&p, 21474836, 469999999);
    model_at(0x80000002u);
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocked_until == 1, "a sleep whose deadline wraps to 0 blocks until 1");

    ask_sleep(&p, 0, 0);
    model_at(0);
    rc = sleep_call(&p);
    CHECK(rc == 0 && sleeps == 0, "a sleep of zero does not block");
}

/**
 * The sleep's rounding, its checks of the request, and the ticks that come
 * as it masks.
 *
 * A sleep ends at the first tick at or after its end, measured from where in
 * the tick in progress it began: one of 15 ms from the start of tick 1100
 * blocks until 1102, one of 1 ns from 1150 until 1151, and one of 1 ms begun
 * 4 ms into tick 1300 until 1301, 6 ms later, where one of 7 ms waits until
 * 1302. A negative second, a negative nanosecond count and one of a whole
 * second are refused. A sleeper that a signal wakes, and that the scheduler
 * switches back to, runs again at once rather than halting, so a sleep of
 * 30 ms begun at 8000 with a signal at 8001 leaves 20 ms. The deadline's
 * tick may come just before the sleep masks, after a yield or after a halt
 * that another interrupt ended; the sleep sees it at once, from 8100 and
 * 8200 alike.
 */
static void sleep_races(void)
{
    isr_params_t p;
    struct kernel_timespec rem;
    int rc;

    ask_sleep(&p, 0, 15000000);
    model_at(1100);
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocked_until == 1102, "a sleep of 15 ms from tick 1100 blocks until 1102");
    ask_sleep(&p, 0, 1);
    model_at(1150);
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocked_until == 1151, "a sleep of 1 ns from tick 1150 blocks until 1151");
    mono_phase_ns = 4000000;
    ask_sleep(&p, 0, 1000000);
    model_at(1300);
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocked_until == 1301,
          "a sleep of 1 ms begun 4 ms into a tick ends at the next");
    ask_sleep(&p, 0, 7000000);
    model_at(1300);
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocked_until == 1302, "and one of 7 ms at the tick after");
    mono_phase_ns = 0;

    ask_sleep(&p, -1, 0);
    model_at(1200);
    CHECK(sleep_call(&p) == -EINVAL, "a sleep of a negative second is refused");
    ask_sleep(&p, 0, 1000000000);
    CHECK(sleep_call(&p) == -EINVAL, "a sleep of 1000000000 ns is refused");
    ask_sleep(&p, 0, -1);
    CHECK(sleep_call(&p) == -EINVAL, "a sleep of -1 ns is refused");

    ask_sleep(&p, 0, 30000000);
    model_at(8000);
    switch_back = tick_signals = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && rem.tv_sec == 0 && rem.tv_nsec == 20000000,
          "a sleeper switched back to after a signal runs again at once");

    ask_sleep(&p, 0, 30000000);
    model_at(8100);
    tick_after_yield = 1;
    rc = sleep_call(&p);
    CHECK(rc == 0 && kernel_ticks == 8103,
          "a deadline tick that comes as the sleep masks after a yield is seen");

    ask_sleep(&p, 0, 30000000);
    model_at(8200);
    halt_without_tick = 1;
    rc = sleep_call(&p);
    CHECK(rc == 0 && kernel_ticks == 8203,
          "a deadline tick that comes as the sleep masks after a halt is seen");
}

/** Set the timer to `sec` s and `usec` us, then every `isec` s `iusec` us. */
static int set_timer(int32_t sec, int32_t usec, int32_t isec, int32_t iusec)
{
    isr_params_t p;
    struct kernel_itimerval set;

    k_memset(&p, 0, sizeof(p));
    k_memset(&set, 0, sizeof(set));
    set.value_sec = sec;
    set.value_usec = usec;
    set.interval_sec = isec;
    set.interval_usec = iusec;
    k_memcpy(user + 32, &set, sizeof(set));
    p.ebx = ITIMER_REAL;
    p.ecx = 32;
    return sys_setitimer(&p);
}

/** Read the timer through getitimer, as a process would. */
static struct kernel_itimerval read_timer(void)
{
    isr_params_t p;
    struct kernel_itimerval cur;

    k_memset(&p, 0, sizeof(p));
    p.ebx = ITIMER_REAL;
    p.ecx = 64;
    k_memset(user + 64, 0xFF, sizeof(cur));
    CHECK(sys_getitimer(&p) == 0, "read the timer");
    k_memcpy(&cur, user + 64, sizeof(cur));
    return cur;
}

/** Ask getitimer for timer `which`, and return what it returned. */
static int32_t get_timer(int32_t which)
{
    isr_params_t p;

    k_memset(&p, 0, sizeof(p));
    p.ebx = (uint32_t)which;
    p.ecx = 64;
    return sys_getitimer(&p);
}

/** Call alarm for `seconds` and return what it returned. */
static int32_t call_alarm(uint32_t seconds)
{
    isr_params_t p;

    k_memset(&p, 0, sizeof(p));
    p.ebx = seconds;
    return sys_alarm(&p);
}

/**
 * The timer's first SIGALRM is never early, and getitimer reads back what
 * was set.
 *
 * The tick in progress is not counted, so a timer of three ticks set at tick
 * 1000 is due at 1004, and getitimer reads it back as the three ticks set and
 * its interval. A tick before it is due, the time left is between none and a
 * tick, and it reads as one tick, since zero means disarmed. A timer of 5 ms,
 * and one of 1 us, is armed, due at the tick after next, and an interval of
 * 5 ms is a tick. A timer of 1 s reads back as 1 s. The count is clamped so
 * that the longest timer set at 0x80000001 still lies ahead of it, and
 * seconds past the clamp are shortened to it rather than wrapped. An interval
 * of a second and more reads back whole. An interval of a whole second in
 * microseconds is refused, and so is a getitimer of a timer other than the
 * three. `alarm` rounds the seconds left to the nearest, a half second
 * up, and to 1 when under a second is left, and leaves no interval.
 */
static void timer_first_expiry(void)
{
    struct kernel_itimerval cur;

    model_at(1000);
    CHECK(set_timer(0, 30000, 0, 250000) == 0 && proc.alarm_tick == 1004,
          "a timer of three ticks set at tick 1000 is due at 1004");
    cur = read_timer();
    CHECK(cur.value_sec == 0 && cur.value_usec == 30000,
          "and it reads back as the three ticks set");
    CHECK(cur.interval_sec == 0 && cur.interval_usec == 250000, "and its interval");
    model_at(1003);
    cur = read_timer();
    CHECK(cur.value_sec == 0 && cur.value_usec == 10000,
          "a timer due at the next tick reads as one tick");
    CHECK(set_timer(0, 0, 0, 0) == 0 && proc.alarm_tick == 0, "a value of zero disarms the timer");

    model_at(1200);
    CHECK(set_timer(0, 5000, 0, 0) == 0 && proc.alarm_tick == 1202,
          "a timer of 5 ms is armed, due at 1202");
    CHECK(set_timer(0, 1, 0, 0) == 0 && proc.alarm_tick == 1202,
          "a timer of 1 us is armed, due at 1202");
    CHECK(set_timer(0, 30000, 0, 5000) == 0 && read_timer().interval_usec == 10000,
          "an interval of 5 ms rounds up to a tick");
    model_at(1300);
    CHECK(set_timer(1, 0, 0, 0) == 0, "set a timer of 1 s");
    cur = read_timer();
    CHECK(cur.value_sec == 1 && cur.value_usec == 0, "a timer of 1 s reads back as 1 s");
    model_at(1400);
    CHECK(set_timer(42949673, 0, 0, 0) == 0 && (int32_t)(proc.alarm_tick - 1400u) > 2000000000,
          "a timer of 42949673 s is shortened to the limit, not wrapped to four ticks");

    model_at(0x80000001u);
    CHECK(set_timer(0x7FFFFFFF, 999999, 0, 0) == 0, "set the longest timer at tick 0x80000001");
    cur = read_timer();
    CHECK(cur.value_sec == 21474836 && cur.value_usec == 450000,
          "the longest timer set at tick 0x80000001 reads back as the longest");
    CHECK(set_timer(0, 0, 0, 0) == 0 && proc.alarm_tick == 0, "and a value of zero disarms it");

    model_at(900);
    CHECK(set_timer(1, 0, 1, 500000) == 0, "set a timer of 1 s, then every 1.5 s");
    cur = read_timer();
    CHECK(cur.interval_sec == 1 && cur.interval_usec == 500000,
          "an interval of 1.5 s reads back as 1.5 s");
    CHECK(set_timer(0, 30000, 0, 1000000) == -EINVAL, "an interval of 1000000 us is refused");
    CHECK(get_timer(3) == -EINVAL, "getitimer refuses a timer other than the three");
    CHECK(call_alarm(5) == 1, "alarm replaces the timer and reports its 1 s");
    cur = read_timer();
    CHECK(cur.interval_sec == 0 && cur.interval_usec == 0, "and leaves no interval");
    CHECK(call_alarm(0) == 5 && proc.alarm_tick == 0, "and alarm(0) disarms it");

    model_at(1000);
    CHECK(call_alarm(5) == 0, "alarm returns 0 when none was armed");
    model_at(1030);
    CHECK(call_alarm(5) == 5, "alarm reports 4.7 s left as 5");
    model_at(1528);
    CHECK(call_alarm(5) == 1, "alarm reports 20 ms left as 1, since 0 means none was armed");
    model_at(1585);
    CHECK(call_alarm(0) == 4, "alarm reports 4.43 s left as 4");
    CHECK(proc.alarm_tick == 0, "and alarm(0) leaves none");
    model_at(2000);
    CHECK(call_alarm(5) == 0, "arm an alarm of 5 s at tick 2000");
    model_at(2050);
    CHECK(call_alarm(0) == 5, "alarm reports 4.5 s left as 5");
}

/** Read coarse time since boot, which counts the uptime and its ticks here,
 * across the tick wrap and the following whole-second boundary. */
static void clocks(void)
{
    isr_params_t p;
    struct kernel_timespec ts;
    int i;
    uint32_t ticks[] = {0xfffffffeu, 0xffffffffu, 0, 3, 4};
    int32_t seconds[] = {42949672, 42949672, 42949672, 42949672, 42949673};
    int32_t nanos[] = {940000000, 950000000, 960000000, 990000000, 0};
    k_memset(&p, 0, sizeof(p));
    p.ebx = 6;
    p.ecx = 16;
    for (i = 0; i < 5; i++) {
        kernel_ticks = ticks[i];
        kernel_uptime_seconds = (uint32_t)seconds[i];
        kernel_second_ticks = (uint32_t)nanos[i] / 10000000;
        CHECK(sys_clock_gettime(&p) == 0, "read monotonic clock");
        k_memcpy(&ts, user + 16, sizeof(ts));
        CHECK(ts.tv_sec == seconds[i] && ts.tv_nsec == nanos[i], "monotonic survives tick wrap");
        CHECK(saves == i + 1 && !masked, "clock restores interrupt state");
    }
    p.ebx = 0;
    CHECK(sys_clock_gettime(&p) == 0, "read realtime clock");
    k_memcpy(&ts, user + 16, sizeof(ts));
    CHECK(ts.tv_sec == 123 && ts.tv_nsec == 450000000, "realtime unchanged");
    p.ebx = 12;
    CHECK(sys_clock_gettime(&p) == -EINVAL, "unknown clock refused");
    p.ebx = 1;
    p.ecx = 0;
    CHECK(sys_clock_gettime(&p) == -EFAULT, "null timespec refused");
}

/** Read clock `clk` into `ts`, returning what clock_gettime returned. */
static int32_t read_clock(int32_t clk, struct kernel_timespec *ts)
{
    isr_params_t p;
    int32_t rc;

    k_memset(&p, 0, sizeof(p));
    k_memset(user + 16, 0xA5, sizeof(*ts));
    p.ebx = (uint32_t)clk;
    p.ecx = 16;
    rc = sys_clock_gettime(&p);
    k_memcpy(ts, user + 16, sizeof(*ts));
    return rc;
}

/** Ask clock_getres about clock `clk`, returning its nanoseconds or the
 * error it returned. */
static int32_t clock_res(int32_t clk)
{
    isr_params_t p;
    struct kernel_timespec ts;
    int32_t rc;

    k_memset(&p, 0, sizeof(p));
    p.ebx = (uint32_t)clk;
    p.ecx = 16;
    rc = sys_clock_getres(&p);
    k_memcpy(&ts, user + 16, sizeof(ts));
    return rc != 0 ? rc : ts.tv_nsec;
}

/** Linux's CPU clock id for process or thread `id` reading time `which`. */
static int32_t cpu_clock_id(uint32_t id, int thread, uint32_t which)
{
    return (int32_t)((~id << 3) | (thread ? 4u : 0u) | which);
}

/**
 * The eight clocks and the CPU clocks by id.
 *
 * At tick 4205 of second 42, the four clocks of time since boot read 42.05 s
 * and coarse real time the boot's epoch plus that; the two CPU clocks read
 * the 250 ticks of user and 120 of system time the process was charged. A
 * process's CPU clock by id reads that process's time, pid 0 reading the
 * caller's; one clock reads user time alone. A thread clock names only the
 * caller, a clock of no such process and a descriptor's clock are refused,
 * and so are ids 8 and above. With the PIT 41 us into the tick, time since
 * boot, raw and boot time read 42.050041 s and the coarse clock 42.05 s. Real
 * time and time since boot resolve to the microsecond and the coarse and CPU
 * clocks to the tick, and a resolution may be asked for with no timespec to
 * write.
 */
static void clock_ids(void)
{
    struct kernel_timespec ts;
    isr_params_t p;
    int32_t clk;

    kernel_ticks = 4205;
    kernel_uptime_seconds = 42;
    kernel_second_ticks = 5;
    proc.pid = proc.tgid = 3;
    proc.utime = 250;
    proc.stime = 120;
    other.pid = other.tgid = 9;
    other.utime = 7;
    other.stime = 1;
    for (clk = 1; clk <= 7; clk++) {
        if (clk == 2 || clk == 3 || clk == 5) {
            continue;
        }
        CHECK(read_clock(clk, &ts) == 0 && ts.tv_sec == 42 && ts.tv_nsec == 50000000,
              "the clocks of time since boot read the uptime");
    }
    CHECK(read_clock(5, &ts) == 0 && ts.tv_sec == 1000042 && ts.tv_nsec == 50000000,
          "coarse real time is the boot's epoch plus the uptime at the tick");
    mono_phase_ns = 41000;
    CHECK(read_clock(1, &ts) == 0 && ts.tv_sec == 42 && ts.tv_nsec == 50041000 &&
              read_clock(4, &ts) == 0 && ts.tv_nsec == 50041000 && read_clock(7, &ts) == 0 &&
              ts.tv_nsec == 50041000,
          "time since boot, raw and boot time read the place within the tick");
    CHECK(read_clock(6, &ts) == 0 && ts.tv_sec == 42 && ts.tv_nsec == 50000000,
          "coarse time since boot stops at the tick");
    mono_phase_ns = 0;
    CHECK(read_clock(2, &ts) == 0 && ts.tv_sec == 3 && ts.tv_nsec == 700000000,
          "the process CPU clock reads user and system time");
    CHECK(read_clock(3, &ts) == 0 && ts.tv_sec == 3 && ts.tv_nsec == 700000000,
          "the thread CPU clock reads the process's time");
    CHECK(read_clock(cpu_clock_id(9, 0, 2), &ts) == 0 && ts.tv_sec == 0 && ts.tv_nsec == 80000000,
          "a CPU clock by pid reads that process's time");
    CHECK(read_clock(cpu_clock_id(9, 0, 1), &ts) == 0 && ts.tv_nsec == 70000000,
          "a CPU clock of user time reads user time alone");
    CHECK(read_clock(cpu_clock_id(0, 0, 0), &ts) == 0 && ts.tv_sec == 3 && ts.tv_nsec == 700000000,
          "a CPU clock of pid 0 reads the caller's time");
    CHECK(read_clock(cpu_clock_id(3, 1, 2), &ts) == 0 && ts.tv_sec == 3,
          "the caller's own thread clock by id is read");
    CHECK(read_clock(cpu_clock_id(9, 1, 2), &ts) == -EINVAL,
          "a thread clock of another process is refused");
    CHECK(read_clock(cpu_clock_id(10, 0, 2), &ts) == -EINVAL,
          "a CPU clock of no such process is refused");
    CHECK(read_clock(cpu_clock_id(9, 0, 3), &ts) == -EINVAL, "a descriptor's clock is refused");
    CHECK(read_clock(8, &ts) == -EINVAL && read_clock(11, &ts) == -EINVAL,
          "clock ids past 7 are refused");
    CHECK(clock_res(0) == 1000, "real time resolves to the microsecond");
    CHECK(clock_res(1) == 1000 && clock_res(4) == 1000 && clock_res(7) == 1000,
          "time since boot, raw and boot time resolve to the microsecond");
    for (clk = 2; clk <= 6; clk++) {
        if (clk != 4) {
            CHECK(clock_res(clk) == 10000000, "the coarse and CPU clocks resolve to the tick");
        }
    }
    CHECK(clock_res(cpu_clock_id(9, 0, 2)) == 10000000, "a CPU clock by pid resolves to the tick");
    CHECK(clock_res(8) == -EINVAL, "an unknown clock has no resolution");
    k_memset(&p, 0, sizeof(p));
    p.ebx = 1;
    CHECK(sys_clock_getres(&p) == 0, "a resolution may be asked for with no timespec");
}

/** Put a clock_nanosleep of `sec` s and `nsec` ns on clock `clk` with
 * `flags` where the call reads it. */
static void ask_clock_sleep(isr_params_t *p, int32_t clk, uint32_t flags, int32_t sec, int32_t nsec)
{
    ask_sleep(p, sec, nsec);
    p->ebx = (uint32_t)clk;
    p->ecx = flags;
    p->edx = 32;
    p->esi = 64;
}

/** Call clock_nanosleep, requiring the return of `sleep_call`. */
static int32_t clock_sleep_call(isr_params_t *p)
{
    int32_t rc = sys_clock_nanosleep(p);

    CHECK(!masked, "the clock sleep returns with the mask lifted");
    CHECK(current_proc->state == PROC_RUNNING && current_proc->wakeup_tick == 0,
          "the clock sleep returns running with no deadline");
    return rc;
}

/** Set the model's tick count and the uptime it reads as, together. */
static void uptime_at(uint32_t tick)
{
    model_at(tick);
    kernel_uptime_seconds = tick / 100u;
    kernel_second_ticks = tick % 100u;
}

/**
 * The caller's process CPU clock while another thread of the process lives
 * and runs, charging the process a tick at every tick: it is read again at
 * every tick, so a sleep from 3.7 s of CPU time until 3.75 s, absolute or
 * relative, blocks until the next tick first and returns at tick 3005.
 */
static void cpu_clock_sibling(void)
{
    isr_params_t p;
    int absolute;

    group.live = 2;
    for (absolute = 0; absolute <= 1; absolute++) {
        proc.utime = 250;
        proc.stime = 120;
        if (absolute) {
            ask_clock_sleep(&p, 2, 1, 3, 750000000);
        } else {
            ask_clock_sleep(&p, 2, 0, 0, 50000000);
        }
        uptime_at(3000);
        sibling_runs = 1;
        CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 3001 && kernel_ticks == 3005,
              "a sleep on the process CPU clock another thread advances ends when it reaches the "
              "time");
    }
    group.live = 0;
}

/**
 * clock_nanosleep's clocks, deadlines and remainders.
 *
 * The raw and coarse clocks and the thread CPU clock cannot be slept on
 * (EOPNOTSUPP), nor a thread clock by id (EINVAL); an unknown clock and a bad
 * request are refused. An interval sleeps as nanosleep does, three ticks from
 * tick 1000 until 1004, but writes nothing on completion; a signal leaves the
 * whole interval. An absolute time of time since boot, 10.035 s, is reached at
 * tick 1004 from tick 1000, the first tick that reads it; one of real time is
 * counted from the boot's epoch, 1000010.02 s reached at 1002; a time already
 * past returns at once, and a signal leaves no remainder. The caller's CPU
 * clock returns at once for a time it has reached, and a signal ends a wait
 * for one it has not, leaving the ticks still due of an interval. A signal is
 * pending in each row that must return without sleeping, so a fault that makes
 * one sleep ends it at once instead of waiting out a deadline the model never
 * reaches.
 */
static void clock_sleeps(void)
{
    isr_params_t p;
    struct kernel_timespec rem;
    int clk;
    static const int nosleep[] = {3, 4, 5, 6};

    for (clk = 0; clk < 4; clk++) {
        ask_clock_sleep(&p, nosleep[clk], 0, 0, 30000000);
        uptime_at(1000);
        pending = 1;
        CHECK(clock_sleep_call(&p) == -EOPNOTSUPP, "a clock Linux cannot sleep on is refused");
    }
    ask_clock_sleep(&p, cpu_clock_id(0, 1, 2), 0, 0, 30000000);
    CHECK(clock_sleep_call(&p) == -EINVAL, "a thread CPU clock by id cannot be slept on");
    ask_clock_sleep(&p, 12, 0, 0, 30000000);
    CHECK(clock_sleep_call(&p) == -EINVAL, "an unknown clock cannot be slept on");
    ask_clock_sleep(&p, 1, 0, 0, 1000000000);
    CHECK(clock_sleep_call(&p) == -EINVAL,
          "a clock sleep of a whole second in nanoseconds is refused");

    ask_clock_sleep(&p, 1, 0, 0, 30000000);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1003 && kernel_ticks == 1003,
          "an interval of three ticks from tick 1000 sleeps until 1003");
    rem = remainder();
    CHECK(rem.tv_sec == UNWRITTEN, "an interval that ran its length writes nothing");
    ask_clock_sleep(&p, 0, 0, 0, 30000000);
    uptime_at(2000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends an interval");
    rem = remainder();
    CHECK(rem.tv_sec == 0 && rem.tv_nsec == 30000000, "and leaves the whole interval");

    ask_clock_sleep(&p, 1, 1, 10, 35000000);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1004 && kernel_ticks == 1004,
          "10.035 s since boot is reached at tick 1004");
    ask_clock_sleep(&p, 7, 1, 10, 35000000);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1004,
          "boot time sleeps as time since boot");
    ask_clock_sleep(&p, 0, 1, 1000010, 20000000);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1002 && kernel_ticks == 1002,
          "real time is counted from the boot's epoch");
    ask_clock_sleep(&p, 1, 1, 9, 990000000);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0 && kernel_ticks == 1000,
          "a time already past returns at once");
    ask_clock_sleep(&p, 0, 1, 999, 0);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0, "a real time before the boot returns at once");
    real_sec = 1000010;
    real_nsec = 5000000;
    ask_clock_sleep(&p, 0, 1, 1000010, 4000000);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0,
          "a real time the clock passed within the tick returns at once");
    ask_clock_sleep(&p, 0, 1, 1000010, 8000000);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1001,
          "a real time still ahead within the tick is reached at the next tick");
    mono_phase_ns = 5000000;
    ask_clock_sleep(&p, 1, 1, 10, 4000000);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0,
          "a time since boot the clock passed within the tick returns at once");
    ask_clock_sleep(&p, 1, 1, 10, 8000000);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1001,
          "a time since boot still ahead within the tick is reached at the next tick");
    mono_phase_ns = 0;
    real_sec = 123;
    real_nsec = 450000000;
    ask_clock_sleep(&p, 1, 1, 10, 35000000);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends an absolute sleep");
    rem = remainder();
    CHECK(rem.tv_sec == UNWRITTEN, "and leaves no remainder");

    proc.utime = 250;
    proc.stime = 120;
    ask_clock_sleep(&p, 2, 1, 3, 700000000);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0,
          "the caller's CPU clock returns at once for a time it has reached");
    ask_clock_sleep(&p, 2, 0, 0, 50000000);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends a wait for the caller's CPU clock");
    rem = remainder();
    CHECK(rem.tv_sec == 0 && rem.tv_nsec == 50000000, "and leaves the ticks still due");
    ask_clock_sleep(&p, 2, 1, 10, 0);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends a wait for a CPU time not reached");
    rem = remainder();
    CHECK(rem.tv_sec == UNWRITTEN, "and an absolute one leaves no remainder");
    cpu_clock_sibling();
}

/** Run the independent syscall state cases. */
int main(void)
{
    proc.tg = &group;
    group.leader = &proc;
    other.tg = &other_group;
    other_group.leader = &other;
    connects();
    framebuffer();
    clocks();
    clock_ids();
    clock_sleeps();
    sleeps_masked();
    sleep_edges();
    sleep_races();
    timer_first_expiry();
    if (failures)
        return 1;
    printf("syscall-test: pass: connect, framebuffer, clocks, sleeps and timer\n");
    return 0;
}
