/*
 * stattest.c - File stat syscall test program for Rach2 OS
 *
 * Tests sys_stat64 syscall by checking file metadata.
 * Verifies that stat works on existing files and fails on nonexistent ones.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o stattest stattest.c
 */

typedef unsigned int size_t;
typedef unsigned int mode_t;
typedef unsigned int ino_t;
typedef unsigned int dev_t;
typedef unsigned int nlink_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int blksize_t;
typedef unsigned int blkcnt_t;
typedef int time_t;
typedef long long off64_t;
typedef unsigned long long ino64_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT   1
#define SYS_WRITE  4
#define SYS_STAT64 195

/* File type bits from mode */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000

/*
 * stat64 structure - matches 32-bit Linux stat64
 * This is the structure filled by sys_stat64
 */
struct stat64 {
    unsigned long long st_dev;
    unsigned char      __pad0[4];
    unsigned long      __st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned long      st_uid;
    unsigned long      st_gid;
    unsigned long long st_rdev;
    unsigned char      __pad3[4];
    long long          st_size;
    unsigned long      st_blksize;
    unsigned long long st_blocks;
    unsigned long      st_atime;
    unsigned long      st_atime_nsec;
    unsigned long      st_mtime;
    unsigned long      st_mtime_nsec;
    unsigned long      st_ctime;
    unsigned long      st_ctime_nsec;
    unsigned long long st_ino;
};

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
 * sys_stat64 - Get file status
 * Returns: 0 on success, -1 on error
 */
static int sys_stat64(const char *path, struct stat64 *buf)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_STAT64), "b"(path), "c"(buf)
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
 * print_num - Print a decimal number (unsigned, up to 32-bit)
 */
static void print_num(unsigned int n)
{
    char buf[12];
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }

    /* Reverse and print */
    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

/*
 * print_size - Print file size (uses 32-bit for simplicity)
 * File sizes in our toy OS are small, so 32-bit is sufficient.
 */
static void print_size(long long n)
{
    /* Just use the low 32 bits - our files are small */
    print_num((unsigned int)n);
}

/*
 * _start - ELF entry point
 *
 * Tests stat64:
 * 1. stat("hello.txt") - should succeed, print size
 * 2. stat("/") - should succeed (root directory)
 * 3. stat("nonexistent") - should fail
 */
void _start(void)
{
    struct stat64 st;
    int result;
    int passed = 0;
    int failed = 0;

    puts("Stat syscall test starting...");
    puts("");

    /* Test 1: stat existing file */
    print("Test 1: stat(\"hello.txt\")... ");
    result = sys_stat64("hello.txt", &st);
    if (result == 0) {
        puts("OK");
        /* Print detailed info */
        print("       size=");
        print_size(st.st_size);
        print(" bytes, mode=0");
        /* Print mode in octal */
        {
            unsigned int m = st.st_mode;
            char octal[8];
            int j = 0;
            do {
                octal[j++] = '0' + (m & 7);
                m >>= 3;
            } while (m > 0);
            while (j > 0) {
                sys_write(STDOUT_FILENO, &octal[--j], 1);
            }
        }
        print(", nlink=");
        print_num(st.st_nlink);
        print(", ino=");
        print_num((unsigned int)st.st_ino);
        puts("");
        passed++;

        /* Verify it's a regular file */
        print("       type: ");
        if ((st.st_mode & S_IFMT) == S_IFREG) {
            puts("regular file (OK)");
        } else {
            puts("not a regular file (unexpected)");
        }
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 2: stat root directory */
    print("Test 2: stat(\"/\")... ");
    result = sys_stat64("/", &st);
    if (result == 0) {
        puts("OK");
        print("       mode=0");
        {
            unsigned int m = st.st_mode;
            char octal[8];
            int j = 0;
            do {
                octal[j++] = '0' + (m & 7);
                m >>= 3;
            } while (m > 0);
            while (j > 0) {
                sys_write(STDOUT_FILENO, &octal[--j], 1);
            }
        }
        print(", nlink=");
        print_num(st.st_nlink);
        print(", ino=");
        print_num((unsigned int)st.st_ino);
        puts("");
        passed++;

        /* Verify it's a directory */
        print("       type: ");
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            puts("directory (OK)");
        } else {
            puts("not a directory (unexpected)");
        }
    } else {
        /* If root stat not implemented, that's acceptable for now */
        puts("FAILED (or not implemented for root)");
        failed++;
    }

    /* Test 3: stat nonexistent file */
    print("Test 3: stat(\"nonexistent\")... ");
    result = sys_stat64("nonexistent", &st);
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    /* Test 4: stat with leading slash */
    print("Test 4: stat(\"/hello.txt\")... ");
    result = sys_stat64("/hello.txt", &st);
    if (result == 0) {
        puts("OK");
        print("       size=");
        print_size(st.st_size);
        print(" bytes, nlink=");
        print_num(st.st_nlink);
        print(", ino=");
        print_num((unsigned int)st.st_ino);
        puts("");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    puts("");
    print("Results: ");
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
        puts("All stat syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
