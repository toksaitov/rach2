/* The executor's complete output files must not depend on capture limits. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/exec.h"
#include "output_sinks.h"

#define BYTES 100000

static int seed(const char *path)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (f == NULL)
        return 0;
    ok = fputs("old bytes must disappear from this file\n", f) >= 0;
    return fclose(f) == 0 && ok;
}

static char *contents(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    char *text = malloc(BYTES + 64);

    *size = 0;
    if (f == NULL || text == NULL) {
        if (f != NULL)
            fclose(f);
        free(text);
        return NULL;
    }
    *size = fread(text, 1, BYTES + 63, f);
    if (ferror(f) || !feof(f)) {
        fclose(f);
        free(text);
        return NULL;
    }
    fclose(f);
    text[*size] = '\0';
    return text;
}

static int pattern(const char *text, size_t n)
{
    size_t i;

    if (n != BYTES)
        return 0;
    for (i = 0; i < n; i++)
        if (text[i] != 'a' + i % 23)
            return 0;
    return 1;
}

static int attempt(struct etude_exec *ex, struct etude_exec_spec *spec, int merged)
{
    struct etude_proc *p = NULL;
    const struct etude_exec_result *r;
    char *out = NULL, *err = NULL, *marker;
    size_t nout = 0, nerr = 0;
    int ok = 0;

    if (etude_exec_start(ex, spec, &p) != 0)
        return 0;
    while (etude_exec_wait(ex, -1) != p)
        ;
    r = etude_proc_result(p);
    if (r->end != ETUDE_END_EXITED || r->value != 0 || r->output_error != 0 ||
        r->cleanup != ETUDE_CLEANUP_CONFIRMED || r->out.kept != 17 || r->out.total != BYTES ||
        r->err.total != 9 || memcmp(r->out.data, "abcdefghijklmnopq", 17) != 0 ||
        strcmp(r->err.data, "big done\n") != 0)
        return 0;
    out = contents(spec->stdout_path, &nout);
    if (!merged) {
        err = contents(spec->stderr_path, &nerr);
        ok = out != NULL && err != NULL && pattern(out, nout) && nerr == 9 &&
             memcmp(err, "big done\n", 9) == 0;
    } else if (out != NULL && nout == BYTES + 9 && (marker = strstr(out, "big done\n")) != NULL) {
        /* Pipe service order may place stderr between stdout chunks. */
        memmove(marker, marker + 9, nout - (size_t)(marker - out) - 9);
        ok = pattern(out, nout - 9);
    }
    free(out);
    free(err);
    return ok;
}

int etude_test_output_sinks(const char *target, const char *mode, const char *scratch)
{
    char out[4096] = {0}, err[4096] = {0}, absent[4096];
    char *argv[] = {(char *)target, (char *)mode, "100000", NULL};
    struct etude_exec_spec spec;
    struct etude_exec *ex = etude_exec_new();
    struct etude_proc *p = NULL;
    int ok = 0;

    if (snprintf(out, sizeof out, "%s/output-sinks.out", scratch) >= (int)sizeof out ||
        snprintf(err, sizeof err, "%s/output-sinks.err", scratch) >= (int)sizeof err ||
        snprintf(absent, sizeof absent, "%s/no-such-parent/output", scratch) >=
            (int)sizeof absent ||
        ex == NULL)
        goto done;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "output-sinks";
    spec.argv = argv;
    spec.output_max = 17;
    spec.stdout_path = out;
    spec.stderr_path = err;
    if (!seed(out) || !seed(err) || !attempt(ex, &spec, 0))
        goto done;
    /* Starting again truncates both old files, even after an earlier run. */
    if (!attempt(ex, &spec, 0))
        goto done;
    spec.stderr_path = out;
    if (!seed(out) || !attempt(ex, &spec, 1))
        goto done;
    /* Closing a merged stream once must release the file for replacement. */
    if (remove(out) != 0 || !seed(out))
        goto done;
    spec.stderr_path = absent;
    errno = 0;
    if (etude_exec_start(ex, &spec, &p) != -1 || errno == 0 || etude_exec_wait(ex, 0) != NULL ||
        remove(out) != 0)
        goto done;
    ok = 1;
done:
    if (ex != NULL)
        etude_exec_free(ex);
    if (!ok)
        fprintf(stderr, "output-sinks: complete output, capture or file ownership differed\n");
    if (out[0])
        remove(out);
    if (err[0])
        remove(err);
    return ok ? 0 : 1;
}
