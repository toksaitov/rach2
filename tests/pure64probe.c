/**
 * pure64probe: an x86-64 kernel without the i386 ABI refuses i386 programs
 * and calls as Linux does, whether built without it (`make IA32=0`) or booted
 * with `ia32_emulation=0`.
 *
 * The exec of an i386 program, a minimal ELF32 executable of EM_386 that the
 * probe writes into its working directory, must fail with ENOEXEC; on a
 * kernel with the i386 ABI the program runs and exits 42. `int $0x80` with
 * the i386 getpid number must kill the process that makes it with SIGSEGV,
 * since the vector has no gate. A far return to 0x23 must still run 32-bit
 * code, since the GDT keeps the 32-bit code segment, as Linux's does.
 *
 * The probe is built for x86-64 alone. It writes etude's report records
 * (probe_report.h) for the case ETUDE_CASE names, an assertion for each
 * check, as `assertion_id` lists them; a failed check fails its assertion
 * and leaves those after it not run. A child writes no record.
 *
 * Prints "PURE64PROBE OK" and exits 0, or "PURE64PROBE FAIL: " and the reason
 * and exits 1.
 */

#include "probe.h"
#include "probe_report.h"

#define GDT_USER_CODE32 0x23

#define I386_EXIT   1
#define I386_GETPID 20

#define ENOEXEC 8
#define SIGSEGV 11

#define O_WRONLY 0x1
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* The i386 program's file, in the probe's working directory, and the
 * status it exits with where it runs. */
#define I386_PATH   "pure64-i386"
#define I386_STATUS 42

/* Where the i386 program is linked, and the sizes of its ELF32 header and
 * of its one program header. */
#define I386_BASE  0x08048000u
#define EHDR32_LEN 52u
#define PHDR32_LEN 32u

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output. */
static void say(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (long)strlen(s));
}

/* The assertions, a check each, in the order the probe makes them. */
static const char *const assertion_id[] = {"exec-enoexec", "int80-sigsegv", "code32-runs"};

/** Print "PURE64PROBE FAIL: " and `why`, end the report at the assertion that
 * `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("PURE64PROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    sys_exit(1);
}

/** The status of child `pid` once it ends; fail when it cannot be reaped. */
static int child_status(long pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("a child could not be reaped");
    }
    return status;
}

/** Store `v` little-endian in the `n` bytes at `p`. */
static void put(unsigned char *p, unsigned long v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

/*
 * The i386 program: an ELF32 header, one PT_LOAD of the whole file at
 * I386_BASE, readable and executable, and the code after the headers,
 * `movl $1, %eax; movl $42, %ebx; int $0x80`, the i386 exit(42).
 */
static const unsigned char i386_code[] = {0xB8,        I386_EXIT, 0, 0, 0,    0xBB,
                                          I386_STATUS, 0,         0, 0, 0xCD, 0x80};
static unsigned char i386_file[EHDR32_LEN + PHDR32_LEN + sizeof(i386_code)];

/** Fill `i386_file` with the program. */
static void build_i386(void)
{
    unsigned char *e = i386_file, *ph = i386_file + EHDR32_LEN;
    unsigned long len = sizeof(i386_file);
    unsigned int i;

    e[0] = 0x7F;
    e[1] = 'E';
    e[2] = 'L';
    e[3] = 'F';
    e[4] = 1;                                            /* ELFCLASS32 */
    e[5] = 1;                                            /* ELFDATA2LSB */
    e[6] = 1;                                            /* EV_CURRENT */
    put(e + 16, 2, 2);                                   /* e_type ET_EXEC */
    put(e + 18, 3, 2);                                   /* e_machine EM_386 */
    put(e + 20, 1, 4);                                   /* e_version */
    put(e + 24, I386_BASE + EHDR32_LEN + PHDR32_LEN, 4); /* e_entry */
    put(e + 28, EHDR32_LEN, 4);                          /* e_phoff */
    put(e + 40, EHDR32_LEN, 2);                          /* e_ehsize */
    put(e + 42, PHDR32_LEN, 2);                          /* e_phentsize */
    put(e + 44, 1, 2);                                   /* e_phnum */
    put(ph + 0, 1, 4);                                   /* p_type PT_LOAD */
    put(ph + 8, I386_BASE, 4);                           /* p_vaddr */
    put(ph + 12, I386_BASE, 4);                          /* p_paddr */
    put(ph + 16, len, 4);                                /* p_filesz */
    put(ph + 20, len, 4);                                /* p_memsz */
    put(ph + 24, 5, 4);                                  /* p_flags PF_R | PF_X */
    put(ph + 28, 0x1000, 4);                             /* p_align */
    for (i = 0; i < sizeof(i386_code); i++) {
        i386_file[EHDR32_LEN + PHDR32_LEN + i] = i386_code[i];
    }
}

/*
 * call32(entry, stack_top) runs the 32-bit code at `entry` on the stack at
 * `stack_top` and returns the EAX that code ends with. It enters by a far
 * return to GDT_USER_CODE32; the 32-bit code comes back by a far jump to
 * `back64` in the 64-bit code segment, 0x33, which restores the
 * callee-saved registers, whose upper halves 32-bit code may not keep.
 *
 * mode32 returns CS, with 0x10000 added when 0x48 decremented EAX, which it
 * does in 32-bit code and not in 64-bit code, where it is a REX prefix.
 */
long call32(void *entry, void *stack_top);
extern char mode32[];

/* The 32-bit code's stack, below 4 GiB with the rest of the probe, which is
 * linked at 0x400000, and the 64-bit RSP while that code runs. */
static unsigned char stack32[4096] __attribute__((aligned(16)));
unsigned long saved_rsp;

__asm__(".text\n"
        ".code64\n"
        ".globl call32\n"
        "call32:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, saved_rsp(%rip)\n"
        "    movq %rsi, %rsp\n"
        "    pushq $0x23\n"
        "    pushq %rdi\n"
        "    lretq\n"
        "back64:\n"
        "    movq saved_rsp(%rip), %rsp\n"
        "    movslq %eax, %rax\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    ret\n"
        ".code32\n"
        ".globl mode32\n"
        "mode32:\n"
        "    xorl %eax, %eax\n"
        "    .byte 0x48\n"
        "    nop\n"
        "    andl $0x10000, %eax\n"
        "    movl %cs, %ecx\n"
        "    movzwl %cx, %ecx\n"
        "    orl %ecx, %eax\n"
        "    ljmp $0x33, $back64\n"
        ".code64\n");

/** The exec of the i386 program fails with ENOEXEC, in a child, which exits
 * 0 for ENOEXEC, 2 for another error, and I386_STATUS where the program
 * ran. */
static void run_exec(char **envp)
{
    char *argv[2];
    long fd, pid, r;
    int status;

    build_i386();
    fd = syscall3(SYS_OPEN, (long)I386_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        fail("the i386 program could not be created in the working directory");
    }
    if (syscall3(SYS_WRITE, fd, (long)i386_file, (long)sizeof(i386_file)) !=
        (long)sizeof(i386_file)) {
        fail("the i386 program could not be written");
    }
    syscall1(SYS_CLOSE, fd);
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        argv[0] = I386_PATH;
        argv[1] = 0;
        r = syscall3(SYS_EXECVE, (long)I386_PATH, (long)argv, (long)envp);
        sys_exit(r == -ENOEXEC ? 0 : 2);
    }
    status = child_status(pid);
    syscall1(SYS_UNLINK, (long)I386_PATH);
    if ((status & 0x7F) == 0 && ((status >> 8) & 0xFF) == I386_STATUS) {
        fail("the i386 program ran");
    }
    if (status != 0) {
        fail("the exec of the i386 program did not fail with ENOEXEC");
    }
    report_held();
}

/** `int $0x80` kills the child that makes it with SIGSEGV. */
static void run_int80(void)
{
    long pid, r;
    int status;

    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        __asm__ __volatile__("int $0x80"
                             : "=a"(r)
                             : "a"((long)I386_GETPID)
                             : "memory", "rcx", "r8", "r9", "r10", "r11");
        sys_exit(r > 0 ? 3 : 4);
    }
    status = child_status(pid);
    if ((status & 0x7F) == 0) {
        fail(((status >> 8) & 0xFF) == 3 ? "int $0x80 made the i386 getpid"
                                         : "int $0x80 returned an error and did not raise SIGSEGV");
    }
    if ((status & 0x7F) != SIGSEGV) {
        fail("int $0x80 ended the process by a signal other than SIGSEGV");
    }
    report_held();
}

/** A far return to 0x23 runs 32-bit code. */
static void run_code32(void)
{
    if (call32(mode32, stack32 + sizeof(stack32)) != (0x10000 | GDT_USER_CODE32)) {
        fail("code entered by a far return to 0x23 did not run in 32-bit mode with CS 0x23");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the rows in order. */
void probe_main(unsigned long *sp)
{
    char **envp = (char **)(sp + 1) + sp[0] + 1;
    unsigned int i;

    report_begin(report_case(envp, "guest.pure64.build"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    run_exec(envp);
    run_int80();
    run_code32();
    report_end();
    say("PURE64PROBE OK\n");
    sys_exit(0);
}
