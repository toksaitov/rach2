/* installer_repair_fixture.c: compiled edits for stopped installer states. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "installer_repair_fixture.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static struct repair_fixture fixture;
static volatile sig_atomic_t child, stopped;
static int remove_tree(const char *path)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    char p[4096];
    int ok = 1;
    if (lstat(path, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return !unlink(path);
    d = opendir(path);
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(p, sizeof(p), "%s/%s", path, e->d_name) >= (int)sizeof(p) || !remove_tree(p))
            ok = 0;
    }
    closedir(d);
    return !rmdir(path) && ok;
}
static void cleanup(void)
{
    if (*fixture.work && !remove_tree(fixture.work))
        fprintf(stderr, "installer-repair-fixture: scratch remains: %s\n", fixture.work);
}
static void interrupt(int sig)
{
    stopped = sig;
    if (child > 0)
        kill(-(pid_t)child, SIGKILL);
}
const char *ir_path(struct repair_fixture *f, const char *name)
{
    static char paths[128][4096];
    static unsigned next;
    char *p = paths[next++ % 128];
    if (snprintf(p, 4096, "%s/%s", f->work, name) >= 4096)
        return NULL;
    return p;
}
int ir_run(const char *const argv[])
{
    pid_t pid, waited;
    int status;
    sigset_t set, old;
    if (stopped)
        return 0;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigprocmask(SIG_BLOCK, &set, &old);
    pid = fork();
    if (!pid) {
        setpgid(0, 0);
        sigprocmask(SIG_SETMASK, &old, NULL);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &old, NULL);
        return 0;
    }
    setpgid(pid, pid);
    child = pid;
    sigprocmask(SIG_SETMASK, &old, NULL);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    child = 0;
    return waited == pid && WIFEXITED(status) && !WEXITSTATUS(status) && !stopped;
}
int ir_mtools(struct repair_fixture *f, const char *tool, const char *const args[])
{
    const char *a[80] = {"/bin/mtools", "-c", tool, "-i", f->volume};
    unsigned i, n = 5;
    for (i = 0; args[i] && n < 79; i++)
        a[n++] = args[i];
    if (args[i])
        return 0;
    a[n] = NULL;
    return ir_run(a);
}
int ir_text(struct repair_fixture *f, const char *name, const char *text)
{
    FILE *out = fopen(ir_path(f, name), "wb");
    int ok;
    if (!out)
        return 0;
    ok = fwrite(text, 1, strlen(text), out) == strlen(text);
    return !fclose(out) && ok;
}
int ir_copy_contents(struct repair_fixture *f, const char *directory, const char *target)
{
    DIR *d = opendir(directory);
    struct dirent *e;
    char path[4096];
    int ok = 1;
    unsigned n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        const char *args[] = {"-s", path, target, NULL};
        if (e->d_name[0] == '.')
            continue;
        n++;
        if (snprintf(path, sizeof(path), "%s/%s", directory, e->d_name) >= (int)sizeof(path) ||
            !ir_mtools(f, "mcopy", args)) {
            ok = 0;
            break;
        }
    }
    closedir(d);
    return ok && n;
}
static int sector(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long n;
    if (!*text || strspn(text, "0123456789") != strlen(text))
        return 0;
    errno = 0;
    n = strtoull(text, &end, 10);
    if (errno || *end || n > (uint64_t)INT64_MAX / 512)
        return 0;
    *value = n;
    return 1;
}
static int arguments(int argc, char **argv)
{
    fixture.disk = argv[2];
    if (!strcmp(argv[1], "root-keys"))
        return argc == 3;
    if (!strcmp(argv[1], "keep"))
        return argc == 5 && sector(argv[3], &fixture.boot) && sector(argv[4], &fixture.recovery);
    if ((!strcmp(argv[1], "legacy") || !strcmp(argv[1], "slot")) && argc == 6 &&
        sector(argv[3], &fixture.recovery)) {
        fixture.name = argv[4];
        fixture.key = argv[5];
        return *fixture.name && *fixture.key && !strpbrk(fixture.name, "\r\n/") &&
               !strpbrk(fixture.key, "\r\n");
    }
    return 0;
}
int main(int argc, char **argv)
{
    struct sigaction sa;
    int ok, fd;
    const char *tmp;
    if (argc < 3 || !arguments(argc, argv)) {
        fputs("usage: installer-repair-fixture root-keys|legacy|slot|keep DISK ARGUMENTS\n",
              stderr);
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = interrupt;
    if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL) ||
        sigaction(SIGHUP, &sa, NULL) || atexit(cleanup))
        return 2;
    tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    if (snprintf(fixture.work, sizeof(fixture.work), "%s/installer-repair.XXXXXX", tmp) >=
        (int)sizeof(fixture.work)) {
        fixture.work[0] = 0;
        return 2;
    }
    if (!mkdtemp(fixture.work)) {
        fixture.work[0] = 0;
        return 2;
    }
    snprintf(fixture.volume, sizeof(fixture.volume), "%s@@%llu", fixture.disk,
             (unsigned long long)fixture.recovery * 512);
    if (!strcmp(argv[1], "root-keys"))
        ok = ir_root_keys(&fixture);
    else if (!strcmp(argv[1], "legacy"))
        ok = ir_legacy(&fixture);
    else if (!strcmp(argv[1], "slot"))
        ok = ir_slot(&fixture);
    else
        ok = ir_keep(&fixture);
    fd = open(fixture.disk, O_RDWR);
    if (fd < 0 || fsync(fd))
        ok = 0;
    if (fd >= 0)
        close(fd);
    if (stopped)
        return 128 + stopped;
    if (!ok)
        fprintf(stderr, "installer-repair-fixture: %s failed\n", argv[1]);
    return ok ? 0 : 1;
}
