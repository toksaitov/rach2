/**
 * loadprobe: the machine under load, and the longest delays it saw.
 *
 * Six children spin, two write a file of 256 KiB on the mounted volume and
 * sync it over and over, one pings the gateway through the kernel's own
 * ping every fifty milliseconds, and one reads lines from the console,
 * which the harness types one after another, each once the reader has
 * said it took the last, for twenty seconds. Then the parent kills them
 * all, reads `/proc/stat`, and prints the four delays the kernel kept as
 * its longest: the gap between two timer ticks, a block request from its
 * taking to its completion, a wait for the request limit, and a console
 * line from readable to read. The four delays depend on the machine and
 * are printed, not judged; what is judged is counted: at least three lines
 * must have been read, and no request may have been quarantined by its
 * deadline, which /proc/disk counts.
 *
 * Prints "LOADPROBE timer N disk N diskwait N console N lines N" and then
 * "LOADPROBE OK" and exits 0, or "LOADPROBE FAIL: " and the reason and
 * exits 1.
 */

#include "probe.h"

#define GATEWAY_IP 0x0A000202u

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define SIGKILL  9

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

/** Print "LOADPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    print("LOADPROBE FAIL: ");
    puts(why);
    sys_exit(1);
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
        args[0] = GATEWAY_IP;
        sock_call(SC_PING, args);
        sleep_ms(50);
    }
}

/** The console reader: count each line read into `report`, one byte each,
 * and print "LOADPROBE read" for it, after which the harness types the next
 * line. It does not return. */
static void reader(int report)
{
    char c;

    for (;;) {
        if (syscall3(SYS_READ, 0, (long)&c, 1) == 1 && c == '\n') {
            syscall3(SYS_WRITE, report, (long)"l", 1);
            puts("LOADPROBE read");
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
 * The probe, entered with `sp` at argc. Every load runs unless the first
 * argument names some by their first letters: `s`pinners, `w`riters, `W` for
 * the first writer alone, `p`inger, `r`eader. With an argument, `n` makes
 * the writers skip the sync and `k` mounts the volume as KRFFS in place of
 * FAT32. A child whose load is not wanted sleeps for 30 seconds, and is
 * killed at 20.
 */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *arg = argc >= 2 ? argv[1] : 0;
    int pids[10];
    int report[2];
    int i, lines = 0;
    unsigned timer, disk, diskwait, console, quarantines;
    char byte;

    if (syscall2(SYS_MKDIR, (long)"/lo", 0755) != 0 ||
        syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/lo",
                 (long)(wanted(arg, 'k') && arg ? "krffs" : "vfat"), 0, 0) != 0) {
        fail("the mount of the volume");
    }
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
            syscall1(SYS_CLOSE, report[0]);
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
                    writer(i == 6 ? "/lo/load1.bin" : "/lo/load2.bin");
                }
            } else if (i == 8) {
                if (wanted(arg, 'p')) {
                    pinger();
                }
            } else if (wanted(arg, 'r')) {
                reader(report[1]);
            }
            sleep_ms(30000);
            sys_exit(0);
        }
    }
    syscall1(SYS_CLOSE, report[1]);
    quarantines = file_field("/proc/disk", "disk-quarantines ");
    sleep_ms(20000);
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
    while (syscall3(SYS_READ, report[0], (long)&byte, 1) == 1) {
        lines++;
    }
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
    if (lines < 3 && wanted(arg, 'r')) {
        fail("the console reader had fewer than three lines");
    }
    if (quarantines == 0xFFFFFFFFu ||
        file_field("/proc/disk", "disk-quarantines ") != quarantines) {
        fail("a request was quarantined by its deadline under the load");
    }
    puts("LOADPROBE OK");
    sys_exit(0);
}
