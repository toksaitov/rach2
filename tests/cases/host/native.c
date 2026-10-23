/* native.c: portable files, processes and reports for host cases. */
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "etude/exec.h"
#include "native.h"
#ifdef _WIN32
#include "win/files.h"
#else
extern char **environ;
#endif

static struct native_case *active;
static volatile sig_atomic_t interrupted;

int native_path(char out[4096], const char *base, const char *name)
{
    int n = snprintf(out, 4096, "%s/%s", base, name);
    return n < 0 || n >= 4096 ? -1 : 0;
}

static int remove_tree(const char *path)
{
    struct stat st;
    struct dirent *e;
    DIR *d;
    char child[4096];
    int bad = 0;
    if (lstat(path, &st) != 0)
        return errno != ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    if (!(d = opendir(path)))
        return -1;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (native_path(child, path, e->d_name) || remove_tree(child))
            bad = 1;
    }
    closedir(d);
    return bad ? -1 : rmdir(path);
}

static int clean(void)
{
    if (active && active->work[0]) {
        if (active->uncertain || (active->failures && !interrupted))
            fprintf(stderr, "host case: evidence: %s\n", active->work);
        else if (remove_tree(active->work)) {
            fprintf(stderr, "host case: cannot remove %s\n", active->work);
            return -1;
        }
    }
    return 0;
}

static void cleanup(void)
{
    (void)clean();
}

static void stop(int sig)
{
    interrupted = sig;
}

int native_begin(struct native_case *c, const char *id)
{
    const char *tree = getenv("ETUDE_TREE");
    char tmp[4096];
    const char *expected = getenv("ETUDE_CASE");
    memset(c, 0, sizeof(*c));
    if (expected && strcmp(expected, id))
        return -1;
    if (!tree)
        tree = ".";
    if (!realpath(tree, c->tree))
        return -1;
    if (native_path(tmp, c->tree, "build/tests"))
        return -1;
    if (native_path(c->work, tmp, "etude-host.XXXXXX"))
        return -1;
    active = c;
    atexit(cleanup);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
#ifndef _WIN32
    signal(SIGHUP, stop);
#endif
    if (!mkdtemp(c->work)) {
        c->work[0] = 0;
        return -1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("@etude 1 begin %s\n", id);
    return 0;
}

void native_assert(struct native_case *c, const char *id, int pass, const char *why)
{
    char detail[145];
    size_t n = 0;
    while (why && *why && n < sizeof(detail) - 1) {
        unsigned char b = (unsigned char)*why++;
        detail[n++] = b >= 32 && b <= 126 ? (char)b : '?';
    }
    detail[n] = 0;
    printf("@etude 1 assert %s %s%s%s\n", id, pass ? "pass" : "fail", pass ? "" : " ",
           pass ? "" : detail);
    c->records++;
    c->failures += !pass;
}

int native_end(struct native_case *c)
{
    int rc = interrupted ? 128 + interrupted : c->failures != 0 || c->uncertain;
    printf("@etude 1 end %u\n", c->records);
    if (clean())
        rc = 1;
    active = NULL;
    return rc;
}

int native_tool(struct native_case *c, char out[4096], const char *name)
{
    char build[4096];
    return native_path(build, c->tree, "build") || native_path(out, build, name) ? -1 : 0;
}

static int register_child(void *ctx, const char *id, const struct etude_pid *keeper,
                          const struct etude_pid *process)
{
    (void)ctx;
    (void)id;
    return printf("@etude 1 child %ld\n@etude 1 child %ld\n", keeper->pid, process->pid) < 0 ? EIO
                                                                                             : 0;
}

int native_run(struct native_case *c, const char *const argv[])
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_exec_spec spec;
    struct etude_proc *p;
    const struct etude_exec_result *r;
    char out[4096], err[4096], name[80];
    int rc = -1;
    memset(&spec, 0, sizeof(spec));
    snprintf(name, sizeof(name), "command-%u.out", ++c->commands);
    native_path(out, c->work, name);
    snprintf(name, sizeof(name), "command-%u.err", c->commands);
    native_path(err, c->work, name);
    spec.attempt_id = name;
    spec.argv = (char *const *)argv;
    spec.envp = environ;
    spec.cwd = c->tree;
    spec.stdout_path = out;
    spec.stderr_path = err;
    spec.output_max = 1u << 20;
    spec.grace_us = 5000000;
    spec.reg = register_child;
    if (!ex || etude_exec_start(ex, &spec, &p)) {
        if (ex)
            etude_exec_free(ex);
        return -1;
    }
    for (;;) {
        if (interrupted)
            etude_exec_cancel(p);
        if (etude_exec_wait(ex, -1) == p)
            break;
    }
    r = etude_proc_result(p);
    c->uncertain |= r->cleanup != ETUDE_CLEANUP_CONFIRMED;
    if (r->end == ETUDE_END_EXITED && r->cleanup == ETUDE_CLEANUP_CONFIRMED && !r->output_error)
        rc = r->value;
    etude_proc_free(p);
    etude_exec_free(ex);
    if (interrupted)
        exit(128 + interrupted);
    return rc;
}

unsigned char *native_read(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data;
    long length;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 || length > 64 * 1024 * 1024 ||
        fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)length + 1);
    if (!data || fread(data, 1, (size_t)length, f) != (size_t)length || ferror(f)) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    data[length] = 0;
    *size = (size_t)length;
    return data;
}

int native_write(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    int bad;
    if (!f)
        return -1;
    bad = fwrite(data, 1, size, f) != size;
    if (fclose(f))
        bad = 1;
    return bad ? -1 : 0;
}
