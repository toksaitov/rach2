/* POSIX cache mutex permissions, serialization, kernel owner cleanup and the
 * retirement of obsolete machines. */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "../../../tools/etude/tooling_cache.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char cache[4096], mutex[4096];
static pid_t child;
static volatile sig_atomic_t stopped;
static const char key[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
/* The made machine, the recipe's machine of the other architecture, and a
 * machine of an obsolete recipe. */
static const char made[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const char other[] = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
static const char old[] = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

/* Give CACHE/PREFIX K SUFFIX, or "" when it is too long. */
static char *in_cache(char path[4096], const char *prefix, const char *k, const char *suffix)
{
    int n = snprintf(path, 4096, "%s/%s%s%s", cache, prefix, k, suffix);
    if (n < 0 || n >= 4096)
        path[0] = '\0';
    return path;
}

static void remove_machines(void)
{
    const char *const keys[] = {made, other, old};
    char path[4096];
    for (size_t i = 0; i < 3; i++) {
        unlink(in_cache(path, "tooling-", keys[i], ".qcow2"));
        unlink(in_cache(path, "tooling-", keys[i], ".tools"));
        unlink(in_cache(path, ".tooling-", keys[i], ".lock"));
    }
    unlink(in_cache(path, ".tooling-cache", "", ".lock"));
}

static void cleanup(void)
{
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        child = 0;
    }
    remove_machines();
    unlink(mutex);
    rmdir(cache);
}

static void interrupted(int sig)
{
    cleanup();
    _exit(128 + sig);
}

static void stop(int sig)
{
    (void)sig;
    stopped = 1;
}

static int reaped(int killed)
{
    int status;
    pid_t pid = child;
    pid_t ended;
    do {
        ended = waitpid(pid, &status, 0);
    } while (ended < 0 && errno == EINTR);
    if (ended != pid)
        return 0;
    child = 0;
    return killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                  : WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* The contender first observes the parent's lock, then enters after release. */
static int serialized(struct etude_tooling_site *site, int cancelled)
{
    intptr_t guard;
    int witness[2], ok;
    char byte;
    if (pipe(witness))
        return 0;
    if (tooling_cache_prepare_enter(site, key, &guard)) {
        close(witness[0]);
        close(witness[1]);
        return 0;
    }
    child = fork();
    if (!child) {
        struct flock lock = {0};
        close(witness[0]);
        close((int)guard);
        signal(SIGUSR1, stop);
        site->stop = &stopped;
        int fd = open(mutex, O_RDWR);
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        ok = fd >= 0 && !fcntl(fd, F_GETLK, &lock) && lock.l_pid == getppid();
        if (fd >= 0)
            close(fd);
        byte = ok ? 'y' : 'n';
        ok &= write(witness[1], &byte, 1) == 1;
        close(witness[1]);
        int entered = !tooling_cache_prepare_enter(site, key, &guard);
        if (entered)
            tooling_cache_leave(guard);
        ok &= cancelled ? !entered && stopped : entered;
        _exit(ok ? 0 : 1);
    }
    close(witness[1]);
    ok = child > 0 && read(witness[0], &byte, 1) == 1 && byte == 'y';
    close(witness[0]);
    if (child > 0 && cancelled) {
        kill(child, SIGUSR1);
        ok &= reaped(0);
        tooling_cache_leave(guard);
        return ok;
    }
    tooling_cache_leave(guard);
    return child > 0 && reaped(0) && ok;
}

/* A dead owner releases the kernel lock; a cancellation acquires nothing. */
static int owner_loss(struct etude_tooling_site *site)
{
    intptr_t guard;
    int witness[2], ok;
    char byte;
    if (pipe(witness))
        return 0;
    child = fork();
    if (!child) {
        close(witness[0]);
        if (tooling_cache_prepare_enter(site, key, &guard))
            _exit(1);
        byte = 'y';
        if (write(witness[1], &byte, 1) != 1)
            _exit(1);
        for (;;)
            pause();
    }
    close(witness[1]);
    ok = child > 0 && read(witness[0], &byte, 1) == 1 && byte == 'y';
    close(witness[0]);
    if (child <= 0)
        return 0;
    kill(child, SIGKILL);
    ok &= reaped(1);
    if (tooling_cache_prepare_enter(site, key, &guard))
        return 0;
    tooling_cache_leave(guard);
    stop(SIGTERM);
    site->stop = &stopped;
    ok &= tooling_cache_prepare_enter(site, key, &guard) != 0;
    return ok;
}

/* Write a completed machine of key K; 0 on success. */
static int machine(const char *k)
{
    static const char *const text[] = {"overlay\n", "etude-tooling: provision: ok\n"};
    static const char *const suffix[] = {".qcow2", ".tools"};
    char path[4096];
    for (size_t i = 0; i < 2; i++) {
        FILE *f = fopen(in_cache(path, "tooling-", k, suffix[i]), "w");
        if (!f)
            return -1;
        int bad = fputs(text[i], f) == EOF;
        if (fclose(f) || bad || chmod(path, 0444))
            return -1;
    }
    return 0;
}

static int present(const char *k)
{
    char path[4096];
    return !access(in_cache(path, "tooling-", k, ".qcow2"), F_OK) &&
           !access(in_cache(path, "tooling-", k, ".tools"), F_OK);
}

/* Pruning keeps every machine it is told to and retires the obsolete one. */
static int retired(struct etude_tooling_site *site)
{
    const char *const keep[] = {made, other, NULL};
    if (machine(made) || machine(other) || machine(old))
        return 0;
    tooling_cache_prune(site, keep);
    int ok = present(made) && present(other) && !present(old);
    remove_machines();
    return ok;
}

int etude_test_tooling_cache(const char *tmp)
{
    struct etude_tooling_site site = {0};
    struct stat st;
    intptr_t guard;
    mode_t mask;
    int n = snprintf(cache, sizeof(cache), "%s/rach2-tooling-cache.XXXXXX", tmp);
    if (n < 0 || n >= (int)sizeof(cache) || !mkdtemp(cache))
        return 1;
    atexit(cleanup);
    signal(SIGHUP, interrupted);
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    n = snprintf(mutex, sizeof(mutex), "%s/.tooling-%s.lock", cache, key);
    if (n < 0 || n >= (int)sizeof(mutex))
        return 1;
    site.cache = cache;
    mask = umask(077);
    int ok = !tooling_cache_prepare_enter(&site, key, &guard);
    umask(mask);
    if (!ok)
        return 1;
    ok = !lstat(mutex, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
         (st.st_mode & 0777) == 0660;
    tooling_cache_leave(guard);
    ok &= retired(&site) && serialized(&site, 0) && serialized(&site, 1) && owner_loss(&site);
    cleanup();
    return ok ? 0 : 1;
}
