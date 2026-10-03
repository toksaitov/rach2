/*
 * crt0.c - C runtime for Doom on Rach2
 *
 * Provides _start entry point, syscall wrappers, and minimal libc functions.
 * The entry point, the wrappers and the system call numbers are written once
 * for i386, with `int $0x80`, and once for x86-64, with `syscall`; the rest is
 * the same C for both.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#if defined __x86_64__
/* Syscall numbers (Linux x86-64, as musl's arch/x86_64/bits/syscall.h.in) */
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
/* Syscall numbers (Linux i386 compatible) */
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

/* Open flags */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000

/* ioctl request codes */
#define KIOCSOUND     0x4B2F
#define FB_SETPALETTE 0x4603

/* Forward declaration of main */
int main(int argc, char **argv);

struct timeval {
    long tv_sec;
    long tv_usec;
};

/*
 * Syscall wrappers
 */
#if defined __x86_64__
/*
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
    return (int)syscall3(SYS_OPEN, (long)path, flags, 0);
}

int close(int fd)
{
    return (int)syscall3(SYS_CLOSE, fd, 0, 0);
}

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

int lseek(int fd, int offset, int whence)
{
    return (int)syscall3(SYS_LSEEK, fd, offset, whence);
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
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_OPEN), "b"(path), "c"(flags)
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

int lseek(int fd, int offset, int whence)
{
    int ret;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(ret)
        : "a"(SYS_LSEEK), "b"(fd), "c"(offset), "d"(whence)
    );
    return ret;
}
#endif

/*
 * Minimal libc functions
 */
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

    /* Copy up to n characters from src */
    for (i = 0; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    /* Pad remainder with nulls */
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

/*
 * Simple printf implementation for debugging
 * Outputs to a buffer first, then writes in one syscall for reliability
 */
static int format_signed(char *buf, int n, int base)
{
    int i = 0;
    int neg = 0;
    unsigned int un;
    char tmp[32];
    int ti = 0;

    if (n < 0 && base == 10) {
        neg = 1;
        un = (unsigned int)(-(n + 1)) + 1;  /* Handle INT_MIN safely */
    } else {
        un = (unsigned int)n;
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

static int format_unsigned(char *buf, unsigned int un, int base, int uppercase)
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

        /* Skip flags: -, +, space, #, 0 */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            p++;
        }

        /* Skip width */
        while (*p >= '0' && *p <= '9') {
            p++;
        }

        /* Skip precision */
        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        }

        /* Check for length modifier */
        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == 'l') p++;  /* skip ll */
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++;  /* skip hh */
        } else if (*p == 'z' || *p == 't' || *p == 'j') {
            is_long = 1;  /* treat size_t etc as long */
            p++;
        }

        switch (*p) {
            case 'd':
            case 'i': {
                int val = is_long ? (int)va_arg(ap, long) : va_arg(ap, int);
                char tmp[32];
                int len = format_signed(tmp, val, 10);
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'u': {
                unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int len = format_unsigned(tmp, val, 10, 0);
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'o': {
                unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int len = format_unsigned(tmp, val, 8, 0);
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'x':
            case 'X': {
                unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char tmp[32];
                int len = format_unsigned(tmp, val, 16, *p == 'X');
                for (int j = 0; j < len && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = tmp[j];
                break;
            }
            case 'p': {
                unsigned int val = (unsigned int)(uintptr_t)va_arg(ap, void *);
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
                /* Consume double argument but just print placeholder */
                (void)va_arg(ap, double);
                const char *flt = "<float>";
                for (int j = 0; flt[j] && outlen < (int)sizeof(outbuf) - 1; j++)
                    outbuf[outlen++] = flt[j];
                break;
            }
            case 'n':
                /* %n - store characters written so far */
                *va_arg(ap, int *) = outlen;
                break;
            case '%':
                if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = '%';
                break;
            case '\0':
                /* End of string after % */
                write(1, outbuf, outlen);
                return outlen;
            default:
                /* Unknown specifier - consume an int to stay in sync */
                (void)va_arg(ap, int);
                if (outlen < (int)sizeof(outbuf) - 1) outbuf[outlen++] = '?';
                break;
        }
    }

    write(1, outbuf, outlen);
    return outlen;
}

/*
 * vsnprintf - format into buffer
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *out = buf;
    char *end = buf + size - 1;  /* leave room for null terminator */
    const char *p;

    if (size == 0) return 0;

    for (p = fmt; *p && out < end; p++) {
        int is_long = 0;
        int zero_pad = 0;
        int width = 0;
        int precision = -1;  /* -1 means not specified */

        if (*p != '%') {
            *out++ = *p;
            continue;
        }
        p++;

        /* Parse flags */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            if (*p == '0') zero_pad = 1;
            p++;
        }

        /* Parse width */
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }

        /* Parse precision */
        if (*p == '.') {
            p++;
            precision = 0;
            while (*p >= '0' && *p <= '9') {
                precision = precision * 10 + (*p - '0');
                p++;
            }
        }

        /* Length modifier */
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
                int val = is_long ? (int)va_arg(ap, long) : va_arg(ap, int);
                char tmp[32];
                int i = 0;
                int neg = 0;
                unsigned int uval;
                int min_digits;
                int num_len;

                if (val < 0) {
                    neg = 1;
                    uval = (unsigned int)(-(val + 1)) + 1;  /* Handle INT_MIN */
                } else {
                    uval = (unsigned int)val;
                }

                do {
                    tmp[i++] = '0' + (uval % 10);
                    uval /= 10;
                } while (uval);

                num_len = i + neg;
                min_digits = (precision >= 0) ? precision : (zero_pad ? (width - neg) : 0);

                /* Pad with spaces if not zero-padding */
                if (!zero_pad && width > 0) {
                    int pad = width - (neg + (i > min_digits ? i : min_digits));
                    while (pad-- > 0 && out < end) *out++ = ' ';
                }

                if (neg && out < end) *out++ = '-';

                /* Add leading zeros */
                while (i < min_digits && out < end) {
                    *out++ = '0';
                    min_digits--;
                }
                while (i-- > 0 && out < end) *out++ = tmp[i];
                break;
            }
            case 'u': {
                unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
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
                unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
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
                unsigned int val = is_long ? (unsigned int)va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
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
                unsigned int val = (unsigned int)(uintptr_t)va_arg(ap, void *);
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

                /* Right-pad with spaces if width specified */
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
                /* Consume double but print placeholder */
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
                /* Unknown - consume int to stay in sync */
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
    /* Use a large size - caller's responsibility to ensure buffer is big
     * enough */
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
 * A fixed array here was 8 MiB, which held a 6 MiB zone and no more; the
 * zone is what keeps a level's lumps in memory, and with a 28 MB WAD a
 * 6 MiB zone reread the disk all game long.
 */
static char *_heap_break;   /* the end of what brk has given, or NULL */
static char *_heap_next;    /* the next byte to hand out */

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

    /* Align to 8 bytes */
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

        /* brk answers with the old break when it refuses. */
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

    /* Reject a product that would wrap size_t and hand back a buffer smaller
     * than the caller asked for. */
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
    /* Simple implementation: always allocate new memory */
    void *new_ptr;
    if (size == 0) {
        return NULL;
    }
    new_ptr = malloc(size);
    if (new_ptr && ptr) {
        /* Copy old data - we don't know the old size, so copy up to new size */
        memcpy(new_ptr, ptr, size);
    }
    return new_ptr;
}

void free(void *ptr)
{
    /* Bump allocator doesn't free - memory is never reclaimed */
    (void)ptr;
}

/*
 * File I/O wrappers (Doom uses these)
 */
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
    else if (mode[0] == 'a') flags = O_WRONLY | O_CREAT;
    else if (mode[0] == 'r' && mode[1] == '+') flags = O_RDWR;

    fd = open(path, flags);
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
    int ret;
    if (!f) return -1;
    ret = lseek(f->fd, (int)offset, whence);
    if (ret < 0) return -1;
    f->eof = 0;  /* Clear EOF on successful seek */
    return 0;
}

long ftell(FILE *f)
{
    if (!f) return -1;
    /* Get current position by seeking 0 from current position */
    return (long)lseek(f->fd, 0, 1);  /* SEEK_CUR = 1 */
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

    /* Format into buffer */
    len = vsnprintf(buf, sizeof(buf), fmt, ap);

    /* Write to the file */
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
    /* Minimal implementation for Doom's needs */
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

/*
 * Additional functions needed by Doom
 */
int system(const char *command)
{
    /* No shell support - just return error */
    (void)command;
    return -1;
}

double atof(const char *s)
{
    double result = 0.0;
    double fraction = 0.0;
    int sign = 1;
    int div = 1;
    int in_fraction = 0;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t') s++;

    /* Handle sign */
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    /* Parse digits */
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

/*
 * 64-bit division helper (needed by libgcc-less freestanding code)
 * This is called by the compiler for 64-bit integer division
 */
long long __divdi3(long long a, long long b)
{
    int neg = 0;
    unsigned long long result = 0;
    unsigned long long bit = 1;
    unsigned long long ua, ub;

    if (b == 0) return 0;  /* Avoid divide by zero */

    /* Magnitudes via unsigned negation, well defined even for the INT64_MIN
     * input where signed negation would overflow. `bit` must be unsigned too,
     * or its >>= 1 sign-extends from bit 63 and the loop never terminates. */
    if (a < 0) { neg = !neg; ua = -(unsigned long long)a; } else { ua = a; }
    if (b < 0) { neg = !neg; ub = -(unsigned long long)b; } else { ub = b; }

    /* Find highest bit in divisor */
    while (ub < ua && !(ub & (1ULL << 63))) {
        ub <<= 1;
        bit <<= 1;
    }

    /* Long division */
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

/* Unsigned version */
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

/* Modulo versions */
long long __moddi3(long long a, long long b)
{
    return a - __divdi3(a, b) * b;
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b)
{
    return a - __udivdi3(a, b) * b;
}

#if defined __x86_64__
/*
 * _start - ELF entry point, x86-64.
 *
 * The SysV x86-64 ABI hands control here with argc at 0(%rsp), the argv
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
 * _start - ELF entry point.
 *
 * The SysV i386 ABI hands control here with argc at 0(%esp) and the argv
 * vector at 4(%esp); %ebp is undefined. This is raw assembly because a C
 * function would run a prologue that moves %esp before any statement could
 * read the arguments. The previous version read them through %ebp and
 * survived only when the %ebp left over from the preceding exec happened to
 * address mapped memory; the ABI does not hand the arguments through %ebp,
 * and Doom is built -fomit-frame-pointer, so _start never established it
 * either.
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
