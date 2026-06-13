/*
 * auxvprobe.c - Phase 1 probe: verify the SysV/i386 initial process stack.
 *
 * A freestanding program (no libc) that walks its OWN initial stack exactly as
 * a C runtime would: argc, the argv[] pointers + NULL, the envp[] pointers +
 * NULL, then the auxv {type,value} pairs terminated by AT_NULL. It prints what
 * it finds (including the 16 AT_RANDOM bytes, so the harness can confirm they
 * differ across two boots) and asserts the layout is correct, exiting 0 only if
 * every check passes.
 *
 * Linux/BSD analogue: the initial process stack the dynamic loader / crt1 reads.
 *
 * Build: i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000
 */

typedef unsigned int u32;
typedef unsigned char u8;

#define SYS_EXIT  1
#define SYS_WRITE 4
#define STDOUT    1

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25

static int sys_write(int fd, const void *buf, u32 n)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(n)
                         : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void puts_(const char *s)
{
    sys_write(STDOUT, s, slen(s));
}

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

static void check(int cond, const char *msg)
{
    if (!cond) {
        puts_("FAIL: ");
        puts_(msg);
        puts_("\n");
        fails++;
    }
}

void probe_main(u32 *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n" /* eax = initial esp (points at argc) */
        "   push %eax\n"      /* pass it to probe_main */
        "   call probe_main\n");

void probe_main(u32 *sp)
{
    int argc;
    char **argv;
    char **envp;
    u32 *auxv;
    int envc = 0;
    int i;
    int saw_null = 0;
    int saw_secure = 0;
    u32 *random_ptr = 0;
    u32 pagesz = 0;
    u32 phnum = 0;
    u32 phent = 0;
    u32 phdr = 0;
    u32 clktck = 0;
    u32 secure = 1;
    u32 entry = 0;

    puts_("auxvprobe: starting\n");

    /* The kernel must hand us a 16-byte-aligned ESP. */
    check(((u32)sp & 0xF) == 0, "esp not 16-byte aligned");

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

    /* envp[] sits just past argv's NULL. */
    envp = (char **)&argv[argc + 1];
    while (envp[envc]) {
        puts_("  envp: ");
        puts_(envp[envc]);
        puts_("\n");
        envc++;
    }
    check(envc >= 1, "no envp entries");

    /* The auxv pairs sit just past envp's NULL. */
    auxv = (u32 *)&envp[envc + 1];
    for (i = 0; i < 128; i += 2) {
        u32 type = auxv[i];
        u32 val = auxv[i + 1];

        if (type == AT_NULL) {
            saw_null = 1;
            break;
        }
        if (type == AT_RANDOM) {
            random_ptr = (u32 *)val;
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
    }
    check(saw_null, "auxv not AT_NULL-terminated");
    check(pagesz == 4096, "AT_PAGESZ != 4096");
    check(phnum != 0, "AT_PHNUM == 0");
    check(phdr != 0, "AT_PHDR == 0");
    check(phent == 32, "AT_PHENT != 32");
    check(clktck == 100, "AT_CLKTCK != 100");
    check(saw_secure && secure == 0, "AT_SECURE missing or nonzero");
    check(entry != 0, "AT_ENTRY == 0");

    /* AT_RANDOM: print the 16 bytes (the harness compares across two boots) and
     * confirm they are not all zero. */
    check(random_ptr != 0, "no AT_RANDOM");
    check(((u32)random_ptr & ~0xFFFu) == ((u32)sp & ~0xFFFu),
          "AT_RANDOM not in the initial stack page");
    if (random_ptr) {
        u8 *r = (u8 *)random_ptr;
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
