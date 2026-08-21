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
 *
 * Prints "PROCPROBE OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_WAITPID 7
#define SYS_GETPID  20
#define SYS_KILL    37
#define SYS_PAUSE   29

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

static char buf[2048];
static char path[32];

void _start(void)
{
    int pid, status, fd, n;

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
    check(lines_in(buf, n) >= 3,
          "/proc/net/dev carries its interface line, not only the two headers");
    check(ends_with(buf, n, " 0 0 0 0 0 0\n"), "/proc/net/dev's last line is a whole counter row");

    n = slurp("/proc/self/stat", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/self/stat ends with its newline");
    check(count_of(buf, n, ')') == 1, "/proc/self/stat closes the comm field");
    check(fields_in(buf, n) >= 24, "/proc/self/stat carries every field through to the last");

    n = slurp("/proc/self/cmdline", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\0', "/proc/self/cmdline ends with its NUL");

    n = slurp("/proc/disk", buf, (int)sizeof(buf));
    check(n > 0 && buf[n - 1] == '\n', "/proc/disk ends with its newline");
    check(has_line_starting(buf, n, "policy "), "/proc/disk names the build policy");

    if (fails == 0) {
        puts("PROCPROBE OK");
    } else {
        puts("PROCPROBE FAIL");
    }
    sys_exit(fails);
}
