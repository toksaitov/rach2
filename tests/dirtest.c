/**
 * Exercises the getcwd and chdir syscalls. KRFFS has no directories, so only
 * "/" exists: chdir("/"), chdir("."), and chdir("..") succeed, and every other
 * path fails. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT   1
#define SYS_WRITE  4
#define SYS_CHDIR  12
#define SYS_GETCWD 183

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    __builtin_unreachable();
}

static int sys_chdir(const char *path)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_CHDIR), "b"(path)
        : "memory"
    );
    return result;
}

/**
 * Returns the path length including the NUL on success (the path is written
 * into `buf`), or a negative errno on error. This is the Linux getcwd(2) ABI.
 */
static int sys_getcwd(char *buf, size_t size)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_GETCWD), "b"(buf), "c"(size)
        : "memory"
    );
    return result;
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

/**
 * ELF entry point. Runs the getcwd and chdir cases, tallies passes and
 * failures, prints the count, and exits 0 only if all pass.
 */
void _start(void)
{
    char cwdbuf[64];
    int cwd;
    int result;
    int passed = 0;
    int failed = 0;

    puts("Directory syscall test starting...");
    puts("");

    /* Test 1: getcwd initial value */
    print("Test 1: getcwd()... ");
    cwd = sys_getcwd(cwdbuf, sizeof(cwdbuf));
    if (cwd > 0 && cwdbuf[0] == '/') {
        print("OK (cwd=\"");
        print(cwdbuf);
        puts("\")");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 2: chdir(".") - stay in current directory */
    print("Test 2: chdir(\".\")... ");
    result = sys_chdir(".");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 3: chdir("/") - go to root */
    print("Test 3: chdir(\"/\")... ");
    result = sys_chdir("/");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 4: chdir("..") - parent of root is root */
    print("Test 4: chdir(\"..\")... ");
    result = sys_chdir("..");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 5: getcwd after chdir operations */
    print("Test 5: getcwd() after chdir... ");
    cwd = sys_getcwd(cwdbuf, sizeof(cwdbuf));
    if (cwd > 0 && cwdbuf[0] == '/' && cwdbuf[1] == '\0') {
        puts("OK (still \"/\")");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 6: chdir("nonexistent") - should fail (ENOENT) */
    print("Test 6: chdir(\"nonexistent\")... ");
    result = sys_chdir("nonexistent");
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    /* Test 7: chdir("hello.txt") - should fail (ENOTDIR - it's a file) */
    print("Test 7: chdir(\"hello.txt\")... ");
    result = sys_chdir("hello.txt");
    if (result < 0) {
        puts("OK (failed as expected - file, not dir)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    puts("");
    print("Results: ");
    /* Print passed count */
    {
        char num = '0' + passed;
        sys_write(STDOUT_FILENO, &num, 1);
    }
    print(" passed, ");
    {
        char num = '0' + failed;
        sys_write(STDOUT_FILENO, &num, 1);
    }
    puts(" failed");

    if (failed == 0) {
        puts("All directory syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
