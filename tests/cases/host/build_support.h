/* Common code of C-controlled build-product regressions in the GNU guest. */
#ifndef HOST_BUILD_SUPPORT_H
#define HOST_BUILD_SUPPORT_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "fixture.h"

static inline const char *bp(const char *base, const char *name)
{
    static char paths[32][4096];
    static unsigned next;
    char *out = paths[next++ % 32];
    int n = snprintf(out, 4096, "%s/%s", base, name);
    if (n < 0 || n >= 4096)
        abort();
    return out;
}
static inline const char *product_path(struct fixture *f, const char *name)
{
    const char *base = getenv("HOST_PRODUCTS");
    const char *path = bp(base ? base : "/host-products", name);
    return fx_exists(path) ? path : bp(bp(f->root, "prepared"), name);
}
static inline int source_copy(struct fixture *f, const char *name)
{
    return fx_copy(bp(f->tree, name), bp(f->work, name));
}
static inline void build_env(struct fixture *f)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s:%s", f->bin,
             getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
    fx_env("PATH", path);
    fx_env("MAKEFLAGS", "");
    fx_env("MFLAGS", NULL);
    fx_env("MAKELEVEL", NULL);
    fx_env("MAKE", NULL);
    fx_env("ETUDE_STEP_KEY", NULL);
    fx_env("ETUDE_STEP_SHIMS", NULL);
    fx_env("RACH2_GNU_OUT", NULL);
}
static inline int output_has(const struct fixture *f, const char *text)
{
    return (f->out && strstr(f->out, text)) || (f->err && strstr(f->err, text));
}
int build_run_scripts(struct fixture *f);
int build_commit_msg(struct fixture *f);
int build_resume(struct fixture *f);
int build_jobs(struct fixture *f);
int build_host_checks(struct fixture *f);
int build_asm(struct fixture *f);
int build_ldscript(struct fixture *f);
int build_prereq(struct fixture *f);
int build_together(struct fixture *f);
int build_suite(struct fixture *f);
int build_runner(struct fixture *f, const char *id);
int build_job_process(const char *name);
int build_prereq_mock(int argc, char **argv);
int build_maker_process(int argc, char **argv);
int build_mock(int argc, char **argv);
#endif
