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
    /* Printed, not only checked: which source and options the root got is the
     * one thing about this file that differs per boot, and the three-boot test
     * outside reads it from here rather than guessing. */
    {
        int i;

        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"PROCPROBE ROOTMOUNT ", 20);
        for (i = 0; i < n && buf[i] != '\n'; i++) {
        }
        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)buf, i);
        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"\n", 1);
    }

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

    if (fails == 0) {
        puts("PROCPROBE OK");
    } else {
        puts("PROCPROBE FAIL");
    }
    sys_exit(fails);
}
