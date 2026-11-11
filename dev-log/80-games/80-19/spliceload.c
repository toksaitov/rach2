/* spliceload: compare a cached file's read and splice copies under load. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MIB      (1024u * 1024u)
#define DIR      "/splice-load"
#define FILE_    DIR "/file"
#define CHILDREN 3

static pid_t children[CHILDREN];
static int mounted, made;
static pid_t owner;
static unsigned char buffer[MIB];
static volatile unsigned int *shared;

/** Reap the load, remove its file and mount, and power off when run as init. */
static void cleanup(void)
{
    int i;

    if (getpid() != owner) {
        return;
    }
    if (shared && shared != MAP_FAILED) {
        shared[0] = 1;
    }
    for (i = 0; i < CHILDREN; i++) {
        if (children[i] > 0) {
            kill(children[i], SIGKILL);
            waitpid(children[i], NULL, 0);
        }
    }
    if (made) {
        unlink(FILE_);
    }
    if (mounted) {
        umount(DIR);
    }
    rmdir(DIR);
    if (getpid() == 1) {
        reboot(RB_POWER_OFF);
    }
}

/** Report the failed operation and leave through cleanup. */
static void fail(const char *what)
{
    fprintf(stderr, "spliceload: %s: %s\n", what, strerror(errno));
    exit(1);
}

/** Monotonic microseconds, used only for measurements, never a pass limit. */
static unsigned long long now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        fail("clock_gettime");
    }
    return (unsigned long long)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u;
}

/** Pin the caller to cpu, so every load process competes for the big lock. */
static void pin(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set)) {
        fail("sched_setaffinity");
    }
}

/** Read one snapshot without printing while the load is running. */
static void snapshot(char *text, size_t size)
{
    int fd = open("/proc/stat", O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        fail("/proc/stat");
    }
    n = read(fd, text, size - 1);
    if (n < 0 || (size_t)n == size - 1) {
        fail("read /proc/stat");
    }
    text[n] = '\0';
    close(fd);
}

/** Print lock and scheduling counters after the load has stopped. */
static void show_snapshot(const char *label, char *text)
{
    char *line = text, *next;

    printf("spliceload: snapshot: %s\n", label);
    while (line && *line) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (!strncmp(line, "bkl_", 4) || !strncmp(line, "ctxt ", 5) || !strncmp(line, "cpu", 3) ||
            !strncmp(line, "tlb_", 4) || !strncmp(line, "timer_", 6)) {
            puts(line);
        }
        line = next;
    }
}

/** The disk-read counter, which must stay still during a cached copy. */
static unsigned long long disk_read(void)
{
    char line[256];
    unsigned long long n;
    FILE *fp = fopen("/proc/vmstat", "r");

    if (!fp) {
        fail("/proc/vmstat");
    }
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "pgpgin %llu", &n) == 1) {
            fclose(fp);
            return n;
        }
    }
    fclose(fp);
    errno = EINVAL;
    fail("pgpgin absent");
    return 0;
}

/** Write a non-sparse file, then read it whole to fill the file page cache. */
static void prepare(unsigned int mib)
{
    unsigned int i;
    int fd = open(FILE_, O_CREAT | O_EXCL | O_RDWR, 0600);

    if (fd < 0) {
        fail("create");
    }
    made = 1;
    memset(buffer, 0x5a, sizeof(buffer));
    for (i = 0; i < mib; i++) {
        if (write(fd, buffer, sizeof(buffer)) != sizeof(buffer)) {
            fail("write file");
        }
        if ((i + 1) % 32 == 0) {
            printf("spliceload: wrote: %u MiB\n", i + 1);
        }
    }
    if (fsync(fd) || lseek(fd, 0, SEEK_SET) != 0) {
        fail("sync or seek");
    }
    for (i = 0; i < mib; i++) {
        if (read(fd, buffer, sizeof(buffer)) != sizeof(buffer)) {
            fail("warm file");
        }
        if ((i + 1) % 32 == 0) {
            printf("spliceload: warmed: %u MiB\n", i + 1);
        }
    }
    close(fd);
}

/** Start n pinned readers of the same cached file, or getpid loops for
 * mode 1. */
static void start_load(int n, int mode)
{
    int i;

    memset((void *)shared, 0, 4096);
    for (i = 0; i < n; i++) {
        children[i] = fork();
        if (children[i] < 0) {
            fail("fork");
        }
        if (!children[i]) {
            int fd;

            pin(i + 1);
            fd = open(FILE_, O_RDONLY);
            if (fd < 0) {
                _exit(2);
            }
            memset(buffer, 0, sizeof(buffer));
            shared[i + 1] = 1;
            while (!shared[0]) {
                if (mode == 1) {
                    syscall(SYS_getpid);
                } else if (pread(fd, buffer, sizeof(buffer), 0) != sizeof(buffer)) {
                    _exit(3);
                }
                shared[i + 1]++;
            }
            close(fd);
            _exit(0);
        }
    }
    for (i = 0; i < n; i++) {
        while (!shared[i + 1]) {
            sched_yield();
        }
    }
}

/** Stop the load between rows, reporting its completed calls. */
static void stop_load(int n)
{
    int i, status;

    shared[0] = 1;
    for (i = 0; i < n; i++) {
        if (waitpid(children[i], &status, 0) != children[i] || status != 0) {
            fail("load child");
        }
        children[i] = 0;
        printf("spliceload: load: cpu %d calls %u\n", i + 1, shared[i + 1] - 1);
    }
}

/** Copy mib MiB through read/write or file-to-pipe-to-null splice calls,
 * the pipe sized as GNU cat 9.12 sizes its own (src/splice.h): 512 KiB by
 * F_SETPIPE_SZ, or what F_GETPIPE_SZ gives when that fails. */
static void copy(unsigned int mib, int splicing, int nload, int mode)
{
    int fd, sink, p[2], pipe_size;
    uint64_t total = (uint64_t)mib * MIB, done = 0;
    unsigned long calls = 0;
    unsigned long long start, elapsed, pgpgin, longest = 0;
    static char before_stat[16384], after_stat[16384];
    struct rusage before_usage, after_usage;

    fd = open(FILE_, O_RDONLY);
    sink = open("/dev/null", O_WRONLY);
    if (fd < 0 || sink < 0 || pipe(p)) {
        fail("copy descriptors");
    }
    pipe_size = fcntl(p[1], F_SETPIPE_SZ, 512 * 1024);
    if (pipe_size < 0) {
        pipe_size = fcntl(p[1], F_GETPIPE_SZ);
    }
    printf("spliceload: begin: %s mib %u load %d mode %d pipe %d\n",
           splicing ? "splice" : "read", mib, nload, mode, pipe_size);
    start_load(nload, mode);
    pgpgin = disk_read();
    snapshot(before_stat, sizeof(before_stat));
    if (getrusage(RUSAGE_SELF, &before_usage)) {
        fail("getrusage");
    }
    start = now();
    while (done < total) {
        size_t step = splicing ? (size_t)pipe_size : MIB;
        size_t want = total - done > step ? step : (size_t)(total - done);
        ssize_t got;
        unsigned long long before = now(), duration;

        if (splicing) {
            ssize_t left;

            got = splice(fd, NULL, p[1], NULL, want, 0);
            calls++;
            if (got <= 0) {
                fail("splice into pipe");
            }
            left = got;
            while (left) {
                ssize_t put = splice(p[0], NULL, sink, NULL, left, 0);

                calls++;
                if (put <= 0) {
                    fail("splice from pipe");
                }
                left -= put;
            }
        } else {
            got = read(fd, buffer, want);
            calls++;
            if (got <= 0 || write(sink, buffer, (size_t)got) != got) {
                fail("read/write copy");
            }
            calls++;
        }
        duration = now() - before;
        if (duration > longest) {
            longest = duration;
        }
        done += (uint64_t)got;
    }
    elapsed = now() - start;
    if (getrusage(RUSAGE_SELF, &after_usage)) {
        fail("getrusage");
    }
    snapshot(after_stat, sizeof(after_stat));
    pgpgin = disk_read() - pgpgin;
    stop_load(nload);
    show_snapshot("before", before_stat);
    show_snapshot("after", after_stat);
    printf("spliceload: result: %s mib %u load %d mode %d us %llu calls %lu "
           "longest_pair_us %llu disk_kib %llu voluntary %ld involuntary %ld\n",
           splicing ? "splice" : "read", mib, nload, mode, elapsed, calls, longest, pgpgin,
           after_usage.ru_nvcsw - before_usage.ru_nvcsw,
           after_usage.ru_nivcsw - before_usage.ru_nivcsw);
    if (pgpgin) {
        errno = EIO;
        fail("cached copy read disk");
    }
    close(p[0]);
    close(p[1]);
    close(sink);
    close(fd);
}

/** Mount NODE, warm a file and measure copies; init reads /spliceload.conf. */
int main(int argc, char **argv)
{
    const char *node = "/dev/disk0";
    unsigned int file_mib = 256, copy_mib = 16;
    int nload = 3, mode = 2, round;
    FILE *config;

    owner = getpid();
    atexit(cleanup);
    setvbuf(stdout, NULL, _IONBF, 0);
    config = fopen("/spliceload.conf", "r");
    if (config) {
        if (fscanf(config, "%u %u %d %d", &file_mib, &copy_mib, &nload, &mode) != 4) {
            fail("configuration");
        }
        fclose(config);
    }
    if (argc > 1) {
        node = argv[1];
    }
    if (argc > 2) {
        copy_mib = (unsigned int)strtoul(argv[2], NULL, 10);
    }
    if (!file_mib || !copy_mib || copy_mib > file_mib || nload < 0 || nload > CHILDREN) {
        errno = EINVAL;
        fail("arguments");
    }
    if (mkdir(DIR, 0700) || mount(node, DIR, "ext2", 0, NULL)) {
        fail("mount");
    }
    mounted = 1;
    pin(0);
    shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        fail("shared memory");
    }
    prepare(file_mib);
    copy(copy_mib, 0, 0, mode);
    copy(copy_mib, 1, 0, mode);
    for (round = 0; round < 2; round++) {
        copy(copy_mib, 0, nload, mode);
        copy(copy_mib, 1, nload, mode);
    }
    puts("SPLICELOAD OK");
    return 0;
}
