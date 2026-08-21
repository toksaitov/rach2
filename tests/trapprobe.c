/**
 * Probes that an unhandled CPU exception in user mode is contained, not fatal.
 *
 * Two children fault on purpose: one executes an invalid opcode (#UD), one
 * divides by zero (#DE). The kernel has a dedicated handler only for the page
 * fault; every other CPU exception shares the default handler, which must kill
 * just the faulting process with the signal its cause maps to (SIGILL for #UD,
 * SIGFPE for #DE) and leave the parent and the kernel running. Without it an
 * unhandled exception returns into the faulting instruction, re-faults, and
 * triple-faults the machine with no diagnostic. The probe reaps each child and
 * checks the wait status has the expected signal.
 *
 * Prints "TRAPPROBE OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_WRITE   4
#define SYS_WAITPID 7

#define SIGILL 4
#define SIGFPE 8

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK) : "memory");
    return r;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                         : "memory");
    return r;
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

/* An invalid opcode (#UD, vector 6) the assembler emits verbatim. */
static void raise_invalid_opcode(void)
{
    __asm__ __volatile__("ud2");
}

/* An unsigned divide of 1 by 0 (#DE, vector 0); volatile-free but unfoldable
 * because the divisor is zeroed in a register the compiler cannot trace. */
static void raise_divide_error(void)
{
    __asm__ __volatile__("xorl %%ecx, %%ecx\n\t"
                         "xorl %%edx, %%edx\n\t"
                         "movl $1, %%eax\n\t"
                         "divl %%ecx"
                         :
                         :
                         : "eax", "ecx", "edx", "cc");
}

/*
 * Fork a child that runs `fault`, reap it, and return whether it was killed by
 * `expect_signo`. The Linux wait status has the terminating signal in its
 * low seven bits.
 */
static int faulted_with(void (*fault)(void), int expect_signo)
{
    int pid, status;

    pid = sys_fork();
    if (pid == 0) {
        fault();
        sys_exit(0); /* a contained fault never reaches here */
    }
    if (pid < 0) {
        return 0;
    }

    status = -1;
    if (sys_waitpid(pid, &status, 0) < 0) {
        return 0;
    }
    return (status & 0x7f) == expect_signo;
}

void _start(void)
{
    if (!faulted_with(raise_invalid_opcode, SIGILL)) {
        puts("trapprobe: #UD was not contained as SIGILL");
        sys_exit(1);
    }
    if (!faulted_with(raise_divide_error, SIGFPE)) {
        puts("trapprobe: #DE was not contained as SIGFPE");
        sys_exit(2);
    }

    puts("TRAPPROBE OK");
    sys_exit(0);
}
