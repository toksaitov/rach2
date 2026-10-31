/* Ordinary admitted preparation for the native sequence owner, whole or in
 * two stages: the build, and then the source and context on that build. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "routine_entry.h"
#include "routine_proof.h"
#include "host_prepare.h"
#include "measurement_context.h"
#include "authority_internal.h"

/* The part of a preparation that runs: all of it, the build alone, or the
 * source capture and context composition on a finished build. */
enum stage { STAGE_WHOLE, STAGE_BUILD, STAGE_CONTEXT };

struct preparation {
    struct routine_input input;
    enum stage stage;
    struct json *phase;
    char record[ROUTINE_PATH], receipt[ROUTINE_PATH], selection[ROUTINE_PATH];
    char source[ROUTINE_PATH], context[ROUTINE_PATH];
    struct hp_words tools;
    volatile sig_atomic_t *stop;
    int argc;
    char **argv;
};

static void operands(struct hp_words *args, const struct routine_input *input)
{
    for (int i = 0; i < input->count; i++)
        hp_add(args, input->selection[i]);
}

static int command(struct preparation *p, struct hp_words *args)
{
    int result = routine_command(args, p->stop);
    hp_words_free(args);
    return result;
}

static int absent(const char *path)
{
    struct stat st;
    return lstat(path, &st) && errno == ENOENT;
}

/**
 * Check OUT for the stage: the build's outputs absent, or present for the
 * context stage; the source and context absent; and the observer's trace
 * present, or absent for the build stage, which runs unobserved and leaves
 * the trace for the collection's own observer to make.
 */
static int preparation_paths(struct preparation *p)
{
    const struct routine_input *in = &p->input;
    static const char *const built[] = {"prepare.record", "preparation.receipt.json",
                                        "selection.list", "selection.export.json"};
    static const char *const composed[] = {"source.json", "source.descriptor.json", "context"};
    char path[ROUTINE_PATH];
    struct stat st;
    for (size_t i = 0; i < sizeof built / sizeof *built; i++)
        if (routine_path(path, in->out, built[i]) ||
            (p->stage == STAGE_CONTEXT ? lstat(path, &st) || !S_ISREG(st.st_mode) : !absent(path)))
            return -1;
    for (size_t i = 0; i < sizeof composed / sizeof *composed; i++)
        if (routine_path(path, in->out, composed[i]) || !absent(path))
            return -1;
    if (routine_path(path, in->out, "outer-load.jsonl") ||
        (p->stage == STAGE_BUILD ? !absent(path) : stat(path, &st) || !S_ISREG(st.st_mode)) ||
        routine_path(path, in->out, "outer-stop.request") || !absent(path))
        return -1;
    return routine_path(p->record, in->out, "prepare.record") ||
                   routine_path(p->receipt, in->out, "preparation.receipt.json") ||
                   routine_path(p->selection, in->out, "selection.export.json") ||
                   routine_path(p->source, in->out, "source.json") ||
                   routine_path(p->context, in->out, "context")
               ? -1
               : 0;
}

/** Clear the receipt's inputs, which only the build's own command takes. */
static void receipt_inputs_clear(void)
{
    unsetenv("ETUDE_PREPARATION_RUN_ID");
    unsetenv("ETUDE_PREPARATION_RECEIPT");
    unsetenv("ETUDE_PREPARATION_COMMIT");
}

/**
 * Refuse an import in a staged preparation, which takes the ordinary backend
 * alone: an import's route and inputs belong to one admission.
 */
static int staged(const struct preparation *p)
{
    static const char *const names[] = {"ETUDE_ROUTINE_ENSURE", "ETUDE_PREPARATION_IMPORT"};
    if (p->stage == STAGE_WHOLE)
        return 0;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        const char *value = getenv(names[i]);
        if (value && *value) {
            fprintf(stderr, "etude routine: %s: set; a staged preparation does not import\n",
                    names[i]);
            return -1;
        }
    }
    return 0;
}

static int environment(struct preparation *p)
{
    const struct routine_input *in = &p->input;
    const char *admitted = getenv("ETUDE_ADMITTED");
    struct authority_file_id expected, actual;
    if (!admitted || authority_file_id(admitted, &actual) ||
        authority_file_id(in->site, &expected) || expected.device != actual.device ||
        expected.inode != actual.inode)
        return -1;
    int trace = getenv("ETUDE_PHYSICAL_TRACE") && *getenv("ETUDE_PHYSICAL_TRACE");
    int stop = getenv("ETUDE_PHYSICAL_STOP_REQUEST") && *getenv("ETUDE_PHYSICAL_STOP_REQUEST");
    return trace != stop || setenv("ETUDE_ADMITTED", in->site, 1) ||
                   setenv("ETUDE_SITE", in->site, 1) || setenv("RACH2_QEMU_ACCEL", in->accel, 1) ||
                   setenv("ETUDE_PREPARATION_RUN_ID", in->run, 1) ||
                   setenv("ETUDE_PREPARATION_RECEIPT", p->receipt, 1) ||
                   setenv("ETUDE_PREPARATION_COMMIT", in->commit, 1)
               ? -1
               : 0;
}

static int tool(struct hp_words *tools, const char *name, int optional)
{
    char *path = hp_program(name);
    if (!path)
        return optional ? 0 : -1;
    hp_add(tools, "--tool");
    hp_add(tools, path);
    free(path);
    return 0;
}

static int preparation_tools(struct preparation *p)
{
    const char *compiler = getenv("CC");
    if (tool(&p->tools, compiler && *compiler ? compiler : "cc", 0) ||
        tool(&p->tools, "qemu-system-i386", 0) || tool(&p->tools, "qemu-system-x86_64", 0) ||
        tool(&p->tools, "i686-elf-gcc", 1) || tool(&p->tools, "x86_64-elf-gcc", 1))
        return -1;
    return 0;
}

static int allocation(struct preparation *p, int export)
{
    const struct routine_input *in = &p->input;
    struct hp_words args = {0};
    hp_add(&args, in->program);
    hp_add(&args, "run");
    hp_add(&args, "-N");
    hp_add(&args, "-t");
    hp_add(&args, in->tree);
    hp_add(&args, "-a");
    hp_add(&args, in->arches);
    hp_add(&args, "-A");
    hp_add(&args, in->accel);
    hp_add(&args, export ? "-J" : "-p");
    hp_add(&args, export ? p->selection : p->record);
    operands(&args, in);
    int result = export ? command(p, &args) : routine_query(in, &args, "selection", p->stop);
    hp_words_free(&args);
    if (result)
        return result;
    if (!export) {
        char from[ROUTINE_PATH], to[ROUTINE_PATH];
        return routine_path(from, in->out, "selection.stdout") ||
                       routine_path(to, in->out, "selection.list") || routine_copy(from, to)
                   ? -1
                   : 0;
    }
    const char *expected = getenv("ETUDE_PHASE_SELECTION_EXPORT");
    return expected && routine_selection(expected, p->selection) ? -1 : 0;
}

static int ensure_import(struct preparation *p)
{
    const char *route = getenv("ETUDE_ROUTINE_ENSURE");
    if (!route || !*route)
        return 0;
    const struct routine_input *in = &p->input;
    struct hp_words args = {0};
    const char *const fixed[] = {in->program, "prepare",   "route",    "ensure",   "--config",
                                 route,       "--tree",    in->tree,   "--out",    in->out,
                                 "--run",     in->run,     "--commit", in->commit, "--purpose",
                                 in->purpose, "--arches",  in->arches, "--site",   in->site,
                                 "--etude",   in->program, "--accel",  in->accel,  "--"};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    operands(&args, in);
    int result = command(p, &args);
    char path[ROUTINE_PATH];
    if (result)
        return result;
    return routine_path(path, in->out, "preparation-import.json") ||
                   setenv("ETUDE_PREPARATION_IMPORT", path, 1)
               ? -1
               : 0;
}

static int build(struct preparation *p)
{
    const struct routine_input *in = &p->input;
    struct hp_words args = {0};
    const char *const fixed[] = {in->program, "prepare",  "collection", "-t",     in->tree,
                                 "-a",        in->arches, "-p",         p->record};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    operands(&args, in);
    int result = command(p, &args);
    receipt_inputs_clear();
    return result;
}

static int producer_export(struct preparation *p)
{
    const char *selected = getenv("ETUDE_PREPARATION_EXPORT");
    if (!selected || strcmp(selected, "1"))
        return 0;
#if defined(__linux__) && !defined(__rach2__)
    const struct routine_input *in = &p->input;
    char output[ROUTINE_PATH];
    if (routine_path(output, in->out, "producer.json"))
        return -1;
    struct hp_words args = {0};
    const char *const fixed[] = {in->program, "prepare",  "export",    "--tree",   in->tree,
                                 "--commit",  in->commit, "--record",  p->record,  "--receipt",
                                 p->receipt,  "--etude",  in->program, "--output", output};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    return command(p, &args);
#else
    (void)p;
    return -1;
#endif
}

static int reference(struct preparation *p)
{
    const struct routine_input *in = &p->input;
    if (!strcmp(in->purpose, "phase_first_run"))
        return 0;
    const char *path = getenv("ETUDE_REFERENCE");
    char output[ROUTINE_PATH];
    if (!path || !*path || routine_path(output, in->out, "reference.forecast"))
        return -1;
    struct hp_words args = {0};
    const char *const fixed[] = {
        in->program, "prepare",       "reference", "--tree",        in->tree,  "--etude",
        in->program, "--reference",   path,        "--output",      output,    "--site",
        in->site,    "--accelerator", in->accel,   "--preparation", p->receipt};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    return command(p, &args);
}

static int source(struct preparation *p, char hash[65])
{
    const struct routine_input *in = &p->input;
    struct hp_words args = {0};
    const char *const fixed[] = {in->program, "prepare",  "source",   "--tree", in->tree,
                                 "--commit",  in->commit, "--output", p->source};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    int result = routine_query(in, &args, "source", p->stop);
    hp_words_free(&args);
    char from[ROUTINE_PATH], to[ROUTINE_PATH];
    struct stat status;
    if (result)
        return result;
    return etude_context_digest(p->source, &status, hash) ||
                   routine_path(from, in->out, "source.stdout") ||
                   routine_path(to, in->out, "source.descriptor.json") || routine_copy(from, to)
               ? -1
               : 0;
}

static void optional(struct hp_words *args, const char *flag, const char *name)
{
    const char *value = getenv(name);
    if (value && *value) {
        hp_add(args, flag);
        hp_add(args, value);
    }
}

static int context_proofs(struct hp_words *args)
{
    const char *grant = getenv("ETUDE_ROUTINE_GRANT");
    const char *upstream = getenv("ETUDE_ROUTINE_UPSTREAM");
    const char *proofs = getenv("ETUDE_ROUTINE_PROOFS");
    grant = grant && *grant ? grant : NULL;
    upstream = upstream && *upstream ? upstream : NULL;
    proofs = proofs && *proofs ? proofs : NULL;
    if ((grant || upstream || proofs) && (!grant || !proofs))
        return -1;
    optional(args, "--routine-grant", "ETUDE_ROUTINE_GRANT");
    optional(args, "--upstream-preparation", "ETUDE_ROUTINE_UPSTREAM");
    optional(args, "--routine-proofs", "ETUDE_ROUTINE_PROOFS");
    optional(args, "--physical-trace", "ETUDE_PHYSICAL_TRACE");
    optional(args, "--physical-stop-request", "ETUDE_PHYSICAL_STOP_REQUEST");
    optional(args, "--cache-protocol", "ETUDE_CACHE_PROTOCOL");
    optional(args, "--selection-export", "ETUDE_PHASE_SELECTION_EXPORT");
    return 0;
}

static int import_tools(struct preparation *p)
{
    const char *path = getenv("ETUDE_PREPARATION_IMPORT");
    if (!path || !*path)
        return 0;
    struct json *doc = NULL;
    if (routine_json(path, &doc))
        return -1;
    const struct json *producer = json_member(doc, "producer");
    const char *transport = json_text(producer, "transport");
    int result = !transport || tool(&p->tools, "make", 0);
    if (!result && (!strcmp(transport, "ssh") || !strcmp(transport, "wsl-ssh")))
        result = tool(&p->tools, "rsync", 0) || tool(&p->tools, "ssh", 0);
    else if (!result && strcmp(transport, "local"))
        result = -1;
    json_free(doc);
    return result;
}

/**
 * Say whether the build cleaned the in-house tree, as its receipt records,
 * since the context stage may run in another process than the build.
 *
 * @return 1 for clean, 0 for retained, or -1 when the receipt says neither.
 */
static int conditioning(const struct preparation *p)
{
    struct json *doc = NULL;
    if (routine_json(p->receipt, &doc))
        return -1;
    const char *state = json_text(json_member(doc, "conditioning"), "inhouse");
    int clean = !state ? -1 : !strcmp(state, "clean") ? 1 : !strcmp(state, "retained") ? 0 : -1;
    json_free(doc);
    return clean;
}

static int compose(struct preparation *p)
{
    const struct routine_input *in = &p->input;
    char hash[65], trace[ROUTINE_PATH], stop[ROUTINE_PATH], method[512];
    int clean = conditioning(p);
    if (clean < 0 || source(p, hash) || import_tools(p) ||
        routine_path(trace, in->out, "outer-load.jsonl") ||
        routine_path(stop, in->out, "outer-stop.request"))
        return -1;
    int n =
        snprintf(method, sizeof method,
                 "Public preparation profile %s; clean-inhouse=%s; imported=%s. "
                 "Artifact hashes are verified; OS cache remains natural.",
                 in->purpose, clean ? "1" : "0", getenv("ETUDE_PREPARATION_IMPORT") ? "yes" : "");
    if (n < 0 || n >= (int)sizeof method)
        return -1;
    struct hp_words args = {0};
    const char *const fixed[] = {
        in->program,
        "prepare",
        "context",
        "--tree",
        in->tree,
        "--commit",
        in->commit,
        "--source-snapshot",
        p->source,
        "--source-sha256",
        hash,
        "--record",
        p->record,
        "--site",
        in->site,
        "--etude",
        in->program,
        "--run-id",
        in->run,
        "--purpose",
        in->purpose,
        "--preparation",
        p->receipt,
        "--outer-trace",
        trace,
        "--outer-stop-request",
        stop,
        "--output",
        p->context,
        "--cache-method",
        method,
        "--ordinary-preparation",
        "Selected candidate production, verification, source capture and context composition "
        "occur inside admission.",
        "--one-time-production",
        "Existing installed toolchains and pinned download caches are retained; candidate "
        "artifact production is ordinary preparation."};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    for (size_t i = 0; i < p->tools.n; i++)
        hp_add(&args, p->tools.v[i]);
    int result = context_proofs(&args) ? -1 : command(p, &args);
    hp_words_free(&args);
    return result;
}

/** Run the stage of a routine preparation that `p->stage` names. */
static int prepare_stage(struct preparation *p)
{
    int result = routine_input_read(p->argc, p->argv, &p->input) ||
                         routine_phase_inputs(p->input.purpose, &p->phase) || staged(p) ||
                         preparation_paths(p) || preparation_tools(p) || environment(p)
                     ? 2
                     : 0;
    int first = !result && !strcmp(p->input.purpose, "phase_first_run");
    if (p->stage == STAGE_CONTEXT) {
        receipt_inputs_clear();
        if (!result)
            result = compose(p);
        return result;
    }
    if (!result && first)
        result = allocation(p, 1);
    if (!result)
        result = ensure_import(p);
    if (!result)
        result = build(p);
    if (!result)
        result = producer_export(p);
    if (!result)
        result = reference(p);
    if (!result && !first)
        result = allocation(p, 1);
    if (!result)
        result = allocation(p, 0);
    if (!result && p->stage == STAGE_WHOLE)
        result = compose(p);
    return result;
}

/** Run a stage, free what it kept and report its failure. */
static int prepare_main(int argc, char **argv, volatile sig_atomic_t *stop, enum stage stage)
{
    struct preparation p = {.stage = stage, .stop = stop, .argc = argc, .argv = argv};
    int result = prepare_stage(&p);
    hp_words_free(&p.tools);
    json_free(p.phase);
    if (result)
        fputs("etude prepare: routine preparation failed; original output is retained\n", stderr);
    return result < 0 ? 2 : result;
}

int etude_prepare_routine_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    return prepare_main(argc, argv, stop, STAGE_WHOLE);
}

int etude_prepare_routine_build_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    return prepare_main(argc, argv, stop, STAGE_BUILD);
}

int etude_prepare_routine_context_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    return prepare_main(argc, argv, stop, STAGE_CONTEXT);
}
