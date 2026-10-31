/*
 * etude.c: etude, the test runner's one command, which runs the verb its
 * first argument names, and the form of a verb that has several. Which verbs
 * a build has depends on its platform: every verb on Linux and macOS, those
 * with a Windows implementation on Windows, `check` and local native runs
 * on Rach2, and `lock`
 * alone where ETUDE_LOCK_ONLY is defined, as the first process of tests/run
 * builds it. Run under the name `make`, as the installed make wrapper is,
 * etude is `etude make`. tools/etude/Readme.md gives each verb's usage.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "verbs.h"
#if !defined(__rach2__) && !defined(ETUDE_LOCK_ONLY)
#include "routine_entry.h"
#endif
#include "authority.h"
#include "etude/args.h"

/* A verb, or a form of one, and the command that runs it. */
struct verb {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *summary;
};

/** Give the number of entries of the array `a`. */
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/**
 * Run the entry of `table` that `argv[1]` names, with the arguments after it
 * and `argv[0]` kept as the path of etude; `what` is the command so far, for
 * the usage. The status of a wrong usage is 2.
 */
static int dispatch(const struct verb *table, size_t n, int argc, char **argv, const char *what)
{
    size_t i;

    for (i = 0; argc >= 2 && i < n; i++) {
        if (strcmp(argv[1], table[i].name) == 0) {
            argv[1] = argv[0];
            return table[i].run(argc - 1, argv + 1);
        }
    }
    if (argc >= 2)
        fprintf(stderr, "%s: %s: no such command\n", what, argv[1]);
    fprintf(stderr, "usage: %s COMMAND [ARGUMENT...]\n", what);
    for (i = 0; i < n; i++)
        fprintf(stderr, "    %-10s %s\n", table[i].name, table[i].summary);
    return 2;
}

#ifndef __rach2__

/** Run `etude lock tree` or `etude lock ports`. */
static int lock_main(int argc, char **argv)
{
    static const struct verb forms[] = {
        {"tree", etude_lock_tree_main, "hold a tree's run lock while a command runs"},
        {"ports", etude_lock_ports_main, "run a command holding the host lock and a port block"},
    };

    return dispatch(forms, COUNT(forms), argc, argv, "etude lock");
}

#endif

#ifndef ETUDE_LOCK_ONLY

/** Run `etude check`, whose form `record` reads a preparation's record. */
static int check_main(int argc, char **argv)
{
#ifndef __rach2__
    if (argc >= 2 && strcmp(argv[1], "record") == 0) {
        argv[1] = argv[0];
        return etude_record_main(argc - 1, argv + 1);
    }
#endif
    return etude_check_main(argc, argv);
}

#ifdef __rach2__

/** Run the local collection, or its list and TinyCC proof forms. */
static int native_main(int argc, char **argv)
{
    char **arguments;
    int status, i;
    if (argc > 1 && (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "proof") == 0))
        return etude_native_collection_main(argc, argv);
    arguments = calloc((size_t)argc + 2, sizeof(*arguments));
    if (arguments == NULL)
        return 2;
    arguments[0] = argv[0];
    arguments[1] = "run";
    for (i = 1; i < argc; i++)
        arguments[i + 1] = argv[i];
    status = etude_native_collection_main(argc + 1, arguments);
    free(arguments);
    return status;
}

static int run_main(int argc, char **argv)
{
    static const struct verb forms[] = {
        {"native", native_main, "run, list or prove the local Rach2 inventory"},
    };
    return dispatch(forms, COUNT(forms), argc, argv, "etude run");
}

#else

/** Run `etude run suites`, `etude run host` or `etude run native`. */
static int run_main(int argc, char **argv)
{
    static const struct verb forms[] = {
        {"routine", etude_routine_entry_main, "prepare and run one installed routine collection"},
        {"routine-sequence", etude_routine_sequence_main, "own routine preparation and collection"},
        {"routine-prepare", etude_routine_prepare_main, "build a routine sequence's preparation"},
        {"routine-collect", etude_routine_collect_main,
         "run the collection of a built preparation"},
        {"suites", etude_suites_main, "run a selection of suites by their cases"},
        {"host", etude_host_main, "run the host checks several at once"},
#ifndef _WIN32
        {"native", etude_native_main, "run one case on a Rach2 machine that is up"},
#endif
    };

    if (argc >= 2 && (strcmp(argv[1], "routine") == 0 || strcmp(argv[1], "routine-sequence") == 0 ||
                      strcmp(argv[1], "routine-prepare") == 0 ||
                      strcmp(argv[1], "routine-collect") == 0 || strcmp(argv[1], "suites") == 0 ||
                      (strcmp(argv[1], "host") == 0 && argc > 2) || strcmp(argv[1], "native") == 0))
        return dispatch(forms, COUNT(forms), argc, argv, "etude run");
    return etude_collection_main(argc, argv);
}

/** Run `etude show lines` or `etude show failures`. */
static int show_main(int argc, char **argv)
{
    static const struct verb forms[] = {
        {"lines", etude_lines_main, "print a run's history as the old runner's lines"},
#ifndef _WIN32
        {"failures", etude_failures_main, "list the attempts of a run that did not pass"},
#endif
    };

    return dispatch(forms, COUNT(forms), argc, argv, "etude show");
}

#ifndef _WIN32

/**
 * Run the script `tools/etude/NAME` of the tree in the current directory
 * with sh and the arguments `argv[1]` on; it does not return unless the
 * script cannot be run.
 */
static int script(const char *name, int argc, char **argv)
{
    char path[64], **args;
    int i;

    snprintf(path, sizeof(path), "tools/etude/%s", name);
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "etude provision: %s: %s; run etude from the root of a Rach2 tree\n", path,
                strerror(errno));
        return 2;
    }
    if ((args = calloc((size_t)argc + 2, sizeof(*args))) == NULL) {
        fputs("etude provision: out of memory\n", stderr);
        return 2;
    }
    args[0] = "sh";
    args[1] = path;
    for (i = 1; i < argc; i++)
        args[i + 1] = argv[i];
    execv("/bin/sh", args);
    fprintf(stderr, "etude provision: /bin/sh: %s\n", strerror(errno));
    free(args);
    return 2;
}

/** Run `etude provision machine`, through provision_machine.sh. */
static int machine_main(int argc, char **argv)
{
    return script("provision_machine.sh", argc, argv);
}

/** Run `etude provision version`, through provision_version.sh. */
static int version_main(int argc, char **argv)
{
    return script("provision_version.sh", argc, argv);
}

#endif

/** Run `etude provision machine`, `version` or `tooling`. */
static int provision_main(int argc, char **argv)
{
    static const struct verb forms[] = {
#ifndef _WIN32
        {"machine", machine_main, "make a machine ready to run etude"},
        {"version", version_main, "build etude into a machine's cache of versions"},
#endif
        {"tooling", etude_tooling_main, "make the tooling machine, or run a job in it"},
    };

    return dispatch(forms, COUNT(forms), argc, argv, "etude provision");
}

#endif /* __rach2__ */
#endif /* ETUDE_LOCK_ONLY */

/* The verbs, in the order the usage lists them. */
static const struct verb verbs[] = {
#ifndef ETUDE_LOCK_ONLY
    {"run", run_main, "run tests: suites, the host checks, or a case on a machine"},
#ifndef __rach2__
    {"prepare", etude_prepare_main, "build what the selected tests need"},
#endif
    {"check", check_main, "check case manifests, results and a run's history"},
#ifndef __rach2__
    {"show", show_main, "print a finished run as lines, or its failed attempts"},
#ifndef _WIN32
    {"compare", cmp_main, "compare a run's times with earlier runs"},
#endif
    {"estimate", etude_budget_main, "forecast a selection's time, or check a run's budget"},
#endif
#endif
#ifndef __rach2__
    {"lock", lock_main, "hold a tree's run lock, or the host lock and a port block"},
#endif
#if !defined(ETUDE_LOCK_ONLY) && !defined(__rach2__)
    {"stress", etude_load_main, "apply a processor, memory or disk load"},
    {"observe", etude_observe_main, "record native system and process load"},
    {"admit", etude_admit_main, "start a command once the ledger admits it"},
    {"launch", etude_launch_main, "admit a build or a test run under the installed site policy"},
    {"make", etude_make_main, "run make within the job count its claim admitted"},
    {"authority", etude_authority_main, "hold the enrolled endpoint through a routine route"},
#ifndef _WIN32
    {"status", etude_status_main, "list the claims in a machine's ledger"},
    {"release", etude_release_main, "drop a claim whose processes have ended"},
    {"serve", etude_serve_main, "respond to remote execution over ssh"},
#endif
    {"provision", provision_main, "make a machine ready, or its tooling machine"},
#ifndef _WIN32
    {"build", etude_build_main, "run one stamped step of the build through the store"},
#endif
#endif
};

#if !defined(ETUDE_LOCK_ONLY) && !defined(__rach2__)
/** Say whether etude runs as `make`, the installed make wrapper. */
static int as_make(const char *path)
{
    const char *name = path + strlen(path);
    while (name > path && name[-1] != '/' && name[-1] != '\\')
        name--;
    return strcmp(name, "make") == 0 || strcmp(name, "make.exe") == 0;
}
#endif

int main(int argc, char **argv)
{
    etude_utf8_args(argc, argv, "etude");
#if !defined(ETUDE_LOCK_ONLY) && !defined(__rach2__)
    if (argc > 0 && as_make(argv[0]))
        return etude_make_main(argc, argv);
#endif
#if defined(__rach2__) && !defined(ETUDE_LOCK_ONLY)
    if (argc > 1 && strcmp(argv[1], "--cut-transport") == 0)
        return etude_native_collection_main(argc, argv);
#endif
#if defined(_WIN32) && !defined(ETUDE_LOCK_ONLY)
    if (argc > 1 && strcmp(argv[1], "--bridge-holder") == 0)
        return etude_win_bridge_main(argc, argv);
#endif
    return dispatch(verbs, COUNT(verbs), argc, argv, "etude");
}
