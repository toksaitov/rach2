/* GNU port resume and make jobserver regressions with native C control. */
#include "build_support.h"
#include <time.h>

static int tree_same(const char *a, const char *b)
{
    struct stat x, y;
    if (lstat(a, &x) || lstat(b, &y) || S_ISDIR(x.st_mode) != S_ISDIR(y.st_mode))
        return 0;
    if (!S_ISDIR(x.st_mode))
        return fx_same(a, b);
    if (fx_count(a) != fx_count(b))
        return 0;
    DIR *d = opendir(a);
    struct dirent *e;
    int same = d != NULL;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        same &= tree_same(bp(a, e->d_name), bp(b, e->d_name));
    }
    if (d)
        closedir(d);
    return same;
}

static int recipe(struct fixture *f, const char *name, const char *depends, const char *install)
{
    char text[4096], relative[256];
    snprintf(relative, sizeof(relative), "repo/vendor/gnu/ports/%s.sh", name);
    snprintf(text, sizeof(text),
             "VERSION=1\nSOURCE=\nDEPENDS=\"%s\"\nport_build()\n{\n"
             "    echo %s >> \"$LOG\"\n"
             "    [ ! -e \"$ROOT/usr/bin/%s\" ] || echo \"%s saw itself\" >> \"$LOG\"\n"
             "    [ \"%s\" != b ] || [ ! -f \"$STOP\" ]\n}\n"
             "port_install()\n{\n    cd \"$DEST\" && %s\n}\n",
             depends, name, name, name, name, install);
    return fx_text(bp(f->work, relative), text);
}

static int resume_tree(struct fixture *f)
{
    const char *files[] = {"vendor/gnu/build_port.sh", "vendor/gnu/arch.sh", "mk/arch.sh", NULL};
    for (size_t i = 0; files[i]; i++)
        if (!fx_copy(bp(f->tree, files[i]), bp(bp(f->work, "repo"), files[i])))
            return 0;
    if (!fx_mock(f, "cc"))
        return 0;
    return recipe(f, "runtime", "", "mkdir -p lib && echo libc > lib/libc.so") &&
           recipe(f, "pkgconf", "", "mkdir -p usr/bin && echo pkgconf > usr/bin/pkgconf") &&
           recipe(
               f, "a", "",
               "mkdir -p usr/bin usr/share && echo a > usr/bin/a && echo a > usr/share/common") &&
           recipe(f, "b", "a", "mkdir -p usr/bin && echo b > usr/bin/b") &&
           recipe(f, "c", "",
                  "mkdir -p usr/bin usr/share && echo c > usr/bin/c && echo c > usr/share/common");
}

static int fresh(struct fixture *f)
{
    const char *out = bp(f->work, "out"), *tools[] = {"gcc", "g++", "c++", "cpp", "strip", NULL};
    if (!fx_remove(out) || !fx_remove(bp(f->work, "log")) || !fx_remove(bp(f->work, "stop")))
        return 0;
    if (!fx_mkdir(bp(out, "cross/bin")))
        return 0;
    for (size_t i = 0; tools[i]; i++) {
        char name[256];
        snprintf(name, sizeof(name), "i686-linux-gnu-%s", tools[i]);
        if (!fx_copy(f->self, bp(bp(out, "cross/bin"), name)))
            return 0;
    }
    return 1;
}

static int ports(struct fixture *f, const char *key, const char *arg)
{
    fx_env("ETUDE_STEP_KEY", key);
    if (!fx_text(bp(f->work, "log"), ""))
        return 125;
    const char *args[] = {"/bin/sh", bp(f->work, "repo/vendor/gnu/build_port.sh"), arg, NULL};
    return fx_run(f, args, f->work, NULL, NULL);
}

static int stopped_ports(struct fixture *f)
{
    if (!fresh(f) || !fx_text(bp(f->work, "stop"), ""))
        return 0;
    fx_check(f, ports(f, "k1", "--all") != 0, "the interrupted port run fails in b");
    if (!fx_remove(bp(f->work, "stop")))
        return 0;
    return fx_text(bp(f->work, "out/stage/b/usr/bin/b"), "b\n") &&
           fx_text(bp(f->work, "out/stage/b.files"), "./usr/bin/b\n") &&
           fx_text(bp(f->work, "out/root/usr/bin/b"), "b\n");
}

static int again(struct fixture *f, const char *row, const char *want, const char *key)
{
    int ok = ports(f, key, "--all") == 0 && fx_equal(bp(f->work, "log"), want) &&
             tree_same(bp(f->work, "ref"), bp(f->work, "out/root")) &&
             !fx_exists(bp(f->work, "out/work/ports.done"));
    return fx_check(f, ok, row);
}

int build_resume(struct fixture *f)
{
    if (!resume_tree(f))
        return 0;
    fx_env("RACH2_GNU_OUT", bp(f->work, "out"));
    fx_env("RACH2_ARCH", "i686");
    fx_env("LOG", bp(f->work, "log"));
    fx_env("STOP", bp(f->work, "stop"));
    if (!fresh(f))
        return 0;
    fx_check(f,
             ports(f, "k1", "--all") == 0 &&
                 fx_equal(bp(f->work, "log"), "runtime\npkgconf\na\nb\nc\n"),
             "whole port build");
    fx_check(f, !fx_exists(bp(f->work, "out/work/ports.done")),
             "whole build removes completion list");
    if (!fx_tree(bp(f->work, "out/root"), bp(f->work, "ref")) || !stopped_ports(f))
        return 0;
    fx_check(f, fx_equal(bp(f->work, "out/work/ports.done"), "key k1\nruntime\npkgconf\na\n"),
             "interrupted build records only completed ports and key");
    again(f, "same-key resume", "b\nc\n", "k1");
    fx_check(f, output_has(f, "resume: keeping the 3 ports"),
             "resume reports three retained ports");
    if (!stopped_ports(f))
        return 0;
    again(f, "another key rebuilds all", "runtime\npkgconf\na\nb\nc\n", "k2");
    if (!stopped_ports(f))
        return 0;
    again(f, "no key rebuilds all", "runtime\npkgconf\na\nb\nc\n", NULL);
    if (!stopped_ports(f))
        return 0;
    fx_check(f, ports(f, "k1", "c") == 0, "lone port builds");
    again(f, "lone port invalidates resume", "runtime\npkgconf\na\nb\nc\n", "k1");
    if (!stopped_ports(f) || !fx_remove(bp(f->work, "out/stage/a")))
        return 0;
    again(f, "missing stage invalidates resume", "runtime\npkgconf\na\nb\nc\n", "k1");
    return !f->failed;
}

/* A one-second job is the workload that makes simultaneous jobs observable.
 * Its elapsed time is never an assertion or a bound on the product. */
int build_job_process(const char *name)
{
    struct timespec remaining = {1, 0};
    const char *running = getenv("RUNNING"), *counts = getenv("COUNTS");
    if (!running || !counts || !fx_text(bp(running, name), ""))
        return 91;
    FILE *f = fopen(counts, "a");
    if (!f)
        return 92;
    fprintf(f, "%d\n", fx_count(running));
    if (fclose(f))
        return 93;
    while (nanosleep(&remaining, &remaining) && errno == EINTR) {
    }
    return unlink(bp(running, name)) ? 94 : 0;
}

static int jobs_tree(struct fixture *f)
{
    char text[16384];
    if (!fx_mkdir(bp(f->work, "src")) || !fx_mock(f, "getconf") || !fx_mock(f, "nproc") ||
        !fx_copy(f->self, bp(f->work, "src/bootstrap")))
        return 0;
    snprintf(text, sizeof(text),
             "all: j1 j2 j3 j4 j5 j6 j7 j8\n"
             "j1 j2 j3 j4 j5 j6 j7 j8:\n\t@'%s' --job $@\n"
             ".PHONY: all j1 j2 j3 j4 j5 j6 j7 j8\n",
             f->self);
    if (!fx_text(bp(f->work, "jobs.mk"), text))
        return 0;
    snprintf(text, sizeof(text),
             "SRC='%s/src'\nOUT='%s/out'\nconfigure_cmake() { :; }\n"
             ". '%s/vendor/gnu/ports/cmake.sh'\ncd '%s/build' && port_build\n",
             f->work, f->work, f->tree, f->work);
    if (!fx_text(bp(f->work, "port.sh"), text))
        return 0;
    snprintf(text, sizeof(text), "all:\n\t+@/bin/sh '%s/port.sh'\n", f->work);
    return fx_text(bp(f->work, "outer.mk"), text);
}

static int jobs_row(struct fixture *f, int width)
{
    const char *names[] = {"build", "out", "running", "counts", NULL};
    for (size_t i = 0; names[i]; i++)
        if (!fx_remove(bp(f->work, names[i])))
            return 0;
    if (!fx_mkdir(bp(f->work, "build")) || !fx_mkdir(bp(f->work, "running")) ||
        !fx_text(bp(f->work, "build/Makefile"), "all:\n") || !fx_text(bp(f->work, "counts"), ""))
        return 0;
    const char *jobserver[] = {"make", "-s", "-j2", "-f", bp(f->work, "outer.mk"), NULL};
    const char *alone[] = {"/bin/sh", bp(f->work, "port.sh"), NULL};
    const char *settings[] = {width == 3 ? "MAKEFLAGS=-j3" : "MAKEFLAGS=", NULL};
    int ok = fx_run(f, width == 3 ? alone : jobserver, f->work, settings, NULL) == 0;
    char *text = fx_read(bp(f->work, "counts")), *save;
    int n = 0, maximum = 0;
    for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        int count = atoi(line);
        n++;
        if (count > maximum)
            maximum = count;
    }
    free(text);
    return fx_check(f, ok && n == 8 && maximum <= width,
                    width == 2 ? "make jobserver limits eight jobs to two"
                               : "MAKEFLAGS limits eight jobs to three");
}

int build_jobs(struct fixture *f)
{
    if (!jobs_tree(f))
        return 0;
    fx_env("JOBS_MK", bp(f->work, "jobs.mk"));
    fx_env("RUNNING", bp(f->work, "running"));
    fx_env("COUNTS", bp(f->work, "counts"));
    return jobs_row(f, 2) && jobs_row(f, 3);
}
