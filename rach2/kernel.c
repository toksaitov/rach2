/* The shared boot (`kernel_main`), the mount of the root, the kernel debug
 * shell, and the end of a process by a signal and of the machine. */

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
#include "sys/proc/smp.h"
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

/* The boot stack and the guard page below it, from the architecture's
 * loader.s. */
extern char stack[];
extern char stack_guard[];

/* A page; nothing in this file uses it. */
#define USER_STACK_SIZE 0x1000

/*
 * The debug shell's command line: the bytes typed, how many, and whether
 * Enter has ended the line. `kernel_shell_active` is set while the shell
 * reads the console and cleared when a program takes the console.
 */
#define CMD_BUFFER_SIZE 64
static char cmd_buffer[CMD_BUFFER_SIZE];
static volatile size_t cmd_pos = 0;
static volatile bool cmd_ready = false;
static volatile bool kernel_shell_active = false;

/* Free-running tick counter, bumped at 100 Hz by the timer IRQ. */
volatile uint32_t kernel_ticks = 0;

/*
 * Whole seconds since boot and the ticks into the current second, counted
 * beside the tick so that the clock does not divide a 32-bit tick count that
 * wraps. `kernel_ticks` wraps after about 497 days, and a clock that took the
 * elapsed ticks as `now - anchor` would jump back by a whole cycle at the
 * wrap. The seconds wrap after 136 years.
 */
volatile uint32_t kernel_uptime_seconds = 0;
volatile uint32_t kernel_second_ticks = 0;

/* PID of the init process, or 0 when none runs. */
volatile uint32_t init_pid = 0;

/* Whether the boot line has `rach2.rescue` or `init=rescue`, which make the
 * boot start no /sbin/init and run the kernel debug shell. */
static bool rescue_mode = false;

/* Whether the kernel debug shell supervises the programs: set on a rescue
 * boot, and on a boot whose root has no /sbin/init (the test image). It
 * decides what the end of an init process does: under the shell, the shell
 * runs again, which the test harness relies on; with /sbin/init as PID 1,
 * the end of init halts the machine (`panic_init_died`). */
bool debug_shell_is_home = false;

/* The process parked in a read of the console, or NULL; while one is, the
 * debug shell does not read the keyboard (`shell_reads_input`). */
volatile process_t *stdin_waiting_proc = NULL;

static void check_timing(void);
static void check_strings(void);
/* sys/syscall_proc.c calls these three too, and declares them itself. */
void shell_loop(void);
void shell_reset_input(void);
void panic_init_died(uint32_t pid); /* never returns */
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
        mod->start = next + (mod->phys - first);
        mod->end = mod->start + mod->size;
        next += last - first;
    }
}

/**
 * Write the memory-map entry of `len` bytes at `addr` into `buf` as text,
 * `START-END TYPE`, END being the first address past it, with 64-bit
 * hexadecimal addresses, since the kernel's formatter prints 32-bit words and
 * a map on a 4 GiB machine reaches past them. `type` is the firmware's, 1 to
 * 5 named and any other "other"; a part that does not fit `buflen` is left
 * out.
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
static void enter_program(struct mm *mm, const char *name, uintptr_t entry, uintptr_t sp);

/**
 * Service a processor's 100 Hz tick and preempt its current process.
 *
 * Every processor has its own tick; a processor other than the boot one only
 * accounts it and preempts. The boot processor's tick also advances the tick
 * count and the uptime, by the ticks the timer did not deliver as well
 * (`timer_lost_ticks`), counts the microsecond clock's period
 * (`timer_tick_clock`, see `timer_now_us`), and runs the periodic work of the
 * kernel: the sleepers and timerfds whose time has come, the HDA tick for a
 * controller whose interrupts do not arrive, the network device that is
 * polled, the TCP timers, the loopback delivery, the USB controller's event
 * ring, the block layer's lagged requests and deadlines, the platform's
 * devices, and the console's serial port.
 *
 * The tick is accounted as idle or busy first, before anything below can
 * wake a process: the accounting records what the processor was doing when
 * this tick arrived, not what it is about to do, and a busy tick is charged
 * to the process it interrupted, as user time when the interrupted code
 * segment is ring 3's and as system time otherwise. The sleepers are woken
 * whether or not the current process is in a system call, so one parked
 * inside its call becomes runnable on time. A polled network device has its
 * turn before the layers above, so a frame that arrived since the last tick
 * is delivered in the tick that acts on it; a device that raises interrupts
 * has no poll, and the call costs it a null check. The handler runs with
 * interrupts off, which the network stack's shared buffers require, and
 * nothing it calls waits on hardware.
 *
 * Then the handler reschedules through `sched_preempt`: at once when the
 * current process is not inside a system call, and at the call's exit when
 * it is, since timer IRQs stay enabled during a call so HDA streaming keeps
 * running, yet a switch in the middle of one is unsafe. Kernel threads (no
 * current_proc, or one not in_syscall) are switched at once.
 */
static void timer_handler(isr_params_t *params)
{
    uint32_t lost;

    if (arch_cpu_id() != 0) {
        sched_account_tick(FRAME_USER(params) != 0);
        sched_preempt();
        return;
    }
    lost = timer_lost_ticks();
    tick_write_begin();
    ++kernel_ticks;
    timer_tick_clock();
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
    tick_write_end();
    latency_tick();

    sched_account_tick(FRAME_USER(params) != 0);
    sched_load_tick();

    proc_wake_sleepers(kernel_ticks);
    timerfd_tick(kernel_ticks);

    hda_tick();
    netdev_poll();
    tcp_tick();
    ip_loop_drain();
    usb_poll();
    block_lag_tick();
    block_request_tick();
    platform_tick();
    tty_console_poll();

    sched_preempt();
}

/**
 * Write the core file of the current process, which the signal `signo`
 * ends, and let the threads parked for it go (`core_release`). Returns the
 * status the process ends with: `signo` with WCOREFLAG, which WCOREDUMP
 * reads, only when the whole file was written, and `signo` otherwise. The
 * caller has set `in_syscall` and enabled interrupts, as a system call
 * writing the file would: the timer does not switch away in the middle of
 * the file system's work, and a disk that answers only by interrupt, as a
 * USB stick does, can be waited for.
 */
static int32_t terminate_core(int32_t signo)
{
    int32_t rc = fs_core_file(signo, current_proc->rlim[RLIM_CORE].cur);

    if (rc == 0) {
        current_proc->tg->exit_code = signo | WCOREFLAG;
    }
    core_release();
    return rc == 0 ? signo | WCOREFLAG : signo;
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
 * The whole process ends: its other threads are sent SIGKILL
 * (`proc_group_exit`), and whichever thread ends last publishes the death,
 * posts SIGCHLD and wakes a parent blocked in waitpid. The exit code is the
 * wait status of a death by a signal, the signal in its low seven bits.
 *
 * A core file is written (`terminate_core`) only when no other thread has
 * begun to end the process: the group's `core_dumping` is set before the
 * others are sent SIGKILL, each of them parks before it releases anything
 * (`core_park`), the file is written once all have parked (`core_notes`
 * waits for them) and before this thread releases anything, and the others
 * go on to their end after it, as Linux's `do_coredump` and `coredump_wait`
 * have it.
 *
 * The teardown runs as a system call does, with `in_syscall` set and
 * interrupts on, also when the signal is acted on at an interrupt's return,
 * which comes here with them off: what it releases and writes may sleep on a
 * disk that answers only by interrupt, and the timer must not switch away in
 * the middle. Interrupts go off again once the process's end is published,
 * before its record can be freed or the processor given up.
 *
 * The session the process leads gives up the console, its pty slaves and the
 * framebuffer (`terminate_release_session`): before the thread's cleanup
 * when it is the process's only thread, so that the timer's serial drain
 * does not deliver to a session being torn down, and after the cleanup when
 * it is the last of several. This thread releases the descriptor table and
 * its reference to the address space, switching off it before its page
 * directory can be freed. A thread that is not the last schedules away and
 * does not run again.
 *
 * The end of init under the debug shell takes the path `sys_exit` takes for
 * an init that exits: the line the probe runner reads is printed, the
 * process freed, and the shell's input reset while interrupts are still off,
 * and `shell_loop` runs again from the top of the boot stack. Any other
 * process stays a zombie for its parent's waitpid, and the next process is
 * scheduled.
 */
void terminate_current_process(int32_t signo)
{
    bool is_init;
    uint32_t pid;
    int core;
    int alone;
    int last;

    if (!current_proc || current_proc->mm == NULL) {
        return;
    }

    ptrace_event(PTRACE_EVENT_EXIT, (uint32_t)(signo & 0x7F));
    pid = current_proc->tgid;
    is_init = (pid == init_pid && init_pid != 0);
    current_proc->in_syscall = 1;
    arch_irq_enable();

    core = (signo & WCOREFLAG) && !current_proc->tg->exiting && !current_proc->tg->exec_task &&
           current_proc->mm->dumpable && current_proc->rlim[RLIM_CORE].cur >= PAGE_SIZE;
    signo &= 0x7F;
    if (core) {
        current_proc->tg->core_dumping = 1;
    }
    proc_group_exit(signo);
    if (core) {
        signo = terminate_core(signo);
    } else if (current_proc->tg->core_dumping) {
        core_park();
    }
    alone = current_proc->tg->live == 1;

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
        if (!debug_shell_is_home) {
            panic_init_died(pid); /* never returns */
        }
        init_end_line(pid, current_proc->tg->exit_code);
        console_puts("Init process exited. Returning to kernel debug shell.");

        free_process(current_proc);

        init_pid = 0;
        current_proc = proc_idle;
        current_proc->state = PROC_RUNNING;

        shell_reset_input();

        sched_disable();

        arch_run_on_stack((uintptr_t)stack + KERNEL_STACK_SIZE, shell_loop);
    } else {
        schedule();
        k_panic("a dying process was scheduled again");
    }
}

/**
 * Deliver SIGINT for a keyboard's Ctrl+C to the console's foreground group.
 *
 * The target is the controlling terminal's foreground process group, the
 * same one the serial ^C reaches through the line discipline, not
 * current_proc: inside the keyboard's interrupt, or the USB poll of the
 * timer, the running process may be unrelated. With no foreground group
 * yet, falls back to the running user process only when it belongs to the
 * console's session, so a lone program is still interruptible while a stray
 * ^C never signals an unrelated daemon that happened to be running.
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
 * What the input layer needs from the debug shell (sys/input.h): whether the
 * shell is the one reading, a character for its buffer, and the out-of-band
 * interrupt it takes where there is no session to take it through the line
 * discipline.
 */
int shell_reads_input(void)
{
    return kernel_shell_active && stdin_waiting_proc == NULL;
}

void shell_interrupt(void)
{
    kill_foreground_process();
}

/**
 * Enter ends the line and Backspace erases a character, each echoed; a
 * character past the buffer's room is dropped. Before it takes a key, the
 * shell returns a scrolled-back screen to the live one.
 */
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

/** Whether `a` and `b` are the same string. */
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

/** Whether `str` begins with `prefix`. */
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

/** Whether the tick advanced while the processor waited for an interrupt
 * five times. */
static bool test_timer(void)
{
    uint32_t start_ticks;
    uint32_t i;

    start_ticks = kernel_ticks;

    for (i = 0; i < 5; ++i) {
        idle_wait_irq_on();
    }

    return kernel_ticks > start_ticks;
}

/**
 * Check by the tick that `k_udelay` waits for as long as it is asked to.
 *
 * The delay measures itself against the 8254's counter, or the platform's
 * reference counter where the 8254 does not count, and the tick comes from
 * the timer that ticks, the local APIC's or the 8254's. The two arrive at a
 * duration by different arithmetic, so a disagreement is an error in one of
 * them, and the device bring-up sequences that ask for microseconds are only
 * as good as the delay. The wait is 200 ms, twenty ticks, so one tick of
 * quantization is five percent of the reading, well inside the band below
 * and short enough not to lengthen the boot noticeably. `*measured_ms`
 * reports what was seen, since a wrong figure says more than a failed
 * comparison.
 *
 * The band is 150 to 350 ms, since a delay off by half or double is what a
 * bring-up sequence would notice. The reading is the shortest of up to five
 * waits, and one wait in the band ends the check, so an unloaded boot pays
 * for one. A host with more machines than processors stops this one part way
 * through a wait and delivers the ticks it missed afterwards, so a wait can
 * read long but not short; under such a host every wait can read long, which
 * is why `check_timing` prints the result as a warning.
 */
static bool test_udelay(uint32_t *measured_ms)
{
    uint32_t best = 0xFFFFFFFFu;
    int attempt;

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
 * Check the microsecond clock against `k_udelay`, which measures time another
 * way, and against the tick. Returns true when both agree; otherwise `*why`
 * says how the check failed.
 *
 * `timer_now_us` counts the periods of the ticking timer's counter and reads
 * where it is inside one; `k_udelay` measures the 8254's counter by its drop.
 * A wait of 2 ms is taken eight times and the shortest of them is the
 * reading, `*measured_us`, which must be 1500 to 3500 us: an interrupt in the
 * middle of a wait can only add to it, and the machine writes whole lines to
 * a serial port at eighty-seven microseconds a byte.
 *
 * Then fifty milliseconds, five of the tick's periods, are walked in single
 * milliseconds with the clock read at every step. A reading that lost which
 * half of the period it was in, or that did not count a wrap, steps backwards
 * there. A clock that runs fast or slow fails too, because the tick measures
 * the same fifty milliseconds beside it, and a clock that counted a sweep as
 * a period, or counted a period twice, would put twice the time in between;
 * the two must agree within a quarter. `*walk_clock` and `*walk_tick` are
 * the two measures of the last walk that did not step backwards.
 *
 * The walk is taken up to three times and passes when one agrees with the
 * tick, and a backwards reading ends a walk and not the check. Under an
 * emulator whose host is busy, a tick can arrive late or not at all inside
 * one walk, which the comparison cannot tell from a slow clock, and an
 * interrupt taken between two reads of the counter has given one backwards
 * reading on real hardware and under a busy emulator; a clock that is wrong
 * is wrong on every walk.
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
 * Prove ring 3 works: run the architecture's payload as a process of its
 * own, in an address space with two pages, the payload's at the lowest
 * user address, read-only and executable, and a stack page two pages above
 * it. The payload writes "Hello from ring 3!" through a system call and
 * exits, and the shell comes back as it does after `exec`
 * (`enter_program`). Returns, false, only when the process cannot be made.
 */
static bool test_usermode(void)
{
    uintptr_t code = USER_ADDR_MIN, stack_top = USER_ADDR_MIN + 3 * PAGE_SIZE;
    uintptr_t code_frame, stack_frame;
    struct mm *mm = mm_new();
    uint8_t *page;

    if (!mm) {
        console_puts("  Usermode test: no address space");
        return false;
    }
    code_frame = alloc_user_page(mm->dir, code);
    stack_frame = alloc_user_page(mm->dir, stack_top - PAGE_SIZE);
    if (code_frame == 0 || stack_frame == 0 || paging_zero_frame(code_frame) != 0 ||
        paging_zero_frame(stack_frame) != 0 ||
        vm_insert(&mm->vm, code, code + PAGE_SIZE, VM_PROT_READ | VM_PROT_EXEC, VM_CODE) != 0 ||
        vm_insert(&mm->vm, stack_top - PAGE_SIZE, stack_top, VM_PROT_READ | VM_PROT_WRITE,
                  VM_STACK) != 0) {
        mm_put(mm);
        console_puts("  Usermode test: the payload's pages could not be had");
        return false;
    }
    page = (uint8_t *)kmap_temp(code_frame);
    k_memcpy(page, arch_user_test_code, arch_user_test_size);
    kunmap_temp(page);
    paging_set_writable(mm->dir, code, false);
    console_printf("  Usermode test: payload at 0x%lx, stack below 0x%lx; entering ring 3...\n",
                   (unsigned long)code, (unsigned long)stack_top);
    enter_program(mm, "usermode", code, stack_top);
    return false;
}

/* Context-switch test state, shared between the two cooperating processes. */
static process_t *switch_proc_a = NULL;
static process_t *switch_proc_b = NULL;
static volatile int switch_count = 0;

/** Process B of `switch`: print 'B' and switch back to A until the count is
 * reached. */
static void proc_b_entry(void)
{
    while (switch_count < 10) {
        console_putc('B');
        ++switch_count;
        switch_context(switch_proc_b, switch_proc_a);
    }

    switch_context(switch_proc_b, switch_proc_a);

    for (;;) {
        idle_wait_irq_on();
    }
}

/**
 * The `switch` command: the shell, as process A, and a kernel process B hand
 * the processor to each other with `switch_context`, each printing its letter,
 * until they have printed ten between them. It passes when the shell runs
 * again after the tenth.
 */
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

    stack_b = (uintptr_t)phys_to_virt(mem_reserve(KERNEL_STACK_SIZE, true));
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

/** Process A of `preempt`: print 'A' in a busy loop until the pair has
 * printed 20. The busy delay must outlast a timer tick (10 ms at 100 Hz) so
 * the preemption is observable. */
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
        idle_wait_irq_on();
    }
}

/** Process B of `preempt`, as `preempt_proc_a` with 'B'. */
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
        idle_wait_irq_on();
    }
}

/**
 * The `preempt` command: two kernel processes print A and B in busy loops,
 * switched by the timer alone, until they have printed twenty between them;
 * it passes when both ran.
 *
 * The test is one processor's: the shell and both threads are bound to the
 * processor the shell runs on until it ends. A thread spinning in the kernel
 * holds the big lock, so on a second processor it would keep the first from
 * taking its own tick, which the round-robin turns are counted by, and the
 * shell would free a thread still running there.
 */
static void test_preempt(void)
{
    process_t *proc_a;
    process_t *proc_b;
    uintptr_t stack_a, stack_b;
    uint64_t shell_affinity = current_proc->affinity;
    uint64_t here = (uint64_t)1 << arch_cpu_id();

    console_puts("Testing preemptive round-robin scheduling...");
    console_puts("Two processes will print A and B, preempted by timer.");
    console_puts("Expected: interleaved As and Bs (e.g., AABBABAB...)");
    console_printf("Result:   ");
    console_printf("Result:   ");

    preempt_done = 0;
    preempt_count_a = 0;
    preempt_count_b = 0;
    sched_set_affinity(current_proc, here);

    proc_a = alloc_process();
    if (!proc_a) {
        sched_set_affinity(current_proc, shell_affinity);
        console_puts("Failed to allocate process A");
        return;
    }
    proc_a->affinity = here;
    stack_a = (uintptr_t)phys_to_virt(mem_reserve(KERNEL_STACK_SIZE, true));
    proc_a->kernel_stack = stack_a + KERNEL_STACK_SIZE;
    proc_a->space_root = 0;
    proc_a->parent_pid = current_proc->pid;
    setup_process_stack(proc_a, preempt_proc_a);

    proc_b = alloc_process();
    if (!proc_b) {
        free_process(proc_a);
        sched_set_affinity(current_proc, shell_affinity);
        console_puts("Failed to allocate process B");
        return;
    }
    proc_b->affinity = here;
    stack_b = (uintptr_t)phys_to_virt(mem_reserve(KERNEL_STACK_SIZE, true));
    proc_b->kernel_stack = stack_b + KERNEL_STACK_SIZE;
    proc_b->space_root = 0;
    proc_b->parent_pid = current_proc->pid;
    setup_process_stack(proc_b, preempt_proc_b);

    sched_enable();

    while (!preempt_done) {
        idle_wait_irq_on();
    }

    sched_disable();

    console_puts("");
    console_printf("\n");
    console_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);
    console_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);

    free_process(proc_a);
    free_process(proc_b);
    sched_set_affinity(current_proc, shell_affinity);

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
 * The state of the `isolate` command. The two processes write different
 * values to the same virtual address through different page directories;
 * isolation holds if each reads back its own value.
 *
 * The address is 0xD00000 (13 MiB), inside the 32 MiB the i686 kernel maps
 * by identity. A private mapping there still gives each process its own page
 * table: `mmu_user_table` copies a table the kernel directory shares before a
 * user page goes into it, so the two processes do not write through one.
 */
#define ISOLATION_TEST_ADDR 0xD00000
static volatile uint32_t isolation_result_a = 0;
static volatile uint32_t isolation_result_b = 0;
static process_t *isolation_proc_a = NULL;
static process_t *isolation_proc_b = NULL;
static process_t *isolation_proc_main = NULL;

/** Process A of `isolate`: write 0xAAAAAAAA, let B run, read the value back
 * for the shell to judge, and switch back to the shell. */
static void isolation_proc_a_entry(void)
{
    volatile uint32_t *ptr = (volatile uint32_t *)ISOLATION_TEST_ADDR;

    *ptr = 0xAAAAAAAA;

    switch_context(isolation_proc_a, isolation_proc_b);

    isolation_result_a = *ptr;

    switch_context(isolation_proc_a, isolation_proc_main);

    for (;;) {
        idle_wait_irq_on();
    }
}

/** Process B of `isolate`: write 0xBBBBBBBB to its own physical page, read
 * it back, and switch to A. */
static void isolation_proc_b_entry(void)
{
    volatile uint32_t *ptr = (volatile uint32_t *)ISOLATION_TEST_ADDR;

    *ptr = 0xBBBBBBBB;

    isolation_result_b = *ptr;

    switch_context(isolation_proc_b, isolation_proc_a);

    for (;;) {
        idle_wait_irq_on();
    }
}

/**
 * The `isolate` command: run processes A and B in two page directories, each
 * with a page of its own at `ISOLATION_TEST_ADDR` in a frame of its own, and
 * return whether each read back the value it wrote. Returns false too when a
 * directory, a page or a process cannot be had.
 */
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

    phys_a = alloc_user_page(dir_a, ISOLATION_TEST_ADDR);
    phys_b = alloc_user_page(dir_b, ISOLATION_TEST_ADDR);
    if (!phys_a || !phys_b || phys_a == phys_b) {
        return false;
    }

    isolation_proc_a = alloc_process();
    if (!isolation_proc_a) {
        return false;
    }
    stack_a = (uintptr_t)phys_to_virt(mem_reserve(KERNEL_STACK_SIZE, true));
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
    stack_b = (uintptr_t)phys_to_virt(mem_reserve(KERNEL_STACK_SIZE, true));
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

/** The segment loader's `elf_read_fn` over a KRFFS file: `ctx` is a
 * `struct exec_krffs_ctx` with the file's path. */
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

/** The segment loader's `elf_read_fn` through the root's mount: `ctx` is
 * `&boot_ctx`. Returns the bytes read, or -1 for an error. */
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

/** Close and release the node `exec_program` read through the root's mount,
 * if it opened one. */
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
 * Run the address space `mm`, its regions made, as a new process named
 * `name`, the name /proc/<pid>/stat and ps show, entering ring 3 at `entry`
 * with the stack pointer `sp`. Does not return, except when no process record
 * can be had: `mm` is then put and the failure printed on the console.
 *
 * The process's kernel stack is the boot stack, which the shell runs on, and
 * it becomes current without a switch, so its floating-point registers and
 * its thread pointer are loaded here: the program before it may have left
 * its own in them, and `switch_context`, which loads them for every other
 * process, is not on this path. A new process's TLS base is 0. The first
 * program while no init runs becomes init.
 *
 * Every program but a real /sbin/init is made the console's foreground
 * group, the debug shell's `exec` included, for as long as it runs: a
 * job-control shell (BusyBox ash) compares tcgetpgrp() with its own group at
 * startup and stops itself with SIGTTIN when it is not the foreground, so a
 * shell started here must already have the terminal. `tty_set_session`
 * changes the session and the group in one update with interrupts off, so
 * the timer's serial drain never sees the session set while the group is
 * still 0, which would send a serial ^C or ^Z to group 0, where it is lost.
 * A real /sbin/init, the first program of a boot the debug shell does not
 * supervise, is left without the console, so that each login shell it starts
 * can `setsid` and take the console, which has no leader, as getty hands a
 * terminal to a login session.
 *
 * The scheduler is enabled, since fork and waitpid need it, and the debug
 * shell stops reading the console. The boot and the debug shell hold the big
 * lock until here; a program in user mode holds none, so it is let go, with
 * interrupts off, just before the entry.
 */
static void enter_program(struct mm *mm, const char *name, uintptr_t entry, uintptr_t sp)
{
    process_t *user_proc;
    int init_is_starting;

    user_proc = alloc_process();
    if (!user_proc) {
        mm_put(mm);
        console_puts("exec: Failed to allocate process");
        return;
    }

    user_proc->state = PROC_RUNNING;
    user_proc->mm = mm;
    user_proc->space_root = mm->space_root;
    user_proc->page_dir = mm->dir;
    user_proc->kernel_stack = (uintptr_t)stack + KERNEL_STACK_SIZE;
    user_proc->parent_pid = current_proc ? current_proc->tgid : 0;
    proc_set_comm(user_proc, name);
    current_proc = user_proc;
    fpu_restore(user_proc->fpu);

    init_is_starting = (init_pid == 0 && !debug_shell_is_home);

    if (init_pid == 0) {
        init_pid = user_proc->pid;
    }

    if (!init_is_starting) {
        tty_set_session(&console_tty, (int)user_proc->sid, (int)user_proc->pgid);
    }

    switch_page_directory(mm->dir);

    sched_enable();

    kernel_shell_active = false;

    arch_tls_set(user_proc->tls_base);

    arch_irq_disable();
    (void)bkl_drop();
    arch_enter_user(entry, sp);

    console_puts("exec: returned from user mode");
}

/**
 * Load the ELF program `argv[0]` and run it in ring 3, with the `argc`
 * strings of `argv` as its arguments and a default environment (PATH, HOME
 * and TERM).
 *
 * The program is read from the KRFFS root by path, or, when the root is not
 * KRFFS, through the root's mount, the node held and open until the segments
 * are in. The segments are read from the file straight into a new address
 * space, so no buffer bounds the program's size, and the file is kept with
 * the address space (`boot_keep_exe`). The initial stack is the one execve
 * builds (`boot_stack`), with the auxiliary vector and its 16 random bytes.
 * The segments are mapped as they are loaded; the regions made after them
 * let the fault handler serve the heap, the stack and later mappings.
 *
 * Enters user mode through `enter_program` and does not return. A setup
 * failure prints a diagnostic and returns to the caller.
 */
static void exec_program(int argc, char *argv[])
{
    const char *path;
    uintptr_t entry_point;
    elf_image_t img;
    struct mm *mm;
    uintptr_t user_sp;
    uintptr_t stack_bottom = USER_STACK_BASE;
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

    if (elf_setup_vmas(&mm->vm, &img, USER_STACK_BASE, VM_STACK_MAX_GROWTH) != 0 ||
        (stack_bottom < USER_STACK_BASE && !vm_grow_stack(&mm->vm, stack_bottom, RLIM_INFINITY))) {
        mm_put(mm);
        console_puts("exec: no memory for the program's regions");
        return;
    }
    mm_set_exe(mm, path);
    enter_program(mm, path, entry_point, user_sp);
}

/**
 * Halt a boot that mounted no root at all, rather than open the debug shell.
 * Returns only when a root is mounted.
 *
 * The kernel runs the debug shell when it cannot start /sbin/init, and that
 * shell authenticates nobody. That is right for one image, the test image,
 * whose mounted root has no init and which the harness drives. Every other
 * way to the shell is a root that did not mount: a disk root on a machine
 * whose disk is absent, damaged or skipped, or a module that did not mount.
 * Those halt here with a line saying so. `rach2.rescue` asks for the shell on
 * purpose, and this runs only when it was not given.
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
 * Returns only when the boot may go on.
 *
 * `rach2.nodisk` skips the disk probe. It is a diagnostic, and a token that a
 * person can edit at the boot prompt is no security boundary, but it must not
 * become a way past the login. On a boot line whose root is on the disk, the
 * skip leaves nothing to mount, /sbin/init cannot be reached, and the kernel
 * would run the debug shell, which authenticates nobody. That combination
 * halts here, unless the line also asks for the debug shell on purpose
 * (`rach2.rescue` or `init=rescue`): a person who skips a suspected disk
 * probe on the way to the shell is using these tokens as they are meant.
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
 * Halt the machine because init `pid` has ended. Never returns.
 *
 * On a normal boot /sbin/init is PID 1 and runs for as long as the machine
 * does; once it exits or is killed, nothing reaps orphans and no login is
 * left, so the kernel stops here rather than run the debug shell, which
 * authenticates nobody. `rach2.rescue` asks for that shell on purpose. Called
 * on every path that ends init (`terminate_current_process` here, and
 * sys/syscall_proc.c) when the debug shell is not the supervisor.
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
 * Start /sbin/init as PID 1, through `exec_program`, which does not return
 * when the program runs. Returns only when no root is mounted or the root
 * has no /sbin/init (the test image), so the caller can run the debug shell.
 *
 * /sbin/init is the only program the kernel starts as a real init. A real
 * init owns no controlling terminal and gives the console to the login
 * sessions it starts, as getty does; a bare shell started here would neither
 * take the console nor survive its own exit, which would halt the machine.
 *
 * A lookup that fails for any other reason, an I/O error or a loop of
 * symbolic links, halts the machine, and so does an init that is there and
 * does not load (a bad ELF, or no memory). The machine has a root and an
 * init on it, and the shell the caller would reach authenticates nobody, so
 * a damaged disk must not be a way to a root prompt.
 *
 * The screen changes hands before init starts. What follows on it is the
 * login prompt and what the person at it runs, so the kernel's records go to
 * the ring, the serial port and netconsole, which lose nothing, and to the
 * screen only at `KLOG_CRIT`, so that a panic still reaches the person in
 * front of the machine. Three boots keep their messages on the screen: one
 * whose boot line named a level; a recovery boot checking file systems
 * (`rach2.fsck`), which has minutes of work to show and would otherwise look
 * like a machine that had stopped; and one whose root is mounted read-only,
 * whose owner needs to know before working on it. The screen is not cleared,
 * so the login prompt appears under the boot log a person has just read.
 *
 * Process numbering restarts at 1 (`proc_reclaim_boot_pids`) only once this
 * boot is certainly starting init, so that the rescue and init-less boots
 * keep the numbering the probes and the harness expect.
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

    {
        struct vfs_mount *root = vfs_root_mount();
        int root_read_only = root != NULL && (root->flags & VFS_MS_RDONLY) != 0;

        if (!klog_screen_level_was_asked_for() && !kcmdline_has("rach2.fsck") && !root_read_only) {
            klog_set_screen_level(KLOG_CRIT);
        }
    }
    klog(KLOG_INFO, "boot", "starting /sbin/init");

    proc_reclaim_boot_pids();

    init_argv[0] = init_path;
    init_argv[1] = NULL;
    exec_program(1, init_argv);

    klog(KLOG_CRIT, "panic",
         "%s present but failed to load; halting "
         "(boot rach2.rescue for the debug shell)\n",
         init_path);
    sched_disable();
    for (;;) {
        arch_stop();
    }
}

/* The page cache's backing over the RAM-disk module, for `rach2.fswindow`:
   KRFFS mounted on the window reads and writes the module image through these
   callbacks, a page at a time, so the whole cache path runs without a disk. */
static uint8_t *_krffs_stub_base;
static uint32_t _krffs_stub_size;

/** Fill `page` from the module's bytes at `off`, with zeros past its end.
 * Returns 0. */
static int krffs_stub_read(uint32_t off, void *page)
{
    uint32_t i;
    for (i = 0; i < PAGE_SIZE; ++i) {
        ((uint8_t *)page)[i] = (off + i < _krffs_stub_size) ? _krffs_stub_base[off + i] : 0;
    }
    return 0;
}

/** Write `page` into the module at `off`, the bytes past its end dropped.
 * Returns 0. */
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
 * Mount KRFFS on the demand-paged window, backed by the RAM-disk module of
 * `disk_size` bytes at `disk_base`. Returns 0 mounted, -1 otherwise.
 *
 * The page cache is pointed at the module through the stub, and KRFFS is
 * mounted at PCWIN_BASE: `init_krffs`'s first read of the root node is the
 * window's first fault, which the cache serves. KRFFS walks the window by
 * pointer as it walks a module mapped in place. A module larger than the
 * window is not mounted, and a mount that fails takes the backing away again,
 * so that a later fault cannot read the module for a file system that is not
 * mounted.
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
 * Flush the cache and then clear the dirty-mount flag, for a clean shutdown.
 * Returns 0 when the flag is cleared and -1 when it is not, so that a caller
 * does not report a clean unmount the data did not reach. Returns -1 at once
 * unless KRFFS is mounted from a disk.
 *
 * The kernel sets the root node's dirty bit on a disk mount, writing it
 * through, and only this clears it. The cache is flushed before the clear
 * and again after it. A single flush after the clear could write the cleared
 * root page while a later frame's write-back failed: `pc_sync` walks the
 * slots by index and fails whole, having written the ones that succeeded, so
 * the disk would read clean with data missing while the caller reported a
 * failure. With a flush first, the flag depends on the data: when that flush
 * fails the bit is not touched, the disk keeps the bit the mount wrote
 * through, and the next boot asks for `fsck.krffs`. A failure of the second
 * flush leaves the cleared byte in a dirty frame and the disk still marked,
 * which errs the same way.
 *
 * The root's mode is read before either flush so that the store which
 * clears the bit needs no disk I/O. The store goes through the demand-paged
 * window, where a fill that cannot be served is a panic and not an error
 * return, and the read first makes it a write fault on a resident frame,
 * which is a remap. `pc_sync` protects frames again but evicts none, so the
 * frame is still resident when the store arrives.
 *
 * The read itself can still panic. Bringing a page in needs a free frame,
 * taking a frame needs an eviction, and an eviction needs a write; on a disk
 * that can no longer be written, with the root evicted and every slot dirty,
 * the fill fails and the machine panics here instead of returning. That
 * machine has already lost its disk, and the panic leaves the flag set,
 * which is what this function protects. Keeping the root's frame resident
 * for the whole mount would remove the case, and is a change to the page
 * cache.
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
 * The one way out of a running Rach2, from the debug shell or from
 * reboot(2). Scheduling is disabled first, so that a second caller cannot
 * exist: the timer does not switch away from a system call (`in_syscall`),
 * `sched_disable` covers the debug shell and the kernel threads, where it
 * would, and nothing returns from here to enter again. It stays ahead of the
 * flush for that reason.
 *
 * A KRFFS root on a disk is flushed and its dirty-mount flag cleared
 * (`krffs_disk_mark_clean`), and the clean-unmount line is printed only when
 * the flag was cleared: a failure has already said why, and a line printed
 * either way would be one no test could trust. A root of another type is
 * unmounted as mount(2)'s are, its buffers written and its valid bit set, so
 * that the next mount does not check it. On any other machine the page cache
 * is flushed. A failed flush does not keep the machine from stopping, since
 * an unwritable disk would otherwise make a machine that cannot be stopped;
 * it costs the flag, which stays set, so the next boot asks for `fsck.krffs`.
 *
 * The goodbye line is a log record and not a console line, so that it reaches
 * the serial port, the ring and netconsole, and the screen too on a boot that
 * did not give the screen to init: a machine read over netconsole has no
 * screen, and this is the last line it sends. The USB controller and then
 * every network card are stopped after it, so that no device writes into its
 * rings while the machine stops; a card sends what it was given before it is
 * silenced, so the goodbye line is sent and not only queued, and the records
 * `power_stop` raises after it reach the port and the ring only. Stopping the
 * cards inside `power_stop` would put knowledge of the subsystems in the one
 * file that has none.
 */
void kernel_shutdown(int how)
{
    sched_disable();

    if (_krffs_on_disk) {
        if (krffs_disk_mark_clean() == 0) {
            klog(KLOG_INFO, "shutdown", "clean unmount: dirty-mount flag cleared");
        }
    } else if (vfs_root_mounted() && vfs_root_mount()->type != &vfs_krffs_type) {
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
    klog(KLOG_INFO, "shutdown", "Stopping the system. Goodbye!");
    usb_stop();
    {
        int i;

        for (i = 0; i < netdev_count(); i++) {
            netdev_stop(netdev_at(i));
        }
    }
    power_stop(how);
}

/** The page cache's backing on the disk: read the page at byte `off` of the
 * KRFFS partition. Returns 0, or the device's error. */
static int krffs_disk_read(uint32_t off, void *page)
{
    return _krffs_disk_part.device.read(&_krffs_disk_part.device, off / BLOCK_SECTOR_SIZE,
                                        PAGE_SIZE / BLOCK_SECTOR_SIZE, page);
}

/** As `krffs_disk_read`, writing the page. */
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
 * otherwise. Returns NULL, logged, when `rach2.boot=` names no one partition
 * or names one that is not a first entry.
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
 * These two devices are the part of the disk that every image may write, the
 * everyday one included: the kernel slots, the boot menu and GRUB's core
 * image. That has to hold on a machine that boots a RAM image and mounts
 * nothing from the disk, which is the recovery case, so the two depend on no
 * mount: a machine with no mounted root can still repair the boot partition
 * that stopped it booting.
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
 * `/dev/grubcore` is published over that partition instead, from the same
 * table (`devfs_grubcore_publish_gpt`).
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
        devfs_grubcore_publish_gpt(disk, &reg->table);
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

/** The little-endian 32-bit word at `p`. */
static uint32_t sb_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The partition device the ext2 root is mounted on. */
static block_partition_t _ext2_root_part;

/* The superblock `ext2_root_at` read last, kept for the identity checks. */
static uint8_t ext2_sb[BLOCK_SECTOR_SIZE * 2];

/** What the ext2 superblock of the `count` sectors at `start` of `disk`, at
 * sector `start + 2`, says: 0 for no ext2 magic, a partition too small, or a
 * failed read; -1 for features this driver does not mount; 1 for a mountable
 * volume. */
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
 * Mount the ext2 root at `start` of `disk`, `count` sectors, published as
 * `path`. Returns 0 mounted, -1, logged, when `path` is not published, is
 * claimed already, or does not mount.
 *
 * The root reads the disk itself, as the KRFFS root does: the partition nodes
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

/**
 * Mount an ext2 root from the boot disk as entry zero of the mount table
 * (`rach2.root=ext2`): the first of the table's entries 1 to 4 whose
 * superblock has the magic and a feature set the driver can mount at all.
 * The check at mount decides whether that root is writable; a later
 * partition is not tried in its place. The window is claimed as `mount(2)`
 * claims one, so the same partition cannot be mounted twice. Returns 0
 * mounted, -1 otherwise, the reason logged under `ext2: root`.
 *
 * The entries and the reads are the boot disk's, which `rach2.boot=` can make
 * a disk other than the first the block layer published. The entries come
 * from the disk's registry, whose GUID table entries keep their 64-bit
 * extents, so a root past 2 TiB is found. The node is the one the entry has
 * in that registry, since naming another disk's partition would claim that
 * one and leave this one free to be mounted a second time.
 */
static int init_ext2_root(void)
{
    block_device_t *disk;
    struct disk *reg;
    int number;

    if (block_ready() != 0 || !block0 || !disc_table_ok) {
        klog(KLOG_ERR, "ext2", "root: no boot disk with a partition table");
        return -1;
    }
    disk = boot_disk_device ? boot_disk_device : block0;
    reg = disk_of(disk);
    for (number = 1; reg && number <= MBR_PART_COUNT; number++) {
        char path[24];
        int slot, found = -1, at;

        for (slot = 0; slot < reg->table.count; slot++) {
            if (reg->table.parts[slot].number == number) {
                found = slot;
            }
        }
        if (found < 0) {
            continue;
        }
        part_node_path(path, reg, found);
        at = ext2_root_at(disk, reg->table.parts[found].start_lba,
                          reg->table.parts[found].sector_count);
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
        return ext2_root_mount(disk, reg->table.parts[found].start_lba,
                               reg->table.parts[found].sector_count, path);
    }
    klog(KLOG_ERR, "ext2", "root: no ext2 partition on the disk");
    return -1;
}

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

/**
 * Mount KRFFS from the boot disk through the page-cache window
 * (`rach2.fsdisk` or `rach2.root=disk`). Returns 0 mounted, -1 otherwise,
 * the reason logged.
 *
 * The partition is the first of the four entries discovery read and judged
 * (`disc_parts`) whose first sector is a KRFFS root: the 16-bit magic, the
 * root type, and a size that is not zero and fits the partition. Checking the
 * root makes up for the weak 16-bit magic and does not depend on the entry's
 * type byte, so a disk partitioned for GRUB, with the boot partition first
 * and KRFFS after it, still mounts. LBA 0, which has the boot code, is not
 * read. The node the root came from, for /proc/mounts, is the registry's
 * entry that starts where the partition does. `krffs_root_mount` mounts it.
 */
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
 * Mount the KRFFS root at `start_lba` of `disk`, `sector_count` sectors,
 * through the page-cache window. `found` is the entry of `disc_parts` that
 * `init_krffs_disk` chose, or -1 for a root named by identity, whose overlap
 * with the boot partition is judged by range. Returns 0 mounted, -1
 * otherwise, the reason logged.
 *
 * A malformed partition is turned away before the mount, so that a bad table
 * fails here instead of faulting past the disk or the window later: it must
 * not be empty, must be a whole number of pages, since the cache moves a page
 * at a time, must lie within the disk, and must fit the window, which also
 * keeps its size in bytes within 32 bits. The device must be fault-safe
 * (`BLOCK_FAULT_SAFE`): the window faults with interrupts off and polls the
 * device there, so a device that completes later cannot back it.
 *
 * The root's dirty bit is read, a set one reported, and then set and written
 * through (`pc_sync`), so that a crash before the clean shutdown clears it
 * leaves the mark on the disk for fsck.krffs. A bit that does not reach the
 * disk fails the mount, since a later crash would leave an image fsck.krffs
 * reads as clean; `init_krffs` has published `krffs_fs` at the window by
 * then, so it is cleared before the backing is dropped, or a later path
 * would read the window with nothing behind it.
 *
 * The orphans a previous shutdown left, nodes that only descriptors kept and
 * that nothing freed once those were gone, are reclaimed only after the bit
 * is on the disk. No descriptor exists at mount, so a reserved node with no
 * name is garbage. The repair rewrites the allocator's metadata, and on an
 * image larger than the cache those pages can be evicted part way through,
 * so a reset during it must leave an image marked dirty, which fsck.krffs
 * then finishes. It runs whatever the bit said before the mount: a clean
 * halt closes no descriptors, so a cleanly unmounted image can have an orphan
 * too, and one left for later would remain once fsck.krffs cleared the bit.
 *
 * The boot partition was judged and published before any root was chosen
 * (`discover_boot_devices`), so a machine that mounts nothing still has it.
 * The mount adds what discovery cannot know, which entry has the root, and a
 * boot partition that overlaps the root is withdrawn.
 */
static int krffs_root_mount(block_device_t *disk, uint32_t start_lba, uint32_t sector_count,
                            int found)
{
    const struct mbr_part *parts = disc_parts;
    uint32_t part_bytes;

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

    {
        krffs_node_t *root = (krffs_node_t *)PCWIN_BASE;
        uint32_t reaped;

        if (root->mode & KRFFS_MODE_DIRTY) {
            klog(KLOG_WARN, "krffs", "not cleanly unmounted last time; run fsck.krffs");
        }
        root->mode |= KRFFS_MODE_DIRTY;
        if (pc_sync() != 0) {
            klog(KLOG_WARN, "krffs", "dirty flag did not reach the disk; mount aborted");
            krffs_fs = 0;
            pc_set_backing(0, 0, 0);
            return -1;
        }

        reaped = krffs_reap_orphans();
        if (reaped != 0) {
            klog(KLOG_INFO, "krffs", "reaped %d orphan(s) with no descriptor left", (int)reaped);
            if (pc_sync() != 0) {
                klog(KLOG_WARN, "krffs", "the orphan repair did not reach the disk");
            }
        }
    }
    _krffs_on_disk = 1;

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

/* A root named by identity, `rach2.root=`: the parsing and the matching, and
 * the mount, `init_root_by_identity`. */

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

/** Whether `s` begins with `prefix`. */
static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) {
            return 0;
        }
    }
    return 1;
}

/** Write the decimal `v` into `out` at `*n`, advancing `*n` past it, and end
 * the string there. */
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
 * Whether `rach2.root=` names the root by identity, its value read into
 * `root_spec`: not when the option is absent or is `ram`, `disk` or `ext2`.
 * A value too long for `root_spec` still names the root, with an empty
 * `root_spec` that matches nothing: an identity that cannot be read is not
 * an absent one, and the boot does not fall back to another root.
 */
static int root_spec_named(void)
{
    if (!kcmdline_value("rach2.root", root_spec, sizeof(root_spec))) {
        root_spec[0] = '\0';
        return kcmdline_count("rach2.root") > 0;
    }
    return !str_equal(root_spec, "ram") && !str_equal(root_spec, "disk") &&
           !str_equal(root_spec, "ext2");
}

/**
 * Mount the root `spec`, the identity `rach2.root=` names. Returns 0
 * mounted, -1 when it is not found, is ambiguous, or does not mount;
 * `*krffs_mounted` says whether a KRFFS root came up, for the caller's
 * report.
 *
 * An identity names the root by what the partition table says of it rather
 * than by its place: `PARTUUID=` a GPT entry's GUID, `MBRID=id,M` an MBR's
 * disk id and entry number, `GPTID=guid,M` a GUID table's disk GUID and
 * entry number, `UUID=` an ext2 volume's UUID, `LABEL=` a GPT entry's name
 * or an ext2 volume's label, or `/dev/diskNpM` as the rescue override. Every
 * partition of every registered disk is compared, and the root is mounted
 * only when exactly one matches: none is "not found", two is "ambiguous",
 * and both leave nothing mounted, because a first match would put a clone's
 * root where the named one was meant. `rach2.rootfstype=` names the type,
 * `krffs` or `ext2`; without it the partition's first sectors decide, KRFFS
 * tried first. A KRFFS root must lie within the 32-bit sector numbers the
 * window addresses.
 */
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

/**
 * Measure what the machine says about time, on the hardware it runs on, and
 * log it, at every boot and at the debug shell's `test`.
 *
 * Three things a program cannot check for itself and a host test cannot check
 * at all: that the tick arrives at all, that a delay of a given length takes
 * that long, and that the clock and the tick agree. The last has failed on
 * real hardware, where an interrupt handler took a latch of the 8254 between
 * two samples and 2000 us read as 1361 us (dev-log/43-machine/notes.md).
 *
 * The numbers are printed rather than a verdict. A machine whose delays run a
 * third short boots and logs in perfectly well, and the fault appears months
 * later as a device that fails now and then; a line with the measurements in
 * it is something a person can read and question. Paging, system calls,
 * context switching, isolation, the heap, the file system and the loader are
 * not checked here: the first program that starts exercises them, and the
 * host tests and the probe suites cover them.
 *
 * The record's level is the clock's period accounting, checked on fixed
 * readings by `timer_clock_check`: a sweep counted as a period, or a period
 * counted twice, fails it on every machine and a stopped machine cannot.
 * The delay, the wait and the walk are measured against time the host owns:
 * a hypervisor that stops the machine and then delivers the ticks it missed
 * reads a 200 ms delay as twice that, or puts the missed ticks inside a walk
 * and not its clock readings, and a suite that judged them would be judging
 * the host's load. They are printed as warnings for a person to read. The
 * time-stamp counter's calibration is logged last.
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

/**
 * Sweep the word-at-a-time copies, the fill and the move both ways, on this
 * processor, when `rach2.strings` is on the boot line.
 *
 * `k_memcpy` and `k_memset` move whole words with the architecture's string
 * instructions (`arch_copy_words` and `arch_fill_words`, `rep movs` and `rep
 * stos` on an x86) and the last bytes with a loop, and `k_memmove` copies
 * forward through `k_memcpy` (sys/lib/kstring.c). The host test beside them
 * (tests/host/kstring_hosttest.c) builds the byte loops alone, so it never
 * runs the instructions the machine runs. Every boot uses these routines
 * constantly, which finds a broken one at once and says nothing about the
 * length or the alignment that breaks a rarely taken tail.
 *
 * So the sweep is here, where the instructions are, and runs only when asked
 * for, since it takes about a second: 68 lengths at four source alignments
 * and four destination alignments, each copied, filled and moved in both
 * directions, with the bytes on each side of the range required to be
 * untouched. The fill is tried with two bytes, one with the top bit set and
 * one without: a fill that sign-extended its byte, or took the tail from the
 * wrong end of the word, shows in one and not the other. The moves are
 * between ranges whose starts are `sa + 1` apart, and the upward move checks
 * both sides of the range it wrote, since a backward copy that went one byte
 * too far changes the byte before it and the next round would put the
 * pattern back over it.
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
 * Prove the whole-disk write path, for the recovery kernel's `disk write`:
 * WRITE, FLUSH and READ of the last sector of disk 0, compared. Does nothing
 * while KRFFS is mounted from the disk.
 *
 * The last sector is written only when nothing holds it: past the boot gap,
 * it disturbs neither a file system nor a future bootloader. A GPT disk keeps
 * its backup header there, and overwriting it leaves a disk only a repair
 * tool can open. LBA 1 says which kind this is, and where it does not, the
 * last sector is read as well: a disk whose primary header is gone keeps its
 * table in the backup alone, and the write would destroy the one copy left.
 * On an MBR disk a partition may reach the last sector, as a root grown into
 * the whole disk does, and a sector one of the four entries holds is not
 * written.
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

/**
 * Run one line `cmd` of the debug shell: the commands `help` lists, the
 * audio commands `audio_debug_handle_command` takes, and `poweroff` and
 * `reboot`, which `help` does not list. An empty line does nothing.
 *
 * The file commands work on the KRFFS root alone (`krffs_fs`); `ls` leaves
 * out an orphan, a node whose name was cleared when it left the namespace
 * while a descriptor still held it, since no path reaches it. `rm` and `mv`
 * keep the rule of the unlink and rename calls: a node a descriptor still
 * holds becomes an orphan and is not freed. The shell runs with no process
 * of its own, but a transient init can be blocked holding a file when the
 * shell runs again, and freeing that node would give its address back to the
 * allocator while a descriptor still points at it. `mv` orphans the node it
 * replaces last, and only once the source is known to exist, so that a
 * rename that fails has not unnamed the destination. `write` replaces the
 * file's contents. A write past the old extent moves the file to another node
 * and frees the old one, so the descriptors on it are moved too
 * (`fs_node_moved`); `krffs_write` finds the file by its name, which does not
 * change, so the node before and after the write is the only sign of a move.
 * `mv` and `write` end their first argument at the first space, and `exec`
 * splits its line at spaces and tabs into at most fifteen arguments.
 * `netbad` pings 10.0.2.99, an address on QEMU's user network that does not
 * answer, to show the timeout.
 *
 * `ahci` probes the controller again. `ahci_init` clears `block0` and the
 * controller's state before its reset, so the boot partition, GRUB's core and
 * the whole-disk nodes, which point at the old `block0`, are withdrawn first
 * and published again from whatever answers, on success and on failure
 * alike, so that no published device is left on a driver with no port.
 * `disk` and `ahci` do nothing while KRFFS is mounted from the disk. `cat`
 * ends with a newline when the file does not.
 */
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

                if (file_size > 0 && last != '\n') {
                    console_puts("");
                }
                console_puts("--- EOF ---");
            }
        }
    } else if (str_starts_with(cmd, "elf ")) {
        const char *filename = cmd + 4;
        /* Static, off the 16 KiB stack: one command runs at a time. */
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
            krffs_truncate(file_buf, 0);
            {
                uint32_t len = (uint32_t)k_strlen(data);
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
        console_puts("Expected: the payload's greeting, then this shell again.");
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
 * returns through process_command (`enter_program` enters user mode), so the
 * post-process_command `cmd_pos = 0` never runs for it; without this reset the
 * next keystrokes append to the previous command's buffer and the shell re-runs
 * it. It also drops any stale stdin-wait state from a program that exited mid
 * read, and releases the console controlling terminal so the timer-IRQ drain
 * does not steal the input the debug shell polls itself. The release is one
 * update with interrupts off (`tty_set_session`), so the drain never sees an
 * owner changed in part; a program started later takes the terminal again
 * (`enter_program`).
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
    tty_set_session(&console_tty, 0, 0);
}

/**
 * Run the kernel debug shell: read a command line and dispatch it, forever.
 *
 * Accepts input both from the keyboard, whose keys the input layer puts in
 * cmd_buffer (`shell_input_char`), and from the serial port, which it polls
 * here so piped commands work; serial CR is folded to newline. Waits for an
 * interrupt between events. Does not return.
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

        idle_wait_irq_on();
    }
}

/**
 * Publish the disks' nodes after the root is mounted.
 *
 * With `rach2.diskdiag` this is the disk diagnostic: it probes a disk only if
 * the boot's mount did not, publishes /proc/disk and /dev/disk0, and issues
 * nothing but IDENTIFY and a sector read, so the read-only build runs it
 * unchanged. Without the flag no disk is probed, and the disks the boot
 * registered get their whole-disk nodes, mode 0400, whose writes the
 * everyday kernel's write handler fails, as Linux gives root a node for a
 * disk; /proc/disk is a synthfs name that always resolves and reports
 * whatever disks are registered. The recovery build needs no flag: the
 * whole-disk window is what that build is for.
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
 * The order of the boot is the hardware's and the allocator's.
 *
 * The boot is kernel code, and holds the big lock, as every entry into the
 * kernel does, until the first program enters user mode. Before anything is
 * allocated, the modules are checked (`modlayout_check`) and the bump
 * allocator is moved past the highest end of those below the window, since
 * the modules need not be in address order, so that it does not carve page
 * tables over their bytes; a module above the window is reserved in the frame
 * map and mapped into the module region once paging is up. A module past
 * 4 GiB, before the kernel image or over another panics, with the reason in
 * the panic's own message: `k_panic` clears the screen, on a machine with no
 * serial port a line printed before it would be gone before a camera could
 * read it, and the panic's record goes to the port and netconsole. The
 * command line is taken next (`kcmdline_capture`), from the boot record's own
 * copy of the loader's string, before the first boot option is read; a line
 * too long to keep whole is logged as cut.
 * `rach2.loglevel=N` sets how much of the log the screen shows; the ring and
 * the port keep every record, and a boot being brought up wants all of them
 * on the screen, since the screen is then the only instrument there is.
 *
 * The firmware's memory map is logged a range to a line at the debug level,
 * since only a person debugging the allocator asks where each range is, and
 * the totals at the info level; a machine with less than
 * `FRAME_MIN_MANAGED_KIB` managed after the kernel's reservations panics. The
 * page cache's page tables are made in the kernel directory before the first
 * fork copies it, and a cache slot takes its frame when it is first filled.
 * RDSEED and RDRAND are trusted entropy, so getrandom and /dev/random work;
 * jitter is a best effort, and the cryptographic sources then fail. Process
 * 0, the idle process, is the boot's own context on the boot stack.
 *
 * The disk policy this kernel was built with is logged before any disk is
 * touched; tools/inspect/check_diskpolicy.sh reads the policy from the built
 * kernel through diskcmd.c's banner. The boot devices are published, and then
 * the root is mounted: with `rach2.root=ext2` an ext2 root from the boot
 * disk, with an identity in `rach2.root=` the partition it names, with
 * `rach2.fsdisk` or `rach2.root=disk` KRFFS from the disk through the page
 * cache, which needs no module, with `rach2.fswindow` the module through the
 * cache's window, and otherwise the module in place. The source and the
 * options /proc/mounts shows, which tell a window mount from a module mounted
 * in place, are recorded in the branch that chose the mount, since afterwards
 * only whether the root is on a disk is known. An ext2 root, or a root named
 * by identity that is not KRFFS, reports itself, mounted or not, and the
 * module is not mounted in its place, so a candidate root that does not come
 * up leaves nothing mounted and a trial boot falls back to the menu entry
 * that works.
 *
 * USB comes up after the root, since a root is on a disk the block layer
 * found and never on a stick, and `rach2.root=ext2` reads the partitions
 * before any USB device exists. The SATA and NVMe disks go over to their
 * interrupts after the root too: the boot's reads were polled, where no
 * process could sleep, and from here a transfer lets the processor run
 * another. The pipe and pty lists are started before a program can make
 * either; each pipe and pty is allocated from the heap when it is made. The
 * address is applied before the network card is started, so the boot line
 * reports what the machine will use and a refused address is on the screen
 * before anything depends on it. The timing is measured after the network
 * (`check_timing`), and the string routines swept with `rach2.strings`.
 * `rach2.diag` shows the diagnostic numbers once more, on a screen nothing
 * scrolls, before init takes the console. The other processors start last
 * and wait for the big lock until the first program, or the debug shell's
 * first idle wait, lets it go.
 *
 * A normal boot then halts when no root is mounted (`refuse_unmounted_root`),
 * and otherwise starts /sbin/init as PID 1 (`boot_init_process`), which does
 * not return while init runs; the end of init halts the machine
 * (`panic_init_died`), so a normal machine does not reach the debug shell.
 * `boot_init_process` returns only when the root has no /sbin/init (the test
 * image), having halted on a lookup that failed for any other reason; that
 * boot, and a `rach2.rescue` boot, make the debug shell the supervisor. The
 * shell reads commands and runs programs with `exec` as transient inits,
 * whose end runs `shell_loop` again while the shell supervises, which is what
 * the test harness drives.
 */
void kernel_main(const struct boot_info *boot)
{
    uint32_t num_modules = boot->modules;

    bkl_enter();

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
            static char verdict_msg[128];

            modlayout_describe(verdict, bad, ranges[bad].start, ranges[bad].end, verdict_msg,
                               (uint32_t)sizeof(verdict_msg));
            k_panic(verdict_msg);
        }
        if (modules_end > mem_reserved_ptr) {
            mem_reserved_ptr = (modules_end + 0xFFF) & ~0xFFF;
        }
    }

    if (kcmdline_capture(boot->cmdline)) {
        klog(KLOG_WARN, "boot", "command line cut: past 510 bytes, at its last whole option");
    }

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

    {
        static struct frame_map map;
        const void *mmap = boot->mmap;
        uint32_t mmap_len = boot->mmap_len;
        uint32_t m;
        uint64_t addr, len;
        uint32_t type;

        for (m = 0; frame_map_entry(mmap, mmap_len, m, &addr, &len, &type); m++) {
            char line[64];

            memory_map_line(line, sizeof(line), addr, len, type);
            klog(KLOG_DEBUG, "memory", "map: %s", line);
        }
        if (frame_map_build(&map, mmap, mmap_len, boot->mem_upper_kb, mmu_phys_reach()) != 0) {
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
             "usable %lu KiB, unreachable %lu KiB, reserved %lu KiB, managed %lu KiB",
             (unsigned long)(map.usable_bytes / 1024),
             (unsigned long)(map.unreachable_bytes / 1024),
             (unsigned long)(map.reserved_bytes / 1024),
             (unsigned long)(frame_total_count() * (PAGE_SIZE / 1024)));
        if (frame_total_count() * (PAGE_SIZE / 1024) < FRAME_MIN_MANAGED_KIB) {
            klog(KLOG_ERR, "memory",
                 "refused: %lu KiB managed after the kernel's reservations, at least %d KiB needed",
                 (unsigned long)(frame_total_count() * (PAGE_SIZE / 1024)),
                 (int)FRAME_MIN_MANAGED_KIB);
            k_panic("memory: too little for this kernel");
        }
    }
    klog(KLOG_DEBUG, "boot", "paging: ready");

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
    tty_console_init();     /* console_tty: the screen and COM1, for fds 0, 1 and 2 */
    serial_enable_rx_irq(); /* COM1's bytes taken as they arrive, not at the tick */
    klog(KLOG_DEBUG, "boot", "keyboard: ready");

    platform_mouse_init();

    init_syscall();
    klog(KLOG_DEBUG, "boot", "syscall: ready");

    init_proc();
    {
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

    klog(diskcmd_policy() == DISKCMD_READ_WRITE ? KLOG_INFO : KLOG_WARN, "disk", "policy: %s",
         diskcmd_policy() == DISKCMD_READ_WRITE ? "read-write" : "read-only");

    if (num_modules > 0) {
        module_info_t *ramdisk = get_ramdisk();
        set_fore_color(light_green);
        klog(KLOG_INFO, "modules", "found %d", num_modules);
        set_fore_color(light_grey);
        if (ramdisk) {
            klog(KLOG_INFO, "modules", "ram disk: %p, %d bytes", (void *)ramdisk->start,
                 ramdisk->size);
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
        const char *source = "none";
        const char *options = "ro";

        if (kcmdline_has("rach2.root=ext2")) {
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
                root_ext2 = 1;
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

    if (kcmdline_has("rach2.nousb")) {
        klog(KLOG_INFO, "usb", "skipped: rach2.nousb");
    } else {
        klog(KLOG_INFO, "usb", "started");
        usb_report_controllers();
        usb_init();
    }

    ahci_start();
    nvme_start();

    publish_disks();

    pipe_init_pool();
    pty_init_pool();

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

    if (kcmdline_has("rach2.diag")) {
        platform_report_hold();
    }

    smp_start();

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
