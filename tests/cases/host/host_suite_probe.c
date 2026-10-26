/* Prepared behavioral probes of the native host-suite driver. */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int count(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d)))
        n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    if (d)
        closedir(d);
    return n;
}
static int touch(const char *path, int exclusive)
{
    int fd = open(path, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : 0), 0600);
    if (fd < 0)
        return 0;
    return close(fd) == 0;
}
static int alone(void)
{
    char parent[4096], own[256], path[8192];
    snprintf(parent, sizeof(parent), "%s", getenv("TMPDIR"));
    char *slash = strrchr(parent, '/');
    if (!slash)
        return 1;
    snprintf(own, sizeof(own), "%s", slash + 1);
    *slash = 0;
    DIR *d = opendir(parent);
    struct dirent *e;
    int others = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, own) || !strcmp(e->d_name, "keep"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", parent, e->d_name);
        struct stat st;
        if (!lstat(path, &st) && S_ISDIR(st.st_mode))
            others++;
    }
    if (d)
        closedir(d);
    return !d || others;
}
static void *volatile lost;
static int fault_probe(const char *mode, int argc)
{
    if (!strcmp(mode, "asan")) {
        volatile char *p = malloc(4);
        p[4] = 1;
        free((void *)p);
        return 0;
    }
    if (!strcmp(mode, "ubsan")) {
        volatile int x = INT_MAX;
        x += argc;
        return x == 0;
    }
    if (!strcmp(mode, "leak")) {
        lost = malloc(64);
        lost = NULL;
        return 0;
    }
    return -1;
}

static int signal_probe(void)
{
    /* This legacy output is input to the host runner's output filter. */
    static const char note[] = "signals_hosttest: not run: nothing: "
                               "this line is repeated under the pass";
    struct sigaction a, b;
    sigset_t mask;
    sigaction(SIGINT, NULL, &a);
    sigaction(SIGQUIT, NULL, &b);
    sigprocmask(SIG_BLOCK, NULL, &mask);
    puts(note);
    return a.sa_handler != SIG_DFL || b.sa_handler != SIG_DFL || sigismember(&mask, SIGINT);
}

/**
 * Record the process ID in `MEET/stuck` and wait until a signal ends the
 * process. The record appears by rename, complete, since the fixture reads it
 * as soon as the name exists. Return 4 when it cannot be written.
 */
static int stuck(const char *meet)
{
    char part[8192], path[8192];
    snprintf(part, sizeof(part), "%s/stuck.part", meet);
    snprintf(path, sizeof(path), "%s/stuck", meet);
    FILE *f = fopen(part, "w");
    if (!f)
        return 4;
    if (fprintf(f, "%ld\n", (long)getpid()) < 0 || fclose(f) != 0 || rename(part, path))
        return 4;
    for (;;)
        pause();
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--selftest"))
        return 0;
    if (argc != 3 || strcmp(argv[1], "--probe"))
        return 2;
    const char *mode = argv[2], *tmp = getenv("TMPDIR"), *meet = getenv("HOST_SUITE_MEET");
    char path[8192];
    if (!tmp || !meet)
        return 3;
    int fault = fault_probe(mode, argc);
    if (fault >= 0)
        return fault;
    if (!strcmp(mode, "keep")) {
        snprintf(path, sizeof(path), "%s/evidence", tmp);
        touch(path, 0);
        printf("keep: %s\n", path);
        return 1;
    }
    if (!strcmp(mode, "signals")) {
        return signal_probe();
    }
    if (!strcmp(mode, "alone"))
        return alone();
    if (!strcmp(mode, "linger")) {
        /* The interval is a workload beside which an alone check must wait. */
        struct timespec tick = {2, 0};
        while (nanosleep(&tick, &tick) && tick.tv_sec >= 0) {
        }
        return 0;
    }
    if (!strcmp(mode, "script_pass")) {
        puts("fine");
        return access(tmp, F_OK) ? 1 : 0;
    }
    if (!strcmp(mode, "script_fail")) {
        puts("script: it failed");
        return 4;
    }
    if (!strcmp(mode, "stuck"))
        return stuck(meet);
    if (!strcmp(mode, "meet")) {
        snprintf(path, sizeof(path), "%s/mine", tmp);
        if (!touch(path, 1))
            return 5;
        snprintf(path, sizeof(path), "%s/%ld", meet, (long)getpid());
        if (!touch(path, 0))
            return 6;
        while (count(meet) < 8) {
            struct timespec tick = {0, 1000000};
            /* interval: poll the meeting directory every millisecond */
            nanosleep(&tick, NULL);
        }
        puts("meet: all 8 checks running");
        return 0;
    }
    return 7;
}
