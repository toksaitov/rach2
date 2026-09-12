/**
 * The /proc snapshot: taken once at open, bounded, and served whole.
 *
 * A /proc file here is generated into a buffer when it is opened and read back
 * from there, so every read of one descriptor sees one moment. That is worth a
 * probe because the alternative is not obviously wrong: regenerating the text
 * on each read would answer the same bytes for a table nothing is changing,
 * and would splice two states of it together for a table something is.
 *
 * So the probe changes the thing under the file while a descriptor is open. It
 * forks a child, opens `/proc/<child>/stat`, kills and reaps the child, and
 * only then reads. The pid no longer names a process at all, so a generator
 * asked at that moment could answer nothing; a snapshot answers what it took.
 * Reading twice with nothing changing would prove neither.
 *
 * The rest is the bound. Each file declares a worst case and a text that
 * exceeds it is refused rather than served short, so every file this image can
 * open must come back whole: `/proc/cmdline` and `/proc/net/dev` end in the
 * newline their last line has, and `/proc/self/stat` ends in one after a
 * closing parenthesis it must also carry, which a truncated line would lack.
 * And two files are two inodes: `/proc/loop` and `/proc/interrupts` once
 * shared a number, and a program that compares files by identity would have
 * called them one.
 *
 * `procprobe mount` reads the root's line out of /proc/mounts, prints it, and
 * stops there. That line is the one thing here that depends on how the machine
 * came up, so the suite boots three ways for it; the rest of this probe does
 * not differ between those boots and runs once, in the shared probe boot.
 *
 * Prints "PROCPROBE OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT          1
#define SYS_FORK          2
#define SYS_READ          3
#define SYS_WRITE         4
#define SYS_OPEN          5
#define SYS_CLOSE         6
#define SYS_WAITPID       7
#define SYS_GETPID        20
#define SYS_KILL          37
#define SYS_PAUSE         29
#define SYS_NANOSLEEP     162
#define SYS_CLOCK_GETTIME 265
#define SYS_SOCKETCALL    102
#define SYS_FSTAT64       197
#define SYS_READLINK      85
#define SYS_STAT64        195

#define SC_SOCKET       1
#define SC_BIND         2
#define SC_LISTEN       4
#define SC_CONNECT      3
#define SYS_SETRESUID32 208
#define SYS_FORK        2
#define SYS_WAITPID     7
#define AF_INET         2
#define SOCK_STREAM     1

/* i386 struct stat64: the 64-bit st_ino musl reads sits at offset 88. */
#define ST_INO64 88

#define CLOCK_MONOTONIC 1

#define O_RDONLY 0
#define SIGKILL  9

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
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
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"\n", 1);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

/**
 * Read a file to its end, and say how long it was.
 *
 * Reads until the file reports the end rather than once, since a snapshot is
 * served in whatever pieces the caller asks for and a single short read would
 * be mistaken for a short file.
 */
static int slurp(const char *name, char *out, int cap)
{
    int fd, n, total = 0;

    fd = syscall3(SYS_OPEN, (int)(size_t)name, O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    while (total < cap - 1) {
        n = syscall3(SYS_READ, fd, (int)(size_t)(out + total), cap - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    syscall1(SYS_CLOSE, fd);
    out[total] = '\0';
    return total;
}

/** Build "/proc/<pid>/stat" for a small pid. */
static void stat_path_of(char *dst, int pid)
{
    const char *head = "/proc/";
    const char *tail = "/stat";
    int i = 0, j;
    char digits[8];
    int k = 0;

    while (head[i]) {
        dst[i] = head[i];
        i++;
    }
    do {
        digits[k++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid > 0);
    while (k > 0) {
        dst[i++] = digits[--k];
    }
    for (j = 0; tail[j]; j++) {
        dst[i++] = tail[j];
    }
    dst[i] = '\0';
}

/** How many newline-terminated lines `buf` holds. */
static int lines_in(const char *b, int n)
{
    int i, lines = 0;

    for (i = 0; i < n; i++) {
        if (b[i] == '\n') {
            lines++;
        }
    }
    return lines;
}

/** How many blank-separated fields the text holds. */
static int fields_in(const char *b, int n)
{
    int i, fields = 0, in_field = 0;

    for (i = 0; i < n; i++) {
        if (b[i] == ' ' || b[i] == '\n') {
            in_field = 0;
        } else if (!in_field) {
            in_field = 1;
            fields++;
        }
    }
    return fields;
}

/** How many times `c` occurs. */
static int count_of(const char *b, int n, char c)
{
    int i, seen = 0;

    for (i = 0; i < n; i++) {
        if (b[i] == c) {
            seen++;
        }
    }
    return seen;
}

/** Whether the text ends with `tail`. */
static int ends_with(const char *b, int n, const char *tail)
{
    int len = (int)strlen(tail);
    int i;

    if (n < len) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (b[n - len + i] != tail[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether `needle` occurs anywhere in the text. */
static int contains(const char *b, int n, const char *needle)
{
    int i, j;

    for (i = 0; i < n; i++) {
        for (j = 0; needle[j] && i + j < n && b[i + j] == needle[j]; j++) {
        }
        if (!needle[j]) {
            return 1;
        }
    }
    return 0;
}

/** Whether some line of the text begins with `head`. */
static int has_line_starting(const char *b, int n, const char *head)
{
    int i, j, at_line_start = 1;

    for (i = 0; i < n; i++) {
        if (at_line_start) {
            for (j = 0; head[j] && i + j < n && b[i + j] == head[j]; j++) {
            }
            if (!head[j]) {
                return 1;
            }
        }
        at_line_start = (b[i] == '\n');
    }
    return 0;
}

/** Read the first unsigned decimal in `b`, or -1. */
static int first_uint(const char *b, int n)
{
    int i, v = -1;

    for (i = 0; i < n; i++) {
        if (b[i] >= '0' && b[i] <= '9') {
            v = 0;
            while (i < n && b[i] >= '0' && b[i] <= '9') {
                v = v * 10 + (b[i] - '0');
                i++;
            }
            return v;
        }
    }
    return v;
}

/** The unsigned decimal after the line starting with `head`, or -1. */
static int uint_after(const char *b, int n, const char *head)
{
    int i, j, at_line_start = 1;

    for (i = 0; i < n; i++) {
        if (at_line_start) {
            for (j = 0; head[j] && i + j < n && b[i + j] == head[j]; j++) {
            }
            if (!head[j]) {
                return first_uint(b + i + j, n - i - j);
            }
        }
        at_line_start = (b[i] == '\n');
    }
    return -1;
}

struct timespec_probe {
    long tv_sec;
    long tv_nsec;
};

/** Sleep `ms`, which parks the processor: the ticks it costs are idle ones. */
static void nap(long ms)
{
    struct timespec_probe req;

    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    syscall3(SYS_NANOSLEEP, (int)(size_t)&req, 0, 0);
}

/** Burn `ms` of wall time in user code, which parks nothing.
 *
 * Bounded by the monotonic clock rather than by an iteration count, so the
 * two halves of the idle test cost the same wall time on any host, and by a
 * source independent of the counters being measured. */
static void spin(long ms)
{
    struct timespec_probe start, now;
    volatile int x = 0;
    long elapsed = 0;

    syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)(size_t)&start, 0);
    while (elapsed < ms) {
        int i;

        for (i = 0; i < 20000; i++) {
            x = x + 1;
        }
        syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int)(size_t)&now, 0);
        elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000L;
    }
}

/**
 * Busy ticks, read out of /proc/stat's cpu line.
 *
 * The line is `cpu user nice system idle ...` and this kernel puts its one busy
 * figure in the system column, so the third number is what is wanted. Read from
 * a different file than the idle count on purpose: the two must agree, and they
 * cannot be checked against each other if one is derived from the other.
 */
static int busy_ticks_now(void)
{
    char t[320];
    int n, i, seen = 0, v;

    n = slurp("/proc/stat", t, (int)sizeof(t));
    if (n <= 0) {
        return -1;
    }
    for (i = 0; i < n && t[i] != '\n'; i++) {
        if (t[i] >= '0' && t[i] <= '9') {
            v = 0;
            while (i < n && t[i] >= '0' && t[i] <= '9') {
                v = v * 10 + (t[i] - '0');
                i++;
            }
            seen++;
            if (seen == 3) {
                return v;
            }
        }
    }
    return -1;
}

/** Total and idle ticks, read out of /proc/uptime's two second counts. */
static void ticks_now(int *total, int *idle)
{
    char t[64];
    int n, i, hundredths, seen = 0;

    n = slurp("/proc/uptime", t, (int)sizeof(t));
    *total = -1;
    *idle = -1;
    if (n <= 0) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (t[i] >= '0' && t[i] <= '9') {
            int whole = 0;

            while (i < n && t[i] >= '0' && t[i] <= '9') {
                whole = whole * 10 + (t[i] - '0');
                i++;
            }
            hundredths = 0;
            if (i < n && t[i] == '.') {
                i++;
                if (i < n && t[i] >= '0' && t[i] <= '9') {
                    hundredths += (t[i] - '0') * 10;
                    i++;
                }
                if (i < n && t[i] >= '0' && t[i] <= '9') {
                    hundredths += (t[i] - '0');
                    i++;
                }
            }
            if (seen == 0) {
                *total = whole * 100 + hundredths;
            } else {
                *idle = whole * 100 + hundredths;
                return;
            }
            seen++;
        }
    }
}

/** Build "/proc/self/fd/<n>". */
static void fd_path_of(char *dst, int fd)
{
    const char *head = "/proc/self/fd/";
    int i = 0, k = 0;
    char digits[8];

    while (head[i]) {
        dst[i] = head[i];
        i++;
    }
    do {
        digits[k++] = (char)('0' + fd % 10);
        fd /= 10;
    } while (fd > 0);
    while (k > 0) {
        dst[i++] = digits[--k];
    }
    dst[i] = '\0';
}

/* Whether the last row of /proc/net/tcp has the retrnsmt column as eight hex
   digits after the timer field and every one of the stack's own fields
   with a digit after its name. */
static int last_row_has_fields(const char *text, int n)
{
    int i, start = 0, hex = 0;
    const char *names[] = {
        " wnd=", " cwnd=", " flight=", " unsent=", " rto=", " zwp=", " refused=", " for="};
    unsigned k;

    for (i = 0; i + 1 < n; i++) {
        if (text[i] == '\n') {
            start = i + 1;
        }
    }
    for (i = start; i + 21 < n; i++) {
        if (text[i] == ' ' && text[i + 1] == '0' && text[i + 2] == '0' && text[i + 3] == ':') {
            int j;

            for (j = i + 13; j < i + 21; j++) {
                char c = text[j];

                if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
                    hex++;
                }
            }
            break;
        }
    }
    if (hex != 8) {
        return 0;
    }
    for (k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        int found = 0;

        for (i = start; i < n && !found; i++) {
            const char *p = names[k];
            int j = 0;

            while (p[j] && i + j < n && text[i + j] == p[j]) {
                j++;
            }
            if (!p[j] && i + j < n && text[i + j] >= '0' && text[i + j] <= '9') {
                found = 1;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

/**
 * The inode on the last row of /proc/net/tcp.
 *
 * Read backwards from the fixed columns every row ends with rather than by
 * counting fields forwards: the row has colons inside two of its fields, so
 * counting depends on a rule about separators that the reader does not share.
 * What both agree on is that the inode is the last number before that tail.
 */
static int last_row_inode(const char *b, int n)
{
    const char *tail = " 1 0000000000000000 100 0 0 10 0 wnd=";
    int taillen = (int)strlen(tail);
    int i, end, start, v = 0;

    for (i = n - taillen; i >= 0; i--) {
        int j;

        for (j = 0; j < taillen && b[i + j] == tail[j]; j++) {
        }
        if (j == taillen) {
            break;
        }
    }
    if (i < 0) {
        return -1;
    }
    end = i; /* one past the inode's last digit */
    while (end > 0 && b[end - 1] == ' ') {
        end--;
    }
    start = end;
    while (start > 0 && b[start - 1] >= '0' && b[start - 1] <= '9') {
        start--;
    }
    if (start == end) {
        return -1;
    }
    for (i = start; i < end; i++) {
        v = v * 10 + (b[i] - '0');
    }
    return v;
}

/** Count rows whose uid field is `want`, and rows whose uid is 0. */
static void count_row_owners(const char *b, int n, int want, int *matching, int *root)
{
    const char *tail = " 1 0000000000000000 100 0 0 10 0 wnd=";
    int taillen = (int)strlen(tail);
    int i, j;

    *matching = 0;
    *root = 0;
    for (i = 0; i + taillen <= n; i++) {
        int end, uid_end, uid_start, v = 0;

        for (j = 0; j < taillen && b[i + j] == tail[j]; j++) {
        }
        if (j != taillen) {
            continue;
        }
        /* Walk back over the inode, then over the timeout, to the uid. */
        end = i;
        while (end > 0 && b[end - 1] == ' ') {
            end--;
        }
        while (end > 0 && b[end - 1] >= '0' && b[end - 1] <= '9') {
            end--; /* the inode */
        }
        while (end > 0 && b[end - 1] == ' ') {
            end--;
        }
        while (end > 0 && b[end - 1] >= '0' && b[end - 1] <= '9') {
            end--; /* the timeout */
        }
        uid_end = end;
        while (uid_end > 0 && b[uid_end - 1] == ' ') {
            uid_end--;
        }
        uid_start = uid_end;
        while (uid_start > 0 && b[uid_start - 1] >= '0' && b[uid_start - 1] <= '9') {
            uid_start--;
        }
        for (j = uid_start; j < uid_end; j++) {
            v = v * 10 + (b[j] - '0');
        }
        if (uid_start == uid_end) {
            continue;
        }
        if (v == want) {
            (*matching)++;
        } else if (v == 0) {
            (*root)++;
        }
    }
}

/** Whether every row's inode is nonzero. */
/**
 * The whitespace-separated field at `idx` of the first line holding `needle`,
 * copied into `out` and NUL-terminated. Returns its length, or -1 if the line
 * is not there or has no such field.
 */
static int line_field(const char *b, int n, const char *needle, int idx, char *out, int cap)
{
    int i = 0, ls, le, f, p, len;

    while (i < n) {
        ls = i;
        while (i < n && b[i] != '\n') {
            i++;
        }
        le = i;
        if (i < n) {
            i++;
        }
        if (!contains(b + ls, le - ls, needle)) {
            continue;
        }
        p = ls;
        for (f = 0; f <= idx; f++) {
            while (p < le && (b[p] == ' ' || b[p] == '\t')) {
                p++;
            }
            if (p >= le) {
                return -1;
            }
            ls = p;
            while (p < le && b[p] != ' ' && b[p] != '\t') {
                p++;
            }
        }
        len = p - ls;
        if (len >= cap) {
            return -1;
        }
        for (f = 0; f < len; f++) {
            out[f] = b[ls + f];
        }
        out[len] = '\0';
        return len;
    }
    return -1;
}

static int no_row_has_zero_inode(const char *b, int n)
{
    const char *tail = " 1 0000000000000000 100 0 0 10 0 wnd=";
    int taillen = (int)strlen(tail);
    int i, j;

    for (i = 0; i + taillen <= n; i++) {
        int end, start, v = 0;

        for (j = 0; j < taillen && b[i + j] == tail[j]; j++) {
        }
        if (j != taillen) {
            continue;
        }
        end = i;
        while (end > 0 && b[end - 1] == ' ') {
            end--;
        }
        start = end;
        while (start > 0 && b[start - 1] >= '0' && b[start - 1] <= '9') {
            start--;
        }
        for (j = start; j < end; j++) {
            v = v * 10 + (b[j] - '0');
        }
        if (start == end || v == 0) {
            return 0;
        }
    }
    return 1;
}

/** 1 if the two NUL-terminated strings are equal. */
static int same_str(const char *a, const char *b)
{
    int i = 0;

    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/** Open an AF_INET stream socket through socketcall. Returns its fd or < 0. */
static int new_socket(unsigned int *args)
{
    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    return syscall3(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args, 0);
}

/** The inode fstat reports for a socket descriptor, or -1. `st` is scratch. */
static int socket_inode(int fd, char *st)
{
    if (fd < 0 || syscall3(SYS_FSTAT64, fd, (int)(size_t)st, 0) != 0) {
        return -1;
    }
    return (int)*(unsigned int *)(st + ST_INO64);
}

/**
 * Bind a socket to `port` on every address and listen. Returns 0 or -1.
 *
 * `sa` is scratch for the sockaddr_in, and `args` for the socketcall block.
 */
static int listen_on(int fd, int port, unsigned char *sa, unsigned int *args)
{
    int i;

    for (i = 0; i < 16; i++) {
        sa[i] = 0;
    }
    sa[0] = AF_INET;
    sa[2] = (unsigned char)((port >> 8) & 0xff);
    sa[3] = (unsigned char)(port & 0xff);
    args[0] = (unsigned int)fd;
    args[1] = (unsigned int)(size_t)sa;
    args[2] = 16;
    if (syscall3(SYS_SOCKETCALL, SC_BIND, (int)(size_t)args, 0) != 0) {
        return -1;
    }
    args[1] = 4;
    if (syscall3(SYS_SOCKETCALL, SC_LISTEN, (int)(size_t)args, 0) != 0) {
        return -1;
    }
    return 0;
}

/* The connection pool is sixteen blocks and a process may hold sixteen
   descriptors, three of them already standard, so filling the pool takes two
   processes. */
#define MAX_TCP_ROWS 16
#define FILL_CHILD   8
#define FILL_HERE    (MAX_TCP_ROWS - FILL_CHILD)

static char buf[4096];
static char path[32];

/* The one verdict, so the mount-only run and the whole run report alike. */
static void done(void)
{
    if (fails == 0) {
        puts("PROCPROBE OK");
    } else {
        puts("PROCPROBE FAIL");
    }
    sys_exit(fails);
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

/* Print the root's line from /proc/mounts, which is the one thing in this
   file that differs per boot: a root mounted from a disk, from the module
   through the page-cache window, or from the module in place. The suite
   outside boots three times and reads this line from each. */
static void print_rootmount(const char *b, int n)
{
    int i;

    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"PROCPROBE ROOTMOUNT ", 20);
    for (i = 0; i < n && b[i] != '\n'; i++) {
    }
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)b, i);
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"\n", 1);
}

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int pid, status, fd, n;

    /* `procprobe mount` reads the mount line and nothing else. The three
       boots that differ are booted for that line alone, and the rest of
       this probe would have them repeat, on each of them, work the shared
       probe boot already does once. */
    if (argc > 1 && argv[1][0] == 'm') {
        n = slurp("/proc/mounts", buf, (int)sizeof(buf));
        check(n > 0 && buf[n - 1] == '\n', "/proc/mounts ends with its newline");
        print_rootmount(buf, n);
        done();
    }

    /* The snapshot. The child exists when the file is opened and does not when
     * it is read, so what comes back can only have been taken at open. */
    pid = syscall1(SYS_FORK, 0);
    check(pid >= 0, "fork");
    if (pid == 0) {
        for (;;) {
            syscall1(SYS_PAUSE, 0);
        }
    }
    stat_path_of(path, pid);
    fd = syscall3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0);
    check(fd >= 0, "open /proc/<child>/stat while the child is alive");
    check(syscall3(SYS_KILL, pid, SIGKILL, 0) == 0, "kill the child");
    check(syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) == pid, "reap the child");
    /* The pid names nothing now. A generator asked here has nothing to answer
     * with; a snapshot answers what it took. */
    check(syscall3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0) < 0,
          "the dead child's /proc entry is gone, so the read below is a snapshot");
    if (fd >= 0) {
        n = syscall3(SYS_READ, fd, (int)(size_t)buf, (int)sizeof(buf) - 1);
        check(n > 0, "the open descriptor still reads after its process is gone");
        if (n > 0) {
            buf[n] = '\0';
            check(buf[0] >= '0' && buf[0] <= '9', "the snapshot starts with the pid");
        }
        syscall1(SYS_CLOSE, fd);
    }

    /* Every file this image can open comes back whole. Ending in a newline is
     * not enough to say so: a generator that stopped after its first line ends
     * in one too. So each file is asked for something only its LAST line has,
     * which is what a text cut short at any earlier point would be missing. */
    n = slurp("/proc/cmdline", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/cmdline ends with its newline");

    n = slurp("/proc/net/dev", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/net/dev ends with its newline");

    {
        static unsigned char st_loop[144], st_intr[144];

        check(syscall3(SYS_STAT64, (int)(size_t)"/proc/loop", (int)(size_t)st_loop, 0) == 0 &&
                  syscall3(SYS_STAT64, (int)(size_t)"/proc/interrupts", (int)(size_t)st_intr, 0) ==
                      0,
              "stat /proc/loop and /proc/interrupts");
        check(*(unsigned int *)(st_loop + ST_INO64) != *(unsigned int *)(st_intr + ST_INO64),
              "/proc/loop and /proc/interrupts have different inodes");
    }
    /* The header is two lines and the interface line is there only when the
     * machine has an interface, which a boot with no network device does not.
     * So what is required is that the text is COMPLETE: the tail of the second
     * header line, which only a whole header carries, and a whole counter row
     * whenever an interface was listed at all. */
    check(contains(buf, n, "carrier compressed\n"), "/proc/net/dev carries its whole header");
    check(lines_in(buf, n) == 2 || ends_with(buf, n, " 0 0 0 0 0 0\n"),
          "/proc/net/dev's interface line, when there is one, is a whole row");

    n = slurp("/proc/self/stat", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/self/stat ends with its newline");
    check(count_of(buf, n, ')') == 1, "/proc/self/stat closes the comm field");
    check(fields_in(buf, n) >= 24, "/proc/self/stat carries every field through to the last");

    n = slurp("/proc/self/cmdline", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\0', "/proc/self/cmdline ends with its NUL");

    /* A forked child keeps the name its parent had, which is what a daemon
     * that backgrounds itself without ever exec'ing has to go on. */
    {
        int kid, kid_status = -1;

        kid = syscall1(SYS_FORK, 0);
        check(kid >= 0, "fork for the name check");
        if (kid == 0) {
            char mine[256];
            int len = slurp("/proc/self/stat", mine, (int)sizeof(mine));

            sys_exit(len > 0 && contains(mine, len, "(procprobe)") ? 0 : 1);
        }
        if (kid > 0) {
            check(syscall3(SYS_WAITPID, kid, (int)(size_t)&kid_status, 0) == kid,
                  "the child of the name check was reaped");
            /* The whole status: a child killed by a signal has a zero exit
               byte and would otherwise read as one that exited cleanly. */
            check(kid_status == 0, "a forked child lost the name its parent had");
        }
    }

    n = slurp("/proc/disk", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/disk ends with its newline");
    check(has_line_starting(buf, n, "policy "), "/proc/disk names the build policy");

    /* The machine's own files. Each is asked for a field only its own last
     * line has, so a text cut short anywhere earlier fails here. */
    n = slurp("/proc/uptime", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/uptime ends with its newline");
    check(count_of(buf, n, '.') == 2, "/proc/uptime carries both of its numbers");

    n = slurp("/proc/stat", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/stat ends with its newline");
    check(has_line_starting(buf, n, "cpu "), "/proc/stat carries the cpu line");
    check(has_line_starting(buf, n, "btime "), "/proc/stat carries btime");
    check(has_line_starting(buf, n, "procs_blocked "), "/proc/stat runs through to its last line");

    n = slurp("/proc/meminfo", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/meminfo ends with its newline");
    check(has_line_starting(buf, n, "MemTotal:"), "/proc/meminfo carries MemTotal");
    check(has_line_starting(buf, n, "SwapFree:"), "/proc/meminfo runs through to its last line");
    check(uint_after(buf, n, "MemTotal:") >= uint_after(buf, n, "MemFree:"),
          "/proc/meminfo reports no more free memory than there is memory");

    n = slurp("/proc/mounts", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/mounts ends with its newline");
    check(lines_in(buf, n) == 3, "/proc/mounts names the root and the two synthetic mounts");
    check(ends_with(buf, n, " 0 0\n"), "/proc/mounts's last line is a whole fstab row");
    print_rootmount(buf, n);

    /* btime names one moment and must not move between two reads: a value
     * taken afresh from the clock each time would drift by whatever the read
     * cost, and anything comparing boots would be misled by it. */
    {
        int first_btime, second_btime;

        n = slurp("/proc/stat", buf, (int)sizeof(buf));
        first_btime = uint_after(buf, n, "btime ");
        /* Longer than a second, and deliberately so: an implementation taking
         * the clock afresh on each read answers the same value whenever the two
         * reads fall inside one second, so a shorter wait proves nothing. */
        nap(1500);
        n = slurp("/proc/stat", buf, (int)sizeof(buf));
        second_btime = uint_after(buf, n, "btime ");
        check(first_btime > 0, "btime is a value, not a missing line");
        check(first_btime == second_btime, "btime is the same across a second boundary");
    }

    /* Idle against busy, over two intervals of the same length. Sleeping parks
     * the processor and running does not, so the halves must move in opposite
     * directions; and the two together must account for every tick the timer
     * delivered.
     *
     * Busy is read from /proc/stat's own column rather than worked out as the
     * total less the idle, which is what an earlier version did: defined that
     * way the sum is an identity and holds however wrong both counters are.
     * Read from the two files the two generators fill, it is a real check that
     * they agree. */
    {
        int t0, i0, t1, i1, t2, i2;
        int b0, b1, b2;
        int idle_slept, busy_slept, idle_spun, busy_spun;

        ticks_now(&t0, &i0);
        b0 = busy_ticks_now();
        nap(600);
        ticks_now(&t1, &i1);
        b1 = busy_ticks_now();
        spin(600);
        ticks_now(&t2, &i2);
        b2 = busy_ticks_now();

        check(t0 >= 0 && i0 >= 0 && t1 >= 0 && i1 >= 0 && t2 >= 0 && i2 >= 0,
              "/proc/uptime parses on every read");
        check(b0 >= 0 && b1 >= 0 && b2 >= 0, "/proc/stat's busy column parses");
        idle_slept = i1 - i0;
        busy_slept = b1 - b0;
        idle_spun = i2 - i1;
        busy_spun = b2 - b1;

        check(idle_slept > busy_slept, "a sleeping machine spends more ticks idle than busy");
        check(idle_slept >= 20, "and enough of them to be a measurement");
        check(busy_spun > idle_spun,
              "a machine running user code spends more ticks busy than idle");
        check(busy_spun >= 20, "and enough of them to be a measurement");
        check((t2 - t0) == (i2 - i0) + (b2 - b0),
              "every delivered tick was counted once, as idle or as busy");
    }

    /* The network files, in the shapes their readers parse. A listening socket
     * is opened first so the tcp table has a row to describe; the row's own
     * inode is then required to be the number the descriptor reports through
     * both of the other two paths, which the constant printed before could
     * never have been. */
    {
        unsigned int args[6];
        int sfd, i, row_inode = -1, fd_inode = -1;
        unsigned char sa[16];
        char stbuf[128] = {0}; /* fstat64 fills it through an int-cast pointer */
        char link[64];
        char fdpath[32];

        /* An inode is a socket's own, and is not handed out twice. Two live
         * sockets must differ, and a socket opened after one of them closes
         * must differ from both: a constant, and a number taken from the pool
         * slot, each fail one of those. The constant this replaced was
         * `socket:[1]`, which the first socket of a boot really has. */
        {
            int ino_a, ino_b, ino_c, fd_a, fd_b, fd_c;

            fd_a = new_socket(args);
            fd_b = new_socket(args);
            check(fd_a >= 0 && fd_b >= 0, "two stream sockets open");
            ino_a = socket_inode(fd_a, stbuf);
            ino_b = socket_inode(fd_b, stbuf);
            check(ino_a > 0 && ino_b > 0, "each reports an inode");
            check(ino_a != ino_b, "and no two live sockets report the same one");
            syscall1(SYS_CLOSE, fd_a);
            fd_c = new_socket(args);
            check(fd_c >= 0, "a third socket opens into the freed slot");
            ino_c = socket_inode(fd_c, stbuf);
            check(ino_c != ino_a && ino_c != ino_b,
                  "and does not come back with an inode already used");
            syscall1(SYS_CLOSE, fd_b);
            syscall1(SYS_CLOSE, fd_c);
        }

        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        sfd = syscall3(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args, 0);
        check(sfd >= 0, "a stream socket opens");
        for (i = 0; i < 16; i++) {
            sa[i] = 0;
        }
        sa[0] = AF_INET;
        sa[2] = 0x1F; /* port 8080, big-endian */
        sa[3] = 0x90;
        args[0] = (unsigned int)sfd;
        args[1] = (unsigned int)(size_t)sa;
        args[2] = 16;
        check(syscall3(SYS_SOCKETCALL, SC_BIND, (int)(size_t)args, 0) == 0, "and binds");
        args[1] = 4;
        check(syscall3(SYS_SOCKETCALL, SC_LISTEN, (int)(size_t)args, 0) == 0, "and listens");

        n = slurp("/proc/net/route", buf, (int)sizeof(buf));
        check(n > 0 && buf[n - 1] == '\n', "/proc/net/route ends with its newline");
        check(has_line_starting(buf, n, "Iface"), "/proc/net/route carries its header");

        n = slurp("/proc/net/arp", buf, (int)sizeof(buf));
        check(n > 0 && buf[n - 1] == '\n', "/proc/net/arp ends with its newline");
        check(has_line_starting(buf, n, "IP address"), "/proc/net/arp carries its header");

        n = slurp("/proc/net/tcp", buf, (int)sizeof(buf));
        check(n > 0 && buf[n - 1] == '\n', "/proc/net/tcp ends with its newline");
        check(contains(buf, n, "inode\n"), "/proc/net/tcp carries its whole header");
        check(lines_in(buf, n) >= 2, "/proc/net/tcp lists the listening socket");
        check(ends_with(buf, n, "\n") && contains(buf, n, " 100 0 0 10 0 wnd="),
              "/proc/net/tcp's last row runs past the inode, which netstat needs");
        check(last_row_has_fields(buf, n),
              "/proc/net/tcp's last row has an eight-digit retransmission count and the "
              "stack's own fields with values");
        /* The listener's row is the one whose local port is 1F90. Its inode is
         * the last number before the trailing fixed columns. */
        row_inode = last_row_inode(buf, n);
        check(row_inode > 1,
              "/proc/net/tcp names an inode past the first, so a constant cannot match");
        /* The state column is Linux's number, not this stack's own: LISTEN is
         * 0A there and something else here, so a cast would print the wrong
         * name in netstat. */
        {
            char st[8];

            check(line_field(buf, n, ":1F90 ", 3, st, (int)sizeof(st)) == 2 && st[0] == '0' &&
                      st[1] == 'A',
                  "the listening socket's row says 0A, the number netstat reads as LISTEN");
        }

        check(syscall3(SYS_FSTAT64, sfd, (int)(size_t)stbuf, 0) == 0, "fstat the socket");
        fd_inode = (int)*(unsigned int *)(stbuf + ST_INO64);
        check(fd_inode == row_inode, "fstat reports the inode /proc/net/tcp does");

        fd_path_of(fdpath, sfd);
        n = syscall3(SYS_READLINK, (int)(size_t)fdpath, (int)(size_t)link, (int)sizeof(link) - 1);
        check(n > 0, "readlink the socket descriptor");
        if (n > 0) {
            link[n] = '\0';
            check(contains(link, n, "socket:["), "the fd names a socket");
            check(first_uint(link, n) == row_inode,
                  "and the same inode, so one socket is one thing in all three");
        }
        syscall1(SYS_CLOSE, sfd);
    }

    /* A descriptor open on one of the network files names that file, and
     * reports the identity the path it was opened by reports. The three of them
     * were added to the open dispatcher and could have been left out of these
     * two, where a file with no entry falls through to /proc/0/stat. */
    {
        static const char *const netfiles[3] = {"/proc/net/route", "/proc/net/arp",
                                                "/proc/net/tcp"};
        char st_by_fd[128] = {0};   /* fstat64 fills it through an int-cast pointer */
        char st_by_path[128] = {0}; /* and stat64 the same */
        char pathbuf[128];
        char link[64];
        char fdpath[32];
        unsigned int by_fd, by_path;
        int netfd, i, k;

        for (i = 0; i < 3; i++) {
            for (k = 0; netfiles[i][k]; k++) {
                pathbuf[k] = netfiles[i][k];
            }
            pathbuf[k] = '\0';
            netfd = syscall3(SYS_OPEN, (int)(size_t)pathbuf, O_RDONLY, 0);
            check(netfd >= 0, "one of the network files opens");
            fd_path_of(fdpath, netfd);
            n = syscall3(SYS_READLINK, (int)(size_t)fdpath, (int)(size_t)link,
                         (int)sizeof(link) - 1);
            check(n > 0, "its descriptor has a link to read");
            if (n > 0) {
                link[n] = '\0';
                check(same_str(link, pathbuf),
                      "and the link names the file it was opened by, not another one");
            }
            check(syscall3(SYS_FSTAT64, netfd, (int)(size_t)st_by_fd, 0) == 0, "fstat it");
            by_fd = *(unsigned int *)(st_by_fd + ST_INO64);
            check(syscall3(SYS_STAT64, (int)(size_t)pathbuf, (int)(size_t)st_by_path, 0) == 0,
                  "and stat its path");
            by_path = *(unsigned int *)(st_by_path + ST_INO64);
            check(by_fd == by_path && by_fd != 0,
                  "the descriptor and the path report one identity");
            syscall1(SYS_CLOSE, netfd);
        }
    }

    /* Capacity. The pool is filled to its last block, half in a child and half
     * here, because one process cannot hold that many descriptors, and the file
     * must then come back with every row and nothing cut: this is what the
     * declared bound is for, and three rows never reached it. */
    {
        unsigned int args[6];
        unsigned char sa[16];
        int fds[FILL_HERE];
        int i, filler_pid, child_status;

        filler_pid = syscall1(SYS_FORK, 0);
        check(filler_pid >= 0, "fork for the second half of the pool");
        if (filler_pid == 0) {
            for (i = 0; i < FILL_CHILD; i++) {
                int lfd = new_socket(args);
                if (lfd < 0 || listen_on(lfd, 9000 + i, sa, args) != 0) {
                    sys_exit(31);
                }
            }
            nap(3000); /* hold them while the parent reads the table */
            sys_exit(0);
        }
        nap(500);
        for (i = 0; i < FILL_HERE; i++) {
            fds[i] = new_socket(args);
            check(fds[i] >= 0 && listen_on(fds[i], 9100 + i, sa, args) == 0,
                  "a listening socket for the parent's half of the pool");
        }

        n = slurp("/proc/net/tcp", buf, (int)sizeof(buf));
        check(n > 0, "/proc/net/tcp reads with the pool full");
        check(n < (int)sizeof(buf) - 1, "and the whole file fit in one read, so nothing was cut");
        check(lines_in(buf, n) == MAX_TCP_ROWS + 1,
              "a full pool prints a header and one row for every block");
        check(ends_with(buf, n, "\n") && contains(buf, n, " 100 0 0 10 0 wnd="),
              "and the last of them is a whole row");

        for (i = 0; i < FILL_HERE; i++) {
            syscall1(SYS_CLOSE, fds[i]);
        }
        syscall3(SYS_WAITPID, filler_pid, (int)(size_t)&child_status, 0);
        check(child_status == 0, "the child that held the other half exited cleanly");
    }

    /* Whose connection it is, and whether the row still says so once the
     * descriptor has gone. A listener owned by an ordinary account is made in a
     * child, and root connects to it: the accepted connection is allocated in
     * receive context, where no process is calling, so an implementation
     * taking the current process's uid would report root and pass a test whose
     * two sides had the same owner. */
    {
        unsigned int args[6];
        unsigned char sa[16];
        int lfd, cfd, child_status, listener_pid, i;
        int owner_rows = 0, root_rows = 0;

        listener_pid = syscall1(SYS_FORK, 0);
        check(listener_pid >= 0, "fork for the listener");
        if (listener_pid == 0) {
            /* The child owns the listener, at a uid the parent lacks. */
            if (syscall3(SYS_SETRESUID32, 1000, 1000, 1000) != 0) {
                sys_exit(21);
            }
            args[0] = AF_INET;
            args[1] = SOCK_STREAM;
            args[2] = 0;
            lfd = syscall3(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args, 0);
            if (lfd < 0) {
                sys_exit(22);
            }
            for (i = 0; i < 16; i++) {
                sa[i] = 0;
            }
            sa[0] = AF_INET;
            sa[2] = 0x1F;
            sa[3] = 0x91; /* port 8081 */
            args[0] = (unsigned int)lfd;
            args[1] = (unsigned int)(size_t)sa;
            args[2] = 16;
            if (syscall3(SYS_SOCKETCALL, SC_BIND, (int)(size_t)args, 0) != 0) {
                sys_exit(23);
            }
            args[1] = 4;
            if (syscall3(SYS_SOCKETCALL, SC_LISTEN, (int)(size_t)args, 0) != 0) {
                sys_exit(24);
            }
            /* Hold it open while the parent connects and reads the table. */
            nap(2500);
            sys_exit(0);
        }
        nap(400);
        args[0] = AF_INET;
        args[1] = SOCK_STREAM;
        args[2] = 0;
        cfd = syscall3(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args, 0);
        check(cfd >= 0, "root opens a connecting socket");
        for (i = 0; i < 16; i++) {
            sa[i] = 0;
        }
        sa[0] = AF_INET;
        sa[2] = 0x1F;
        sa[3] = 0x91;
        sa[4] = 127;
        sa[7] = 1; /* 127.0.0.1 */
        args[0] = (unsigned int)cfd;
        args[1] = (unsigned int)(size_t)sa;
        args[2] = 16;
        check(syscall3(SYS_SOCKETCALL, SC_CONNECT, (int)(size_t)args, 0) == 0,
              "root connects to the child's listener over loopback");

        n = slurp("/proc/net/tcp", buf, (int)sizeof(buf));
        check(n > 0, "/proc/net/tcp reads with connections live");
        count_row_owners(buf, n, 1000, &owner_rows, &root_rows);
        check(owner_rows >= 2, "the listener and the connection it accepted both belong to the "
                               "account that listened, not to whoever was running");
        check(root_rows >= 1, "and root's own connecting socket belongs to root");

        /* Capacity: the table must come back whole however many rows it has,
         * which is what the bound is for. */
        check(ends_with(buf, n, "\n") && contains(buf, n, " 100 0 0 10 0 wnd="),
              "a table with several rows still ends with a whole row");

        syscall1(SYS_CLOSE, cfd);
        /* A closed connection stays in the pool while it finishes closing, and
         * its socket is detached the moment the descriptor goes. The row must
         * still name its owner and its inode, which reading them through the
         * socket could not do. */
        n = slurp("/proc/net/tcp", buf, (int)sizeof(buf));
        check(n > 0 && no_row_has_zero_inode(buf, n),
              "a row whose socket is detached still reports its inode");
        syscall3(SYS_WAITPID, listener_pid, (int)(size_t)&child_status, 0);
    }

    done();
}
