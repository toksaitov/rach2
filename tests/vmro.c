/**
 * Probes that a PROT_READ-only page faults on write.
 *
 * Maps an anonymous page with PROT_READ only, reads it (must succeed and read
 * zero), then writes to it, which must fault as a protection violation, since
 * the demand-paging handler honors the VMA's protection in the PTE. The
 * process must die and the kernel return to its shell; the "still alive" line
 * must NEVER appear. Companion to vmfault (an address in no VMA at all).
 *
 * Run as a standalone boot; the harness then checks the kernel survived.
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT  1
#define SYS_WRITE 4
#define SYS_MMAP2 192

#define PAGE 4096u

#define PROT_READ     0x1
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

/** mmap2 anonymous; addr=0, fd=-1, pgoffset=0 (in %ebp, saved/restored). */
static unsigned sys_mmap2_anon(unsigned len, int prot)
{
    unsigned result;
    __asm__ __volatile__("push %%ebp\n\t"
                         "xor %%ebp, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp\n\t"
                         : "=a"(result)
                         : "0"(SYS_MMAP2), "b"(0), "c"(len), "d"(prot),
                           "S"(MAP_PRIVATE | MAP_ANONYMOUS), "D"(-1)
                         : "memory");
    return result;
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

void _start(void)
{
    unsigned p = sys_mmap2_anon(PAGE, PROT_READ);
    volatile unsigned char *ro = (volatile unsigned char *)p;

    if (p >= 0xFFFFF001u) {
        puts("vmro: ERROR mmap PROT_READ failed");
        sys_exit(1);
    }

    /* A read is allowed and the lazy page reads zero. */
    if (ro[0] != 0) {
        puts("vmro: ERROR read-only page did not read zero");
        sys_exit(1);
    }

    puts("vmro: about to write a read-only page");

    ro[0] = 0xFF; /* protection violation: the kernel must kill us */

    puts("vmro: ERROR still alive after writing read-only page");
    sys_exit(0);
}
