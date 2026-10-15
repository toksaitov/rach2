/*
 * crt0.c: the entry point, the system calls and the C library of Doom on
 * Rach2, which links no other library.
 *
 * The entry point, the system call wrappers and their numbers are written
 * once for i386, with `int $0x80`, and once for x86-64, with `syscall`; the
 * rest is the same C for both. The headers in include/ declare what this
 * file defines and state where it differs from the C library's contract.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#if defined __x86_64__
/* The numbers of rach2/sys/arch/x86_64/sysnr.h, which are Linux's. */
#define SYS_EXIT        60
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_LSEEK       8
#define SYS_BRK         12
#define SYS_IOCTL       16
#define SYS_GETTIMEOFDAY 96
#else
/* The numbers of rach2/sys/arch/i686/sysnr.h, which are Linux's. */
#define SYS_EXIT        1
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_LSEEK       19
#define SYS_BRK         45
#define SYS_IOCTL       54
#define SYS_GETTIMEOFDAY 78
#endif

/* The open flags, with Linux's values. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

/* The requests of the console and the framebuffer (rach2/sys/syscall.c). */
#define KIOCSOUND     0x4B2F
#define FB_SETPALETTE 0x4603

/* The platform layer's, in doomgeneric_rach2.c. */
int main(int argc, char **argv);

struct timeval {
    long tv_sec;
    long tv_usec;
};

/*
 * The system calls. Each wrapper returns what the kernel returns, the
 * result or a negative errno, and sets no errno. The i386 definitions after
 * the x86-64 ones have the same contracts.
 */
#if defined __x86_64__
/**
 * One x86-64 system call of up to three arguments: the number in RAX, the
 * arguments in RDI, RSI and RDX, the result in RAX. The instruction
 * overwrites RCX and R11.
 */
static long syscall3(long number, long a, long b, long c)
{
    long ret;
    __asm__ __volatile__ (
        "syscall"
        : "=a"(ret)
        : "a"(number), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory"
    );
    return ret;
}

void _exit(int status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    __builtin_unreachable();
}

int read(int fd, void *buf, size_t count)
{
    return (int)syscall3(SYS_READ, fd, (long)buf, (long)count);
}

int write(int fd, const void *buf, size_t count)
{
    return (int)syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    int mode = 0;

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)syscall3(SYS_OPEN, (long)path, flags, mode);
}

int close(int fd)
{
    return (int)syscall3(SYS_CLOSE, fd, 0, 0);
}

/**
 * The ioctl `request` of `fd`, with the one argument each of the kernel's
 * requests takes, an integer or a pointer passed as an unsigned long.
 * Returns the kernel's answer: 0 or a count, or a negative errno.
 */
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    unsigned long arg;

    va_start(ap, request);
    arg = va_arg(ap, unsigned long);
    va_end(ap);

    return (int)syscall3(SYS_IOCTL, fd, (long)request, (long)arg);
}

int gettimeofday(struct timeval *tv, void *tz)
{
    return (int)syscall3(SYS_GETTIMEOFDAY, (long)tv, (long)tz, 0);
}

/**
 * Move the offset of `fd` as `whence` says: 0 from the start, 1 from the
 * current offset, 2 from the end. Returns the new offset or a negative
 * errno.
 */
long lseek(int fd, long offset, int whence)
{
    return syscall3(SYS_LSEEK, fd, offset, whence);
}
#else
void _exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    __builtin_unreachable();
}

int read(int fd, void *buf, size_t count)
{
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_READ), "b"(fd), "c"(buf), "d"(count)
    );
    return ret;
}

int write(int fd, const void *buf, size_t count)
{
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
    );
    return ret;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    int mode = 0;
    int ret;

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_OPEN), "b"(path), "c"(flags), "d"(mode)
    );
    return ret;
}

int close(int fd)
{
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_CLOSE), "b"(fd)
    );
    return ret;
}

int ioctl(int fd, unsigned long request, ...)
{
    int ret;
    va_list ap;
    unsigned long arg;

    va_start(ap, request);
    arg = va_arg(ap, unsigned long);
    va_end(ap);

    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_IOCTL), "b"(fd), "c"(request), "d"(arg)
    );
    return ret;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_GETTIMEOFDAY), "b"(tv), "c"(tz)
    );
    return ret;
}

long lseek(int fd, long offset, int whence)
{
    long ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_LSEEK), "b"(fd), "c"(offset), "d"(whence)
    );
    return ret;
}
#endif

/* The functions of string.h, ctype.h and stdlib.h. */
void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++) len++;
    return len;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;

    for (i = 0; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n-- && *s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    if (n == (size_t)-1) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == c) return (char *)s;
        s++;
    }
    return c == '\0' ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == c) last = s;
        s++;
    }
    return c == '\0' ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0) return (char *)haystack;

    while (*haystack) {
        if (strncmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

int atoi(const char *s)
{
    int n = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return neg ? -n : n;
}

int abs(int n)
{
    return n < 0 ? -n : n;
}

int toupper(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/**
 * Write `n` in `base` to `buf`, with a '-' first when `n` is negative and
 * `base` is 10, and return the characters written. `buf` is not terminated,
 * and 32 bytes hold any long.
 */
static int format_signed(char *buf, long n, int base)
{
    int i = 0;
    int neg = 0;
    unsigned long un;
    char tmp[32];
    int ti = 0;

    if (n < 0 && base == 10) {
        neg = 1;
        un = (unsigned long)(-(n + 1)) + 1;  /* -LONG_MIN overflows a long */
    } else {
        un = (unsigned long)n;
    }

    do {
        int digit = un % base;
        tmp[ti++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        un /= base;
    } while (un);

    if (neg) buf[i++] = '-';
    while (ti > 0) buf[i++] = tmp[--ti];
    return i;
}

/**
 * Write `un` in `base` to `buf`, in capital hexadecimal digits when
 * `uppercase` is set, and return the characters written. `buf` is not
 * terminated, and 32 bytes hold any unsigned long.
 */
static int format_unsigned(char *buf, unsigned long un, int base, int uppercase)
{
    int i = 0;
    char tmp[32];
    int ti = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        tmp[ti++] = digits[un % base];
        un /= base;
    } while (un);

    while (ti > 0) buf[i++] = tmp[--ti];
    return i;
}

/**
 * printf's work, which stdio.h describes: the output is formatted into a
 * buffer of 1024 bytes and written in one write, so that one call's line
 * reaches the console whole.
 */
int vprintf(const char *fmt, va_list ap)
{
    char outbuf[1024];
    int outlen = 0;
    const char *p;

    for (p = fmt; *p; p++) {
        int is_long = 0;

        if (*p != '%') {
            if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = *p;
            continue;
        }
        p++;

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            p++;
        }

        while (*p >= '0' && *p <= '9') {
            p++;
        }

        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        }

        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == 'l') p++;
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++;
        } else if (*p == 'z' || *p == 't' || *p == 'j') {
            is_long = 1;  /* size_t and ptrdiff_t have the size of long */
            p++;
        }

        switch (*p) {
            case 'd':
            case 'i': {
                long val = is_long ? va_arg(ap, long) : va_arg(ap, int);
                char tmp[32];
                int len = format_signed(tmp, val, 10);
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'u': {
                unsigned long val = is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int len = format_unsigned(tmp, val, 10, 0);
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'o': {
                unsigned long val = is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int len = format_unsigned(tmp, val, 8, 0);
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'x':
            case 'X': {
                unsigned long val = is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int len = format_unsigned(tmp, val, 16, *p == 'X');
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(ap, void *);
                char tmp[32];
                int len;
                if (outlen < (int)sizeof(outbuf) - 2) {
                    outbuf[outlen++] = '0';
                    outbuf[outlen++] = 'x';
                }
                len = format_unsigned(tmp, val, 16, 0);
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && outlen < (int)sizeof(outbuf) - 1)
                    outbuf[outlen++] = *s++;
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = c;
                break;
            }
            case 'f':
            case 'g':
            case 'e':
            case 'F':
            case 'G':
            case 'E': {
                (void)va_arg(ap, double);
                const char *flt = "<float>";
                for (int j = 0; flt[j] && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = flt[j];
                break;
            }
            case 'n':
                *va_arg(ap, int *) = outlen;
                break;
            case '%':
                if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = '%';
                break;
            case '\0':
                write(1, outbuf, outlen);
                return outlen;
            default:
                (void)va_arg(ap, int);
                if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = '?';
                break;
        }
    }

    write(1, outbuf, outlen);
    return outlen;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *out = buf;
    char *end = buf + size - 1;
    const char *p;

    if (size == 0) return 0;

    for (p = fmt; *p && out < end; p++) {
        int is_long = 0;
        int zero_pad = 0;
        int width = 0;
        int precision = -1;  /* -1: none given */

        if (*p != '%') {
            *out++ = *p;
            continue;
        }
        p++;

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            if (*p == '0') zero_pad = 1;
            p++;
        }

        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }

        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == 'l') p++;
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++;
        } else if (*p == 'z' || *p == 't' || *p == 'j') {
            is_long = 1;
            p++;
        }

        switch (*p) {
            case 'd':
            case 'i': {
                long val = is_long ? va_arg(ap, long) : va_arg(ap, int);
                char tmp[32];
                int i = 0;
                int neg = 0;
                unsigned long uval;
                int min_digits;
                int num_len;

                if (val < 0) {
                    neg = 1;
                    /* -LONG_MIN overflows a long */
                    uval = (unsigned long)(-(val + 1)) + 1;
                } else {
                    uval = (unsigned long)val;
                }

                do {
                    tmp[i++] = '0' + (uval % 10);
                    uval /= 10;
                } while (uval);

                num_len = i + neg;
                min_digits = (precision >= 0) ? precision : (zero_pad ? (width - neg) : 0);

                if (!zero_pad && width > 0) {
                    int pad = width - (neg + (i > min_digits ? i : min_digits));
                    while (pad-- > 0 && out < end) *out++ = ' ';
                }

                if (neg && out < end) *out++ = '-';

                while (i < min_digits && out < end) {
                    *out++ = '0';
                    min_digits--;
                }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'u': {
                unsigned long val = is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int i = 0;
                int min_digits;

                do {
                    tmp[i++] = '0' + (val % 10);
                    val /= 10;
                } while (val);

                min_digits = (precision >= 0) ? precision : (zero_pad ? width : 0);

                if (!zero_pad && width > 0) {
                    int pad = width - (i > min_digits ? i : min_digits);
                    while (pad-- > 0 && out < end) *out++ = ' ';
                }

                while (i < min_digits && out < end) {
                    *out++ = '0';
                    min_digits--;
                }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'o': {
                unsigned long val = is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int i = 0;
                int min_digits;

                do {
                    tmp[i++] = '0' + (val & 7);
                    val >>= 3;
                } while (val);

                min_digits = (precision >= 0) ? precision : (zero_pad ? width : 0);
                while (i < min_digits && out < end) {
                    *out++ = '0';
                    min_digits--;
                }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'x':
            case 'X': {
                unsigned long val = is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int i = 0;
                int min_digits;
                const char *hex = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";

                do {
                    tmp[i++] = hex[val & 0xF];
                    val >>= 4;
                } while (val);

                min_digits = (precision >= 0) ? precision : (zero_pad ? width : 0);

                if (!zero_pad && width > 0) {
                    int pad = width - (i > min_digits ? i : min_digits);
                    while (pad-- > 0 && out < end) *out++ = ' ';
                }

                while (i < min_digits && out < end) {
                    *out++ = '0';
                    min_digits--;
                }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(ap, void *);
                char tmp[32];
                int i = 0;

                if (out < end) *out++ = '0';
                if (out < end) *out++ = 'x';

                do {
                    tmp[i++] = "0123456789abcdef"[val & 0xF];
                    val >>= 4;
                } while (val);

                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                int slen, pad;
                if (!s) s = "(null)";
                slen = 0;
                while (s[slen]) slen++;

                pad = (width > slen) ? (width - slen) : 0;
                while (pad-- > 0 && out < end) *out++ = ' ';

                while (*s && out < end) *out++ = *s++;
                break;
            }
            case 'c':
                if (out < end) *out++ = (char)va_arg(ap, int);
                break;
            case 'f':
            case 'g':
            case 'e':
            case 'F':
            case 'G':
            case 'E': {
                (void)va_arg(ap, double);
                const char *flt = "<float>";
                while (*flt && out < end) *out++ = *flt++;
                break;
            }
            case 'n':
                *va_arg(ap, int *) = out - buf;
                break;
            case '%':
                if (out < end) *out++ = '%';
                break;
            case '\0':
                *out = '\0';
                return out - buf;
            default:
                (void)va_arg(ap, int);
                if (out < end) *out++ = '?';
                break;
        }
    }

    *out = '\0';
    return out - buf;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, 4096, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vprintf(fmt, ap);
    va_end(ap);
    return ret;
}

int puts(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

int putchar(int c)
{
    char ch = c;
    write(1, &ch, 1);
    return c;
}

/*
 * The heap: a bump allocator over the process's brk range, grown 64 KiB at
 * a time as it is used. The kernel reserves the addresses at brk and gives
 * a page its frame at first touch, so the zone Doom takes at start is as
 * large as I_ZoneBase asks for and costs frames only as the game fills it.
 */
static char *_heap_break;   /* the end of what brk has given, or NULL */
static char *_heap_next;    /* the next byte to hand out */

/**
 * The brk system call: set the break to `addr` and return the break as it
 * then is, which is the old one when the kernel refuses; a NULL `addr`
 * asks for the current break.
 */
#if defined __x86_64__
static char *sys_brk(char *addr)
{
    return (char *)syscall3(SYS_BRK, (long)addr, 0, 0);
}
#else
static char *sys_brk(char *addr)
{
    char *ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_BRK), "b"(addr)
    );
    return ret;
}
#endif

void *malloc(size_t size)
{
    void *ptr;

    size = (size + 7) & ~7;

    if (_heap_break == NULL) {
        _heap_break = sys_brk(NULL);
        _heap_next = _heap_break;
        if (_heap_break == NULL) {
            return NULL;
        }
    }
    if (size > (size_t)(_heap_break - _heap_next)) {
        size_t need = size - (size_t)(_heap_break - _heap_next);
        size_t grow = (need + 65535u) & ~(size_t)65535u;
        char *want = _heap_break + grow;

        if (sys_brk(want) != want) {
            return NULL;
        }
        _heap_break = want;
    }

    ptr = _heap_next;
    _heap_next += size;
    return ptr;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total;
    void *ptr;

    if (size != 0 && nmemb > (size_t)-1 / size) {
        return NULL;
    }
    total = nmemb * size;
    ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size)
{
    void *new_ptr;
    if (size == 0) {
        return NULL;
    }
    new_ptr = malloc(size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, size);
    }
    return new_ptr;
}

void free(void *ptr)
{
    (void)ptr;
}

/* The streams of stdio.h, whose FILE this must match. */
typedef struct {
    int fd;
    int eof;
    int error;
} FILE;

static FILE _stdin_file  = { 0, 0, 0 };
static FILE _stdout_file = { 1, 0, 0 };
static FILE _stderr_file = { 2, 0, 0 };

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

FILE *fopen(const char *path, const char *mode)
{
    FILE *f;
    int flags = O_RDONLY;
    int fd;

    if (mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mode[0] == 'r' && mode[1] == '+') flags = O_RDWR;

    fd = open(path, flags, 0666);
    if (fd < 0) return NULL;

    f = malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    f->eof = 0;
    f->error = 0;
    return f;
}

int fclose(FILE *f)
{
    if (!f) return -1;
    close(f->fd);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t total = size * nmemb;
    int ret;

    if (!f || total == 0) return 0;

    ret = read(f->fd, ptr, total);
    if (ret < 0) {
        f->error = 1;
        return 0;
    }
    if (ret == 0) {
        f->eof = 1;
    }
    return ret / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t total = size * nmemb;
    int ret;

    if (!f || total == 0) return 0;

    ret = write(f->fd, ptr, total);
    if (ret < 0) {
        f->error = 1;
        return 0;
    }
    return ret / size;
}

int fseek(FILE *f, long offset, int whence)
{
    long ret;
    if (!f) return -1;
    ret = lseek(f->fd, offset, whence);
    if (ret < 0) return -1;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f)
{
    if (!f) return -1;
    return lseek(f->fd, 0, 1);  /* SEEK_CUR */
}

int feof(FILE *f)
{
    return f ? f->eof : 1;
}

int ferror(FILE *f)
{
    return f ? f->error : 1;
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[1024];
    int len;

    if (!f) return -1;

    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) {
        write(f->fd, buf, len);
    }

    return len;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vfprintf(f, fmt, ap);
    va_end(ap);
    return ret;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int count = 0;

    va_start(ap, fmt);
    while (*fmt && *str) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd') {
                int *p = va_arg(ap, int *);
                *p = atoi(str);
                while (*str == ' ') str++;
                if (*str == '-' || *str == '+') str++;
                while (*str >= '0' && *str <= '9') str++;
                count++;
            }
            fmt++;
        } else if (*fmt == *str) {
            fmt++;
            str++;
        } else {
            break;
        }
    }
    va_end(ap);
    return count;
}

/** A stub: there is no shell, so `command` is not run and the answer is
 * -1. */
int system(const char *command)
{
    (void)command;
    return -1;
}

/**
 * The decimal number of `s`: spaces and tabs, a sign, digits and a fraction
 * after a '.'; no exponent.
 */
double atof(const char *s)
{
    double result = 0.0;
    double fraction = 0.0;
    int sign = 1;
    int div = 1;
    int in_fraction = 0;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    while (*s) {
        if (*s == '.') {
            in_fraction = 1;
            s++;
            continue;
        }
        if (*s < '0' || *s > '9') break;

        if (in_fraction) {
            div *= 10;
            fraction = fraction + (double)(*s - '0') / div;
        } else {
            result = result * 10 + (*s - '0');
        }
        s++;
    }

    return sign * (result + fraction);
}

/**
 * The 64-bit signed division that i386 code calls and libgcc would supply,
 * as it would the three functions after it: the quotient of `a` by `b`,
 * rounded toward zero. A `b` of 0 gives 0 where hardware would fault.
 *
 * The magnitudes are negated as unsigned values, which is defined for
 * INT64_MIN, and `bit` is unsigned so that its shift right reaches 0.
 */
long long __divdi3(long long a, long long b)
{
    int neg = 0;
    unsigned long long result = 0;
    unsigned long long bit = 1;
    unsigned long long ua, ub;

    if (b == 0) return 0;

    if (a < 0) { neg = !neg; ua = -(unsigned long long)a; } else { ua = a; }
    if (b < 0) { neg = !neg; ub = -(unsigned long long)b; } else { ub = b; }

    while (ub < ua && !(ub & (1ULL << 63))) {
        ub <<= 1;
        bit <<= 1;
    }

    while (bit) {
        if (ua >= ub) {
            ua -= ub;
            result |= bit;
        }
        ub >>= 1;
        bit >>= 1;
    }

    return neg ? (long long)(0ULL - result) : (long long)result;
}

/** The unsigned quotient of `a` by `b`; a `b` of 0 gives 0. */
unsigned long long __udivdi3(unsigned long long a, unsigned long long b)
{
    unsigned long long result = 0;
    unsigned long long bit = 1;

    if (b == 0) return 0;

    while (b < a && !(b & (1ULL << 63))) {
        b <<= 1;
        bit <<= 1;
    }

    while (bit) {
        if (a >= b) {
            a -= b;
            result |= bit;
        }
        b >>= 1;
        bit >>= 1;
    }

    return result;
}

/** The remainder of `a` by `b`, with the sign of `a`; `a` itself when `b` is
 * 0. */
long long __moddi3(long long a, long long b)
{
    return a - __divdi3(a, b) * b;
}

/** The unsigned remainder of `a` by `b`; `a` itself when `b` is 0. */
unsigned long long __umoddi3(unsigned long long a, unsigned long long b)
{
    return a - __udivdi3(a, b) * b;
}

#if defined __x86_64__
/*
 * _start: the ELF entry point, x86-64.
 *
 * The System V x86-64 ABI gives control here with argc at 0(%rsp), the argv
 * vector at 8(%rsp) and %rsp 16-byte aligned; the call to main pushes the
 * return address that leaves %rsp where a called function expects it. The
 * arguments go to main in %rdi and %rsi, and its status to _exit in %edi.
 */
__asm__(
    ".text\n\t"
    ".globl _start\n"
    "_start:\n\t"
    "xorq %rbp, %rbp\n\t"     /* mark the outermost stack frame (ABI) */
    "movq (%rsp), %rdi\n\t"   /* argc */
    "leaq 8(%rsp), %rsi\n\t"  /* argv */
    "andq $-16, %rsp\n\t"     /* keep the entry stack 16-byte aligned */
    "call main\n\t"
    "movl %eax, %edi\n\t"     /* exit status from main */
    "call _exit\n"
);
#else
/*
 * _start: the ELF entry point, i386.
 *
 * The System V i386 ABI gives control here with argc at 0(%esp) and the
 * argv vector at 4(%esp); %ebp is undefined, so the arguments are read from
 * %esp. This is assembly because a C function would run a prologue that
 * moves %esp before any statement could read them. main takes its
 * arguments on the stack, and _exit its status.
 */
__asm__(
    ".text\n\t"
    ".globl _start\n"
    "_start:\n\t"
    "xorl %ebp, %ebp\n\t"      /* mark the outermost stack frame (ABI) */
    "movl (%esp), %eax\n\t"    /* argc */
    "leal 4(%esp), %edx\n\t"   /* argv */
    "andl $-16, %esp\n\t"      /* keep the entry stack 16-byte aligned */
    "pushl %edx\n\t"           /* main's second argument: argv */
    "pushl %eax\n\t"           /* main's first argument: argc */
    "call main\n\t"
    "pushl %eax\n\t"           /* exit status from main */
    "call _exit\n"
);
#endif
