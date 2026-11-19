/* bench: the cost of a ptrace syscall stop, as /sbin/init of a Rach2 root
 * and /init of a Linux initramfs. Runs each load plain, under strace -f -c
 * and under strace -f -e trace=ioctl, and powers the machine off. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int p1[2], p2[2];
static long trips;

static void *pong(void *arg)
{
    char c;
    long i;
    (void)arg;
    for (i = 0; i < trips; i++) {
        if (read(p1[0], &c, 1) != 1 || write(p2[1], &c, 1) != 1)
            break;
    }
    return NULL;
}

/* One load: getppid, ioctl, or pingpong between two threads by pipes. */
static int load(const char *what, long n)
{
    double t0, t1;
    long i;
    struct rusage ru;
    int fd;

    t0 = now_ms();
    if (strcmp(what, "getppid") == 0) {
        for (i = 0; i < n; i++)
            syscall(SYS_getppid);
    } else if (strcmp(what, "ioctl") == 0) {
        int v;
        if (pipe(p1) < 0)
            return 1;
        fd = p1[0];
        for (i = 0; i < n; i++)
            ioctl(fd, FIONREAD, &v);
    } else {
        pthread_t th;
        char c = 'x';
        trips = n;
        if (pipe(p1) < 0 || pipe(p2) < 0)
            return 1;
        pthread_create(&th, NULL, pong, NULL);
        for (i = 0; i < n; i++) {
            if (write(p1[1], &c, 1) != 1 || read(p2[0], &c, 1) != 1)
                break;
        }
        pthread_join(th, NULL);
    }
    t1 = now_ms();
    getrusage(RUSAGE_SELF, &ru);
    printf("bench: %s %ld: %.1f ms, %.2f us an iteration, %ld voluntary, %ld involuntary\n", what,
           n, t1 - t0, (t1 - t0) * 1e3 / n, ru.ru_nvcsw, ru.ru_nivcsw);
    fflush(stdout);
    return 0;
}

/* Zero bkl/calls, or print its lines that counted. */
static void calls(const char *label)
{
    char line[256];
    FILE *f;

    if (!label) {
        f = fopen("/sys/kernel/debug/bkl/calls", "w");
        if (f) {
            fputs("0\n", f);
            fclose(f);
        }
        return;
    }
    f = fopen("/sys/kernel/debug/bkl/calls", "r");
    if (!f) {
        printf("bench: no bkl/calls\n");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char kind[32];
        unsigned long nr, n, wall, holds, held, wait;
        if (sscanf(line, "%31s %lx %lu %lu %lu %lu %lu", kind, &nr, &n, &wall, &holds, &held, &wait) >= 4 &&
            (n != 0 || holds != 0) && strcmp(kind, "kind") != 0)
            printf("calls %s: %s", label, line);
    }
    fclose(f);
    fflush(stdout);
}

static void run(const char *mode, char *const *argv)
{
    pid_t pid;
    int status;
    double t0 = now_ms();

    pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    printf("bench: %s: whole run %.1f ms, status %d\n", mode, now_ms() - t0, status);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    static const struct { const char *what; const char *n; } loads[] = {
        {"getppid", "20000"}, {"ioctl", "20000"}, {"pingpong", "5000"},
    };
    struct utsname u;
    int round, k;

    if (argc == 3)
        return load(argv[1], atol(argv[2]));

    mkdir("/dev", 0755);
    mkdir("/proc", 0755);
    mkdir("/tmp", 01777);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
    uname(&u);
    printf("bench: %s %s, %ld processors\n", u.sysname, u.release, sysconf(_SC_NPROCESSORS_ONLN));
    fflush(stdout);
    for (round = 0; round < 4; round++) {
        if (round == 2) {
            int j;
            for (j = 0; j < 500; j++) {
                if (fork() == 0) {
                    for (;;)
                        pause();
                }
            }
            printf("bench: 500 sleeping processes started\n");
        }
        if (round == 0 || round == 2) {
            char *all[] = {"/strace", "-f", "-o", "/tmp/c.txt", "/bench", "getppid", "20000", NULL};
            calls(NULL);
            run("profiled strace -f", all);
            calls(round == 0 ? "alone" : "sleepers");
        }
        for (k = 0; k < 3; k++) {
            char *plain[] = {"/bench", (char *)loads[k].what, (char *)loads[k].n, NULL};
            char *all[] = {"/strace", "-f", "-c", "-w", "-o", "/tmp/c.txt", "/bench",
                           (char *)loads[k].what, (char *)loads[k].n, NULL};
            char *io[] = {"/strace", "-f", "-e", "trace=ioctl", "-o", "/tmp/io.txt", "/bench",
                          (char *)loads[k].what, (char *)loads[k].n, NULL};
            char *bpf[] = {"/strace", "-f", "--seccomp-bpf", "-e", "trace=ioctl", "-o",
                           "/tmp/bpf.txt", "/bench", (char *)loads[k].what,
                           (char *)loads[k].n, NULL};
            printf("== round %d, %s ==\n", round, loads[k].what);
            run("plain", plain);
            run("strace -f -c", all);
            run("strace -f -e ioctl", io);
            run("strace -f --seccomp-bpf -e ioctl", bpf);
        }
    }
    printf("BENCH DONE\n");
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;)
        pause();
}
