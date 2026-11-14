/**
 * auxvprobe: the System V initial process stack the kernel gives a new
 * program.
 *
 * Walks its own initial stack as a C runtime would, in words of a long, 4
 * bytes on i386 and 8 on x86-64: argc, the argv[] pointers and their NULL,
 * the envp[] pointers and their NULL, then the auxv {type,value} pairs
 * terminated by AT_NULL. It prints what it finds, the 16 AT_RANDOM bytes
 * included, and checks the layout and the values of the auxv entries; it
 * prints "auxvprobe: ALL OK" and exits 0 when every check passes, and
 * "auxvprobe: FAILED" and exits 1 otherwise. This is the same stack the
 * dynamic loader and crt1 read on Linux. Its entry also checks that every
 * general register but the stack pointer is 0, since i386's crt1 registers
 * a nonzero EDX with atexit.
 *
 * Two checks depend on the kernel, whose architecture ETUDE_KERNEL in the
 * environment names, or the probe's own where it is unset: `sysinfo`, that
 * the i686 kernel gives no AT_SYSINFO_EHDR, having no vDSO, while the x86-64
 * kernel gives it page-aligned, and to an i386 program also AT_SYSINFO, the
 * vDSO's __kernel_vsyscall, inside the vDSO's page, as Linux's 64-bit
 * kernel does; and `machine`, that uname(2)'s machine is the kernel's
 * architecture, x86_64 for an i386 program too.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each check, which `check` names: `registers-zero`,
 * `stack-aligned`, `argv`, `envp`, `auxv-end`, `pagesz`, `phdr`, `phent`,
 * `clktck`, `secure`, `entry`, `minsigstksz`, `hwcap`, `hwcap2`, `flags`,
 * `platform`, `sysinfo`, `machine`, `random` and `random-page`, each failed
 * with its first failed check.
 */

#include "probe.h"
#include "probe_report.h"

typedef unsigned int u32;
typedef unsigned char u8;

#define STDOUT 1

#define AT_NULL         0
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_FLAGS        8
#define AT_ENTRY        9
#define AT_PLATFORM     15
#define AT_HWCAP        16
#define AT_CLKTCK       17
#define AT_SECURE       23
#define AT_RANDOM       25
#define AT_HWCAP2       26
#define AT_SYSINFO      32
#define AT_SYSINFO_EHDR 33
#define AT_MINSIGSTKSZ  51

/* The bytes of a field of struct utsname, and its fields. */
#define UTS_FIELD 65
#define UTS_COUNT 6

/* The size of a program header, which AT_PHENT reports: ELF32's on i386 and
 * ELF64's on x86-64. */
#if defined __x86_64__
#define PHENT_SIZE 56
#define PHENT_TEXT "56"
#define OWN_ARCH   "x86_64"
#else
#define PHENT_SIZE 32
#define PHENT_TEXT "32"
#define OWN_ARCH   "i686"
#endif

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, u32 n)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)n);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Say whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output, with no newline. */
static void puts_(const char *s)
{
    sys_write(STDOUT, s, slen(s));
}

/** Write `v` as "0x" and eight hex digits. */
static void put_u32(u32 v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    }
    buf[10] = '\0';
    puts_(buf);
}

/** Write `b` as two hex digits. */
static void put_byte(u8 b)
{
    static const char hex[] = "0123456789abcdef";
    char buf[3];

    buf[0] = hex[(b >> 4) & 0xF];
    buf[1] = hex[b & 0xF];
    buf[2] = '\0';
    puts_(buf);
}

static int fails;

/** Begin the part of the assertion `id` (probe_report.h), unless it is the
 * part open. */
static void part(const char *id)
{
    static const char *open;
    int i = 0;

    if (open != 0) {
        while (id[i] != '\0' && open[i] == id[i]) {
            i++;
        }
        if (open[i] == id[i]) {
            return;
        }
    }
    report_part(id);
    open = id;
}

/** Count a failure of the assertion `id` and print `msg` when `cond` is
 * false. */
static void check(int cond, const char *id, const char *msg)
{
    part(id);
    if (!cond) {
        puts_("FAIL: ");
        puts_(msg);
        puts_("\n");
        fails++;
        report_miss(msg);
    }
}

void probe_main(unsigned long *sp);

/* The general registers but the stack pointer as the kernel entered `_start`
 * with them, in the order of `entry_reg_names`. */
#if defined __x86_64__
#define ENTRY_REGS 15
unsigned long entry_regs[ENTRY_REGS];
static const char *const entry_reg_names[ENTRY_REGS] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8",
    "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
};

__asm__(".pushsection .text\n"
        ".globl _start\n"
        "_start:\n"
        "    movq %rax, entry_regs(%rip)\n"
        "    movq %rbx, entry_regs+8(%rip)\n"
        "    movq %rcx, entry_regs+16(%rip)\n"
        "    movq %rdx, entry_regs+24(%rip)\n"
        "    movq %rsi, entry_regs+32(%rip)\n"
        "    movq %rdi, entry_regs+40(%rip)\n"
        "    movq %rbp, entry_regs+48(%rip)\n"
        "    movq %r8, entry_regs+56(%rip)\n"
        "    movq %r9, entry_regs+64(%rip)\n"
        "    movq %r10, entry_regs+72(%rip)\n"
        "    movq %r11, entry_regs+80(%rip)\n"
        "    movq %r12, entry_regs+88(%rip)\n"
        "    movq %r13, entry_regs+96(%rip)\n"
        "    movq %r14, entry_regs+104(%rip)\n"
        "    movq %r15, entry_regs+112(%rip)\n"
        "    xorq %rbp, %rbp\n"
        "    movq %rsp, %rdi\n"
        "    andq $-16, %rsp\n"
        "    call probe_main\n"
        "    hlt\n"
        ".popsection\n");
#else
#define ENTRY_REGS 7
unsigned long entry_regs[ENTRY_REGS];
static const char *const entry_reg_names[ENTRY_REGS] = {
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp",
};

__asm__(".pushsection .text\n"
        ".globl _start\n"
        "_start:\n"
        "    movl %eax, entry_regs\n"
        "    movl %ebx, entry_regs+4\n"
        "    movl %ecx, entry_regs+8\n"
        "    movl %edx, entry_regs+12\n"
        "    movl %esi, entry_regs+16\n"
        "    movl %edi, entry_regs+20\n"
        "    movl %ebp, entry_regs+24\n"
        "    mov %esp, %eax\n"
        "    push %eax\n"
        "    call probe_main\n"
        ".popsection\n");
#endif

/** Count a failure for each register of `entry_regs` that is not 0. */
static void check_entry_regs(void)
{
    int i;

    part("registers-zero");
    for (i = 0; i < ENTRY_REGS; i++) {
        if (entry_regs[i] != 0) {
            puts_("FAIL: ");
            puts_(entry_reg_names[i]);
            puts_(" not 0 at entry: ");
            put_u32((u32)entry_regs[i]);
            puts_("\n");
            fails++;
            report_miss("a general register but the stack pointer was not 0 at entry");
        }
    }
}

/**
 * Check that AT_RANDOM, at `random_ptr`, gives 16 bytes just above `end`,
 * the end of the auxiliary vector, or 0 without its AT_NULL, where the
 * kernel puts them below the strings, and not all zero, and print them. The
 * place's check comes between the two of `random`, which is reported once
 * both have run.
 */
static void check_random(const u8 *random_ptr, const u8 *end)
{
    int present = random_ptr != 0;
    u32 acc = 1;
    int i;

    if (!present) {
        puts_("FAIL: no AT_RANDOM\n");
        fails++;
    }
    check(end != 0 && random_ptr >= end && random_ptr < end + 64, "random-page",
          "AT_RANDOM not just above the auxiliary vector");
    if (random_ptr) {
        const u8 *r = random_ptr;

        acc = 0;
        puts_("AT_RANDOM=");
        for (i = 0; i < 16; i++) {
            put_byte(r[i]);
            acc |= r[i];
        }
        puts_("\n");
        if (acc == 0) {
            puts_("FAIL: AT_RANDOM all zero\n");
            fails++;
        }
    }
    if (!present) {
        report_fail("random", "no AT_RANDOM");
    } else if (acc == 0) {
        report_fail("random", "AT_RANDOM all zero");
    } else {
        report_pass("random");
    }
}

/** The kernel's architecture, as ETUDE_KERNEL in the environment `envp`
 * names it, or the probe's own where it is unset. */
static const char *env_kernel(char **envp)
{
    static const char name[] = "ETUDE_KERNEL=";
    int i, n;

    for (i = 0; envp[i] != 0; i++) {
        for (n = 0; name[n] != '\0' && envp[i][n] == name[n]; n++) {
        }
        if (name[n] == '\0') {
            return envp[i] + n;
        }
    }
    return OWN_ARCH;
}

/** The value of the auxv entry of type `type` in `auxv`, which AT_NULL
 * ends within 64 pairs, or 0 where it has none. */
static unsigned long aux_find(const unsigned long *auxv, unsigned long type)
{
    int i;

    for (i = 0; i < 128 && auxv[i] != AT_NULL; i += 2) {
        if (auxv[i] == type) {
            return auxv[i + 1];
        }
    }
    return 0;
}

/** Whether `auxv`, which AT_NULL ends within 64 pairs, has an entry of type
 * `type`. */
static int aux_has(const unsigned long *auxv, unsigned long type)
{
    int i;

    for (i = 0; i < 128 && auxv[i] != AT_NULL; i += 2) {
        if (auxv[i] == type) {
            return 1;
        }
    }
    return 0;
}

/** Run `cpuid` for `leaf` and `sub` and leave EAX, EBX, ECX and EDX in
 * `r`. The instruction writes EBX, which an i386 position-independent build
 * keeps for its global offset table, so EBX is exchanged with ESI around
 * it. */
static void cpuid(u32 leaf, u32 sub, u32 *r)
{
    u32 a = leaf, b, c = sub, d;

#if defined __x86_64__
    __asm__ __volatile__("mov %%rbx, %%rsi\n\tcpuid\n\txchg %%rsi, %%rbx"
                         : "+a"(a), "=S"(b), "+c"(c), "=d"(d));
#else
    __asm__ __volatile__("mov %%ebx, %%esi\n\tcpuid\n\txchg %%esi, %%ebx"
                         : "+a"(a), "=S"(b), "+c"(c), "=d"(d));
#endif
    r[0] = a;
    r[1] = b;
    r[2] = c;
    r[3] = d;
}

/** The feature bits of `cpuid` leaf 1 in EDX, Linux's ELF_HWCAP on x86. */
static u32 cpuid1_edx(void)
{
    u32 r[4];

    cpuid(1, 0, r);
    return r[3];
}

/**
 * Linux's AT_MINSIGSTKSZ on x86, `init_sigframe_size`'s bound: the i386
 * `struct sigframe_ia32`, its largest frame, 732 bytes, or 736 as the
 * `x86_64` kernel lays it out, 15 bytes of the frame's alignment, the state
 * of `fpu__get_fpstate_size` and 63 bytes of the state's alignment, rounded
 * up to 16. The state is the XSAVE image of the components XCR0 enables,
 * whose size `cpuid` leaf 0xD gives in EBX, with its 4-byte end mark, or
 * the 512-byte `fxsave` image, and the 112 bytes of x87 state before it in
 * an i386 frame; or without FXSR those 112 bytes alone.
 */
static unsigned long linux_minsigstksz(int x86_64)
{
    u32 r[4];
    unsigned long state = 112;

    cpuid(1, 0, r);
    if (r[2] & (1u << 27)) { /* OSXSAVE */
        u32 x[4];

        cpuid(0xD, 0, x);
        state = x[1] + 4 + 112;
    } else if (r[3] & (1u << 24)) { /* FXSR */
        state = 512 + 112;
    }
    return ((x86_64 ? 736ul : 732ul) + 15 + state + 63 + 15) & ~15ul;
}

/**
 * Check the entries that describe the processor, as Linux's
 * `create_elf_tables` gives them on x86: AT_HWCAP, `cpuid` leaf 1's EDX;
 * AT_HWCAP2, the features the kernel enabled, there even when 0; AT_FLAGS,
 * 0; and AT_PLATFORM, the name of the program's own architecture, i686 for
 * an i386 program on either kernel.
 */
static void check_cpu(const unsigned long *auxv)
{
    const char *platform = (const char *)aux_find(auxv, AT_PLATFORM);

    check(aux_has(auxv, AT_HWCAP) && aux_find(auxv, AT_HWCAP) == cpuid1_edx(), "hwcap",
          "AT_HWCAP missing or not cpuid leaf 1's EDX");
    check(aux_has(auxv, AT_HWCAP2), "hwcap2", "AT_HWCAP2 missing");
    check(aux_has(auxv, AT_FLAGS) && aux_find(auxv, AT_FLAGS) == 0, "flags",
          "AT_FLAGS missing or nonzero");
    if (platform != 0) {
        puts_("platform=");
        puts_(platform);
        puts_("\n");
    }
    check(platform != 0 && same(platform, OWN_ARCH), "platform",
          "AT_PLATFORM missing or not the program's architecture");
}

/**
 * Check the vDSO's auxv entries, `ehdr` and `sysinfo` or 0 where absent,
 * and uname's machine against `kernel`, the kernel's architecture, as the
 * head comment says.
 */
static void check_kernel(const char *kernel, unsigned long ehdr, unsigned long sysinfo)
{
    char uts[UTS_FIELD * UTS_COUNT];
    int x86_64 = same(kernel, "x86_64");

    puts_("kernel=");
    puts_(kernel);
    puts_("\n");
    if (!x86_64) {
        check(ehdr == 0 && sysinfo == 0, "sysinfo",
              "the i686 kernel gave AT_SYSINFO or AT_SYSINFO_EHDR, but it has no vDSO");
    } else {
        check(ehdr != 0 && (ehdr & 0xFFF) == 0, "sysinfo",
              "the x86-64 kernel gave no page-aligned AT_SYSINFO_EHDR");
#if defined __x86_64__
        check(sysinfo == 0, "sysinfo", "an x86-64 program was given AT_SYSINFO");
#else
        check(sysinfo > ehdr && sysinfo < ehdr + 4096, "sysinfo",
              "AT_SYSINFO is not inside the vDSO's page above AT_SYSINFO_EHDR");
#endif
    }
    if (syscall1(SYS_UNAME, (long)uts) != 0) {
        check(0, "machine", "uname failed");
        return;
    }
    puts_("uname machine=");
    puts_(uts + 4 * UTS_FIELD);
    puts_("\n");
    check(same(uts + 4 * UTS_FIELD, kernel), "machine",
          "uname's machine is not the kernel's architecture");
}

/**
 * The probe, entered with `sp` at argc. It reads at most 64 auxv pairs.
 * AT_MINSIGSTKSZ must be Linux's bound for the processor and the kernel
 * (`linux_minsigstksz`).
 */
void probe_main(unsigned long *sp)
{
    int argc;
    char **argv;
    char **envp;
    unsigned long *auxv;
    int envc = 0;
    int i;
    int saw_null = 0;
    int saw_secure = 0;
    u8 *random_ptr = 0;
    const u8 *auxv_end = 0;
    unsigned long pagesz = 0;
    unsigned long phnum = 0;
    unsigned long phent = 0;
    unsigned long phdr = 0;
    unsigned long clktck = 0;
    unsigned long secure = 1;
    unsigned long entry = 0;
    unsigned long minsigstksz = 0;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.auxvprobe"));
    puts_("auxvprobe: starting\n");

    check_entry_regs();

    /* The initial stack pointer is 16-byte aligned. */
    check(((unsigned long)sp & 0xF) == 0, "stack-aligned", "esp not 16-byte aligned");

    /* argc and argv[]. */
    argc = (int)sp[0];
    argv = (char **)&sp[1];
    puts_("argc=");
    put_u32((u32)argc);
    puts_("\n");
    check(argc >= 1, "argv", "argc < 1");
    for (i = 0; i < argc; i++) {
        puts_("  argv: ");
        puts_(argv[i]);
        puts_("\n");
    }
    check(argv[argc] == 0, "argv", "argv not NULL-terminated");

    /* envp[] begins just past argv's NULL. */
    envp = (char **)&argv[argc + 1];
    while (envp[envc]) {
        puts_("  envp: ");
        puts_(envp[envc]);
        puts_("\n");
        envc++;
    }
    check(envc >= 1, "envp", "no envp entries");

    /* The auxv pairs begin just past envp's NULL. */
    auxv = (unsigned long *)&envp[envc + 1];
    for (i = 0; i < 128; i += 2) {
        unsigned long type = auxv[i];
        unsigned long val = auxv[i + 1];

        if (type == AT_NULL) {
            saw_null = 1;
            auxv_end = (const u8 *)&auxv[i + 2];
            break;
        }
        if (type == AT_RANDOM) {
            random_ptr = (u8 *)val;
        }
        if (type == AT_PAGESZ) {
            pagesz = val;
        }
        if (type == AT_PHNUM) {
            phnum = val;
        }
        if (type == AT_PHDR) {
            phdr = val;
        }
        if (type == AT_PHENT) {
            phent = val;
        }
        if (type == AT_CLKTCK) {
            clktck = val;
        }
        if (type == AT_SECURE) {
            saw_secure = 1;
            secure = val;
        }
        if (type == AT_ENTRY) {
            entry = val;
        }
        if (type == AT_MINSIGSTKSZ) {
            minsigstksz = val;
        }
    }
    check(saw_null, "auxv-end", "auxv not AT_NULL-terminated");
    check(pagesz == 4096, "pagesz", "AT_PAGESZ != 4096");
    check(phnum != 0, "phdr", "AT_PHNUM == 0");
    check(phdr != 0, "phdr", "AT_PHDR == 0");
    check(phent == PHENT_SIZE, "phent", "AT_PHENT != " PHENT_TEXT);
    check(clktck == 100, "clktck", "AT_CLKTCK != 100");
    check(saw_secure && secure == 0, "secure", "AT_SECURE missing or nonzero");
    check(entry != 0, "entry", "AT_ENTRY == 0");
    puts_("minsigstksz=");
    put_u32((u32)minsigstksz);
    puts_(" linux=");
    put_u32((u32)linux_minsigstksz(same(env_kernel(envp), "x86_64")));
    puts_("\n");
    check(minsigstksz == linux_minsigstksz(same(env_kernel(envp), "x86_64")), "minsigstksz",
          "AT_MINSIGSTKSZ missing or not Linux's bound for this processor and kernel");
    check_cpu(auxv);
    check_kernel(env_kernel(envp), aux_find(auxv, AT_SYSINFO_EHDR), aux_find(auxv, AT_SYSINFO));

    check_random(random_ptr, auxv_end);

    report_finish();
    if (fails == 0) {
        puts_("auxvprobe: ALL OK\n");
    } else {
        puts_("auxvprobe: FAILED\n");
    }
    sys_exit(fails == 0 ? 0 : 1);
}
