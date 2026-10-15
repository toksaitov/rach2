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
 * dynamic loader and crt1 read on Linux.
 */

#include "probe.h"

typedef unsigned int u32;
typedef unsigned char u8;

#define STDOUT 1

#define AT_NULL        0
#define AT_PHDR        3
#define AT_PHENT       4
#define AT_PHNUM       5
#define AT_PAGESZ      6
#define AT_ENTRY       9
#define AT_CLKTCK      17
#define AT_SECURE      23
#define AT_RANDOM      25
#define AT_MINSIGSTKSZ 51

/* The size of a program header, which AT_PHENT reports: ELF32's on i386 and
 * ELF64's on x86-64. */
#if defined __x86_64__
#define PHENT_SIZE 56
#define PHENT_TEXT "56"
#else
#define PHENT_SIZE 32
#define PHENT_TEXT "32"
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

/** Count a failure and print `msg` when `cond` is false. */
static void check(int cond, const char *msg)
{
    if (!cond) {
        puts_("FAIL: ");
        puts_(msg);
        puts_("\n");
        fails++;
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, entered with `sp` at argc. It reads at most 64 auxv pairs.
 * AT_MINSIGSTKSZ must exceed 512 bytes: a signal frame has the
 * floating-point state, 512 bytes at the least, and the registers and the
 * siginfo above it.
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
    unsigned long pagesz = 0;
    unsigned long phnum = 0;
    unsigned long phent = 0;
    unsigned long phdr = 0;
    unsigned long clktck = 0;
    unsigned long secure = 1;
    unsigned long entry = 0;
    unsigned long minsigstksz = 0;

    puts_("auxvprobe: starting\n");

    /* The initial stack pointer is 16-byte aligned. */
    check(((unsigned long)sp & 0xF) == 0, "esp not 16-byte aligned");

    /* argc and argv[]. */
    argc = (int)sp[0];
    argv = (char **)&sp[1];
    puts_("argc=");
    put_u32((u32)argc);
    puts_("\n");
    check(argc >= 1, "argc < 1");
    for (i = 0; i < argc; i++) {
        puts_("  argv: ");
        puts_(argv[i]);
        puts_("\n");
    }
    check(argv[argc] == 0, "argv not NULL-terminated");

    /* envp[] begins just past argv's NULL. */
    envp = (char **)&argv[argc + 1];
    while (envp[envc]) {
        puts_("  envp: ");
        puts_(envp[envc]);
        puts_("\n");
        envc++;
    }
    check(envc >= 1, "no envp entries");

    /* The auxv pairs begin just past envp's NULL. */
    auxv = (unsigned long *)&envp[envc + 1];
    for (i = 0; i < 128; i += 2) {
        unsigned long type = auxv[i];
        unsigned long val = auxv[i + 1];

        if (type == AT_NULL) {
            saw_null = 1;
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
    check(saw_null, "auxv not AT_NULL-terminated");
    check(pagesz == 4096, "AT_PAGESZ != 4096");
    check(phnum != 0, "AT_PHNUM == 0");
    check(phdr != 0, "AT_PHDR == 0");
    check(phent == PHENT_SIZE, "AT_PHENT != " PHENT_TEXT);
    check(clktck == 100, "AT_CLKTCK != 100");
    check(saw_secure && secure == 0, "AT_SECURE missing or nonzero");
    check(entry != 0, "AT_ENTRY == 0");
    check(minsigstksz > 512, "AT_MINSIGSTKSZ missing or smaller than a frame");

    /* AT_RANDOM: 16 bytes in the initial stack page, not all zero. */
    check(random_ptr != 0, "no AT_RANDOM");
    check(((unsigned long)random_ptr & ~0xFFFul) == ((unsigned long)sp & ~0xFFFul),
          "AT_RANDOM not in the initial stack page");
    if (random_ptr) {
        u8 *r = random_ptr;
        u32 acc = 0;

        puts_("AT_RANDOM=");
        for (i = 0; i < 16; i++) {
            put_byte(r[i]);
            acc |= r[i];
        }
        puts_("\n");
        check(acc != 0, "AT_RANDOM all zero");
    }

    if (fails == 0) {
        puts_("auxvprobe: ALL OK\n");
    } else {
        puts_("auxvprobe: FAILED\n");
    }
    sys_exit(fails == 0 ? 0 : 1);
}
