/**
 * Host test of system calls of syscall.c and syscall_net.c: a connect whose
 * handshake ended, a datagram a full queue lost, a stream's sendmsg and the
 * SIGPIPE it posts, the shutdown of a raw and a packet socket, the clocks
 * past 2^32 ticks and their resolutions, and the sleeps and the interval
 * timer, with their deadlines, masks, remainders, clamps and checks of
 * arguments, and the sleeps that end between ticks at their time.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/network/socket/address.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/tick.c tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * The system calls no row makes are declared static before the sources are
 * included, so that the compiler removes them and the hardware they reach. A
 * user pointer is an offset into the array `user`, which every copy checks.
 */
#include "isr.h"
#include "network/socket/socket.h"
#include "fs/fd.h"
struct tty;
static struct tty *fd_tty(fd_entry_t *entry);
static int fd_tty_hung(fd_entry_t *e);
static long sys_vhangup(isr_params_t *params);
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
static long ioctl_fd(int fd_num, uint32_t request, uintptr_t arg);
static long sys_sysinfo(isr_params_t *params);
static long sys_syslog(isr_params_t *params);
static long sys_socketcall(isr_params_t *params);
static long sys_nanosleep(isr_params_t *params);
static long sys_clock_getres(isr_params_t *params);
static long sys_clock_nanosleep(isr_params_t *params);
static long sys_setitimer(isr_params_t *params);
static long sys_getitimer(isr_params_t *params);
static int32_t dgram_send(struct socket *sock, net_addr_t dst_ip, uint16_t dst_port,
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
struct kernel_msghdr;
struct kernel_cmsghdr;
struct kernel_iovec;
static int32_t stream_readv(struct fd_entry *entry, const struct kernel_iovec *iov, uint32_t iovlen,
                            int flags);
static int socketcall_nargs(int call);
static int32_t socket_call(int call, unsigned long *args);
static long sys_recvmmsg_time64(isr_params_t *params);
static uint32_t msghdr_size(void);
static int msghdr_get(struct kernel_msghdr *h, uintptr_t u);
static int msghdr_put(uintptr_t u, const struct kernel_msghdr *h);
static uint32_t cmsg_hdr(void);
static uint32_t cmsg_align(uint32_t n);
static int cmsg_get(struct kernel_cmsghdr *h, uintptr_t at);
static int cmsg_put(uintptr_t at, const struct kernel_cmsghdr *h);
static int32_t sock_timeo_get(int name, uintptr_t val, uint32_t len, int64_t tv[2]);
static uint32_t sock_timeo_put(int name, const int64_t tv[2], uint8_t out[16]);
#include "syscall.c"
#include "syscall_net.c"
#include "ns_first.h"

extern int printf(const char *format, ...);
extern int dprintf(int fd, const char *format, ...);

static process_t proc;
static int udp_answer; /* what `udp_tx` answers */
struct percpu cpu_table[NR_CPUS] = {{.current = &proc}};

/** Every socket here is in the first network namespace. */
int socket_outside(const struct socket *s)
{
    (void)s;
    return 0;
}

/** A socket of the first network namespace reaches every address. */
int socket_ns_reaches(const struct socket *s, net_addr_t a, int up)
{
    (void)s;
    (void)a;
    (void)up;
    return 1;
}

/** A socket of the first network namespace names every interface there. */
struct netdev *socket_iface6(const struct socket *s, int ifindex)
{
    (void)s;
    return ifindex > 0 ? netdev_at(ifindex - 1) : NULL;
}

static int sigpipes; /* the SIGPIPEs posted to the caller */

/** Count a SIGPIPE posted to the caller, the one signal a row posts. */
void signal_post_thread(process_t *t, int signo, const struct k_siginfo *info)
{
    (void)info;
    sigpipes += t == &proc && signo == SIGPIPE;
}

/** No row sets an option of a netlink socket, which this refuses. */
int32_t netlink_setsockopt(struct socket *s, int optname, int val, int optlen)
{
    (void)s;
    (void)optname;
    (void)val;
    (void)optlen;
    return -ENOPROTOOPT;
}
volatile uint64_t kernel_ticks;

/** The tick count as the kernel reads it whole: the model's `kernel_ticks`. */
uint64_t tick_count(void)
{
    return kernel_ticks;
}

/* The real-time clock's step count, which the step rows move (`model_step`). */
static uint32_t model_steps = 1u;

/** The real-time clock's step count (`rtc_step_count`). */
uint32_t rtc_step_count(void)
{
    return model_steps;
}

/** The timer's resolution within the tick: a microsecond, the PIT's when
 * the machine has no finer reference. */
uint32_t timer_resolution_ns(void)
{
    return 1000u;
}

/* The nanoseconds and the microseconds of `n` ticks. */
#define TICKS_NS(n) ((int32_t)((n) * TICK_NS))
#define TICKS_US(n) ((int32_t)((n) * TICK_US))

volatile uint32_t kernel_uptime_seconds;
volatile uint32_t kernel_second_ticks;
uint32_t net_ip_addr = 0x0a00020fu;
static struct socket sock;
static struct tcb control;
static fd_entry_t entry;
static struct open_file description;
static uint8_t user[1024];
static int failures, masked, saves;
static int completion, immediate, waits;
static int pending, pending_masked, blocked_masked, sleeps, cut_short;
static uint64_t blocked_until;
static int post_at_block, wake_at_deadline, tick_signals, wake_unacted;
static int state_at_unmask = -1, unblocked_unmasks;
static int halts, unmasked_halts, blocks;
static uint64_t first_blocked_until, wakeup_at_unmask;
static int unarmed_unmasks, switch_back, tick_at_save, tick_after_yield, halt_without_tick;
static int sibling_runs;
static int wakes_all; /* the calls of wq_wake_all */

#define CHECK(c, detail)                                    \
    do {                                                    \
        if (!(c)) {                                         \
            dprintf(2, "syscall-test: fail: %s\n", detail); \
            failures++;                                     \
        }                                                   \
    } while (0)

/** No row makes an ICMP datagram socket, which alone has an echo port. */
int raw_echo_port(struct socket *s)
{
    (void)s;
    return -1;
}

/** No row connects an ICMP datagram socket. */
void raw_echo_hold(struct socket *s)
{
    (void)s;
    CHECK(0, "no row connects an ICMP datagram socket");
}

/** Whether [addr, addr + len) lies in the model's user memory, `user`;
 * address 0 does not. */
int user_range_ok(uintptr_t addr, size_t len)
{
    return addr && addr <= sizeof(user) && len <= sizeof(user) - addr;
}

/** Whether the range lies in user memory, every page of which is
 * mapped. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    (void)write;
    return user_range_ok(addr, len);
}

/** Copy `len` bytes to the offset `dst` of user memory; 0, or -EFAULT
 * outside it. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    uint32_t at = (uint32_t)(unsigned long)dst;
    if (!user_range_ok(at, len))
        return -EFAULT;
    k_memcpy(user + at, src, len);
    return 0;
}

/** Copy `len` bytes from the offset `src` of user memory; 0, or -EFAULT
 * outside it. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    uint32_t at = (uint32_t)(unsigned long)src;
    if (!user_range_ok(at, len))
        return -EFAULT;
    k_memcpy(dst, user + at, len);
    return 0;
}

static void tick(void);

/** Timer list membership is checked by process_timer_hosttest. */
void proc_timer_update(process_t *p)
{
    (void)p;
}

/** Set the real-time timer as the kernel does, without a timer list. */
void proc_alarm_set(process_t *p, uint64_t tick, uint64_t interval)
{
    p->alarm_tick = tick;
    p->alarm_interval = tick != 0 ? interval : 0;
}

/** The real-time timer's due tick as the kernel reads it, 0 when it is
 * disarmed or its SIGALRM is marked. */
uint64_t proc_alarm_due_tick(const process_t *p)
{
    return p->alarm_due ? 0 : p->alarm_tick;
}

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

/* Real time less time since boot, in seconds and nanoseconds below a
 * second, and the nanoseconds a setting of the clock moves it by right after
 * the next reading of it, or in the next sleep; 0 for none. */
static int64_t model_epoch = 1000000;
static int32_t model_epoch_ns;
static int64_t step_after_offset, step_in_sleep;

/** Add `ns` nanoseconds to the time `*sec` and `*nsec`, keeping `*nsec`
 * within the second. */
static void add_ns(int64_t *sec, int64_t *nsec, int64_t ns)
{
    *nsec += ns % 1000000000;
    *sec += ns / 1000000000;
    if (*nsec < 0) {
        *nsec += 1000000000;
        (*sec)--;
    } else if (*nsec >= 1000000000) {
        *nsec -= 1000000000;
        (*sec)++;
    }
}

/** A setting of the real-time clock `ns` nanoseconds on from where it is, as
 * `rtc_set_realtime` makes on another processor: the real time and the
 * clock's offset move by it, and so does the step count. */
static void model_step(int64_t ns)
{
    int64_t offset_ns = model_epoch_ns;

    add_ns(&real_sec, &real_nsec, ns);
    add_ns(&model_epoch, &offset_ns, ns);
    model_epoch_ns = (int32_t)offset_ns;
    model_steps++;
}

/** The real time at boot, the offset's seconds, which coarse real time is
 * counted from. */
int64_t rtc_boot_epoch(void)
{
    return model_epoch;
}

/** The time since boot at which real time reaches `sec` and `nsec`
 * (`rtc_mono_of_real`): that time less the offset, real time less time
 * since boot, since no row slews the clock; with `step_after_offset`, the
 * clock is set once this reading is taken. */
void rtc_mono_of_real(int64_t sec, int32_t nsec, int64_t *mono_sec, int32_t *mono_nsec)
{
    *mono_sec = sec - model_epoch;
    *mono_nsec = nsec - model_epoch_ns;
    if (*mono_nsec < 0) {
        *mono_nsec += 1000000000;
        (*mono_sec)--;
    }
    if (step_after_offset != 0) {
        model_step(step_after_offset);
        step_after_offset = 0;
    }
}

/** The tick's counts as one reading, with interrupts off as the kernel's
 * reads them, which on the host's one thread is all one reading needs. */
void tick_now(uint32_t *seconds, uint32_t *second_ticks, uint64_t *ticks)
{
    uint32_t flags = kirq_save();

    *seconds = kernel_uptime_seconds;
    *second_ticks = kernel_second_ticks;
    *ticks = kernel_ticks;
    kirq_restore(flags);
}

/** Coarse real time as the clock keeps it: the boot's epoch plus the uptime
 * at the tick, read as one reading (`tick_now`). */
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t uptime, ticks;
    uint64_t total;

    tick_now(&uptime, &ticks, &total);
    *seconds = rtc_boot_epoch() + (int64_t)uptime;
    *nanoseconds = (int64_t)ticks * TICK_NS;
}

/** Coarse time since boot as the clock keeps it: the uptime at the tick,
 * read as one reading (`tick_now`). */
void rtc_coarse_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t uptime, ticks;
    uint64_t total;

    tick_now(&uptime, &ticks, &total);
    *seconds = (int64_t)uptime;
    *nanoseconds = (int64_t)ticks * TICK_NS;
}

/** Report that the model has no vDSO clock, so the timer's resolution
 * stands. */
int vclock_running(void)
{
    return 0;
}

/** The vDSO clock's resolution: unused, since `vclock_running` reports none. */
uint32_t vclock_resolution_ns(void)
{
    return 1u;
}

/* The timer's place within the tick, which the model's time since boot adds;
 * and, when not 0, the reading in nanoseconds the clock keeps after a lost
 * tick put the tick count behind it, which it reads until the count passes. */
static int64_t mono_phase_ns, mono_held_ns;

/** Time since boot as the clock keeps it: the uptime and its ticks, with the
 * place within the tick, read with interrupts masked. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();
    int64_t ns = (int64_t)kernel_second_ticks * TICK_NS + mono_phase_ns;

    *seconds = (int64_t)kernel_uptime_seconds;
    if (mono_held_ns > *seconds * 1000000000 + ns) {
        *seconds = mono_held_ns / 1000000000;
        ns = mono_held_ns % 1000000000;
    }
    *nanoseconds = ns;
    kirq_restore(flags);
}

/** The raw clock, which no row slews, the monotonic clock's reading. */
void rtc_raw_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    rtc_monotonic(seconds, nanoseconds);
}

/** The tick count and the place within the tick, as a sleep reads them. */
void rtc_tick_phase(uint64_t *ticks, uint32_t *phase_ns)
{
    *ticks = kernel_ticks;
    *phase_ns = (uint32_t)mono_phase_ns;
}

/** `rtc_monotonic` as nanoseconds since boot. */
int64_t rtc_monotonic_ns(void)
{
    int64_t sec, nsec;

    rtc_monotonic(&sec, &nsec);
    return sec * 1000000000 + nsec;
}

/* The earliest time the interrupt between ticks is armed for, 0 for none,
 * which `schedule` brings with `wake_between`. */
static int64_t armed_ns;

/** Keep the earliest time the interrupt between ticks is armed for, as a
 * processor's timer keeps it (`timer_wake_at`). */
void timer_wake_at(int64_t ns)
{
    if (armed_ns == 0 || ns < armed_ns) {
        armed_ns = ns;
    }
}

static int wake_between;

/** Whether a sleep ends between ticks: in the rows that say so
 * (`wake_between`). */
int timer_wakes_between(void)
{
    return wake_between;
}

/** The model's one other process, pid 9, whose CPU clock a caller may read. */
static process_t other;
static struct tgroup group, other_group;

/** Each process here is one thread, whose CPU time is the process's. */
void proc_cpu_time(const process_t *p, uint64_t *user, uint64_t *sys)
{
    *user = p->utime;
    *sys = p->stime;
}

/** Find the model's processes: this one and pid 9. */
process_t *get_process(uint32_t pid)
{
    if (pid == proc.pid) {
        return &proc;
    }
    return pid == 9 ? &other : NULL;
}

/** TIOCLINUX's blank, which no row reaches. */
void console_blank_now(void)
{}

/** The unblank of TIOCLINUX, which no row checks. */
void console_unblank_now(void)
{}

/** TIOCLINUX's question, which no row asks: never blank. */
int console_is_blank(void)
{
    return 0;
}

/** The socket's entry for descriptor 3, NULL for any other. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    return fd == 3 ? &entry : NULL;
}

/** 0: no network interrupt runs. */
uint32_t net_lock(void)
{
    return 0;
}

/** Nothing: no network interrupt runs. */
void net_unlock(uint32_t flags)
{
    (void)flags;
}

/** 0: no row binds a datagram socket. */
uint16_t udp_bind_ephemeral(struct socket *s)
{
    (void)s;
    return 0;
}

/** No datagram socket here holds a pending error. */
int socket_dgram_error(struct socket *s)
{
    (void)s;
    return 0;
}

/** No address here is a broadcast address. */
int netconf_is_broadcast(uint32_t ip)
{
    (void)ip;
    return 0;
}

/** Answer as the row says: the result of IP's send. */
int udp_tx(const struct socket *s, net_addr_t ip, uint16_t port, const uint8_t *payload,
           uint16_t plen, const struct net_txopt *opt)
{
    (void)s;
    (void)ip;
    (void)port;
    (void)payload;
    (void)plen;
    (void)opt;
    return udp_answer;
}

/** Give `s` the test's control block, in the state `completion` at once
 * when `immediate` is set and SYN_SENT otherwise; 0. */
int tcp_connect(struct socket *s, net_addr_t ip, uint16_t port)
{
    (void)ip;
    (void)port;
    s->tcb = &control;
    control.state = immediate ? completion : TCP_SYN_SENT;
    return 0;
}

/** A wait in which the handshake ends: put the state `completion` and
 * check that `ready` is then true; 0, or -EINTR when it is not. */
int wq_wait_event(wait_queue_t *q, int (*ready)(void *), void *arg)
{
    (void)q;
    waits++;
    control.state = completion;
    CHECK(ready(arg), "completed handshake wakes caller");
    return ready(arg) ? 0 : -EINTR;
}

/** Count a wake of every waiter, which a shutdown makes. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes_all++;
}

/** 0: no row shuts down a stream. */
int tcp_shutdown(struct socket *s, int rd, int wr)
{
    (void)s;
    (void)rd;
    (void)wr;
    return 0;
}

/**
 * A blocking connect whose handshake ends ESTABLISHED, or CLOSE_WAIT after
 * a FIN that came before the caller ran, returns 0, and one that ends
 * CLOSED gives -ECONNREFUSED; each waits through its predicate. A
 * nonblocking connect returns 0 when the handshake ended at once and
 * -EINPROGRESS otherwise, and `connect_done` is false while the handshake
 * runs and true once the socket has no connection.
 */
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

static void ticks_pass(uint64_t n);

/* The time the clock moves on at the first question that finds a signal
 * pending, the ticks it crosses counted, 0 for none: the sleep's end comes
 * between that question and the remainder's reading. */
static int64_t signal_moves_ns;

/**
 * The pending state the sleep asks about, and the mask of its first question,
 * which must be on: the sleep asks at the top of each turn, masked, whether a
 * signal it acts on is pending. With `signal_moves_ns`, a question that finds
 * one moves the clock on by that much, once.
 */
int signal_pending(void)
{
    if (pending_masked < 0) {
        pending_masked = masked;
    }
    if (pending && signal_moves_ns != 0) {
        mono_phase_ns += signal_moves_ns;
        signal_moves_ns = 0;
        while (mono_phase_ns >= TICKS_NS(1)) {
            mono_phase_ns -= TICKS_NS(1);
            ticks_pass(1);
        }
    }
    return pending;
}

/* With `wake_between`, the yield of a sleeper with a time armed between
 * ticks moves the clock to that time and the interrupt there wakes it; the
 * sleep's `wakeup_ns` at the last block is `blocked_ns`. */
static int64_t blocked_ns;

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

/** Count `n` ticks, which the uptime counts too. */
static void ticks_pass(uint64_t n)
{
    kernel_ticks += n;
    kernel_uptime_seconds += (uint32_t)(n / TICK_HZ);
    kernel_second_ticks += (uint32_t)(n % TICK_HZ);
    if (kernel_second_ticks >= TICK_HZ) {
        kernel_second_ticks -= TICK_HZ;
        kernel_uptime_seconds++;
    }
}

/** One tick of the model's clock, and the timer's wake of a due sleeper, or
 * of a sleeper on real time whose clock was set since it counted its tick
 * (`rt_steps`), as `proc_wake_sleepers` wakes them; with `sibling_runs`,
 * another thread of the process runs in the tick, which the timer charges to
 * the process. */
static void tick(void)
{
    ticks_pass(1);
    if (sibling_runs) {
        proc.utime++;
    }
    if (current_proc->state == PROC_BLOCKED &&
        ((current_proc->wakeup_tick != 0 && kernel_ticks >= current_proc->wakeup_tick) ||
         (current_proc->rt_steps != 0 && current_proc->rt_steps != model_steps))) {
        wake(0);
    }
}

/** Move the model's clock forward to `tick`, never back. */
static void clock_to(uint64_t tick)
{
    if (tick > kernel_ticks) {
        ticks_pass(tick - kernel_ticks);
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
 * `switch_back`, as the kernel's `schedule` leaves it when it switches back.
 *
 * With `step_in_sleep`, the clock is set once instead, and the sleeper left
 * blocked for the next tick to wake.
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
    blocked_ns = current_proc->wakeup_ns;
    if (blocks++ == 0) {
        first_blocked_until = blocked_until;
    }
    if (wake_between && armed_ns != 0) {
        int64_t at = armed_ns;

        armed_ns = 0;
        while (at - ((int64_t)kernel_uptime_seconds * 1000000000 +
                     (int64_t)kernel_second_ticks * TICK_NS) >=
               (int64_t)TICK_NS) {
            ticks_pass(1);
        }
        mono_phase_ns = at - ((int64_t)kernel_uptime_seconds * 1000000000 +
                              (int64_t)kernel_second_ticks * TICK_NS);
        wake(0);
        return;
    }
    if (step_in_sleep != 0) {
        model_step(step_in_sleep);
        step_in_sleep = 0;
        return;
    }
    if (tick_after_yield) {
        tick_after_yield = 0;
        clock_to(current_proc->wakeup_tick - 1);
        tick_at_save = 1;
        return;
    }
    if (tick_signals) {
        ticks_pass(1);
        wake(1);
    } else if (wake_at_deadline) {
        clock_to(current_proc->wakeup_tick);
        wake(post_at_block);
    } else if (post_at_block) {
        clock_to(current_proc->wakeup_tick - 1);
        wake(1);
    } else if (wake_unacted) {
        wake_unacted = 0;
        ticks_pass(1);
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
uint64_t proc_alarm_at(uint64_t now, uint64_t ticks)
{
    uint64_t at = now + ticks;

    CHECK(masked, "the timer is armed with interrupts masked");
    return at != 0 ? at : 1u;
}

/** Count a sleep a signal ended early, as the kernel's own counter does. */
void process_count_sleep_cut_short(void)
{
    cut_short++;
}

/** Put a sleep of `sec` s and `nsec` ns where the call reads it. */
static void ask_sleep(isr_params_t *p, long sec, int32_t nsec)
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
static void model_at(uint64_t tick)
{
    kernel_ticks = tick;
    pending = 0;
    post_at_block = wake_at_deadline = tick_signals = wake_unacted = 0;
    switch_back = tick_at_save = tick_after_yield = halt_without_tick = sibling_runs = 0;
    sleeps = cut_short = halts = unmasked_halts = blocks = unblocked_unmasks = 0;
    unarmed_unmasks = 0;
    blocked_until = first_blocked_until = 0;
    mono_held_ns = 0;
    armed_ns = blocked_ns = 0;
    wake_between = 0;
    signal_moves_ns = 0;
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

/** Whether the remainder's place still holds the bytes `ask_sleep` filled
 * it with: nothing was written there. */
static int remainder_untouched(void)
{
    size_t i;

    for (i = 0; i < sizeof(struct kernel_timespec); i++) {
        if (user[64 + i] != 0xA5) {
            return 0;
        }
    }
    return 1;
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

    ask_sleep(&p, 0, TICKS_NS(3));
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

    ask_sleep(&p, 0, TICKS_NS(3));
    model_at(2000);
    pending = 1;
    CHECK(sleep_call(&p) == -EINTR, "a signal already there ends the sleep at once");
    CHECK(sleeps == 0, "with no yield");
    CHECK(cut_short == 1, "and the count of sleeps a signal cut short rises");
    rem = remainder();
    CHECK(rem.tv_sec == 0 && rem.tv_nsec == TICKS_NS(3), "and the whole request is left");
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
 * to sleep with its deadline armed again. A remainder of two seconds less a
 * tick is 1 s and a second less a tick. A sleep resumed for its remainder
 * after a signal at every tick ends within four calls, no earlier than the
 * first sleep's deadline. The count of ticks is 64 bits wide: a sleep begun a
 * tick before 2^32 ticks blocks until its deadline past them, and one of
 * 2^62 s is cut at 2^40 s, which a signal a tick in leaves less that tick. A
 * sleep of zero blocks for Linux's 50 us of slack, and the interrupt between
 * ticks ends it then, within its tick.
 */
static void sleep_edges(void)
{
    isr_params_t p;
    struct kernel_timespec rem;
    int64_t start;
    int rc, calls;

    ask_sleep(&p, 0, TICKS_NS(3));
    model_at(3000);
    post_at_block = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && rem.tv_sec == 0 && rem.tv_nsec == TICKS_NS(1),
          "a signal as the last tick begins leaves that tick");
    CHECK(cut_short == 1, "and the sleep is counted as one a signal cut short");

    ask_sleep(&p, 0, TICKS_NS(3));
    model_at(3000);
    post_at_block = wake_at_deadline = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == 0 && cut_short == 0 && rem.tv_sec == 0 && rem.tv_nsec == 0,
          "a signal at the deadline ends nothing");

    ask_sleep(&p, 0, TICKS_NS(3));
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
    CHECK(rc == -EINTR && rem.tv_sec == 1 && rem.tv_nsec == 1000000000 - TICKS_NS(1),
          "a remainder of two seconds less a tick is 1 s and a second less a tick");

    ask_sleep(&p, 0, TICKS_NS(3));
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

    ask_sleep(&p, 0, TICKS_NS(3));
    model_at(0xFFFFFFFFull);
    rc = sleep_call(&p);
    CHECK(rc == 0 && first_blocked_until == 0x100000002ull && kernel_ticks == 0x100000002ull,
          "a sleep begun a tick before 2^32 ticks blocks until its deadline past them");

    ask_sleep(&p, (long)((int64_t)1 << 62), 0);
    model_at(1400);
    tick_signals = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && first_blocked_until == 1400u + ((uint64_t)1 << 40) * TICK_HZ &&
              rem.tv_sec == (long)(((int64_t)1 << 40) - 1) &&
              rem.tv_nsec == 1000000000 - TICKS_NS(1),
          "a sleep of 2^62 s is cut at 2^40 s");

    ask_sleep(&p, 0, 0);
    model_at(0);
    wake_between = 1;
    start = rtc_monotonic_ns();
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocks == 1 && kernel_ticks == 0 && blocked_ns == start + 50000,
          "a sleep of zero blocks for Linux's 50 us of slack and ends between ticks");
    mono_phase_ns = 0;
}

/**
 * The sleep's rounding, its checks of the request, and the ticks that come
 * as it masks.
 *
 * A sleep ends at the first tick at or after its end, measured from where in
 * the tick in progress it began: one of a tick and a half from the start of
 * tick 1100 blocks until 1102, one of 1 ns from 1150 until 1151, and one of a
 * tenth of a tick begun four tenths into tick 1300 until 1301, where one of
 * seven tenths waits until 1302. A negative second, a negative nanosecond
 * count and one of a whole second fail with -EINVAL. A sleeper that a signal
 * wakes, and that the scheduler switches back to, runs again at once rather
 * than halting, so a sleep of three ticks begun at 8000 with a signal at 8001
 * leaves two. The
 * deadline's tick may come just before the sleep masks, after a yield or after
 * a halt that another interrupt ended; the sleep sees it at once, from 8100 and
 * 8200 alike.
 */
static void sleep_races(void)
{
    isr_params_t p;
    struct kernel_timespec rem;
    int rc;

    ask_sleep(&p, 0, TICKS_NS(3) / 2);
    model_at(1100);
    rc = sleep_call(&p);
    CHECK(rc == 0 && first_blocked_until == 1102,
          "a sleep of a tick and a half from tick 1100 blocks until 1102");
    ask_sleep(&p, 0, 1);
    model_at(1150);
    rc = sleep_call(&p);
    CHECK(rc == 0 && first_blocked_until == 1151,
          "a sleep of 1 ns from tick 1150 blocks until 1151");
    mono_phase_ns = TICKS_NS(4) / 10;
    ask_sleep(&p, 0, TICKS_NS(1) / 10);
    model_at(1300);
    rc = sleep_call(&p);
    CHECK(rc == 0 && first_blocked_until == 1301,
          "a sleep of a tenth of a tick begun four tenths into a tick ends at the next");
    ask_sleep(&p, 0, TICKS_NS(7) / 10);
    model_at(1300);
    rc = sleep_call(&p);
    CHECK(rc == 0 && first_blocked_until == 1302, "and one of seven tenths at the tick after");
    mono_phase_ns = 0;

    ask_sleep(&p, -1, 0);
    model_at(1200);
    CHECK(sleep_call(&p) == -EINVAL, "a sleep of a negative second is refused");
    ask_sleep(&p, 0, 1000000000);
    CHECK(sleep_call(&p) == -EINVAL, "a sleep of 1000000000 ns is refused");
    ask_sleep(&p, 0, -1);
    CHECK(sleep_call(&p) == -EINVAL, "a sleep of -1 ns is refused");

    ask_sleep(&p, 0, TICKS_NS(3));
    model_at(8000);
    switch_back = tick_signals = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && rem.tv_sec == 0 && rem.tv_nsec == TICKS_NS(2),
          "a sleeper switched back to after a signal runs again at once");

    ask_sleep(&p, 0, TICKS_NS(3));
    model_at(8100);
    tick_after_yield = 1;
    rc = sleep_call(&p);
    CHECK(rc == 0 && kernel_ticks == 8103,
          "a deadline tick that comes as the sleep masks after a yield is seen");

    ask_sleep(&p, 0, TICKS_NS(3));
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
 * A sleep lasts its request by time since boot as well as by the tick count.
 *
 * After a lost tick the clock keeps a reading ahead of the count, here eight
 * tenths of a tick past tick 1000, which reads as 10 s. A sleep of a tick and
 * a half from there has its deadline tick at 1002, where the clock reads two
 * ticks past 10 s, short of 2.3 ticks, so it sleeps on to 1003. A signal at
 * tick 1002 ends it with the three tenths of a tick the clock still lacks as
 * its remainder.
 */
static void sleep_held_clock(void)
{
    isr_params_t p;
    struct kernel_timespec rem;
    int rc;

    ask_sleep(&p, 0, TICKS_NS(3) / 2);
    model_at(1000);
    kernel_uptime_seconds = 10;
    kernel_second_ticks = 0;
    mono_held_ns = 10000000000LL + TICKS_NS(8) / 10;
    rc = sleep_call(&p);
    CHECK(rc == 0 && blocked_until == 1003 && kernel_ticks == 1003,
          "a sleep of a tick and a half from a clock 0.8 tick ahead of tick 1000 ends at 1003");

    ask_sleep(&p, 0, TICKS_NS(3) / 2);
    model_at(1000);
    kernel_uptime_seconds = 10;
    kernel_second_ticks = 0;
    mono_held_ns = 10000000000LL + TICKS_NS(8) / 10;
    post_at_block = wake_at_deadline = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && rem.tv_sec == 0 && rem.tv_nsec == TICKS_NS(3) / 10 && cut_short == 1,
          "and a signal at tick 1002 leaves the 0.3 tick the clock lacks");
}

/**
 * A sleep ends at its time between ticks, as Linux's hrtimer ends it, and
 * not at the tick after it.
 *
 * A sleep of a quarter tick begun half a tick into tick 1000 arms the
 * interrupt between ticks for three quarters into it, and that interrupt
 * ends it with the tick count still at 1000. One of a tick and a quarter
 * has its deadline tick two ahead, at 1002, which a clock running ahead of
 * the tick count gives such a time; the clock decides, so it too is armed
 * at once, and ends at its time, in tick 1001. A signal pending as a sleep
 * of a quarter tick begun half a tick in blocks leaves a remainder of the
 * quarter tick, the time to its end, as Linux's, and not the half tick to
 * its deadline tick. A signal seen as the clock reaches such a sleep's end
 * returns 0 with no remainder written and counts no sleep cut short, as
 * Linux's do_nanosleep returns 0 for a remainder of 0 or less; so does one
 * seen as the tick count reaches the deadline tick where there is no
 * interrupt between ticks. A sleep of 10,000,000,000 s, which a sleep
 * accepts, has a time past what nanoseconds since boot hold, so it keeps no
 * time between ticks and is the tick's, as Linux takes such a time as
 * KTIME_MAX.
 */
static void sleep_between(void)
{
    isr_params_t p;
    struct kernel_timespec rem;
    int64_t start;
    int32_t rc;

    mono_phase_ns = TICKS_NS(1) / 2;
    ask_sleep(&p, 0, TICKS_NS(1) / 4);
    model_at(1000);
    kernel_uptime_seconds = 1;
    kernel_second_ticks = 0;
    start = rtc_monotonic_ns();
    wake_between = 1;
    CHECK(sleep_call(&p) == 0 && kernel_ticks == 1000 && blocks == 1,
          "a sleep of a quarter tick begun half a tick in ends within the tick");
    CHECK(blocked_until == 1001 && blocked_ns == start + TICKS_NS(1) / 4 &&
              mono_phase_ns == TICKS_NS(3) / 4,
          "at its time, which it armed between ticks, before its deadline tick 1001");

    mono_phase_ns = TICKS_NS(1) / 2;
    ask_sleep(&p, 0, TICKS_NS(5) / 4);
    model_at(1000);
    kernel_uptime_seconds = 1;
    kernel_second_ticks = 0;
    start = rtc_monotonic_ns();
    wake_between = 1;
    CHECK(sleep_call(&p) == 0 && kernel_ticks == 1001 && blocks == 1 && blocked_until == 1002,
          "a sleep whose deadline tick is two ahead is armed at once and ends in tick 1001");
    CHECK(blocked_ns == start + TICKS_NS(5) / 4 && mono_phase_ns == TICKS_NS(3) / 4,
          "at its time, three quarters into that tick");

    mono_phase_ns = TICKS_NS(1) / 2;
    ask_sleep(&p, 0, TICKS_NS(1) / 4);
    model_at(1000);
    kernel_uptime_seconds = 1;
    kernel_second_ticks = 0;
    wake_between = 1;
    pending = 1;
    rc = sleep_call(&p);
    rem = remainder();
    CHECK(rc == -EINTR && blocks == 0 && rem.tv_sec == 0 && rem.tv_nsec == TICKS_NS(1) / 4,
          "a signal as a sleep of a quarter tick blocks leaves the quarter tick to its end");
    pending = 0;

    mono_phase_ns = TICKS_NS(1) / 2;
    ask_sleep(&p, 0, TICKS_NS(1) / 4);
    model_at(1000);
    kernel_uptime_seconds = 1;
    kernel_second_ticks = 0;
    wake_between = 1;
    pending = 1;
    signal_moves_ns = TICKS_NS(1) / 4;
    rc = sleep_call(&p);
    CHECK(rc == 0 && cut_short == 0 && remainder_untouched(),
          "a signal seen as the clock reaches the sleep's end returns 0 and writes no remainder");
    pending = 0;

    mono_phase_ns = TICKS_NS(1) / 2;
    ask_sleep(&p, 0, TICKS_NS(1) / 4);
    model_at(1000);
    kernel_uptime_seconds = 1;
    kernel_second_ticks = 0;
    pending = 1;
    signal_moves_ns = TICKS_NS(1) / 2;
    rc = sleep_call(&p);
    CHECK(rc == 0 && cut_short == 0 && kernel_ticks == 1001 && remainder_untouched(),
          "without the interrupt between ticks, one seen as the deadline tick comes returns 0 too");
    pending = 0;
    mono_phase_ns = 0;

    ask_sleep(&p, 10000000000L, 0);
    model_at(1000);
    kernel_uptime_seconds = 1;
    kernel_second_ticks = 0;
    wake_between = 1;
    tick_signals = 1;
    CHECK(sleep_call(&p) == -EINTR && blocks == 1 && blocked_ns == 0 && armed_ns == 0,
          "a sleep of 10,000,000,000 s keeps no time between ticks");
    pending = 0;
    tick_signals = 0;
    wake_between = 0;
}

/**
 * The timer's first SIGALRM is never early, and getitimer reads back what
 * was set.
 *
 * The tick in progress is not counted, so a timer of three ticks set at tick
 * 1000 is due at 1004, and getitimer reads it back as the three ticks set and
 * its interval. A tick before it is due, the time left is between none and a
 * tick, and it reads as one tick, since zero means disarmed. A timer of half a
 * tick, and one of 1 us, is armed, due at the tick after next, and an
 * interval of half a tick is a tick. A timer of 1 s reads back as 1 s. The
 * count is 64 bits wide, so a timer of 42949673 s is due whole, and the
 * longest timer, 2^31 - 1 s and 999999 us, which rounds up to 2^31 s, reads
 * back as that on the host's 64-bit long; the i686's long cuts it to the
 * longest it holds. An interval of a second and more reads back whole. An
 * interval of a whole second in microseconds fails with -EINVAL, and so does
 * a getitimer of a timer other than the three. `alarm` rounds the seconds
 * left to the nearest, a half second up, and to 1 when under a second is
 * left, and leaves no interval.
 */
static void timer_first_expiry(void)
{
    struct kernel_itimerval cur;

    model_at(1000);
    CHECK(set_timer(0, TICKS_US(3), 0, 250000) == 0 && proc.alarm_tick == 1004,
          "a timer of three ticks set at tick 1000 is due at 1004");
    cur = read_timer();
    CHECK(cur.value_sec == 0 && cur.value_usec == TICKS_US(3),
          "and it reads back as the three ticks set");
    CHECK(cur.interval_sec == 0 && cur.interval_usec == 250000, "and its interval");
    model_at(1003);
    cur = read_timer();
    CHECK(cur.value_sec == 0 && cur.value_usec == TICKS_US(1),
          "a timer due at the next tick reads as one tick");
    CHECK(set_timer(0, 0, 0, 0) == 0 && proc.alarm_tick == 0, "a value of zero disarms the timer");

    model_at(1200);
    CHECK(set_timer(0, TICKS_US(1) / 2, 0, 0) == 0 && proc.alarm_tick == 1202,
          "a timer of half a tick is armed, due at 1202");
    CHECK(set_timer(0, 1, 0, 0) == 0 && proc.alarm_tick == 1202,
          "a timer of 1 us is armed, due at 1202");
    CHECK(set_timer(0, TICKS_US(3), 0, TICKS_US(1) / 2) == 0 &&
              read_timer().interval_usec == TICKS_US(1),
          "an interval of half a tick rounds up to a tick");
    model_at(1300);
    CHECK(set_timer(1, 0, 0, 0) == 0, "set a timer of 1 s");
    cur = read_timer();
    CHECK(cur.value_sec == 1 && cur.value_usec == 0, "a timer of 1 s reads back as 1 s");
    model_at(1400);
    CHECK(set_timer(42949673, 0, 0, 0) == 0 &&
              proc.alarm_tick == 1400u + 42949673ull * TICK_HZ + 1u,
          "a timer of 42949673 s is due whole, past 32 bits of ticks");

    model_at(0x100000001ull);
    CHECK(set_timer(0x7FFFFFFF, 999999, 0, 0) == 0, "set the longest timer past 2^32 ticks");
    cur = read_timer();
    CHECK(cur.value_sec == 0x80000000L && cur.value_usec == 0,
          "the longest timer reads back as 2^31 s, its ticks rounded up");
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
    model_at(1000u + 3u * TICK_HZ / 10u);
    CHECK(call_alarm(5) == 5, "alarm reports 4.7 s left as 5");
    model_at(1000u + 3u * TICK_HZ / 10u + 5u * TICK_HZ - 2u);
    CHECK(call_alarm(5) == 1, "alarm reports two ticks left as 1, since 0 means none was armed");
    model_at(1000u + 3u * TICK_HZ / 10u + 5u * TICK_HZ - 2u + 57u * TICK_HZ / 100u);
    CHECK(call_alarm(0) == 4, "alarm reports 4.43 s left as 4");
    CHECK(proc.alarm_tick == 0, "and alarm(0) leaves none");
    model_at(2000);
    CHECK(call_alarm(5) == 0, "arm an alarm of 5 s at tick 2000");
    model_at(2000u + TICK_HZ / 2u);
    CHECK(call_alarm(0) == 5, "alarm reports 4.5 s left as 5");
}

/**
 * A timer whose SIGALRM is sent reads no time left, as Linux's
 * `itimer_get_remtime` reads its stopped timer (kernel/time/itimer.c:29-45).
 *
 * A timer of 1 s, then every half second, set at tick 3000 is due at
 * 3000 + TICK_HZ + 1. While the tick has marked its SIGALRM (`alarm_due`)
 * and the due tick still stands, getitimer reads a value of 0 with the
 * interval, and alarm returns 0. Once the post has stopped it, its due tick
 * 0 and its interval kept until the SIGALRM is taken, getitimer reads the
 * same.
 */
static void timer_marked(void)
{
    struct kernel_itimerval cur;

    model_at(3000);
    CHECK(set_timer(1, 0, 0, 500000) == 0 && proc.alarm_tick == 3000u + TICK_HZ + 1u,
          "set a timer of 1 s, then every 0.5 s, at tick 3000");
    model_at(3000u + TICK_HZ + 1u);
    proc.alarm_due = 1;
    cur = read_timer();
    CHECK(cur.value_sec == 0 && cur.value_usec == 0 && cur.interval_sec == 0 &&
              cur.interval_usec == 500000,
          "with its SIGALRM marked, getitimer reads no time left and the interval");
    CHECK(call_alarm(0) == 0, "and alarm returns 0");
    proc.alarm_due = 0;

    CHECK(set_timer(1, 0, 0, 500000) == 0, "set the timer again");
    proc.alarm_tick = 0;
    cur = read_timer();
    CHECK(cur.value_sec == 0 && cur.value_usec == 0 && cur.interval_sec == 0 &&
              cur.interval_usec == 500000,
          "stopped at its SIGALRM's post, getitimer reads no time left and the interval");
    CHECK(set_timer(0, 0, 0, 0) == 0 && proc.alarm_interval == 0,
          "and a value of zero clears the interval");
}

/**
 * Coarse time since boot reads the uptime and its ticks across 2^32 ticks
 * and into the next second, and leaves interrupts as it found them. Real
 * time reads the model's; clock 12 gives -EINVAL, and a null timespec
 * -EFAULT.
 */
static void clocks(void)
{
    isr_params_t p;
    struct kernel_timespec ts;
    int i;
    uint64_t ticks[] = {0xfffffffeull, 0xffffffffull, 0x100000000ull, 0x100000003ull,
                        (0x100000000ull / TICK_HZ + 1u) * TICK_HZ};

    k_memset(&p, 0, sizeof(p));
    p.ebx = 6;
    p.ecx = 16;
    for (i = 0; i < 5; i++) {
        kernel_ticks = ticks[i];
        kernel_uptime_seconds = (uint32_t)(ticks[i] / TICK_HZ);
        kernel_second_ticks = (uint32_t)(ticks[i] % TICK_HZ);
        CHECK(sys_clock_gettime(&p) == 0, "read monotonic clock");
        k_memcpy(&ts, user + 16, sizeof(ts));
        CHECK(ts.tv_sec == (long)(ticks[i] / TICK_HZ) &&
                  ts.tv_nsec == (long)(ticks[i] % TICK_HZ * TICK_NS),
              "monotonic reads across 2^32 ticks");
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
 * Five hundredths into second 42, the four clocks of time since boot read
 * 42.05 s and coarse real time the boot's epoch plus that; the two CPU clocks
 * read the 2.5 s of user and 1.2 s of system time the process was charged. A
 * process's CPU clock by id reads that process's time, pid 0 reading the
 * caller's; one clock reads user time alone. A thread clock names only the
 * caller, and a clock of no such process, a descriptor's clock and ids 8 and
 * above fail with -EINVAL. With the timer 41 us into the tick, time since
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

    kernel_ticks = 42u * TICK_HZ + 5u * TICK_HZ / 100u;
    kernel_uptime_seconds = 42;
    kernel_second_ticks = 5u * TICK_HZ / 100u;
    proc.pid = proc.tgid = 3;
    proc.utime = 250u * TICK_HZ / 100u;
    proc.stime = 120u * TICK_HZ / 100u;
    other.pid = other.tgid = 9;
    other.utime = 7u * TICK_HZ / 100u;
    other.stime = 1u * TICK_HZ / 100u;
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
            CHECK(clock_res(clk) == TICKS_NS(1), "the coarse and CPU clocks resolve to the tick");
        }
    }
    CHECK(clock_res(cpu_clock_id(9, 0, 2)) == TICKS_NS(1),
          "a CPU clock by pid resolves to the tick");
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
static void uptime_at(uint64_t tick)
{
    model_at(tick);
    kernel_uptime_seconds = (uint32_t)(tick / TICK_HZ);
    kernel_second_ticks = (uint32_t)(tick % TICK_HZ);
}

/** Put a clock_nanosleep on clock `clk` with `flags` until the time `epoch`
 * seconds and `tick` ticks and `ns` nanoseconds, as `uptime_at` reads a
 * tick. */
static void ask_clock_tick(isr_params_t *p, int32_t clk, uint32_t flags, long epoch, uint64_t tick,
                           int32_t ns)
{
    ask_clock_sleep(p, clk, flags, epoch + (long)(tick / TICK_HZ),
                    (int32_t)(tick % TICK_HZ * TICK_NS) + ns);
}

/**
 * The caller's process CPU clock while another thread of the process lives
 * and runs, charging the process a tick at every tick: it is read again at
 * every tick, so a sleep from 3.7 s of CPU time until five ticks later,
 * absolute or relative, blocks until the next tick first and returns at tick
 * 3005, and one of 1 ns returns at tick 3001.
 */
static void cpu_clock_sibling(void)
{
    isr_params_t p;
    int absolute;

    group.live = 2;
    for (absolute = 0; absolute <= 1; absolute++) {
        proc.utime = 250u * TICK_HZ / 100u;
        proc.stime = 120u * TICK_HZ / 100u;
        if (absolute) {
            ask_clock_sleep(&p, 2, 1, 3, 700000000 + TICKS_NS(5));
        } else {
            ask_clock_sleep(&p, 2, 0, 0, TICKS_NS(5));
        }
        uptime_at(3000);
        sibling_runs = 1;
        CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 3001 && kernel_ticks == 3005,
              "a sleep on the process CPU clock another thread advances ends when it reaches the "
              "time");
    }
    proc.utime = 250u * TICK_HZ / 100u;
    proc.stime = 120u * TICK_HZ / 100u;
    ask_clock_sleep(&p, 2, 0, 0, 1);
    uptime_at(3000);
    sibling_runs = 1;
    CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 3001 && kernel_ticks == 3001,
          "a sleep of 1 ns on the process CPU clock lasts to its next tick");
    group.live = 0;
}

/**
 * clock_nanosleep's refusals: the raw and coarse clocks and the thread CPU
 * clock cannot be slept on (EOPNOTSUPP), nor a thread clock by id (EINVAL);
 * an unknown clock and a bad request fail with EINVAL. A signal is pending
 * in each row that names a clock Linux cannot sleep on, so a fault that
 * makes one sleep ends it at once.
 */
static void clock_sleep_refusals(void)
{
    isr_params_t p;
    int clk;
    static const int nosleep[] = {3, 4, 5, 6};

    for (clk = 0; clk < 4; clk++) {
        ask_clock_sleep(&p, nosleep[clk], 0, 0, TICKS_NS(3));
        uptime_at(1000);
        pending = 1;
        CHECK(clock_sleep_call(&p) == -EOPNOTSUPP, "a clock Linux cannot sleep on is refused");
    }
    ask_clock_sleep(&p, cpu_clock_id(0, 1, 2), 0, 0, TICKS_NS(3));
    CHECK(clock_sleep_call(&p) == -EINVAL, "a thread CPU clock by id cannot be slept on");
    ask_clock_sleep(&p, 12, 0, 0, TICKS_NS(3));
    CHECK(clock_sleep_call(&p) == -EINVAL, "an unknown clock cannot be slept on");
    ask_clock_sleep(&p, 1, 0, 0, 1000000000);
    CHECK(clock_sleep_call(&p) == -EINVAL,
          "a clock sleep of a whole second in nanoseconds is refused");
}

/**
 * clock_nanosleep's deadlines and remainders.
 *
 * An interval sleeps as nanosleep does, three ticks from tick 1000 until
 * 1003, but writes nothing on completion; a signal leaves the whole
 * interval. An absolute time of time since boot three and a half ticks past
 * tick 1000 is reached at tick 1004, the first tick that reads it; one of
 * real time is counted from the boot's epoch, two ticks past tick 1000
 * reached at 1002; a time whose tick comes while the clock, held a
 * microsecond behind the tick count as the TSC's clock can be, still reads
 * short of it sleeps on to the next tick, since Linux's hrtimer never
 * expires before its clock reaches its time; a time already past returns at
 * once, and a signal leaves no remainder. A time the clock passed within
 * the tick returns at once, and one still ahead within it is reached at the
 * next tick. The caller's CPU clock returns at once for a time it has
 * reached, and a signal ends a wait for one it has not, leaving the ticks
 * still due of an interval. A signal is pending in each row that must
 * return without sleeping, so a fault that makes one sleep ends it at once
 * instead of waiting out a deadline the model never reaches.
 */
static void clock_sleeps(void)
{
    isr_params_t p;
    struct kernel_timespec rem;

    ask_clock_sleep(&p, 1, 0, 0, TICKS_NS(3));
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1003 && kernel_ticks == 1003,
          "an interval of three ticks from tick 1000 sleeps until 1003");
    rem = remainder();
    CHECK(rem.tv_sec == UNWRITTEN, "an interval that ran its length writes nothing");
    ask_clock_sleep(&p, 0, 0, 0, TICKS_NS(3));
    uptime_at(2000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends an interval");
    rem = remainder();
    CHECK(rem.tv_sec == 0 && rem.tv_nsec == TICKS_NS(3), "and leaves the whole interval");

    ask_clock_tick(&p, 1, 1, 0, 1003, TICKS_NS(1) / 2);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1004 && kernel_ticks == 1004,
          "a time since boot 3.5 ticks past tick 1000 is reached at tick 1004");
    ask_clock_tick(&p, 7, 1, 0, 1003, TICKS_NS(1) / 2);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1004,
          "boot time sleeps as time since boot");
    ask_clock_tick(&p, 1, 1, 0, 1003, 0);
    uptime_at(1000);
    mono_phase_ns = -1000;
    CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 1003 && blocked_until == 1004 &&
              kernel_ticks == 1004,
          "a clock a microsecond behind the tick at the time's tick sleeps a tick more");
    mono_phase_ns = 0;
    ask_clock_tick(&p, 0, 1, 1000000, 1002, 0);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1002 && kernel_ticks == 1002,
          "real time is counted from the boot's epoch");
    ask_clock_tick(&p, 1, 1, 0, 999, 0);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0 && kernel_ticks == 1000,
          "a time already past returns at once");
    ask_clock_sleep(&p, 0, 1, 999, 0);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0, "a real time before the boot returns at once");
    real_sec = 1000000 + 1000 / TICK_HZ;
    real_nsec = (int64_t)(1000 % TICK_HZ * TICK_NS) + TICKS_NS(1) / 2;
    ask_clock_tick(&p, 0, 1, 1000000, 1000, TICKS_NS(4) / 10);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0,
          "a real time the clock passed within the tick returns at once");
    ask_clock_tick(&p, 0, 1, 1000000, 1000, TICKS_NS(8) / 10);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1001,
          "a real time still ahead within the tick is reached at the next tick");
    mono_phase_ns = TICKS_NS(1) / 2;
    ask_clock_tick(&p, 1, 1, 0, 1000, TICKS_NS(4) / 10);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0,
          "a time since boot the clock passed within the tick returns at once");
    ask_clock_tick(&p, 1, 1, 0, 1000, TICKS_NS(8) / 10);
    uptime_at(1000);
    CHECK(clock_sleep_call(&p) == 0 && blocked_until == 1001,
          "a time since boot still ahead within the tick is reached at the next tick");
    mono_phase_ns = 0;
    real_sec = 123;
    real_nsec = 450000000;
    ask_clock_tick(&p, 1, 1, 0, 1003, TICKS_NS(1) / 2);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends an absolute sleep");
    rem = remainder();
    CHECK(rem.tv_sec == UNWRITTEN, "and leaves no remainder");

    proc.utime = 250u * TICK_HZ / 100u;
    proc.stime = 120u * TICK_HZ / 100u;
    ask_clock_sleep(&p, 2, 1, 3, 700000000);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == 0 && sleeps == 0,
          "the caller's CPU clock returns at once for a time it has reached");
    ask_clock_sleep(&p, 2, 0, 0, TICKS_NS(5));
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends a wait for the caller's CPU clock");
    rem = remainder();
    CHECK(rem.tv_sec == 0 && rem.tv_nsec == TICKS_NS(5), "and leaves the ticks still due");
    ask_clock_sleep(&p, 2, 1, 10, 0);
    uptime_at(1000);
    pending = 1;
    CHECK(clock_sleep_call(&p) == -EINTR, "a signal ends a wait for a CPU time not reached");
    rem = remainder();
    CHECK(rem.tv_sec == UNWRITTEN, "and an absolute one leaves no remainder");
    cpu_clock_sibling();
}

/** Put an absolute clock_nanosleep on CLOCK_REALTIME until `tick` ticks
 * past the boot's epoch, 1000000 s, and start the model at tick 1000, real
 * time 1000001 s, with the clock's offset 1000000 s on the second. */
static void ask_rt_sleep(isr_params_t *p, uint64_t tick)
{
    model_epoch = 1000000;
    model_epoch_ns = 0;
    real_sec = 1000001;
    real_nsec = 0;
    ask_clock_tick(p, 0, 1, 1000000, tick, 0);
    uptime_at(1000);
}

/**
 * An absolute clock_nanosleep on CLOCK_REALTIME follows a setting of the
 * clock, as Linux's clock_was_set retriggers its hrtimer
 * (kernel/time/hrtimer.c:951-987, 2133-2142). From tick 1000, real time
 * 1000001 s, until 10 ms past that: a step back of a second during the sleep
 * wakes it at the next tick, which counts its tick again and sleeps on to
 * 2010; a step forward of a second past the time ends it at that tick; and a
 * step right after the sleep turns its time into time since boot
 * (`rtc_mono_of_real`), before it blocks, sends it to count again without
 * a block, so that it blocks once, until 2010. A sleep until 1000012 s
 * counts the offset to the nanosecond, as Linux subtracts its base's whole
 * offset (kernel/time/hrtimer.c:922, 2140): after a step of half a second
 * forward it sleeps on to tick 11500, and after one of half a second back
 * to tick 12500.
 */
static void realtime_steps(void)
{
    isr_params_t p;
    int64_t sec = real_sec, nsec = real_nsec;

    ask_rt_sleep(&p, 1010);
    step_in_sleep = -1000000000LL;
    CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 1010 && blocked_until == 2010 &&
              kernel_ticks == 2010,
          "a step back during an absolute sleep on real time sleeps on to the time");
    ask_rt_sleep(&p, 1010);
    step_in_sleep = 1000000000LL;
    CHECK(clock_sleep_call(&p) == 0 && kernel_ticks == 1001 && blocks == 1,
          "a step forward past the time ends an absolute sleep on real time at the next tick");
    ask_rt_sleep(&p, 1010);
    step_after_offset = -1000000000LL;
    CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 2010 && kernel_ticks == 2010,
          "a step between the sleep's reading of the clock and its block is counted first");
    CHECK(current_proc->rt_steps == 0, "the sleep leaves no step count behind");
    ask_rt_sleep(&p, 12000);
    step_in_sleep = 500000000LL;
    CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 12000 && blocked_until == 11500 &&
              kernel_ticks == 11500,
          "a step of half a second forward brings the sleep's tick half a second nearer");
    ask_rt_sleep(&p, 12000);
    step_in_sleep = -500000000LL;
    CHECK(clock_sleep_call(&p) == 0 && first_blocked_until == 12000 && blocked_until == 12500 &&
              kernel_ticks == 12500,
          "a step of half a second back moves the sleep's tick half a second on");
    model_epoch = 1000000;
    model_epoch_ns = 0;
    real_sec = sec;
    real_nsec = nsec;
}

/** Run every row; 0 when each passes, 1 otherwise. */
/**
 * A UDP send answers its length when a full queue lost the datagram, as
 * Linux does without IP_RECVERR, and any other error of IP's as it stands.
 * BusyBox ping exits at the first error from sendto, so a radio queue that
 * fills for a moment must not reach it.
 */
static void queue_full(void)
{
    static const uint8_t data[64];
    struct socket s;
    net_addr_t dst;

    k_memset(&s, 0, sizeof(s));
    k_memset(&dst, 0, sizeof(dst));
    dst.family = AF_INET;
    s.local_port = 1024;
    udp_answer = -ENOBUFS;
    CHECK(dgram_send(&s, dst, 7, data, sizeof(data)) == 64, "a lost datagram is sent");
    udp_answer = 0;
    CHECK(dgram_send(&s, dst, 7, data, sizeof(data)) == 64, "a datagram is sent");
    udp_answer = -ENETUNREACH;
    CHECK(dgram_send(&s, dst, 7, data, sizeof(data)) == -ENETUNREACH, "no route is an error");
    CHECK(dgram_sent(-ENOBUFS, 8) == 8, "a raw datagram lost is sent");
    CHECK(dgram_sent(-EMSGSIZE, 8) == -EMSGSIZE, "a raw datagram too long is an error");
}

/** No row's descriptor is the query descriptor. */
int sock_is_query(const struct socket *s)
{
    (void)s;
    return 0;
}

/** Nothing: a row's socket is not freed. */
void socket_ref(struct socket *s)
{
    (void)s;
}

/** Nothing: a row's socket is not freed. */
void socket_unref(struct socket *s)
{
    (void)s;
}

/** No row sends on a netlink socket: -EINVAL. */
int32_t netlink_send_to(struct socket *s, const uint8_t *buf, uint32_t len,
                        const struct nl_dest *dst)
{
    (void)s;
    (void)buf;
    (void)len;
    (void)dst;
    CHECK(0, "no row sends on a netlink socket");
    return -EINVAL;
}

/** No address is the machine's own: no row sends on a raw IPv6 socket. */
int net_addr_local(net_addr_t a)
{
    (void)a;
    return 0;
}

/** No row sends an IPv6 datagram: -EINVAL. */
int ip6_tx_whole(const uint8_t *dg, uint32_t len, const uint8_t *dst, int ifindex)
{
    (void)dg;
    (void)len;
    (void)dst;
    (void)ifindex;
    CHECK(0, "no row sends an IPv6 datagram");
    return -EINVAL;
}

/** No row sends an ICMPv6 message: -EINVAL. */
int icmp6_send_opts(const uint8_t *src, const uint8_t *dst, int ifindex, uint8_t *msg, uint16_t len,
                    uint8_t hlim, int opts)
{
    (void)src;
    (void)dst;
    (void)ifindex;
    (void)msg;
    (void)len;
    (void)hlim;
    (void)opts;
    CHECK(0, "no row sends an ICMPv6 message");
    return -EINVAL;
}

/** No row sends on a raw IPv6 socket: its hop limit is never asked. */
uint8_t socket_hops6(const struct socket *s, const uint8_t *dst, int ifindex)
{
    (void)s;
    (void)dst;
    (void)ifindex;
    return 64;
}

/** No row sends on a raw socket: -EINVAL. */
int raw_echo_stamp(struct socket *s, uint8_t *msg, int len, uint8_t request)
{
    (void)s;
    (void)msg;
    (void)len;
    (void)request;
    CHECK(0, "no row sends on a raw socket");
    return -EINVAL;
}

/** No row sends on a raw socket: its checksum is never asked. */
uint16_t ip_checksum(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
    return 0;
}

/** Store `value` big-endian at `p`. */
void write16be(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/** No row sends on a raw socket: -EINVAL. */
int ip_tx_opts(uint32_t src, uint32_t dst_ip, uint8_t proto, const uint8_t *payload, uint16_t plen,
               uint8_t ttl, int pmtudisc, int ifindex)
{
    (void)src;
    (void)dst_ip;
    (void)proto;
    (void)payload;
    (void)plen;
    (void)ttl;
    (void)pmtudisc;
    (void)ifindex;
    CHECK(0, "no row sends on a raw socket");
    return -EINVAL;
}

/** Nothing: no row sends an ICMP message. */
void icmp_count_out(uint8_t type)
{
    (void)type;
}

/** No row sends on a packet socket: -EINVAL. */
int packet_send(struct socket *s, int ifindex, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *data, uint16_t len)
{
    (void)s;
    (void)ifindex;
    (void)dst_mac;
    (void)ethertype;
    (void)data;
    (void)len;
    CHECK(0, "no row sends on a packet socket");
    return -EINVAL;
}

/* What `socket_tcp_send` answers to the calls of a row, the bytes sent or
 * -errno, and the calls made. */
static int32_t stream_answer[2];
static int stream_sends;

/** Answer the row's next call from `stream_answer`, no more than `len`
 * bytes, and -EPIPE past its two. */
int socket_tcp_send(struct socket *s, const uint8_t *buf, int len, int nonblock)
{
    int32_t answer;

    (void)s;
    (void)buf;
    (void)nonblock;
    if (stream_sends >= 2) {
        return -EPIPE;
    }
    answer = stream_answer[stream_sends++];
    return answer > len ? len : answer;
}

/** Read element `i` of the vector at the offset `uvec` of user memory, in
 * the host's layout: 0, or -EFAULT outside it. */
int iov_fetch(struct kernel_iovec *iov, uintptr_t uvec, uint32_t i)
{
    return copy_from_user(iov, (const void __user *)(uvec + i * sizeof(*iov)), sizeof(*iov)) != 0
               ? -EFAULT
               : 0;
}

/** Put a vector of `n` elements of the lengths `lens` at offset 512 of user
 * memory, each element's bytes at a place of its own past offset 600. */
static void stream_vector(const unsigned long *lens, uint32_t n)
{
    struct kernel_iovec v;
    uint32_t i;

    for (i = 0; i < n; i++) {
        v.iov_base = 600 + 64 * i;
        v.iov_len = lens[i];
        k_memcpy(user + 512 + i * sizeof(v), &v, sizeof(v));
    }
}

/**
 * A stream's sendmsg posts SIGPIPE only for an error it returns, as
 * Linux's tcp_sendmsg returns what it copied and sk_stream_error posts the
 * signal: a vector whose first element goes and whose second fails with
 * EPIPE returns the first's count with no signal; one whose first fails
 * posts it, but under MSG_NOSIGNAL; and a vector with no byte, of empty
 * elements or of none, is a send of 0 bytes, which fails after a shutdown
 * as any send does, with the signal.
 */
static void stream_sendmsg(void)
{
    static const unsigned long two[2] = {4, 4}, empty[2] = {0, 0};

    sock.type = SOCK_STREAM;
    entry.type = FD_TYPE_SOCKET;
    entry.sock = &sock;
    entry.ofd = &description;
    description.status_flags = 0;
    stream_vector(two, 2);
    stream_answer[0] = 4;
    stream_answer[1] = -EPIPE;
    stream_sends = 0;
    sigpipes = 0;
    CHECK(sendmsg_stream(3, 512, 2, 0) == 4 && stream_sends == 2 && sigpipes == 0,
          "a sendmsg whose second element fails returns the first's count with no SIGPIPE");
    stream_answer[0] = -EPIPE;
    stream_sends = 0;
    CHECK(sendmsg_stream(3, 512, 2, 0) == -EPIPE && sigpipes == 1,
          "a sendmsg whose first element fails with EPIPE posts SIGPIPE");
    stream_sends = 0;
    CHECK(sendmsg_stream(3, 512, 2, MSG_NOSIGNAL) == -EPIPE && sigpipes == 1,
          "and one under MSG_NOSIGNAL posts none");
    stream_vector(empty, 2);
    stream_sends = 0;
    CHECK(sendmsg_stream(3, 512, 2, 0) == -EPIPE && stream_sends == 1 && sigpipes == 2,
          "a sendmsg of empty elements after a shutdown is EPIPE, with SIGPIPE");
    stream_sends = 0;
    CHECK(sendmsg_stream(3, 512, 0, MSG_NOSIGNAL) == -EPIPE && stream_sends == 1 && sigpipes == 2,
          "and one of no element under MSG_NOSIGNAL is EPIPE with none");
    entry.sock = NULL;
}

/**
 * shutdown(2) of a raw IPv4 or IPv6 socket is Linux's inet_shutdown, as
 * inet_sockraw_ops and inet6_sockraw_ops name it: the halves are taken,
 * ENOTCONN with no peer and 0 with one, and EINVAL for another `how`; a
 * packet socket's sock_no_shutdown is EOPNOTSUPP and takes nothing.
 */
static void raw_shutdown(void)
{
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_RAW;
    sock.family = AF_INET;
    sock.protocol = 1;
    entry.type = FD_TYPE_SOCKET;
    entry.sock = &sock;
    wakes_all = 0;
    CHECK(do_shutdown(3, SHUT_RD) == -ENOTCONN && sock.shutdown == SOCK_RCV_SHUTDOWN &&
              wakes_all == 1,
          "a raw socket with no peer takes SHUT_RD, wakes its waiters and is ENOTCONN");
    sock.connected = 1;
    CHECK(do_shutdown(3, SHUT_WR) == 0 && sock.shutdown == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN),
          "and one with a peer takes SHUT_WR and succeeds");
    CHECK(do_shutdown(3, 3) == -EINVAL, "and another how is EINVAL");
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_RAW;
    sock.family = AF_INET6;
    sock.protocol = 58;
    CHECK(do_shutdown(3, SHUT_RDWR) == -ENOTCONN &&
              sock.shutdown == (SOCK_RCV_SHUTDOWN | SOCK_SEND_SHUTDOWN),
          "a raw IPv6 socket with no peer takes both halves and is ENOTCONN");
    k_memset(&sock, 0, sizeof(sock));
    sock.type = SOCK_DGRAM;
    sock.family = AF_PACKET;
    CHECK(do_shutdown(3, SHUT_RD) == -EOPNOTSUPP && sock.shutdown == 0,
          "a packet socket is EOPNOTSUPP and takes nothing");
    entry.sock = NULL;
}

/** Store `value` at `p` and return what was there: one thread. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

/** 0: no USB disk has work. */
int usbdisk_pending(void)
{
    return 0;
}

int main(void)
{
    proc.tg = &group;
    group.leader = &proc;
    other.tg = &other_group;
    other_group.leader = &other;
    connects();
    queue_full();
    clocks();
    clock_ids();
    clock_sleep_refusals();
    clock_sleeps();
    realtime_steps();
    sleeps_masked();
    sleep_edges();
    sleep_races();
    sleep_held_clock();
    sleep_between();
    timer_first_expiry();
    timer_marked();
    stream_sendmsg();
    raw_shutdown();
    if (failures)
        return 1;
    printf("syscall-test: pass: connect, datagram, sendmsg, shutdown, clocks, sleeps, sleeps "
           "between ticks, steps of real time and timer\n");
    return 0;
}

/** Supply an IPv4 source for the fixture's immediate handshake. */
int net_addr_source(net_addr_t bound, net_addr_t dst, int ifindex, net_addr_t *src)
{
    (void)dst;
    (void)ifindex;
    *src = bound;
    return 0;
}

/** The syscall fixture has no interfaces or IPv6 link-scoped endpoints. */
struct netdev *netdev_at(int n)
{
    (void)n;
    return 0;
}
