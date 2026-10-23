/**
 * loadprobe: the machine under load, and the longest delays it saw.
 *
 * Six children spin, two write a file of 256 KiB on the mounted volume and
 * sync it over and over, one pings the gateway through the kernel's own
 * ping every fifty milliseconds, and one reads lines from the console,
 * which the case's scenario types one after another, each once the reader
 * has said it took the last. The load lasts until the reader has reported
 * LOAD_LINES lines over a pipe, or until the pipe ends because the reader
 * did; however long the scenario and the loaded machine take, the parent
 * waits for the lines and not for a time. Then the parent kills them all,
 * reads `/proc/stat`, and prints the four delays the kernel kept as its
 * longest: the gap between two timer ticks, a block request from its
 * taking to its completion, a wait for the request limit, and a console
 * line from readable to read. The four delays depend on the machine and
 * are printed, not judged; what is judged is counted: every line the
 * scenario typed must have been read, and no request may have been
 * quarantined by its deadline, which /proc/disk counts. The case's bound
 * ends a run whose reader never gets its lines.
 *
 * The volume is /dev/disk0p1, mounted on /lo, and the pinger pings QEMU's
 * gateway, 10.0.2.2. A native run gives its own as arguments
 * (probe_args.h): the first device of `loops=DEVICE,...` is the volume,
 * `dir=PATH` its mount point, and `gateway=ADDRESS` the machine's gateway,
 * which the pinger pings. With `machine=native` the probe leaves out the
 * check of /proc/disk's quarantines, which counts the requests of every
 * disk of a running machine.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, machine.load.lag without it: `mounted`, the volume mounted;
 * `started`, the report pipe and the ten load processes made; `killed`, each
 * load process ended by the kill and none by itself; `latencies`, the lines
 * of /proc/stat for the timer, the disk and the console read; `lines`,
 * every line typed read, not run without the reader; `no-quarantine`,
 * /proc/disk's quarantines unchanged, not run with `machine=native`; and
 * `unmounted`, the volume unmounted and its mount point removed. Beside
 * them it prints "LOADPROBE timer N disk N diskwait N console N lines N" and
 * then "LOADPROBE OK" and exits 0, or "LOADPROBE FAIL: " and the reason and
 * exits 1.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define GATEWAY_IP 0x0A000202u

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define SIGKILL  9

/* The lines the scenario of machine.load types, one after each read. */
#define LOAD_LINES 6

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output. */
static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    print("\n");
}

/** Write `v` in decimal to standard output. */
static void print_num(unsigned v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        syscall3(SYS_WRITE, 1, (long)&tmp[--n], 1);
    }
}

/** Report the failure `why` of the planned assertion now checked, print
 * "LOADPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    report_stop(why);
    print("LOADPROBE FAIL: ");
    puts(why);
    sys_exit(1);
}

/* The volume's node and mount point, the two writers' files on it, and the
 * address the pinger pings, in host order. */
static const char *node, *mount_dir;
static char load_path[2][128];
static unsigned ping_ip = GATEWAY_IP;

/** Copy `dir` and `name` into `out`, of 128 bytes, as one string, cut short
 * where `out` is full. */
static void join(char *out, const char *dir, const char *name)
{
    int n = 0;

    while (*dir && n < 127) {
        out[n++] = *dir++;
    }
    while (*name && n < 127) {
        out[n++] = *name++;
    }
    out[n] = '\0';
}

/** The IPv4 address at the start of `s`, A.B.C.D, in host order, or
 * `fallback` when `s` is NULL or does not begin with one. */
static unsigned parse_ip(const char *s, unsigned fallback)
{
    unsigned ip = 0;
    int part;

    if (s == 0) {
        return fallback;
    }
    for (part = 0; part < 4; part++) {
        unsigned v = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (unsigned)(*s++ - '0');
            digits++;
        }
        if (digits == 0 || v > 255 || (part < 3 && *s++ != '.')) {
            return fallback;
        }
        ip = (ip << 8) | v;
    }
    return ip;
}

/** Take the volume's node, its mount point and the pinger's address from
 * the arguments, with QEMU's as the defaults. */
static void take_arguments(void)
{
    static char first[64];
    const char *loops = probe_arg("loops", "");
    int n;

    for (n = 0; n < (int)sizeof(first) - 1 && loops[n] != '\0' && loops[n] != ','; n++) {
        first[n] = loops[n];
    }
    first[n] = '\0';
    node = n > 0 ? first : "/dev/disk0p1";
    mount_dir = probe_arg("dir", "/lo");
    join(load_path[0], mount_dir, "/load1.bin");
    join(load_path[1], mount_dir, "/load2.bin");
    ping_ip = parse_ip(probe_arg("gateway", 0), GATEWAY_IP);
}

/** Close the report reader and remove the mount after the loads end. */
static void cleanup_mount(int report)
{
    syscall1(SYS_CLOSE, report);
    if (syscall2(SYS_UMOUNT2, (long)mount_dir, 0) != 0 ||
        syscall1(SYS_RMDIR, (long)mount_dir) != 0) {
        fail("unmount and remove /lo");
    }
}

/** Sleep for `ms` milliseconds. */
static void sleep_ms(int ms)
{
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

static unsigned char block[4096];

/* Whether a writer syncs after each pass; the argument's `n` clears it. */
static int writer_syncs = 1;

/** A writer: write the file `path` whole, 64 blocks of 4 KiB, close it and,
 * when `writer_syncs` is set, sync, again and again. It exits 2 when the
 * open fails and 3 when a write is short. */
static void writer(const char *path)
{
    unsigned i;

    for (i = 0; i < sizeof(block); i++) {
        block[i] = (unsigned char)i;
    }
    for (;;) {
        int fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd < 0) {
            sys_exit(2);
        }
        for (i = 0; i < 64; i++) {
            if (syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block)) != (int)sizeof(block)) {
                sys_exit(3);
            }
        }
        syscall1(SYS_CLOSE, fd);
        if (writer_syncs) {
            syscall1(SYS_SYNC, 0);
        }
    }
}

/** The pinger: the kernel's ping of the gateway every 50 ms, forever. */
static void pinger(void)
{
    unsigned long args[3];

    for (;;) {
        args[0] = ping_ip;
        sock_call(SC_PING, args);
        sleep_ms(50);
    }
}

/** The console reader: print "LOADPROBE read" for each line read, after
 * which the scenario types the next line, and then count the line into
 * `report`, one byte each. The answer comes first, so that the parent, which
 * kills the reader at the last line's byte, does so after the scenario has
 * seen the last answer. It does not return. */
static void reader(int report)
{
    char c;

    for (;;) {
        if (syscall3(SYS_READ, 0, (long)&c, 1) == 1 && c == '\n') {
            puts("LOADPROBE read");
            syscall3(SYS_WRITE, report, (long)"l", 1);
        }
    }
}

/** The number after `key` on the line of the file `path` that begins with it,
 * or 0xFFFFFFFF when the file or the line is absent or the file does not fit
 * in 32 KiB. The whole file is read, since /proc/stat has a line for each
 * processor before the ones read here. */
static unsigned file_field(const char *path, const char *key)
{
    static char text[32768];
    int fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
    int n = 0, got, i;

    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    while ((got = syscall3(SYS_READ, fd, (long)(text + n), (int)sizeof(text) - 1 - n)) > 0) {
        n += got;
    }
    syscall1(SYS_CLOSE, fd);
    if (got < 0 || n <= 0 || n == (int)sizeof(text) - 1) {
        return 0xFFFFFFFFu;
    }
    text[n] = 0;
    for (i = 0; i < n; i++) {
        int k;

        for (k = 0; key[k] && text[i + k] == key[k]; k++) {
        }
        if (!key[k] && (i == 0 || text[i - 1] == '\n')) {
            unsigned v = 0;

            i += k;
            while (text[i] == ' ') {
                i++;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (unsigned)(text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0xFFFFFFFFu;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Non-zero when the argument `arg` has the letter `kind`, or when there is
 * no argument. */
static int wanted(const char *arg, char kind)
{
    int i;

    if (!arg) {
        return 1;
    }
    for (i = 0; arg[i]; i++) {
        if (arg[i] == kind) {
            return 1;
        }
    }
    return 0;
}

/**
 * Run load process `i` of the ten, as the argument `arg` asks: 0 to 5 spin,
 * 6 and 7 write, 8 pings and 9 reads the console. Only the reader keeps the
 * write end of `report`, so the pipe ends when it does. A process whose load
 * is not wanted waits in pause until it is killed. It does not return.
 */
static void load_child(int i, const char *arg, const int report[2])
{
    syscall1(SYS_CLOSE, report[0]);
    if (i != 9 || !wanted(arg, 'r')) {
        syscall1(SYS_CLOSE, report[1]);
    }
    if (i < 6) {
        if (wanted(arg, 's')) {
            for (;;) {
                /* A spin, meant to last until the kill. */
                __asm__ __volatile__("" ::: "memory");
            }
        }
    } else if (i < 8) {
        writer_syncs = !(arg && wanted(arg, 'n'));
        if (wanted(arg, 'w') || (i == 6 && wanted(arg, 'W'))) {
            writer(i == 6 ? load_path[0] : load_path[1]);
        }
    } else if (i == 8) {
        if (wanted(arg, 'p')) {
            pinger();
        }
    } else if (wanted(arg, 'r')) {
        reader(report[1]);
    }
    for (;;) {
        syscall0(SYS_PAUSE);
    }
}

/**
 * The probe, entered with `sp` at argc. Every load runs unless the first
 * argument without `=` names some by their first letters: `s`pinners,
 * `w`riters, `W` for the first writer alone, `p`inger, `r`eader. With such
 * an argument, `n` makes the writers skip the sync and `k` mounts the volume
 * as KRFFS in place of FAT32. A child whose load is not wanted waits in
 * pause until it is killed. Without the reader nothing reports lines, and
 * the load is killed as soon as every child has closed its end of the report
 * pipe.
 */
void probe_main(unsigned long *sp)
{
    const char *arg;
    int pids[10];
    int report[2];
    int i, lines = 0;
    unsigned timer, disk, diskwait, console, quarantines;
    char byte;

    probe_args((int)sp[0], (char **)(sp + 1));
    arg = probe_word(1);
    take_arguments();
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "machine.load.lag"));
    report_plan("mounted");
    report_plan("started");
    report_plan("killed");
    report_plan("latencies");
    report_plan("lines");
    report_plan("no-quarantine");
    report_plan("unmounted");
    if (syscall2(SYS_MKDIR, (long)mount_dir, 0755) != 0 ||
        syscall5(SYS_MOUNT, (long)node, (long)mount_dir,
                 (long)(wanted(arg, 'k') && arg ? "krffs" : "vfat"), 0, 0) != 0) {
        fail("the mount of the volume");
    }
    report_held();
    report[0] = report[1] = -1;
    if (syscall1(SYS_PIPE, (long)report) != 0) {
        fail("the report pipe");
    }
    for (i = 0; i < 10; i++) {
        pids[i] = syscall1(SYS_FORK, 0);
        if (pids[i] < 0) {
            fail("fork of a load process");
        }
        if (pids[i] == 0) {
            load_child(i, arg, report);
        }
    }
    syscall1(SYS_CLOSE, report[1]);
    report_held();
    quarantines = file_field("/proc/disk", "disk-quarantines ");
    while (lines < LOAD_LINES && syscall3(SYS_READ, report[0], (long)&byte, 1) == 1) {
        lines++;
    }
    for (i = 0; i < 10; i++) {
        syscall2(SYS_KILL, pids[i], SIGKILL);
    }
    for (i = 0; i < 10; i++) {
        int status = 0;

        syscall3(SYS_WAITPID, pids[i], (long)&status, 0);
        if ((status & 0x7F) == 0) {
            fail("a load process exited on its own before it was killed");
        }
    }
    report_held();
    timer = file_field("/proc/stat", "latency_timer_us ");
    disk = file_field("/proc/stat", "latency_disk_us ");
    diskwait = file_field("/proc/stat", "latency_diskwait_us ");
    console = file_field("/proc/stat", "latency_console_us ");
    print("LOADPROBE timer ");
    print_num(timer);
    print(" disk ");
    print_num(disk);
    print(" diskwait ");
    print_num(diskwait);
    print(" console ");
    print_num(console);
    print(" lines ");
    print_num((unsigned)lines);
    print("\n");
    if (timer == 0xFFFFFFFFu || disk == 0xFFFFFFFFu || console == 0xFFFFFFFFu) {
        fail("/proc/stat has no latency lines");
    }
    report_held();
    if (!wanted(arg, 'r')) {
        report_skip("the load has no console reader");
    } else if (lines < LOAD_LINES) {
        fail("the console reader ended before it read every line");
    } else {
        report_held();
    }
    if (probe_native()) {
        report_skip("a running machine's other disks move the count");
    } else if (quarantines == 0xFFFFFFFFu ||
               file_field("/proc/disk", "disk-quarantines ") != quarantines) {
        fail("a request was quarantined by its deadline under the load");
    } else {
        report_held();
    }
    cleanup_mount(report[0]);
    report_held();
    report_end();
    puts("LOADPROBE OK");
    sys_exit(0);
}
