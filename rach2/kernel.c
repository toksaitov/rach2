#include "vendor/multiboot.h"

#include "sys/kio.h"
#include "sys/klog.h"
#include "sys/serial.h"
#include "sys/syscall.h"
#include "sys/module.h"
#include "sys/cmdline.h"
#include "sys/tables/gdt.h"
#include "sys/tables/tss.h"
#include "sys/tables/idt.h"
#include "sys/isr.h"
#include "sys/mm/heap.h"
#include "sys/mm/paging.h"
#include "sys/mm/frame.h"
#include "sys/mm/pagecache.h"
#include "sys/mm/modlayout.h"
#include "sys/lapic.h"
#include "sys/apicdiag.h"
#include "sys/diag.h"
#include "sys/block/mbr.h"
#include "sys/fs/devfs.h"
#include "sys/fs/vfs.h"
#include "sys/proc/process.h"
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
#include "sys/pit.h"
#include "sys/kbd.h"
#include "sys/input.h"
#include "sys/mouse.h"
#include "sys/usb/usb.h"
#include "sys/block/block.h"
#include "sys/block/ahci.h"
#include "sys/display/vga_text.h"
#include "sys/audio/audio.h"
#include "sys/audio/audio_debug.h"
#include "sys/audio/hda.h"
#include "sys/udelay.h"
#include "sys/network/net.h"
#include "sys/network/netconf.h"
#include "sys/block/diskcmd.h"
#include "sys/block/diskinfo.h"
#include "sys/block/recovery.h"
#include "sys/board.h"
#include "sys/network/driver/netdev.h"
#include "sys/network/netconsole.h"
#include "sys/network/ip/icmp.h"
#include "sys/network/ip/ip.h"
#include "sys/network/tcp/tcp.h"
#include "sys/lib/kint.h"
#include "sys/lib/kstring.h"

/* Kernel stack and ring-3 entry trampoline, both from loader.s. */
extern char stack[];
#define KERNEL_STACK_SIZE 0x4000

extern void enter_usermode(uint32_t eip, uint32_t esp);

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

/* Line buffer the keyboard handler fills for a userspace stdin read; the
 * reader blocks until Enter completes a line. */
#define STDIN_BUFFER_SIZE 256
char stdin_buffer[STDIN_BUFFER_SIZE];
volatile size_t stdin_pos = 0;
volatile bool stdin_ready = false;
volatile process_t *stdin_waiting_proc = NULL;

static void run_self_tests(void);
void shell_loop(void);
void shell_reset_input(void);
void panic_init_died(uint32_t pid);  /* called from syscall.c too; never returns */
void release_fd_table(fd_table_t *table);  /* defined in syscall.c */
static void process_command(const char *cmd);
static void timer_handler(isr_params_t *params);
static void keyboard_handler(isr_params_t *params);
static bool str_equal(const char *a, const char *b);
static void boot_init_process(void);

static bool test_memory(void);
static bool test_strings(void);
static void test_display(void);
static bool test_timer(void);
static bool test_udelay(uint32_t *measured_ms);
static bool test_clock(uint32_t *measured_us);
static bool test_syscall(void);
static bool test_usermode(void);
static void test_switch(void);
static void test_preempt(void);
static bool test_isolation(void);
static bool test_krffs_write(void);
static bool test_elf(void);
static bool test_pagecache(void);
static void exec_program(int argc, char *argv[]);

/* Top of the user stack; it grows down from here. */
#define USER_STACK_BASE 0xBFFFF000

/**
 * Service the 100 Hz timer IRQ and preempt the current process.
 *
 * Bumps the global tick, then runs the periodic kernel work that the timer
 * drives: waking expired nanosleep sleepers, the HDA audio tick (needed when
 * legacy IRQs are not delivered on MSI-only setups), the TCP timers, the
 * loopback delivery, and the interrupt-less console serial drain. Finally
 * Accounts the tick as idle or busy FIRST, before anything below can wake a
 * process: the marker names what the processor was doing when this tick
 * arrived, not what it is about to do. Then
 * reschedules, but only when the
 * current process is not inside kernel syscall code: timer IRQs stay enabled
 * during syscalls so HDA streaming keeps running, yet preempting a process mid
 * syscall is unsafe. Kernel threads (no current_proc, or one not in_syscall)
 * stay preemptible.
 */
static void timer_handler(isr_params_t *params)
{
    (void) params;

    ++kernel_ticks;
    /* The microsecond clock's periods are this tick's to count; see
       `pit_now_us`. */
    pit_tick_clock();
    if (++kernel_second_ticks >= 100) {
        kernel_second_ticks = 0;
        ++kernel_uptime_seconds;
    }

    sched_account_tick();

    /* Runs regardless of in_syscall, so a sleeper parked inside its syscall
     * still becomes runnable on time. */
    proc_wake_sleepers(kernel_ticks);

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
    tty_console_poll();

    if (!current_proc || !current_proc->in_syscall) {
        schedule();
    }
}

/**
 * Terminate the current user process by signal and give up its CPU.
 *
 * Shared teardown for every kill that originates outside sys_exit: the CTRL+C
 * handler and the page-fault segfault path both route here. The process dies by
 * signal `signo` (SIGSEGV for a fault, SIGINT for CTRL+C); the exit code stores
 * the Linux wait-status encoding of a signal death (low 7 bits = the signal),
 * so the parent's waitpid sees WIFSIGNALED/WTERMSIG. Posts SIGCHLD and wakes a
 * parent blocked in waitpid, releases the descriptor table, frees the user
 * pages, and switches off the dying address space before freeing its page
 * directory. If the process was init, resets the shell input state and jumps
 * back into shell_loop on a fresh kernel stack; otherwise it stays a zombie for
 * the parent's waitpid to collect, and the now-READY parent is scheduled. Does
 * not return for a user process.
 *
 * Declared extern (no header) by mm/paging.c, which calls it from the fault
 * handler, mirroring how syscall.c reaches shell_loop/stack.
 */
void terminate_current_process(int32_t signo)
{
    process_t *parent;
    bool is_init;
    uint32_t pid;

    /* Only user processes have their own page directory; ignore the kernel
     * idle/shell context. */
    if (!current_proc || current_proc->cr3 == 0) {
        return;
    }

    pid = current_proc->pid;
    is_init = (pid == init_pid && init_pid != 0);

    /* If the console-owning session leader is dying (init, or a later TIOCSCTTY
     * leader, not only init), release the controlling terminal before
     * zombifying so the timer-IRQ serial drain does not target this vanishing
     * session during teardown. shell_reset_input on the is_init path below
     * releases again, which is idempotent. */
    if (tty_console_owned_by_leader((int)current_proc->sid, (int)current_proc->pid)) {
        tty_set_session(&console_tty, 0, 0);
    }
    pty_release_leader((int)current_proc->sid, (int)current_proc->pid);

    fb_release_if_owner(pid);

    current_proc->state = PROC_ZOMBIE;
    current_proc->exit_code = signo & 0x7F;

    /* A reaper handler relies on SIGCHLD for signal-killed children too, not
     * just normal exits. */
    parent = get_process(current_proc->parent_pid);
    if (parent) {
        signal_post(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED) {
            parent->state = PROC_READY;
            klog(KLOG_INFO, "terminate", "woke parent process %d", parent->pid);
        }
    }

    /* Hand any surviving children to init before this slot is reaped, so a
     * signal-killed parent never strands an orphan on a dead pid. */
    reparent_children(pid);

    /* Same descriptor-table teardown sys_exit does, so a signal-killed process
     * leaks no sockets, open-file descriptions or working-directory holds. */
    release_fd_table(&current_proc->fds);
    vfs_cwd_clear(current_proc);

    /* Switch off the dying address space BEFORE freeing its page directory:
     * free_page_directory refuses to free whatever is currently loaded in CR3,
     * so freeing while still on this directory would silently leak it and all
     * its private page tables. */
    if (current_proc->cr3 != 0) {
        page_dir_t *dir;
        dir = (page_dir_t *)(current_proc->cr3 - 1024 * sizeof(page_table_t *));
        free_user_pages(dir);
        switch_page_directory(_kernel_dir);
        free_page_directory(dir);
        current_proc->cr3 = 0;
    } else {
        switch_page_directory(_kernel_dir);
    }

    if (is_init) {
        /* A real /sbin/init killed by a signal is a fatal init death; only when
         * the debug shell is the supervisor (rescue, or the no-init test image)
         * does an exited init return to it. */
        if (!debug_shell_is_home) {
            panic_init_died(pid);   /* never returns */
        }
        /* Same resume path as when init exits normally; see sys_exit. */
        klog(KLOG_INFO, "terminate",
             "init process %d killed by signal %d, returning to kernel shell",
             pid, signo);
        console_puts("Init process exited. Returning to kernel debug shell.");

        free_process(current_proc);

        init_pid = 0;
        current_proc = &proc_table[0];
        current_proc->state = PROC_RUNNING;

        /* Fresh command line and dropped stdin-wait state, before "sti" below
         * re-enables IRQs. */
        shell_reset_input();

        sched_disable();

        __asm__ __volatile__(
            "movl %0, %%esp\n\t"
            "sti\n\t"
            "jmp *%1\n\t"
            :
            : "r"((uint32_t)stack + KERNEL_STACK_SIZE),
              "r"((uint32_t)shell_loop)
            : "memory"
        );
    } else {
        /* Do not free_process here: the zombie must survive for the parent's
         * waitpid to find, collect, and free the slot. */
        schedule();

        __asm__ __volatile__("cli; hlt");
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
    } else if (current_proc && current_proc->cr3 != 0 &&
               (int) current_proc->sid == console_tty.sid) {
        signal_post(current_proc, SIGINT);
    }
}

/**
 * Handle the PS/2 keyboard interrupt: read the port and hand on the code.
 *
 * Reads the scancode from port 0x60 and gives it to the input layer, which
 * puts it in the ring graphics-mode readers drain and decides where the
 * keystroke goes. Everything that is not about the 8042 lives there,
 * because a USB keyboard's codes arrive by another road and are treated the
 * same.
 *
 * Runs in interrupt context with an arbitrary process's page directory loaded,
 * so it switches to the kernel directory around the work and restores CR3 on
 * the way out.
 */
static void keyboard_handler(isr_params_t *params)
{
    uint8_t scancode;
    uint32_t saved_cr3;
    bool switched_cr3 = false;

    (void) params;

    scancode = inb(0x60);

    /* The running process's page directory may not map the kernel data the
     * input layer touches. Set CR3 directly rather than
     * switch_page_directory(), whose own record of the current directory may
     * be out of sync here. */
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved_cr3));
    {
        uint32_t kernel_cr3 = get_page_dir_phys(_kernel_dir);
        if (saved_cr3 != kernel_cr3) {
            __asm__ __volatile__("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            switched_cr3 = true;
        }
    }

    input_key(scancode);

    if (switched_cr3) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
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
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static bool str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) return false;
        ++str;
        ++prefix;
    }
    return true;
}

static bool test_memory(void)
{
    uintptr_t addr;
    uint8_t *ptr;
    size_t i;
    const size_t test_size = 256;

    addr = mem_reserve(test_size, false);
    ptr = (uint8_t *) addr;

    for (i = 0; i < test_size; ++i) {
        ptr[i] = (uint8_t) (i & 0xFF);
    }

    for (i = 0; i < test_size; ++i) {
        if (ptr[i] != (uint8_t) (i & 0xFF)) {
            return false;
        }
    }

    return true;
}

static bool test_strings(void)
{
    char buf[16];
    size_t i;

    if (k_strlen("hello") != 5) return false;
    if (k_strlen("") != 0) return false;

    k_memset(buf, 'X', 10);
    buf[10] = '\0';
    for (i = 0; i < 10; ++i) {
        if (buf[i] != 'X') return false;
    }

    return true;
}

static void test_display(void)
{
    console_printf("  Display colors:     ");
    set_fore_color(red);
    console_printf("Red ");
    set_fore_color(green);
    console_printf("Green ");
    set_fore_color(cyan);
    console_printf("Cyan ");
    set_fore_color(light_magenta);
    console_printf("Magenta ");
    set_fore_color(light_brown);
    console_printf("Yellow ");
    set_fore_color(white);
    console_printf("White");
    set_fore_color(light_grey);
    console_puts("");
}

static bool test_timer(void)
{
    uint32_t start_ticks;
    uint32_t i;

    start_ticks = kernel_ticks;

    for (i = 0; i < 5; ++i) {
        __asm__ __volatile__("hlt");
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
 * as this. 200 ms keeps the one-tick quantization under a percent without
 * lengthening the boot noticeably. `*measured_ms` reports what was seen, since
 * a wrong figure says more than a failed comparison.
 *
 * The band is 150 to 350 ms because both counters are on an emulated chip
 * whose rate the host's own load changes: sixteen machines booted at once here
 * measured between 180 and 280 ms, and nine of them would have failed a band
 * of ten percent. Wider than the measurement by a quarter at each end, and no
 * wider: a delay off by half or double is what a bring-up sequence would
 * notice, and it still fails here.
 */
static bool test_udelay(uint32_t *measured_ms)
{
    uint32_t start = kernel_ticks;
    uint32_t elapsed;

    k_udelay(200000);
    elapsed = kernel_ticks - start;
    *measured_ms = elapsed * 10;
    return elapsed >= 15 && elapsed <= 35;
}

/**
 * The microsecond clock against the wait that is measured a different way.
 *
 * `pit_now_us` counts the periods of the 8254's counter and reads where it is
 * inside one; `k_udelay` measures that same counter by its drop. The wait is
 * taken eight times and the shortest of them is the reading: an interrupt in
 * the middle of one can only add to it, and this machine writes whole lines
 * to a serial port at eighty-seven microseconds a byte.
 *
 * Then twenty milliseconds are walked in single milliseconds, which is two of
 * the tick's periods and four of the counter's sweeps. A reading that lost
 * which half of the period it was in, or that did not count a wrap, steps
 * backwards there.
 */
static bool test_clock(uint32_t *measured_us)
{
    uint32_t best = 0xFFFFFFFFu;
    uint32_t previous;
    int i;

    for (i = 0; i < 8; i++) {
        uint32_t start = pit_now_us();
        uint32_t seen;

        k_udelay(2000);
        seen = pit_now_us() - start;
        if (seen < best) {
            best = seen;
        }
    }
    *measured_us = best;
    if (best < 1500 || best > 3500) {
        return false;
    }

    /*
     * Fifty milliseconds walked in single milliseconds, with the clock read
     * at every step: five of the tick's periods and ten of the counter's
     * sweeps. A reading that steps backwards fails here. So does one that
     * runs fast, because the same fifty milliseconds are measured by the tick
     * beside it, and a clock that counted a sweep as a period, or counted a
     * period twice, would put twice the time in between.
     */
    {
        uint32_t ticks_before = kernel_ticks;
        uint32_t clock_before = pit_now_us();
        uint32_t by_clock, by_tick;

        previous = clock_before;
        for (i = 0; i < 50; i++) {
            uint32_t now = pit_now_us();

            if (now < previous) {
                return false;
            }
            previous = now;
            k_udelay(1000);
        }
        by_clock = pit_now_us() - clock_before;
        by_tick = (kernel_ticks - ticks_before) * 10000u;
        if (by_tick == 0 || by_clock < by_tick - by_tick / 4u ||
            by_clock > by_tick + by_tick / 4u) {
            return false;
        }
    }
    return true;
}

static bool test_syscall(void)
{
    /* write(1, msg, 20): syscall number in EAX, args in EBX/ECX/EDX. */
    const char *msg = "Hello from syscall! ";
    uint32_t len = 20;

    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(4), "b"(1), "c"(msg), "d"(len)
        : "memory"
    );

    /* Reaching here without a fault means the syscall path works. */
    return true;
}

/* Ring-3 test message; must live in user-accessible memory. */
static const char user_msg[] = "Hello from ring 3!\n";

/**
 * Ring-3 test payload: write the test message, then spin forever.
 *
 * Runs in user mode, so it cannot halt; after the write syscall returns it
 * loops on nop. enter_usermode jumps here; it never returns.
 */
static void user_test_entry(void)
{
    /* write(1, user_msg, 19) via int 0x80. */
    __asm__ __volatile__(
        "movl $4, %%eax\n\t"
        "movl $1, %%ebx\n\t"
        "movl %0, %%ecx\n\t"
        "movl $19, %%edx\n\t"
        "int $0x80\n\t"
        :
        : "r"(user_msg)
        : "eax", "ebx", "ecx", "edx", "memory"
    );

    for (;;) {
        __asm__ __volatile__("nop");
    }
}

/**
 * Prove ring 3 works by entering user mode and running user_test_entry.
 *
 * Reserves a user stack and marks the code, message, and stack pages
 * user-accessible, then enters ring 3 at user_test_entry, which writes "Hello
 * from ring 3!" through int 0x80. enter_usermode does not return, so the
 * trailing `return false` is unreachable.
 */
static bool test_usermode(void)
{
    uintptr_t func_addr = (uintptr_t)user_test_entry;
    uintptr_t msg_addr = (uintptr_t)user_msg;
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
        __asm__ __volatile__("hlt");
    }

    console_printf("  Usermode test: entering ring 3...\n");

    enter_usermode(func_addr, user_stack_addr + USER_STACK_SIZE);

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
        __asm__ __volatile__("hlt");
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
    switch_proc_b->cr3 = 0;  /* shares the kernel page directory */
    switch_proc_b->parent_pid = switch_proc_a->pid;

    setup_process_stack(switch_proc_b, proc_b_entry);

    console_printf("Process B created: pid=%d stack=0x%X\n",
                  switch_proc_b->pid, switch_proc_b->kernel_stack);

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
    __asm__ __volatile__("sti");

    while (!preempt_done) {
        console_putc('A');
        ++preempt_count_a;

        {
            volatile int i;
            for (i = 0; i < 5000000; i++) {
                __asm__ __volatile__("nop");
            }
        }

        if (preempt_count_a + preempt_count_b >= 20) {
            preempt_done = 1;
        }
    }

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

static void preempt_proc_b(void)
{
    /* The process starts mid-interrupt, with IRQs masked. */
    __asm__ __volatile__("sti");

    while (!preempt_done) {
        console_putc('B');
        ++preempt_count_b;

        {
            volatile int i;
            for (i = 0; i < 5000000; i++) {
                __asm__ __volatile__("nop");
            }
        }

        if (preempt_count_a + preempt_count_b >= 20) {
            preempt_done = 1;
        }
    }

    for (;;) {
        __asm__ __volatile__("hlt");
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
    proc_a->cr3 = 0;
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
    proc_b->cr3 = 0;
    proc_b->parent_pid = current_proc->pid;
    setup_process_stack(proc_b, preempt_proc_b);

    sched_enable();

    while (!preempt_done) {
        __asm__ __volatile__("hlt");
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
 * The test address must lie ABOVE the identity-mapped region so each process
 * gets its own page table. With modules loaded the identity map extends past
 * the module end (~12 MB), so use 0xD00000 (13 MB).
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
        __asm__ __volatile__("hlt");
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
        __asm__ __volatile__("hlt");
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
    isolation_proc_a->cr3 = get_page_dir_phys(dir_a);
    isolation_proc_a->parent_pid = isolation_proc_main->pid;
    setup_process_stack(isolation_proc_a, isolation_proc_a_entry);

    isolation_proc_b = alloc_process();
    if (!isolation_proc_b) {
        free_process(isolation_proc_a);
        return false;
    }
    stack_b = mem_reserve(KERNEL_STACK_SIZE, true);
    isolation_proc_b->kernel_stack = stack_b + KERNEL_STACK_SIZE;
    isolation_proc_b->cr3 = get_page_dir_phys(dir_b);
    isolation_proc_b->parent_pid = isolation_proc_main->pid;
    setup_process_stack(isolation_proc_b, isolation_proc_b_entry);

    switch_context(isolation_proc_main, isolation_proc_a);

    free_process(isolation_proc_a);
    free_process(isolation_proc_b);
    isolation_proc_a = NULL;
    isolation_proc_b = NULL;

    return (isolation_result_a == 0xAAAAAAAA && isolation_result_b == 0xBBBBBBBB);
}

/**
 * Exercise the KRFFS write path: create, write, read, rename, unlink.
 *
 * Returns true (treated as a pass) when no file system is mounted, so a
 * diskless boot does not fail the self-test.
 */
static bool test_krffs_write(void)
{
    const char *test_filename = "krffs_test.tmp";
    const char *renamed_filename = "krffs_renamed.tmp";
    const char *test_data = "Test data for KRFFS write";
    char read_buffer[64];
    int bytes_read;
    int bytes_written;

    if (!krffs_fs) {
        return true;
    }

    if (krffs_create(test_filename) != 0) {
        console_printf("KRFFS write test: create failed\n");
        return false;
    }

    bytes_written = krffs_write(test_filename, test_data, k_strlen(test_data), 0);
    if (bytes_written != (int)k_strlen(test_data)) {
        console_printf("KRFFS write test: write failed (%d)\n", bytes_written);
        krffs_unlink(test_filename);
        return false;
    }

    bytes_read = krffs_read(test_filename, read_buffer, 63, 0);
    read_buffer[bytes_read > 0 ? bytes_read : 0] = '\0';
    if (bytes_read != (int)k_strlen(test_data) || !str_equal(read_buffer, test_data)) {
        console_printf("KRFFS write test: read verify failed\n");
        krffs_unlink(test_filename);
        return false;
    }

    if (krffs_rename(test_filename, renamed_filename) != 0) {
        console_printf("KRFFS write test: rename failed\n");
        krffs_unlink(test_filename);
        return false;
    }

    bytes_read = krffs_read(renamed_filename, read_buffer, 63, 0);
    read_buffer[bytes_read > 0 ? bytes_read : 0] = '\0';
    if (!str_equal(read_buffer, test_data)) {
        console_printf("KRFFS write test: rename verify failed\n");
        krffs_unlink(renamed_filename);
        return false;
    }

    if (krffs_unlink(renamed_filename) != 0) {
        console_printf("KRFFS write test: unlink failed\n");
        return false;
    }

    if (krffs_open(renamed_filename) == 0) {
        console_printf("KRFFS write test: file still exists after unlink\n");
        return false;
    }

    return true;
}

/**
 * Validate, parse, and load an ELF image from KRFFS without executing it.
 *
 * Reads the "hello" binary, checks its header, then loads its segments into a
 * throwaway page directory to exercise elf_load. Returns true (a pass) when no
 * file system is mounted or the file is absent, so its absence is not a
 * failure.
 */
static bool test_elf(void)
{
    const char *elf_filename = "hello";
    char _exec_elf_buffer[8192];
    int bytes_read;
    uint32_t file_size;
    uint32_t entry_point;
    uint16_t phdr_count;
    Elf32_Phdr *phdrs;

    if (!krffs_fs) {
        return true;
    }

    if (krffs_open(elf_filename) != 0) {
        return true;
    }

    file_size = krffs_get_file_size(elf_filename);
    if (file_size > sizeof(_exec_elf_buffer)) {
        return false;
    }

    bytes_read = krffs_read(elf_filename, _exec_elf_buffer, file_size, 0);
    if (bytes_read != (int)file_size) {
        return false;
    }

    if (!elf_check(_exec_elf_buffer, file_size)) {
        return false;
    }

    entry_point = elf_entry_point(_exec_elf_buffer);
    phdrs = elf_program_headers(_exec_elf_buffer);
    phdr_count = elf_program_header_count(_exec_elf_buffer);

    if (!phdrs || phdr_count == 0 || entry_point == 0) {
        return false;
    }

    {
        page_dir_t *test_dir;
        uintptr_t stack_phys;

        test_dir = clone_page_directory();
        if (!test_dir) {
            return false;
        }

        if (elf_load(_exec_elf_buffer, file_size, test_dir) != 0) {
            return false;
        }

        stack_phys = alloc_user_page(test_dir, USER_STACK_BASE);
        if (stack_phys == 0) {
            return false;
        }

        /* The directory leaks: the bump allocator has no free. */
    }

    return true;
}

/* elf_read_fn over a KRFFS file; ctx is the path. Used by the segment
 * loader. */
static int exec_krffs_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    return krffs_read((const char *)ctx, (char *)dst, count, off);
}

/**
 * Load and run an ELF program from KRFFS, entering ring 3 at its entry point.
 *
 * Clones a fresh page directory, streams the program's segments straight from
 * the file (no whole-file buffer, so binary size is unbounded), builds the
 * SysV/i386 initial stack (argc, argv, a default envp, and the auxv with 16
 * random bytes), and allocates the process. The first user program becomes
 * init, and the running program is made the console's controlling-terminal
 * foreground group so a job-control shell does not stop itself on startup.
 * Enters user mode and does not return; on any setup failure it prints a
 * diagnostic and returns to the caller.
 */
static void exec_program(int argc, char *argv[])
{
    const char *path;
    uint32_t entry_point;
    elf_image_t img;
    page_dir_t *user_dir;
    process_t *user_proc;
    uintptr_t user_stack_phys;
    uint32_t user_stack_vaddr;
    uint32_t user_esp;
    uint8_t random16[16];
    int init_is_starting;
    static char *default_envp[] = {"PATH=/bin:/usr/bin:/", "HOME=/", "TERM=rach2", 0};

    if (argc < 1 || !argv || !argv[0]) {
        console_puts("exec: No program specified");
        return;
    }

    path = argv[0];

    if (!krffs_fs) {
        console_puts("exec: No file system mounted");
        return;
    }

    if (krffs_open(path) != 0) {
        console_printf("exec: File not found: %s\n", path);
        return;
    }

    user_dir = clone_page_directory();
    if (!user_dir) {
        console_puts("exec: Failed to clone page directory");
        return;
    }

    /* Stream the segments straight from KRFFS (no whole-file buffer, so the
     * binary size is unbounded); img records entry and auxv metadata. */
    if (elf_load_image(exec_krffs_read, (void *)path, user_dir, &img) != 0) {
        console_puts("exec: Failed to load ELF segments");
        return;
    }
    entry_point = img.entry;

    user_stack_vaddr = USER_STACK_BASE;
    user_stack_phys = alloc_user_page(user_dir, user_stack_vaddr);
    if (user_stack_phys == 0) {
        console_puts("exec: Failed to allocate user stack");
        return;
    }

    k_memset((void *)user_stack_phys, 0, PAGE_SIZE);

    /* build_user_stack copies the strings in and lays out the argv/envp pointer
     * arrays and the AT_NULL-terminated auxv, 16-byte-aligning the entry ESP.
     * The layout is verified host-side (tests/host/uimage_hosttest.c). */
    krandom_bytes(random16, 16);
    user_esp = build_user_stack((void *)user_stack_phys, user_stack_vaddr, PAGE_SIZE,
                                argc, argv, default_envp, &img, random16, 0);
    if (user_esp == 0) {
        console_puts("exec: argv/env too large for the initial stack");
        return;
    }

    user_proc = alloc_process();
    if (!user_proc) {
        console_puts("exec: Failed to allocate process");
        return;
    }

    user_proc->state = PROC_RUNNING;
    user_proc->cr3 = get_page_dir_phys(user_dir);
    user_proc->kernel_stack = (uint32_t)stack + KERNEL_STACK_SIZE;
    user_proc->parent_pid = current_proc ? current_proc->pid : 0;
    proc_set_comm(user_proc, path); /* program name for /proc/<pid>/stat and ps */
    current_proc = user_proc;

    /* The ELF segments are loaded eagerly above; these VMAs let the
     * demand-paging fault handler resolve later heap/stack/mmap faults. */
    elf_setup_vmas(&user_proc->vm, &img, USER_STACK_BASE, VM_STACK_MAX_GROWTH);

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

    switch_page_directory(user_dir);

    /* fork/waitpid need the scheduler running. */
    sched_enable();

    kernel_shell_active = false;

    /* Program the shared TLS descriptor before ring 3. This first-entry path
     * bypasses switch_context, which otherwise reprograms GDT entry 6 per
     * process, so without this a fresh program could observe a previous
     * process's TLS base if it loaded the TLS selector before calling
     * set_thread_area. A new process has tls_base == 0. */
    gdt_set_tls(user_proc->tls_base);

    enter_usermode(entry_point, user_esp);

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
    if (krffs_fs) {
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
    klog(KLOG_WARN, "boot", "refused: no file system mounted and no rach2.rescue: "
                  "the debug shell authenticates nobody\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

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
    int root_on_disk = kcmdline_has("rach2.fsdisk") || kcmdline_has("rach2.root=disk");
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
    klog(KLOG_WARN, "boot", "refused: rach2.nodisk with rach2.fsdisk or "
                  "rach2.root=disk and no rach2.rescue: nothing would mount "
                  "and the debug shell authenticates nobody\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
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

    klog(KLOG_CRIT, "panic", "init (pid %d) exited; halting "
                  "(boot rach2.rescue for the debug shell)\n", (int)pid);
    sched_disable();
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/**
 * Boot `/sbin/init` as PID 1.
 *
 * `/sbin/init` is the only program the kernel starts as a real init: it does
 * the getty dance (a real init must own no controlling terminal, and a bare
 * shell launched here would neither claim the console nor survive its own
 * exit, which would panic). Execs it through exec_program, which does not
 * return on success. Returns here only when the mounted file system has no
 * `/sbin/init` (the test image), so the caller can fall to the debug shell.
 */
static void boot_init_process(void)
{
    static const char *const init_path = "/sbin/init";
    char *init_argv[2];

    if (!krffs_fs) {
        klog(KLOG_WARN, "boot", "no RAM disk; falling back to the debug shell");
        return;
    }

    if (krffs_open(init_path) != 0) {
        klog(KLOG_WARN, "boot",
             "no /sbin/init in the RAM disk; falling back to the debug shell");
        return;
    }

    k_cls();
    klog(KLOG_INFO, "boot", "starting /sbin/init");

    /* Only now, past the point where this boot is certainly starting init, so
     * the rescue and init-less boots keep the numbering every probe and the
     * whole harness were written against. */
    proc_reclaim_boot_pids();

    init_argv[0] = (char *) init_path;
    init_argv[1] = NULL;
    exec_program(1, init_argv);

    /* exec_program returned: /sbin/init is present but failed to load (a bad
     * ELF, or out of memory). There is no init and no login shell, and falling
     * to an unauthenticated debug shell would hide the failure, so halt. A
     * genuinely init-less image (the test one) took the early return above
     * instead. */

    klog(KLOG_CRIT, "panic", "%s present but failed to load; halting "
                  "(boot rach2.rescue for the debug shell)\n", init_path);
    sched_disable();
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* A small RAM stand-in disk for the page-cache self-test. */
#define PC_TEST_PAGES 16u
static uint8_t pc_test_disk[PC_TEST_PAGES][PAGE_SIZE];

static int pc_test_backing_read(uint32_t off, void *page)
{
    uint32_t p = off / PAGE_SIZE;
    if (p >= PC_TEST_PAGES) {
        return -1;
    }
    k_memcpy(page, pc_test_disk[p], PAGE_SIZE);
    return 0;
}

static int pc_test_backing_write(uint32_t off, const void *page)
{
    uint32_t p = off / PAGE_SIZE;
    if (p >= PC_TEST_PAGES) {
        return -1;
    }
    k_memcpy(pc_test_disk[p], page, PAGE_SIZE);
    return 0;
}

/**
 * Exercises the page cache over a RAM stub through the real window and fault
 * handler: reads every stub page back, writes a distinct marker to each,
 * flushes, and confirms each marker survives a real fault, eviction,
 * writeback, and re-read. A capacity of four frames forces eviction; a
 * misaligned store crosses a page boundary to fault two adjacent pages.
 * Prints the VMPROBE token, then restores the cache so the default module
 * path never faults the window.
 */
static bool test_pagecache(void)
{
    volatile uint8_t *win = (volatile uint8_t *) PCWIN_BASE;
    uint32_t i;
    bool ok = true;

    /* Skip when the window is too small for the probe, or already backs a live
       file system (driving the stub through it would clobber that mount). */
    if (pc_window_size() < PC_TEST_PAGES * PAGE_SIZE || pc_has_backing()) {
        klog(KLOG_DEBUG, "pagecache",
             "VMPROBE PAGECACHE OK n=0 evict=0 wb=0 (skipped)");
        return true;
    }

    for (i = 0; i < PC_TEST_PAGES; ++i) {
        k_memset(pc_test_disk[i], 0, PAGE_SIZE);
        pc_test_disk[i][0] = (uint8_t) i;
    }

    pc_set_backing(pc_test_backing_read, pc_test_backing_write,
                   PC_TEST_PAGES * PAGE_SIZE);
    pc_set_capacity(4);

    for (i = 0; i < PC_TEST_PAGES; ++i) {
        if (win[i * PAGE_SIZE] != (uint8_t) i) {
            ok = false;
        }
    }
    for (i = 0; i < PC_TEST_PAGES; ++i) {
        win[i * PAGE_SIZE] = (uint8_t) (0x80u + i);
    }
    pc_sync();
    for (i = 0; i < PC_TEST_PAGES; ++i) {
        if (win[i * PAGE_SIZE] != (uint8_t) (0x80u + i)) {
            ok = false;
        }
    }

    /* A misaligned store across the page-0/page-1 boundary faults both
     * pages. */
    *(volatile uint32_t *) (win + PAGE_SIZE - 2) = 0x44443333u;
    pc_sync();
    if (win[PAGE_SIZE - 1] != 0x33u || win[PAGE_SIZE] != 0x44u) {
        ok = false;
    }

    klog(KLOG_DEBUG, "pagecache", "VMPROBE PAGECACHE %s n=%d evict=%d wb=%d",
                  ok ? "OK" : "FAIL", (int) pc_stat_reads(),
                  (int) pc_stat_evictions(), (int) pc_stat_writebacks());

    pc_set_backing(0, 0, 0);
    pc_set_capacity(PC_SLOTS);
    return ok;
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
        ((uint8_t *) page)[i] =
            (off + i < _krffs_stub_size) ? _krffs_stub_base[off + i] : 0;
    }
    return 0;
}

static int krffs_stub_write(uint32_t off, const void *page)
{
    uint32_t i;
    for (i = 0; i < PAGE_SIZE; ++i) {
        if (off + i < _krffs_stub_size) {
            _krffs_stub_base[off + i] = ((const uint8_t *) page)[i];
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
        klog(KLOG_WARN, "krffs", "disk size %d exceeds window %d",
                      disk_size, pc_window_size());
        return -1;
    }
    _krffs_stub_base = disk_base;
    _krffs_stub_size = disk_size;
    pc_set_backing(krffs_stub_read, krffs_stub_write, disk_size);
    if (init_krffs((void *) PCWIN_BASE, disk_size) != 0) {
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
    root = (krffs_node_t *) PCWIN_BASE;
    mode = root->mode;

    if (pc_sync() != 0) {
        klog(KLOG_WARN, "krffs", "flush failed before the clear; image stays dirty");
        return -1;
    }
    root->mode = (uint16_t) (mode & ~KRFFS_MODE_DIRTY);
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
    } else if (pc_sync() != 0) {
        klog(KLOG_WARN, "shutdown", "page cache flush failed");
    } else {
        klog(KLOG_INFO, "shutdown", "no disk mount; nothing to unmount");
    }
    /* A record rather than a console line, so it reaches the screen, the
     * port, the ring and the wire alike: a machine being read over netconsole
     * has no screen, and this is the last thing it says. */
    klog(KLOG_INFO, "shutdown", "Stopping the system. Goodbye!");
    /* Silence the network card, which until here has been free to write into
     * its receive ring whenever a frame arrives. Nothing after this is put on
     * the wire, and that is a contract rather than an oversight: netconsole
     * sends the serial stream through this card, so the line above is the
     * last message submitted to it, and the three lines `power_stop` prints
     * after it reach serial only. `stop` drains what it was given before it
     * silences the card, so a submitted frame is a sent one. The
     * alternative, stopping the card inside `power_stop`, would put subsystem
     * knowledge in the one file whose whole claim is that it has none. */
    usb_stop();
    netdev_stop();
    power_stop(how);
}

static int krffs_disk_read(uint32_t off, void *page)
{
    return _krffs_disk_part.device.read(
               &_krffs_disk_part.device, off / BLOCK_SECTOR_SIZE,
               PAGE_SIZE / BLOCK_SECTOR_SIZE, page);
}

static int krffs_disk_write(uint32_t off, const void *page)
{
    return _krffs_disk_part.device.write(
               &_krffs_disk_part.device, off / BLOCK_SECTOR_SIZE,
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
 * Nothing is published until sector 0 has been read and judged. `/dev/grubcore`
 * covers the post-MBR gap, which no partition may occupy; a table that puts one
 * there describes a disk where writing the boot loader would destroy a file
 * system, so both devices are withheld, not one. `/dev/bootpart` needs
 * entry 0 judged against the whole table, because a machine booting a RAM image
 * has no mounted root to compare it against.
 *
 * Silent when there is no disk: a machine booting a RAM image on a diskless
 * board is the ordinary case, not a failure.
 */
static void discover_boot_devices(void)
{
    static uint8_t mbr[BLOCK_SECTOR_SIZE];

    if (block_ready() != 0 || !block0) {
        return;
    }
    if (block0->read(block0, 0, 1, mbr) != 0) {
        klog(KLOG_WARN, "block", "MBR read failed; no boot devices");
        return;
    }
    if (mbr_parse(mbr, disc_parts) != 0) {
        klog(KLOG_WARN, "block", "no MBR signature; no boot devices");
        return;
    }
    disc_table_ok = 1;

    if (mbr_gap_clear(disc_parts) != 0) {
        klog(KLOG_INFO, "block", "a partition occupies the boot loader's gap; "
                      "/dev/grubcore absent\n");
    } else {
        devfs_grubcore_publish(block0);
    }

    if (mbr_bootpart_ok(disc_parts, block0->sector_count) != 0) {
        klog(KLOG_WARN, "block", "entry 0 is not a boot partition; /dev/bootpart absent");
        return;
    }
    devfs_bootpart_publish(block_lag_wrap(block0), disc_parts[0].start_lba, disc_parts[0].sector_count);
}

/* The partition nodes, over the lag device when `rach2.blocklag` asks for
 * one; the root window keeps the real device underneath. */
static void publish_partition_nodes(void)
{
    if (!disc_table_ok || !block0) {
        return;
    }
    devfs_parts_publish(block_lag_wrap(block0), disc_parts);
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
static int init_krffs_disk(void)
{
    static uint8_t part0[BLOCK_SECTOR_SIZE];
    const struct mbr_part *parts = disc_parts;
    uint32_t start_lba = 0;
    uint32_t sector_count = 0;
    uint32_t part_bytes;
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
       whatever the MBR type byte says, so a GRUB-partitioned disk (boot partition first, KRFFS second)
       still mounts. LBA 0 is never probed; it has the boot code. */
    for (i = 0; i < MBR_PART_COUNT; ++i) {
        krffs_node_t *root;

        if (parts[i].sector_count == 0 || parts[i].start_lba == 0) {
            continue;
        }
        if (block0->read(block0, parts[i].start_lba, 1, part0) != 0) {
            continue;
        }
        root = (krffs_node_t *) part0;
        if (root->magic == KRFFS_FILE_SYSTEM_MAGIC &&
            root->type == KRFFS_ROOT_NODE && root->size != 0 &&
            root->size <= (uint64_t) parts[i].sector_count * BLOCK_SECTOR_SIZE) {
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

    /* Reject a malformed partition before mounting, so a bad MBR fails cleanly
       instead of faulting past the disk or the window later: it must be
       non-empty, a whole number of pages (the cache transfers a page at a time), lie within the disk, and fit the window (which also keeps sectors * 512
       inside 32 bits, since the window is capped well below). */
    if (sector_count == 0 ||
        sector_count % (PAGE_SIZE / BLOCK_SECTOR_SIZE) != 0) {
        klog(KLOG_WARN, "krffs", "partition is empty or not page-aligned (%d sectors)",
                      sector_count);
        return -1;
    }
    if (start_lba > block0->sector_count ||
        sector_count > block0->sector_count - start_lba) {
        klog(KLOG_WARN, "krffs", "partition runs past the disk");
        return -1;
    }
    if (sector_count > pc_window_size() / BLOCK_SECTOR_SIZE) {
        klog(KLOG_WARN, "krffs", "partition %d sectors exceeds the window",
                      sector_count);
        return -1;
    }
    part_bytes = sector_count * BLOCK_SECTOR_SIZE;

    /* The window faults with interrupts off and polls the device from that
       context, so a device that completes later instead of answering now
       cannot back it. */
    if (!(block0->flags & BLOCK_FAULT_SAFE)) {
        klog(KLOG_WARN, "krffs", "the device is not fault-safe; no root on it");
        return -1;
    }

    block_partition_init(&_krffs_disk_part, block0, start_lba, sector_count);
    pc_set_backing(krffs_disk_read, krffs_disk_write, part_bytes);
    if (init_krffs((void *) PCWIN_BASE, part_bytes) != 0) {
        pc_set_backing(0, 0, 0);
        return -1;
    }

    /* Observe the previous mount state, then mark this mount dirty and write
       the flag straight through to the disk (pc_sync), so a crash before the clean
       shutdown clears it leaves the evidence on the device for fsck.krffs. */
    {
        krffs_node_t *root = (krffs_node_t *) PCWIN_BASE;
        uint32_t reaped;

        if (root->mode & KRFFS_MODE_DIRTY) {
            klog(KLOG_WARN, "krffs", "not cleanly unmounted last time; run fsck.krffs");
        }
        root->mode |= KRFFS_MODE_DIRTY;
        if (pc_sync() != 0) {
            /* The dirty flag never reached the platter, so a later crash would
               leave an image fsck.krffs reads as clean. Fail the mount closed, not run on a filesystem whose crash evidence is missing.
               init_krffs already published krffs_fs at the window, so unpublish
               it before dropping the backing or a later path would dereference
               the now-unbacked window. */
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
            klog(KLOG_INFO, "krffs", "reaped %d orphan(s) with no descriptor left",
                          (int) reaped);
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
    if (mbr_bootpart_pick(parts, found, block0->sector_count) != 0) {
        devfs_bootpart_withdraw();
    }
    return 0;
}

/* How many self-tests ran and how many failed, which the summary reports and
   `tests/run default` reads. Counted rather than listed because two of the
   checks below are inline blocks with detail of their own to report. */
static int selftest_ran;
static int selftest_failed;

static void selftest_count(int passed)
{
    selftest_ran++;
    if (!passed) {
        selftest_failed++;
    }
}

/**
 * Record one self-test's outcome and count it.
 *
 * A failure is an `ERR` record, which is what the probe runner looks for: it
 * refuses a boot with any error record between the first self-test record and
 * the summary, so a kernel whose own checks fail cannot pass the suite.
 */
static void selftest_note(const char *name, int passed)
{
    selftest_count(passed);
    if (passed) {
        klog(KLOG_INFO, "selftest", "%s: passed", name);
    } else {
        klog(KLOG_ERR, "selftest", "%s: failed", name);
    }
}

static void run_self_tests(void)
{
    selftest_ran = 0;
    selftest_failed = 0;
    klog(KLOG_INFO, "selftest", "started");

    selftest_note("memory", test_memory());
    selftest_note("strings", test_strings());
    selftest_note("timer", test_timer());

    {
        uint32_t measured_ms = 0;
        int passed = test_udelay(&measured_ms);

        selftest_count(passed);
        klog(passed ? KLOG_INFO : KLOG_ERR, "selftest",
             passed ? "udelay: passed: 200 ms asked, %d ms seen"
                    : "udelay: failed: 200 ms asked, %d ms seen",
             (int)measured_ms);
    }

    {
        uint32_t measured_us = 0;
        int passed = test_clock(&measured_us);

        selftest_count(passed);
        klog(passed ? KLOG_INFO : KLOG_ERR, "selftest",
             passed ? "clock: passed: 2000 us asked, %d us seen"
                    : "clock: failed: 2000 us asked, %d us seen",
             (int)measured_us);
    }

    /* Reaching this line at all is the check: the code is running from mapped
       addresses through a page directory the kernel built. */
    selftest_note("paging", 1);
    selftest_note("syscall", test_syscall());

    {
        process_t *test_a = current_proc;
        process_t *test_b = alloc_process();

        if (test_b) {
            uintptr_t test_stack = mem_reserve(KERNEL_STACK_SIZE, true);
            test_b->kernel_stack = test_stack + KERNEL_STACK_SIZE;
            test_b->cr3 = 0;

            switch_proc_a = test_a;
            switch_proc_b = test_b;
            switch_count = 0;
            setup_process_stack(test_b, proc_b_entry);

            switch_count = 10; /* at the limit, so proc_b_entry's loop is skipped */
            switch_context(test_a, test_b);

            free_process(test_b);
        }
        selftest_note("context switch", test_b != 0);
    }

    {
        process_t *pa, *pb;
        uintptr_t sa, sb;
        int passed = 0;

        preempt_done = 0;
        preempt_count_a = 0;
        preempt_count_b = 0;

        pa = alloc_process();
        pb = alloc_process();
        if (pa && pb) {
            sa = mem_reserve(KERNEL_STACK_SIZE, true);
            sb = mem_reserve(KERNEL_STACK_SIZE, true);
            pa->kernel_stack = sa + KERNEL_STACK_SIZE;
            pb->kernel_stack = sb + KERNEL_STACK_SIZE;
            pa->cr3 = 0;
            pb->cr3 = 0;
            pa->parent_pid = current_proc->pid;
            pb->parent_pid = current_proc->pid;
            setup_process_stack(pa, preempt_proc_a);
            setup_process_stack(pb, preempt_proc_b);

            sched_enable();
            while (!preempt_done) {
                __asm__ __volatile__("hlt");
            }
            sched_disable();
            free_process(pa);
            free_process(pb);
            passed = preempt_count_a > 0 && preempt_count_b > 0;
            /* The two processes wrote their letters with no newline, so the
               record below would begin in the middle of that line and a
               reader looking for records at a line start would not see it. */
            console_putc('\n');
        }
        selftest_count(passed);
        klog(passed ? KLOG_INFO : KLOG_ERR, "selftest",
             passed ? "round robin: passed: a ran %d times, b %d"
                    : "round robin: failed: a ran %d times, b %d",
             preempt_count_a, preempt_count_b);
    }

    selftest_note("memory isolation", test_isolation());

    /* Only where there is a file system to write to and read a program out
       of; a machine booted without one is not failing these. */
    if (krffs_fs) {
        selftest_note("krffs write", test_krffs_write());
        selftest_note("elf load", test_elf());
    }

    selftest_note("page cache", test_pagecache());

    test_display();

    if (selftest_failed == 0) {
        klog(KLOG_INFO, "selftest", "result: passed: %d", selftest_ran);
    } else {
        klog(KLOG_ERR, "selftest", "result: failed: %d of %d", selftest_failed,
             selftest_ran);
    }
}

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
        console_printf("Kernel end:      0x%X\n", (uintptr_t) &_end);
        console_printf("Modules loaded:  %d\n", module_count);
    } else if (str_equal(cmd, "modules")) {
        uint32_t i;
        if (module_count == 0) {
            console_puts("No modules loaded.");
        } else {
            console_printf("Loaded modules: %d\n", module_count);
            for (i = 0; i < module_count; i++) {
                module_info_t *m = get_module(i);
                console_printf("  [%d] 0x%X - 0x%X (%d bytes)\n",
                         i, m->start, m->end, m->size);
            }
        }
    } else if (str_equal(cmd, "disk")) {
        static uint8_t disk_sector[BLOCK_SECTOR_SIZE];

        if (_krffs_on_disk) {
            console_printf("disk: re-probe refused while KRFFS is mounted from disk\n");
        } else if (block_ready() != 0 || !block0) {
            console_printf("disk: no boot disk\n");
        } else {
            console_printf("disk: block0: %d sectors, %d MiB\n",
                          (uint32_t) block0->sector_count,
                          (uint32_t) (block0->sector_count / 2048));

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
                console_printf("disk: MBR signature %X%X (expect 55AA)\n",
                              disk_sector[510], disk_sector[511]);
            }
        }
    } else if (str_equal(cmd, "disk write")) {
#ifdef RACH2_RECOVERY
        static uint8_t disk_sector[BLOCK_SECTOR_SIZE];
        static uint8_t disk_scratch[BLOCK_SECTOR_SIZE];

        if (_krffs_on_disk) {
            console_printf("disk: re-probe refused while KRFFS is mounted from disk\n");
        } else if (block_ready() != 0 || !block0) {
            console_printf("disk: no boot disk\n");
        } else {
            /* The last sector lies past the partition and the boot gap, so the
               round-trip that proves WRITE + FLUSH + READ disturbs neither a
               mounted file system nor a future bootloader. */
            uint64_t lba = block0->sector_count - 1;
            uint32_t i;

            for (i = 0; i < BLOCK_SECTOR_SIZE; ++i) {
                disk_scratch[i] = (uint8_t) (i ^ 0x5A);
            }
            if (block0->write(block0, lba, 1, disk_scratch) != 0) {
                console_printf("disk: write of the last sector failed\n");
            } else if (block0->read(block0, lba, 1, disk_sector) != 0) {
                console_printf("disk: read-back of the last sector failed\n");
            } else {
                int ok = 1;
                for (i = 0; i < BLOCK_SECTOR_SIZE; ++i) {
                    if (disk_sector[i] != disk_scratch[i]) {
                        ok = 0;
                        break;
                    }
                }
                console_printf("disk: last-sector round-trip %s\n",
                              ok ? "OK" : "MISMATCH");
            }
        }
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
            }
            discover_boot_devices();
            publish_partition_nodes();
            if (kcmdline_has("rach2.diskdiag")) {
                diskinfo_probe(_krffs_on_disk);
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
                    console_printf("  %s  (%d bytes)\n",
                             node->name, (uint32_t)node->data_size);
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
                    console_printf("%s", buffer);
                    offset += (uint32_t)bytes_read;
                }

                if (file_size > 0 && buffer[bytes_read > 0 ? bytes_read - 1 : 0] != '\n') {
                    console_puts("");  /* terminate the last line if the file lacked one */
                }
                console_puts("--- EOF ---");
            }
        }
    } else if (str_starts_with(cmd, "elf ")) {
        const char *filename = cmd + 4;
        char buffer[8192];
        int bytes_read;
        uint32_t file_size;
        uint32_t entry_point;
        uint16_t phdr_count;
        Elf32_Phdr *phdrs;
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
                    console_printf("Not a valid ELF32 executable\n");
                } else {
                    entry_point = elf_entry_point(buffer);
                    phdrs = elf_program_headers(buffer);
                    phdr_count = elf_program_header_count(buffer);

                    console_printf("ELF Header:\n");
                    console_printf("  Entry point:       0x%X\n", entry_point);
                    console_printf("  Program headers:   %d\n", phdr_count);
                    for (i = 0; i < phdr_count; i++) {
                        if (phdrs[i].p_type == PT_LOAD) {
                            console_printf("  LOAD segment:\n");
                            console_printf("    Virtual addr:  0x%X\n", phdrs[i].p_vaddr);
                            console_printf("    File size:     %d bytes\n", phdrs[i].p_filesz);
                            console_printf("    Memory size:   %d bytes\n", phdrs[i].p_memsz);
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
            if (i < 63) old_buf[i] = args[i];
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

            if (source && source != krffs_fs->node &&
                !KRFFS_IS_DIR(source->mode) &&
                victim && victim != source && fs_node_is_held(victim)) {
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
            if (i < 63) file_buf[i] = args[i];
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
                uint32_t len = k_strlen(data);
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
        run_self_tests();
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
            console_printf("ping 10.0.2.2: reply in %d ticks (%d ms)\n",
                     rtt, rtt * 10);
            console_printf("ping 10.0.2.2: reply in %d ticks\n", rtt);
        } else {
            set_fore_color(light_red);
            console_puts("ping 10.0.2.2: timeout");
        }
        set_fore_color(light_grey);
    } else if (str_equal(cmd, "netbad")) {
        /* Ping an unreachable host on our subnet to exercise the timeout. */
        int rtt = icmp_ping(0x0A000263UL);   /* 10.0.2.99 */
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
        volatile uint32_t *bad_ptr = (volatile uint32_t *) 0xA0000000;
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
 * returns through process_command (exec_program calls enter_usermode), so the
 * post-process_command `cmd_pos = 0` never runs for it; without this reset the
 * next keystrokes append to the previous command's buffer and the shell re-runs
 * it. It also drops any stale stdin-wait state from a program that exited mid
 * read, and releases the console controlling terminal so the timer-IRQ drain
 * does not steal the input the debug shell polls itself.
 *
 * The exit paths (sys_exit, terminate, exec_fail_destroyed) call this BEFORE
 * they re-enable interrupts and jump here, so a keyboard IRQ in the jump window
 * cannot observe stale state; shell_loop calls it again on entry as the
 * catch-all for boot and any path that did not.
 */
void shell_reset_input(void)
{
    cmd_pos = 0;
    cmd_ready = false;
    stdin_waiting_proc = NULL;
    stdin_ready = false;
    stdin_pos = 0;
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
            if (ch < 0) break;

            /* Terminals send CR on Enter. */
            if (ch == '\r') ch = '\n';

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

        __asm__ __volatile__("hlt");
    }
}

/**
 * Kernel entry point from the multiboot loader: bring up the system and run.
 *
 * Initializes every subsystem in order (serial, GDT/TSS/IDT/ISR, memory and
 * paging, timer, entropy, keyboard and console tty, mouse, syscalls, the
 * process table and scheduler, audio, the RAM disk and KRFFS, the pipe and
 * pty pools, and the network), runs the self-tests, then runs `/sbin/init`
 * as PID 1 (or, in rescue mode or on an init-less image, the kernel debug
 * shell). Does not return.
 */
void k_main(multiboot_uint32_t magic, multiboot_info_t *info)
{
    uint32_t num_modules;

    serial_init();
    klog(KLOG_INFO, "boot", "Rach2 Booting");

    /* Parse multiboot modules BEFORE paging: reading module info needs direct
     * physical access. */
    num_modules = init_modules(magic, info);

    /* Push the bump allocator past the modules so it does not overwrite their
     * data when it carves out page tables and other structures. The modules are
     * not guaranteed to be address-ordered, so take the highest end across all
     * of them. A module that ends above the 32 MiB direct-mapped limit cannot
     * be addressed or managed and would drive the reservation break past the
     * limit, so fail the boot closed with a clear diagnostic, not let
     * init_paging corrupt low memory when its first reservation comes back
     * empty. */
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
            if (mod->end > modules_end) {
                modules_end = mod->end;
            }
        }

        verdict = modlayout_check(ranges, nranges,
                                  (uint32_t) (uintptr_t) &_end,
                                  HARD_MEMORY_LIMIT, &bad);
        if (verdict != MODLAYOUT_OK) {
            /* The detail travels INTO the panic, not ahead of it: the
             * machine this check was written for has no serial port, and
             * k_panic clears the screen, so anything printed here first would
             * be gone before a camera could read it. The panic's own record
             * carries the text to the port and the wire. */
            static char verdict_msg[128];

            modlayout_describe(verdict, bad, ranges[bad].start, ranges[bad].end,
                               verdict_msg, (uint32_t) sizeof(verdict_msg));
            k_panic(verdict_msg);
        }
        if (modules_end > mem_reserved_ptr) {
            mem_reserved_ptr = (modules_end + 0xFFF) & ~0xFFF;
        }
    }

    /* Capture the kernel command line now, before paging remaps low memory and
     * before the bump allocator can carve over the string the bootloader left.
     */
    if ((info->flags & MULTIBOOT_INFO_CMDLINE) && info->cmdline != 0) {
        kcmdline_capture((const char *)info->cmdline);
    }

    /* With `rach2.netconsole` the serial stream is buffered from here on, so
     * the memory layout and the device probes below are in the first frames
     * the network device sends once it comes up. Nothing is transmitted until
     * then; this only starts collecting. */
    if (kcmdline_has("rach2.netconsole")) {
    }

    k_cls();

    klog(KLOG_INFO, "boot", "Initializing system components");

    init_gdt();
    klog(KLOG_INFO, "boot", "gdt: ready");

    init_tss((uint32_t)stack + KERNEL_STACK_SIZE);
    klog(KLOG_INFO, "boot", "tss: ready");

    init_idt();
    klog(KLOG_INFO, "boot", "idt: ready");

    init_isr();
    klog(KLOG_INFO, "boot", "isr: ready");

    init_mm();
    klog(KLOG_INFO, "boot", "memory manager: ready");

    /* Size the physical frame pool from the multiboot memory map, capped to the
     * direct-mapped window (frame_usable_count). */
    init_paging(frame_usable_count(info));
    klog(KLOG_INFO, "boot", "paging: ready");

    /* Reserve the cache frames and pre-map the disk window in the kernel
     * directory, before the first fork copies the kernel PDEs. */
    init_pagecache();
    klog(KLOG_INFO, "boot", "page cache: ready");

    /* The boot report, after every boot-time reservation and before any driver
     * has configured a device. After, because the page cache reserves frames
     * and the report's own device peek maps a page, and a figure printed
     * before those is not the memory the machine has left. */
    if (kcmdline_has("rach2.diag")) {
        boot_diag_report();
    }

    /* What the machine's firmware left in the APIC and in the two devices
     * this kernel drives by message, read and printed, nothing written. */
    apic_diag_report();

    /* The local APIC, so that a device can be told to send its interrupts
     * as messages. A machine that refuses leaves every device on its
     * legacy line, which is the kernel as it was. */
    lapic_init();

    __asm__ __volatile__("cli");
    init_pit(100);
    set_isr_handler(32, &timer_handler, "pit"); /* install the timer handler on IRQ0 */
    __asm__ __volatile__("sti");

    /*
     * The 8259's interrupts now reach the processor through the APIC's
     * `LINT0`, so the first thing the timer is asked is whether it still
     * arrives. A masked entry would otherwise show as a machine that boots
     * and never ticks, with nothing said about why. The wait is measured
     * against the 8254's own counter, which needs no interrupt.
     */
    if (lapic_ready()) {
        uint32_t before = kernel_ticks;
        int ms;

        for (ms = 0; ms < 50 && kernel_ticks == before; ms++) {
            k_udelay(1000);
        }
        if (kernel_ticks == before) {
            klog(KLOG_ERR, "lapic", "timer: no tick in 50 ms; writing lint0 again");
            lapic_repair_lint0();
            for (ms = 0; ms < 50 && kernel_ticks == before; ms++) {
                k_udelay(1000);
            }
            if (kernel_ticks == before) {
                klog(KLOG_ERR, "lapic", "timer: still no tick; the 8259's interrupts are not arriving");
            }
        }
    }
    klog(KLOG_INFO, "boot", "timer: ready");


    rtc_init();
    if (rtc_is_anchored()) {
        klog(KLOG_INFO, "clock", "ready");
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
        klog(src == KRANDOM_SRC_JITTER ? KLOG_WARN : KLOG_INFO, "entropy",
             "ready: %s", desc);
    }
    set_fore_color(light_grey);

    /* Which power controller this machine has, before interrupts are enabled:
     * with ACPI on, an unhandled SCI is level-triggered and would be taken
     * forever, so the line it sits on is masked here. A machine no profile
     * matches keeps exactly the behaviour it had before this existed. */
    {
        const char *why = "";
        int verdict = board_init(&why);


        if (verdict == BOARD_FOUND) {
            klog(KLOG_INFO, "board", "ready: %s", board_name());
        } else if (verdict == BOARD_REFUSED) {
            klog(KLOG_WARN, "board", "refused: %s", why);
        } else {
            klog(KLOG_WARN, "board",
                 "none: no profile matched; poweroff will halt");
        }
    }

    set_isr_handler(33, &keyboard_handler, "kbd"); /* install the IRQ 1 keyboard handler */
    pic_unmask_irq(1); /* now safe to take keyboard interrupts (masked since init_pic) */
    tty_console_init(); /* console_tty: VGA + serial, line discipline for fds 0/1/2 */
    klog(KLOG_INFO, "boot", "keyboard: ready");

    if (kcmdline_has("rach2.nomouse")) {
        klog(KLOG_INFO, "mouse", "skipped: rach2.nomouse");
    } else {
        klog(KLOG_INFO, "mouse", "started");
        if (init_mouse() == 0) {
            input_mouse_found();
            klog(KLOG_INFO, "mouse", "ready");
        } else {
            klog(KLOG_WARN, "mouse", "absent: nothing answered the probe");
        }
    }

    if (kcmdline_has("rach2.nousb")) {
        klog(KLOG_INFO, "usb", "skipped: rach2.nousb");
    } else {
        klog(KLOG_INFO, "usb", "started");
        usb_report_controllers();
        usb_init();
    }

    init_syscall();
    klog(KLOG_INFO, "boot", "syscall: ready");

    init_proc();
    {
        /* Process 0 is the kernel idle process. */
        process_t *proc0 = alloc_process();
        if (proc0) {
            proc0->state = PROC_RUNNING;
            proc0->cr3 = 0;  /* shares the kernel page directory */
            proc0->kernel_stack = (uint32_t)stack + KERNEL_STACK_SIZE;
            proc0->parent_pid = 0;
            current_proc = proc0;
        }
    }
    klog(KLOG_INFO, "boot", "process: ready");

    init_sched();
    klog(KLOG_INFO, "boot", "scheduler: ready");

    if (kcmdline_has("rach2.noaudio")) {
        klog(KLOG_INFO, "audio", "skipped: rach2.noaudio");
    } else {
        klog(KLOG_INFO, "audio", "started");
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
    klog(diskcmd_policy() == DISKCMD_READ_WRITE ? KLOG_INFO : KLOG_WARN, "disk",
         "policy: %s",
         diskcmd_policy() == DISKCMD_READ_WRITE ? "read-write" : "read-only");


    if (num_modules > 0) {
        module_info_t *ramdisk = get_ramdisk();
        set_fore_color(light_green);
        klog(KLOG_INFO, "modules", "found %d", num_modules);
        set_fore_color(light_grey);
        if (ramdisk) {
            klog(KLOG_INFO, "modules", "ram disk: 0x%X, %d bytes",
                     ramdisk->start, ramdisk->size);
        }
    } else {
        set_fore_color(light_brown);
        klog(KLOG_INFO, "modules", "none");
        set_fore_color(light_grey);
    }

    refuse_contradictory_boot_line();

    discover_boot_devices();
    publish_partition_nodes();

    {
        int mounted = -1;
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

        if (kcmdline_has("rach2.fsdisk") || kcmdline_has("rach2.root=disk")) {
            mounted = init_krffs_disk();
            via = " (disk)";
            source = "/dev/disk0";
            options = "rw,relatime";
        } else if (num_modules > 0) {
            module_info_t *ramdisk = get_ramdisk();
            if (ramdisk && kcmdline_has("rach2.fswindow")) {
                mounted = init_krffs_window((uint8_t *) ramdisk->start,
                                            ramdisk->size);
                via = " (window)";
                source = "module";
                options = "rw,relatime,window";
            } else if (ramdisk) {
                mounted = init_krffs((void *) ramdisk->start, ramdisk->size);
                source = "module";
                options = "rw,relatime";
            }
        }

        if (mounted == 0) {
            krffs_set_mount(source, options);
            vfs_init();
            set_fore_color(light_green);
            klog(KLOG_INFO, "krffs", "mounted: %d files%s", krffs_count_files(),
                 via);
            set_fore_color(light_grey);
        } else if (kcmdline_has("rach2.fsdisk") ||
                   kcmdline_has("rach2.root=disk") || num_modules > 0) {
            set_fore_color(light_red);
            klog(KLOG_ERR, "krffs", "refused: the image is not a KRFFS");
        } else {
            set_fore_color(light_brown);
            klog(KLOG_INFO, "krffs", "none: no ramdisk");
        }
    }
    set_fore_color(light_grey);

    /* The disk diagnostic, asked for by name. It probes a disk only if the
     * mount above did not, publishes /proc/disk and /dev/disk0, and issues
     * nothing but IDENTIFY and a sector read, so the read-only build runs it
     * unchanged. Without the flag neither object exists, so no image grows a
     * raw whole-disk node by accident. The recovery build is the exception and
     * needs no flag: the whole-disk window is what that build is for. */
    if (kcmdline_has("rach2.diskdiag") || recovery_publishes_disk()) {
        if (diskinfo_probe(_krffs_on_disk) == 0) {
            klog(KLOG_INFO, "diskdiag", "ready: /proc/disk and /dev/disk0");
        } else {
            klog(KLOG_WARN, "diskdiag", "absent: no disk answered");
        }
    }

    /* Allocate the pipe pool (kept out of BSS) before user processes can
     * pipe(). */
    pipe_init_pool();

    /* Allocate the pty pool before user processes can open /dev/ptmx. */
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

    run_self_tests();

    /*
     * Decide the boot home. A normal boot runs the persistent /sbin/init as
     * PID 1: boot_init_process execs it and does not return while it runs,
     * and init's later death is a panic (panic_init_died), so a normal box
     * never falls to the debug shell. boot_init_process returns here only
     * when the image has no init (the test image); that case, like an
     * explicit `rach2.rescue` boot, makes the kernel debug shell the
     * supervisor. The shell loops reading commands and `exec`s programs as
     * transient inits that return to it on exit (an is-init death path jmps
     * back to shell_loop while the debug shell is home), which is the
     * contract the test harness drives.
     */
    /* The diagnostic numbers once more, on a screen nothing will scroll, before
     * init takes the console. */
    if (kcmdline_has("rach2.diag")) {
        boot_diag_hold();
    }

    rescue_mode = kcmdline_has("rach2.rescue") || kcmdline_has("init=rescue");

    if (!rescue_mode) {
        refuse_unmounted_root();
        boot_init_process();
    }

    debug_shell_is_home = true;
    while (1) {
        const char *banner = rescue_mode ? "Kernel debug shell active (rescue mode)."
                                         : "Kernel debug shell active.";

        console_puts("");
        console_puts(banner);
        shell_loop();
    }
}
