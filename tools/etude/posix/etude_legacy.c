/*
 * etude_legacy.c: etude-legacy, which prepares the suites of tests/registry
 * before a suite starts, as tests/run prepared them, checks a preparation's
 * record, and runs a prepared suite under runutil's supervision.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "etude/legacy.h"

/* The longest bound of a step or a run, in seconds: a week. */
#define SECONDS_MAX 604800ul

/* The longest idle limit of a suite, in seconds: a day, as tests/run's. */
#define IDLE_MAX 86400ul

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
                    "       etude-legacy run [-w WORK] [-i IDLE] [-s SECONDS] RECORD SUITE\n");
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

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "prepare") == 0)
        return prepare_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "check") == 0)
        return check_main(argc - 1, argv + 1);
    if (argc >= 2 && strcmp(argv[1], "run") == 0)
        return run_main(argc - 1, argv + 1);
    return usage();
}
