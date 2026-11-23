/*
 * Time bare x86-64 system calls with no C library, so that Linux and Rach2
 * run the same user instructions around each syscall instruction.
 *
 * Build: gcc -O2 -ffreestanding -static -nostdlib -fno-stack-protector -fno-pie
 *        -no-pie -o barecall barecall.c
 * Use:   barecall [FILE [COUNT [ROUNDS [KIND]]]]
 *
 * Each round times COUNT calls of each kind in turn, 200000 unless given: a
 * number no table holds, getpid, lseek(fd, 0, SEEK_CUR) and fstat on FILE's
 * descriptor, a one-byte pread64 at 0, and an open and close of FILE. Then,
 * with the working directory at FILE's directory, COUNT/20 each of FILE's
 * name opened by its name alone with O_RDWR|O_CREAT|O_CLOEXEC, as the
 * shader cache's opens are, and the close of each at once, timed apart by
 * the time-stamp counter, and unlinks of a name the directory does not hold. ROUNDS
 * rounds, 9 unless given, give each kind's median, least and most, in
 * nanoseconds a call by CLOCK_MONOTONIC. KIND, one of the names below, runs
 * that kind alone, COUNT times a round, for a profile of its kernel time.
 *
 * As PID 1 in a Linux initramfs it loads /virtio_blk.ko.xz, mounts devtmpfs
 * and the ext2 volume on /dev/vda at /mnt by the ext4 driver, which takes
 * extra_isize where Linux's ext2 mode does not, without access times; times
 * /mnt/f, or /init when there is no module, and powers the machine off.
 */

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_pread64 17
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_chdir 80
#define SYS_unlink 87
#define SYS_exit 60
#define SYS_mount 165
#define SYS_reboot 169
#define SYS_clock_gettime 228
#define SYS_finit_module 313
#define SYS_none 1023 /* above every table: ENOSYS from dispatch */

#define MS_NOATIME 1024
#define MODULE_INIT_COMPRESSED_FILE 4
#define KINDS 9
#define PATH_KINDS 5 /* the kinds from here on run COUNT/20 times */
#define O_RDWR 2
#define O_CREAT 0100
#define O_CLOEXEC 02000000
#define MAX_ROUNDS 31

/** System call N with three arguments; its result. */
static long sys3(long n, long a, long b, long c)
{
    long r;

    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/** System call N with four arguments; its result. */
static long sys4(long n, long a, long b, long c, long d)
{
    long r;
    register long r10 __asm__("r10") = d;

    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

/** System call N with five arguments; its result. */
static long sys5(long n, long a, long b, long c, long d, long e)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;

    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

/** The length of the string S. */
static unsigned long length(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    return n;
}

/** Write the string S to standard output. */
static void put(const char *s)
{
    sys3(SYS_write, 1, (long)s, (long)length(s));
}

/** Write V in decimal. */
static void put_num(unsigned long v)
{
    char buf[24];
    int i = sizeof(buf);

    buf[--i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    put(buf + i);
}

/** Write V/1000 with three decimals. */
static void put_milli(unsigned long v)
{
    put_num(v / 1000);
    put(".");
    put_num(v % 1000 / 100);
    put_num(v % 100 / 10);
    put_num(v % 10);
}

/** The decimal number S, or DEF when S is absent, empty or 0. */
static unsigned long parse(const char *s, unsigned long def)
{
    unsigned long v = 0;

    if (!s || !*s)
        return def;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (unsigned long)(*s++ - '0');
    return v ? v : def;
}

/** CLOCK_MONOTONIC in nanoseconds. */
static unsigned long now_ns(void)
{
    long ts[2];

    sys3(SYS_clock_gettime, 1, (long)ts, 0);
    return (unsigned long)ts[0] * 1000000000UL + (unsigned long)ts[1];
}

/** Power the machine off when PID1 is set, else exit. */
static void finish(int pid1)
{
    if (pid1)
        sys4(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0);
    sys3(SYS_exit, 0, 0, 0);
}

/** The time-stamp counter, after the instructions before it. */
static unsigned long tsc(void)
{
    unsigned int lo, hi;

    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (unsigned long)hi << 32 | lo;
}

/**
 * Open NAME COUNT times with O_RDWR|O_CREAT|O_CLOEXEC and close each
 * descriptor at once, timing the open and the close apart by the
 * time-stamp counter, whose rate the round's CLOCK_MONOTONIC gives; store
 * each's mean in picoseconds in *OPEN_PS and *CLOSE_PS.
 */
static void open_close_apart(const char *name, unsigned long count,
                             unsigned long *open_ps, unsigned long *close_ps)
{
    unsigned long i, a, b, c, opens = 0, closes = 0, t0, t1, c0, c1;
    long fd;

    t0 = now_ns();
    c0 = tsc();
    for (i = 0; i < count; i++) {
        a = tsc();
        fd = sys3(SYS_open, (long)name, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        b = tsc();
        sys3(SYS_close, fd, 0, 0);
        c = tsc();
        opens += b - a;
        closes += c - b;
    }
    c1 = tsc();
    t1 = now_ns();
    /* cycles a call, times 1000, times nanoseconds a cycle */
    *open_ps = opens * 1000UL / count * (t1 - t0) / (c1 - c0);
    *close_ps = closes * 1000UL / count * (t1 - t0) / (c1 - c0);
}

/**
 * Time COUNT calls of kind K on FD, or opens and closes of PATH, and return
 * the mean in picoseconds.
 */
static unsigned long run(int k, long fd, const char *path, unsigned long count)
{
    static long st[32];
    unsigned long i, t0, t1;

    t0 = now_ns();
    for (i = 0; i < count; i++) {
        switch (k) {
        case 0:
            sys3(SYS_none, 0, 0, 0);
            break;
        case 1:
            sys3(SYS_getpid, 0, 0, 0);
            break;
        case 2:
            sys3(SYS_lseek, fd, 0, 1);
            break;
        case 3:
            sys3(SYS_fstat, fd, (long)st, 0);
            break;
        case 4:
            sys4(SYS_pread64, fd, (long)st, 1, 0);
            break;
        case 5:
            sys3(SYS_close, sys3(SYS_open, (long)path, 0, 0), 0, 0);
            break;
        default:
            sys3(SYS_unlink, (long)path, 0, 0);
            break;
        }
    }
    t1 = now_ns();
    return (t1 - t0) * 1000UL / count;
}

static const char *const names[KINDS] = {
    "enosys",    "getpid", "lseek",  "fstat",  "pread1",
    "openclose", "openrw", "closerw", "unlinkx"};

/** The index of the kind named S, or -1. */
static int kind_named(const char *s)
{
    int k, i;

    for (k = 0; k < KINDS; k++) {
        for (i = 0; s[i] && s[i] == names[k][i]; i++)
            ;
        if (!s[i] && !names[k][i])
            return k;
    }
    return -1;
}

/** Write that the setup call WHAT failed with the error -RC. */
static void put_error(const char *what, long rc)
{
    put("barecall: ");
    put(what);
    put(" failed: errno ");
    put_num((unsigned long)-rc);
    put("\n");
}

/** Make FILE's directory the working directory; FILE's last name. */
static const char *enter_dir(const char *file)
{
    static char dir[256];
    unsigned long n = length(file), slash = n;
    long rc;

    while (slash > 0 && file[slash - 1] != '/')
        slash--;
    if (slash == 0 || slash >= sizeof(dir))
        return file;
    for (n = 0; n < slash; n++)
        dir[n] = file[n];
    dir[slash] = 0;
    rc = sys3(SYS_chdir, (long)dir, 0, 0);
    if (rc != 0)
        put_error("chdir", rc);
    return file + slash;
}

/** Load the disk driver and mount the test volume at /mnt; 0 or -1. */
static int mount_volume(void)
{
    long mod = sys3(SYS_open, (long)"/virtio_blk.ko.xz", 0, 0);
    long ts[2] = {0, 10000000};
    long rc = 0;
    int i;

    if (mod < 0)
        return -1;
    rc = sys3(SYS_finit_module, mod, (long)"", MODULE_INIT_COMPRESSED_FILE);
    if (rc != 0)
        put_error("finit_module", rc);
    rc = sys5(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);
    if (rc != 0)
        put_error("devtmpfs mount", rc);
    for (i = 0; i < 500; i++) {
        rc = sys5(SYS_mount, (long)"/dev/vda", (long)"/mnt", (long)"ext4", MS_NOATIME, 0);
        if (rc == 0)
            return 0;
        sys3(SYS_nanosleep, (long)ts, 0, 0); /* while the disk appears */
    }
    put_error("mount", rc);
    return -1;
}

/** Sort the first N of V in place, least first. */
static void sort(unsigned long *v, unsigned long n)
{
    unsigned long a, b, t;

    for (a = 1; a < n; a++)
        for (b = a; b > 0 && v[b - 1] > v[b]; b--) {
            t = v[b];
            v[b] = v[b - 1];
            v[b - 1] = t;
        }
}

/** The program, from the stack the kernel left at SP. */
void start_c(long *sp)
{
    long argc = sp[0];
    char **argv = (char **)(sp + 1);
    int pid1 = sys3(SYS_getpid, 0, 0, 0) == 1;
    const char *file = argc > 1 ? argv[1] : "/init";
    unsigned long count = parse(argc > 2 ? argv[2] : 0, 200000);
    unsigned long rounds = parse(argc > 3 ? argv[3] : 0, 9);
    unsigned long ps[KINDS][MAX_ROUNDS];
    unsigned long r, fewer, unused;
    const char *name;
    long fd;
    int k, only = argc > 4 ? kind_named(argv[4]) : -1;

    if (pid1 && mount_volume() == 0)
        file = "/mnt/f";
    if (rounds > MAX_ROUNDS)
        rounds = MAX_ROUNDS;
    fd = sys3(SYS_open, (long)file, 0, 0);
    if (fd < 0) {
        put_error("open", fd);
        finish(pid1);
    }
    name = enter_dir(file);
    if (only >= 0) {
        for (r = 0; r < rounds; r++) {
            if (only == 6 || only == 7)
                open_close_apart(name, count, &ps[6][0], &ps[7][0]);
            else
                unused = run(only, fd, only == 8 ? "absent" : file, count);
        }
        (void)unused;
        put("barecall: only ");
        put(names[only]);
        put(" done\n");
        finish(pid1);
    }
    fewer = count / 20 ? count / 20 : 1;
    /* warm the paths and the TCG blocks */
    for (k = 0; k < PATH_KINDS; k++)
        run(k, fd, file, count / 10);
    run(5, fd, file, fewer / 10 + 1);
    open_close_apart(name, fewer / 10 + 1, &ps[6][0], &ps[7][0]);
    run(8, fd, "absent", fewer / 10 + 1);
    for (r = 0; r < rounds; r++) {
        put("barecall: round ");
        put_num(r);
        for (k = 0; k < PATH_KINDS; k++)
            ps[k][r] = run(k, fd, file, count);
        ps[5][r] = run(5, fd, file, fewer);
        open_close_apart(name, fewer, &ps[6][r], &ps[7][r]);
        ps[8][r] = run(8, fd, "absent", fewer);
        for (k = 0; k < KINDS; k++) {
            put(" ");
            put_milli(ps[k][r]);
        }
        put("\n");
    }
    put("barecall: count ");
    put_num(count);
    put(" rounds ");
    put_num(rounds);
    put(" file ");
    put(file);
    put("\n");
    for (k = 0; k < KINDS; k++) {
        sort(ps[k], rounds);
        put("barecall: ");
        put(names[k]);
        put(": median ");
        put_milli(ps[k][rounds / 2]);
        put(" ns, min ");
        put_milli(ps[k][0]);
        put(" ns, max ");
        put_milli(ps[k][rounds - 1]);
        put(" ns\n");
    }
    finish(pid1);
}

__asm__(".globl _start\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call start_c\n"
        "    hlt\n");
