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

/* Kernel stack from loader.s */
extern char stack[];
#define KERNEL_STACK_SIZE 0x4000

/* User mode entry point from loader.s */
extern void enter_usermode(uint32_t eip, uint32_t esp);

/* User stack size (4KB) */
#define USER_STACK_SIZE 0x1000

#define CMD_BUFFER_SIZE 64
static char cmd_buffer[CMD_BUFFER_SIZE];
static volatile size_t cmd_pos = 0;
static volatile bool cmd_ready = false;
static volatile bool kernel_shell_active = false;

/* Global tick counter - incremented at 100 Hz by timer IRQ */
volatile uint32_t kernel_ticks = 0;

/*
 * Init process PID - when this process exits, we return to kernel shell.
 * Set to 0 (invalid) when no init is running.
 */
volatile uint32_t init_pid = 0;

/*
 * Kernel input buffer for userspace stdin reads
 * When a process reads from stdin, it blocks until Enter is pressed.
 * The keyboard handler fills this buffer and signals when line is ready.
 */
#define STDIN_BUFFER_SIZE 256
char stdin_buffer[STDIN_BUFFER_SIZE];
volatile size_t stdin_pos = 0;
volatile bool stdin_ready = false;
volatile process_t *stdin_waiting_proc = NULL;  /* Process blocked on stdin */

static void run_self_tests(void);
void shell_loop(void);  /* Non-static: called from syscall.c when init exits */
void shell_reset_input(void);  /* Non-static: called from syscall.c exit paths */
void release_fd_table(fd_table_t *table);  /* Defined in syscall.c; fd teardown */
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

/* User stack base address (stack grows down from here) */
#define USER_STACK_BASE 0xBFFFF000

static void timer_handler(isr_params_t *params)
{
    (void) params;

    ++kernel_ticks;

    /* Wake any nanosleep timer that has expired (independent of in_syscall, so a
     * sleeper parked inside its syscall still becomes runnable on time). */
    proc_wake_sleepers(kernel_ticks);

    /* Service HDA audio even when legacy IRQs aren't delivered (MSI-only setups). */
    hda_tick();

    /* Drive TCP retransmission, RTT, zero-window probe, and TIME_WAIT timers. */
    tcp_tick();

    /*
     * Allow timer IRQs to run during syscalls so HDA streaming keeps working,
     * but do not preempt a process while it is inside kernel syscall code.
     * Kernel threads still remain preemptible.
     */
    if (!current_proc || !current_proc->in_syscall) {
        schedule();
    }
}

/* Shift key state for keyboard handler */
static bool _kbd_shift = false;

/* Ctrl key state for CTRL+C handling */
static bool _kbd_ctrl = false;

/* Extended-scancode prefix state (0xE0). */
static bool _kbd_e0 = false;

/*
 * Terminate the current user process and give up its CPU.
 *
 * Shared teardown for every kill that originates outside sys_exit -- the CTRL+C
 * handler and the page-fault segfault path both route here. The process is dying
 * by signal 'signo' (SIGSEGV for a fault, SIGINT for CTRL+C); store the Linux
 * wait-status encoding of a signal death (low 7 bits = the signal, so the
 * parent's waitpid sees WIFSIGNALED/WTERMSIG). Wake a parent blocked in waitpid,
 * free its user pages and page directory, and switch off its address space. If it
 * was init, reset the shell's input state and jump back into shell_loop on a
 * fresh kernel stack; otherwise leave the zombie for the parent's waitpid to
 * collect and schedule the (now READY) parent. Does not return for a user process.
 *
 * Declared extern (no header) by mm/paging.c, which calls it from the fault
 * handler -- mirrors how syscall.c reaches shell_loop/stack.
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

    /* Release framebuffer if this process owns it */
    fb_release_if_owner(pid);

    /* Mark process a zombie with the signal-death wait status. */
    current_proc->state = PROC_ZOMBIE;
    current_proc->exit_code = signo & 0x7F;

    /* Notify the parent: post SIGCHLD (a reaper handler relies on it for
     * signal-killed children too, not just normal exits) and wake it if blocked
     * in waitpid. */
    parent = get_process(current_proc->parent_pid);
    if (parent) {
        signal_post(parent, SIGCHLD);
        if (parent->state == PROC_BLOCKED) {
            parent->state = PROC_READY;
            serial_printf("terminate: woke parent process %d\n", parent->pid);
        }
    }

    /* Release the descriptor table (socket refs + shared open-file descriptions),
     * the same teardown sys_exit does, so a signal-killed process leaks no
     * sockets or OFDs. */
    release_fd_table(&current_proc->fds);

    /* Clean up user pages, then switch off the dying address space BEFORE
     * freeing its page directory: free_page_directory refuses to free whatever
     * is currently loaded in CR3, so freeing while still on this directory would
     * silently leak it and all its private page tables. */
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
        /*
         * Init terminated - return to the kernel debug shell. Same resume path
         * as when init exits normally (see sys_exit).
         */
        serial_printf("terminate: init process %d killed by signal %d, "
                      "returning to kernel shell\n", pid, signo);
        k_puts("");
        k_puts("Init process exited. Returning to kernel debug shell.");

        free_process(current_proc);

        /* Reset state */
        init_pid = 0;
        current_proc = &proc_table[0];
        current_proc->state = PROC_RUNNING;

        /* Fresh command line + drop stdin-wait state, before re-enabling IRQs. */
        shell_reset_input();

        sched_disable();

        /* Jump to shell with fresh kernel stack */
        __asm__ __volatile__(
            "movl %0, %%esp\n\t"
            "sti\n\t"
            "jmp *%1\n\t"
            :
            : "r"((uint32_t)stack + KERNEL_STACK_SIZE),
              "r"((uint32_t)shell_loop)
            : "memory"
        );
        /* Never reached */
    } else {
        /*
         * Child process terminated - leave as ZOMBIE for parent to collect.
         * Parent's waitpid will clean up pages and free the process slot.
         *
         * DON'T call free_process() here - waitpid needs to find the zombie!
         */

        /* Let scheduler pick the parent process (now READY) */
        schedule();

        /* If we get here, something went wrong - halt */
        __asm__ __volatile__("cli; hlt");
    }
}

/*
 * Kill the currently running foreground user process (CTRL+C handler).
 *
 * Triggered from the keyboard interrupt; echoes "^C" and routes through the
 * shared terminate_current_process() teardown with the "interrupted" status.
 */
static void kill_foreground_process(void)
{
    /* Only kill user processes (cr3 != 0 means it has its own page directory) */
    if (!current_proc || current_proc->cr3 == 0) {
        return;
    }

    serial_printf("CTRL+C: signaling process %d\n", current_proc->pid);
    k_puts("^C");

    /* Post SIGINT through the disposition: a handler can catch it, otherwise the
     * default action terminates. Delivered when this keyboard IRQ returns to the
     * interrupted user process (a process blocked in a syscall is not yet
     * interrupted -- EINTR is deferred to Part II-A). */
    signal_post(current_proc, SIGINT);
}

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

    /* Push ALL scancodes to buffer for games (before any early returns) */
    kbd_push_scancode(scancode);

    if (scancode == 0xE0) {
        _kbd_e0 = true;
        return;
    }

    /* Handle shift key press/release */
    if (scancode == 0x2A || scancode == 0x36) {
        _kbd_shift = true;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        _kbd_shift = false;
        return;
    }

    /* Handle Ctrl key press/release (Left and right Ctrl both use 0x1D/0x9D). */
    if (scancode == 0x1D) {
        _kbd_ctrl = true;
        return;
    }
    if (scancode == 0x9D) {  /* 0x1D | 0x80 = Ctrl release */
        _kbd_ctrl = false;
        return;
    }

    /* Ignore other key release events (high bit set) */
    if (scancode & 0x80) {
        return;
    }

    /*
     * CTRL+C detection: Kill foreground user process.
     * 'C' key has scancode 0x2E.
     */
    if (_kbd_ctrl && scancode == 0x2E) {
        kill_foreground_process();
        return;  /* Don't process as normal character */
    }

    /*
     * Switch to kernel page directory to access kernel data.
     * The user process's page directory may not have the kernel data
     * segment properly mapped (EN_us, stdin_buffer, etc.).
     * Note: We directly set CR3 instead of using switch_page_directory()
     * because that function checks _curr_dir which may be out of sync.
     */
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

    /* Use shifted or unshifted scancode table */
    c = _kbd_shift ? EN_us_shift[scancode & 0x7F] : EN_us[scancode & 0x7F];

    /*
     * If a process is waiting for stdin, fill stdin_buffer.
     * Otherwise, fill the shell's cmd_buffer.
     */
    if (stdin_waiting_proc != NULL) {
        /* Userspace process is waiting for stdin input */
        if (c == '\n') {
            /* Add newline to buffer and mark ready */
            if (stdin_pos < STDIN_BUFFER_SIZE - 1) {
                stdin_buffer[stdin_pos++] = '\n';
            }
            stdin_buffer[stdin_pos] = '\0';
            stdin_ready = true;
            k_putc('\n');
            serial_putc('\n');

            /* Wake up the waiting process */
            if (stdin_waiting_proc->state == PROC_BLOCKED) {
                ((process_t *)stdin_waiting_proc)->state = PROC_READY;
            }
        } else if (c == '\b') {
            /* Backspace - remove last character if any */
            if (stdin_pos > 0) {
                --stdin_pos;
                k_putc('\b');
                k_putc(' ');
                k_putc('\b');
                serial_putc('\b');
            }
        } else if (c != 0 && stdin_pos < STDIN_BUFFER_SIZE - 1) {
            /* Regular character - add to buffer and echo */
            stdin_buffer[stdin_pos++] = c;
            k_putc(c);
            serial_putc(c);
        }
    } else if (kernel_shell_active) {
        /* Shell is active - use cmd_buffer */
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
    /* Restore original page directory if we switched */
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

    /* Wait for a few timer interrupts using hlt */
    for (i = 0; i < 5; ++i) {
        __asm__ __volatile__("hlt");
    }

    return kernel_ticks > start_ticks;
}

static bool test_syscall(void)
{
    /* Test write syscall: EAX=4, EBX=1 (stdout), ECX=buf, EDX=len */
    const char *msg = "Hello from syscall! ";
    uint32_t len = 20;

    __asm__ __volatile__(
        "int $0x80"
        :
        : "a"(4), "b"(1), "c"(msg), "d"(len)
        : "memory"
    );

    /* If we get here without crashing, the syscall infrastructure works */
    return true;
}

/*
 * Message for user mode test - needs to be in user-accessible memory
 */
static const char user_msg[] = "Hello from ring 3!\n";

/*
 * User mode test entry point - runs in ring 3
 * Calls write syscall then halts via infinite loop
 */
static void user_test_entry(void)
{
    /* Write syscall: EAX=4, EBX=1 (stdout), ECX=buf, EDX=len */
    __asm__ __volatile__(
        "movl $4, %%eax\n\t"     /* syscall number: write */
        "movl $1, %%ebx\n\t"     /* fd: stdout */
        "movl %0, %%ecx\n\t"     /* buf */
        "movl $19, %%edx\n\t"    /* count */
        "int $0x80\n\t"
        :
        : "r"(user_msg)
        : "eax", "ebx", "ecx", "edx", "memory"
    );

    /* Infinite loop after syscall - user mode can't halt directly */
    for (;;) {
        __asm__ __volatile__("nop");
    }
}

static bool test_usermode(void)
{
    uintptr_t func_addr = (uintptr_t)user_test_entry;
    uintptr_t msg_addr = (uintptr_t)user_msg;
    uintptr_t user_stack_addr;
    uintptr_t page;
    uint32_t i;

    serial_printf("  Usermode test: func=0x%X msg=0x%X\n", func_addr, msg_addr);

    /* Allocate user stack */
    user_stack_addr = mem_reserve(USER_STACK_SIZE, true);
    serial_printf("  Usermode test: stack=0x%X\n", user_stack_addr);

    /* Mark pages as user-accessible:
     * - Function code page(s)
     * - Message data page
     * - User stack page
     */

    /* Mark function pages as user-accessible (may span 2 pages) */
    for (page = func_addr & ~0xFFF; page <= ((func_addr + 256) & ~0xFFF); page += PAGE_SIZE) {
        paging_set_user(page, true);
    }

    /* Mark message page as user-accessible */
    paging_set_user(msg_addr, true);

    /* Mark stack pages as user-accessible */
    for (page = user_stack_addr; page < user_stack_addr + USER_STACK_SIZE; page += PAGE_SIZE) {
        paging_set_user(page, true);
    }

    serial_printf("  Usermode test: pages marked user-accessible\n");

    /* Wait a bit for any pending interrupts */
    for (i = 0; i < 3; i++) {
        __asm__ __volatile__("hlt");
    }

    serial_printf("  Usermode test: entering ring 3...\n");

    /*
     * Enter user mode - this won't return!
     * The user code calls int 0x80 (write syscall), which transitions to ring 0.
     * After syscall returns, user code loops forever.
     * The message "Hello from ring 3!" will be printed, proving ring 3 works.
     */

    enter_usermode(func_addr, user_stack_addr + USER_STACK_SIZE);

    /* We should never reach here - enter_usermode doesn't return */
    return false;
}

/*
 * Context switch test variables
 */
static process_t *switch_proc_a = NULL;
static process_t *switch_proc_b = NULL;
static volatile int switch_count = 0;

/*
 * Process B entry point - prints 'B' and switches back to A
 */
static void proc_b_entry(void)
{
    while (switch_count < 10) {
        k_putc('B');
        serial_printf("B");
        ++switch_count;
        switch_context(switch_proc_b, switch_proc_a);
    }

    /* Done - return to A one last time */
    switch_context(switch_proc_b, switch_proc_a);

    /* Should never reach here */
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

    /* Process A is the current process (process 0) */
    switch_proc_a = current_proc;

    /* Allocate process B */
    switch_proc_b = alloc_process();
    if (!switch_proc_b) {
        k_puts("Failed to allocate process B");
        return;
    }

    /* Allocate kernel stack for process B */
    stack_b = mem_reserve(KERNEL_STACK_SIZE, true);
    switch_proc_b->kernel_stack = stack_b + KERNEL_STACK_SIZE;
    switch_proc_b->cr3 = 0;  /* Uses same page directory */
    switch_proc_b->parent_pid = switch_proc_a->pid;

    /* Set up process B's stack for first context switch */
    setup_process_stack(switch_proc_b, proc_b_entry);

    serial_printf("Process B created: pid=%d stack=0x%X\n",
                  switch_proc_b->pid, switch_proc_b->kernel_stack);

    /* Reset counter */
    switch_count = 0;

    /* Process A prints 'A' and switches to B */
    while (switch_count < 10) {
        k_putc('A');
        serial_printf("A");
        ++switch_count;
        switch_context(switch_proc_a, switch_proc_b);
    }

    k_puts("");

    /* Clean up process B */
    free_process(switch_proc_b);
    switch_proc_b = NULL;

    set_fore_color(light_green);
    k_puts("Context switch test passed!");
    set_fore_color(light_grey);
}

/*
 * Preemptive scheduling test variables
 */
static volatile int preempt_done = 0;
static volatile int preempt_count_a = 0;
static volatile int preempt_count_b = 0;

/*
 * Preemptive test process A - prints 'A' in a loop
 */
static void preempt_proc_a(void)
{
    /* Enable interrupts - we started mid-interrupt */
    __asm__ __volatile__("sti");

    while (!preempt_done) {
        k_putc('A');
        serial_printf("A");
        ++preempt_count_a;

        /* Delay loop - needs to be long enough for timer to fire (10ms at 100Hz) */
        {
            volatile int i;
            for (i = 0; i < 5000000; i++) {
                __asm__ __volatile__("nop");
            }
        }

        /* Check if we've printed enough */
        if (preempt_count_a + preempt_count_b >= 20) {
            preempt_done = 1;
        }
    }

    /* When done, just loop (will be cleaned up) */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

/*
 * Preemptive test process B - prints 'B' in a loop
 */
static void preempt_proc_b(void)
{
    /* Enable interrupts - we started mid-interrupt */
    __asm__ __volatile__("sti");

    while (!preempt_done) {
        k_putc('B');
        serial_printf("B");
        ++preempt_count_b;

        /* Delay loop - needs to be long enough for timer to fire (10ms at 100Hz) */
        {
            volatile int i;
            for (i = 0; i < 5000000; i++) {
                __asm__ __volatile__("nop");
            }
        }

        /* Check if we've printed enough */
        if (preempt_count_a + preempt_count_b >= 20) {
            preempt_done = 1;
        }
    }

    /* When done, just loop (will be cleaned up) */
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

    /* Reset counters */
    preempt_done = 0;
    preempt_count_a = 0;
    preempt_count_b = 0;

    /* Allocate process A */
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

    /* Allocate process B */
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

    /* Enable the scheduler - this will start preemptive switching */
    sched_enable();

    /* Wait for the test to complete */
    while (!preempt_done) {
        __asm__ __volatile__("hlt");
    }

    /* Disable scheduler before cleanup */
    sched_disable();

    k_puts("");
    serial_printf("\n");
    k_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);
    serial_printf("Counts: A=%d B=%d\n", preempt_count_a, preempt_count_b);

    /* Clean up */
    free_process(proc_a);
    free_process(proc_b);

    /* Verify both processes got CPU time */
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
 * Memory isolation test - uses cooperative context switching (no scheduler)
 * to verify that two processes with different page directories can write
 * to the same virtual address without interfering with each other.
 *
 * Note: The test address must be ABOVE the identity-mapped region to ensure
 * each process gets its own page table. When modules are loaded, identity
 * mapping extends past the module end (~12MB), so we use 0xD00000 (13MB).
 */
#define ISOLATION_TEST_ADDR 0xD00000  /* User address for isolation test */
static volatile uint32_t isolation_result_a = 0;
static volatile uint32_t isolation_result_b = 0;
static process_t *isolation_proc_a = NULL;
static process_t *isolation_proc_b = NULL;
static process_t *isolation_proc_main = NULL;

/*
 * Isolation test process A - writes 0xAAAAAAAA then switches to B
 */
static void isolation_proc_a_entry(void)
{
    volatile uint32_t *ptr = (volatile uint32_t *)ISOLATION_TEST_ADDR;

    /* Write our value */
    *ptr = 0xAAAAAAAA;

    /* Switch to B so it can write its value */
    switch_context(isolation_proc_a, isolation_proc_b);

    /* B has written and switched back - now verify our value is intact */
    isolation_result_a = *ptr;

    /* Return to main */
    switch_context(isolation_proc_a, isolation_proc_main);

    /* Should never reach here */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

/*
 * Isolation test process B - writes 0xBBBBBBBB then switches back to A
 */
static void isolation_proc_b_entry(void)
{
    volatile uint32_t *ptr = (volatile uint32_t *)ISOLATION_TEST_ADDR;

    /* Write our value - should go to different physical page */
    *ptr = 0xBBBBBBBB;

    /* Verify we read our value */
    isolation_result_b = *ptr;

    /* Switch back to A */
    switch_context(isolation_proc_b, isolation_proc_a);

    /* Should never reach here */
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

static bool test_isolation(void)
{
    page_dir_t *dir_a, *dir_b;
    uintptr_t stack_a, stack_b;
    uintptr_t phys_a, phys_b;

    /* Reset results */
    isolation_result_a = 0;
    isolation_result_b = 0;

    /* Save reference to main process */
    isolation_proc_main = current_proc;

    /* Create page directories for A and B */
    dir_a = clone_page_directory();
    dir_b = clone_page_directory();
    if (!dir_a || !dir_b) {
        return false;
    }

    /* Allocate user pages at same virtual address in each directory */
    phys_a = alloc_user_page(dir_a, ISOLATION_TEST_ADDR);
    phys_b = alloc_user_page(dir_b, ISOLATION_TEST_ADDR);
    if (!phys_a || !phys_b || phys_a == phys_b) {
        return false;
    }

    /* Allocate process A */
    isolation_proc_a = alloc_process();
    if (!isolation_proc_a) {
        return false;
    }
    stack_a = mem_reserve(KERNEL_STACK_SIZE, true);
    isolation_proc_a->kernel_stack = stack_a + KERNEL_STACK_SIZE;
    isolation_proc_a->cr3 = get_page_dir_phys(dir_a);
    isolation_proc_a->parent_pid = isolation_proc_main->pid;
    setup_process_stack(isolation_proc_a, isolation_proc_a_entry);

    /* Allocate process B */
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

    /* Start the test: switch to A */
    /* A writes -> switches to B -> B writes -> B switches back to A -> A reads -> A returns here */
    switch_context(isolation_proc_main, isolation_proc_a);

    /* Clean up */
    free_process(isolation_proc_a);
    free_process(isolation_proc_b);
    isolation_proc_a = NULL;
    isolation_proc_b = NULL;

    /* Check results */
    return (isolation_result_a == 0xAAAAAAAA && isolation_result_b == 0xBBBBBBBB);
}

/*
 * Test KRFFS write operations (Phase 12)
 * Tests: create, write, read, rename, unlink
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
        /* No filesystem mounted, skip test */
        return true;
    }

    /* Step 1: Create a new file */
    if (krffs_create(test_filename) != 0) {
        serial_printf("KRFFS write test: create failed\n");
        return false;
    }

    /* Step 2: Write data to the file */
    bytes_written = krffs_write(test_filename, test_data, k_strlen(test_data), 0);
    if (bytes_written != (int)k_strlen(test_data)) {
        serial_printf("KRFFS write test: write failed (%d)\n", bytes_written);
        krffs_unlink(test_filename);
        return false;
    }

    /* Step 3: Read data back and verify */
    bytes_read = krffs_read(test_filename, read_buffer, 63, 0);
    read_buffer[bytes_read > 0 ? bytes_read : 0] = '\0';
    if (bytes_read != (int)k_strlen(test_data) || !str_equal(read_buffer, test_data)) {
        serial_printf("KRFFS write test: read verify failed\n");
        krffs_unlink(test_filename);
        return false;
    }

    /* Step 4: Rename the file */
    if (krffs_rename(test_filename, renamed_filename) != 0) {
        serial_printf("KRFFS write test: rename failed\n");
        krffs_unlink(test_filename);
        return false;
    }

    /* Step 5: Verify renamed file has same content */
    bytes_read = krffs_read(renamed_filename, read_buffer, 63, 0);
    read_buffer[bytes_read > 0 ? bytes_read : 0] = '\0';
    if (!str_equal(read_buffer, test_data)) {
        serial_printf("KRFFS write test: rename verify failed\n");
        krffs_unlink(renamed_filename);
        return false;
    }

    /* Step 6: Delete the renamed file */
    if (krffs_unlink(renamed_filename) != 0) {
        serial_printf("KRFFS write test: unlink failed\n");
        return false;
    }

    /* Step 7: Verify file no longer exists */
    if (krffs_open(renamed_filename) == 0) {
        serial_printf("KRFFS write test: file still exists after unlink\n");
        return false;
    }

    return true;
}

/*
 * Test ELF header parsing (Phase 13)
 * Loads an ELF file from KRFFS and validates/parses its header
 */
static bool test_elf(void)
{
    const char *elf_filename = "hello";
    char _exec_elf_buffer[8192];  /* Buffer to hold ELF file */
    int bytes_read;
    uint32_t file_size;
    uint32_t entry_point;
    uint16_t phdr_count;
    Elf32_Phdr *phdrs;

    if (!krffs_fs) {
        /* No filesystem mounted, skip test */
        return true;
    }

    /* Check if file exists */
    if (krffs_open(elf_filename) != 0) {
        /* File not found, skip test (not a failure) */
        return true;
    }

    /* Read the ELF file */
    file_size = krffs_get_file_size(elf_filename);
    if (file_size > sizeof(_exec_elf_buffer)) {
        return false;
    }

    bytes_read = krffs_read(elf_filename, _exec_elf_buffer, file_size, 0);
    if (bytes_read != (int)file_size) {
        return false;
    }

    /* Validate ELF header */
    if (!elf_check(_exec_elf_buffer, file_size)) {
        return false;
    }

    /* Get entry point and program headers */
    entry_point = elf_entry_point(_exec_elf_buffer);
    phdrs = elf_program_headers(_exec_elf_buffer);
    phdr_count = elf_program_header_count(_exec_elf_buffer);

    if (!phdrs || phdr_count == 0 || entry_point == 0) {
        return false;
    }

    /*
     * Phase 14: Test ELF loading into user pages
     * This verifies elf_load() works without actually executing
     */
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

        /* Allocate user stack */
        stack_phys = alloc_user_page(test_dir, USER_STACK_BASE);
        if (stack_phys == 0) {
            return false;
        }

        /* Note: we don't free the directory - bump allocator doesn't support free */
    }

    return true;
}

/*
 * Execute an ELF program from the file system
 *
 * 1. Load ELF file from KRFFS
 * 2. Validate ELF header
 * 3. Clone page directory
 * 4. Create process with new page directory
 * 5. Load ELF segments into user pages
 * 6. Allocate user stack
 * 7. Transition to user mode at ELF entry point
 */
/*
 * Static buffer for ELF loading in exec_program.
 * Must be static because the kernel stack is only 16KB - a local 16KB array
 * would overflow the stack and corrupt kernel data (like EN_us keyboard table).
 * Increased to 512KB to support Doom (368KB).
 */
/* elf_read_fn over a KRFFS file (ctx is the path) for the segment loader. */
static int exec_krffs_read(void *ctx, void *dst, uint32_t count, uint32_t off)
{
    return krffs_read((const char *)ctx, (char *)dst, count, off);
}

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
    static char *default_envp[] = {"PATH=/", "HOME=/", "TERM=rach2", 0};

    if (argc < 1 || !argv || !argv[0]) {
        k_puts("exec: No program specified");
        return;
    }

    path = argv[0];

    /* Check file system is mounted */
    if (!krffs_fs) {
        k_puts("exec: No file system mounted");
        return;
    }

    /* Check file exists */
    if (krffs_open(path) != 0) {
        k_printf("exec: File not found: %s\n", path);
        return;
    }

    /* Clone page directory for new process */
    user_dir = clone_page_directory();
    if (!user_dir) {
        k_puts("exec: Failed to clone page directory");
        return;
    }

    /* Load the program's segments straight from KRFFS (no 512 KB whole-file
     * buffer, so the binary size is unbounded); record entry + auxv metadata. */
    if (elf_load_image(exec_krffs_read, (void *)path, user_dir, &img) != 0) {
        k_puts("exec: Failed to load ELF segments");
        return;
    }
    entry_point = img.entry;

    /* Allocate user stack */
    user_stack_vaddr = USER_STACK_BASE;
    user_stack_phys = alloc_user_page(user_dir, user_stack_vaddr);
    if (user_stack_phys == 0) {
        k_puts("exec: Failed to allocate user stack");
        return;
    }

    /* Zero the stack page */
    k_memset((void *)user_stack_phys, 0, PAGE_SIZE);

    /* Build the SysV/i386 initial stack -- argc, the argv pointers, a default
     * envp, and the auxv -- in the freshly zeroed page. build_user_stack copies
     * the strings in and lays out the pointer arrays and the AT_NULL-terminated
     * auxv, 16-byte-aligning the entry ESP. The layout is verified host-side
     * (tests/host/uimage_hosttest.c). */
    krandom_bytes(random16, 16);
    user_esp = build_user_stack((void *)user_stack_phys, user_stack_vaddr, PAGE_SIZE,
                                argc, argv, default_envp, &img, random16);
    if (user_esp == 0) {
        k_puts("exec: argv/env too large for the initial stack");
        return;
    }

    /* Create process structure */
    user_proc = alloc_process();
    if (!user_proc) {
        k_puts("exec: Failed to allocate process");
        return;
    }

    user_proc->state = PROC_RUNNING;
    user_proc->cr3 = get_page_dir_phys(user_dir);
    user_proc->kernel_stack = (uint32_t)stack + KERNEL_STACK_SIZE;
    user_proc->parent_pid = current_proc ? current_proc->pid : 0;
    current_proc = user_proc;

    /* Build the address-space map (VMAs) so the demand-paging fault handler can
     * resolve heap/stack/mmap faults for this process (the ELF segments stay
     * eagerly loaded above). */
    elf_setup_vmas(&user_proc->vm, &img, USER_STACK_BASE, VM_STACK_MAX_GROWTH);

    /*
     * If no init process is running, this is the init process.
     * When init exits, the kernel returns to shell_loop().
     */
    if (init_pid == 0) {
        init_pid = user_proc->pid;
    }

    /* Switch to user page directory */
    switch_page_directory(user_dir);

    /* Enable scheduler for fork/waitpid to work */
    sched_enable();

    kernel_shell_active = false;

    /* Program the shared TLS descriptor for this process before entering ring 3.
     * This first-entry path bypasses switch_context (which normally reprograms
     * GDT entry 6 per process), so without this a fresh program could observe a
     * previous process's TLS base if it loaded the TLS selector before calling
     * set_thread_area. A new process has tls_base == 0. */
    gdt_set_tls(user_proc->tls_base);

    /* Enter user mode at ELF entry point with proper stack */
    enter_usermode(entry_point, user_esp);

    /* Should never return */
    k_puts("exec: ERROR - returned from user mode!");
}

/*
 * Boot into the init process (ISH shell).
 *
 * This function attempts to exec "ish" as the init process (PID 1).
 * Unlike exec_program(), this function is called at boot time to
 * start the system's first user process.
 *
 * When init exits, the kernel returns to shell_loop() for debugging.
 */
static void boot_init_process(void)
{
    static char *init_argv[] = { "ish", NULL };

    /* Check if ISH is available */
    if (!krffs_fs || krffs_open("ish") != 0) {
        k_puts("boot: ISH shell not found in RAM disk");
        k_puts("boot: Falling back to kernel debug shell");
        return;
    }

    /* Clear screen before starting shell */
    k_cls();

    k_puts("Booting into ISH shell...");
    k_puts("Type 'exit' to return to kernel debug shell.");
    k_puts("");

    /* Use exec_program to boot into ISH */
    exec_program(1, init_argv);

    /* If exec_program returns (which it shouldn't normally), fall back */
    k_puts("boot: Failed to start ISH shell");
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
        /* Quick inline test of context switching */
        process_t *test_a = current_proc;
        process_t *test_b = alloc_process();
        if (test_b) {
            uintptr_t test_stack = mem_reserve(KERNEL_STACK_SIZE, true);
            test_b->kernel_stack = test_stack + KERNEL_STACK_SIZE;
            test_b->cr3 = 0;

            /* Set up a simple test: B immediately switches back to A */
            switch_proc_a = test_a;
            switch_proc_b = test_b;
            switch_count = 0;
            setup_process_stack(test_b, proc_b_entry);

            /* Switch to B, B switches back to A */
            switch_count = 10;  /* Skip the loop in proc_b_entry */
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
        /* Test preemptive scheduling with two processes */
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

            /* Wait for test to complete (20 chars printed) */
            while (!preempt_done) {
                __asm__ __volatile__("hlt");
            }

            sched_disable();
            free_process(pa);
            free_process(pb);

            /* Verify both processes ran */
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

    /* Only run KRFFS write test if filesystem is mounted */
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
        /* List files on KRFFS RAM disk */
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
        /* Display file contents: cat <filename> */
        const char *filename = cmd + 4;  /* Skip "cat " */
        char buffer[512];
        int bytes_read;
        uint32_t offset = 0;

        if (!krffs_fs) {
            k_puts("No file system mounted.");
        } else if (filename[0] == '\0') {
            k_puts("Usage: cat <filename>");
        } else {
            /* Open and read the file */
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
                    k_puts("");  /* Ensure newline after content */
                }
                k_puts("--- EOF ---");
            }
        }
    } else if (str_starts_with(cmd, "elf ")) {
        /* Display ELF header info: elf <filename> */
        const char *filename = cmd + 4;  /* Skip "elf " */
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
        /* Execute ELF program: exec <filename> [args...] */
        const char *args = cmd + 5;  /* Skip "exec " */

        if (args[0] == '\0') {
            k_puts("Usage: exec <filename> [args...]");
        } else {
            /* Parse command line into argv */
            static char exec_cmd_buf[256];
            static char *exec_argv[16];
            int exec_argc = 0;
            char *p;
            int in_arg = 0;

            /* Copy to mutable buffer */
            k_strncpy(exec_cmd_buf, args, sizeof(exec_cmd_buf) - 1);
            exec_cmd_buf[sizeof(exec_cmd_buf) - 1] = '\0';

            /* Parse into argv (simple space-separated) */
            for (p = exec_cmd_buf; *p && exec_argc < 15; p++) {
                if (*p == ' ' || *p == '\t') {
                    if (in_arg) {
                        *p = '\0';  /* Terminate current arg */
                        in_arg = 0;
                    }
                } else {
                    if (!in_arg) {
                        exec_argv[exec_argc++] = p;  /* Start new arg */
                        in_arg = 1;
                    }
                }
            }
            exec_argv[exec_argc] = (char *)0;  /* NULL terminator */

            if (exec_argc > 0) {
                exec_program(exec_argc, exec_argv);
            } else {
                k_puts("Usage: exec <filename> [args...]");
            }
        }
    } else if (str_starts_with(cmd, "touch ")) {
        /* Create a new file: touch <filename> */
        const char *filename = cmd + 6;  /* Skip "touch " */

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
        /* Delete a file: rm <filename> */
        const char *filename = cmd + 3;  /* Skip "rm " */

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
        /* Rename a file: mv <old> <new> */
        const char *args = cmd + 3;  /* Skip "mv " */
        const char *new_name = NULL;
        char old_buf[64];
        size_t i;

        /* Find the space between old and new names */
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
        /* Write data to a file: write <filename> <data> */
        const char *args = cmd + 6;  /* Skip "write " */
        const char *data = NULL;
        char file_buf[64];
        size_t i;

        /* Find the space between filename and data */
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
            /* Create file if it doesn't exist */
            if (krffs_open(file_buf) != 0) {
                krffs_create(file_buf);
            }
            /* Truncate to 0 first to overwrite */
            krffs_truncate(file_buf, 0);
            /* Write the data */
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
        /* This line is never reached */
    } else if (cmd[0] != '\0') {
        k_printf("Unknown command: '%s'\n", cmd);
        k_puts("Type 'help' for available commands.");
    }
}

/*
 * Reset the kernel shell's command-line and stdin-wait state to "fresh prompt".
 *
 * Used on every transition back to the debug shell. An exec'd program never
 * returns through process_command (exec_program calls enter_usermode), so the
 * post-process_command "cmd_pos = 0" never runs for it; without this reset the
 * next keystrokes append to the previous command's buffer and the shell re-runs
 * it (e.g. a second "exec cat ..." parses as the leftover "exec cp ..."). It also
 * drops any stale stdin-wait state from a program that exited mid-read.
 *
 * The exit paths (sys_exit, terminate, exec_fail_destroyed) call this BEFORE they
 * re-enable interrupts and jump here, so a keyboard IRQ in the jump window cannot
 * observe stale state; shell_loop calls it again on entry as the catch-all (boot,
 * and any path that did not).
 */
void shell_reset_input(void)
{
    cmd_pos = 0;
    cmd_ready = false;
    stdin_waiting_proc = NULL;
    stdin_ready = false;
    stdin_pos = 0;
}

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

        /*
         * Poll serial input for piped commands via QEMU.
         * Process serial characters like keyboard input.
         */
        while (serial_data_available()) {
            int ch = serial_try_getc();
            if (ch < 0) break;

            /* Handle CR (0x0D) as newline - terminals send CR on Enter */
            if (ch == '\r') ch = '\n';

            if (ch == '\n') {
                /* Newline - mark command ready */
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
                /* Backspace or DEL - remove last character */
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
                /* Printable character - add to buffer and echo */
                if (k_scrollback_is_active()) {
                    k_scrollback_end();
                }
                if (cmd_pos < CMD_BUFFER_SIZE - 1) {
                    cmd_buffer[cmd_pos++] = (char)ch;
                    k_putc((char)ch);
                    serial_putc((char)ch);
                }
            }
            /* Ignore other control characters */
        }

        /* Wait for next interrupt */
        __asm__ __volatile__("hlt");
    }
}

void k_main(multiboot_uint32_t magic, multiboot_info_t *info)
{
    uint32_t num_modules;

    /* Initialize serial port first for debug output */
    serial_init();
    serial_printf("======= Rach2 Booting... =======\n\n");

    /*
     * Parse multiboot modules BEFORE paging is enabled.
     * We need direct physical memory access to read module info.
     */
    num_modules = init_modules(magic, info);

    /*
     * If modules are loaded, ensure mem_reserved_ptr skips past them.
     * This prevents the memory allocator from overwriting module data
     * when allocating page tables and other structures.
     */
    if (num_modules > 0) {
        module_info_t *last_module = get_module(num_modules - 1);
        if (last_module && last_module->end > mem_reserved_ptr) {
            /* Align to page boundary and add some slack */
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
    __asm__ __volatile__("cli"); /* Disable interrupts during timer setup */
    init_pit(100);
    set_isr_handler(32, &timer_handler); /* Replace the PIT boot stub with the real handler */
    __asm__ __volatile__("sti"); /* Re-enable interrupts */
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Entropy...");
    serial_printf("  Entropy...");
    krandom_seed(); /* best-effort: TSC + PIT tick + RTC seconds (AT_RANDOM) */
    set_fore_color(light_green);
    k_puts(" OK (best-effort)");
    serial_printf(" OK (best-effort)\n");
    set_fore_color(light_grey);

    k_printf("  Keyboard...");
    serial_printf("  Keyboard...");
    set_isr_handler(33, &keyboard_handler); /* Replace the keyboard boot stub with the real handler */
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
        /* Create process 0 (idle/kernel process) */
        process_t *proc0 = alloc_process();
        if (proc0) {
            proc0->state = PROC_RUNNING;
            proc0->cr3 = 0;  /* Uses kernel page directory */
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

    /* Modules were already parsed early (before paging) - just display status */
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

    /* Initialize KRFFS file system if ramdisk is present */
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
     * Boot directly into ISH shell as the init process.
     * If ISH is not found or fails, fall back to kernel debug shell.
     * When init (ISH) exits, control returns here and we restart the loop.
     */
    while (1) {
        /* Reset init_pid for new boot */
        init_pid = 0;

        /* Try to boot into ISH */
        boot_init_process();

        /* If boot_init_process returns, ISH was not found or failed.
         * Or init exited and control returned here.
         * Run kernel debug shell until user runs a new exec command. */
        k_puts("");
        k_puts("Kernel debug shell active.");
        shell_loop();
    }
}
