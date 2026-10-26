/*
 * run_suites.c: `etude prepare`, which builds and records the goals that the
 * suites of tests/run read before one starts, `etude check record`, which
 * checks a preparation's record, and `etude run suites`, which runs a set of
 * suites by their cases through the machine's ledger as one run of etude.
 */
#include <errno.h>
#include <limits.h>
#ifndef _WIN32
#include <pwd.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#include "../win/files.h"
#endif

#include "etude/legacy.h"
#include "measurement_context.h"
#include "collection_tools.h"
#include "preparation_import.h"
#include "retention.h"
#include "routine_evidence.h"
#include "routine_producer.h"
#include "routine_route.h"
#include "routine_entry.h"
#include "json.h"
#include "verbs.h"
#include "host_prepare.h"
#ifndef _WIN32
#include "native_prepare.h"
#endif

/* The longest bound of a step or a run, in seconds: a week. */
#define SECONDS_MAX 604800ul

/* The longest wait of a set for the machine, in seconds: a day. */
#define WAIT_MAX 86400ul

/* The most suites of a set, and the longest line of its selection. */
#define MEMBERS_MAX  1024
#define LINE_MAX_SET 256

/* The most goals of a preparation. */
#define GOALS_MAX 4096

/* The stop signal received, or 0. */
static volatile sig_atomic_t stop_signal;

/** Note a stop signal; the preparation then ends the step that runs. */
static void note_stop(int sig)
{
    stop_signal = sig;
}

/** Catch the stop signals without restarting the executor's waits. */
static void catch_stops(void)
{
#ifdef _WIN32
    signal(SIGTERM, note_stop);
    signal(SIGINT, note_stop);
#else
    static const int sigs[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = note_stop;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
#endif
}

/* The usages of the three commands, which `usage` prints. */
static const char prepare_usage[] = "usage: etude prepare [-t TREE] [-a ARCH] [-n KERNEL] "
                                    "[-w WORK] [-o RECORD] [-s SECONDS] -b GOAL [-b GOAL]...\n";
static const char record_usage[] = "usage: etude check record [-d] RECORD\n";
static const char suites_usage[] =
    "usage: etude run suites -l LEDGER -c CPU,MEMORY,DISK -o RESULTS -e EVIDENCE -S SCHEMAS "
    "[-d LOCKS] [-t TREE] [-a ARCH] [-n KERNEL] [-p RECORD] [-W WAIT] [-s SECONDS] [-j WIDTH] "
    "[-k] [-r STATUS] [-m STAMP] [-g COMMIT] [-q ARGS] [-P SITE] SELECTION\n";

/** Print the usage `text` and give the status of a wrong usage. */
static int usage(const char *text)
{
    fputs(text, stderr);
    return 2;
}

/** Read a whole number of seconds from 1 to `max`; 0 when the text is not
 * one. */
static unsigned number_of(const char *s, unsigned long max)
{
    char *end;
    unsigned long v;

    if (*s < '0' || *s > '9')
        return 0;
    v = strtoul(s, &end, 10);
    return *end == '\0' && v >= 1 && v <= max ? (unsigned)v : 0;
}

/* These forms share the existing preparation stop handler. */
static int prepare_native(int argc, char **argv, int *matched)
{
    static const struct {
        const char *name;
        int (*main)(int, char **, volatile sig_atomic_t *);
    } forms[] = {{"route", etude_prepare_route_main},
                 {"routine", etude_prepare_routine_main},
                 {"evidence", etude_prepare_evidence_main},
                 {"producer", etude_routine_producer_main},
                 {"retention", etude_retention_main},
                 {"reference", etude_reference_main},
                 {"import", etude_prepare_import_main},
                 {"export", etude_prepare_export_main},
#ifndef _WIN32
                 {"native", etude_native_prepare_main},
#endif
                 {"-H", etude_host_prepare_main}};
    if (argc < 2)
        return 0;
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        if (!strcmp(argv[1], forms[i].name)) {
            *matched = 1;
            catch_stops();
            return forms[i].main(argc - 1, argv + 1, &stop_signal);
        }
    }
    return 0;
}

static int prepare_form(int argc, char **argv, int *matched)
{
    int status = prepare_native(argc, argv, matched);
    if (*matched || argc < 2)
        return status;
    *matched = 1;
    if (!strcmp(argv[1], "source"))
        return etude_prepare_source_main(argc - 1, argv + 1);
    if (!strcmp(argv[1], "context"))
        return etude_prepare_context_main(argc - 1, argv + 1);
    if (!strcmp(argv[1], "reader"))
        return etude_prepare_reader_main(argc - 1, argv + 1);
    if (!strcmp(argv[1], "tools"))
        return etude_collection_tools_main(argc - 2, argv + 2);
    if (!strcmp(argv[1], "collection")) {
        argv[1] = argv[0];
        return etude_collection_prepare_main(argc - 1, argv + 1);
    }
    *matched = 0;
    return 0;
}

/** Run `etude prepare` with the arguments after it. */
int etude_prepare_main(int argc, char **argv)
{
    static char *goals[GOALS_MAX];
    struct etude_legacy_opts o;
    char program[4096];
    int opt;

    int matched = 0;
    int status = prepare_form(argc, argv, &matched);
    if (matched)
        return status;
    memset(&o, 0, sizeof(o));
    o.tree = ".";
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.out = stdout;
    o.stop = &stop_signal;
    o.goals = goals;
    json_program = "etude prepare";
    while ((opt = getopt(argc, argv, "t:a:n:w:o:s:b:")) != -1) {
        if (opt == 'b' && o.ngoals < GOALS_MAX)
            goals[o.ngoals++] = optarg;
        else if (opt == 't')
            o.tree = optarg;
        else if (opt == 'a')
            o.arch = optarg;
        else if (opt == 'n')
            o.kernel = optarg;
        else if (opt == 'w')
            o.work = optarg;
        else if (opt == 'o')
            o.record = optarg;
        else if (opt == 's' && (o.seconds = number_of(optarg, SECONDS_MAX)) != 0)
            continue;
        else
            return usage(prepare_usage);
    }
    if (optind != argc)
        return usage(prepare_usage);
    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    o.program = realpath(argv[0], program);
    return etude_legacy_prepare(&o);
}

/** Run `etude check record` with the arguments after it. */
int etude_record_main(int argc, char **argv)
{
    int digests = 0, opt;

    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt != 'd')
            return usage(record_usage);
        digests = 1;
    }
    if (optind != argc - 1)
        return usage(record_usage);
    return etude_legacy_check(argv[optind], digests, stdout);
}

/** Read the limits `CPU,MEMORY,DISK` of a ledger into `cap`; 0, or -1. */
static int limits_of(const char *s, uint64_t cap[3])
{
    char *end;
    int i;

    for (i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9')
            return -1;
        errno = 0;
        cap[i] = strtoull(s, &end, 10);
        if (errno != 0 || cap[i] == 0 || *end != (i < 2 ? ',' : '\0'))
            return -1;
        s = end + 1;
    }
    return 0;
}

/**
 * Read the selection at `path`: one suite on each line, its name followed
 * by `serial`, `stops` or both, `arch=ARCH` for a suite that runs on that
 * one of the set's architectures alone, and last `cases=MANIFEST`, the
 * manifest of the cases that run it, with `#` beginning a comment line. The
 * names go to `names` and the manifests to `cases`.
 *
 * @return the count of members in `*m`, or -1 after a message.
 */
static long selection_of(const char *path, struct etude_legacy_member *m, char names[][72],
                         char cases[][LINE_MAX_SET])
{
    char line[LINE_MAX_SET], *w;
    FILE *f = fopen(path, "r");
    long n = 0, no = 0;

    if (f == NULL) {
        fprintf(stderr, "etude run: %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        no++;
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#')
            continue;
        if (n == MEMBERS_MAX || (w = strtok(line, " \t")) == NULL || strlen(w) >= 72)
            goto bad;
        memset(&m[n], 0, sizeof(m[n]));
        snprintf(names[n], 72, "%s", w);
        m[n].suite = names[n];
        while ((w = strtok(NULL, " \t")) != NULL && m[n].cases == NULL) {
            if (strcmp(w, "serial") == 0)
                m[n].serial = 1;
            else if (strcmp(w, "stops") == 0)
                m[n].stops = 1;
            else if (strcmp(w, "arch=i686") == 0 || strcmp(w, "arch=x86_64") == 0)
                m[n].arch = strcmp(w + 5, "i686") == 0 ? "i686" : "x86_64";
            else if (strncmp(w, "cases=", 6) == 0 && w[6] != '\0')
                m[n].cases = strcpy(cases[n], w + 6);
            else
                goto bad;
        }
        if (w != NULL || m[n].cases == NULL)
            goto bad;
        n++;
    }
    fclose(f);
    return n;
bad:
    fprintf(stderr,
            "etude run: %s:%ld: not a suite, serial, stops or arch=ARCH, and cases=MANIFEST\n",
            path, no);
    fclose(f);
    return -1;
}

/** Give the field of `o` that the option `opt` sets to its text, or NULL
 * for an option that is not one of those. */
static const char **text_option(struct etude_legacy_set_opts *o, int opt)
{
    switch (opt) {
    case 'l':
        return &o->ledger;
    case 'o':
        return &o->results;
    case 'e':
        return &o->evidence;
    case 'S':
        return &o->schemas;
    case 'd':
        return &o->locks;
    case 't':
        return &o->tree;
    case 'a':
        return &o->arch;
    case 'n':
        return &o->kernel;
    case 'p':
        return &o->record;
    case 'r':
        return &o->status;
    case 'm':
        return &o->stamp;
    case 'g':
        return &o->commit;
    case 'q':
        return &o->args;
    case 'P':
        return &o->site;
    default:
        return NULL;
    }
}

/** Read the options of `set` into `o`; 0, or the status of a wrong usage. */
static int set_options(int argc, char **argv, struct etude_legacy_set_opts *o)
{
    const char **text;
    int opt, have_cap = 0;

    while ((opt = getopt(argc, argv, "l:c:o:e:S:d:t:a:n:p:W:s:j:kr:m:g:q:P:")) != -1) {
        if ((text = text_option(o, opt)) != NULL)
            *text = optarg;
        else if (opt == 'c' && limits_of(optarg, o->cap) == 0)
            have_cap = 1;
        else if (opt == 'W' && (o->wait = number_of(optarg, WAIT_MAX)) != 0)
            continue;
        else if (opt == 's' && (o->seconds = number_of(optarg, SECONDS_MAX)) != 0)
            continue;
        else if (opt == 'j' && (o->width = number_of(optarg, MEMBERS_MAX)) != 0)
            continue;
        else if (opt == 'k')
            o->stop_on_fail = 1;
        else
            return usage(suites_usage);
    }
    if (optind != argc - 1 || !have_cap || o->ledger == NULL || o->results == NULL ||
        o->evidence == NULL || o->schemas == NULL)
        return usage(suites_usage);
    return 0;
}

/**
 * Run `etude run suites` with the arguments after it; `argv[0]` is the path
 * of etude, which the set runs again for its preparation and its judgements.
 */
int etude_suites_main(int argc, char **argv)
{
    static struct etude_legacy_member members[MEMBERS_MAX];
    static char names[MEMBERS_MAX][72];
    static char cases[MEMBERS_MAX][LINE_MAX_SET];
    struct etude_legacy_set_opts o;
    char program[PATH_MAX], account[64];
    const char *cpus = getenv("RACH2_CPUS");
    const char *self = argv[0], *separator = strrchr(self, '/');
#ifndef _WIN32
    struct passwd *pw = getpwuid(getuid());
#endif
    long n;

    memset(&o, 0, sizeof(o));
    o.tree = ".";
    o.arch = "i686";
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.wait = (unsigned)WAIT_MAX;
    o.out = stdout;
    o.stop = &stop_signal;
    json_program = "etude run";
    if (set_options(argc, argv, &o) != 0)
        return 2;
#ifdef _WIN32
    if (separator == NULL)
        separator = strrchr(self, '\\');
#endif
    if (separator == NULL || realpath(self, program) == NULL) {
        fprintf(stderr, "etude run: run etude by its path\n");
        return 2;
    }
    o.program = program;
    if ((o.accel = getenv("RACH2_QEMU_ACCEL")) == NULL || *o.accel == '\0') {
        fprintf(stderr, "etude run: RACH2_QEMU_ACCEL must name the accelerator\n");
        return 2;
    }
    o.cpus = cpus == NULL || *cpus == '\0' ? 2u : number_of(cpus, 64);
#ifdef _WIN32
    DWORD account_size = sizeof(account);
    if (!GetUserNameA(account, &account_size))
        snprintf(account, sizeof(account), "windows");
#else
    if (pw != NULL && strlen(pw->pw_name) < sizeof(account))
        snprintf(account, sizeof(account), "%s", pw->pw_name);
    else
        snprintf(account, sizeof(account), "uid%ld", (long)getuid());
#endif
    o.account = account;
    if ((n = selection_of(argv[optind], members, names, cases)) < 0)
        return 2;
    o.members = members;
    o.nmembers = (size_t)n;
    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    return etude_legacy_set(&o);
}
