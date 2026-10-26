/* flow_launch_claims.c: the claim, environment and refusal of each launch. */
#define _POSIX_C_SOURCE 200809L
#include "flow_launch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A launch, the environment it adds and what the policy's etude is given. */
struct claim_row {
    const char *env;      /* NAME=VALUE words separated by `;`, or "" */
    const char *args;     /* the launcher's arguments separated by `|` */
    const char *expected; /* the recorder's line, or `refused: REASON` */
};

/* The registry of the suites the rows select. */
static const char registry[] = "style|-|full\nhost|-|full||384\ndefault|qemu|core\n"
                               "big|-|full||900000000\nser|-|full||128||serial\n"
                               "x64|-|full||256|x86_64\nbad|-|full||abc\nzero|-|full||0\n";

static const struct claim_row rows[] = {
    {"", "build|make", "shared 4000,5368709120,42949672960 jobs=4 workers=8 pool=16384: make -j1"},
    {"", "build|make|-j|7",
     "shared 7000,8589934592,42949672960 jobs=7 workers=8 pool=16384: make -j 7 -j7"},
    {"", "build|make|-j",
     "shared 24000,26843545600,42949672960 jobs=24 workers=8 pool=16384: make -j -j24"},
    {"JOBS=6", "build|make|--jobs=3",
     "shared 6000,7516192768,42949672960 jobs=6 workers=8 pool=16384: make --jobs=3 -j3"},
    {"", "build|make|JOBS=9",
     "shared 9000,10737418240,42949672960 jobs=9 workers=8 pool=16384: make JOBS=9 -j1"},
    {"MAKEFLAGS=-j5 -- JOBS=12", "build|make",
     "shared 5000,6442450944,42949672960 jobs=5 workers=8 pool=16384: make -j5"},
    {"", "build|gmake|--jobs|6",
     "shared 6000,7516192768,42949672960 jobs=6 workers=8 pool=16384: gmake --jobs 6 -j6"},
    {"", "build|sh|-c|make -j9",
     "shared 8000,27917287424,42949672960 jobs=4 workers=8 pool=16384: sh -c make -j9"},
    {"", "build|make|test",
     "alone 24000,103079215104,171798691840 jobs=4 workers=8 pool=16384: make test -j1"},
    {"", "build|make|-j0", "refused: invalid build job count"},
    {"", "build|make|-j2000", "refused: invalid build job count"},
    {"JOBS=0", "build|make", "refused: invalid build job count"},
    {"", "build|make|-j25", "refused: launch exceeds site capacity"},
    {"", "alone|true", "alone 24000,103079215104,171798691840 jobs=8 workers=8 pool=16384: true"},
    {"", "legacy|sh|tests/run|style",
     "shared 8000,10871635968,42949672960 jobs=8 workers=8 pool=16384: sh tests/run style"},
    {"ARCH=i686 x86_64", "legacy|tests/run|core",
     "shared 8000,11005853696,42949672960 jobs=8 workers=8 pool=16384: tests/run core"},
    {"ARCH=i686 x86_64", "legacy|tests/run|x86_64:default",
     "shared 8000,10871635968,42949672960 jobs=8 workers=8 pool=16384: tests/run x86_64:default"},
    {"", "legacy|tests/run|x64", "refused: no suites selected"},
    {"RACH2_WORKERS=3;RACH2_MEMORY_MIB=500", "legacy|tests/run|style|host|default|x64",
     "shared 3000,5368709120,42949672960 jobs=3 workers=3 pool=500: tests/run style host default "
     "x64"},
    {"RACH2_WORKERS=4;RACH2_MEMORY_MIB=1000;ARCH=x86_64", "legacy|tests/run|host|default|x64",
     "shared 4000,6710886400,42949672960 jobs=4 workers=4 pool=1000: tests/run host default x64"},
    {"", "legacy|tests/run|ser",
     "alone 24000,103079215104,171798691840 jobs=1 workers=1 pool=16384: tests/run ser"},
    {"", "legacy|tests/run|big",
     "alone 24000,103079215104,171798691840 jobs=8 workers=8 pool=16384: tests/run big"},
    {"", "legacy|tests/run|bad", "refused: invalid suite memory"},
    {"", "legacy|tests/run|zero",
     "shared 8000,10871635968,42949672960 jobs=8 workers=8 pool=16384: tests/run zero"},
    {"", "legacy|tests/run|inventory",
     "shared 4000,5368709120,42949672960 jobs=4 workers=8 pool=16384: tests/run inventory"},
    {"", "legacy|/opaque/command|arg",
     "shared 8000,27917287424,42949672960 jobs=8 workers=8 pool=16384: /opaque/command arg"},
    {"RACH2_CPUS=1", "legacy|tests/run|style",
     "shared 8000,10871635968,42949672960 jobs=8 workers=8 pool=16384: tests/run style"},
    {"RACH2_CPUS=8", "legacy|tests/run|style", "refused: launch exceeds site capacity"},
    {"RACH2_CPUS=65", "legacy|tests/run|style", "refused: invalid RACH2_CPUS"},
    {"RACH2_WORKERS=1025", "legacy|tests/run|style", "refused: invalid RACH2_WORKERS"},
    {"RACH2_MEMORY_MIB=01", "legacy|tests/run|style", "refused: invalid RACH2_MEMORY_MIB"},
    {"", "frob|make", "refused: unknown launch kind frob"},
};

/** Split `text` at each `separator` into `words`, after their `first`. */
static void split(char *text, int separator, const char **words, size_t first, size_t max)
{
    size_t n = first;
    for (char *word = text, *next; *text && word && n + 1 < max; word = next) {
        next = strchr(word, separator);
        if (next)
            *next++ = 0;
        words[n++] = word;
    }
    words[n] = NULL;
}

/**
 * Run one row through the launcher whose command is `launcher`, and compare
 * what the policy's etude recorded, or the refusal, with the row.
 */
static int claim_check(struct launch_fixture *u, const char *const launcher[],
                       const struct claim_row *row)
{
    struct fixture *f = u->f;
    char env_text[1024], args_text[1024], label[1600], wanted[300], *seen;
    const char *env[24] = {"RACH2_WORKERS", "RACH2_MEMORY_MIB", "RACH2_CPUS",
                           "JOBS",          "MAKEFLAGS",        "ARCH"};
    const char *argv[24];
    size_t n = 0;
    int status, ok;
    snprintf(env_text, sizeof env_text, "%s", row->env);
    snprintf(args_text, sizeof args_text, "%s", row->args);
    split(env_text, ';', env, 6, 24);
    for (; launcher[n]; n++)
        argv[n] = launcher[n];
    split(args_text, '|', argv, n, 24);
    fx_remove(flow_path(f, "record"));
    status = fx_run(f, argv, u->tree, env, NULL);
    seen = fx_read(flow_path(f, "record"));
    snprintf(wanted, sizeof wanted, "etude-launch: result: %s\n", row->expected);
    if (!strncmp(row->expected, "refused: ", 9))
        ok = status == 2 && !fx_exists(flow_path(f, "record")) && f->err && strstr(f->err, wanted);
    else
        ok = status == 0 && seen && !strcmp(seen, row->expected);
    snprintf(label, sizeof label, "%s: claim of %s %s: %s", launcher[n - 1], row->env, row->args,
             seen && *seen ? seen
             : f->err      ? f->err
                           : "");
    free(seen);
    return fx_check(f, ok, label);
}

/**
 * Give each row to the installed launcher under a policy whose etude is the
 * recorder, from a checkout with the rows' registry.
 */
int launch_claims(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char recorder[4200], *saved = fx_read(flow_path(f, "checkout/tests/registry"));
    const char *launcher[] = {u->launcher, NULL};
    int ok;
    snprintf(recorder, sizeof recorder, "etude %s\n", flow_path(f, "etude-record"));
    ok = saved && fx_copy(f->self, flow_path(f, "etude-record")) &&
         !chmod(flow_path(f, "etude-record"), 0755) &&
         fx_replace(flow_path(f, "site"), flow_path(f, "claims-site"),
                    "etude /host-products/etude\n", recorder) &&
         flow_put(f, "checkout/tests/registry", registry);
    fx_env("ETUDE_SITE", flow_path(f, "claims-site"));
    for (size_t i = 0; ok && i < sizeof rows / sizeof *rows; i++)
        ok = claim_check(u, launcher, &rows[i]);
    fx_env("ETUDE_SITE", flow_path(f, "site"));
    ok = saved && flow_put(f, "checkout/tests/registry", saved) && ok;
    free(saved);
    return ok && launch_clear(u);
}
