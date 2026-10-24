/* The executor's complete output files must not depend on capture limits. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/exec.h"
#include "output_sinks.h"
#include "sanitizer_env_host.h"

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
        if (text[i] != (char)('a' + i % 23))
            return 0;
    return 1;
}

static void capture_detail(const char *name, const struct etude_output *output)
{
    size_t i;

    fprintf(stderr, "output-sinks: %s: kept %lu, total %llu, closed %d, bytes", name,
            (unsigned long)output->kept, (unsigned long long)output->total, output->closed);
    if (output->data == NULL) {
        fputs(" unavailable\n", stderr);
        return;
    }
    for (i = 0; i < output->kept; i++)
        fprintf(stderr, " %02x", (unsigned char)output->data[i]);
    fputc('\n', stderr);
}

static int attempt(struct etude_exec *ex, struct etude_exec_spec *spec, int merged)
{
    struct etude_proc *p = NULL;
    const struct etude_exec_result *r;
    char *out = NULL, *err = NULL, *marker;
    size_t nout = 0, nerr = 0;
    int ok = 0;

    if (etude_exec_start(ex, spec, &p) != 0) {
        fprintf(stderr, "output-sinks: start: %s\n", strerror(errno));
        return 0;
    }
    while (etude_exec_wait(ex, -1) != p)
        ;
    r = etude_proc_result(p);
    if (r->end != ETUDE_END_EXITED || r->value != 0 || r->output_error != 0 ||
        r->cleanup != ETUDE_CLEANUP_CONFIRMED || r->out.kept != 17 || r->out.total != BYTES ||
        r->err.total != 9 || r->out.data == NULL || r->err.data == NULL ||
        memcmp(r->out.data, "abcdefghijklmnopq", 17) != 0 ||
        strcmp(r->err.data, "big done\n") != 0)
        goto done;
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
done:
    if (!ok) {
        fprintf(stderr,
                "output-sinks: result: end %d, value %d, output error %d, cleanup %d\n",
                (int)r->end, r->value, r->output_error, (int)r->cleanup);
        fprintf(stderr,
                "output-sinks: predicates: exited %d, zero status %d, no output error %d, "
                "cleanup confirmed %d, stdout kept %d, stdout total %d, stderr total %d, "
                "stdout prefix %d, stderr marker %d\n",
                r->end == ETUDE_END_EXITED, r->value == 0, r->output_error == 0,
                r->cleanup == ETUDE_CLEANUP_CONFIRMED, r->out.kept == 17,
                r->out.total == BYTES, r->err.total == 9,
                r->out.data != NULL && r->out.kept >= 17 &&
                    memcmp(r->out.data, "abcdefghijklmnopq", 17) == 0,
                r->err.data != NULL && strcmp(r->err.data, "big done\n") == 0);
        capture_detail("stdout", &r->out);
        capture_detail("stderr", &r->err);
        fprintf(stderr, "output-sinks: files: merged %d, stdout read %d (%lu bytes), "
                        "stderr read %d (%lu bytes)\n",
                merged, out != NULL, (unsigned long)nout, err != NULL, (unsigned long)nerr);
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
    const char *stage = "initialize paths and executor";
    int ok = 0, saved;

    if (snprintf(out, sizeof out, "%s/output-sinks.out", scratch) >= (int)sizeof out ||
        snprintf(err, sizeof err, "%s/output-sinks.err", scratch) >= (int)sizeof err ||
        snprintf(absent, sizeof absent, "%s/no-such-parent/output", scratch) >=
            (int)sizeof absent ||
        ex == NULL)
        goto done;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "output-sinks";
    spec.argv = argv;
#ifndef _WIN32
    spec.envp = host_sanitizer_environment(getenv("ASAN_SYMBOLIZER_PATH"));
    if (spec.envp == NULL)
        goto done;
#endif
    spec.output_max = 17;
    spec.stdout_path = out;
    spec.stderr_path = err;
    stage = "separate streams";
    if (!seed(out) || !seed(err) || !attempt(ex, &spec, 0))
        goto done;
    /* Starting again truncates both old files, even after an earlier run. */
    stage = "truncate separate streams";
    if (!attempt(ex, &spec, 0))
        goto done;
    spec.stderr_path = out;
    stage = "merged streams";
    if (!seed(out) || !attempt(ex, &spec, 1))
        goto done;
    /* Closing a merged stream once must release the file for replacement. */
    stage = "replace merged file";
    if (remove(out) != 0 || !seed(out))
        goto done;
    spec.stderr_path = absent;
    stage = "refuse missing stderr parent and release stdout file";
    errno = 0;
    if (etude_exec_start(ex, &spec, &p) != -1 || errno == 0 || etude_exec_wait(ex, 0) != NULL ||
        remove(out) != 0)
        goto done;
    ok = 1;
done:
    saved = errno;
    if (ex != NULL)
        etude_exec_free(ex);
    if (!ok) {
        fprintf(stderr, "output-sinks: %s: complete output, capture or file ownership differed "
                        "(errno %d: %s)\n",
                stage, saved, strerror(saved));
        fprintf(stderr, "output-sinks: evidence: %s and %s\n", out, err);
    } else {
        if (out[0])
            remove(out);
        if (err[0])
            remove(err);
    }
    return ok ? 0 : 1;
}
