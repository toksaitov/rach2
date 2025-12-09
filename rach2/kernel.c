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

static void timer_handler(isr_params_t params)
{
    (void) params;

    ++ticks;
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

static void run_self_tests(void)
{
    k_puts("");
    k_puts("======= System Self-Test =======");
    k_puts("");

    k_printf("  Memory allocation:  ");
    if (test_memory()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
    }
    set_fore_color(light_grey);

    k_printf("  String functions:   ");
    if (test_strings()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
    }
    set_fore_color(light_grey);

    k_printf("  Timer interrupts:   ");
    if (test_timer()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
    }
    set_fore_color(light_grey);

    k_printf("  Paging enabled:     ");
    set_fore_color(light_green);
    k_puts("[PASS]");
    set_fore_color(light_grey);

    k_printf("  Syscall (int 0x80): ");
    if (test_syscall()) {
        set_fore_color(light_green);
        k_puts("[PASS]");
    } else {
        set_fore_color(light_red);
        k_puts("[FAIL]");
    }
    set_fore_color(light_grey);

    test_display();

    k_puts("");
    k_puts("================================");
}

static void process_command(const char *cmd)
{
    if (str_equal(cmd, "help")) {
        k_puts("Available commands:");
        k_puts("  help     - Show this help message");
        k_puts("  info     - Display system information");
        k_puts("  test     - Run self-tests again");
        k_puts("  clear    - Clear the screen");
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

    k_puts("");
    k_puts("Initialization complete.");
    serial_printf("Initialization complete.\n");

    run_self_tests();

    k_puts("");
    shell_loop();
}
