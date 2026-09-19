/**
 * Exercises sys_read from stdin (fd 0): blocking, line buffering, and a
 * short-count read into a small buffer. Each test prompts, reads a line,
 * and echoes it back.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return result;
}

static int sys_read(int fd, void *buf, size_t count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_READ), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

static void print_num(int n)
{
    char buf[12];
    int i = 0;
    int neg = 0;

    if (n < 0) {
        neg = 1;
        n = -n;
    }

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }

    if (neg) {
        sys_write(STDOUT_FILENO, "-", 1);
    }

    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

/* Neither a character a line would hold nor zero, so a copy that ran past the
 * count it was given shows up whatever it copied. */
#define FILL_BYTE '\xA5'

void _start(void)
{
    char buf[128];
    int n, i;
    int passed = 0;
    int failed = 0;

    puts("Stdin read syscall test starting...");
    puts("");

    /* Test 1: Basic stdin read */
    print("Test 1: read from stdin (type something and press Enter)...\n");
    print("> ");
    n = sys_read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (n > 0 && buf[n - 1] == '\n') {
            buf[n - 1] = '\0';
            n--;
        }
        print("Read ");
        print_num(n);
        print(" bytes: \"");
        print(buf);
        puts("\"");
        puts("Test 1: OK");
        passed++;
    } else if (n == 0) {
        puts("Read returned 0 (EOF or empty)");
        puts("Test 1: FAILED (stdin not working)");
        failed++;
    } else {
        puts("Read returned error");
        puts("Test 1: FAILED");
        failed++;
    }

    /* Test 2: Second read to verify repeated reads work */
    puts("");
    print("Test 2: second read (type another line)...\n");
    print("> ");
    n = sys_read(STDIN_FILENO, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (n > 0 && buf[n - 1] == '\n') {
            buf[n - 1] = '\0';
            n--;
        }
        print("Read ");
        print_num(n);
        print(" bytes: \"");
        print(buf);
        puts("\"");
        puts("Test 2: OK");
        passed++;
    } else if (n == 0) {
        puts("Read returned 0");
        puts("Test 2: FAILED");
        failed++;
    } else {
        puts("Read returned error");
        puts("Test 2: FAILED");
        failed++;
    }

    /* Test 3: Small buffer read. The count the kernel returns is not the
     * whole claim: a kernel that copied the rest of the line past the fifth
     * byte and still returned 5 would be caught by nothing the count can
     * say, so the rest of the buffer is filled with a byte of our own first
     * and required to still be there afterwards. */
    puts("");
    print("Test 3: small buffer read (5 bytes max, type a longer line)...\n");
    print("> ");
    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = FILL_BYTE;
    }
    n = sys_read(STDIN_FILENO, buf, 5);
    if (n > 0) {
        int spilled = 0;

        for (i = n; i < (int)sizeof(buf); i++) {
            if (buf[i] != FILL_BYTE) {
                spilled = 1;
            }
        }
        buf[n] = '\0';
        print("Read ");
        print_num(n);
        print(" bytes: \"");
        print(buf);
        puts("\"");
        if (n > 5) {
            puts("Test 3: FAILED (read more than buffer size)");
            failed++;
        } else if (spilled) {
            puts("Test 3: FAILED (wrote past the size it was given)");
            failed++;
        } else {
            puts("Test 3: OK (correctly limited to buffer size)");
            passed++;
        }
    } else {
        puts("Read returned 0 or error");
        puts("Test 3: FAILED");
        failed++;
    }

    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All stdin syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
