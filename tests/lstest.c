/*
 * lstest.c - Directory listing (getdents64) syscall test for Rach2 OS
 *
 * Tests sys_getdents64 syscall for listing directory entries.
 * Since KRFFS is a flat filesystem, all files appear in root "/".
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o lstest lstest.c
 */

typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT      1
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_GETDENTS64 220

/* Open flags */
#define O_RDONLY   0
#define O_DIRECTORY 0200000  /* Must be a directory */

/* Directory entry types (d_type values) */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/*
 * Linux dirent64 structure
 * This is the format returned by getdents64 syscall
 */
struct linux_dirent64 {
    uint64_t       d_ino;     /* 64-bit inode number */
    int64_t        d_off;     /* 64-bit offset to next structure */
    uint16_t       d_reclen;  /* Size of this dirent */
    uint8_t        d_type;    /* File type */
    char           d_name[];  /* Filename (null-terminated) */
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
 * sys_open - Open a file
 */
static int sys_open(const char *pathname, int flags)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_OPEN), "b"(pathname), "c"(flags)
        : "memory"
    );
    return result;
}

/*
 * sys_close - Close a file descriptor
 */
static int sys_close(int fd)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_CLOSE), "b"(fd)
        : "memory"
    );
    return result;
}

/*
 * sys_getdents64 - Get directory entries
 * fd: file descriptor of opened directory
 * dirp: buffer to store directory entries
 * count: size of buffer in bytes
 * Returns: number of bytes read, 0 at end of directory, -1 on error
 */
static int sys_getdents64(int fd, void *dirp, unsigned int count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_GETDENTS64), "b"(fd), "c"(dirp), "d"(count)
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
 * print_num - Print a number (simple version for small numbers)
 */
static void print_num(int n)
{
    if (n >= 10) {
        print_num(n / 10);
    }
    char c = '0' + (n % 10);
    sys_write(STDOUT_FILENO, &c, 1);
}

/*
 * print_hex - Print a number in hex
 */
static void print_hex(uint32_t n)
{
    const char *hex = "0123456789abcdef";
    char buf[9];
    int i;

    for (i = 7; i >= 0; i--) {
        buf[i] = hex[n & 0xF];
        n >>= 4;
    }
    buf[8] = '\0';
    print("0x");
    print(buf);
}

/*
 * get_type_name - Return string name for d_type
 */
static const char *get_type_name(uint8_t d_type)
{
    switch (d_type) {
    case DT_REG:  return "REG";
    case DT_DIR:  return "DIR";
    case DT_LNK:  return "LNK";
    case DT_CHR:  return "CHR";
    case DT_BLK:  return "BLK";
    case DT_FIFO: return "FIFO";
    case DT_SOCK: return "SOCK";
    default:      return "???";
    }
}

/*
 * _start - ELF entry point
 *
 * Tests getdents64 syscall:
 * 1. Open "/" directory
 * 2. Call getdents64 to read entries
 * 3. Parse and print each entry
 * 4. Call getdents64 again to verify end-of-directory
 * 5. Close directory fd
 */
void _start(void)
{
    char buf[1024];  /* Buffer for directory entries */
    struct linux_dirent64 *d;
    int fd;
    int nread;
    int pos;
    int passed = 0;
    int failed = 0;
    int file_count = 0;

    puts("Getdents64 (directory listing) syscall test starting...");
    puts("");

    /* Test 1: Open root directory "/" */
    print("Test 1: open(\"/\", O_RDONLY)... ");
    fd = sys_open("/", O_RDONLY);
    if (fd >= 0) {
        print("OK (fd=");
        print_num(fd);
        puts(")");
        passed++;
    } else {
        print("FAILED (ret=");
        print_num(fd);
        puts(")");
        failed++;
        goto done;
    }

    /* Test 2: Read directory entries with getdents64 */
    print("Test 2: getdents64(fd, buf, 1024)... ");
    nread = sys_getdents64(fd, buf, sizeof(buf));
    if (nread > 0) {
        print("OK (");
        print_num(nread);
        puts(" bytes)");
        passed++;
    } else if (nread == 0) {
        puts("OK (empty directory)");
        passed++;
    } else {
        print("FAILED (ret=");
        print_num(nread);
        puts(")");
        failed++;
        goto cleanup;
    }

    /* Test 3: Parse and display entries */
    puts("Test 3: Parsing directory entries...");
    if (nread > 0) {
        pos = 0;
        while (pos < nread) {
            d = (struct linux_dirent64 *)(buf + pos);

            /* Sanity check reclen */
            if (d->d_reclen == 0) {
                puts("  ERROR: d_reclen is 0, breaking");
                failed++;
                break;
            }

            print("  [");
            print_num(file_count);
            print("] \"");
            print(d->d_name);
            print("\" type=");
            print(get_type_name(d->d_type));
            print(" ino=");
            print_hex((uint32_t)d->d_ino);
            print(" reclen=");
            print_num(d->d_reclen);
            puts("");

            file_count++;
            pos += d->d_reclen;
        }

        if (file_count > 0) {
            print("  Found ");
            print_num(file_count);
            puts(" entries... OK");
            passed++;
        } else {
            puts("  No entries found... FAILED");
            failed++;
        }
    } else {
        puts("  (no entries to parse)");
        passed++;
    }

    /* Test 4: Call getdents64 again - should return 0 (end of dir) or more entries */
    print("Test 4: getdents64 again... ");
    nread = sys_getdents64(fd, buf, sizeof(buf));
    if (nread >= 0) {
        if (nread == 0) {
            puts("OK (end of directory)");
        } else {
            print("OK (");
            print_num(nread);
            puts(" more bytes)");
        }
        passed++;
    } else {
        print("FAILED (ret=");
        print_num(nread);
        puts(")");
        failed++;
    }

    /* Test 5: getdents64 on invalid fd should fail */
    print("Test 5: getdents64(999, ...)... ");
    nread = sys_getdents64(999, buf, sizeof(buf));
    if (nread < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

cleanup:
    /* Test 6: Close directory */
    print("Test 6: close(fd)... ");
    if (sys_close(fd) == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

done:
    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All getdents64 syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
