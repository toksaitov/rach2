#include "sys/arch/arch.h"
#include "kernel.h"

#include "sys/kio.h"
#include "sys/lib/kerrno.h"
#include "sys/fpu.h"
#include "sys/klog.h"
#include "sys/serial.h"
#include "sys/syscall.h"
#include "sys/module.h"
#include "sys/cmdline.h"
#include "sys/irq.h"
#include "sys/mm/heap.h"
#include "sys/mm/paging.h"
#include "sys/mm/mm.h"
#include "sys/proc/tgroup.h"
#include "sys/proc/ptrace.h"
#include "sys/proc/coredump.h"
#include "sys/mm/kheap.h"
#include "sys/mm/frame.h"
#include "sys/mm/pagecache.h"
#include "sys/mm/modlayout.h"
#include "sys/block/mbr.h"
#include "sys/fs/devfs.h"
#include "sys/fs/vfs.h"
#include "sys/fs/ext2.h"
#include "sys/proc/process.h"
#include "sys/io/timerfd.h"
#include "sys/tty/tty.h"
#include "sys/tty/pty.h"
#include "sys/ipc/pipe.h"
#include "sys/proc/sched.h"
#include "sys/power.h"
#include "sys/proc/elf.h"
#include "sys/proc/uimage.h"
#include "sys/lib/krandom.h"
#include "sys/lib/ksignal.h"
#include "sys/rtc.h"
#include "sys/fs/krffs.h"
#include "sys/timer.h"
#include "sys/platform.h"
#include "sys/boot.h"
#include "sys/latency.h"
#include "sys/input.h"
#include "sys/usb/usb.h"
#include "sys/block/block.h"
#include "sys/block/ahci.h"
#include "sys/block/nvme.h"
#include "sys/audio/audio.h"
#include "sys/audio/audio_debug.h"
#include "sys/audio/hda.h"
#include "sys/udelay.h"
#include "sys/network/net.h"
#include "sys/network/netconf.h"
#include "sys/block/diskcmd.h"
#include "sys/block/diskinfo.h"
#include "sys/block/disk.h"
#include "sys/block/recovery.h"
#include "sys/network/driver/netdev.h"
#include "sys/network/netconsole.h"
#include "sys/network/ip/icmp.h"
#include "sys/network/ip/ip.h"
#include "sys/network/tcp/tcp.h"
#include "sys/lib/kint.h"
#include "sys/lib/kstring.h"

/* The boot stack and the page below it, from the architecture's entry
 * (sys/arch/i686/loader.s). */
extern char stack[];
extern char stack_guard[];
#define KERNEL_STACK_SIZE 0x4000

/* The stack the debug shell's ring-3 test runs on, reserved when it runs. */
#define USER_STACK_SIZE 0x1000

#define CMD_BUFFER_SIZE 64
static char cmd_buffer[CMD_BUFFER_SIZE];
static volatile size_t cmd_pos = 0;
static volatile bool cmd_ready = false;
static volatile bool kernel_shell_active = false;

/* Free-running tick counter, bumped at 100 Hz by the timer IRQ. */
volatile uint32_t kernel_ticks = 0;

/*
 * Whole seconds since boot, and the ticks into the current one, carried beside
 * the tick counter so the clock does not have to divide a wrapping 32-bit tick
 * count. `kernel_ticks` wraps after about 497 days, and a clock that read the
 * elapsed ticks as `now - anchor` would jump back by that whole cycle at the
 * wrap. Seconds wrap after 136 years instead, which outlives the machine.
 */
volatile uint32_t kernel_uptime_seconds = 0;
volatile uint32_t kernel_second_ticks = 0;

/* PID of the init process, or 0 when none runs. */
volatile uint32_t init_pid = 0;

/* Set once from the multiboot kernel command line (rach2.rescue / init=rescue).
 * Rescue mode skips the persistent init and boots straight to the kernel debug
 * shell, the developer/test home; a normal boot runs /sbin/init instead. */
static bool rescue_mode = false;

/* True once the kernel debug shell becomes the supervisor: rescue mode, or a
 * normal boot whose image has no init (the test image). It decides what the
 * death of an init-role process MEANS: when the debug shell is home, an exited
 * init returns to it (today's behavior, which the test harness drives); when a
 * real /sbin/init is PID 1 (debug shell NOT home), init's death is a panic. */
bool debug_shell_is_home = false;

volatile process_t *stdin_waiting_proc = NULL;

static void check_timing(void);
static void check_strings(void);
void shell_loop(void);
void shell_reset_input(void);
void panic_init_died(uint32_t pid); /* called from syscall.c too; never returns */
static void process_command(const char *cmd);
static void timer_handler(isr_params_t *params);
static bool str_equal(const char *a, const char *b);
static void boot_init_process(void);

/**
 * Unmap the page below the boot stack, its guard (loader.s), so that the
 * boot, process 0, the debug shell and the programs they start fault when
 * they run the stack past its base, as a thread does below a stack
 * `kstack_alloc` gives. The page is part of the kernel image, whose frames
 * no allocator gives, and the identity window's page tables are shared by
 * every directory, so it stays unmapped everywhere. A linker that did not
 * align it to a page would have put other data in that page, and the boot
 * stops there by name rather than unmap it.
 */
static void guard_boot_stack(void)
{
    if ((uintptr_t)stack_guard & (PAGE_SIZE - 1)) {
        k_panic("boot: the boot stack's guard is not a page of its own");
    }
    unmap_kernel_frame((uintptr_t)stack_guard);
}

/**
 * Map every boot module into the module region of the kernel's layout,
 * page by page from wherever the loader put it, and point its record at
 * the mapping; the physical address stays in `phys`. The region is 64 MiB,
 * a bound of this layout, and a set of modules that does not fit stops the
 * boot by name rather than reading a module in part.
 */
static void map_modules(void)
{
    uintptr_t next = KVA_MODULES;
    uint32_t m;

    for (m = 0; m < module_count; m++) {
        module_info_t *mod = &module_table[m];
        uintptr_t first = mod->phys & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t last = (mod->phys + mod->size + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t off;

        if (mod->size == 0) {
            continue;
        }
        if (last - first > KVA_MODULES + KVA_MODULES_SIZE - next) {
            k_panic("modules: more than the 64 MiB module region holds");
        }
        for (off = 0; off < last - first; off += PAGE_SIZE) {
            map_kernel_frame(next + off, first + off, 1);
        }
        mod->start = (uint32_t)(next + (mod->phys - first));
        mod->end = mod->start + mod->size;
        next += last - first;
    }
}

/**
 * One memory-map entry as text, `START-END TYPE` with 64-bit hexadecimal
 * addresses, since the kernel's formatter prints 32-bit words and a map on
 * a 4 GiB machine reaches past them.
 */
static void memory_map_line(char *buf, uint32_t buflen, uint64_t addr, uint64_t len, uint32_t type)
{
    static const char digits[] = "0123456789ABCDEF";
    uint64_t v[2];
    uint32_t pos = 0, k;
    int shift;

    v[0] = addr;
    v[1] = addr + len;
    for (k = 0; k < 2 && pos + 20 < buflen; k++) {
        if (k == 1) {
            buf[pos++] = '-';
        }
        buf[pos++] = '0';
        buf[pos++] = 'x';
        for (shift = 60; shift >= 0; shift -= 4) {
            buf[pos++] = digits[(v[k] >> shift) & 0xF];
        }
    }
    if (pos + 12 < buflen) {
        const char *name = type == 1   ? " available"
                           : type == 2 ? " reserved"
                           : type == 3 ? " acpi"
                           : type == 4 ? " nvs"
                           : type == 5 ? " bad"
                                       : " other";

        for (k = 0; name[k] && pos + 1 < buflen; k++) {
            buf[pos++] = name[k];
        }
    }
    buf[pos] = '\0';
}

static bool test_timer(void);
static bool test_udelay(uint32_t *measured_ms);
static bool test_clock(uint32_t *measured_us, const char **why, uint32_t *walk_clock,
                       uint32_t *walk_tick);
static bool test_usermode(void);
static void test_switch(void);
static void test_preempt(void);
static void exec_program(int argc, char *argv[]);

/**
 * Service the 100 Hz timer IRQ and preempt the current process.
 *
 * Bumps the global tick, then runs the periodic kernel work that the timer
 * drives: waking expired nanosleep sleepers, the HDA audio tick (needed when
 * legacy IRQs are not delivered on MSI-only setups), the TCP timers, the
 * loopback delivery, and the interrupt-less console serial drain. Finally
 * Accounts the tick as idle or busy FIRST, before anything below can wake a
 * process: the marker names what the processor was doing when this tick
 * arrived, not what it is about to do, and a busy tick is charged to the
 * process it interrupted, as user time when the interrupted code segment is
 * ring 3's and as system time otherwise. Then reschedules through
 * `sched_preempt`: at once when the current process is not inside a system
 * call, and at the call's exit when it is, since timer IRQs stay enabled
 * during a call so HDA streaming keeps running, yet a switch in the middle of
 * one is unsafe. Kernel threads (no current_proc, or one not in_syscall)
 * are switched at once.
 */
static void timer_handler(isr_params_t *params)
{
    uint32_t lost = timer_lost_ticks();

    ++kernel_ticks;
    /* The microsecond clock's periods are this tick's to count; see
       `timer_now_us`. */
    timer_tick_clock();
    latency_tick();
    if (++kernel_second_ticks >= 100) {
        kernel_second_ticks = 0;
        ++kernel_uptime_seconds;
    }
    while (lost-- > 0) {
        ++kernel_ticks;
        if (++kernel_second_ticks >= 100) {
            kernel_second_ticks = 0;
            ++kernel_uptime_seconds;
        }
    }

    sched_account_tick(FRAME_USER(params) != 0);
    sched_load_tick();

    /* Runs regardless of in_syscall, so a sleeper parked inside its syscall
     * still becomes runnable on time. */
    proc_wake_sleepers(kernel_ticks);
    timerfd_tick(kernel_ticks);

    hda_tick();
    /* A polled network device gets its turn here, before the layers above are
     * given theirs, so a frame that arrived since the last tick is delivered
     * in the same tick that acts on it. A device that raises an interrupt
     * instead has no poll and this costs it a null check. Running from the
     * timer handler means interrupts are already off, which is the discipline
     * the stack's shared buffers require. */
    netdev_poll();
    tcp_tick();
    ip_loop_drain();
    /* The USB controller's turn, for the same reason and under the same rule:
     * this takes what the controller has left in its event ring and returns.
     * Nothing on this path waits on hardware. */
    usb_poll();
    block_lag_tick();
    block_request_tick();
    platform_tick();
    tty_console_poll();

    sched_preempt();
}

/**
 * Leave the core file of the current process, which the signal `signo`
 * ends, when it is dumpable and its soft RLIMIT_CORE allows a page: the
 * status it ends with, which has WCOREFLAG, as WCOREDUMP reads it, only when
 * the whole file was written. The caller has set `in_syscall` and enabled
 * interrupts, as a system call writing the file would: the timer does not
 * switch away in the middle of the file system's work, and a disk that
 * answers only by interrupt, as a USB stick does, can be waited for.
 */
static int32_t terminate_core(int32_t signo)
{
    if (!current_proc->mm->dumpable || current_proc->rlim[RLIM_CORE].cur < PAGE_SIZE) {
        return signo;
    }
    if (fs_core_file(signo, current_proc->rlim[RLIM_CORE].cur) != 0) {
        return signo;
    }
    current_proc->tg->exit_code = signo | WCOREFLAG;
    return signo | WCOREFLAG;
}

/**
 * Release what the end of process `pid`, the current one, gives up beyond
 * its threads: the console its session leads, the pty slaves that session
 * controls, and the framebuffer it owns, so a later session can claim them.
 */
static void terminate_release_session(uint32_t pid)
{
    if (tty_console_owned_by_leader((int)current_proc->sid, (int)pid)) {
        tty_set_session(&console_tty, 0, 0);
    }
    pty_release_leader((int)current_proc->sid, (int)pid);
    fb_release_if_owner(pid);
}

/**
 * Print the line the probe runner scores for init `pid`, which ended with
 * its process's wait status `status`: sys_exit's for an exit, terminate's
 * for a signal. The status is the process's, so a thread that exit_group's
 * SIGKILL ends last reports the group's exit.
 */
static void init_end_line(uint32_t pid, int32_t status)
{
    if ((status & 0x7F) == 0) {
        klog(KLOG_INFO, "sys_exit",
             "init process %d exited with status %d, returning to kernel shell", pid,
             (status >> 8) & 0xFF);
    } else {
        klog(KLOG_INFO, "terminate",
             "init process %d killed by signal %d, returning to kernel shell", pid, status & 0x7F);
    }
}

/**
 * Terminate the current user process by signal and give up its CPU.
 *
 * Shared teardown for every death by signal: a signal whose default action
 * ends the process, and a signal frame that cannot be built or read back. The
 * process dies by signal `signo`; the exit code stores the Linux wait-status
 * encoding of a signal death (low 7 bits = the signal), so the parent's
 * waitpid sees WIFSIGNALED/WTERMSIG. With WCOREFLAG in `signo` the process
 * leaves a core file first (`terminate_core`), unless another thread has
 * begun to end it, after the other threads are sent SIGKILL and have ended
 * (`core_write` waits for them) and before this thread releases anything,
 * as Linux's `do_coredump` does. The teardown runs as a system call does,
 * with `in_syscall` set and interrupts on, also when the signal is acted on
 * at an interrupt's return, which comes here with them off: what it
 * releases and writes may sleep in a disk that answers only by interrupt,
 * and the timer must not switch away in the middle. Interrupts go off again
 * once the process's end is published, before its record can be freed or
 * the processor given up. The whole process dies:
 * its other threads are sent SIGKILL, and the last thread to end publishes
 * the death. This thread releases the descriptor table and its reference to
 * the address space, switching off it before its page directory can be
 * freed; the last thread then posts SIGCHLD and wakes a parent blocked in
 * waitpid. A dying init panics through panic_init_died, unless the debug
 * shell is the supervisor, in which case the shell's input state is reset and
 * this jumps back into shell_loop on a fresh kernel stack. Any other process
 * stays a zombie for the parent's waitpid to collect, and the now-READY parent
 * is scheduled. Does not return for a user process.
 */
void terminate_current_process(int32_t signo)
{
    bool is_init;
    uint32_t pid;
    int core;
    int alone;
    int last;

    /* Only user processes have an address space; ignore the kernel
     * idle/shell context. */
    if (!current_proc || current_proc->mm == NULL) {
        return;
    }

    ptrace_event(PTRACE_EVENT_EXIT, (uint32_t)(signo & 0x7F));
    pid = current_proc->tgid;
    is_init = (pid == init_pid && init_pid != 0);
    current_proc->in_syscall = 1;
    arch_irq_enable();

    /* The signal ends the whole process: the other threads are sent SIGKILL,
     * and the process ends with this signal whichever thread ends last. */
    core = (signo & WCOREFLAG) && !current_proc->tg->exiting && !current_proc->tg->exec_task;
    signo &= 0x7F;
    proc_group_exit(signo);
    if (core) {
        signo = terminate_core(signo);
    }
    alone = current_proc->tg->live == 1;

    /* If the console-owning session leader is dying (init, or a later TIOCSCTTY
     * leader, not only init), release the controlling terminal before
     * zombifying so the timer-IRQ serial drain does not target this vanishing
     * session during teardown. shell_reset_input on the is_init path below
     * releases again, which is idempotent. */
    if (alone) {
        terminate_release_session(pid);
    }

    last = proc_exit_cleanup(signo);
    arch_irq_disable();

    if (!last) {
        for (;;) {
            arch_irq_disable();
            schedule();
            idle_wait_irq_off();
        }
    }
    if (!alone) {
        terminate_release_session(pid);
    }

    if (is_init) {
        /* A real /sbin/init killed by a signal is a fatal init death; only when
         * the debug shell is the supervisor (rescue, or the no-init test image)
         * does an exited init return to it. */
        if (!debug_shell_is_home) {
            panic_init_died(pid); /* never returns */
        }
        /* Same resume path as when init exits normally; see sys_exit. */
        init_end_line(pid, current_proc->tg->exit_code);
        console_puts("Init process exited. Returning to kernel debug shell.");

        free_process(current_proc);

        init_pid = 0;
        current_proc = proc_idle;
        current_proc->state = PROC_RUNNING;

        /* Fresh command line and dropped stdin-wait state, before "sti" below
         * re-enables IRQs. */
        shell_reset_input();

        sched_disable();

        arch_run_on_stack((uintptr_t)stack + KERNEL_STACK_SIZE, shell_loop);
    } else {
        schedule();
        k_panic("a dying process was scheduled again");
    }
}

/**
 * Deliver SIGINT for a PS/2-keyboard Ctrl+C to the console's foreground group.
 *
 * The target is the controlling terminal's foreground process group, the
 * same one the serial ^C reaches through the line discipline, not
 * current_proc: inside this keyboard IRQ the running process may be
 * unrelated. With no foreground group yet, falls back to the running user
 * process only when it belongs to the console's session, so a lone program
 * is still interruptible while a stray ^C never signals an unrelated daemon
 * that happened to be running.
 */
static void kill_foreground_process(void)
{
    console_puts("^C");
    if (console_tty.fg_pgid != 0) {
        tty_signal_group(console_tty.fg_pgid, SIGINT);
    } else if (current_proc && current_proc->space_root != 0 &&
               (int)current_proc->sid == console_tty.sid) {
        signal_post(current_proc, SIGINT);
    }
}

/*
 * What the input layer needs from the debug shell: whether the shell is the
 * one reading, a character for its buffer, and the out-of-band interrupt it
 * takes where there is no session to take it through the line discipline.
 */
int shell_reads_input(void)
{
    return kernel_shell_active && stdin_waiting_proc == NULL;
}

void shell_interrupt(void)
{
    kill_foreground_process();
}

void shell_input_char(char c)
{
    if (!kernel_shell_active) {
        return;
    }
    if (c == '\n') {
        if (k_scrollback_is_active()) {
            k_scrollback_end();
        }
        cmd_buffer[cmd_pos] = '\0';
        cmd_ready = true;
        console_putc('\n');
        return;
    }
    if (c == '\b') {
        if (k_scrollback_is_active()) {
            k_scrollback_end();
        }
        if (cmd_pos > 0) {
            --cmd_pos;
            console_putc('\b');
            console_putc(' ');
            console_putc('\b');
        }
        return;
    }
    if (cmd_pos < CMD_BUFFER_SIZE - 1) {
        if (k_scrollback_is_active()) {
            k_scrollback_end();
        }
        cmd_buffer[cmd_pos++] = c;
        console_putc(c);
    }
}

static bool str_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix)
            return false;
        ++str;
        ++prefix;
    }
    return true;
}

static bool test_timer(void)
{
    uint32_t start_ticks;
    uint32_t i;

    start_ticks = kernel_ticks;

    for (i = 0; i < 5; ++i) {
        arch_halt();
    }

    return kernel_ticks > start_ticks;
}

/**
 * Check that `k_udelay` waits for as long as it is asked to.
 *
 * The delay measures itself against the 8254's counter; the timer interrupt
 * counts its own 100 Hz reloads. The two arrive at a duration by different
 * arithmetic over the same chip, so a disagreement is an error in one of them,
 * and the device bring-up sequences that ask for microseconds are only as good
 * as this. The wait is 200 ms, twenty ticks, so one tick of quantization is
 * five percent of the reading, well inside the band below and short enough not
 * to lengthen the boot noticeably. `*measured_ms` reports what was seen, since
 * a wrong figure says more than a failed comparison.
 *
 * The band is 150 to 350 ms, since a delay off by half or double is what a
 * bring-up sequence would notice. The reading is the shortest of a few waits,
 * because a host with more machines than processors stops this one part way
 * through a wait; under a hypervisor that then delivers the ticks it
 * missed, every wait reads long, which is why the result is printed as a
 * warning and is no longer the record's verdict.
 */
static bool test_udelay(uint32_t *measured_ms)
{
    uint32_t best = 0xFFFFFFFFu;
    int attempt;

    /* The shortest of a few waits is the reading, which is what the clock
     * test beside this one does and for the same reason: a host with more
     * machines on it than processors stops this one part way through a wait,
     * and the ticks it was not running for are delivered afterwards, so the
     * wait is counted long. Nothing can make a wait read short. One wait in
     * the band ends this, so an unloaded boot pays for one. */
    for (attempt = 0; attempt < 5; attempt++) {
        uint32_t start = kernel_ticks;
        uint32_t elapsed;

        k_udelay(200000);
        elapsed = kernel_ticks - start;
        if (elapsed < best) {
            best = elapsed;
        }
        if (best >= 15 && best <= 35) {
            break;
        }
    }
    *measured_ms = best * 10;
    return best >= 15 && best <= 35;
}

/**
 * The microsecond clock against the wait that is measured a different way.
 *
 * `timer_now_us` counts the periods of the 8254's counter and reads where it is
 * inside one; `k_udelay` measures that same counter by its drop. The wait is
 * taken eight times and the shortest of them is the reading: an interrupt in
 * the middle of one can only add to it, and this machine writes whole lines
 * to a serial port at eighty-seven microseconds a byte.
 *
 * Then fifty milliseconds are walked in single milliseconds, which is five of
 * the tick's periods and five of the counter's sweeps. A reading that lost
 * which half of the period it was in, or that did not count a wrap, steps
 * backwards there.
 */
static bool test_clock(uint32_t *measured_us, const char **why, uint32_t *walk_clock,
                       uint32_t *walk_tick)
{
    uint32_t best = 0xFFFFFFFFu;
    uint32_t previous;
    int attempt;
    int i;

    *why = "";
    *walk_clock = 0;
    *walk_tick = 0;

    for (i = 0; i < 8; i++) {
        uint32_t start = timer_now_us();
        uint32_t seen;

        k_udelay(2000);
        seen = timer_now_us() - start;
        if (seen < best) {
            best = seen;
        }
    }
    *measured_us = best;
    if (best < 1500 || best > 3500) {
        *why = "the shortest of eight 2 ms waits is outside 1500 to 3500 us";
        return false;
    }

    /*
     * Fifty milliseconds walked in single milliseconds, with the clock read
     * at every step: five of the tick's periods and ten of the counter's
     * sweeps. A reading that steps backwards fails here. So does one that
     * runs fast, because the same fifty milliseconds are measured by the tick
     * beside it, and a clock that counted a sweep as a period, or counted a
     * period twice, would put twice the time in between.
     *
     * The walk is taken up to three times and passes when one agrees with
     * the tick. Under an emulator whose host is busy, a tick can arrive late
     * or not at all inside one walk, which the comparison cannot tell from
     * a slow clock; a clock that is fast or slow disagrees on every walk.
     */
    for (attempt = 0; attempt < 3; attempt++) {
        uint32_t ticks_before = kernel_ticks;
        uint32_t clock_before = timer_now_us();
        uint32_t by_clock, by_tick;
        int stepped_back = 0;

        previous = clock_before;
        for (i = 0; i < 50; i++) {
            uint32_t now = timer_now_us();

            if (now < previous) {
                stepped_back = 1;
                break;
            }
            previous = now;
            k_udelay(1000);
        }
        /* One backwards reading ends this walk and not the check, for the
           reason the three walks exist: an interrupt taken between two reads
           of the counter has been seen to give one backwards reading on this
           laptop and under a busy emulator, and a clock that really runs
           backwards does it in every walk. */
        if (stepped_back) {
            *why = "three walks each saw the clock step backwards";
            continue;
        }
        by_clock = timer_now_us() - clock_before;
        by_tick = (kernel_ticks - ticks_before) * 10000u;
        *walk_clock = by_clock;
        *walk_tick = by_tick;
        if (by_tick != 0 && by_clock >= by_tick - by_tick / 4u &&
            by_clock <= by_tick + by_tick / 4u) {
            return true;
        }
        *why = "three walks each disagreed with the tick by more than a quarter";
    }
    return false;
}

/**
 * Prove ring 3 works by entering user mode and running user_test_entry.
 *
 * Reserves a user stack and marks the code, message, and stack pages
 * user-accessible, then enters ring 3 at the architecture's test payload,
 * which writes "Hello from ring 3!" through a system call. The entry does not
 * return, so the trailing `return false` is unreachable.
 */
static bool test_usermode(void)
{
    uintptr_t func_addr = (uintptr_t)arch_user_test_entry;
    uintptr_t msg_addr = (uintptr_t)arch_user_test_msg;
    uintptr_t user_stack_addr;
    uintptr_t page;
    uint32_t i;

    console_printf("  Usermode test: func=0x%X msg=0x%X\n", func_addr, msg_addr);

    user_stack_addr = mem_reserve(USER_STACK_SIZE, true);
    console_printf("  Usermode test: stack=0x%X\n", user_stack_addr);

    /* The function code may span two pages. */
    for (page = func_addr & ~0xFFF; page <= ((func_addr + 256) & ~0xFFF); page += PAGE_SIZE) {
        paging_set_user(page, true);
    }

    paging_set_user(msg_addr, true);

    for (page = user_stack_addr; page < user_stack_addr + USER_STACK_SIZE; page += PAGE_SIZE) {
        paging_set_user(page, true);
    }

    console_printf("  Usermode test: pages marked user-accessible\n");

    for (i = 0; i < 3; i++) {
        arch_halt();
    }

    console_printf("  Usermode test: entering ring 3...\n");

    arch_enter_user(func_addr, user_stack_addr + USER_STACK_SIZE);

    return false;
}

/* Context-switch test state, shared between the two cooperating processes. */
static process_t *switch_proc_a = NULL;
static process_t *switch_proc_b = NULL;
static volatile int switch_count = 0;

/* Test process B: print 'B' and switch back to A until the count is reached. */
static void proc_b_entry(void)
{
    while (switch_count < 10) {
        console_putc('B');
        ++switch_count;
        switch_context(switch_proc_b, switch_proc_a);
    }

    switch_context(switch_proc_b, switch_proc_a);

    for (;;) {
        arch_halt();
    }
}

static void test_switch(void)
{
    uintptr_t stack_b;

    console_puts("Testing context switch between two processes...");
    console_puts("Expected output: ABABABABAB done");
    console_printf("Result:          ");

    switch_proc_a = current_proc;

    switch_proc_b = alloc_process();
    if (!switch_proc_b) {
        console_puts("Failed to allocate process B");
        return;
    }

    stack_b = mem_reserve(KERNEL_STACK_SIZE, true);
    switch_proc_b->kernel_stack = stack_b + KERNEL_STACK_SIZE;
    switch_proc_b->space_root = 0; /* shares the kernel page directory */
    switch_proc_b->parent_pid = switch_proc_a->pid;

    setup_process_stack(switch_proc_b, proc_b_entry);

    console_printf("Process B created: pid=%d stack=0x%X\n", switch_proc_b->pid,
                   switch_proc_b->kernel_stack);

    switch_count = 0;

    while (switch_count < 10) {
        console_putc('A');
        ++switch_count;
        switch_context(switch_proc_a, switch_proc_b);
    }

    console_puts("");

    free_process(switch_proc_b);
    switch_proc_b = NULL;

    set_fore_color(light_green);
    console_puts("Context switch test passed!");
    set_fore_color(light_grey);
}

/* Preemptive-scheduling test state, shared between the two processes. */
static volatile int preempt_done = 0;
static volatile int preempt_count_a = 0;
static volatile int preempt_count_b = 0;

/* Test process A: print 'A' in a busy loop until the pair has printed 20. The
 * busy delay must outlast a timer tick (10 ms at 100 Hz) so the preemption is
 * observable. The B variant below is identical. */
static void preempt_proc_a(void)
{
    /* The process starts mid-interrupt, with IRQs masked. */
    arch_irq_enable();

    while (!preempt_done) {
        console_putc('A');
        ++preempt_count_a;

        {
            volatile int i;
            for (i = 0; i < 5000000; i++) {
                arch_relax();
            }
        }

        if (preempt_count_a + preempt_count_b >= 20) {
            preempt_done = 1;
        }
    }

    for (;;) {
        arch_halt();
    }
}

static void preempt_proc_b(void)
{
    /* The process starts mid-interrupt, with IRQs masked. */
    arch_irq_enable();

    while (!preempt_done) {
        console_putc('B');
        ++preempt_count_b;

        {
            volatile int i;
            for (i = 0; i < 5000000; i++) {
                arch_relax();
            }
        }

        if (preempt_count_a + preempt_count_b >= 20) {
            preempt_done = 1;
        }
    }

    for (;;) {
        arch_halt();
    }
}

static void test_preempt(void)
{
    process_t *proc_a;
    process_t *proc_b;
    uintptr_t stack_a, stack_b;

    console_puts("Testing preemptive round-robin scheduling...");
    console_puts("Two processes will print A and B, preempted by timer.");
    console_puts("Expected: interleaved As and Bs (e.g., AABBABAB...)");
    console_printf("Result:   ");
    console_printf("Result:   ");

    preempt_done = 0;
    preempt_count_a = 0;
    preempt_count_b = 0;

    proc_a = alloc_process();
    if (!proc_a) {
        console_puts("Failed to allocate process A");
        return;
    }
    stack_a = mem_reserve(KERNEL_STACK_SIZE, true);
    proc_a->kernel_stack = stack_a + KERNEL_STACK_SIZE;
    proc_a->space_root = 0;
    proc_a->parent_pid = current_proc->pid;
    setup_process_stack(proc_a, preempt_proc_a);

    proc_b = alloc_process();
    if (!proc_b) {
        free_process(proc_a);
        console_puts("Failed to allocate process B");
        return;
    }
    stack_b = mem_reserve(KERNEL_STACK_SIZE, true);
    proc_b->kernel_stack = stack_b + KERNEL_STACK_SIZE;
    proc_b->space_root = 0;
    proc_b->parent_pid = current_proc->pid;
    setup_process_stack(proc_b, preempt_proc_b);

    sched_enable();

    while (!preempt_done) {
        arch_halt();
    }

    sched_disable();

    console_puts("");
    console_printf("\n");
    console_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);
    console_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);

    free_process(proc_a);
    free_process(proc_b);

    if (preempt_count_a > 0 && preempt_count_b > 0) {
        set_fore_color(light_green);
        console_puts("Preemptive scheduling test passed!");
        console_printf("Preemptive scheduling test passed!\n");
    } else {
        set_fore_color(light_red);
        console_puts("Preemptive scheduling test FAILED - one process didn't run!");
        console_printf("Preemptive scheduling test FAILED!\n");
    }
    set_fore_color(light_grey);
}

/*
 * Memory-isolation test state. The two processes write different values to the
 * same virtual address through different page directories; isolation holds if
 * each reads back its own value.
 *
 * The address is 0xD00000 (13 MB), inside the 32 MiB the kernel maps by
 * identity. A private mapping there still gives each process its own page
 * table: ensure_user_table copies a table the kernel directory shares before
 * a user page goes into it, so the two processes do not write through one.
 */
#define ISOLATION_TEST_ADDR 0xD00000
static volatile uint32_t isolation_result_a = 0;
static volatile uint32_t isolation_result_b = 0;
static process_t *isolation_proc_a = NULL;
static process_t *isolation_proc_b = NULL;
static process_t *isolation_proc_main = NULL;

/* Isolation process A: write 0xAAAAAAAA, let B run, then verify A's value
 * survived and return to main. */
static void isolation_proc_a_entry(void)
{
    volatile uint32_t *ptr = (volatile uint32_t *)ISOLATION_TEST_ADDR;

    *ptr = 0xAAAAAAAA;

    switch_context(isolation_proc_a, isolation_proc_b);

    isolation_result_a = *ptr;

    switch_context(isolation_proc_a, isolation_proc_main);

    for (;;) {
        arch_halt();
    }
}

/* Isolation process B: write 0xBBBBBBBB to its own physical page, read it
 * back, and switch to A. */
static void isolation_proc_b_entry(void)
{
    volatile uint32_t *ptr = (volatile uint32_t *)ISOLATION_TEST_ADDR;

    *ptr = 0xBBBBBBBB;

    isolation_result_b = *ptr;

    switch_context(isolation_proc_b, isolation_proc_a);

    for (;;) {
        arch_halt();
    }
}

static bool test_isolation(void)
{
    page_dir_t *dir_a, *dir_b;
    uintptr_t stack_a, stack_b;
    uintptr_t phys_a, phys_b;

    isolation_result_a = 0;
    isolation_result_b = 0;

    isolation_proc_main = current_proc;

    dir_a = clone_page_directory();
    dir_b = clone_page_directory();
    if (!dir_a || !dir_b) {
        return false;
    }

    /* Same virtual address in each directory must map distinct physical
     * pages. */
    phys_a = alloc_user_page(dir_a, ISOLATION_TEST_ADDR);
    phys_b = alloc_user_page(dir_b, ISOLATION_TEST_ADDR);
    if (!phys_a || !phys_b || phys_a == phys_b) {
        return false;
    }

    isolation_proc_a = alloc_process();
    if (!isolation_proc_a) {
        return false;
    }
    stack_a = mem_reserve(KERNEL_STACK_SIZE, true);
    isolation_proc_a->kernel_stack = stack_a + KERNEL_STACK_SIZE;
    isolation_proc_a->space_root = get_page_dir_phys(dir_a);
    isolation_proc_a->page_dir = dir_a;
    isolation_proc_a->parent_pid = isolation_proc_main->pid;
    setup_process_stack(isolation_proc_a, isolation_proc_a_entry);

    isolation_proc_b = alloc_process();
    if (!isolation_proc_b) {
        free_process(isolation_proc_a);
        return false;
    }
    stack_b = mem_reserve(KERNEL_STACK_SIZE, true);
    isolation_proc_b->kernel_stack = stack_b + KERNEL_STACK_SIZE;
    isolation_proc_b->space_root = get_page_dir_phys(dir_b);
    isolation_proc_b->page_dir = dir_b;
    isolation_proc_b->parent_pid = isolation_proc_main->pid;
    setup_process_stack(isolation_proc_b, isolation_proc_b_entry);

    switch_context(isolation_proc_main, isolation_proc_a);

    free_process(isolation_proc_a);
    free_process(isolation_proc_b);
    isolation_proc_a = NULL;
    isolation_proc_b = NULL;

    return (isolation_result_a == 0xAAAAAAAA && isolation_result_b == 0xBBBBBBBB);
}

/* elf_read_fn over a KRFFS file; ctx is the path. Used by the segment
 * loader. */
static int exec_krffs_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    const struct exec_krffs_ctx *c = ctx;

    return krffs_read(c->name, (char *)dst, count, off);
}

/* The program the boot or the debug shell runs, read through the root's
 * mount when that root is not KRFFS: the node held from the resolution and
 * opened once for the load. One such exec runs at a time. */
static struct boot_exec_ctx {
    struct vfs_ref ref;
    void *handle;
} boot_ctx;

static int boot_vfs_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    struct boot_exec_ctx *c = ctx;
    int32_t got = vfs_read(c->ref.mnt, c->handle, dst, count, off);

    return got < 0 ? -1 : (int)got;
}

/**
 * Keep the program `exec_program` loaded from `path` with its address space
 * `mm`, as an execve's image keeps its file (`mm_exe_open`): the node the
 * load read through on a root that is not KRFFS, and the node `path` names
 * on the KRFFS root. 0, or -1 for no node or no room.
 */
static int boot_keep_exe(struct mm *mm, const char *path)
{
    struct vfs_ref exe = boot_ctx.ref;

    if (!exe.mnt) {
        exe.mnt = vfs_root_mount();
        exe.node = krffs_resolve(path);
    }
    return exe.node && mm_exe_open(mm, &exe) == 0 ? 0 : -1;
}

static void boot_exec_done(void)
{
    if (boot_ctx.ref.mnt) {
        vfs_release(boot_ctx.ref.mnt, boot_ctx.handle);
        vfs_ref_release(&boot_ctx.ref);
        boot_ctx.ref.mnt = 0;
        boot_ctx.handle = 0;
    }
}

/**
 * Build the initial stack of the program `exec_program` starts, in `mm`:
 * its path, the environment `envp` and the `argc` strings of `argv`, laid
 * down as execve lays them (`proc/uimage.c`) under the default stack limit,
 * and keep in `mm` where the strings are and the auxiliary vector, for
 * /proc.
 *
 * @return the entry stack pointer, with the lowest page the stack took in
 *         `*bottom`; 0 when the strings do not fit or the pages or their
 *         mappings could not be had.
 */
static uintptr_t boot_stack(struct mm *mm, int argc, char *argv[], char *envp[],
                            const elf_image_t *img, uintptr_t *bottom)
{
    struct uimage u;
    static const struct uimage_aux root = {0, 0, 0, 0, 0, 0};
    uint8_t random16[16];
    uint32_t envc = 0;
    uintptr_t sp = 0;
    int rc;
    int i;

    while (envp[envc] != 0) {
        envc++;
    }
    rc = uimage_start(&u, USER_STACK_BASE + PAGE_SIZE, RLIM_STACK_DEFAULT, (uint32_t)argc, envc);
    if (rc == 0) {
        rc = uimage_push(&u, argv[0]);
    }
    for (i = (int)envc - 1; i >= 0 && rc == 0; i--) {
        rc = uimage_push(&u, envp[i]);
    }
    for (i = argc - 1; i >= 0 && rc == 0; i--) {
        rc = uimage_push(&u, argv[i]);
    }
    if (rc == 0) {
        rc = uimage_place(&u, (uint32_t)argc, envc);
    }
    krandom_bytes(random16, 16);
    if (rc == 0 && uimage_write(&u, (uint32_t)argc, envc, img, random16, &root) == 0 &&
        uimage_map(&u, mm->dir) == 0) {
        sp = u.sp;
        *bottom = uimage_bottom(&u);
        mm->arg_start = u.arg_start;
        mm->arg_end = u.env_start;
        mm->env_start = u.env_start;
        mm->env_end = u.env_end;
        k_memcpy(mm->auxv, u.auxv, sizeof(u.auxv));
    }
    uimage_release(&u);
    return sp;
}

/**
 * Load and run an ELF program, entering ring 3 at its entry point.
 *
 * The program comes from the KRFFS root, or through the mounted root's file
 * system when that root is not KRFFS. Clones a fresh page directory, streams
 * the program's segments straight from the file (no whole-file buffer, so
 * binary size is unbounded) and keeps the file with the address space
 * (`boot_keep_exe`), builds the SysV/i386 initial stack (argc, argv, a
 * default envp, and the auxv with 16 random bytes), and allocates the
 * process. The first user program becomes init.
 *
 * Every program but a real `/sbin/init` is made the console's
 * controlling-terminal foreground group, so a job-control shell does not stop
 * itself on startup; init is left without the console for the login sessions
 * it spawns to claim.
 *
 * Enters user mode and does not return. A setup failure prints a diagnostic
 * and returns to the caller, except that a failure to set up the first
 * process's memory regions panics, since there is no caller left to tell.
 */
static void exec_program(int argc, char *argv[])
{
    const char *path;
    uintptr_t entry_point;
    elf_image_t img;
    struct mm *mm;
    process_t *user_proc;
    uintptr_t user_sp;
    uintptr_t stack_bottom = USER_STACK_BASE;
    int init_is_starting;
    struct exec_krffs_ctx kctx;
    static char env_path[] = "PATH=/bin:/usr/bin:/";
    static char env_home[] = "HOME=/";
    static char env_term[] = "TERM=rach2";
    static char *default_envp[] = {env_path, env_home, env_term, 0};

    if (argc < 1 || !argv || !argv[0]) {
        console_puts("exec: No program specified");
        return;
    }

    path = argv[0];

    if (!vfs_root_mounted()) {
        console_puts("exec: No file system mounted");
        return;
    }

    /* The program is read through the root's mount when the root is not
     * KRFFS, held and open until the segments are in; on KRFFS it is
     * streamed by path as before. */
    boot_ctx.ref.mnt = 0;
    if (vfs_root_mount()->type == &vfs_krffs_type) {
        if (krffs_open(path) != 0) {
            console_printf("exec: File not found: %s\n", path);
            return;
        }
    } else {
        if (vfs_resolve(0, path, 0, &boot_ctx.ref, 0, 0) != 0) {
            console_printf("exec: File not found: %s\n", path);
            return;
        }
        if (vfs_open_node(boot_ctx.ref.mnt, boot_ctx.ref.node, 0, &boot_ctx.handle) != 0) {
            vfs_ref_release(&boot_ctx.ref);
            boot_ctx.ref.mnt = 0;
            console_printf("exec: File did not open: %s\n", path);
            return;
        }
    }

    mm = mm_new();
    if (!mm) {
        boot_exec_done();
        console_puts("exec: Failed to clone page directory");
        return;
    }

    kctx.name = path;
    if (elf_load_image(boot_ctx.ref.mnt ? boot_vfs_read : exec_krffs_read,
                       boot_ctx.ref.mnt ? (void *)&boot_ctx : (void *)&kctx, mm->dir, &img) != 0 ||
        boot_keep_exe(mm, path) != 0) {
        boot_exec_done();
        mm_put(mm);
        console_puts("exec: Failed to load ELF segments");
        return;
    }
    boot_exec_done();
    entry_point = img.entry;

    user_sp = boot_stack(mm, argc, argv, default_envp, &img, &stack_bottom);
    if (user_sp == 0) {
        mm_put(mm);
        console_puts("exec: the initial stack could not be built");
        return;
    }

    user_proc = alloc_process();
    if (!user_proc) {
        mm_put(mm);
        console_puts("exec: Failed to allocate process");
        return;
    }

    user_proc->state = PROC_RUNNING;
    mm_set_exe(mm, path);
    user_proc->mm = mm;
    user_proc->space_root = mm->space_root;
    user_proc->page_dir = mm->dir;
    user_proc->kernel_stack = (uintptr_t)stack + KERNEL_STACK_SIZE;
    user_proc->parent_pid = current_proc ? current_proc->tgid : 0;
    proc_set_comm(user_proc, path); /* program name for /proc/<pid>/stat and ps */
    current_proc = user_proc;
    /* Entered without a switch, so the registers are loaded here: the
     * program before it may have left its own state in them. */
    fpu_restore(user_proc->fpu);

    /* The ELF segments are loaded eagerly above; these VMAs let the
     * demand-paging fault handler resolve later heap/stack/mmap faults. */
    if (elf_setup_vmas(&mm->vm, &img, USER_STACK_BASE, VM_STACK_MAX_GROWTH) != 0 ||
        (stack_bottom < USER_STACK_BASE &&
         !vm_grow_stack(&mm->vm, stack_bottom, RLIM_INFINITY32))) {
        k_panic("boot: no memory for the first process's regions");
    }

    /* The first user process of a normal boot is the persistent /sbin/init.
     * Capture that before claiming init_pid, because a real init must NOT own
     * the console (see the gated publish below). */
    init_is_starting = (init_pid == 0 && !debug_shell_is_home);

    if (init_pid == 0) {
        init_pid = user_proc->pid;
    }

    /*
     * Make this program the console's controlling-terminal foreground group. A
     * job-control shell (busybox ash) checks tcgetpgrp() against its own group
     * at startup and SIGTTIN-stops itself if it is not the foreground, so a
     * shell launched here must already own the terminal; the kernel debug shell
     * hands the terminal over for the program's lifetime. tty_set_session is
     * one interrupts-off update so the timer-IRQ serial drain never sees sid
     * set while fg_pgid is still the stale 0, which would route a serial ^C/^Z
     * to group 0 and drop it.
     *
     * The exception is a real /sbin/init: it must not own the console, so
     * that each login shell it spawns can `setsid` and claim the (still
     * leaderless) console itself, the way getty hands a terminal to a login
     * session. Skip the publish for init; every other program (including the
     * debug shell's own `exec`) still gets it.
     */
    if (!init_is_starting) {
        tty_set_session(&console_tty, (int)user_proc->sid, (int)user_proc->pgid);
    }

    switch_page_directory(mm->dir);

    /* fork/waitpid need the scheduler running. */
    sched_enable();

    kernel_shell_active = false;

    /* Load the thread pointer before ring 3. This first-entry path bypasses
     * switch_context, which otherwise loads it per process, so without this a
     * fresh program could observe a previous process's TLS base before it set
     * its own. A new process has tls_base == 0. */
    arch_tls_set(user_proc->tls_base);

    arch_enter_user(entry_point, user_sp);

    console_puts("exec: returned from user mode");
}

/**
 * Halt a boot that mounted no root at all, rather than open the debug shell.
 *
 * The kernel falls to the debug shell when it cannot start `/sbin/init`, and
 * that shell authenticates nobody. There is one case where it is the right
 * home and not a way in: an image whose mounted root simply has no init, which
 * is the test image, and which the harness drives. Every other way to get
 * there is a root that did not mount: a disk root on a machine whose disk is
 * absent, damaged or skipped, or a module that would not mount. Those halt
 * here with a line saying so. `rach2.rescue` is the way to ask for the shell
 * on purpose, and this runs only when it was not asked for.
 *
 * Never returns when it refuses.
 */
static void refuse_unmounted_root(void)
{
    if (vfs_root_mounted()) {
        return;
    }

    set_fore_color(light_red);
    klog(KLOG_WARN, "boot", "refused: no file system mounted");
    set_fore_color(light_grey);
    klog(KLOG_WARN, "boot",
         "nothing mounted, so /sbin/init cannot be reached and this machine "
         "would fall to a shell that authenticates nobody");
    klog(KLOG_WARN, "boot",
         "check the root this boot line asks for, or add rach2.rescue to ask "
         "for that shell deliberately");
    klog(KLOG_WARN, "boot",
         "refused: no file system mounted and no rach2.rescue: "
         "the debug shell authenticates nobody\n");
    for (;;) {
        arch_stop();
    }
}

static int root_spec_named(void);

/**
 * Halt a boot whose command line asks for two things that cannot both hold.
 *
 * `rach2.nodisk` skips the disk probe, and it is a diagnostic: an editable
 * token is not an authority boundary, and this project settled that in
 * milestone 25. What it must not become is a way past the login. Discovery
 * runs before the root is mounted, so on an image whose root is the disk, the
 * skip leaves nothing to mount, `/sbin/init` cannot be reached, and the kernel
 * falls to the debug shell, which authenticates nobody. That is the one
 * combination refused here, and the exemption is the token that asks for the
 * debug shell on purpose: a person skipping a suspected disk probe on the way
 * to rescue is doing exactly what these tokens are for.
 *
 * Never returns when it refuses.
 */
static void refuse_contradictory_boot_line(void)
{
    int skipping_disk = kcmdline_has("rach2.nodisk");
    int root_on_disk = kcmdline_has("rach2.fsdisk") || kcmdline_has("rach2.root=disk") ||
                       kcmdline_has("rach2.root=ext2") || root_spec_named();
    int wants_rescue = kcmdline_has("rach2.rescue") || kcmdline_has("init=rescue");

    if (!skipping_disk || !root_on_disk || wants_rescue) {
        return;
    }

    set_fore_color(light_red);
    klog(KLOG_WARN, "boot", "refused: rach2.nodisk with a root on the disk");
    set_fore_color(light_grey);
    klog(KLOG_WARN, "boot",
         "rach2.nodisk skips the disk this boot line asks to mount, so nothing "
         "would mount and the machine would fall to a shell that "
         "authenticates nobody");
    klog(KLOG_WARN, "boot",
         "drop one of the two, or add rach2.rescue to ask for that shell "
         "deliberately");
    klog(KLOG_WARN, "boot",
         "refused: rach2.nodisk with rach2.fsdisk or "
         "rach2.root=disk and no rach2.rescue: nothing would mount "
         "and the debug shell authenticates nobody\n");
    for (;;) {
        arch_stop();
    }
}

/**
 * Halt the machine because init died. Never returns.
 *
 * On a normal boot /sbin/init is PID 1 and must run forever; if it exits or
 * is killed there is no reaper and no login shell left, so the kernel stops
 * here, not fall to an unauthenticated debug shell. Boot
 * `rach2.rescue` to get the debug shell deliberately. Called from the
 * is-init branch of every death path (here and in syscall.c) when the debug
 * shell is not the supervisor.
 */
void panic_init_died(uint32_t pid)
{
    klog(KLOG_CRIT, "panic",
         "init (pid %d) exited; halting "
         "(boot rach2.rescue for the debug shell)\n",
         (int)pid);
    sched_disable();
    for (;;) {
        arch_stop();
    }
}

/**
 * Boot `/sbin/init` as PID 1.
 *
 * `/sbin/init` is the only program the kernel starts as a real init: it does
 * the getty dance (a real init must own no controlling terminal, and a bare
 * shell launched here would neither claim the console nor survive its own
 * exit, which would panic). Execs it through exec_program, which does not
 * return on success. Returns here only when the root holds no `/sbin/init`
 * (the test image), so the caller can fall to the debug shell.
 *
 * A lookup that fails for any other reason, an I/O error or a symbolic link
 * loop, halts instead. The machine has a root and an init on it, and the
 * shell the caller would reach authenticates nobody, so a damaged disk must
 * not be a way to a root prompt.
 */
static void boot_init_process(void)
{
    static char init_path[] = "/sbin/init";
    char *init_argv[2];

    if (!vfs_root_mounted()) {
        klog(KLOG_WARN, "boot", "no root; falling back to the debug shell");
        return;
    }

    {
        struct vfs_ref ref;
        int rc = vfs_resolve(0, init_path, 0, &ref, 0, 0);

        if (rc == -ENOENT) {
            klog(KLOG_WARN, "boot", "no /sbin/init in the root; falling back to the debug shell");
            return;
        }
        if (rc != 0) {
            set_fore_color(light_red);
            klog(KLOG_ERR, "boot", "refused: /sbin/init cannot be read (%d)", rc);
            set_fore_color(light_grey);
            klog(KLOG_ERR, "boot",
                 "the root is mounted and this is not a rach2.rescue boot, so the "
                 "debug shell, which authenticates nobody, is not the answer to a "
                 "disk that cannot be read\n");
            for (;;) {
                arch_stop();
            }
        }
        vfs_ref_release(&ref);
    }

    /* The console changes hands here. What follows on the screen is the login
     * prompt and whatever the person at it runs, so the kernel stops writing
     * there and keeps to the ring, the serial port and netconsole, which lose
     * nothing. `KLOG_CRIT` is the floor the setter enforces, so a panic still
     * reaches the person in front of the machine: this hands over the screen,
     * it does not promise the machine is well.
     *
     * Three boots keep their messages. A boot line that named a level asked
     * for them and is left alone. A recovery boot that is checking file
     * systems has minutes of work to show, and a screen that went quiet there
     * would read as a machine that had stopped. A root mounted read-only is a
     * machine whose owner needs to know before they try to work on it.
     *
     * The screen is not cleared. The boot log is what a person has just read,
     * and the login prompt appears under it.
     */
    {
        struct vfs_mount *root = vfs_root_mount();
        int root_read_only = root != NULL && (root->flags & VFS_MS_RDONLY) != 0;

        if (!klog_screen_level_was_asked_for() && !kcmdline_has("rach2.fsck") && !root_read_only) {
            klog_set_screen_level(KLOG_CRIT);
        }
    }
    klog(KLOG_INFO, "boot", "starting /sbin/init");

    /* Only now, past the point where this boot is certainly starting init, so
     * the rescue and init-less boots keep the numbering every probe and the
     * whole harness were written against. */
    proc_reclaim_boot_pids();

    init_argv[0] = init_path;
    init_argv[1] = NULL;
    exec_program(1, init_argv);

    /* exec_program returned: /sbin/init is present but failed to load (a bad
     * ELF, or out of memory). There is no init and no login shell, and falling
     * to an unauthenticated debug shell would hide the failure, so halt. A
     * genuinely init-less image (the test one) took the early return above
     * instead. */

    klog(KLOG_CRIT, "panic",
         "%s present but failed to load; halting "
         "(boot rach2.rescue for the debug shell)\n",
         init_path);
    sched_disable();
    for (;;) {
        arch_stop();
    }
}

/* RAM-disk stub backing for the page cache: with KRFFS mounted on the window,
   the file system reads and writes the module image through these page-grained
   callbacks instead of touching it directly, exercising the whole cache path
   before a real disk exists. */
static uint8_t *_krffs_stub_base;
static uint32_t _krffs_stub_size;

static int krffs_stub_read(uint32_t off, void *page)
{
    uint32_t i;
    for (i = 0; i < PAGE_SIZE; ++i) {
        ((uint8_t *)page)[i] = (off + i < _krffs_stub_size) ? _krffs_stub_base[off + i] : 0;
    }
    return 0;
}

static int krffs_stub_write(uint32_t off, const void *page)
{
    uint32_t i;
    for (i = 0; i < PAGE_SIZE; ++i) {
        if (off + i < _krffs_stub_size) {
            _krffs_stub_base[off + i] = ((const uint8_t *)page)[i];
        }
    }
    return 0;
}

/**
 * Mounts KRFFS on the demand-paged window, backed by the RAM-disk module.
 *
 * Points the page cache at the module through the stub, then mounts KRFFS at
 * PCWIN_BASE: `init_krffs`'s first read of the root node is the first window
 * fault, served by the cache. Rejects a disk larger than the window. The file
 * system code is unchanged; it just pointer-walks the window instead of the
 * module. Returns 0 on a valid mount, -1 otherwise.
 */
static int init_krffs_window(uint8_t *disk_base, uint32_t disk_size)
{
    if (disk_size > pc_window_size()) {
        klog(KLOG_WARN, "krffs", "disk size %d exceeds window %d", disk_size, pc_window_size());
        return -1;
    }
    _krffs_stub_base = disk_base;
    _krffs_stub_size = disk_size;
    pc_set_backing(krffs_stub_read, krffs_stub_write, disk_size);
    if (init_krffs((void *)PCWIN_BASE, disk_size) != 0) {
        /* Roll the backing back so a later fault cannot read stub data for a
         * file system that never mounted. */
        pc_set_backing(0, 0, 0);
        _krffs_stub_base = 0;
        _krffs_stub_size = 0;
        return -1;
    }
    return 0;
}

/* The KRFFS partition on the real disk, sliced out of the whole-disk device. */
static block_partition_t _krffs_disk_part;

/* Set once KRFFS is mounted from a real disk, so the dirty-mount flag is
 * managed only on the persistent path (the RAM paths have no on-disk dirty
 * state). */
static int _krffs_on_disk;

/**
 * Flushes the cache and then clears the dirty-mount flag, for a clean shutdown.
 * Returns 0 when the flag is cleared and -1 when it is not, so a caller cannot
 * announce a clean unmount the data did not earn.
 *
 * The kernel sets the root node's dirty bit on a disk mount and only this
 * clears it, write-through. A no-op unless KRFFS is mounted from a disk.
 *
 * The two flushes are the whole of it, and the order is the correction.
 * Clearing the flag and then flushing once, which is what this did, can publish
 * the cleared root page while a later frame's write-back fails: `pc_sync` walks
 * the slots by index and fails whole, having already written the ones that
 * succeeded, so the platter is left reading CLEAN with data missing while the
 * caller reports a failure. Flushing first makes the flag conditional on the
 * data: if that flush fails the bit is never touched, the platter keeps the bit
 * the mount wrote through, and the next boot invites `fsck.krffs`, which is the
 * safe direction. A failure of the second flush leaves the cleared byte in a
 * dirty frame and the platter still marked, which is the same safe direction.
 *
 * The root's mode is read before either flush, which buys exactly one thing:
 * the store below cannot need disk I/O. That store goes through the
 * demand-paged window, where a fill that cannot be served is a panic and not an
 * error return, and reading first turns it into a write fault on a resident
 * frame, which is a remap. `pc_sync` re-protects but never evicts, so the frame
 * is still there when the store arrives.
 *
 * It does not make the read itself safe, and nothing here can. Bringing a
 * page in needs a free frame, taking a frame needs an eviction, and an
 * eviction needs a write; on a disk that can no longer be written, with the
 * root evicted and every slot dirty, the fill fails and the machine panics
 * here instead of reporting. That is a machine which has already lost its
 * disk, the panic stops it with the flag still set, which is the direction
 * this function exists to protect, and closing the window would mean pinning
 * the root frame for the life of the mount, which is a page-cache change and
 * not a shutdown one.
 */
static int krffs_disk_mark_clean(void)
{
    krffs_node_t *root;
    uint16_t mode;

    if (!_krffs_on_disk) {
        return -1;
    }
    root = (krffs_node_t *)PCWIN_BASE;
    mode = root->mode;

    if (pc_sync() != 0) {
        klog(KLOG_WARN, "krffs", "flush failed before the clear; image stays dirty");
        return -1;
    }
    root->mode = (uint16_t)(mode & ~KRFFS_MODE_DIRTY);
    if (pc_sync() != 0) {
        klog(KLOG_WARN, "krffs", "flush failed after the clear; image stays dirty");
        return -1;
    }

    return 0;
}

/**
 * Bring the machine to a stop the way `how` asks, flushing first. Does not
 * return.
 *
 * The one path out of a running Rach2, whether the request came from the debug
 * shell or from a `reboot(2)`. It disables scheduling, flushes the page cache,
 * clears the dirty-mount flag when there is a disk mount to clear it on, says
 * on the serial line which of those happened, and stops. The clean-unmount line
 * is printed only when the flag was actually cleared; a failure has already
 * said why, and a token that appeared either way would be a claim no test could
 * trust.
 *
 * A failed flush does not stop the machine from stopping. An unwritable disk
 * would otherwise be an unstoppable machine, and the lie this refuses is the
 * other one: reporting a clean unmount the data did not earn. What a failure
 * costs is the flag, which stays set, so the next boot invites `fsck.krffs`.
 *
 * The network card is silenced last, after the flush and after the goodbye
 * line, so that a machine being read over netconsole sees the shutdown it is
 * watching for; silencing drains the card first, so that line is sent and not
 * merely handed over. See the note at the call.
 *
 * A second concurrent caller cannot exist, which is the first thing a reader
 * of a function that never returns wants to know. Preemption is already off
 * inside a syscall, since the timer skips `schedule()` while `in_syscall` is
 * set; `sched_disable()` closes the debug-shell and kernel-thread cases
 * where it is not; and nothing returns from here to enter it again.
 * `sched_disable()` has to stay ahead of the flush for that reason and not
 * merely for tidiness.
 */
void kernel_shutdown(int how)
{
    sched_disable();

    if (_krffs_on_disk) {
        if (krffs_disk_mark_clean() == 0) {
            klog(KLOG_INFO, "shutdown", "clean unmount: dirty-mount flag cleared");
        }
    } else if (vfs_root_mounted() && vfs_root_mount()->type != &vfs_krffs_type) {
        /* A root of another type is unmounted as mount(2)'s are: its
         * buffers written and its valid bit set, so the next mount does not
         * check it. */
        struct vfs_mount *root = vfs_root_mount();

        if (root->type->ops->prepare_unmount && root->type->ops->prepare_unmount(root) != 0) {
            klog(KLOG_WARN, "shutdown", "root: the unmount failed; the next mount checks it");
        } else {
            klog(KLOG_INFO, "shutdown", "root: unmounted");
        }
    } else if (pc_sync() != 0) {
        klog(KLOG_WARN, "shutdown", "page cache flush failed");
    } else {
        klog(KLOG_INFO, "shutdown", "no disk mount; nothing to unmount");
    }
    /* A record rather than a console line, so it reaches the port, the ring
     * and the wire, and the screen too on a boot that did not hand the screen
     * to init: a machine being read over netconsole has no screen, and this is
     * the last thing it says. */
    klog(KLOG_INFO, "shutdown", "Stopping the system. Goodbye!");
    /* Silence the network card, which until here has been free to write into
     * its receive ring whenever a frame arrives. Nothing after this is put on
     * the wire, and that is a contract rather than an oversight: netconsole
     * sends the log records through this card, so the line above is the last
     * one submitted to it, and the records `power_stop` raises after it, one
     * to three of them depending on the path it takes, reach the port and the
     * ring only. `stop` drains what it was given before it silences the card,
     * so a submitted frame is a sent one. The
     * alternative, stopping the card inside `power_stop`, would put subsystem
     * knowledge in the one file whose whole claim is that it has none. */
    usb_stop();
    {
        int i;

        for (i = 0; i < netdev_count(); i++) {
            netdev_stop(netdev_at(i));
        }
    }
    power_stop(how);
}

static int krffs_disk_read(uint32_t off, void *page)
{
    return _krffs_disk_part.device.read(&_krffs_disk_part.device, off / BLOCK_SECTOR_SIZE,
                                        PAGE_SIZE / BLOCK_SECTOR_SIZE, page);
}

static int krffs_disk_write(uint32_t off, const void *page)
{
    return _krffs_disk_part.device.write(&_krffs_disk_part.device, off / BLOCK_SECTOR_SIZE,
                                         PAGE_SIZE / BLOCK_SECTOR_SIZE, page);
}

/*
 * The partition table as discovery read it, and whether it is usable. The disk
 * mount reads no second copy: two reads of one sector can disagree, and then
 * the published device would address one table while the mounted root came
 * from another.
 */
static struct mbr_part disc_parts[MBR_PART_COUNT];
static int disc_table_ok;

/* The disk the boot windows are on: the one `rach2.boot=` names, else the
   one the root named by identity is on, else disk 0. */
static block_device_t *boot_disk_device;

/* A partition an identity resolved to: the disk and the table slot. */
struct root_match {
    struct disk *disk;
    int slot;
};

/* The identity `rach2.root=` names, when it names one: not `ram`, `disk`
   or `ext2`, which choose by kind and place. `root_source` is the node the
   KRFFS root came from, for `/proc/mounts`. */
static char root_spec[80];
static char root_source[24];

static int root_resolve(const char *spec, struct root_match *m);
static void part_node_path(char out[24], const struct disk *d, int slot);

/**
 * Choose the disk the boot windows are on.
 *
 * `rach2.boot=` names the boot partition by identity, as `rach2.root=`
 * names the root; the named partition must be the first entry of an MBR
 * or a GUID table, since the boot layout is judged from that table. Without
 * it, the root's disk when the root is named by identity, and disk 0
 * otherwise.
 */
static block_device_t *choose_boot_disk(void)
{
    char spec[80];
    struct root_match m;

    if (kcmdline_value("rach2.boot", spec, sizeof(spec))) {
        if (root_resolve(spec, &m) != 1) {
            klog(KLOG_WARN, "block", "rach2.boot=%s names no one partition; no boot devices", spec);
            return 0;
        }
        if ((m.disk->table.kind != PART_MBR && m.disk->table.kind != PART_GPT) ||
            m.disk->table.parts[m.slot].number != 1) {
            klog(KLOG_WARN, "block",
                 "rach2.boot=%s is not the first entry of an MBR or GUID table; no boot devices",
                 spec);
            return 0;
        }
        return m.disk->dev;
    }
    if (root_spec_named() && root_resolve(root_spec, &m) == 1) {
        return m.disk->dev;
    }
    return block0;
}

/**
 * Publish the boot partition and the boot loader's gap, before any root is
 * chosen.
 *
 * These two devices are level one of the milestone 25 disk model: every image,
 * including the everyday one, may write the kernel slots, the boot menu and
 * GRUB's core image. That has to hold on a machine booting a RAM image and
 * mounting nothing from the disk, which is exactly the recovery case, and the
 * old order made it impossible: `/dev/bootpart` appeared only at the end of a
 * successful KRFFS mount, so a machine with no mounted root had no way to
 * repair the boot partition that had stopped it booting.
 *
 * Nothing is published until sector 0 has been read and judged, and the two
 * devices are judged apart. `/dev/grubcore` covers the post-MBR gap, which no
 * partition may occupy: a table that puts one there describes a disk where
 * writing the boot loader would destroy a file system, so that window alone
 * is withheld. `/dev/bootpart` needs entry 0 judged against the whole table,
 * because a machine booting a RAM image has no mounted root to compare it
 * against; a partition in the gap does not make entry 0 unwritable.
 *
 * A disk with a GUID table is judged by the same rules, from its entries 1
 * to 4 as the partition service read them (`part_gpt_as_mbr`). Its gap holds
 * the table itself and GRUB's core is in the BIOS boot partition, so
 * `/dev/grubcore` is never published over it.
 *
 * Silent when there is no disk: a machine booting a RAM image on a diskless
 * board is the ordinary case, not a failure.
 */
static void discover_boot_devices(void)
{
    static uint8_t mbr[BLOCK_SECTOR_SIZE];
    block_device_t *disk;
    struct disk *reg;

    if (block_ready() != 0 || !block0) {
        return;
    }
    disk = choose_boot_disk();
    boot_disk_device = disk;
    if (!disk) {
        return;
    }
    reg = disk_of(disk);
    if (reg && reg->table.kind == PART_GPT) {
        part_gpt_as_mbr(&reg->table, disc_parts);
        disc_table_ok = 1;
        klog(KLOG_INFO, "block", "a GUID table holds the boot loader's gap; /dev/grubcore absent");
    } else if (disk->read(disk, 0, 1, mbr) != 0) {
        klog(KLOG_WARN, "block", "MBR read failed; no boot devices");
        return;
    } else if (mbr_parse(mbr, disc_parts) != 0) {
        klog(KLOG_WARN, "block", "no MBR signature; no boot devices");
        return;
    } else {
        disc_table_ok = 1;
        if (mbr_gap_clear(disc_parts) != 0) {
            klog(KLOG_INFO, "block",
                 "a partition occupies the boot loader's gap; "
                 "/dev/grubcore absent\n");
        } else {
            devfs_grubcore_publish(disk);
        }
    }

    if (mbr_bootpart_ok(disc_parts, disk->sector_count) != 0) {
        klog(KLOG_WARN, "block", "entry 0 is not a boot partition; /dev/bootpart absent");
        return;
    }
    devfs_bootpart_publish(block_lag_wrap(disk), disc_parts[0].start_lba,
                           disc_parts[0].sector_count);
}

static uint32_t sb_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Mounts an ext2 root from the disk as entry zero of the mount table: the
 * first partition in MBR order whose superblock has the magic and a
 * feature set the driver can mount at all. The check at mount decides
 * whether that root is writable; a later partition is never tried in its
 * place. The window is claimed as `mount(2)` claims one, so the same
 * partition cannot be mounted twice. Returns 0 mounted, -1 otherwise, the
 * reason logged under `ext2: root`.
 */
static block_partition_t _ext2_root_part;

/* What the ext2 superblock at `start + 2` of `disk` says: 0 for no ext2
   magic, -1 for features this driver does not mount, 1 for a mountable
   volume. The superblock stays in `ext2_sb` for the identity checks. */
static uint8_t ext2_sb[BLOCK_SECTOR_SIZE * 2];

static int ext2_root_at(block_device_t *disk, uint64_t start, uint64_t count)
{
    uint32_t rev, incompat;

    if (count < 4 || disk->read(disk, start + 2, 2, ext2_sb) != 0) {
        return 0;
    }
    if (ext2_sb[56] != 0x53 || ext2_sb[57] != 0xEF) {
        return 0;
    }
    rev = sb_le32(ext2_sb + 76);
    incompat = rev >= 1 ? sb_le32(ext2_sb + 96) : 0;
    if (rev > 1 || (incompat & ~0x2u) != 0 || sb_le32(ext2_sb + 24) > 2) {
        return -1;
    }
    return 1;
}

/**
 * Mount the ext2 root at `start` of `disk`, published as `path`.
 *
 * The root reads the real disk, as the KRFFS root does: the partition nodes
 * are on the lag wrapper for the tests, and a root that slept behind it
 * would sleep outside any process.
 */
static int ext2_root_mount(block_device_t *disk, uint64_t start, uint64_t count, const char *path)
{
    struct block_device *view, *backing;
    uint64_t first, span;
    int claim, rc;

    rc = devfs_window_of(path, &view, &backing, &first, &span);
    if (rc != 0) {
        klog(KLOG_ERR, "ext2", "root: %s is not published", path);
        return -1;
    }
    claim = devfs_claim(backing, first, first + span - 1);
    if (claim < 0) {
        klog(KLOG_ERR, "ext2", "root: %s is claimed", path);
        return -1;
    }
    block_partition_init(&_ext2_root_part, disk, start, count);
    devfs_window_alias(view, &_ext2_root_part.device);
    rc = vfs_init_root(&vfs_ext2_type, &_ext2_root_part.device, path, claim);
    if (rc != 0) {
        devfs_release_claim(claim);
        klog(KLOG_ERR, "ext2", "root: %s refused", path);
        return -1;
    }
    set_fore_color(light_green);
    klog(KLOG_INFO, "ext2", "root: %s%s", path,
         (vfs_root_mount()->flags & VFS_MS_RDONLY) ? ", read-only" : "");
    set_fore_color(light_grey);
    return 0;
}

static int init_ext2_root(void)
{
    int i;

    if (block_ready() != 0 || !block0 || !disc_table_ok) {
        klog(KLOG_ERR, "ext2", "root: no boot disk with a partition table");
        return -1;
    }
    /* The entries are the boot disk's, so the reads must go to that disk
     * and not to the first one the layer published; `rach2.boot=` can name
     * another. */
    for (i = 0; i < MBR_PART_COUNT; i++) {
        block_device_t *disk = boot_disk_device ? boot_disk_device : block0;
        struct disk *reg = disk_of(disk);
        char path[24];
        int at = ext2_root_at(disk, disc_parts[i].start_lba, disc_parts[i].sector_count);
        int slot;

        /* The node is the one this entry has in the registry, since the
         * disk read here is the boot disk and need not be disk 0; naming
         * another disk's partition would claim it and leave this one free
         * to be mounted a second time. */
        path[0] = '\0';
        if (reg) {
            for (slot = 0; slot < reg->table.count; slot++) {
                if (reg->table.parts[slot].start_lba == disc_parts[i].start_lba) {
                    part_node_path(path, reg, slot);
                }
            }
        }
        if (path[0] == '\0') {
            continue;
        }
        if (at == 0) {
            continue;
        }
        if (at < 0) {
            klog(KLOG_WARN, "ext2",
                 "root: %s has the magic and features this driver "
                 "does not mount",
                 path);
            continue;
        }
        return ext2_root_mount(disk, disc_parts[i].start_lba, disc_parts[i].sector_count, path);
    }
    klog(KLOG_ERR, "ext2", "root: no ext2 partition on the disk");
    return -1;
}

/**
 * Mounts KRFFS from the real disk through the page-cache window.
 *
 * Slices the first KRFFS partition out of the table discovery already read and
 * judged, and points the cache at that partition through `block0` so the window
 * is backed by real sectors. KRFFS then mounts at PCWIN_BASE exactly as over
 * the RAM stub. Rejects a partition larger than the window (which also keeps
 * the byte size within 32 bits). Returns 0 on a valid mount, -1 otherwise.
 */
static int krffs_root_mount(block_device_t *disk, uint32_t start_lba, uint32_t sector_count,
                            int found);

/** Whether the sector at `start_lba` of `disk` is a KRFFS root that fits
 * `count` sectors. */
static int krffs_root_at(block_device_t *disk, uint64_t start_lba, uint64_t count)
{
    static uint8_t part0[BLOCK_SECTOR_SIZE];
    krffs_node_t *root = (krffs_node_t *)part0;

    if (count == 0 || start_lba == 0 || disk->read(disk, start_lba, 1, part0) != 0) {
        return 0;
    }
    return root->magic == KRFFS_FILE_SYSTEM_MAGIC && root->type == KRFFS_ROOT_NODE &&
           root->size != 0 && root->size <= count * BLOCK_SECTOR_SIZE;
}

static int init_krffs_disk(void)
{
    const struct mbr_part *parts = disc_parts;
    uint32_t start_lba = 0;
    uint32_t sector_count = 0;
    int i;
    int found = -1;

    if (block_ready() != 0 || !block0) {
        klog(KLOG_WARN, "krffs", "no boot disk");
        return -1;
    }
    if (!disc_table_ok) {
        klog(KLOG_WARN, "krffs", "no usable partition table");
        return -1;
    }

    /* Find the KRFFS partition by reading each non-empty entry's first sector
       and accepting the first that has a real KRFFS root: the 16-bit magic,
       the root type, and a size that is non-zero and fits the partition.
       Checking the actual root tightens the weak 16-bit magic and works
       whatever the MBR type byte says, so a GRUB-partitioned disk (boot partition first, KRFFS
       second) still mounts. LBA 0 is never probed; it has the boot code. */
    for (i = 0; i < MBR_PART_COUNT; ++i) {
        if (krffs_root_at(boot_disk_device ? boot_disk_device : block0, parts[i].start_lba,
                          parts[i].sector_count)) {
            start_lba = parts[i].start_lba;
            sector_count = parts[i].sector_count;
            found = i;
            break;
        }
    }
    if (found < 0) {
        klog(KLOG_WARN, "krffs", "no KRFFS partition on the disk");
        return -1;
    }
    root_source[0] = '\0';
    if (disk_of(boot_disk_device ? boot_disk_device : block0)) {
        int slot;
        struct disk *d = disk_of(boot_disk_device ? boot_disk_device : block0);

        /* The node the root came from, for `/proc/mounts`: the table entry
         * whose start is the partition's. */
        for (slot = 0; slot < d->table.count; slot++) {
            if (d->table.parts[slot].start_lba == start_lba) {
                part_node_path(root_source, d, slot);
            }
        }
    }
    return krffs_root_mount(boot_disk_device ? boot_disk_device : block0, start_lba, sector_count,
                            found);
}

/**
 * Mount the KRFFS root at `start_lba` of `disk` through the page-cache
 * window. `found` is the boot disk's MBR entry when the legacy scan chose
 * it, and -1 for a root named by identity, whose overlap with the boot
 * partition is judged by range.
 */
static int krffs_root_mount(block_device_t *disk, uint32_t start_lba, uint32_t sector_count,
                            int found)
{
    const struct mbr_part *parts = disc_parts;
    uint32_t part_bytes;

    /* Reject a malformed partition before mounting, so a bad MBR fails cleanly
       instead of faulting past the disk or the window later: it must be
       non-empty, a whole number of pages (the cache transfers a page at a time), lie within the
       disk, and fit the window (which also keeps sectors * 512 inside 32 bits, since the window is
       capped well below). */
    if (sector_count == 0 || sector_count % (PAGE_SIZE / BLOCK_SECTOR_SIZE) != 0) {
        klog(KLOG_WARN, "krffs", "partition is empty or not page-aligned (%d sectors)",
             sector_count);
        return -1;
    }
    if (start_lba > disk->sector_count || sector_count > disk->sector_count - start_lba) {
        klog(KLOG_WARN, "krffs", "partition runs past the disk");
        return -1;
    }
    if (sector_count > pc_window_size() / BLOCK_SECTOR_SIZE) {
        klog(KLOG_WARN, "krffs", "partition %d sectors exceeds the window", sector_count);
        return -1;
    }
    part_bytes = sector_count * BLOCK_SECTOR_SIZE;

    /* The window faults with interrupts off and polls the device from that
       context, so a device that completes later instead of answering now
       cannot back it. */
    if (!(disk->flags & BLOCK_FAULT_SAFE)) {
        klog(KLOG_WARN, "krffs", "the device is not fault-safe; no root on it");
        return -1;
    }

    block_partition_init(&_krffs_disk_part, disk, start_lba, sector_count);
    pc_set_backing(krffs_disk_read, krffs_disk_write, part_bytes);
    if (init_krffs((void *)PCWIN_BASE, part_bytes) != 0) {
        pc_set_backing(0, 0, 0);
        return -1;
    }

    /* Observe the previous mount state, then mark this mount dirty and write
       the flag straight through to the disk (pc_sync), so a crash before the clean
       shutdown clears it leaves the evidence on the device for fsck.krffs. */
    {
        krffs_node_t *root = (krffs_node_t *)PCWIN_BASE;
        uint32_t reaped;

        if (root->mode & KRFFS_MODE_DIRTY) {
            klog(KLOG_WARN, "krffs", "not cleanly unmounted last time; run fsck.krffs");
        }
        root->mode |= KRFFS_MODE_DIRTY;
        if (pc_sync() != 0) {
            /* The dirty flag never reached the platter, so a later crash would
               leave an image fsck.krffs reads as clean. Fail the mount closed, not run on a
               filesystem whose crash evidence is missing. init_krffs already published krffs_fs at
               the window, so unpublish it before dropping the backing or a later path would
               dereference the now-unbacked window. */
            klog(KLOG_WARN, "krffs", "dirty flag did not reach the disk; mount aborted");
            krffs_fs = 0;
            pc_set_backing(0, 0, 0);
            return -1;
        }

        /* Only now, with the dirty flag on the platter, reclaim the orphans a
           previous shutdown stranded: nodes kept alive by descriptors alone,
           with nothing left to free them once those descriptors died. No
           descriptor exists yet at mount, so any nameless reserved node is
           garbage and this is the one safe moment.

           After the flag and not before, because the reap rewrites allocator
           metadata: on an image larger than the cache those pages can be
           evicted mid-repair, and a reset in that window would leave a partial
           repair under a root that still read as cleanly unmounted. With the
           flag down first, such an image is recognisably dirty and fsck.krffs
           finishes the job. Not gated on that flag either: a clean halt closes
           no descriptors, so a cleanly unmounted image can carry an orphan too,
           and gating would strand it the moment fsck.krffs cleared the flag. */
        reaped = krffs_reap_orphans();
        if (reaped != 0) {
            klog(KLOG_INFO, "krffs", "reaped %d orphan(s) with no descriptor left", (int)reaped);
            if (pc_sync() != 0) {
                klog(KLOG_WARN, "krffs", "the orphan repair did not reach the disk");
            }
        }
    }
    _krffs_on_disk = 1;

    /* The boot partition was judged and published before any root was chosen
       (discover_boot_devices), so a machine that mounts nothing still has it.
       What the mount adds is the one fact discovery cannot know: which entry
       has the root, and therefore whether entry 0 overlaps it. A disk whose
       boot partition sits over its own root loses the device again. */
    if (found >= 0 && mbr_bootpart_pick(parts, found, disk->sector_count) != 0) {
        devfs_bootpart_withdraw();
    } else if (found < 0 && disk == boot_disk_device && disc_table_ok &&
               (uint64_t)start_lba <
                   (uint64_t)disc_parts[0].start_lba + disc_parts[0].sector_count &&
               (uint64_t)disc_parts[0].start_lba < (uint64_t)start_lba + sector_count) {
        klog(KLOG_WARN, "krffs", "the root overlaps the boot partition; /dev/bootpart withdrawn");
        devfs_bootpart_withdraw();
    }
    return 0;
}

/*
 * A root named by identity.
 *
 * `rach2.root=` may name the root by what the partition table says of it
 * rather than by its place: `PARTUUID=` a GPT entry's GUID, `MBRID=id,M`
 * an MBR's disk id and entry number, `GPTID=guid,M` a GUID table's disk
 * GUID and entry number, `UUID=` an ext2 volume's UUID,
 * `LABEL=` a GPT entry's name or an ext2 volume's label, or `/dev/diskNpM`
 * as the rescue override. Every partition of every registered disk is
 * compared, and the root is mounted only when exactly one matches: none is
 * "not found", two is "ambiguous", and both leave nothing mounted, because
 * a first match would put a clone's root where the named one was meant.
 * `rach2.rootfstype=` names the type, `krffs` or `ext2`; without it the
 * partition's first sectors decide.
 */
/** Case-insensitive equality of two strings. */
static int str_equal_fold(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;

        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/** Append the decimal `v` at `*n` in `out`. */
static void put_number(char *out, int *n, unsigned v)
{
    char digits[12];
    int k = 0;

    do {
        digits[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (k > 0) {
        out[(*n)++] = digits[--k];
    }
    out[*n] = '\0';
}

/** The node path of partition `slot` of `d`, `/dev/diskNpM`. */
static void part_node_path(char out[24], const struct disk *d, int slot)
{
    static const char head[] = "/dev/disk";
    int n = 0;

    while (head[n]) {
        out[n] = head[n];
        n++;
    }
    put_number(out, &n, (unsigned)d->number);
    out[n++] = 'p';
    put_number(out, &n, (unsigned)d->table.parts[slot].number);
}

/** The 36-character text of the ext2 UUID in `ext2_sb`. */
static void ext2_uuid_text(char out[37])
{
    static const char digits[] = "0123456789abcdef";
    int i, n = 0;

    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out[n++] = '-';
        }
        out[n++] = digits[ext2_sb[104 + i] >> 4];
        out[n++] = digits[ext2_sb[104 + i] & 0xF];
    }
    out[n] = '\0';
}

/**
 * The entry number after the comma of an identity, `,M`, at `p`: the number,
 * or -1 when `p` is not a comma and decimal digits to the end of the string.
 */
static int spec_entry_number(const char *p)
{
    int number = 0;

    if (*p++ != ',' || *p == '\0') {
        return -1;
    }
    for (; *p >= '0' && *p <= '9'; p++) {
        if (number > PART_MAX) {
            return -1; /* past any entry a table can hold */
        }
        number = number * 10 + (*p - '0');
    }
    return *p == '\0' ? number : -1;
}

/**
 * The disk id of an identity, eight hexadecimal digits at `p`, into `*id`:
 * 0, or -1 when a digit is not one.
 */
static int spec_mbr_id(const char *p, uint32_t *id)
{
    int i;

    *id = 0;
    for (i = 0; i < 8; i++) {
        int v = p[i] >= '0' && p[i] <= '9'   ? p[i] - '0'
                : p[i] >= 'a' && p[i] <= 'f' ? p[i] - 'a' + 10
                : p[i] >= 'A' && p[i] <= 'F' ? p[i] - 'A' + 10
                                             : -1;

        if (v < 0) {
            return -1;
        }
        *id = (*id << 4) | (uint32_t)v;
    }
    return 0;
}

/**
 * Whether partition `slot` of `d` is the one `spec` names.
 *
 * `LABEL=` and `UUID=` read the partition's ext2 superblock, since a label
 * may be the volume's, so a partition with neither a GPT name nor a
 * readable ext2 superblock does not match those.
 */
static int root_spec_matches(const char *spec, struct disk *d, int slot)
{
    const struct part_entry *e = &d->table.parts[slot];
    char text[40];
    int i;

    if (str_starts(spec, "PARTUUID=")) {
        uint8_t guid[16];

        return d->table.kind == PART_GPT && part_guid_parse(spec + 9, guid) == 0 &&
               part_guid_equal(guid, e->guid);
    }
    if (str_starts(spec, "MBRID=")) {
        uint32_t id;

        return spec_mbr_id(spec + 6, &id) == 0 && d->table.kind == PART_MBR &&
               d->table.mbr_id == id && e->number == spec_entry_number(spec + 14);
    }
    if (str_starts(spec, "GPTID=")) {
        uint8_t guid[16];

        for (i = 0; i < 36 && spec[6 + i] != '\0'; i++) {
            text[i] = spec[6 + i];
        }
        text[i] = '\0';
        return i == 36 && d->table.kind == PART_GPT && part_guid_parse(text, guid) == 0 &&
               part_guid_equal(guid, d->table.disk_guid) &&
               e->number == spec_entry_number(spec + 42);
    }
    if (str_starts(spec, "/dev/disk")) {
        part_node_path(text, d, slot);
        return str_equal(spec, text);
    }
    if (str_starts(spec, "LABEL=") && d->table.kind == PART_GPT && e->label[0] != '\0' &&
        str_equal(spec + 6, e->label)) {
        return 1;
    }
    if (str_starts(spec, "LABEL=") || str_starts(spec, "UUID=")) {
        if (ext2_root_at(d->dev, e->start_lba, e->sector_count) == 0) {
            return 0;
        }
        if (str_starts(spec, "UUID=")) {
            ext2_uuid_text(text);
            return str_equal_fold(spec + 5, text);
        }
        for (i = 0; i < 16; i++) {
            text[i] = (char)ext2_sb[120 + i];
        }
        text[16] = '\0';
        return text[0] != '\0' && str_equal(spec + 6, text);
    }
    return 0;
}

/**
 * Resolve `spec` over every registered disk. Returns 1 with `m` filled
 * when exactly one partition matches, 0 when none does, and -1 when more
 * than one does; the last two are recorded by name.
 */
static int root_resolve(const char *spec, struct root_match *m)
{
    struct root_match first, second;
    char a[24], b[24];
    int found = 0, i, k;

    first.disk = 0;
    second.disk = 0;
    first.slot = second.slot = 0;
    for (i = 0; i < disk_count(); i++) {
        struct disk *d = disk_at(i);

        for (k = 0; k < d->table.count; k++) {
            if (!root_spec_matches(spec, d, k)) {
                continue;
            }
            if (found == 0) {
                first.disk = d;
                first.slot = k;
            } else if (found == 1) {
                second.disk = d;
                second.slot = k;
            }
            found++;
        }
    }
    if (found == 1) {
        *m = first;
        return 1;
    }
    if (found == 0) {
        klog(KLOG_ERR, "root", "%s: not found on any of %d disks", spec, disk_count());
        return 0;
    }
    part_node_path(a, first.disk, first.slot);
    part_node_path(b, second.disk, second.slot);
    klog(KLOG_ERR, "root", "%s: ambiguous: %s and %s%s", spec, a, b, found > 2 ? " and more" : "");
    return -1;
}

/**
 * Mount the root `rach2.root=` names by identity: -1 when it is not
 * found, is ambiguous, or does not mount. `krffs_mounted` says whether a
 * KRFFS root came up, for the caller's report.
 */
static int root_spec_named(void)
{
    if (!kcmdline_value("rach2.root", root_spec, sizeof(root_spec))) {
        /* The option is there and its value did not fit. An identity that
         * cannot be read is not an absent one: the root stays named, and
         * matches nothing, rather than falling back to another root. */
        root_spec[0] = '\0';
        return kcmdline_count("rach2.root") > 0;
    }
    return !str_equal(root_spec, "ram") && !str_equal(root_spec, "disk") &&
           !str_equal(root_spec, "ext2");
}

static int init_root_by_identity(const char *spec, int *krffs_mounted)
{
    struct root_match m;
    const struct part_entry *e;
    char type[8];
    char path[24];
    int want_krffs = 0, want_ext2 = 0;

    *krffs_mounted = 0;
    if (block_ready() != 0 || root_resolve(spec, &m) != 1) {
        return -1;
    }
    e = &m.disk->table.parts[m.slot];
    part_node_path(path, m.disk, m.slot);
    if (kcmdline_value("rach2.rootfstype", type, sizeof(type))) {
        want_krffs = str_equal(type, "krffs");
        want_ext2 = str_equal(type, "ext2");
        if (!want_krffs && !want_ext2) {
            klog(KLOG_ERR, "root", "%s: rach2.rootfstype=%s is not krffs or ext2", path, type);
            return -1;
        }
    }
    if (!want_ext2 && krffs_root_at(m.disk->dev, e->start_lba, e->sector_count)) {
        if (e->start_lba > 0xFFFFFFFFull || e->sector_count > 0xFFFFFFFFull) {
            klog(KLOG_ERR, "root", "%s: past what the KRFFS window addresses", path);
            return -1;
        }
        klog(KLOG_INFO, "root", "%s: %s, KRFFS", spec, path);
        if (krffs_root_mount(m.disk->dev, (uint32_t)e->start_lba, (uint32_t)e->sector_count, -1) !=
            0) {
            return -1;
        }
        k_strncpy(root_source, path, sizeof(root_source));
        *krffs_mounted = 1;
        return 0;
    }
    if (!want_krffs && ext2_root_at(m.disk->dev, e->start_lba, e->sector_count) == 1) {
        klog(KLOG_INFO, "root", "%s: %s, ext2", spec, path);
        return ext2_root_mount(m.disk->dev, e->start_lba, e->sector_count, path);
    }
    klog(KLOG_ERR, "root", "%s: %s has no %s file system this kernel mounts", spec, path,
         want_krffs  ? "KRFFS"
         : want_ext2 ? "ext2"
                     : "root");
    return -1;
}

/*
 * What the machine says about time, measured on the hardware it runs on.
 *
 * Three things a program cannot check for itself and a host test cannot check
 * at all: that the tick arrives at all, that a delay of a given length takes
 * that long, and that the clock and the tick agree. The last
 * earned its place on the laptop this targets, where an interrupt handler took
 * a PIT latch between two samples and 2000 us read as 1361 us
 * (dev-log/43-machine/notes.md).
 *
 * The numbers are printed rather than a verdict, and only the clock's
 * agreement with itself decides the record's level. A machine whose delays
 * run a third short boots and logs in perfectly well, and the fault appears
 * months later as a device that fails now and then; a line with the
 * measurements in it is something a person can read and disbelieve.
 *
 * The eleven other checks that ran here are gone. Paging, system calls,
 * context switching, isolation, the heap, the file system and the loader are
 * exercised by the first program that starts, and are covered by the host
 * tests and the probe suites in a way these never were: the memory check read
 * 256 bytes and the page-cache one ran against a RAM disk.
 *
 * The record's level is the clock's period accounting, checked on fixed
 * readings by `timer_clock_check`: a sweep counted as a period, or a period
 * counted twice, fails it on every machine and a stopped machine cannot.
 * The delay, the wait and the walk are measured against time the host
 * owns: a hypervisor that stops the machine and then delivers the ticks it
 * missed reads a 200 ms delay as twice that, or puts the missed ticks
 * inside a walk and not its clock readings, and a suite that judged them
 * would be judging the host's load. They are printed, and a person reads
 * them.
 */
static void check_timing(void)
{
    uint32_t measured_ms = 0;
    uint32_t measured_us = 0;
    uint32_t walk_clock = 0, walk_tick = 0, tsc;
    const char *why = "";
    int delay_ok, walk_ok, clock_ok, tick_ok;

    tick_ok = test_timer();
    delay_ok = test_udelay(&measured_ms);
    walk_ok = test_clock(&measured_us, &why, &walk_clock, &walk_tick);
    clock_ok = timer_clock_check();
    tsc = timer_calibrate();

    klog(clock_ok ? KLOG_INFO : KLOG_ERR, "timing",
         "tick %d Hz, 200 ms delay measured %d ms, 2000 us wait measured %d us",
         (int)platform_timer_hz(), (int)measured_ms, (int)measured_us);
    if (!tick_ok) {
        klog(KLOG_WARN, "timing", "the tick did not arrive while the processor halted five times");
    }
    if (!delay_ok) {
        klog(KLOG_WARN, "timing", "the 200 ms delay was not 150 to 350 ms by the tick");
    }
    if (!walk_ok) {
        klog(KLOG_WARN, "timing", "%s; last walk %d us by the clock, %d by the tick", why,
             (int)walk_clock, (int)walk_tick);
    }
    if (!clock_ok) {
        klog(KLOG_ERR, "timing", "the clock's arithmetic failed its check on fixed readings");
    }
    if (tsc) {
        klog(KLOG_INFO, "tsc", "%d counts a tick; a tick the timer misses is counted", (int)tsc);
    } else {
        klog(KLOG_INFO, "tsc", "none at one rate; a tick the timer misses is lost");
    }
}

/*
 * Sweep the word-at-a-time copies, the fill and the move both ways, on this
 * processor, when `rach2.strings` is on the boot line.
 *
 * `k_memcpy`, `k_memset` and `k_memmove` move their words with `rep movsl` and
 * `rep stosl` and the last bytes with a loop (sys/lib/kstring.c). That asm is
 * i386's, so the host test beside it (tests/host/kstring_hosttest.c) runs the C
 * the compiler writes for the host and never the instructions the machine runs.
 * Every boot uses these routines constantly, which finds a broken one at once
 * and says nothing about the length or the alignment that breaks a rarely
 * taken tail.
 *
 * So the sweep is here, where the instructions are, and not at every boot: 68
 * lengths at four source alignments and four destination alignments, each
 * copied, filled and moved in both directions, with the bytes on each side of
 * the range required to be untouched. It ran at every boot until 2026-09-14
 * and cost a second of every one of them.
 */
static void check_strings(void)
{
    static unsigned char pat[80], got[80];
    static const unsigned char fills[2] = {0xa5, 0x58};
    const char *what;
    size_t i;
    int len, sa, da, f, src, dst;

    for (i = 0; i < sizeof(pat); i++) {
        pat[i] = (unsigned char)(0x21 + (i * 7) % 90);
    }
    for (len = 0; len <= 67; len++) {
        for (sa = 0; sa < 4; sa++) {
            for (da = 1; da < 5; da++) {
                what = "copy";
                src = sa;
                dst = da;
                k_memset(got, 0x5a, sizeof(got));
                k_memcpy(got + da, pat + sa, (size_t)len);
                for (i = 0; i < (size_t)len; i++) {
                    if (got[da + i] != pat[sa + i]) {
                        goto broken;
                    }
                }
                if (got[da - 1] != 0x5a || got[da + len] != 0x5a) {
                    goto broken;
                }
                /* Two fill bytes, one with the top bit set and one without: a
                   fill that sign-extended its byte, or took the tail from the
                   wrong end of the word, shows in one and not the other. */
                what = "fill";
                src = 0;
                dst = da;
                for (f = 0; f < 2; f++) {
                    k_memset(got, 0x5a, sizeof(got));
                    k_memset(got + da, (char)fills[f], (size_t)len);
                    for (i = 0; i < (size_t)len; i++) {
                        if (got[da + i] != fills[f]) {
                            goto broken;
                        }
                    }
                    if (got[da - 1] != 0x5a || got[da + len] != 0x5a) {
                        goto broken;
                    }
                }
                /* The move, both ways, between ranges whose starts are
                   sa + 1 apart. */
                what = "move down";
                src = da + sa + 1;
                dst = da;
                k_memcpy(got, pat, sizeof(got));
                k_memmove(got + da, got + da + sa + 1, (size_t)len);
                for (i = 0; i < (size_t)len; i++) {
                    if (got[da + i] != pat[da + sa + 1 + i]) {
                        goto broken;
                    }
                }
                if (got[da - 1] != pat[da - 1] || got[da + len] != pat[da + len]) {
                    goto broken;
                }
                what = "move up";
                src = da;
                dst = da + sa + 1;
                k_memcpy(got, pat, sizeof(got));
                k_memmove(got + da + sa + 1, got + da, (size_t)len);
                for (i = 0; i < (size_t)len; i++) {
                    if (got[da + sa + 1 + i] != pat[da + i]) {
                        goto broken;
                    }
                }
                /* Both sides of the range this one wrote: a backward copy that
                   went one byte too far leaves the byte before it changed, and
                   the next round would put the pattern back over it. */
                if (got[da + sa] != pat[da + sa] ||
                    got[da + sa + 1 + len] != pat[da + sa + 1 + len]) {
                    goto broken;
                }
            }
        }
    }
    if (k_strlen("hello") != 5 || k_strlen("") != 0) {
        klog(KLOG_ERR, "strings", "k_strlen is wrong about a string it was given");
        return;
    }
    klog(KLOG_INFO, "strings", "swept 68 lengths at 4 source and 4 destination offsets: ok");
    return;

broken:
    klog(KLOG_ERR, "strings", "%s wrong at length %d, source offset %d, destination offset %d",
         what, len, src, dst);
}

#ifdef RACH2_RECOVERY
/** Whether `sector` begins with the signature of a GPT header. */
static int holds_gpt_header(const uint8_t *sector)
{
    static const char gpt_signature[] = "EFI PART";
    uint32_t i;

    for (i = 0; i < 8; ++i) {
        if (sector[i] != (uint8_t)gpt_signature[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * The number, 1 to 4, of the primary MBR entry whose extent holds `lba`,
 * or 0 when none does or `mbr` has no partition table's signature.
 *
 * An extended partition's entry covers its logical partitions, so an entry
 * is enough to say the sector is in use.
 */
static int mbr_entry_covering(const uint8_t *mbr, uint64_t lba)
{
    uint32_t i;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        return 0;
    }
    for (i = 0; i < 4; ++i) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint32_t start =
            (uint32_t)e[8] | (uint32_t)e[9] << 8 | (uint32_t)e[10] << 16 | (uint32_t)e[11] << 24;
        uint32_t count =
            (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;

        if (e[4] != 0 && count != 0 && lba >= start && lba - start < count) {
            return (int)i + 1;
        }
    }
    return 0;
}

/**
 * Prove the whole-disk write path: WRITE, FLUSH and READ of one sector.
 *
 * The last sector is written only when nothing holds it: past the boot gap,
 * it disturbs neither a file system nor a future bootloader. A GPT disk keeps
 * its backup header there, and overwriting it leaves a disk only a repair
 * tool can open. LBA 1 says which kind this is, and where it does not, the
 * last sector is read as well: a disk whose primary header is gone keeps its
 * table in the backup alone, and the write would destroy the one copy left.
 * On an MBR disk a partition may reach the last sector, as a root grown into
 * the whole disk does, and a sector one of the four entries holds is
 * refused.
 */
static void disk_write_roundtrip(void)
{
    static uint8_t disk_sector[BLOCK_SECTOR_SIZE];
    static uint8_t disk_scratch[BLOCK_SECTOR_SIZE];
    uint64_t lba;
    uint32_t i;
    int is_gpt;
    int ok = 1;

    if (_krffs_on_disk) {
        console_printf("disk: re-probe refused while KRFFS is mounted from disk\n");
        return;
    }
    if (block_ready() != 0 || !block0) {
        console_printf("disk: no boot disk\n");
        return;
    }
    lba = block0->sector_count - 1;
    if (block0->read(block0, 1, 1, disk_sector) != 0) {
        console_printf("disk: LBA 1 unreadable; the last sector is not written\n");
        return;
    }
    is_gpt = holds_gpt_header(disk_sector);
    if (!is_gpt) {
        if (block0->read(block0, lba, 1, disk_sector) != 0) {
            console_printf("disk: the last sector is unreadable; it is not written\n");
            return;
        }
        is_gpt = holds_gpt_header(disk_sector);
    }
    if (is_gpt) {
        console_printf("disk: GPT disk; the last sector is its backup header, not "
                       "a scratch sector\n");
        return;
    }
    if (block0->read(block0, 0, 1, disk_sector) != 0) {
        console_printf("disk: LBA 0 unreadable; the last sector is not written\n");
        return;
    }
    i = (uint32_t)mbr_entry_covering(disk_sector, lba);
    if (i != 0) {
        console_printf("disk: the last sector belongs to partition %d, not a scratch sector\n",
                       (int)i);
        return;
    }

    for (i = 0; i < BLOCK_SECTOR_SIZE; ++i) {
        disk_scratch[i] = (uint8_t)(i ^ 0x5A);
    }
    if (block0->write(block0, lba, 1, disk_scratch) != 0) {
        console_printf("disk: write of the last sector failed\n");
        return;
    }
    if (block0->read(block0, lba, 1, disk_sector) != 0) {
        console_printf("disk: read-back of the last sector failed\n");
        return;
    }
    for (i = 0; i < BLOCK_SECTOR_SIZE; ++i) {
        if (disk_sector[i] != disk_scratch[i]) {
            ok = 0;
            break;
        }
    }
    console_printf("disk: last-sector round-trip %s\n", ok ? "OK" : "MISMATCH");
}
#endif

static void process_command(const char *cmd)
{
    if (str_equal(cmd, "help")) {
        console_puts("Available commands:");
        console_puts("  help                - Show this help message");
        console_puts("  info                - Display system information");
        console_puts("  modules             - List loaded multiboot modules");
        console_puts("  disk                - Probe the boot disk and dump sector 0");
#ifdef RACH2_RECOVERY
        console_puts("  disk write          - Round-trip the last sector (destructive)");
#endif
        console_puts("  ls                  - List files in the mounted file system");
        console_puts("  cat <file>          - Display file contents");
        console_puts("  elf <file>          - Display ELF header info");
        console_puts("  exec <file>         - Execute ELF program");
        console_puts("  touch <file>        - Create an empty file");
        console_puts("  rm <file>           - Delete a file");
        console_puts("  mv <old> <new>      - Rename a file");
        console_puts("  write <file> <data> - Write data to file");
        console_puts("  test                - Run self-tests again");
        console_puts("  clear               - Clear the screen");
        console_puts("  switch              - Test context switching");
        console_puts("  preempt             - Test preemptive scheduling");
        console_puts("  isolate             - Test memory isolation");
        console_puts("  usermode            - Test ring 3 execution");
        console_puts("  audio ...           - Audio tests, routing, and HDA diagnostics");
        console_puts("  ahci                - Probe the AHCI/SATA HBA, dump registers, read LBA 0");
        console_puts("  netping             - Ping the gateway (10.0.2.2)");
        console_puts("  netbad              - Ping an unreachable host (timeout demo)");
        console_puts("  fault               - Trigger page fault (panic)");
        console_puts("  halt                - Halt the CPU");
        console_printf("  ...and more (see VGA display)\n");
    } else if (str_equal(cmd, "info")) {
        console_printf("Memory pointer:  0x%X\n", mem_reserved_ptr);
        console_printf("Timer ticks:     %d\n", kernel_ticks);
        console_printf("Kernel end:      0x%X\n", (uintptr_t)&_end);
        console_printf("Modules loaded:  %d\n", module_count);
    } else if (str_equal(cmd, "modules")) {
        uint32_t i;
        if (module_count == 0) {
            console_puts("No modules loaded.");
        } else {
            console_printf("Loaded modules: %d\n", module_count);
            for (i = 0; i < module_count; i++) {
                module_info_t *m = get_module(i);
                console_printf("  [%d] 0x%X - 0x%X (%d bytes)\n", i, m->start, m->end, m->size);
            }
        }
    } else if (str_equal(cmd, "disk")) {
        static uint8_t disk_sector[BLOCK_SECTOR_SIZE];

        if (_krffs_on_disk) {
            console_printf("disk: re-probe refused while KRFFS is mounted from disk\n");
        } else if (block_ready() != 0 || !block0) {
            console_printf("disk: no boot disk\n");
        } else {
            console_printf("disk: block0: %d sectors, %d MiB\n", (uint32_t)block0->sector_count,
                           (uint32_t)(block0->sector_count / 2048));

            if (block0->read(block0, 0, 1, disk_sector) != 0) {
                console_printf("disk: read of sector 0 failed\n");
            } else {
                uint32_t i;
                for (i = 0; i < BLOCK_SECTOR_SIZE; i += 16) {
                    uint32_t j;
                    console_printf("%X:", i);
                    for (j = 0; j < 16; ++j) {
                        uint8_t b = disk_sector[i + j];
                        console_putc(' ');
                        if (b < 0x10) {
                            console_putc('0');
                        }
                        console_printf("%X", b);
                    }
                    console_putc('\n');
                }
                console_printf("disk: MBR signature %X%X (expect 55AA)\n", disk_sector[510],
                               disk_sector[511]);
            }
        }
    } else if (str_equal(cmd, "disk write")) {
#ifdef RACH2_RECOVERY
        disk_write_roundtrip();
#else
        console_printf("disk: writing the whole disk needs a recovery kernel\n");
#endif
    } else if (str_equal(cmd, "ahci")) {
        if (_krffs_on_disk) {
            console_printf("ahci: re-probe refused while KRFFS is mounted from disk\n");
        } else {
            /* ahci_init clears block0 and the HBA globals before resetting, so
               the raw devices published over the old block0 would be pointing
               at a driver with no port. All three raw windows come down first
               and are rebuilt from whatever answers, which is what makes a
               re-probe leave a usable machine, not a half-attached one
               on either the success or the failure path. */
            devfs_bootpart_withdraw();
            devfs_grubcore_withdraw();
            devfs_disk_withdraw();
            disc_table_ok = 0;
            if (ahci_init() != 0) {
                console_printf("ahci: no AHCI HBA on the PCI bus\n");
            } else {
                ahci_dump();
                ahci_selftest();
                block_scan_disks();
            }
            discover_boot_devices();
            if (kcmdline_has("rach2.diskdiag")) {
                diskinfo_probe(_krffs_on_disk);
            } else {
                diskinfo_publish();
            }
        }
    } else if (str_equal(cmd, "ls")) {
        if (!krffs_fs) {
            console_puts("No file system mounted.");
        } else {
            krffs_node_t *node;
            uint32_t count = 0;

            console_puts("Files in the file system:");
            node = krffs_fs->node;
            do {
                /* Skip an orphan: a node whose name was cleared when it was
                   taken out of the namespace while a descriptor still held it.
                   No path reaches it, so no listing shows it either. */
                if (node->type == KRFFS_RESERVED_NODE && node->name[0] != '\0') {
                    console_printf("  %s  (%d bytes)\n", node->name, (uint32_t)node->data_size);
                    count++;
                }
            } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

            if (count == 0) {
                console_puts("  (no files)");
            }
        }
    } else if (str_starts_with(cmd, "cat ")) {
        const char *filename = cmd + 4;
        char buffer[512];
        int bytes_read;
        uint32_t offset = 0;
        char last = '\n';

        if (!krffs_fs) {
            console_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            console_puts("Usage: cat <filename>");
        } else {
            if (krffs_open(filename) != 0) {
                console_printf("File not found: %s\n", filename);
            } else {
                uint32_t file_size = krffs_get_file_size(filename);
                console_printf("--- %s (%d bytes) ---\n", filename, file_size);

                while ((bytes_read = krffs_read(filename, buffer, 511, offset)) > 0) {
                    buffer[bytes_read] = '\0';
                    last = buffer[bytes_read - 1];
                    console_printf("%s", buffer);
                    offset += (uint32_t)bytes_read;
                }

                /* The last byte of the last chunk, kept as it went past: the
                   loop ends with a read of zero, so the buffer no longer
                   holds it. */
                if (file_size > 0 && last != '\n') {
                    console_puts("");
                }
                console_puts("--- EOF ---");
            }
        }
    } else if (str_starts_with(cmd, "elf ")) {
        const char *filename = cmd + 4;
        /* Static, not on the 16 KiB stack: the shell runs one command at a
         * time on the console. */
        static char buffer[8192];
        int bytes_read;
        uint32_t file_size;
        uintptr_t entry_point;
        uint16_t phdr_count;
        const Elf_Phdr *phdrs;
        uint16_t i;

        if (!krffs_fs) {
            console_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            console_puts("Usage: elf <filename>");
        } else if (krffs_open(filename) != 0) {
            console_printf("File not found: %s\n", filename);
        } else {
            file_size = krffs_get_file_size(filename);
            if (file_size > sizeof(buffer)) {
                console_printf("File too large: %d bytes\n", file_size);
            } else {
                bytes_read = krffs_read(filename, buffer, file_size, 0);
                if (bytes_read != (int)file_size) {
                    console_printf("Read error: %d/%d bytes\n", bytes_read, file_size);
                } else if (!elf_check(buffer, file_size)) {
                    console_printf("Not a valid ELF executable\n");
                } else {
                    entry_point = elf_entry_point(buffer);
                    phdrs = elf_program_headers(buffer);
                    phdr_count = elf_program_header_count(buffer);

                    console_printf("ELF Header:\n");
                    console_printf("  Entry point:       0x%lX\n", (unsigned long)entry_point);
                    console_printf("  Program headers:   %d\n", phdr_count);
                    for (i = 0; i < phdr_count; i++) {
                        if (phdrs[i].p_type == PT_LOAD) {
                            console_printf("  LOAD segment:\n");
                            console_printf("    Virtual addr:  0x%lX\n",
                                           (unsigned long)phdrs[i].p_vaddr);
                            console_printf("    File size:     %ld bytes\n",
                                           (long)phdrs[i].p_filesz);
                            console_printf("    Memory size:   %ld bytes\n",
                                           (long)phdrs[i].p_memsz);
                            console_printf("    Flags:         %s%s%s\n",
                                           (phdrs[i].p_flags & PF_R) ? "R" : "-",
                                           (phdrs[i].p_flags & PF_W) ? "W" : "-",
                                           (phdrs[i].p_flags & PF_X) ? "X" : "-");
                        }
                    }
                }
            }
        }
    } else if (str_starts_with(cmd, "exec ")) {
        const char *args = cmd + 5;

        if (args[0] == '\0') {
            console_puts("Usage: exec <filename> [args...]");
        } else {
            static char exec_cmd_buf[256];
            static char *exec_argv[16];
            int exec_argc = 0;
            char *p;
            int in_arg = 0;

            k_strncpy(exec_cmd_buf, args, sizeof(exec_cmd_buf) - 1);
            exec_cmd_buf[sizeof(exec_cmd_buf) - 1] = '\0';

            /* Split on spaces and tabs in place, building argv into the
             * buffer. */
            for (p = exec_cmd_buf; *p && exec_argc < 15; p++) {
                if (*p == ' ' || *p == '\t') {
                    if (in_arg) {
                        *p = '\0';
                        in_arg = 0;
                    }
                } else {
                    if (!in_arg) {
                        exec_argv[exec_argc++] = p;
                        in_arg = 1;
                    }
                }
            }
            exec_argv[exec_argc] = (char *)0;

            if (exec_argc > 0) {
                exec_program(exec_argc, exec_argv);
            } else {
                console_puts("Usage: exec <filename> [args...]");
            }
        }
    } else if (str_starts_with(cmd, "touch ")) {
        const char *filename = cmd + 6;

        if (!krffs_fs) {
            console_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            console_puts("Usage: touch <filename>");
        } else {
            if (krffs_create(filename) == 0) {
                console_printf("Created file: %s\n", filename);
            } else {
                console_printf("Failed to create file: %s\n", filename);
            }
        }
    } else if (str_starts_with(cmd, "rm ")) {
        const char *filename = cmd + 3;

        if (!krffs_fs) {
            console_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            console_puts("Usage: rm <filename>");
        } else {
            /* Orphan a node some descriptor still has, not free it,
               the same rule the unlink syscall follows. The rescue shell runs
               with no process of its own, but a transient init can be blocked
               holding a file when the shell resumes, and freeing that node
               would hand its address back to the allocator while a descriptor
               still names it. */
            krffs_node_t *victim = krffs_resolve(filename);

            if (victim && fs_node_is_held(victim)) {
                krffs_unlink_held(victim);
                console_printf("Deleted file (still open elsewhere): %s\n", filename);
            } else if (krffs_unlink(filename) == 0) {
                console_printf("Deleted file: %s\n", filename);
            } else {
                console_printf("Failed to delete file: %s\n", filename);
            }
        }
    } else if (str_starts_with(cmd, "mv ")) {
        const char *args = cmd + 3;
        const char *new_name = NULL;
        char old_buf[64];
        size_t i;

        /* Split the old and new names at the first space. */
        for (i = 0; args[i] != '\0' && args[i] != ' '; i++) {
            if (i < 63)
                old_buf[i] = args[i];
        }
        old_buf[i < 63 ? i : 63] = '\0';

        if (args[i] == ' ') {
            new_name = args + i + 1;
        }

        if (!krffs_fs) {
            console_puts("No file system mounted.");
        } else if (old_buf[0] == '\0' || !new_name || new_name[0] == '\0') {
            console_puts("Usage: mv <oldname> <newname>");
        } else {
            /* Same rule at the destination: renaming over a name some
               descriptor still has orphans the node it replaces instead of
               freeing it. Last, and only once the source is known good: an
               orphan taken before the rename could fail is a refusal that
               destroyed the file it refused to touch, so `mv missing held`
               would report failure having already unnamed the destination. */
            krffs_node_t *source = krffs_resolve(old_buf);
            krffs_node_t *victim = krffs_resolve(new_name);

            if (source && source != krffs_fs->node && !KRFFS_IS_DIR(source->mode) && victim &&
                victim != source && fs_node_is_held(victim)) {
                krffs_orphan_node(victim);
            }
            if (source && krffs_rename(old_buf, new_name) == 0) {
                console_printf("Renamed '%s' to '%s'\n", old_buf, new_name);
            } else {
                console_printf("Failed to rename file\n");
            }
        }
    } else if (str_starts_with(cmd, "write ")) {
        const char *args = cmd + 6;
        const char *data = NULL;
        char file_buf[64];
        size_t i;

        /* Split the filename and data at the first space. */
        for (i = 0; args[i] != '\0' && args[i] != ' '; i++) {
            if (i < 63)
                file_buf[i] = args[i];
        }
        file_buf[i < 63 ? i : 63] = '\0';

        if (args[i] == ' ') {
            data = args + i + 1;
        }

        if (!krffs_fs) {
            console_puts("No file system mounted.");
        } else if (file_buf[0] == '\0' || !data) {
            console_puts("Usage: write <filename> <data>");
        } else {
            if (krffs_open(file_buf) != 0) {
                krffs_create(file_buf);
            }
            /* Overwrite, not append. */
            krffs_truncate(file_buf, 0);
            {
                uint32_t len = (uint32_t)k_strlen(data);
                /* A write past the old extent relocates the file and frees the
                   node it moved from, so any descriptor still on it must be
                   repointed, the same as on the write syscall's path. Captured
                   before, compared after: krffs_write resolves by name and the
                   name is unchanged, so the node pointer is the only witness
                   that a move happened. */
                krffs_node_t *before = krffs_resolve(file_buf);
                int written = krffs_write(file_buf, data, len, 0);
                krffs_node_t *after = krffs_resolve(file_buf);

                if (before && after && before != after) {
                    fs_node_moved(before, after);
                }
                if (written > 0) {
                    console_printf("Wrote %d bytes to %s\n", written, file_buf);
                } else {
                    console_printf("Failed to write to %s\n", file_buf);
                }
            }
        }
    } else if (str_equal(cmd, "test")) {
        check_timing();
    } else if (str_equal(cmd, "clear")) {
        k_cls();
    } else if (str_equal(cmd, "switch")) {
        test_switch();
    } else if (str_equal(cmd, "preempt")) {
        test_preempt();
    } else if (str_equal(cmd, "isolate")) {
        console_puts("Testing memory isolation between processes...");
        console_puts("Two processes will write different values to same virtual address.");
        if (test_isolation()) {
            set_fore_color(light_green);
            console_puts("Memory isolation test passed!");
        } else {
            set_fore_color(light_red);
            console_puts("Memory isolation test FAILED!");
        }
        set_fore_color(light_grey);
    } else if (audio_debug_handle_command(cmd)) {
    } else if (str_equal(cmd, "netping")) {
        int rtt = icmp_ping(net_ip_gateway);
        if (rtt >= 0) {
            set_fore_color(light_green);
            console_printf("ping 10.0.2.2: reply in %d ticks (%d ms)\n", rtt, rtt * 10);
            console_printf("ping 10.0.2.2: reply in %d ticks\n", rtt);
        } else {
            set_fore_color(light_red);
            console_puts("ping 10.0.2.2: timeout");
        }
        set_fore_color(light_grey);
    } else if (str_equal(cmd, "netbad")) {
        /* Ping an unreachable host on our subnet to exercise the timeout. */
        int rtt = icmp_ping(0x0A000263UL); /* 10.0.2.99 */
        if (rtt >= 0) {
            set_fore_color(light_red);
            console_printf("ping 10.0.2.99: unexpected reply (%d ticks)\n", rtt);
            console_printf("ping 10.0.2.99: unexpected reply\n");
        } else {
            set_fore_color(light_green);
            console_puts("ping 10.0.2.99: timeout (expected)");
        }
        set_fore_color(light_grey);
    } else if (str_equal(cmd, "fault")) {
        volatile uint32_t *bad_ptr = (volatile uint32_t *)0xA0000000;
        console_puts("Triggering page fault at 0xA0000000...");
        *bad_ptr = 0xDEADBEEF;
    } else if (str_equal(cmd, "halt")) {
        kernel_shutdown(POWER_HALT);
    } else if (str_equal(cmd, "poweroff")) {
        kernel_shutdown(POWER_OFF);
    } else if (str_equal(cmd, "reboot")) {
        kernel_shutdown(POWER_RESTART);
    } else if (str_equal(cmd, "usermode")) {
        console_puts("Testing user mode (ring 3) transition...");
        console_puts("Expected: 'Hello from ring 3!' followed by infinite loop.");
        test_usermode();
    } else if (cmd[0] != '\0') {
        console_printf("Unknown command: '%s'\n", cmd);
        console_puts("Type 'help' for available commands.");
    }
}

/**
 * Reset the kernel shell's command-line and stdin-wait state to a fresh prompt.
 *
 * Called on every transition back to the debug shell. An exec'd program never
 * returns through process_command (exec_program calls arch_enter_user), so the
 * post-process_command `cmd_pos = 0` never runs for it; without this reset the
 * next keystrokes append to the previous command's buffer and the shell re-runs
 * it. It also drops any stale stdin-wait state from a program that exited mid
 * read, and releases the console controlling terminal so the timer-IRQ drain
 * does not steal the input the debug shell polls itself.
 *
 * The exit paths (sys_exit, terminate_current_process) call this with
 * interrupts off, before they enable them and jump here, so a keyboard IRQ in
 * the jump window cannot observe stale state; shell_loop calls it again on
 * entry as the catch-all for boot and any path that did not.
 */
void shell_reset_input(void)
{
    cmd_pos = 0;
    cmd_ready = false;
    stdin_waiting_proc = NULL;
    /* A launched program re-claims the terminal (exec_program sets
     * sid/fg_pgid). One interrupts-off update so the timer-IRQ drain never sees
     * a torn owner. */
    tty_set_session(&console_tty, 0, 0);
}

/**
 * Run the kernel debug shell: read a command line and dispatch it, forever.
 *
 * Accepts input both from the PS/2 keyboard (the IRQ fills cmd_buffer) and from
 * QEMU's serial port, which it polls here so piped commands work; serial CR is
 * folded to newline. Sleeps on hlt between events. Does not return.
 */
void shell_loop(void)
{
    shell_reset_input();

    kernel_shell_active = true;
    console_puts("Type 'help' for available commands.");
    console_puts("");
    console_printf("> ");

    while (1) {
        if (cmd_ready) {
            cmd_ready = false;
            process_command(cmd_buffer);
            cmd_pos = 0;
            console_printf("> ");
        }

        while (serial_data_available()) {
            int ch = serial_try_getc();
            if (ch < 0)
                break;

            /* Terminals send CR on Enter. */
            if (ch == '\r')
                ch = '\n';

            if (ch == '\n') {
                if (k_scrollback_is_active()) {
                    k_scrollback_end();
                }
                cmd_buffer[cmd_pos] = '\0';
                cmd_ready = true;
                console_putc('\n');
                console_putc('\r');
                console_putc('\n');
                break;
            } else if (ch == '\b' || ch == 0x7F) {
                if (k_scrollback_is_active()) {
                    k_scrollback_end();
                }
                if (cmd_pos > 0) {
                    --cmd_pos;
                    console_putc('\b');
                    console_putc(' ');
                    console_putc('\b');
                    console_putc(' ');
                    console_putc('\b');
                }
            } else if (ch >= 0x20 && ch < 0x7F) {
                if (k_scrollback_is_active()) {
                    k_scrollback_end();
                }
                if (cmd_pos < CMD_BUFFER_SIZE - 1) {
                    cmd_buffer[cmd_pos++] = (char)ch;
                    console_putc((char)ch);
                }
            }
        }

        arch_halt();
    }
}

/**
 * The disk diagnostic, asked for by name. It probes a disk only if the boot's
 * mount did not, publishes /proc/disk and /dev/disk0, and issues nothing but
 * IDENTIFY and a sector read, so the read-only build runs it unchanged.
 * Without the flag no disk is probed, and the disks the boot registered get
 * their whole-disk nodes, mode 0400, which the everyday kernel's write handler
 * refuses, as Linux gives root a disk's node; /proc/disk is a synthfs name
 * that always resolves and reports whatever disks are registered. The
 * recovery build needs no flag: the whole-disk window is what that build is
 * for.
 */
static void publish_disks(void)
{
    if (kcmdline_has("rach2.diskdiag") || recovery_publishes_disk()) {
        if (diskinfo_probe(_krffs_on_disk) == 0) {
            klog(KLOG_INFO, "diskdiag", "ready: /proc/disk and /dev/disk0");
        } else {
            klog(KLOG_WARN, "diskdiag", "absent: no disk answered");
        }
    } else {
        diskinfo_publish();
    }
}

/**
 * Kernel entry point from the multiboot loader: bring up the system and run.
 *
 * Initializes every subsystem in order (serial, GDT/TSS/IDT/ISR, memory and
 * paging, timer, entropy, keyboard and console tty, mouse, syscalls, the
 * process table and scheduler, audio, the RAM disk and KRFFS, the pipe and
 * pty lists, and the network), runs the self-tests, then runs `/sbin/init`
 * as PID 1 (or, in rescue mode or on an init-less image, the kernel debug
 * shell). Does not return.
 */
void kernel_main(const struct boot_info *boot)
{
    uint32_t num_modules = boot->modules;

    /* Push the bump allocator past the modules below the window, so it does
     * not carve page tables over their bytes; a module above the window is
     * reserved in the frame map and mapped into the module region once
     * paging is up, and the break has nothing to do with it. The modules are
     * not guaranteed to be address-ordered, so take the highest end across
     * all of them. A module past 4 GiB, before the kernel image, or over
     * another fails the boot closed with a clear diagnostic. */
    if (num_modules > 0) {
        static struct modlayout_range ranges[MAX_MODULES];
        uint32_t m;
        uint32_t nranges = 0;
        uintptr_t modules_end = 0;
        uint32_t bad = 0;
        int verdict;

        for (m = 0; m < num_modules && m < MAX_MODULES; m++) {
            module_info_t *mod = get_module(m);

            if (!mod) {
                continue;
            }
            ranges[nranges].start = mod->start;
            ranges[nranges].end = mod->end;
            nranges++;
            if (mod->end <= HARD_MEMORY_LIMIT && mod->end > modules_end) {
                modules_end = mod->end;
            }
        }

        verdict = modlayout_check(ranges, nranges, (uint32_t)(uintptr_t)&_end, 0xFFFFFFFFu, &bad);
        if (verdict != MODLAYOUT_OK) {
            /* The detail travels INTO the panic, not ahead of it: the
             * machine this check was written for has no serial port, and
             * k_panic clears the screen, so anything printed here first would
             * be gone before a camera could read it. The panic's own record
             * carries the text to the port and the wire. */
            static char verdict_msg[128];

            modlayout_describe(verdict, bad, ranges[bad].start, ranges[bad].end, verdict_msg,
                               (uint32_t)sizeof(verdict_msg));
            k_panic(verdict_msg);
        }
        if (modules_end > mem_reserved_ptr) {
            mem_reserved_ptr = (modules_end + 0xFFF) & ~0xFFF;
        }
    }

    /* Capture the kernel command line now, before paging remaps low memory and
     * before the bump allocator can carve over the string the bootloader left.
     */
    if (boot->cmdline != NULL) {
        kcmdline_capture(boot->cmdline);
    }

    /* How much of its own record the screen shows. The ring and the port keep
     * everything either way; this is only what a person in front of the
     * machine is made to read, and a boot being brought up wants all of it
     * because the screen is then the only instrument there is. */
    {
        char level[8];

        if (kcmdline_value("rach2.loglevel", level, sizeof(level)) && level[0] >= '0' &&
            level[0] <= '9' && level[1] == '\0') {
            klog_set_screen_level_from_cmdline(level[0] - '0');
        }
    }

    k_cls();
    k_font_init();

    klog(KLOG_DEBUG, "boot", "Initializing system components");

    platform_cpu_init((uintptr_t)stack + KERNEL_STACK_SIZE);

    fpu_init();
    block_request_init();

    init_mm();
    klog(KLOG_DEBUG, "boot", "memory manager: ready");

    /* The firmware's memory map, entry by entry, then the reservations and
     * the frame table; below `FRAME_MIN_MANAGED_KIB` the boot is refused
     * by name there. */
    {
        static struct frame_map map;
        const void *mmap = boot->mmap;
        uint32_t mmap_len = boot->mmap_len;
        uint32_t m;
        uint64_t addr, len;
        uint32_t type;

        for (m = 0; frame_map_entry(mmap, mmap_len, m, &addr, &len, &type); m++) {
            char line[64];

            /* One line per firmware range, which on a laptop is a score of
               them and is what a person reading a boot has to scroll past to
               reach anything addressed to them. The summary below says what
               the machine has; these say where, which is a question only
               somebody debugging the allocator asks. */
            memory_map_line(line, sizeof(line), addr, len, type);
            klog(KLOG_DEBUG, "memory", "map: %s", line);
        }
        if (frame_map_build(&map, mmap, mmap_len, boot->mem_upper_kb) != 0) {
            k_panic("memory: the firmware's map has more ranges than the kernel keeps");
        }
        if (map.from_mem_upper) {
            klog(KLOG_WARN, "memory", "no usable map; mem_upper taken as one range");
        }
        frame_map_reserve(&map, 0, (uintptr_t)&_end);
        for (m = 0; m < num_modules && m < MAX_MODULES; m++) {
            module_info_t *mod = get_module(m);

            if (mod) {
                frame_map_reserve(&map, mod->start, mod->end);
            }
        }
        init_paging(&map);
        guard_boot_stack();
        map_modules();
        klog(KLOG_INFO, "memory",
             "usable %d KiB, unreachable %d KiB, reserved %d KiB, managed %d KiB",
             (int)(map.usable_bytes / 1024), (int)(map.unreachable_bytes / 1024),
             (int)(map.reserved_bytes / 1024), (int)(frame_total_count() * (PAGE_SIZE / 1024)));
        if (frame_total_count() * (PAGE_SIZE / 1024) < FRAME_MIN_MANAGED_KIB) {
            klog(KLOG_ERR, "memory",
                 "refused: %d KiB managed after the kernel's reservations, at least %d KiB needed",
                 (int)(frame_total_count() * (PAGE_SIZE / 1024)), (int)FRAME_MIN_MANAGED_KIB);
            k_panic("memory: too little for this kernel");
        }
    }
    klog(KLOG_DEBUG, "boot", "paging: ready");

    /* Pre-map the disk window's page tables in the kernel directory, before
     * the first fork copies the kernel PDEs. No cache frame is taken here: a
     * slot gets its frame the first time it is filled. */
    init_pagecache();
    klog(KLOG_DEBUG, "boot", "page cache: ready");

    platform_firmware_init(boot);

    platform_report();

    platform_timer_start(&timer_handler);
    klog(KLOG_DEBUG, "boot", "timer: ready");

    rtc_init();
    if (rtc_is_anchored()) {
        klog(KLOG_DEBUG, "clock", "ready");
    } else {
        klog(KLOG_WARN, "clock", "none: times run from the epoch");
    }

    krandom_seed();
    {
        /* RDSEED/RDRAND are trusted hardware entropy, so getrandom and
         * /dev/[u]random work; jitter is best-effort only, and the crypto
         * sources then fail closed. */
        int src = krandom_source();
        const char *desc = (src == KRANDOM_SRC_RDSEED)   ? "RDSEED, trusted"
                           : (src == KRANDOM_SRC_RDRAND) ? "RDRAND, trusted"
                                                         : "jitter, best-effort; "
                                                           "the crypto RNG fails closed";
        klog(src == KRANDOM_SRC_JITTER ? KLOG_WARN : KLOG_INFO, "entropy", "ready: %s", desc);
    }
    set_fore_color(light_grey);

    platform_board_init();

    platform_keyboard_init();
    tty_console_init();     /* console_tty: VGA + serial, line discipline for fds 0/1/2 */
    serial_enable_rx_irq(); /* COM1's bytes taken as they arrive, not at the tick */
    klog(KLOG_DEBUG, "boot", "keyboard: ready");

    platform_mouse_init();

    init_syscall();
    klog(KLOG_DEBUG, "boot", "syscall: ready");

    init_proc();
    {
        /* Process 0 is the kernel idle process. */
        process_t *proc0 = alloc_process();
        if (proc0) {
            proc0->state = PROC_RUNNING;
            proc0->space_root = 0; /* shares the kernel page directory */
            proc0->kernel_stack = (uintptr_t)stack + KERNEL_STACK_SIZE;
            proc0->parent_pid = 0;
            proc_idle = proc0;
            current_proc = proc0;
        }
    }
    klog(KLOG_DEBUG, "boot", "process: ready");

    init_sched();
    klog(KLOG_DEBUG, "boot", "scheduler: ready");

    kstack_overrun_test();

    if (kcmdline_has("rach2.noaudio")) {
        klog(KLOG_INFO, "audio", "skipped: rach2.noaudio");
    } else {
        klog(KLOG_DEBUG, "audio", "started");
        init_audio();
        if (audio) {
            klog(KLOG_INFO, "audio", "ready: %s", audio_get_boot_status());
        } else {
            klog(KLOG_WARN, "audio", "absent: no supported card");
        }
    }

    /* Modules were parsed early, before paging; just report them. */
    /* Which disk policy this kernel was built with, on the screen and in the
     * log. A machine whose disk must not be written says so before it has
     * touched one, and the same string is what check_diskpolicy.sh reads
     * out of the artifact. */
    klog(diskcmd_policy() == DISKCMD_READ_WRITE ? KLOG_INFO : KLOG_WARN, "disk", "policy: %s",
         diskcmd_policy() == DISKCMD_READ_WRITE ? "read-write" : "read-only");

    if (num_modules > 0) {
        module_info_t *ramdisk = get_ramdisk();
        set_fore_color(light_green);
        klog(KLOG_INFO, "modules", "found %d", num_modules);
        set_fore_color(light_grey);
        if (ramdisk) {
            klog(KLOG_INFO, "modules", "ram disk: 0x%X, %d bytes", ramdisk->start, ramdisk->size);
        }
    } else {
        set_fore_color(light_brown);
        klog(KLOG_INFO, "modules", "none");
        set_fore_color(light_grey);
    }

    refuse_contradictory_boot_line();

    discover_boot_devices();

    {
        int mounted = -1;
        int root_ext2 = 0;
        const char *via = "";

        /* rach2.fsdisk (or rach2.root=disk) mounts KRFFS from the real disk
         * through the cache (and needs no module); rach2.fswindow mounts the
         * module through the cache window; the default mounts the module in
         * place. Which one it was is recorded below while the branch that chose
         * it is still in scope: what survives afterwards is one bit, disk or
         * not, and `/proc/mounts` has to tell a window mount from a plain
         * module one as well. */
        const char *source = "none";
        const char *options = "ro";

        if (kcmdline_has("rach2.root=ext2")) {
            /* Reported by name inside, mounted or refused; the KRFFS
             * module is not tried in its place, so a candidate root that
             * does not come up leaves nothing mounted and the trial falls
             * back to the entry that does. */
            mounted = init_ext2_root();
            root_ext2 = 1;
        } else if (root_spec_named()) {
            int krffs_root = 0;

            mounted = init_root_by_identity(root_spec, &krffs_root);
            if (krffs_root) {
                via = " (disk)";
                source = root_source;
                options = "rw,relatime";
            } else {
                root_ext2 = 1; /* reported by name inside, mounted or refused */
            }
        } else if (kcmdline_has("rach2.fsdisk") || kcmdline_has("rach2.root=disk")) {
            mounted = init_krffs_disk();
            via = " (disk)";
            source = root_source[0] ? root_source : "/dev/disk0";
            options = "rw,relatime";
        } else if (num_modules > 0) {
            module_info_t *ramdisk = get_ramdisk();
            if (ramdisk && kcmdline_has("rach2.fswindow")) {
                mounted = init_krffs_window((uint8_t *)ramdisk->start, ramdisk->size);
                via = " (window)";
                source = "module";
                options = "rw,relatime,window";
            } else if (ramdisk) {
                mounted = init_krffs((void *)ramdisk->start, ramdisk->size);
                source = "module";
                options = "rw,relatime";
            }
        }

        if (root_ext2) {
            /* reported above */
        } else if (mounted == 0) {
            krffs_set_mount(source, options);
            vfs_init();
            set_fore_color(light_green);
            klog(KLOG_INFO, "krffs", "mounted: %d files%s", krffs_count_files(), via);
            set_fore_color(light_grey);
        } else if (kcmdline_has("rach2.fsdisk") || kcmdline_has("rach2.root=disk") ||
                   num_modules > 0) {
            set_fore_color(light_red);
            klog(KLOG_ERR, "krffs", "refused: the image is not a KRFFS");
        } else {
            set_fore_color(light_brown);
            klog(KLOG_INFO, "krffs", "none: no ramdisk");
        }
    }
    set_fore_color(light_grey);

    /* The USB stack comes up after the root: a root is on the disk the
     * block layer found, never on a stick, and `rach2.root=ext2` scans the
     * partitions before any USB device exists to be scanned. */
    if (kcmdline_has("rach2.nousb")) {
        klog(KLOG_INFO, "usb", "skipped: rach2.nousb");
    } else {
        klog(KLOG_INFO, "usb", "started");
        usb_report_controllers();
        usb_init();
    }

    /* The SATA and NVMe disks go over to their interrupts here, after the
     * root: the boot's reads were polled where no process could sleep, and
     * from here a transfer lets the processor run someone else. */
    ahci_start();
    nvme_start();

    publish_disks();

    /* Empty the pipe and pty lists before a user process can pipe() or open
     * /dev/ptmx. Each pipe and each pty is allocated from the heap when it is
     * created; these only start the lists. */
    pipe_init_pool();
    pty_init_pool();

    /* The address before the device, so the boot line reports what the machine
     * will use and a refusal is on screen before anything depends on it. */
    {
        const char *why = "";
        int verdict = netconf_apply(&why);

        if (verdict == NETCONF_REFUSED) {
            set_fore_color(light_brown);
            klog(KLOG_WARN, "net", "address refused: %s", why);
            set_fore_color(light_grey);
        }
    }

    if (kcmdline_has("rach2.nonet")) {
        klog(KLOG_INFO, "net", "skipped: rach2.nonet");
    } else {
        klog(KLOG_INFO, "net", "started");
        if (init_network()) {
            klog(KLOG_INFO, "net", "ready: %s", net_status());
            if (kcmdline_has("rach2.netconsole")) {
                netconsole_enable();
            }
            net_print_info();
        } else {
            klog(KLOG_WARN, "net", "absent: %s", net_error());
        }
    }

    check_timing();
    if (kcmdline_has("rach2.strings")) {
        check_strings();
    }

    /*
     * Decide the boot home. A normal boot runs the persistent /sbin/init as
     * PID 1: boot_init_process execs it and does not return while it runs,
     * and init's later death is a panic (panic_init_died), so a normal box
     * never falls to the debug shell. boot_init_process returns here only
     * when the root holds no init (the test image), having halted on a
     * lookup that failed for any other reason; that case, like an
     * explicit `rach2.rescue` boot, makes the kernel debug shell the
     * supervisor. The shell loops reading commands and `exec`s programs as
     * transient inits that return to it on exit (an is-init death path jmps
     * back to shell_loop while the debug shell is home), which is the
     * contract the test harness drives.
     */
    /* The diagnostic numbers once more, on a screen nothing will scroll, before
     * init takes the console. */
    if (kcmdline_has("rach2.diag")) {
        platform_report_hold();
    }

    rescue_mode = kcmdline_has("rach2.rescue") || kcmdline_has("init=rescue");

    if (!rescue_mode) {
        refuse_unmounted_root();
        boot_init_process();
    }

    debug_shell_is_home = true;
    while (1) {
        const char *banner =
            rescue_mode ? "Kernel debug shell active (rescue mode)." : "Kernel debug shell active.";

        console_puts("");
        console_puts(banner);
        shell_loop();
    }
}
