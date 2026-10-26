/* Construct physical phase vectors for the existing native lifetime owner. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "routine_entry.h"
#include "routine_proof.h"
#include "host_prepare.h"
#include "authority_internal.h"
#include "observe_sequence.h"

struct owner_entry {
    struct routine_input in;
    struct routine_site site;
    struct json *config, *phase, *request;
    const struct json *physical, *producer;
    const char *route;
    char trace[ROUTINE_PATH], stop[ROUTINE_PATH];
};

static const char *field(const struct json *value, const char *name)
{
    const char *text = json_text(value, name);
    return text && *text ? text : NULL;
}

static int setting(const char *name, const char *value)
{
    return value ? setenv(name, value, 1) : -1;
}

static int path_setting(const char *name, const char *root, const char *leaf)
{
    char path[ROUTINE_PATH];
    return routine_path(path, root, leaf) || setenv(name, path, 1) ? -1 : 0;
}

static int wsl_environment(const char *const names[])
{
    struct jbuf value = {0};
    const char *old = getenv("WSLENV");
    char *copy = strdup(old ? old : ""), *save = NULL;
    if (!copy)
        return -1;
    for (char *part = strtok_r(copy, ":", &save); part; part = strtok_r(NULL, ":", &save)) {
        size_t size = strcspn(part, "/"), i;
        for (i = 0; names[i]; i++)
            if (strlen(names[i]) == size && !memcmp(part, names[i], size))
                break;
        if (!names[i]) {
            if (value.len)
                jbuf_puts(&value, ":");
            jbuf_puts(&value, part);
        }
    }
    for (size_t i = 0; names[i]; i++) {
        if (value.len)
            jbuf_puts(&value, ":");
        jbuf_puts(&value, names[i]);
    }
    int error = setenv("WSLENV", value.data, 1);
    jbuf_free(&value);
    free(copy);
    return error;
}

#ifndef _WIN32
static char *path_query(const struct owner_entry *e, const char *program, const char *flag,
                        const char *path, const char *label)
{
    struct hp_words args = {0};
    hp_add(&args, program);
    hp_add(&args, flag);
    hp_add(&args, path);
    int error = routine_query(&e->in, &args, label, NULL);
    hp_words_free(&args);
    char name[128], output[ROUTINE_PATH];
    int n = snprintf(name, sizeof name, "%s.stdout", label);
    if (error || n < 0 || n >= (int)sizeof name || routine_path(output, e->in.out, name))
        return NULL;
    char *answer = hp_read(output);
    if (answer)
        answer[strcspn(answer, "\r\n")] = 0;
    return answer;
}

static int wsl_path(const struct owner_entry *e, const char *path, char native[ROUTINE_PATH],
                    const char *label)
{
    char *program = hp_program("wslpath"), back[128];
    if (!program)
        return -1;
    int n = snprintf(back, sizeof back, "%s-reverse", label);
    char *answer = path_query(e, program, "-w", path, label);
    char *reverse = answer && n >= 0 && n < (int)sizeof back
                        ? path_query(e, program, "-u", answer, back)
                        : NULL;
    struct authority_file_id a, b;
    int error = !answer || !*answer || strlen(answer) >= ROUTINE_PATH || !reverse ||
                authority_file_id(path, &a) || authority_file_id(reverse, &b) ||
                a.device != b.device || a.inode != b.inode;
    if (!error) {
        strcpy(native, answer);
        for (char *at = native; *at; at++)
            if (*at == '\\')
                *at = '/';
    }
    free(answer);
    free(reverse);
    free(program);
    return error ? -1 : 0;
}
#endif

static void arguments(struct hp_words *args, const struct routine_input *in)
{
    const char *const values[] = {in->purpose, in->tree, in->out,     in->arches,
                                  in->run,     in->site, in->program, in->accel};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        hp_add(args, values[i]);
    for (int i = 0; i < in->count; i++)
        hp_add(args, in->selection[i]);
}

static struct json *request_json(const struct routine_input *in, struct json *phase)
{
    struct json *doc = hp_value(JSON_OBJECT), *selection = hp_value(JSON_ARRAY);
    const char *const keys[] = {"purpose", "tree",  "out",   "arches", "run",
                                "site",    "etude", "accel", "commit"};
    const char *const values[] = {in->purpose, in->tree,    in->out,   in->arches, in->run,
                                  in->site,    in->program, in->accel, in->commit};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        hp_member(doc, keys[i], hp_string(values[i]));
    for (int i = 0; i < in->count; i++)
        hp_item(selection, hp_string(in->selection[i]));
    hp_member(doc, "selection", selection);
    hp_member(doc, "preparation", hp_clone(phase));
    return doc;
}

#ifndef _WIN32
static int request_write(struct owner_entry *e)
{
    const struct json *export = json_member(e->phase, "selection_export");
    const char *original = field(export, "path");
    char *selection = original ? strdup(original) : NULL;
    char native[ROUTINE_PATH];
    if (original && !selection)
        return -1;
    if (selection) {
        if (wsl_path(e, selection, native, "native-selection") ||
            setting("ETUDE_PHASE_SELECTION_EXPORT", native)) {
            free(selection);
            return -1;
        }
        hp_member((struct json *)export, "path", hp_string(native));
    }
    struct json *request = request_json(&e->in, e->phase);
    if (selection)
        hp_member(request, "selection_export_linux", hp_string(selection));
    struct jbuf bytes = {0};
    char path[ROUTINE_PATH];
    json_canonical(request, &bytes);
    jbuf_puts(&bytes, "\n");
    int error =
        routine_path(path, e->in.out, "routine.request.json") || routine_create(path, &bytes);
    jbuf_free(&bytes);
    json_free(request);
    free(selection);
    return error ? -1 : 0;
}

static int proxy(struct owner_entry *e)
{
    struct routine_input native = e->in;
    char route[ROUTINE_PATH];
    if (request_write(e) || wsl_path(e, e->in.tree, native.tree, "native-tree") ||
        wsl_path(e, e->in.out, native.out, "native-out") ||
        wsl_path(e, e->in.site, native.site, "native-site") ||
        wsl_path(e, e->route, route, "native-route") || setting("ETUDE_ROUTINE_ROUTE", route) ||
        path_setting("ETUDE_ROUTINE_COMPLETION_LOCATOR", e->in.out, "routine.completion.json"))
        return 2;
    const char *program = field(e->physical, "etude");
    if (!program || strlen(program) >= sizeof native.program)
        return 2;
    strcpy(native.program, program);
    static const char *const names[] = {"ETUDE_ROUTINE_ROUTE",
                                        "ETUDE_ROUTINE_COMMIT",
                                        "ETUDE_ROUTINE_COMPLETION_LOCATOR",
                                        "GITHUB_RUN_ID",
                                        "RUNNER_NAME",
                                        "ETUDE_ROUTINE_INPUT",
                                        "ETUDE_PHASE_SELECTION_EXPORT",
                                        "ETUDE_PREPARATION_CLEAN",
                                        NULL};
    if (wsl_environment(names))
        return 2;
    char *execution = NULL;
    char *wslpath = hp_program("wslpath");
    if (wslpath)
        execution = path_query(e, wslpath, "-u", program, "physical-program");
    free(wslpath);
    if (!execution || *execution != '/' || access(execution, X_OK)) {
        free(execution);
        return 2;
    }
    struct hp_words args = {0};
    const char *const fixed[] = {e->in.program, "proxy", execution, "authority",
                                 "helper",      program, "run",     "routine-sequence"};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    arguments(&args, &native);
    int result = etude_authority_main((int)args.n, args.v);
    hp_words_free(&args);
    free(execution);
    return result;
}
#endif

static int phase_helper(struct owner_entry *e, struct hp_words *args, const char *action)
{
    const struct routine_input *in = &e->in;
    const char *const values[] = {
        in->program, "authority", "helper",   in->program, "prepare",   "route",    action,
        "--config",  e->route,    "--tree",   in->tree,    "--out",     in->out,    "--run",
        in->run,     "--commit",  in->commit, "--purpose", in->purpose, "--arches", in->arches,
        "--site",    in->site,    "--etude",  in->program, "--accel",   in->accel,  "--"};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        hp_add(args, values[i]);
    for (int i = 0; i < in->count; i++)
        hp_add(args, in->selection[i]);
    return 0;
}

static int import_collection(struct owner_entry *e, struct hp_words *args)
{
    const struct routine_input *in = &e->in;
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

static int request_read(struct owner_entry *e)
{
    char path[ROUTINE_PATH];
    if (routine_path(path, e->in.out, "routine.request.json") || routine_json(path, &e->request))
        return -1;
    struct json *expected = request_json(&e->in, e->phase);
    const char *const keys[] = {"purpose", "commit",    "run",        "arches",
                                "accel",   "selection", "preparation"};
    int error = 0;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        const struct json *actual = json_member(e->request, keys[i]);
        const struct json *wanted = json_member(expected, keys[i]);
        if (!actual || !wanted || !json_equal(actual, wanted))
            error = 1;
    }
    const char *const required[] = {"tree", "out", "site", "etude"};
    for (size_t i = 0; i < sizeof required / sizeof *required; i++) {
        const char *value = field(e->request, required[i]);
        if (!value || *value != '/')
            error = 1;
    }
    json_free(expected);
    return error ? -1 : 0;
}

static int environment_path(struct json *env, const char *key, const char *root, const char *name)
{
    char path[ROUTINE_PATH];
    if (routine_path(path, root, name))
        return -1;
    hp_member(env, key, hp_string(path));
    return 0;
}

static struct json *direct_environment(struct owner_entry *e)
{
    const char *out = field(e->request, "out");
    struct json *env = hp_clone(json_member(e->producer, "environment"));
    if (!env || env->type != JSON_OBJECT)
        goto bad;
    hp_member(env, "ETUDE_AUTHORITY_INPUT", hp_string("pipe"));
    hp_member(env, "ETUDE_ROUTINE_DIRECT", hp_string("1"));
    hp_member(env, "ETUDE_ROUTINE_DIRECTORY", hp_string(out));
    hp_member(env, "ETUDE_ROUTINE_RUN", hp_string(e->in.run));
    hp_member(env, "ETUDE_ROUTINE_COMMIT", hp_string(e->in.commit));
    if (environment_path(env, "ETUDE_ROUTINE_GRANT", out, "routine.grant.json") ||
        environment_path(env, "ETUDE_ROUTINE_PROOFS", out, "routine-proofs") ||
        environment_path(env, "ETUDE_PHYSICAL_TRACE", out, "physical-load.jsonl") ||
        environment_path(env, "ETUDE_PHYSICAL_STOP_REQUEST", out, "physical-stop.request"))
        goto bad;
    const char *selection = field(e->request, "selection_export_linux");
    if (field(json_member(e->phase, "selection_export"), "path")) {
        if (!selection || *selection != '/')
            goto bad;
        hp_member(env, "ETUDE_PHASE_SELECTION_EXPORT", hp_string(selection));
    }
    const struct json *clean = json_member(e->phase, "clean");
    if (clean && clean->type == JSON_BOOLEAN && clean->integer)
        hp_member(env, "ETUDE_PREPARATION_CLEAN", hp_string("1"));
    return env;
bad:
    json_free(env);
    return NULL;
}

static int environment_words(struct hp_words *args, const struct json *env)
{
    if (!env || env->type != JSON_OBJECT)
        return -1;
    for (size_t i = 0; i < env->count; i++) {
        const struct json *item = env->item[i];
        const char *value = item->type == JSON_STRING ? item->string : NULL;
        if (!value || !item->name || !*item->name || strchr(item->name, '='))
            return -1;
        struct jbuf word = {0};
        jbuf_puts(&word, item->name);
        jbuf_puts(&word, "=");
        jbuf_puts(&word, value);
        hp_add(args, word.data);
        jbuf_free(&word);
    }
    return 0;
}

static int direct_command(struct owner_entry *e, struct hp_words *args)
{
    if (request_read(e))
        return -1;
    struct json *env = direct_environment(e);
    const char *program = field(e->request, "etude"), *out = field(e->request, "out");
    char owner[ROUTINE_PATH], site[ROUTINE_PATH];
    const char *wsl = field(e->physical, "wsl"), *distribution = field(e->producer, "distribution");
    const char *user = field(e->producer, "user");
    if (!env || !wsl || !distribution || !user || routine_path(owner, out, "direct.owner.json") ||
        routine_path(site, out, "consumer.linux.site")) {
        json_free(env);
        return -1;
    }
    const char *const values[] = {wsl,          "--distribution",
                                  distribution, "--user",
                                  user,         "--exec",
                                  program,      "authority",
                                  "phase",      e->in.run,
                                  e->in.commit, "consumer",
                                  owner,        "/usr/bin/env",
                                  "-u",         "ETUDE_ROUTINE_ROUTE",
                                  "-u",         "ETUDE_ROUTINE_INPUT",
                                  "-u",         "ETUDE_SEQUENCE_PARENT",
                                  "-u",         "ETUDE_PHASE_SELECTION_EXPORT",
                                  "-u",         "ETUDE_PREPARATION_CLEAN"};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        hp_add(args, values[i]);
    int error = environment_words(args, env);
    hp_add(args, program);
    hp_add(args, "run");
    hp_add(args, "routine-sequence");
    struct routine_input linux = e->in;
    const char *tree = field(e->request, "tree");
    if (strlen(tree) >= sizeof linux.tree || strlen(out) >= sizeof linux.out ||
        strlen(program) >= sizeof linux.program)
        error = 1;
    if (!error) {
        strcpy(linux.tree, tree);
        strcpy(linux.out, out);
        strcpy(linux.program, program);
        strcpy(linux.site, site);
        arguments(args, &linux);
    }
    json_free(env);
    static const char *const names[] = {"ETUDE_AUTHORITY_NONCE", "ETUDE_AUTHORITY_RUN", NULL};
    return error || wsl_environment(names) ? -1 : 0;
}

static int counted(struct hp_words *owner, const struct hp_words *command)
{
    if (!command->n || command->n > 65536)
        return -1;
    char size[32];
    snprintf(size, sizeof size, "%zu", command->n);
    hp_add(owner, command->v[0]);
    hp_add(owner, size);
    for (size_t i = 0; i < command->n; i++)
        hp_add(owner, command->v[i]);
    return 0;
}

static int physical_owner(struct owner_entry *e)
{
    const struct routine_input *in = &e->in;
    struct authority_file_id supplied, installed;
    const char *program = field(e->physical, "etude");
    if (!program || authority_file_id(in->program, &supplied) ||
        authority_file_id(program, &installed) || supplied.device != installed.device ||
        supplied.inode != installed.inode || unsetenv("ETUDE_SEQUENCE_PARENT") ||
        path_setting("ETUDE_SEQUENCE_LOG", in->out, "log") || setting("ETUDE_SITE", in->site))
        return 2;
    int direct = !strcmp(e->site.mode, "wsl-native");
    char receipt[ROUTINE_PATH];
    if (routine_path(receipt, in->out, "sequence.receipt") ||
        routine_path(e->trace, in->out, direct ? "physical-load.jsonl" : "outer-load.jsonl") ||
        routine_path(e->stop, in->out, direct ? "physical-stop.request" : "outer-stop.request"))
        return 2;
    struct hp_words quiesce = {0}, prepare = {0}, run = {0}, restore = {0}, args = {0};
    phase_helper(e, &quiesce, "quiesce");
    phase_helper(e, &restore, "restore");
    phase_helper(e, &prepare, direct ? "direct-setup" : "prepare");
    int error = direct ? direct_command(e, &run) : import_collection(e, &run);
    const char *preparation = field(e->physical, "preparation_path");
    const char *runtime = field(e->physical, "runtime_path");
    if (!preparation || !runtime)
        error = 1;
    if (!error) {
        const char *const fixed[] = {in->program, "routine", receipt,     in->run,
                                     e->trace,    e->stop,   in->program, preparation,
                                     runtime,     e->route,  in->out,     in->purpose};
        for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
            hp_add(&args, fixed[i]);
        error = counted(&args, &quiesce) || counted(&args, &prepare) || counted(&args, &run) ||
                counted(&args, &restore);
    }
    int result = error ? 2 : etude_observe_sequence_main((int)args.n, args.v);
    hp_words_free(&quiesce);
    hp_words_free(&prepare);
    hp_words_free(&run);
    hp_words_free(&restore);
    hp_words_free(&args);
    return result;
}

int etude_routine_owner_main(int argc, char **argv)
{
    struct owner_entry e = {0};
    e.route = getenv("ETUDE_ROUTINE_ROUTE");
    if (!e.route || !*e.route || routine_input_read(argc, argv, &e.in) ||
        routine_phase_inputs(e.in.purpose, &e.phase) ||
        routine_site_read(e.in.site, e.route, &e.site) || routine_json(e.route, &e.config) ||
        setting("ETUDE_ROUTINE_COMMIT", e.in.commit))
        goto refused;
    if (!strcmp(e.site.mode, "macos-import")) {
        if (setting("ETUDE_ROUTINE_ENSURE", e.route) || unsetenv("ETUDE_ROUTINE_ROUTE"))
            goto refused;
        int result = etude_routine_sequence_main(argc, argv);
        json_free(e.config);
        json_free(e.phase);
        return result;
    }
    e.physical = json_member(e.config, "physical");
    e.producer = json_member(e.config, "producer");
#ifdef _WIN32
    int result = physical_owner(&e);
#else
    int result = !strcmp(e.site.mode, "wsl-native") ? proxy(&e) : 2;
    (void)physical_owner;
#endif
    json_free(e.config);
    json_free(e.phase);
    json_free(e.request);
    return result;
refused:
    json_free(e.config);
    json_free(e.phase);
    fputs("etude run: invalid or changed routine route inputs\n", stderr);
    return 2;
}
