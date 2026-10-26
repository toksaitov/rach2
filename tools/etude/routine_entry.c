/* Select one ordinary native collection and project its final C verdicts. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "routine_entry.h"
#include "host_prepare.h"
#include "authority_internal.h"
#include "routine_proof.h"
#include "measurement_context.h"

struct entry {
    struct routine_input in;
    struct routine_site site;
    struct hp_words selection;
    const char *kind, *purpose;
    int i686, x86_64, status;
    char completion[ROUTINE_PATH], accel[16];
};

static volatile sig_atomic_t entry_stop;
static void entry_signal(int signal_number)
{
    entry_stop = signal_number;
}

static int entry_file(const char *out, const char *name, const char *bytes)
{
    char path[ROUTINE_PATH];
    struct jbuf buffer = {0};
    if (routine_path(path, out, name))
        return -1;
    jbuf_puts(&buffer, bytes);
    int result = routine_create(path, &buffer);
    jbuf_free(&buffer);
    return result;
}

static int entry_refuse(const char *why)
{
    fprintf(stderr, "ci: refused: %s\n", why);
    return 2;
}

static int entry_kind(struct entry *e, int argc, char **argv)
{
    if (argc < 3 || (strcmp(argv[1], "core") && strcmp(argv[1], "full") && strcmp(argv[1], "push")))
        return -1;
    e->kind = argv[1];
    const char *purpose = getenv("ETUDE_PURPOSE");
    e->purpose = !strcmp(e->kind, "push")   ? "push"
                 : purpose && *purpose      ? purpose
                 : !strcmp(e->kind, "full") ? "release"
                                            : "ci";
    return strcmp(e->purpose, "push") && strcmp(e->purpose, "ci") &&
                   (strcmp(e->kind, "full") || strcmp(e->purpose, "release"))
               ? -1
               : 0;
}

static int entry_arches(struct entry *e)
{
    const char *arch = getenv("ARCH");
    if (!arch || !*arch)
        arch = "i686";
    e->i686 = !strcmp(arch, "i686") || !strcmp(arch, "i686 x86_64");
    e->x86_64 = !strcmp(arch, "x86_64") || !strcmp(arch, "i686 x86_64");
    e->in.arches = e->i686 && e->x86_64 ? "both" : e->i686 ? "i686" : "x86_64";
    return e->i686 || e->x86_64 ? 0 : -1;
}

static int entry_inputs(struct entry *e, int argc, char **argv)
{
    if (entry_kind(e, argc, argv) || entry_arches(e))
        return -1;
    char *tree = realpath(".", NULL), *out = realpath(argv[2], NULL);
    char *program = hp_program(argv[0]);
    int error = !tree || !out || !program || strlen(tree) >= sizeof e->in.tree ||
                strlen(out) >= sizeof e->in.out || strlen(program) >= sizeof e->in.program;
    if (!error) {
        strcpy(e->in.tree, tree);
        strcpy(e->in.out, out);
        strcpy(e->in.program, program);
    }
    free(tree);
    free(out);
    free(program);
    return error;
}

static int entry_policy(struct entry *e)
{
    const char *path = getenv("ETUDE_SITE"), *route = getenv("ETUDE_ROUTINE_ROUTE");
    if (!path) {
#ifdef _WIN32
        path = "C:/ProgramData/rach2/etude-site";
#else
        path = "/etc/rach2/etude-site";
#endif
    }
    if (!route || !*route)
        route = NULL;
    if (!authority_absolute(path) || strlen(path) >= sizeof e->in.site ||
        routine_site_read(path, route, &e->site))
        return -1;
    strcpy(e->in.site, path);
    struct authority_file_id supplied, installed;
    if (authority_file_id(e->in.program, &supplied) ||
        authority_file_id(e->site.program, &installed) || supplied.device != installed.device ||
        supplied.inode != installed.inode)
        return -1;
    size_t tree = strlen(e->in.tree), out = strlen(e->in.out);
    if ((!strncmp(e->in.program, e->in.tree, tree) &&
         !strncmp(e->in.program + tree, "/build/", 7)) ||
        (!strncmp(e->in.program, e->in.out, out) && e->in.program[out] == '/'))
        return -1;
    return etude_context_candidate(e->in.tree, e->in.commit);
}

static int entry_accelerator(struct entry *e)
{
    const char *given = getenv("RACH2_QEMU_ACCEL"), *accel = e->accel;
    if (given && *given)
        accel = given;
    else {
        char path[ROUTINE_PATH], line[512], name[64], value[256], extra;
        if (routine_path(path, e->in.tree, "tools/ci/site"))
            return -1;
        FILE *file = fopen(path, "rb");
        if (!file)
            return -1;
        int found = 0, runner = 0, error = 0;
        while (fgets(line, sizeof line, file)) {
            if (*line == '#' || *line == '\n')
                continue;
            if (!strchr(line, '\n') || sscanf(line, "%63s %255s %c", name, value, &extra) != 2) {
                error = 1;
                break;
            }
            if (!strcmp(name, "accelerator")) {
                if (found++ || strlen(value) >= sizeof e->accel) {
                    error = 1;
                    break;
                }
                strcpy(e->accel, value);
            } else if (strcmp(name, "runner") || runner++ ||
                       !strchr("abcdefghijklmnopqrstuvwxyz0123456789", value[0]) ||
                       strspn(value, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(value)) {
                error = 1;
                break;
            }
        }
        error |= ferror(file) || !found || !runner;
        if (fclose(file))
            error = 1;
        if (error)
            return -1;
    }
    e->in.accel = accel;
    return strcmp(accel, "tcg") && strcmp(accel, "kvm") && strcmp(accel, "hvf") &&
                   strcmp(accel, "whpx")
               ? -1
               : 0;
}

static int entry_environment(struct entry *e, char run[256])
{
    if (strcmp(e->kind, "push")) {
        const char *const filters[] = {"RACH2_CASES", "RACH2_AGENTS_ROWS", "RACH2_ACCEPT_AGENTS"};
        for (size_t i = 0; i < sizeof filters / sizeof *filters; i++) {
            const char *value = getenv(filters[i]);
            if (value && *value)
                return -1;
        }
    }
    const char *id = getenv("GITHUB_RUN_ID"), *attempt = getenv("GITHUB_RUN_ATTEMPT");
    int n = snprintf(run, 256, "routine-%s-%s-%s-%ld", e->purpose, id && *id ? id : "local",
                     attempt && *attempt ? attempt : "1", (long)getpid());
    if (n < 0 || n >= 256 || !authority_token(run))
        return -1;
    e->in.run = run;
    e->in.purpose = e->purpose;
    return entry_accelerator(e);
}

static int entry_retention(struct entry *e, const char *action)
{
    const char *policy = getenv("ETUDE_RETENTION");
    if (!policy || !*policy)
        return -1;
    struct hp_words args = {0};
    const char *const prefix[] = {e->in.program, "prepare",  "retention", "--policy", policy,
                                  "--site",      e->in.site, action,      e->in.out};
    for (size_t i = 0; i < sizeof prefix / sizeof *prefix; i++)
        hp_add(&args, prefix[i]);
    if (!strcmp(action, "begin"))
        hp_add(&args, e->in.run);
    int result = routine_command(&args, &entry_stop);
    hp_words_free(&args);
    return result;
}

static int entry_inventory(struct entry *e)
{
    struct hp_words args = {0};
    const char *const words[] = {e->in.program, "run", "-t", e->in.tree, "inventory"};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        hp_add(&args, words[i]);
    int result = routine_query(&e->in, &args, "suites", &entry_stop);
    hp_words_free(&args);
    if (result)
        return result;
    char from[ROUTINE_PATH], to[ROUTINE_PATH];
    return routine_path(from, e->in.out, "suites.stdout") ||
                   routine_path(to, e->in.out, "suites.inventory") || routine_copy(from, to)
               ? -1
               : 0;
}

static int entry_core(struct entry *e)
{
    char path[ROUTINE_PATH];
    if (routine_path(path, e->in.out, "suites.inventory"))
        return -1;
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    char line[512];
    int error = 0;
    while (!error && fgets(line, sizeof line, file)) {
        char *tier = strchr(line, '|'), *arches = tier ? strchr(tier + 1, '|') : NULL;
        error = !tier || !arches || !strchr(arches + 1, '\n');
        if (error)
            break;
        *tier++ = 0;
        *arches++ = 0;
        arches[strcspn(arches, "\r\n")] = 0;
        if ((strcmp(tier, "core") && strcmp(line, "style") && strcmp(line, "host")) ||
            (*arches && strcmp(e->in.arches, "both") && !strstr(arches, e->in.arches)))
            continue;
        hp_add(&e->selection, line);
    }
    error = error || ferror(file) || !e->selection.n;
    if (fclose(file))
        error = 1;
    return error ? -1 : 0;
}

static int entry_selection(struct entry *e, int argc, char **argv)
{
    if (entry_inventory(e))
        return -1;
    if (!strcmp(e->kind, "core"))
        return entry_core(e);
    if (!strcmp(e->kind, "push")) {
        for (int i = 3; i < argc; i++)
            hp_add(&e->selection, argv[i]);
        return e->selection.n ? 0 : -1;
    }
    const char *suites = getenv("SUITES");
    if (!suites || !*suites) {
        hp_add(&e->selection, "all");
        return 0;
    }
    if (hp_split(&e->selection, suites))
        return -1;
    int i686 = 0, x86_64 = 0;
    for (size_t i = 0; i < e->selection.n; i++) {
        const char *word = e->selection.v[i];
        if (!strncmp(word, "i686:", 5) && word[5])
            i686 = 1;
        else if (!strncmp(word, "x86_64:", 7) && word[7])
            x86_64 = 1;
        else
            return -1;
    }
    if (!i686 && !x86_64)
        return -1;
    e->in.arches = i686 && x86_64 ? "both" : i686 ? "i686" : "x86_64";
    return 0;
}

static int entry_architecture(struct entry *e, const char *arch)
{
    char name[128], output[ROUTINE_PATH], commit[64];
    int n = snprintf(name, sizeof name, "%s-%s", e->kind, arch);
    if (n < 0 || n >= (int)sizeof name || routine_path(output, e->in.out, name) ||
        mkdir(output, 0700))
        return -1;
    snprintf(commit, sizeof commit, "%s\n", e->in.commit);
    return entry_file(output, "native", "etude-native-1\n") ||
                   entry_file(output, "commit", commit) || entry_file(output, "failed", "")
               ? -1
               : 0;
}

static int entry_sequence(struct entry *e)
{
    struct hp_words args = {0};
    const struct routine_input *in = &e->in;
    const char *const fixed[] = {in->program, "run",       "routine-sequence", e->purpose,
                                 in->tree,    in->out,     in->arches,         in->run,
                                 in->site,    in->program, in->accel};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    for (size_t i = 0; i < e->selection.n; i++)
        hp_add(&args, e->selection.v[i]);
    e->status = routine_command(&args, &entry_stop);
    hp_words_free(&args);
    char status[32];
    snprintf(status, sizeof status, "%d\n", e->status);
    return entry_file(in->out, "sequence.status", status);
}

static int entry_completion(struct entry *e)
{
    char path[ROUTINE_PATH];
    if (routine_path(path, e->in.out, "routine-completion.path"))
        return -1;
    struct stat st;
    int present = !lstat(path, &st);
    int required = !strcmp(e->site.mode, "wsl-native") || !strcmp(e->site.mode, "windows-import");
    if (!present)
        return required || errno != ENOENT ? -1 : 0;
    if (!S_ISREG(st.st_mode) || st.st_size < 2 || st.st_size >= ROUTINE_PATH)
        return -1;
    FILE *file = fopen(path, "rb");
    size_t size = file ? fread(e->completion, 1, sizeof e->completion - 1, file) : 0;
    int error = !file || ferror(file) || size != (size_t)st.st_size;
    if (file && fclose(file))
        error = 1;
    if (error || e->completion[size - 1] != '\n')
        return -1;
    e->completion[size - 1] = 0;
    if (strchr(e->completion, '\n') || !authority_absolute(e->completion))
        return -1;
    struct hp_words args = {0};
    const char *const fixed[] = {e->in.program, "prepare", "evidence", "pack", e->in.out};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&args, fixed[i]);
    int result = routine_command(&args, &entry_stop);
    hp_words_free(&args);
    return result;
}

static int entry_suite_list(struct entry *e, const char *arch, struct hp_words *suites)
{
    char path[ROUTINE_PATH], line[1024], suite[256], selected[32];
    if (routine_path(path, e->in.out, "selection.list"))
        return -1;
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    int error = 0;
    while (!error && fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n') || sscanf(line, "%255s %31s", suite, selected) != 2) {
            error = 1;
            break;
        }
        if (strcmp(selected, arch) && strcmp(selected, "none"))
            continue;
        size_t i;
        for (i = 0; i < suites->n && strcmp(suites->v[i], suite); i++)
            ;
        if (i == suites->n)
            hp_add(suites, suite);
    }
    error = error || ferror(file);
    if (fclose(file))
        error = 1;
    return error ? -1 : 0;
}

static int entry_failure_words(const char *path, struct hp_words *failures)
{
    FILE *file = fopen(path, "rb");
    char line[2048];
    int error = !file;
    const char *prefix = "etude check: result: failed: ";
    while (!error && fgets(line, sizeof line, file)) {
        if (strncmp(line, prefix, strlen(prefix)))
            continue;
        char *cursor = line + strlen(prefix);
        while (*cursor) {
            cursor += strspn(cursor, " \t\r\n");
            size_t n = strcspn(cursor, " \t\r\n");
            if (!n)
                break;
            char saved = cursor[n];
            cursor[n] = 0;
            if (!authority_token(cursor)) {
                error = 1;
                break;
            }
            hp_add(failures, cursor);
            cursor[n] = saved;
            cursor += n;
        }
    }
    if (file) {
        error = error || ferror(file);
        if (fclose(file))
            error = 1;
    }
    return error ? -1 : 0;
}

static int entry_verdict(struct entry *e, const char *arch, const char *out,
                         const struct hp_words *suites)
{
    struct jbuf names = {0};
    for (size_t i = 0; i < suites->n; i++) {
        if (i)
            jbuf_puts(&names, ",");
        jbuf_puts(&names, suites->v[i]);
    }
    char store[ROUTINE_PATH], history[ROUTINE_PATH], path[ROUTINE_PATH], proofs[ROUTINE_PATH];
    if (routine_path(store, e->in.out, "store") || routine_path(history, e->in.out, "history") ||
        routine_path(path, out, "verdict") || routine_path(proofs, e->in.out, "routine-proofs")) {
        jbuf_free(&names);
        return 2;
    }
    struct hp_words args = {0};
    hp_add(&args, e->in.program);
    hp_add(&args, "check");
    hp_add(&args, "verdict");
    if (!strcmp(e->kind, "push")) {
        char selection[ROUTINE_PATH];
        if (routine_path(selection, e->in.out, "selection.export.json")) {
            hp_words_free(&args);
            jbuf_free(&names);
            return 2;
        }
        hp_add(&args, "--selection");
        hp_add(&args, selection);
    }
    if (*e->completion) {
        hp_add(&args, "--routine-completion");
        hp_add(&args, e->completion);
        hp_add(&args, "--routine-proofs");
        hp_add(&args, proofs);
    }
    const char *const tail[] = {"--project-architecture",
                                "--require-budget",
                                "-b",
                                store,
                                "-c",
                                e->in.commit,
                                arch,
                                e->kind,
                                names.data,
                                history};
    for (size_t i = 0; i < sizeof tail / sizeof *tail; i++)
        hp_add(&args, tail[i]);
    struct hp_context context = {.stop = &entry_stop};
    strcpy(context.tree, e->in.tree);
    int result = hp_run(&context, args.v, path, path);
    hp_words_free(&args);
    jbuf_free(&names);
    return result;
}

static int entry_failure_list(const char *out, const char *arch, const struct hp_words *failures,
                              FILE *combined)
{
    char path[ROUTINE_PATH];
    if (routine_path(path, out, "failed"))
        return -1;
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    int error = 0;
    for (size_t i = 0; !error && i < failures->n; i++) {
        size_t j;
        for (j = 0; j < i && strcmp(failures->v[j], failures->v[i]); j++)
            ;
        if (j != i)
            continue;
        error = fprintf(file, "%s\n", failures->v[i]) < 0 ||
                fprintf(combined, "%s:%s\n", arch, failures->v[i]) < 0;
    }
    if (fclose(file))
        error = 1;
    return error ? -1 : 0;
}

static int entry_skipped(const struct entry *e, const char *arch)
{
    const char *selected = getenv("SUITES");
    if (strcmp(e->kind, "full") || !selected || !*selected)
        return 0;
    for (size_t i = 0; i < e->selection.n; i++)
        if (!strncmp(e->selection.v[i], arch, strlen(arch)) &&
            e->selection.v[i][strlen(arch)] == ':')
            return 0;
    return 1;
}

static int entry_names(struct entry *e, const char *arch, const char *out, struct hp_words *suites)
{
    if (entry_suite_list(e, arch, suites) || !suites->n)
        return -1;
    struct jbuf names = {0};
    for (size_t i = 0; i < suites->n; i++) {
        if (i)
            jbuf_puts(&names, ",");
        jbuf_puts(&names, suites->v[i]);
    }
    jbuf_puts(&names, "\n");
    int result = entry_file(out, "suites", names.data);
    jbuf_free(&names);
    return result;
}

static int entry_project_result(struct entry *e, const char *arch, const char *out,
                                int completion_error, struct hp_words *failures)
{
    struct hp_words suites = {0};
    char verdict[ROUTINE_PATH];
    int status = entry_names(e, arch, out, &suites) || completion_error ? 2 : 0;
    if (!status)
        status = entry_verdict(e, arch, out, &suites);
    else if (entry_file(out, "verdict",
                        completion_error ? "ci: routine completion record is missing or malformed\n"
                                         : "ci: no complete native selection\n"))
        status = 2;
    if (routine_path(verdict, out, "verdict") || entry_failure_words(verdict, failures))
        status = 2;
    if ((status && (status != 1 || !failures->n)) || (!status && e->status > 1))
        hp_add(failures, "runner");
    hp_words_free(&suites);
    return 0;
}

static int entry_project(struct entry *e, const char *arch, int completion_error, FILE *combined)
{
    char name[128], out[ROUTINE_PATH], verdict[ROUTINE_PATH], log[ROUTINE_PATH];
    char target[ROUTINE_PATH];
    snprintf(name, sizeof name, "%s-%s", e->kind, arch);
    if (routine_path(out, e->in.out, name) || routine_path(verdict, out, "verdict") ||
        routine_path(log, e->in.out, "log"))
        return 2;
    if (entry_skipped(e, arch))
        return entry_file(out, "skipped", "") || entry_file(out, "done", "") ? 2 : 0;
    struct hp_words failures = {0};
    entry_project_result(e, arch, out, completion_error, &failures);
    int result = failures.n ? 1 : 0;
    if (entry_failure_list(out, arch, &failures, combined) || routine_path(target, out, "log") ||
        routine_copy(log, target) || routine_path(target, out, "err") ||
        routine_copy(verdict, target) || entry_file(out, "done", ""))
        result = 2;
    hp_words_free(&failures);
    return result;
}

static int entry_start(struct entry *e, int argc, char **argv, char run[256])
{
    char start[ROUTINE_PATH];
    struct stat st;
    const char *retention = getenv("ETUDE_RETENTION");
    if (!retention || !*retention)
        return entry_refuse("selected-site retention policy is absent");
    if (entry_inputs(e, argc, argv) || entry_policy(e) || entry_environment(e, run) ||
        routine_path(start, e->in.out, "start") || !lstat(start, &st) || errno != ENOENT)
        return entry_refuse("invalid, changed or previously used routine inputs");
    if (entry_retention(e, "begin") || entry_file(e->in.out, "start", "") ||
        entry_selection(e, argc, argv) || (e->i686 && entry_architecture(e, "i686")) ||
        (e->x86_64 && entry_architecture(e, "x86_64")))
        return entry_refuse("routine output allocation or selection failed");
    return 0;
}

static int entry_finish(struct entry *e, int error)
{
    int completion_error = entry_completion(e);
    if (completion_error && e->status < 2)
        e->status = 2;
    char failed[ROUTINE_PATH], log[ROUTINE_PATH];
    struct stat st;
    if (routine_path(failed, e->in.out, "failed") || routine_path(log, e->in.out, "log"))
        error = 1;
    if (!error && stat(log, &st) && errno == ENOENT)
        error = entry_file(e->in.out, "log",
                           "ci: native sequence stopped before its output log opened\n");
    FILE *combined = error ? NULL : fopen(failed, "wb");
    int result = combined ? 0 : 2;
    if (combined) {
        int a = e->i686 ? entry_project(e, "i686", completion_error, combined) : 0;
        int b = e->x86_64 ? entry_project(e, "x86_64", completion_error, combined) : 0;
        result = a > b ? a : b;
        if (fclose(combined))
            result = 2;
    }
    if (entry_retention(e, "seal"))
        result = 2;
    return result;
}

int etude_routine_entry_main(int argc, char **argv)
{
    struct entry e = {0};
    char run[256];
    entry_stop = 0;
    signal(SIGTERM, entry_signal);
    signal(SIGINT, entry_signal);
#ifndef _WIN32
    signal(SIGHUP, entry_signal);
#endif
    int result = entry_start(&e, argc, argv, run);
    if (!result) {
        int error = entry_sequence(&e);
        result = entry_stop || e.status >= 128 ? 125 : entry_finish(&e, error);
    }
    hp_words_free(&e.selection);
    return result;
}
