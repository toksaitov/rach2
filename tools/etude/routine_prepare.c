/* Ordinary admitted preparation for the existing native sequence owner. */
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

struct preparation {
    struct routine_input input;
    struct json *phase;
    char record[ROUTINE_PATH], receipt[ROUTINE_PATH], selection[ROUTINE_PATH];
    char source[ROUTINE_PATH], context[ROUTINE_PATH];
    struct hp_words tools;
    volatile sig_atomic_t *stop;
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

static int preparation_paths(struct preparation *p)
{
    const struct routine_input *in = &p->input;
    static const char *const fresh[] = {
        "prepare.record", "preparation.receipt.json", "selection.list", "selection.export.json",
        "source.json",    "source.descriptor.json",   "context"};
    char path[ROUTINE_PATH];
    for (size_t i = 0; i < sizeof fresh / sizeof *fresh; i++)
        if (routine_path(path, in->out, fresh[i]) || !absent(path))
            return -1;
    struct stat st;
    if (routine_path(path, in->out, "outer-load.jsonl") || stat(path, &st) ||
        !S_ISREG(st.st_mode) || routine_path(path, in->out, "outer-stop.request") || !absent(path))
        return -1;
    return routine_path(p->record, in->out, "prepare.record") ||
                   routine_path(p->receipt, in->out, "preparation.receipt.json") ||
                   routine_path(p->selection, in->out, "selection.export.json") ||
                   routine_path(p->source, in->out, "source.json") ||
                   routine_path(p->context, in->out, "context")
               ? -1
               : 0;
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
    unsetenv("ETUDE_PREPARATION_RUN_ID");
    unsetenv("ETUDE_PREPARATION_RECEIPT");
    unsetenv("ETUDE_PREPARATION_COMMIT");
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

static int compose(struct preparation *p)
{
    const struct routine_input *in = &p->input;
    char hash[65], trace[ROUTINE_PATH], stop[ROUTINE_PATH], method[512];
    if (source(p, hash) || import_tools(p) || routine_path(trace, in->out, "outer-load.jsonl") ||
        routine_path(stop, in->out, "outer-stop.request"))
        return -1;
    int n = snprintf(method, sizeof method,
                     "Public preparation profile %s; clean-inhouse=%s; imported=%s. "
                     "Artifact hashes are verified; OS cache remains natural.",
                     in->purpose, getenv("ETUDE_PREPARATION_CLEAN") ? "1" : "0",
                     getenv("ETUDE_PREPARATION_IMPORT") ? "yes" : "");
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

int etude_prepare_routine_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    struct preparation p = {.stop = stop};
    int result = routine_input_read(argc, argv, &p.input) ||
                         routine_phase_inputs(p.input.purpose, &p.phase) || preparation_paths(&p) ||
                         preparation_tools(&p) || environment(&p)
                     ? 2
                     : 0;
    if (!result && !strcmp(p.input.purpose, "phase_first_run"))
        result = allocation(&p, 1);
    if (!result)
        result = ensure_import(&p);
    if (!result)
        result = build(&p);
    if (!result)
        result = producer_export(&p);
    if (!result)
        result = reference(&p);
    if (!result && strcmp(p.input.purpose, "phase_first_run"))
        result = allocation(&p, 1);
    if (!result)
        result = allocation(&p, 0);
    if (!result)
        result = compose(&p);
    hp_words_free(&p.tools);
    json_free(p.phase);
    if (result)
        fputs("etude prepare: routine preparation failed; original output is retained\n", stderr);
    return result < 0 ? 2 : result;
}
