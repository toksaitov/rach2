#include "vendor/multiboot.h"

#include "sys/kio.h"
#include "sys/serial.h"
#include "sys/syscall.h"
#include "sys/module.h"
#include "sys/tables/gdt.h"
#include "sys/tables/tss.h"
#include "sys/tables/idt.h"
#include "sys/isr.h"
#include "sys/mm/heap.h"
#include "sys/mm/paging.h"
#include "sys/mm/frame.h"
#include "sys/proc/process.h"
#include "sys/tty/tty.h"
#include "sys/tty/pty.h"
#include "sys/ipc/pipe.h"
#include "sys/proc/sched.h"
#include "sys/proc/elf.h"
#include "sys/proc/uimage.h"
#include "sys/lib/krandom.h"
#include "sys/lib/ksignal.h"
#include "sys/fs/krffs.h"
#include "sys/pit.h"
#include "sys/kbd.h"
#include "sys/mouse.h"
#include "sys/display/vga_text.h"
#include "sys/audio/audio.h"
#include "sys/audio/audio_debug.h"
#include "sys/audio/hda.h"
#include "sys/network/net.h"
#include "sys/network/ip/icmp.h"
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

/* PID of the init process, or 0 when none runs. When init exits, control
 * returns to the kernel debug shell. */
volatile uint32_t init_pid = 0;

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
static bool test_syscall(void);
static bool test_usermode(void);
static void test_switch(void);
static void test_preempt(void);
static bool test_isolation(void);
static bool test_krffs_write(void);
static bool test_elf(void);
static void exec_program(int argc, char *argv[]);

/* Top of the user stack; it grows down from here. */
#define USER_STACK_BASE 0xBFFFF000

/**
 * Service the 100 Hz timer IRQ and preempt the current process.
 *
 * Bumps the global tick, then runs the periodic kernel work that the timer
 * drives: waking expired nanosleep sleepers, the HDA audio tick (needed when
 * legacy IRQs are not delivered on MSI-only setups), the TCP timers, and the
 * interrupt-less console serial drain. Finally reschedules, but only when the
 * current process is not inside kernel syscall code: timer IRQs stay enabled
 * during syscalls so HDA streaming keeps running, yet preempting a process mid
 * syscall is unsafe. Kernel threads (no current_proc, or one not in_syscall)
 * stay preemptible.
 */
static void timer_handler(isr_params_t *params)
{
    (void) params;

    ++kernel_ticks;

    /* Runs regardless of in_syscall, so a sleeper parked inside its syscall
     * still becomes runnable on time. */
    proc_wake_sleepers(kernel_ticks);

    hda_tick();
    tcp_tick();
    tty_console_poll();

    if (!current_proc || !current_proc->in_syscall) {
        schedule();
    }
}

/* Keyboard handler key state: shift, ctrl, and the 0xE0 extended-scancode
 * prefix seen on the previous byte. */
static bool _kbd_shift = false;
static bool _kbd_ctrl = false;
static bool _kbd_e0 = false;

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
     * leader, not only init), release the controlling terminal before zombifying
     * so the timer-IRQ serial drain does not target this vanishing session during
     * teardown. shell_reset_input on the is_init path below releases again, which
     * is idempotent. */
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
            serial_printf("terminate: woke parent process %d\n", parent->pid);
        }
    }

    /* Same descriptor-table teardown sys_exit does, so a signal-killed process
     * leaks no sockets or open-file descriptions. */
    release_fd_table(&current_proc->fds);

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
        /* Same resume path as when init exits normally; see sys_exit. */
        serial_printf("terminate: init process %d killed by signal %d, "
                      "returning to kernel shell\n", pid, signo);
        k_puts("");
        k_puts("Init process exited. Returning to kernel debug shell.");

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
 * The target is the controlling terminal's foreground process group, the same
 * one the serial ^C reaches through the line discipline, not current_proc:
 * inside this keyboard IRQ the running process may be unrelated. With no
 * foreground group yet, falls back to the running user process so a lone
 * program is still interruptible.
 */
static void kill_foreground_process(void)
{
    k_puts("^C");
    if (console_tty.fg_pgid != 0) {
        tty_signal_group(console_tty.fg_pgid, SIGINT);
    } else if (current_proc && current_proc->cr3 != 0) {
        signal_post(current_proc, SIGINT);
    }
}

/**
 * Handle the PS/2 keyboard IRQ: decode a scancode and route the keystroke.
 *
 * Reads the scancode from port 0x60 and pushes the raw byte to the games
 * scancode ring before any early return. Tracks the 0xE0 extended prefix and
 * the shift/ctrl modifier keys, swallows key-release events, and intercepts
 * Ctrl+C to signal the foreground group. A live keystroke routes one of three
 * ways: to the tty line discipline when a user process waits on the console, to
 * the shell's command buffer when the debug shell is active, or to scrollback
 * navigation for the extended arrow/page keys. Runs in interrupt context with
 * an arbitrary process's page directory loaded, so it switches to the kernel
 * directory around any access to kernel data and restores CR3 on exit.
 */
static void keyboard_handler(isr_params_t *params)
{
    uint8_t scancode;
    char c;
    uint32_t saved_cr3;
    bool switched_cr3 = false;
    bool was_e0;

    (void) params;

    scancode = inb(0x60);
    was_e0 = _kbd_e0;
    _kbd_e0 = false;

    /* Push every scancode, including the ones the branches below consume, so a
     * game reading the raw ring sees modifiers and releases too. */
    kbd_push_scancode(scancode);

    if (scancode == 0xE0) {
        _kbd_e0 = true;
        return;
    }

    /* Left and right shift. */
    if (scancode == 0x2A || scancode == 0x36) {
        _kbd_shift = true;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        _kbd_shift = false;
        return;
    }

    /* Left and right Ctrl both report 0x1D press / 0x9D release. */
    if (scancode == 0x1D) {
        _kbd_ctrl = true;
        return;
    }
    if (scancode == 0x9D) {
        _kbd_ctrl = false;
        return;
    }

    /* High bit set marks a key release. */
    if (scancode & 0x80) {
        return;
    }

    /* 'C' is scancode 0x2E. */
    if (_kbd_ctrl && scancode == 0x2E) {
        kill_foreground_process();
        return;
    }

    /* The running process's page directory may not map the kernel data this
     * handler touches (EN_us, stdin_buffer). Set CR3 directly rather than
     * switch_page_directory(), whose _curr_dir check may be out of sync here. */
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(saved_cr3));
    {
        uint32_t kernel_cr3 = get_page_dir_phys(_kernel_dir);
        if (saved_cr3 != kernel_cr3) {
            __asm__ __volatile__("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
            switched_cr3 = true;
        }
    }

    if (was_e0 && kernel_shell_active && stdin_waiting_proc == NULL) {
        if (scancode == 0x49) {
            k_scrollback_page_up();
            goto restore_cr3;
        }
        if (scancode == 0x51) {
            k_scrollback_page_down();
            goto restore_cr3;
        }
        if (scancode == 0x47) {
            k_scrollback_home();
            goto restore_cr3;
        }
        if (scancode == 0x4F) {
            k_scrollback_end();
            goto restore_cr3;
        }
    }

    c = _kbd_shift ? EN_us_shift[scancode & 0x7F] : EN_us[scancode & 0x7F];

    if (wq_active(&console_tty.input_wq)) {
        /*
         * A user process waits on the console (a blocked reader or a poller):
         * cook the keystroke through the tty line discipline, which echoes it
         * and wakes every waiter, so pollers and a second reader are not missed
         * the way the single-reader stdin_waiting_proc flag would drop them. The
         * PS/2 keyboard sends '\b' for Backspace; map it to the discipline's
         * ERASE (DEL), and turn Ctrl+letter into its control byte so line
         * editing (^U/^W) and EOF (^D) reach the discipline like a real
         * terminal. Ctrl+C is intercepted earlier.
         */
        if (c != 0) {
            unsigned char tc = (unsigned char)c;
            if (_kbd_ctrl && c >= 'a' && c <= 'z') {
                tc = (unsigned char)(c - 'a' + 1);
            } else if (_kbd_ctrl && c >= 'A' && c <= 'Z') {
                tc = (unsigned char)(c - 'A' + 1);
            } else if (c == '\b') {
                tc = 0x7F;
            }
            tty_input_char(&console_tty, tc);
        }
    } else if (kernel_shell_active) {
        if (c == '\n') {
            if (k_scrollback_is_active()) {
                k_scrollback_end();
            }
            cmd_buffer[cmd_pos] = '\0';
            cmd_ready = true;
            k_putc('\n');
        } else if (c == '\b') {
            if (k_scrollback_is_active()) {
                k_scrollback_end();
            }
            if (cmd_pos > 0) {
                --cmd_pos;
                k_putc('\b');
                k_putc(' ');
                k_putc('\b');
            }
        } else if (c != 0 && cmd_pos < CMD_BUFFER_SIZE - 1) {
            if (k_scrollback_is_active()) {
                k_scrollback_end();
            }
            cmd_buffer[cmd_pos++] = c;
            k_putc(c);
        }
    }

restore_cr3:
    if (switched_cr3) {
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(saved_cr3) : "memory");
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
    k_printf("  Display colors:     ");
    set_fore_color(red);
    k_printf("Red ");
    set_fore_color(green);
    k_printf("Green ");
    set_fore_color(cyan);
    k_printf("Cyan ");
    set_fore_color(light_magenta);
    k_printf("Magenta ");
    set_fore_color(light_brown);
    k_printf("Yellow ");
    set_fore_color(white);
    k_printf("White");
    set_fore_color(light_grey);
    k_puts("");
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

    serial_printf("  Usermode test: func=0x%X msg=0x%X\n", func_addr, msg_addr);

    user_stack_addr = mem_reserve(USER_STACK_SIZE, true);
    serial_printf("  Usermode test: stack=0x%X\n", user_stack_addr);

    /* The function code may span two pages. */
    for (page = func_addr & ~0xFFF; page <= ((func_addr + 256) & ~0xFFF); page += PAGE_SIZE) {
        paging_set_user(page, true);
    }

    paging_set_user(msg_addr, true);

    for (page = user_stack_addr; page < user_stack_addr + USER_STACK_SIZE; page += PAGE_SIZE) {
        paging_set_user(page, true);
    }

    serial_printf("  Usermode test: pages marked user-accessible\n");

    for (i = 0; i < 3; i++) {
        __asm__ __volatile__("hlt");
    }

    serial_printf("  Usermode test: entering ring 3...\n");

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
        k_putc('B');
        serial_printf("B");
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

    k_puts("Testing context switch between two processes...");
    k_puts("Expected output: ABABABABAB done");
    k_printf("Result:          ");

    switch_proc_a = current_proc;

    switch_proc_b = alloc_process();
    if (!switch_proc_b) {
        k_puts("Failed to allocate process B");
        return;
    }

    stack_b = mem_reserve(KERNEL_STACK_SIZE, true);
    switch_proc_b->kernel_stack = stack_b + KERNEL_STACK_SIZE;
    switch_proc_b->cr3 = 0;  /* shares the kernel page directory */
    switch_proc_b->parent_pid = switch_proc_a->pid;

    setup_process_stack(switch_proc_b, proc_b_entry);

    serial_printf("Process B created: pid=%d stack=0x%X\n",
                  switch_proc_b->pid, switch_proc_b->kernel_stack);

    switch_count = 0;

    while (switch_count < 10) {
        k_putc('A');
        serial_printf("A");
        ++switch_count;
        switch_context(switch_proc_a, switch_proc_b);
    }

    k_puts("");

    free_process(switch_proc_b);
    switch_proc_b = NULL;

    set_fore_color(light_green);
    k_puts("Context switch test passed!");
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
        k_putc('A');
        serial_printf("A");
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
        k_putc('B');
        serial_printf("B");
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

    k_puts("Testing preemptive round-robin scheduling...");
    k_puts("Two processes will print A and B, preempted by timer.");
    k_puts("Expected: interleaved As and Bs (e.g., AABBABAB...)");
    k_printf("Result:   ");
    serial_printf("Result:   ");

    preempt_done = 0;
    preempt_count_a = 0;
    preempt_count_b = 0;

    proc_a = alloc_process();
    if (!proc_a) {
        k_puts("Failed to allocate process A");
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
        k_puts("Failed to allocate process B");
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

    k_puts("");
    serial_printf("\n");
    k_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);
    serial_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);

    free_process(proc_a);
    free_process(proc_b);

    if (preempt_count_a > 0 && preempt_count_b > 0) {
        set_fore_color(light_green);
        k_puts("Preemptive scheduling test passed!");
        serial_printf("Preemptive scheduling test passed!\n");
    } else {
        set_fore_color(light_red);
        k_puts("Preemptive scheduling test FAILED - one process didn't run!");
        serial_printf("Preemptive scheduling test FAILED!\n");
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

    /* Same virtual address in each directory must map distinct physical pages. */
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
        serial_printf("KRFFS write test: create failed\n");
        return false;
    }

    bytes_written = krffs_write(test_filename, test_data, k_strlen(test_data), 0);
    if (bytes_written != (int)k_strlen(test_data)) {
        serial_printf("KRFFS write test: write failed (%d)\n", bytes_written);
        krffs_unlink(test_filename);
        return false;
    }

    bytes_read = krffs_read(test_filename, read_buffer, 63, 0);
    read_buffer[bytes_read > 0 ? bytes_read : 0] = '\0';
    if (bytes_read != (int)k_strlen(test_data) || !str_equal(read_buffer, test_data)) {
        serial_printf("KRFFS write test: read verify failed\n");
        krffs_unlink(test_filename);
        return false;
    }

    if (krffs_rename(test_filename, renamed_filename) != 0) {
        serial_printf("KRFFS write test: rename failed\n");
        krffs_unlink(test_filename);
        return false;
    }

    bytes_read = krffs_read(renamed_filename, read_buffer, 63, 0);
    read_buffer[bytes_read > 0 ? bytes_read : 0] = '\0';
    if (!str_equal(read_buffer, test_data)) {
        serial_printf("KRFFS write test: rename verify failed\n");
        krffs_unlink(renamed_filename);
        return false;
    }

    if (krffs_unlink(renamed_filename) != 0) {
        serial_printf("KRFFS write test: unlink failed\n");
        return false;
    }

    if (krffs_open(renamed_filename) == 0) {
        serial_printf("KRFFS write test: file still exists after unlink\n");
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

/* elf_read_fn over a KRFFS file; ctx is the path. Used by the segment loader. */
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
    static char *default_envp[] = {"PATH=/bin:/usr/bin:/", "HOME=/", "TERM=rach2", 0};

    if (argc < 1 || !argv || !argv[0]) {
        k_puts("exec: No program specified");
        return;
    }

    path = argv[0];

    if (!krffs_fs) {
        k_puts("exec: No file system mounted");
        return;
    }

    if (krffs_open(path) != 0) {
        k_printf("exec: File not found: %s\n", path);
        return;
    }

    user_dir = clone_page_directory();
    if (!user_dir) {
        k_puts("exec: Failed to clone page directory");
        return;
    }

    /* Stream the segments straight from KRFFS (no whole-file buffer, so the
     * binary size is unbounded); img records entry and auxv metadata. */
    if (elf_load_image(exec_krffs_read, (void *)path, user_dir, &img) != 0) {
        k_puts("exec: Failed to load ELF segments");
        return;
    }
    entry_point = img.entry;

    user_stack_vaddr = USER_STACK_BASE;
    user_stack_phys = alloc_user_page(user_dir, user_stack_vaddr);
    if (user_stack_phys == 0) {
        k_puts("exec: Failed to allocate user stack");
        return;
    }

    k_memset((void *)user_stack_phys, 0, PAGE_SIZE);

    /* build_user_stack copies the strings in and lays out the argv/envp pointer
     * arrays and the AT_NULL-terminated auxv, 16-byte-aligning the entry ESP.
     * The layout is verified host-side (tests/host/uimage_hosttest.c). */
    krandom_bytes(random16, 16);
    user_esp = build_user_stack((void *)user_stack_phys, user_stack_vaddr, PAGE_SIZE,
                                argc, argv, default_envp, &img, random16);
    if (user_esp == 0) {
        k_puts("exec: argv/env too large for the initial stack");
        return;
    }

    user_proc = alloc_process();
    if (!user_proc) {
        k_puts("exec: Failed to allocate process");
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

    if (init_pid == 0) {
        init_pid = user_proc->pid;
    }

    /*
     * Make this program the console's controlling-terminal foreground group. A
     * job-control shell (busybox ash) checks tcgetpgrp() against its own group
     * at startup and SIGTTIN-stops itself if it is not the foreground, so a
     * shell launched here must already own the terminal; the kernel debug shell
     * hands the terminal over for the program's lifetime. tty_set_session is one
     * interrupts-off update so the timer-IRQ serial drain never sees sid set
     * while fg_pgid is still the stale 0, which would route a serial ^C/^Z to
     * group 0 and drop it.
     */
    tty_set_session(&console_tty, (int)user_proc->sid, (int)user_proc->pgid);

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

    k_puts("exec: ERROR - returned from user mode!");
}

/**
 * Boot the first user process (init), then return to the kernel debug shell.
 *
 * Tries a real init in order: `/sbin/init`, then `/bin/sh` (the os-real image,
 * where the shell is busybox), then `ish` (the os-simple image's shell, at the
 * root). Execs the first the RAM disk holds, through exec_program. Control comes
 * back here only when the RAM disk holds none of them, or when init later exits.
 */
static void boot_init_process(void)
{
    static const char *const candidates[] = { "/sbin/init", "/bin/sh", "ish" };
    char *init_argv[2];
    unsigned int i;

    if (!krffs_fs) {
        k_puts("boot: no RAM disk; falling back to kernel debug shell");
        return;
    }

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (krffs_open(candidates[i]) != 0) {
            continue;
        }

        k_cls();
        k_printf("Booting into %s...\n", candidates[i]);
        k_puts("Type 'exit' to return to the kernel debug shell.");
        k_puts("");

        init_argv[0] = (char *) candidates[i];
        init_argv[1] = NULL;
        exec_program(1, init_argv);

        k_printf("boot: failed to start %s\n", candidates[i]);
        return;
    }

    k_puts("boot: no init (/sbin/init, /bin/sh, or ish) in RAM disk");
    k_puts("boot: falling back to kernel debug shell");
}

static void run_self_tests(void)
{
    k_puts("======= System Self-Test =======");
    serial_printf("\n======= System Self-Test =======\n");
    k_puts("");

    k_printf("  Memory allocation:  ");
    serial_printf("  Memory allocation:  ");
    if (test_memory()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
        serial_printf("[PASS]\n");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
        serial_printf("[FAIL]\n");
    }
    set_fore_color(light_grey);

    k_printf("  String functions:   ");
    serial_printf("  String functions:   ");
    if (test_strings()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
        serial_printf("[PASS]\n");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
        serial_printf("[FAIL]\n");
    }
    set_fore_color(light_grey);

    k_printf("  Timer interrupts:   ");
    serial_printf("  Timer interrupts:   ");
    if (test_timer()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
        serial_printf("[PASS]\n");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
        serial_printf("[FAIL]\n");
    }
    set_fore_color(light_grey);

    k_printf("  Paging enabled:     ");
    serial_printf("  Paging enabled:     ");
    set_fore_color(light_green);
    k_puts("[PASS]");
    serial_printf("[PASS]\n");
    set_fore_color(light_grey);

    k_printf("  Syscall (int 0x80): ");
    serial_printf("  Syscall (int 0x80): ");
    if (test_syscall()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
        serial_printf("[PASS]\n");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
        serial_printf("[FAIL]\n");
    }
    set_fore_color(light_grey);

    k_printf("  Context switch:     ");
    serial_printf("  Context switch:     ");
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

            switch_count = 10;  /* start at the limit so proc_b_entry's loop is skipped */
            switch_context(test_a, test_b);

            free_process(test_b);
            set_fore_color(light_green);
            k_puts("[PASS]");
            serial_printf("[PASS]\n");
        } else {
            set_fore_color(light_red);
            k_puts("[FAIL]");
            serial_printf("[FAIL]\n");
        }
    }
    set_fore_color(light_grey);

    k_printf("  Round-robin sched:  ");
    serial_printf("  Round-robin sched:  ");
    {
        process_t *pa, *pb;
        uintptr_t sa, sb;

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

            if (preempt_count_a > 0 && preempt_count_b > 0) {
                set_fore_color(light_green);
                k_printf(" [PASS] (A=%d B=%d)\n", preempt_count_a, preempt_count_b);
                serial_printf(" [PASS] (A=%d B=%d)\n",
                              preempt_count_a, preempt_count_b);
            } else {
                set_fore_color(light_red);
                k_printf(" [FAIL] (A=%d B=%d)\n", preempt_count_a, preempt_count_b);
                serial_printf(" [FAIL] (A=%d B=%d)\n",
                              preempt_count_a, preempt_count_b);
            }
        } else {
            set_fore_color(light_red);
            k_puts("[FAIL]");
        }
    }
    set_fore_color(light_grey);

    k_printf("  Memory isolation:   ");
    serial_printf("  Memory isolation:   ");
    if (test_isolation()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
        serial_printf("[PASS]\n");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
        serial_printf("[FAIL]\n");
    }
    set_fore_color(light_grey);

    if (krffs_fs) {
        k_printf("  KRFFS write ops:    ");
        serial_printf("  KRFFS write ops:    ");
        if (test_krffs_write()) {
            set_fore_color(light_green);
            k_puts("[PASS]");
            serial_printf("[PASS]\n");
        } else {
            set_fore_color(light_red);
            k_puts("[FAIL]");
            serial_printf("[FAIL]\n");
        }
        set_fore_color(light_grey);

        k_printf("  ELF parse + load:   ");
        serial_printf("  ELF parse + load:   ");
        if (test_elf()) {
            set_fore_color(light_green);
            k_puts("[PASS]");
            serial_printf("[PASS]\n");
        } else {
            set_fore_color(light_red);
            k_puts("[FAIL]");
            serial_printf("[FAIL]\n");
        }
        set_fore_color(light_grey);
    }

    test_display();

    k_puts("");
    k_puts("================================");
    serial_printf("\n================================\n");
}

static void process_command(const char *cmd)
{
    if (str_equal(cmd, "help")) {
        k_puts("Available commands:");
        serial_puts("Available commands:\n");
        k_puts("  help                - Show this help message");
        serial_puts("  help                - Show this help message\n");
        k_puts("  info                - Display system information");
        serial_puts("  info                - Display system information\n");
        k_puts("  modules             - List loaded multiboot modules");
        k_puts("  ls                  - List files on RAM disk");
        k_puts("  cat <file>          - Display file contents");
        k_puts("  elf <file>          - Display ELF header info");
        k_puts("  exec <file>         - Execute ELF program");
        k_puts("  touch <file>        - Create an empty file");
        k_puts("  rm <file>           - Delete a file");
        k_puts("  mv <old> <new>      - Rename a file");
        k_puts("  write <file> <data> - Write data to file");
        k_puts("  test                - Run self-tests again");
        k_puts("  clear               - Clear the screen");
        k_puts("  switch              - Test context switching");
        k_puts("  preempt             - Test preemptive scheduling");
        k_puts("  isolate             - Test memory isolation");
        k_puts("  usermode            - Test ring 3 execution");
        k_puts("  audio ...           - Audio tests, routing, and HDA diagnostics");
        k_puts("  netping             - Ping the gateway (10.0.2.2)");
        k_puts("  netbad              - Ping an unreachable host (timeout demo)");
        k_puts("  fault               - Trigger page fault (panic)");
        k_puts("  halt                - Halt the CPU");
        serial_puts("  ...and more (see VGA display)\n");
    } else if (str_equal(cmd, "info")) {
        k_printf("Memory pointer:  0x%X\n", mem_reserved_ptr);
        k_printf("Timer ticks:     %d\n", kernel_ticks);
        k_printf("Kernel end:      0x%X\n", (uintptr_t) &k_phys_mem_end);
        k_printf("Modules loaded:  %d\n", module_count);
    } else if (str_equal(cmd, "modules")) {
        uint32_t i;
        if (module_count == 0) {
            k_puts("No modules loaded.");
        } else {
            k_printf("Loaded modules: %d\n", module_count);
            for (i = 0; i < module_count; i++) {
                module_info_t *m = get_module(i);
                k_printf("  [%d] 0x%X - 0x%X (%d bytes)\n",
                         i, m->start, m->end, m->size);
            }
        }
    } else if (str_equal(cmd, "ls")) {
        if (!krffs_fs) {
            k_puts("No file system mounted.");
        } else {
            krffs_node_t *node;
            uint32_t count = 0;

            k_puts("Files on RAM disk:");
            node = krffs_fs->node;
            do {
                if (node->type == KRFFS_RESERVED_NODE) {
                    k_printf("  %s  (%d bytes)\n",
                             node->name, (uint32_t)node->data_size);
                    count++;
                }
            } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

            if (count == 0) {
                k_puts("  (no files)");
            }
        }
    } else if (str_starts_with(cmd, "cat ")) {
        const char *filename = cmd + 4;
        char buffer[512];
        int bytes_read;
        uint32_t offset = 0;

        if (!krffs_fs) {
            k_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            k_puts("Usage: cat <filename>");
        } else {
            if (krffs_open(filename) != 0) {
                k_printf("File not found: %s\n", filename);
            } else {
                uint32_t file_size = krffs_get_file_size(filename);
                k_printf("--- %s (%d bytes) ---\n", filename, file_size);

                while ((bytes_read = krffs_read(filename, buffer, 511, offset)) > 0) {
                    buffer[bytes_read] = '\0';
                    k_printf("%s", buffer);
                    offset += (uint32_t)bytes_read;
                }

                if (file_size > 0 && buffer[bytes_read > 0 ? bytes_read - 1 : 0] != '\n') {
                    k_puts("");  /* terminate the last line if the file lacked one */
                }
                k_puts("--- EOF ---");
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
            k_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            k_puts("Usage: elf <filename>");
        } else if (krffs_open(filename) != 0) {
            k_printf("File not found: %s\n", filename);
        } else {
            file_size = krffs_get_file_size(filename);
            if (file_size > sizeof(buffer)) {
                k_printf("File too large: %d bytes\n", file_size);
            } else {
                bytes_read = krffs_read(filename, buffer, file_size, 0);
                if (bytes_read != (int)file_size) {
                    k_printf("Read error: %d/%d bytes\n", bytes_read, file_size);
                } else if (!elf_check(buffer, file_size)) {
                    k_printf("Not a valid ELF32 executable\n");
                } else {
                    entry_point = elf_entry_point(buffer);
                    phdrs = elf_program_headers(buffer);
                    phdr_count = elf_program_header_count(buffer);

                    k_printf("ELF Header:\n");
                    k_printf("  Entry point:       0x%X\n", entry_point);
                    k_printf("  Program headers:   %d\n", phdr_count);
                    for (i = 0; i < phdr_count; i++) {
                        if (phdrs[i].p_type == PT_LOAD) {
                            k_printf("  LOAD segment:\n");
                            k_printf("    Virtual addr:  0x%X\n", phdrs[i].p_vaddr);
                            k_printf("    File size:     %d bytes\n", phdrs[i].p_filesz);
                            k_printf("    Memory size:   %d bytes\n", phdrs[i].p_memsz);
                            k_printf("    Flags:         %s%s%s\n",
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
            k_puts("Usage: exec <filename> [args...]");
        } else {
            static char exec_cmd_buf[256];
            static char *exec_argv[16];
            int exec_argc = 0;
            char *p;
            int in_arg = 0;

            k_strncpy(exec_cmd_buf, args, sizeof(exec_cmd_buf) - 1);
            exec_cmd_buf[sizeof(exec_cmd_buf) - 1] = '\0';

            /* Split on spaces and tabs in place, building argv into the buffer. */
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
                k_puts("Usage: exec <filename> [args...]");
            }
        }
    } else if (str_starts_with(cmd, "touch ")) {
        const char *filename = cmd + 6;

        if (!krffs_fs) {
            k_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            k_puts("Usage: touch <filename>");
        } else {
            if (krffs_create(filename) == 0) {
                k_printf("Created file: %s\n", filename);
            } else {
                k_printf("Failed to create file: %s\n", filename);
            }
        }
    } else if (str_starts_with(cmd, "rm ")) {
        const char *filename = cmd + 3;

        if (!krffs_fs) {
            k_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            k_puts("Usage: rm <filename>");
        } else {
            if (krffs_unlink(filename) == 0) {
                k_printf("Deleted file: %s\n", filename);
            } else {
                k_printf("Failed to delete file: %s\n", filename);
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
            k_puts("No file system mounted.");
        } else if (old_buf[0] == '\0' || !new_name || new_name[0] == '\0') {
            k_puts("Usage: mv <oldname> <newname>");
        } else {
            if (krffs_rename(old_buf, new_name) == 0) {
                k_printf("Renamed '%s' to '%s'\n", old_buf, new_name);
            } else {
                k_printf("Failed to rename file\n");
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
            k_puts("No file system mounted.");
        } else if (file_buf[0] == '\0' || !data) {
            k_puts("Usage: write <filename> <data>");
        } else {
            if (krffs_open(file_buf) != 0) {
                krffs_create(file_buf);
            }
            /* Overwrite, not append. */
            krffs_truncate(file_buf, 0);
            {
                uint32_t len = k_strlen(data);
                int written = krffs_write(file_buf, data, len, 0);
                if (written > 0) {
                    k_printf("Wrote %d bytes to %s\n", written, file_buf);
                } else {
                    k_printf("Failed to write to %s\n", file_buf);
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
        k_puts("Testing memory isolation between processes...");
        k_puts("Two processes will write different values to same virtual address.");
        if (test_isolation()) {
            set_fore_color(light_green);
            k_puts("Memory isolation test passed!");
        } else {
            set_fore_color(light_red);
            k_puts("Memory isolation test FAILED!");
        }
        set_fore_color(light_grey);
    } else if (audio_debug_handle_command(cmd)) {
    } else if (str_equal(cmd, "netping")) {
        int rtt = icmp_ping(NET_IP_GATEWAY);
        if (rtt >= 0) {
            set_fore_color(light_green);
            k_printf("ping 10.0.2.2: reply in %d ticks (%d ms)\n",
                     rtt, rtt * 10);
            serial_printf("ping 10.0.2.2: reply in %d ticks\n", rtt);
        } else {
            set_fore_color(light_red);
            k_puts("ping 10.0.2.2: timeout");
            serial_printf("ping 10.0.2.2: timeout\n");
        }
        set_fore_color(light_grey);
    } else if (str_equal(cmd, "netbad")) {
        /* Ping an unreachable host on our subnet to exercise the timeout. */
        int rtt = icmp_ping(0x0A000263UL);   /* 10.0.2.99 */
        if (rtt >= 0) {
            set_fore_color(light_red);
            k_printf("ping 10.0.2.99: unexpected reply (%d ticks)\n", rtt);
            serial_printf("ping 10.0.2.99: unexpected reply\n");
        } else {
            set_fore_color(light_green);
            k_puts("ping 10.0.2.99: timeout (expected)");
            serial_printf("ping 10.0.2.99: timeout (expected)\n");
        }
        set_fore_color(light_grey);
    } else if (str_equal(cmd, "fault")) {
        volatile uint32_t *bad_ptr = (volatile uint32_t *) 0xA0000000;
        k_puts("Triggering page fault at 0xA0000000...");
        *bad_ptr = 0xDEADBEEF;
    } else if (str_equal(cmd, "halt")) {
        k_puts("Halting system. Goodbye!");
        __asm__ __volatile__("cli; hlt");
    } else if (str_equal(cmd, "usermode")) {
        k_puts("Testing user mode (ring 3) transition...");
        k_puts("Expected: 'Hello from ring 3!' followed by infinite loop.");
        test_usermode();
    } else if (cmd[0] != '\0') {
        k_printf("Unknown command: '%s'\n", cmd);
        k_puts("Type 'help' for available commands.");
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
    /* A launched program re-claims the terminal (exec_program sets sid/fg_pgid).
     * One interrupts-off update so the timer-IRQ drain never sees a torn owner. */
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
    k_puts("Type 'help' for available commands.");
    serial_puts("Type 'help' for available commands.\n");
    k_puts("");
    k_printf("> ");
    serial_puts("> ");

    while (1) {
        if (cmd_ready) {
            cmd_ready = false;
            process_command(cmd_buffer);
            cmd_pos = 0;
            k_printf("> ");
            serial_puts("> ");
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
                k_putc('\n');
                serial_putc('\r');
                serial_putc('\n');
                break;
            } else if (ch == '\b' || ch == 0x7F) {
                if (k_scrollback_is_active()) {
                    k_scrollback_end();
                }
                if (cmd_pos > 0) {
                    --cmd_pos;
                    k_putc('\b');
                    k_putc(' ');
                    k_putc('\b');
                    serial_putc('\b');
                    serial_putc(' ');
                    serial_putc('\b');
                }
            } else if (ch >= 0x20 && ch < 0x7F) {
                if (k_scrollback_is_active()) {
                    k_scrollback_end();
                }
                if (cmd_pos < CMD_BUFFER_SIZE - 1) {
                    cmd_buffer[cmd_pos++] = (char)ch;
                    k_putc((char)ch);
                    serial_putc((char)ch);
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
 * process table and scheduler, audio, the RAM disk and KRFFS, the pipe and pty
 * pools, and the network), runs the self-tests, then loops booting ISH as init
 * and dropping to the kernel debug shell each time init exits. Does not return.
 */
void k_main(multiboot_uint32_t magic, multiboot_info_t *info)
{
    uint32_t num_modules;

    serial_init();
    serial_printf("======= Rach2 Booting... =======\n\n");

    /* Parse multiboot modules BEFORE paging: reading module info needs direct
     * physical access. */
    num_modules = init_modules(magic, info);

    /* Push the bump allocator past the modules so it does not overwrite their
     * data when it carves out page tables and other structures. */
    if (num_modules > 0) {
        module_info_t *last_module = get_module(num_modules - 1);
        if (last_module && last_module->end > mem_reserved_ptr) {
            mem_reserved_ptr = (last_module->end + 0xFFF) & ~0xFFF;
        }
    }

    k_cls();

    set_fore_color(light_cyan);
    k_puts("======= Rach2 Booting... =======");
    set_fore_color(light_grey);
    k_puts("");

    k_puts("Initializing system components...");
    k_puts("");
    serial_printf("\nInitializing system components...\n");

    k_printf("  GDT...");
    serial_printf("  GDT...");
    init_gdt();
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  TSS...");
    serial_printf("  TSS...");
    init_tss((uint32_t)stack + KERNEL_STACK_SIZE);
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  IDT...");
    serial_printf("  IDT...");
    init_idt();
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  ISR...");
    serial_printf("  ISR...");
    init_isr();
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Memory manager...");
    serial_printf("  Memory manager...");
    init_mm();
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Paging...");
    serial_printf("  Paging...");
    /* Size the physical frame pool from the multiboot memory map, capped to the
     * direct-mapped window (frame_usable_count). */
    init_paging(frame_usable_count(info));
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Timer (100 Hz)...");
    serial_printf("  Timer (100 Hz)...");
    __asm__ __volatile__("cli");
    init_pit(100);
    set_isr_handler(32, &timer_handler); /* swap the PIT boot stub for the real handler */
    __asm__ __volatile__("sti");
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Entropy...");
    serial_printf("  Entropy...");
    krandom_seed();
    {
        /* RDSEED/RDRAND are trusted hardware entropy, so getrandom and
         * /dev/[u]random work; jitter is best-effort only, and the crypto
         * sources then fail closed. */
        int src = krandom_source();
        const char *desc = (src == KRANDOM_SRC_RDSEED)   ? " OK (RDSEED, trusted)"
                           : (src == KRANDOM_SRC_RDRAND) ? " OK (RDRAND, trusted)"
                                                         : " OK (jitter, best-effort -- "
                                                           "crypto RNG fails closed)";
        set_fore_color(src == KRANDOM_SRC_JITTER ? light_brown : light_green);
        k_puts(desc);
        serial_printf("%s\n", desc);
    }
    set_fore_color(light_grey);

    k_printf("  Keyboard...");
    serial_printf("  Keyboard...");
    set_isr_handler(33, &keyboard_handler); /* swap the keyboard boot stub for the real handler */
    tty_console_init(); /* console_tty: VGA + serial, line discipline for fds 0/1/2 */
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Mouse...");
    serial_printf("  Mouse...");
    if (init_mouse() == 0) {
        set_fore_color(light_green);
        k_puts(" OK");
        serial_printf(" OK\n");
    } else {
        set_fore_color(light_red);
        k_puts(" Not detected");
        serial_printf(" Not detected\n");
    }
    set_fore_color(light_grey);

    k_printf("  Syscall...");
    serial_printf("  Syscall...");
    init_syscall();
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Process...");
    serial_printf("  Process...");
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
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Scheduler...");
    serial_printf("  Scheduler...");
    init_sched();
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Audio...");
    serial_printf("  Audio...");
    init_audio();
    if (audio) {
        set_fore_color(light_green);
        k_printf(" OK (%s)\n", audio_get_boot_status());
        serial_printf(" OK (%s)\n", audio_get_boot_status());
        set_fore_color(light_grey);
    } else {
        set_fore_color(light_red);
        k_puts(" FAIL");
        serial_printf(" FAIL\n");
        set_fore_color(light_grey);
    }

    /* Modules were parsed early, before paging; just report them. */
    k_printf("  Modules...");
    serial_printf("  Modules...");
    if (num_modules > 0) {
        module_info_t *ramdisk = get_ramdisk();
        set_fore_color(light_green);
        k_printf(" OK (%d found)\n", num_modules);
        serial_printf(" OK (%d found)\n", num_modules);
        set_fore_color(light_grey);
        if (ramdisk) {
            k_printf("    RAM disk: 0x%X, size: %d bytes\n",
                     ramdisk->start, ramdisk->size);
        }
    } else {
        set_fore_color(light_brown);
        k_puts(" None");
        serial_printf(" None\n");
        set_fore_color(light_grey);
    }

    k_printf("  File system...");
    serial_printf("  File system...");
    if (num_modules > 0) {
        module_info_t *ramdisk = get_ramdisk();
        if (ramdisk && init_krffs((void *)ramdisk->start, ramdisk->size) == 0) {
            set_fore_color(light_green);
            k_printf(" OK (KRFFS, %d files)\n", krffs_count_files());
            serial_printf(" OK (KRFFS, %d files)\n", krffs_count_files());
            set_fore_color(light_grey);
        } else {
            set_fore_color(light_red);
            k_puts(" FAIL (invalid KRFFS)");
            serial_printf(" FAIL (invalid KRFFS)\n");
        }
    } else {
        set_fore_color(light_brown);
        k_puts(" None (no ramdisk)");
        serial_printf(" None (no ramdisk)\n");
    }
    set_fore_color(light_grey);

    /* Allocate the pipe pool (kept out of BSS) before user processes can pipe(). */
    pipe_init_pool();

    /* Allocate the pty pool before user processes can open /dev/ptmx. */
    pty_init_pool();

    k_printf("  Network...");
    serial_printf("  Network...");
    if (init_network()) {
        set_fore_color(light_green);
        k_printf(" OK (%s)\n", net_status());
        serial_printf(" OK (%s)\n", net_status());
        set_fore_color(light_grey);
        net_print_info();
    } else {
        set_fore_color(light_brown);
        k_printf(" None (%s)\n", net_error());
        serial_printf(" None (%s)\n", net_error());
        set_fore_color(light_grey);
    }

    k_puts("");
    set_fore_color(light_cyan);
    k_puts("================================");
    set_fore_color(light_grey);
    k_puts("");
    k_puts("---");
    k_puts("");
    serial_printf("\n================================\n\n---\n");

    run_self_tests();

    k_puts("");
    k_puts("---");
    k_puts("");
    serial_printf("\n---\n\n");

    /*
     * Boot into ISH as init. boot_init_process returns when ISH is missing or
     * fails, or when init later exits, so each pass drops to the kernel debug
     * shell until a new exec restarts the cycle.
     */
    while (1) {
        init_pid = 0;

        boot_init_process();

        k_puts("");
        k_puts("Kernel debug shell active.");
        shell_loop();
    }
}
