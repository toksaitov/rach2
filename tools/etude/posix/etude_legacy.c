/*
 * etude_legacy.c: etude-legacy, which prepares the suites of tests/registry
 * before a suite starts, as tests/run prepared them, checks a preparation's
 * record, runs a prepared suite under runutil's supervision, and runs a set
 * of suites through the machine's ledger as one run of etude.
 */
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "etude/legacy.h"

/* The longest bound of a step or a run, in seconds: a week. */
#define SECONDS_MAX 604800ul

/* The longest idle limit of a suite, in seconds: a day, as tests/run's. */
#define IDLE_MAX 86400ul

/* The longest wait of a set for the machine, in seconds: a day. */
#define WAIT_MAX 86400ul

/* The most suites of a set, and the longest line of its selection. */
#define MEMBERS_MAX  1024
#define LINE_MAX_SET 256

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
    static const int sigs[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = note_stop;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}

/** Print the usage and give the status of a wrong usage. */
static int usage(void)
{
    fprintf(stderr, "usage: etude-legacy prepare [-t TREE] [-a ARCH] [-n KERNEL] [-w WORK] "
                    "[-o RECORD] [-s SECONDS] SUITE...\n"
                    "       etude-legacy check [-d] RECORD\n"
                    "       etude-legacy run [-w WORK] [-i IDLE] [-s SECONDS] RECORD SUITE\n"
                    "       etude-legacy set -l LEDGER -c CPU,MEMORY,DISK -o RESULTS -e EVIDENCE "
                    "-S SCHEMAS [-d LOCKS] [-t TREE] [-a ARCH] [-n KERNEL] [-p RECORD] "
                    "[-W WAIT] [-s SECONDS] [-i IDLE] [-j WIDTH] [-k] [-r STATUS] [-m STAMP] "
                    "[-g COMMIT] [-q ARGS] SELECTION\n");
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

/** Run `prepare` with the arguments after it. */
static int prepare_main(int argc, char **argv)
{
    struct etude_legacy_opts o;
    int opt;

    memset(&o, 0, sizeof(o));
    o.tree = ".";
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.out = stdout;
    o.stop = &stop_signal;
    while ((opt = getopt(argc, argv, "t:a:n:w:o:s:")) != -1) {
        if (opt == 't')
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
            return usage();
    }
    if (optind == argc)
        return usage();
    o.suites = argv + optind;
    o.nsuites = (size_t)(argc - optind);
    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    return etude_legacy_prepare(&o);
}

/** Run `check` with the arguments after it. */
static int check_main(int argc, char **argv)
{
    int digests = 0, opt;

    while ((opt = getopt(argc, argv, "d")) != -1) {
        if (opt != 'd')
            return usage();
        digests = 1;
    }
    if (optind != argc - 1)
        return usage();
    return etude_legacy_check(argv[optind], digests, stdout);
}

/** Run `run` with the arguments after it. */
static int run_main(int argc, char **argv)
{
    struct etude_legacy_run_opts o;
    int opt;

    memset(&o, 0, sizeof(o));
    o.idle = ETUDE_LEGACY_IDLE;
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.out = stdout;
    o.stop = &stop_signal;
    while ((opt = getopt(argc, argv, "w:i:s:")) != -1) {
        if (opt == 'w')
            o.work = optarg;
        else if (opt == 'i' && (o.idle = number_of(optarg, IDLE_MAX)) != 0)
            continue;
        else if (opt == 's' && (o.seconds = number_of(optarg, SECONDS_MAX)) != 0)
            continue;
        else
            return usage();
    }
    if (optind != argc - 2)
        return usage();
    o.record = argv[optind];
    o.suite = argv[optind + 1];
    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    return etude_legacy_run(&o);
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
 * Read the selection at `path`: one suite on each line, `NAME MIB` followed
 * by `serial`, `stops` or both, with `#` beginning a comment line.
 *
 * @return the count of members in `*m`, or -1 after a message.
 */
static long selection_of(const char *path, struct etude_legacy_member *m, char names[][72])
{
    char line[LINE_MAX_SET], *w, *end;
    FILE *f = fopen(path, "r");
    long n = 0, no = 0;

    if (f == NULL) {
        fprintf(stderr, "etude-legacy: %s: %s\n", path, strerror(errno));
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
        if ((w = strtok(NULL, " \t")) == NULL || *w < '0' || *w > '9')
            goto bad;
        m[n].memory_mib = strtoull(w, &end, 10);
        if (*end != '\0' || m[n].memory_mib > (UINT64_C(1) << 40))
            goto bad;
        while ((w = strtok(NULL, " \t")) != NULL) {
            if (strcmp(w, "serial") == 0)
                m[n].serial = 1;
            else if (strcmp(w, "stops") == 0)
                m[n].stops = 1;
            else
                goto bad;
        }
        n++;
    }
    fclose(f);
    return n;
bad:
    fprintf(stderr, "etude-legacy: %s:%ld: not a suite, its memory in MiB, and serial or stops\n",
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
    default:
        return NULL;
    }
}

/** Read the options of `set` into `o`; 0, or the status of a wrong usage. */
static int set_options(int argc, char **argv, struct etude_legacy_set_opts *o)
{
    const char **text;
    int opt, have_cap = 0;

    while ((opt = getopt(argc, argv, "l:c:o:e:S:d:t:a:n:p:W:s:i:j:kr:m:g:q:")) != -1) {
        if ((text = text_option(o, opt)) != NULL)
            *text = optarg;
        else if (opt == 'c' && limits_of(optarg, o->cap) == 0)
            have_cap = 1;
        else if (opt == 'W' && (o->wait = number_of(optarg, WAIT_MAX)) != 0)
            continue;
        else if (opt == 's' && (o->seconds = number_of(optarg, SECONDS_MAX)) != 0)
            continue;
        else if (opt == 'i' && (o->idle = number_of(optarg, IDLE_MAX)) != 0)
            continue;
        else if (opt == 'j' && (o->width = number_of(optarg, MEMBERS_MAX)) != 0)
            continue;
        else if (opt == 'k')
            o->stop_on_fail = 1;
        else
            return usage();
    }
    if (optind != argc - 1 || !have_cap || o->ledger == NULL || o->results == NULL ||
        o->evidence == NULL || o->schemas == NULL)
        return usage();
    return 0;
}

/** Run `set` with the arguments after it; `self` is etude-legacy's path. */
static int set_main(int argc, char **argv, const char *self)
{
    static struct etude_legacy_member members[MEMBERS_MAX];
    static char names[MEMBERS_MAX][72];
    struct etude_legacy_set_opts o;
    char program[PATH_MAX], account[64];
    const char *cpus = getenv("RACH2_CPUS");
    struct passwd *pw = getpwuid(getuid());
    long n;

    memset(&o, 0, sizeof(o));
    o.tree = ".";
    o.arch = "i686";
    o.seconds = ETUDE_LEGACY_SECONDS;
    o.idle = ETUDE_LEGACY_IDLE;
    o.wait = (unsigned)WAIT_MAX;
    o.out = stdout;
    o.stop = &stop_signal;
    if (set_options(argc, argv, &o) != 0)
        return 2;
    if (strchr(self, '/') == NULL || realpath(self, program) == NULL) {
        fprintf(stderr, "etude-legacy: set: run etude-legacy by its path\n");
        return 2;
    }
    o.program = program;
    if ((o.accel = getenv("RACH2_QEMU_ACCEL")) == NULL || *o.accel == '\0') {
        fprintf(stderr, "etude-legacy: set: RACH2_QEMU_ACCEL must name the accelerator\n");
        return 2;
    }
    o.cpus = cpus == NULL || *cpus == '\0' ? 2u : number_of(cpus, 64);
    if (pw != NULL && strlen(pw->pw_name) < sizeof(account))
        snprintf(account, sizeof(account), "%s", pw->pw_name);
    else
        snprintf(account, sizeof(account), "uid%ld", (long)getuid());
    o.account = account;
    if ((n = selection_of(argv[optind], members, names)) < 0)
        return 2;
    o.members = members;
    o.nmembers = (size_t)n;
    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    return etude_legacy_set(&o);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "prepare") == 0)
        return prepare_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "check") == 0)
        return check_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "run") == 0)
        return run_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "set") == 0)
        return set_main(argc - 1, argv + 1, argv[0]);
    return usage();
}
