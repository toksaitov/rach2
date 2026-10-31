/* Construct the counted preparation and collection vectors of a routine
 * sequence, which runs whole or in two runs: the preparation's build, and
 * later the collection of that build under an observer of its own. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "routine_entry.h"
#include "host_prepare.h"
#include "observe_sequence.h"
#include "authority_run.h"

/* How a sequence runs: whole; its preparation's build alone; or the
 * collection of a built preparation, whose context it composes first. */
enum sequence_form { SEQUENCE_WHOLE, SEQUENCE_PREPARE, SEQUENCE_COLLECT };

static void number(struct hp_words *args, unsigned long long value)
{
    char text[32];
    snprintf(text, sizeof text, "%llu", value);
    hp_add(args, text);
}

static void fixed_inputs(struct hp_words *args, const struct routine_input *in)
{
    const char *const values[] = {in->purpose, in->tree, in->out,     in->arches,
                                  in->run,     in->site, in->program, in->accel};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        hp_add(args, values[i]);
    for (int i = 0; i < in->count; i++)
        hp_add(args, in->selection[i]);
}

static unsigned sequence_limit(const char *purpose)
{
    if (!strcmp(purpose, "push"))
        return 270;
    if (!strcmp(purpose, "ci"))
        return 540;
    if (!strcmp(purpose, "release"))
        return 13800;
    return !strcmp(purpose, "phase_first_run") ? 86400 : 0;
}

static int sequence_environment(const struct routine_input *in, const struct routine_site *site)
{
    const char *admitted = getenv("ETUDE_ADMITTED");
    if (admitted && *admitted) {
        fputs("etude routine: ETUDE_ADMITTED: set; a sequence starts outside admission\n", stderr);
        return -1;
    }
    char jobs[32], path[ROUTINE_PATH];
    snprintf(jobs, sizeof jobs, "%u", site->jobs);
    if (setenv("ETUDE_BUILD_JOBS", jobs, 1) || setenv("JOBS", jobs, 1) ||
        setenv("RACH2_MIN_FREE_GIB", "40", 0) || setenv("ETUDE_SITE", in->site, 1) ||
        routine_path(path, in->out, "preparation.grant.json") ||
        setenv("ETUDE_PREPARATION_GRANT", path, 1))
        return -1;
    const char *clean = getenv("ETUDE_PREPARATION_CLEAN");
    int first = !strcmp(in->purpose, "phase_first_run");
    if (first && clean && strcmp(clean, "1"))
        return -1;
    if (!strcmp(in->purpose, "release") || (first && clean)) {
        if (setenv("ETUDE_PREPARATION_CLEAN", "1", 1))
            return -1;
    } else if (unsetenv("ETUDE_PREPARATION_CLEAN"))
        return -1;
#ifdef _WIN32
    if (setenv("MSYS2_ARG_CONV_EXCL", "*", 1))
        return -1;
#endif
    return unsetenv("MAKEFLAGS") || unsetenv("MFLAGS") ? -1 : 0;
}

/**
 * Add the admitted preparation of `form`'s stage to `args`: the whole
 * preparation, its build, or the context of a built one, which takes a
 * claim, `RUN:context`, and a result file of its own beside the build's.
 */
static int preparation(struct hp_words *args, const struct routine_input *in,
                       const struct routine_site *site, unsigned limit, enum sequence_form form)
{
    static const char *const stages[] = {"routine", "routine-build", "routine-context"};
    int composed = form == SEQUENCE_COLLECT;
    char result[ROUTINE_PATH], claim[512], admitted[ROUTINE_PATH + 32];
    char *env = hp_program("env");
    int n = snprintf(claim, sizeof claim, "%s:%s", in->run, composed ? "context" : "prepare");
    int m = snprintf(admitted, sizeof admitted, "ETUDE_ADMITTED=%s", in->site);
    if (!env || n < 0 || n >= (int)sizeof claim || m < 0 || m >= (int)sizeof admitted ||
        routine_path(result, in->out,
                     composed ? "context.admit.result" : "preparation.admit.result")) {
        free(env);
        return -1;
    }
    const char *const prefix[] = {in->program, "admit", "-d", site->locks, "-w"};
    for (size_t i = 0; i < sizeof prefix / sizeof *prefix; i++)
        hp_add(args, prefix[i]);
    number(args, site->wait);
    hp_add(args, "-r");
    hp_add(args, result);
    hp_add(args, site->ledger);
    hp_add(args, claim);
    hp_add(args, site->account);
    hp_add(args, "shared");
    hp_add(args, site->resources);
    hp_add(args, site->capacity_text);
    number(args, limit);
    hp_add(args, env);
    hp_add(args, admitted);
    hp_add(args, in->program);
    hp_add(args, "prepare");
    hp_add(args, stages[form]);
    fixed_inputs(args, in);
    free(env);
    return 0;
}

static int collection(struct hp_words *args, const struct routine_input *in)
{
    char record[ROUTINE_PATH], evidence[ROUTINE_PATH], context[ROUTINE_PATH];
    if (routine_path(record, in->out, "prepare.record") ||
        routine_path(evidence, in->out, "failed-evidence") ||
        routine_path(context, in->out, "context/context.json"))
        return -1;
    const char *const values[] = {in->program, "run",   "-t", in->tree,   "-a", in->arches,
                                  "-p",        record,  "-o", in->out,    "-e", evidence,
                                  "-I",        context, "-g", in->commit, "-A", in->accel};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        hp_add(args, values[i]);
    for (int i = 0; i < in->count; i++)
        hp_add(args, in->selection[i]);
    return 0;
}

static int counted(struct hp_words *args, const struct routine_input *in,
                   const struct hp_words *command)
{
    if (command->n > 65536)
        return -1;
    hp_add(args, in->program);
    number(args, command->n);
    for (size_t i = 0; i < command->n; i++)
        hp_add(args, command->v[i]);
    return 0;
}

/** The admission bound of a preparation: its purpose's, within the site's. */
static unsigned admission_limit(const struct routine_input *in, const struct routine_site *site)
{
    unsigned limit = sequence_limit(in->purpose);
    return limit > site->seconds ? site->seconds : limit;
}

/**
 * Run the admitted build of a preparation as one owned child, unobserved,
 * with its output on this process's own; the collection's observer starts
 * later, in `run routine-collect`. A stop, or the loss of the caller, cancels
 * the child and its claim.
 *
 * @return the build's status, 128 plus the signal after a cancellation, or
 *         125 when its cleanup was not confirmed.
 */
static int prepare(const struct routine_input *in, const struct routine_site *site)
{
    struct hp_words prep = {0};
    int result = preparation(&prep, in, site, admission_limit(in, site), SEQUENCE_PREPARE) ? 2 : 0;
    if (!result) {
        struct authority_run run = {.phase = "routine-preparation", .argv = prep.v};
        struct etude_exec_result end = {0};
        result = authority_run_command(&run, &end);
    }
    hp_words_free(&prep);
    return result;
}

static int sequence(struct routine_input *in, const struct routine_site *site,
                    enum sequence_form form)
{
    char receipt[ROUTINE_PATH], trace[ROUTINE_PATH], stop[ROUTINE_PATH], log[ROUTINE_PATH];
    const char *direct = getenv("ETUDE_ROUTINE_DIRECT");
    int linux = direct && !strcmp(direct, "1");
    const char *prepare_path = getenv("PATH"), *runtime_path = getenv("ETUDE_RUNTIME_PATH");
    if (!runtime_path || !*runtime_path)
        runtime_path = prepare_path;
    if (!prepare_path || !*prepare_path ||
        routine_path(receipt, in->out, linux ? "linux.sequence.receipt" : "sequence.receipt") ||
        routine_path(log, in->out, linux ? "linux.log" : "log") ||
        routine_path(trace, in->out, "outer-load.jsonl") ||
        routine_path(stop, in->out, "outer-stop.request") || setenv("ETUDE_SEQUENCE_LOG", log, 1))
        return 2;
    struct hp_words prep = {0}, run = {0}, args = {0};
    int result =
        preparation(&prep, in, site, admission_limit(in, site), form) || collection(&run, in);
    if (!result) {
        const char *const values[] = {in->program, "sequence",  receipt,      in->run,     trace,
                                      stop,        in->program, prepare_path, runtime_path};
        for (size_t i = 0; i < sizeof values / sizeof *values; i++)
            hp_add(&args, values[i]);
        result = counted(&args, in, &prep) || counted(&args, in, &run);
    }
    if (!result)
        result = etude_observe_sequence_main((int)args.n, args.v);
    hp_words_free(&prep);
    hp_words_free(&run);
    hp_words_free(&args);
    return result;
}

static int sequence_main(int argc, char **argv, enum sequence_form form)
{
    struct routine_input in;
    struct routine_site site;
    struct json *phase = NULL;
    if (routine_input_read(argc, argv, &in))
        return 2;
    if (!sequence_limit(in.purpose)) {
        fprintf(stderr, "etude routine: PURPOSE: %s has no sequence\n", in.purpose);
        return 2;
    }
    if (routine_phase_inputs(in.purpose, &phase))
        return 2;
    json_free(phase);
    const char *route = getenv("ETUDE_ROUTINE_ROUTE");
    if (route && *route) {
        if (form != SEQUENCE_WHOLE) {
            fputs("etude routine: ETUDE_ROUTINE_ROUTE: set; a sequence in two runs has no route\n",
                  stderr);
            return 2;
        }
        /* The physical route entry uses the same native lifetime owner. */
        return etude_routine_owner_main(argc, argv);
    }
    if (routine_site_read(in.site, NULL, &site) || sequence_environment(&in, &site))
        return 2;
    if (form == SEQUENCE_PREPARE)
        return prepare(&in, &site);
    /* The context's claim reserves memory alone and writes no grant. */
    if (form == SEQUENCE_COLLECT && unsetenv("ETUDE_PREPARATION_GRANT"))
        return 2;
    return sequence(&in, &site, form);
}

int etude_routine_sequence_main(int argc, char **argv)
{
    return sequence_main(argc, argv, SEQUENCE_WHOLE);
}

int etude_routine_prepare_main(int argc, char **argv)
{
    return sequence_main(argc, argv, SEQUENCE_PREPARE);
}

int etude_routine_collect_main(int argc, char **argv)
{
    return sequence_main(argc, argv, SEQUENCE_COLLECT);
}
