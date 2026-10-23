/* fixture.c: private files, child groups and complete command evidence. */
#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE  1
#define _POSIX_C_SOURCE  200809L
#include "fixture.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <regex.h>
#include <pwd.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
/* Group identities stay registered after their leaders exit, until cleanup. */
static pid_t groups[1024];
static size_t ngroups;
static volatile sig_atomic_t stopped;
static struct fixture *owner;
static void signal_children(int sig)
{
    size_t i;
    stopped = sig;
    for (i = 0; i < ngroups; i++)
        if (groups[i] > 0)
            kill(-groups[i], SIGTERM);
}
static void children_cleanup(void)
{
    size_t i;
    int status;
    for (i = 0; i < ngroups; i++)
        if (groups[i] > 0)
            kill(-groups[i], SIGKILL);
    for (i = 0; i < ngroups; i++)
        if (groups[i] > 0)
            while (waitpid(groups[i], &status, 0) < 0 && errno == EINTR) {
            }
    if (stopped && owner)
        fprintf(stderr, "fixture: interrupted; evidence: %s\n", owner->work);
    ngroups = 0;
}
static void interrupted(void)
{
    if (stopped) {
        int sig = stopped;
        children_cleanup();
        exit(128 + sig);
    }
}
int fx_exists(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0;
}
int fx_mkdir(const char *p)
{
    char path[4096], *at;
    if (strlen(p) >= sizeof(path))
        return 0;
    strcpy(path, p);
    for (at = path + 1; *at; at++)
        if (*at == '/') {
            *at = 0;
            if (mkdir(path, 0700) && errno != EEXIST)
                return 0;
            *at = '/';
        }
    return !mkdir(path, 0700) || errno == EEXIST;
}
int fx_write(const char *p, const void *data, size_t n, unsigned mode)
{
    char path[4096], *at;
    FILE *s;
    int ok;
    if (strlen(p) >= sizeof(path))
        return 0;
    strcpy(path, p);
    at = strrchr(path, '/');
    if (at) {
        *at = 0;
        if (!fx_mkdir(path))
            return 0;
    }
    s = fopen(p, "wb");
    if (!s)
        return 0;
    ok = fwrite(data, 1, n, s) == n;
    if (fclose(s))
        ok = 0;
    return !chmod(p, (mode_t)mode) && ok;
}
int fx_text(const char *p, const char *s)
{
    return fx_write(p, s, strlen(s), 0600);
}
char *fx_read(const char *p)
{
    FILE *s = fopen(p, "rb");
    size_t n = 0, cap = 4096;
    char *v = malloc(cap), *next;
    if (!v)
        return NULL;
    if (!s) {
        v[0] = 0;
        return v;
    }
    for (;;) {
        size_t k = fread(v + n, 1, cap - n - 1, s);
        n += k;
        if (ferror(s)) {
            fclose(s);
            free(v);
            return NULL;
        }
        if (feof(s))
            break;
        if (n + 1 == cap) {
            cap *= 2;
            next = realloc(v, cap);
            if (!next) {
                fclose(s);
                free(v);
                return NULL;
            }
            v = next;
        }
    }
    fclose(s);
    v[n] = 0;
    return v;
}
int fx_copy(const char *a, const char *b)
{
    FILE *in = fopen(a, "rb"), *out;
    char buf[32768], path[4096], *at;
    size_t n;
    int ok = 1;
    struct stat st;
    if (!in || stat(a, &st) || strlen(b) >= sizeof(path)) {
        if (in)
            fclose(in);
        return 0;
    }
    strcpy(path, b);
    at = strrchr(path, '/');
    if (at) {
        *at = 0;
        if (!fx_mkdir(path)) {
            fclose(in);
            return 0;
        }
    }
    out = fopen(b, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)))
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out))
        ok = 0;
    return !chmod(b, st.st_mode & 0777) && ok;
}
int fx_tree(const char *a, const char *b)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char from[4096], to[4096];
    int ok = 1;
    if (lstat(a, &st))
        return 0;
    if (S_ISLNK(st.st_mode)) {
        ssize_t n = readlink(a, from, sizeof(from) - 1);
        if (n < 0)
            return 0;
        from[n] = 0;
        return !symlink(from, b);
    }
    if (!S_ISDIR(st.st_mode))
        return fx_copy(a, b);
    if (!fx_mkdir(b) || (d = opendir(a)) == NULL)
        return 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(from, sizeof(from), "%s/%s", a, e->d_name) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", b, e->d_name) >= (int)sizeof(to) ||
            !fx_tree(from, to))
            ok = 0;
    }
    closedir(d);
    chmod(b, st.st_mode & 0777);
    return ok;
}
int fx_remove(const char *p)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char q[4096];
    int ok = 1;
    if (lstat(p, &st))
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return !unlink(p);
    chmod(p, 0700);
    d = opendir(p);
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(q, sizeof(q), "%s/%s", p, e->d_name) >= (int)sizeof(q) || !fx_remove(q))
            ok = 0;
    }
    closedir(d);
    return !rmdir(p) && ok;
}
int fx_count(const char *p)
{
    DIR *d = opendir(p);
    struct dirent *e;
    int n = 0;
    if (!d)
        return -1;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    closedir(d);
    return n;
}
int fx_empty(const char *p)
{
    return fx_count(p) == 0;
}
int fx_same(const char *a, const char *b)
{
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    int c, d, ok = 1;
    if (!x || !y) {
        if (x)
            fclose(x);
        if (y)
            fclose(y);
        return 0;
    }
    do {
        c = fgetc(x);
        d = fgetc(y);
        if (c != d) {
            ok = 0;
            break;
        }
    } while (c != EOF);
    ok = ok && !ferror(x) && !ferror(y);
    fclose(x);
    fclose(y);
    return ok;
}
int fx_equal(const char *p, const char *s)
{
    char *t = fx_read(p);
    int ok = t && !strcmp(t, s);
    free(t);
    return ok;
}
int fx_has(const char *p, const char *s)
{
    char *t = fx_read(p);
    int ok = t && strstr(t, s) != NULL;
    free(t);
    return ok;
}
int fx_regex(const char *s, const char *p)
{
    regex_t r;
    int ok;
    if (regcomp(&r, p, REG_EXTENDED | REG_NEWLINE))
        return 0;
    ok = !regexec(&r, s, 0, NULL, 0);
    regfree(&r);
    return ok;
}
int fx_replace(const char *a, const char *b, const char *from, const char *to)
{
    char *s = fx_read(a), *v, *at, *next;
    size_t n, m, k = 0;
    int ok;
    if (!s || !*from) {
        free(s);
        return 0;
    }
    n = strlen(from);
    m = strlen(to);
    for (at = s; (at = strstr(at, from)); at += n)
        k++;
    v = malloc(strlen(s) + k * m + 1);
    if (!v) {
        free(s);
        return 0;
    }
    v[0] = 0;
    at = s;
    while ((next = strstr(at, from))) {
        strncat(v, at, (size_t)(next - at));
        strcat(v, to);
        at = next + n;
    }
    strcat(v, at);
    ok = fx_text(b, v);
    free(v);
    free(s);
    return ok;
}
void fx_env(const char *key, const char *value)
{
    if (value)
        setenv(key, value, 1);
    else
        unsetenv(key);
}
void fx_pause(void)
{
    struct timespec t = {0, 20000000};
    nanosleep(&t, NULL);
}
int fx_start_io(struct fixture *f, struct fx_child *p, const char *const args[], const char *cwd,
                const char *const env[], const struct fx_io *io)
{
    const char *input = io ? io->input : NULL, *output = io ? io->output : NULL;
    int i;
    pid_t child;
    interrupted();
    if (ngroups == sizeof(groups) / sizeof(groups[0]))
        return 0;
    memset(p, 0, sizeof(*p));
    snprintf(p->out, sizeof(p->out), "%s/command.%u.out", f->work, ++f->command);
    snprintf(p->err, sizeof(p->err), "%s/command.%u.err", f->work, f->command);
    if (output)
        snprintf(p->out, sizeof(p->out), "%s", output);
    child = fork();
    if (child < 0)
        return 0;
    if (!child) {
        int in = open(input ? input : "/dev/null", O_RDONLY),
            out = open(p->out, O_CREAT | O_TRUNC | O_WRONLY, 0600),
            err = open(p->err, O_CREAT | O_TRUNC | O_WRONLY, 0600);
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        if (in < 0 || out < 0 || err < 0 || dup2(in, 0) < 0 || dup2(out, 1) < 0 || dup2(err, 2) < 0)
            _exit(126);
        close(in);
        close(out);
        close(err);
        if (cwd && chdir(cwd))
            _exit(126);
        for (i = 0; env && env[i]; i++) {
            char *entry = strdup(env[i]), *eq = strchr(entry, '=');
            if (eq) {
                *eq = 0;
                setenv(entry, eq + 1, 1);
            } else
                unsetenv(entry);
            free(entry);
        }
        execvp(args[0], (char *const *)args);
        perror(args[0]);
        _exit(127);
    }
    p->pid = child;
    groups[ngroups++] = child;
    setpgid(child, child);
    return 1;
}
int fx_start(struct fixture *f, struct fx_child *p, const char *const a[], const char *cwd,
             const char *const env[], const char *input)
{
    struct fx_io io = {input, NULL};
    return fx_start_io(f, p, a, cwd, env, &io);
}
int fx_poll(struct fx_child *p)
{
    int st;
    pid_t n;
    if (p->done)
        return 1;
    n = waitpid(p->pid, &st, WNOHANG);
    if (n == 0)
        return 0;
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        p->status = 255;
        p->done = 1;
        return 1;
    }
    p->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    p->done = 1;
    return 1;
}
void fx_stop(struct fx_child *p, int sig)
{
    if (p->pid > 0)
        kill(-p->pid, sig);
}
int fx_wait(struct fixture *f, struct fx_child *p)
{
    while (!fx_poll(p)) {
        interrupted();
        fx_pause();
    }
    interrupted();
    free(f->out);
    free(f->err);
    f->out = fx_read(p->out);
    f->err = fx_read(p->err);
    f->status = p->status;
    printf("command %u: status %d\n%s%s", f->command, f->status, f->out ? f->out : "",
           f->err ? f->err : "");
    fflush(stdout);
    return f->status;
}
int fx_run(struct fixture *f, const char *const a[], const char *cwd, const char *const env[],
           const char *input)
{
    struct fx_child p;
    if (!fx_start(f, &p, a, cwd, env, input)) {
        f->status = 255;
        return 255;
    }
    return fx_wait(f, &p);
}
int fx_sh(struct fixture *f, const char *command)
{
    const char *a[] = {"/bin/sh", "-c", command, NULL};
    return fx_run(f, a, f->work, NULL, NULL);
}
int fx_mock(struct fixture *f, const char *name)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", f->bin, name);
    return !link(f->self, path) || fx_copy(f->self, path);
}
int fx_until(struct fx_child *p, const char *path)
{
    while (!fx_exists(path)) {
        if (fx_poll(p))
            return 0;
        fx_pause();
    }
    return 1;
}
int fx_check(struct fixture *f, int held, const char *label)
{
    if (!held) {
        fprintf(stderr, "fixture: failed: %s\n", label);
        f->failed = 1;
    }
    return held;
}
int fx_init(struct fixture *f, const char *root, const char *name, const char *self)
{
    memset(f, 0, sizeof(*f));
    owner = f;
    if (geteuid() == 0) {
        struct passwd *account = getpwnam("test");
        if (!account || !fx_mkdir(root) || chown(root, account->pw_uid, account->pw_gid) ||
            initgroups(account->pw_name, account->pw_gid) || setgid(account->pw_gid) ||
            setuid(account->pw_uid))
            return 0;
    }

    if (!ngroups) {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = signal_children;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        atexit(children_cleanup);
    }
    if (!realpath(self, f->self))
        return 0;
    snprintf(f->root, sizeof(f->root), "%s", root);
    snprintf(f->tree, sizeof(f->tree), "%s/tree", root);
    snprintf(f->work, sizeof(f->work), "%s/%s", root, name);
    snprintf(f->bin, sizeof(f->bin), "%s/bin", f->work);
    return fx_remove(f->work) && fx_mkdir(f->bin);
}
void fx_destroy(struct fixture *f)
{
    children_cleanup();
    owner = NULL;
    free(f->out);
    free(f->err);
    if (!f->failed)
        fx_remove(f->work);
}
