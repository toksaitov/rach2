/* host_products_drive.c: guest process fixtures for tooling_image.sh.
 * The host C controller selects every row. This compiled guest fixture
 * supplies the stand-in commands and sends signals to the actual product. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

static volatile sig_atomic_t stopped;
static pid_t active;
static char root[4096];

static void caught(int signal_number)
{
    stopped = signal_number;
}

static int text(const char *path, const char *value)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (f == NULL)
        return 0;
    ok = fputs(value, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char child[8192];
    int ok = 1;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    dir = opendir(path);
    if (dir == NULL)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child) ||
            !remove_tree(child))
            ok = 0;
    }
    closedir(dir);
    return rmdir(path) == 0 && ok;
}

/** Stand in for git's two inventory operations, not for the product. */
static int fake_git(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "rev-parse")) {
        puts(".git");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "ls-files")) {
        fwrite("src/crypt.c", 1, 12, stdout);
        return 0;
    }
    return 2;
}

static const char *option(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return NULL;
}

/** Simulate an etude job while retaining its private cache and lock records. */
static int fake_etude(int argc, char **argv)
{
    const char *fix = getenv("FIX"), *lock = getenv("TEST_LOCK");
    const char *cache = option(argc, argv, "-C"), *out = option(argc, argv, "-O");
    char path[8192], job[8192], mode[32];
    FILE *f;
    unsigned delay = 1;
    if (argc < 3 || strcmp(argv[1], "provision") || strcmp(argv[2], "tooling") || !cache || !fix ||
        !lock)
        return 2;
    if (out == NULL)
        return 0;
    snprintf(job, sizeof(job), "%s/job-%ld-0", cache, (long)getpid());
    if (mkdir(job, 0700) != 0)
        return 3;
    snprintf(path, sizeof(path), "%s/channel.log", job);
    if (!text(path, "gnu-vm: make: the layer is built\n"))
        return 3;
    snprintf(path, sizeof(path), "%s/mode", fix);
    f = fopen(path, "r");
    if (!f || fscanf(f, "%31s", mode) != 1)
        return 3;
    fclose(f);
    if (!strcmp(mode, "pass")) {
        snprintf(path, sizeof(path), "%s/digest", out);
        if (!text(path, "feed\n") || !remove_tree(job))
            return 3;
        return 0;
    }
    snprintf(path, sizeof(path), "%s/delay", fix);
    f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%u", &delay) != 1)
            delay = 1;
        fclose(f);
    }
    if (strcmp(mode, "bare")) {
        signal(SIGHUP, caught);
        signal(SIGINT, caught);
        signal(SIGTERM, caught);
    }
    snprintf(path, sizeof(path), "%s/running", fix);
    if (!text(path, "running\n"))
        return 3;
    while (!stopped)
        pause();
    {
        unsigned left = delay;
        while (left)
            left = sleep(left);
    }
    if (exists(lock)) {
        snprintf(path, sizeof(path), "%s/held", fix);
        if (!text(path, "held\n"))
            return 3;
    }
    if (!remove_tree(job))
        return 3;
    return 128 + stopped;
}

/** Stop a failed fixture's process group before returning to etude-guest. */
static void cleanup(void)
{
    if (active > 0) {
        kill(-active, SIGKILL);
        while (waitpid(active, NULL, 0) < 0 && errno == EINTR)
            ;
        active = 0;
    }
}

static int start(const char *shell)
{
    char path[8192];
    int fd;
    snprintf(path, sizeof(path), "%s/fix/running", root);
    unlink(path);
    snprintf(path, sizeof(path), "%s/fix/held", root);
    unlink(path);
    active = fork();
    if (active < 0)
        return 0;
    if (active != 0)
        return 1;
    if (setsid() < 0 || chdir(root) != 0)
        _exit(120);
    signal(SIGINT, SIG_DFL);
    snprintf(path, sizeof(path), "%s/output", root);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
        _exit(120);
    close(fd);
    execl(shell, shell, "vendor/gnu/tooling_image.sh", "build/images/out.ext2", "16", (char *)NULL);
    _exit(121);
}

/** Wait for the compiled stand-in's marker, stopping if the product ended. */
static int running(void)
{
    char path[8192];
    int status;
    snprintf(path, sizeof(path), "%s/fix/running", root);
    while (!exists(path)) {
        pid_t got = waitpid(active, &status, WNOHANG);
        if (got == active) {
            active = 0;
            return 0;
        }
        if (got < 0 && errno != EINTR)
            return 0;
        /* interval: poll the readiness file every 100 ms */
        usleep(100000);
    }
    return 1;
}

static int finish(void)
{
    int status;
    pid_t pid = active;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    active = 0;
    while (kill(-pid, 0) == 0 || errno == EPERM)
        /* interval: poll the process group every 100 ms */
        usleep(100000);
    return WIFEXITED(status)     ? WEXITSTATUS(status)
           : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                 : -1;
}

static int empty_prefix(const char *directory, const char *prefix)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    int ok = 1;
    if (dir == NULL)
        return errno == ENOENT;
    while ((entry = readdir(dir)) != NULL)
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0)
            ok = 0;
    closedir(dir);
    return ok;
}

static int clean(int held)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/build/vendor/gnu/tooling.disk.lock", root);
    if (exists(path))
        return 0;
    snprintf(path, sizeof(path), "%s/build/vendor/gnu", root);
    if (!empty_prefix(path, ".tooling."))
        return 0;
    snprintf(path, sizeof(path), "%s/build/tooling", root);
    if (!empty_prefix(path, "job-"))
        return 0;
    snprintf(path, sizeof(path), "%s/fix/held", root);
    return !held || exists(path);
}

static int configure(const char *mode, unsigned delay)
{
    char path[8192], value[32];
    snprintf(path, sizeof(path), "%s/fix/mode", root);
    if (!text(path, mode))
        return 0;
    snprintf(path, sizeof(path), "%s/fix/delay", root);
    snprintf(value, sizeof(value), "%u\n", delay);
    return text(path, value);
}

/** Run one row on the product, with the same signals as make or a terminal. */
static int row(const char *name, const char *shell)
{
    int signal_number = SIGTERM, expected = 143;
    char path[8192];
    int is_pass = !strcmp(name, "pass"), bare = !strcmp(name, "bare"),
        killed = !strcmp(name, "kill");
    unsigned delay = killed ? 4 : !strcmp(name, "twice") ? 2 : 1;
    if (!configure(is_pass ? "pass" : bare ? "bare" : "wait", delay) || !start(shell))
        return 0;
    if (is_pass) {
        snprintf(path, sizeof(path), "%s/build/images/out.ext2", root);
        return finish() == 0 && exists(path) && clean(0);
    }
    if (!running())
        return 0;
    if (!strcmp(name, "int")) {
        signal_number = SIGINT;
        expected = 130;
    } else if (!strcmp(name, "hup")) {
        signal_number = SIGHUP;
        expected = 129;
    }
    if (kill(!strcmp(name, "alone") ? active : -active, signal_number) != 0)
        return 0;
    if (!strcmp(name, "twice")) {
        usleep(500000);
        if (kill(active, SIGTERM) != 0)
            return 0;
    }
    if (killed) {
        sleep(2);
        if (kill(-active, SIGKILL) != 0)
            return 0;
        if (finish() != 137 || !configure("pass", 1) || !start(shell))
            return 0;
        snprintf(path, sizeof(path), "%s/build/images/out.ext2", root);
        return finish() == 0 && exists(path) && clean(0);
    }
    return finish() == expected && clean(!bare);
}

int main(int argc, char **argv)
{
    char *name = strrchr(argv[0], '/');
    char path[8192];
    const char *old;
    int ok;
    name = name != NULL ? name + 1 : argv[0];
    if (!strcmp(name, "git"))
        return fake_git(argc, argv);
    if (!strcmp(name, "sha256")) {
        puts("feed  fixture");
        return 0;
    }
    if (!strcmp(name, "etude"))
        return fake_etude(argc, argv);
    if (argc != 4 || strlen(argv[1]) >= sizeof(root))
        return 2;
    strcpy(root, argv[1]);
    if (atexit(cleanup) != 0)
        return 2;
    snprintf(path, sizeof(path), "%s/fix", root);
    if (setenv("FIX", path, 1) != 0)
        return 2;
    snprintf(path, sizeof(path), "%s/build/vendor/gnu/tooling.disk.lock", root);
    if (setenv("TEST_LOCK", path, 1) != 0)
        return 2;
    old = getenv("PATH");
    snprintf(path, sizeof(path), "%s/bin:%s", root, old != NULL ? old : "/bin:/usr/bin");
    if (setenv("PATH", path, 1) != 0 || setenv("RACH2_ARCH", "i686", 1) != 0 ||
        setenv("RACH2_QEMU_ACCEL", "tcg", 1) != 0 ||
        setenv("MUSL_CRYPT_SRC", "src/crypt.c", 1) != 0 ||
        setenv("GNU_TOOLING_DISK_GIB", "1", 1) != 0 ||
        setenv("ETUDE_TOOLING_ARCH", "amd64", 1) != 0)
        return 2;
    unsetenv("ETUDE_TOOLING_CACHE");
    unsetenv("GNU_TOOLING_DRY_RUN");
    unsetenv("RACH2_GNU_OUT");
    ok = row(argv[3], argv[2]);
    if (!ok) {
        FILE *f;
        int byte;
        snprintf(path, sizeof(path), "%s/output", root);
        f = fopen(path, "r");
        while (f && (byte = fgetc(f)) != EOF)
            fputc(byte, stderr);
        if (f)
            fclose(f);
    }
    return ok ? 0 : 1;
}
