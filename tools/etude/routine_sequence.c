/* Construct the existing counted preparation and collection vectors in C. */
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

static int preparation(struct hp_words *args, const struct routine_input *in,
                       const struct routine_site *site, unsigned limit)
{
    char result[ROUTINE_PATH], claim[512], admitted[ROUTINE_PATH + 32];
    char *env = hp_program("env");
    int n = snprintf(claim, sizeof claim, "%s:prepare", in->run);
    int m = snprintf(admitted, sizeof admitted, "ETUDE_ADMITTED=%s", in->site);
    if (!env || n < 0 || n >= (int)sizeof claim || m < 0 || m >= (int)sizeof admitted ||
        routine_path(result, in->out, "preparation.admit.result")) {
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
    hp_add(args, "routine");
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

static int sequence(struct routine_input *in, const struct routine_site *site)
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
    unsigned limit = sequence_limit(in->purpose);
    if (limit > site->seconds)
        limit = site->seconds;
    int result = preparation(&prep, in, site, limit) || collection(&run, in);
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

int etude_routine_sequence_main(int argc, char **argv)
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
        /* The physical route entry uses the same native lifetime owner. */
        return etude_routine_owner_main(argc, argv);
    }
    if (routine_site_read(in.site, NULL, &site) || sequence_environment(&in, &site))
        return 2;
    return sequence(&in, &site);
}
