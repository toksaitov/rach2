/*
 * dirtest.c - Directory syscall test program for Rach2 OS
 *
 * Tests sys_getcwd and sys_chdir syscalls.
 * Since KRFFS has no directories, only "/" exists.
 * chdir("/"), chdir("."), and chdir("..") should succeed.
 * All other paths should fail.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o dirtest dirtest.c
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT   1
#define SYS_WRITE  4
#define SYS_CHDIR  12
#define SYS_GETCWD 183

/*
 * sys_write - Write to a file descriptor
 */
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

/*
 * sys_exit - Terminate the process
 */
static void sys_exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    __builtin_unreachable();
}

/*
 * sys_chdir - Change current working directory
 * Returns: 0 on success, -1 on error
 */
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

/*
 * sys_getcwd - Get current working directory
 * Returns: buf on success (pointer to cwd), NULL on error
 */
static char *sys_getcwd(char *buf, size_t size)
{
    char *result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_GETCWD), "b"(buf), "c"(size)
        : "memory"
    );
    return result;
}

/*
 * strlen - Calculate string length
 */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/*
 * puts - Print string followed by newline
 */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/*
 * print - Print string without newline
 */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/*
 * _start - ELF entry point
 *
 * Tests getcwd and chdir:
 * 1. Get initial cwd (should be "/")
 * 2. Test chdir(".") - should succeed
 * 3. Test chdir("/") - should succeed
 * 4. Test chdir("..") - should succeed (parent of / is /)
 * 5. Test chdir("nonexistent") - should fail
 * 6. Test chdir("hello.txt") - should fail (it's a file, not a dir)
 */
void _start(void)
{
    char cwdbuf[64];
    char *cwd;
    int result;
    int passed = 0;
    int failed = 0;

    puts("Directory syscall test starting...");
    puts("");

    /* Test 1: getcwd initial value */
    print("Test 1: getcwd()... ");
    cwd = sys_getcwd(cwdbuf, sizeof(cwdbuf));
    if (cwd != (char *)0 && cwd[0] == '/') {
        print("OK (cwd=\"");
        print(cwd);
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
    if (cwd != (char *)0 && cwd[0] == '/' && cwd[1] == '\0') {
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
