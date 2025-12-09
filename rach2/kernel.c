#include "vendor/multiboot.h"

#include "sys/kio.h"
#include "sys/serial.h"
#include "sys/syscall.h"
#include "sys/tables/gdt.h"
#include "sys/tables/tss.h"
#include "sys/tables/idt.h"
#include "sys/isr.h"
#include "sys/mm/heap.h"
#include "sys/mm/paging.h"
#include "sys/proc/process.h"
#include "sys/proc/sched.h"
#include "sys/pit.h"
#include "sys/kbd.h"
#include "sys/display/display.h"
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

static volatile uint32_t ticks = 0;

static void run_self_tests(void);
static void shell_loop(void);
static void process_command(const char *cmd);
static void timer_handler(isr_params_t params);
static void keyboard_handler(isr_params_t params);
static bool str_equal(const char *a, const char *b);

static bool test_memory(void);
static bool test_strings(void);
static void test_display(void);
static bool test_timer(void);
static bool test_syscall(void);
static bool test_usermode(void);
static void test_switch(void);
static void test_preempt(void);

static void timer_handler(isr_params_t params)
{
    (void) params;

    ++ticks;

    /* Call the scheduler on every timer tick */
    schedule();
}

static void keyboard_handler(isr_params_t params)
{
    uint8_t scancode;
    char c;

    (void) params;

    scancode = inb(0x60);

    /* Ignore key release events (high bit set) */
    if (scancode & 0x80) return;

    c = EN_us[scancode & 0x7F];

    if (c == '\n') {
        cmd_buffer[cmd_pos] = '\0';
        cmd_ready = true;
        k_putc('\n');
    } else if (c == '\b') {
        if (cmd_pos > 0) {
            --cmd_pos;
            k_putc('\b');
            k_putc(' ');
            k_putc('\b');
        }
    } else if (c != 0 && cmd_pos < CMD_BUFFER_SIZE - 1) {
        cmd_buffer[cmd_pos++] = c;
        k_putc(c);
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

    start_ticks = ticks;

    /* Wait for a few timer interrupts using hlt */
    for (i = 0; i < 5; ++i) {
        __asm__ __volatile__("hlt");
    }

    return ticks > start_ticks;
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

static void run_self_tests(void)
{
    k_puts("");
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

    test_display();

    k_puts("");
    k_puts("================================");
    serial_printf("================================\n");
}

static void process_command(const char *cmd)
{
    if (str_equal(cmd, "help")) {
        k_puts("Available commands:");
        k_puts("  help     - Show this help message");
        k_puts("  info     - Display system information");
        k_puts("  test     - Run self-tests again");
        k_puts("  clear    - Clear the screen");
        k_puts("  switch   - Test manual context switching");
        k_puts("  preempt  - Test preemptive round-robin scheduling");
        k_puts("  usermode - Test ring 3 execution");
        k_puts("  fault    - Trigger a page fault (panic)");
        k_puts("  halt     - Halt the CPU");
    } else if (str_equal(cmd, "info")) {
        k_printf("Memory pointer:  0x%X\n", mem_reserved_ptr);
        k_printf("Timer ticks:     %d\n", ticks);
        k_printf("Kernel end:      0x%X\n", (uintptr_t) &k_phys_mem_end);
    } else if (str_equal(cmd, "test")) {
        run_self_tests();
    } else if (str_equal(cmd, "clear")) {
        k_cls();
    } else if (str_equal(cmd, "switch")) {
        test_switch();
    } else if (str_equal(cmd, "preempt")) {
        test_preempt();
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

static void shell_loop(void)
{
    k_puts("Type 'help' for available commands.");
    k_puts("");
    k_printf("> ");

    while (1) {
        if (cmd_ready) {
            cmd_ready = false;
            process_command(cmd_buffer);
            cmd_pos = 0;
            k_printf("> ");
        }
        /* Wait for next interrupt */
        __asm__ __volatile__("hlt");
    }
}

void k_main(multiboot_uint32_t magic, multiboot_info_t *info)
{
    (void)magic;
    (void)info;

    /* Initialize serial port first for debug output */
    serial_init();
    serial_printf("\n");
    serial_printf("========================================\n");
    serial_printf("Serial: Rach2 booting...\n");
    serial_printf("========================================\n");

    k_cls();

    set_fore_color(light_cyan);
    k_puts("===================================");
    k_puts("           Rach2 OS Test           ");
    k_puts("===================================");
    set_fore_color(light_grey);
    k_puts("");

    k_puts("Initializing system components...");
    k_puts("");
    serial_printf("Initializing system components...\n");

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
    init_paging();
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Timer (100 Hz)...");
    serial_printf("  Timer (100 Hz)...");
    __asm__ __volatile__("cli"); /* Disable interrupts during timer setup */
    init_pit(100);
    register_isr_handler(32, &timer_handler); /* Override with silent handler */
    __asm__ __volatile__("sti"); /* Re-enable interrupts */
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
    set_fore_color(light_grey);

    k_printf("  Keyboard...");
    serial_printf("  Keyboard...");
    register_isr_handler(33, &keyboard_handler);
    set_fore_color(light_green);
    k_puts(" OK");
    serial_printf(" OK\n");
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

    k_puts("");
    k_puts("Initialization complete.");
    serial_printf("Initialization complete.\n");

    run_self_tests();

    k_puts("");
    shell_loop();
}
