/*
 * etude_proof.c: etude-proof, the driver of mutation proofs on Linux and
 * macOS. It runs a proof's catalogue, or checks the texts of its faults
 * against the tree.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../json.h"
#include "etude/proof.h"

/* The signal that asked the proof to stop, or 0. */
static volatile sig_atomic_t stopped;

/** Note the signal that asks the proof to stop. */
static void on_signal(int sig)
{
    stopped = sig;
}

/** Print the usage and return 2. */
static int usage(void)
{
    fputs("usage: etude-proof [-s SCHEMAS] [-t TREE] [-w WORK] [-b STORE] [-k] PROOF [FAULT...]\n"
          "       etude-proof [-s SCHEMAS] [-t TREE] [-w WORK] [-b STORE] [-k] --base PROOF\n"
          "       etude-proof [-s SCHEMAS] [-t TREE] --texts PROOF...\n",
          stderr);
    return 2;
}

/** Catch the signals that stop a proof, without restarting the calls they
 * interrupt. */
static void catch_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/** Check the texts of each catalogue of `argv`; give the worst status. */
static int texts(const char *schemas, const char *tree, char **argv, int argc)
{
    struct etude_proof *p;
    int i, status = 0, s;

    for (i = 0; i < argc; i++) {
        s = etude_proof_read(argv[i], schemas, stderr, &p);
        if (s == 0) {
            s = etude_proof_texts(p, tree, stderr);
            if (s == 0)
                printf("etude-proof: texts: %s: %zu faults, each edit's text once in its source\n",
                       p->name, p->nfaults);
            etude_proof_free(p);
        }
        if (s > status)
            status = s;
    }
    return status;
}

int main(int argc, char **argv)
{
    struct etude_proof_opts o;
    struct etude_proof *p;
    char tree[PATH_MAX], schemas[PATH_MAX + 32];
    const char *sdir = NULL, *tdir = ".";
    int i, check = 0, status;

    json_program = "etude-proof";
    memset(&o, 0, sizeof(o));
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--texts") == 0)
            check = 1;
        else if (strcmp(argv[i], "--base") == 0)
            o.base = 1;
        else if (strcmp(argv[i], "-k") == 0)
            o.keep = 1;
        else if (i + 1 >= argc)
            return usage();
        else if (strcmp(argv[i], "-s") == 0)
            sdir = argv[++i];
        else if (strcmp(argv[i], "-t") == 0)
            tdir = argv[++i];
        else if (strcmp(argv[i], "-w") == 0)
            o.work = argv[++i];
        else if (strcmp(argv[i], "-b") == 0)
            o.store = argv[++i];
        else
            return usage();
    }
    if (i >= argc || (check && (o.keep || o.work || o.store || o.base)) || (o.base && i + 1 < argc))
        return usage();
    if (realpath(tdir, tree) == NULL) {
        fprintf(stderr, "etude-proof: %s: %s\n", tdir, strerror(errno));
        return 2;
    }
    snprintf(schemas, sizeof schemas, "%s/tools/etude/schemas", tree);
    if (sdir == NULL)
        sdir = schemas;
    if (check)
        return texts(sdir, tree, argv + i, argc - i);
    status = etude_proof_read(argv[i], sdir, stderr, &p);
    if (status != 0)
        return status;
    catch_signals();
    o.tree = tree;
    o.only = i + 1 < argc ? argv + i + 1 : NULL;
    o.log = stdout;
    o.stop = &stopped;
    setvbuf(stdout, NULL, _IOLBF, 0);
    status = etude_proof_run(p, &o);
    etude_proof_free(p);
    return status == ETUDE_PROOF_STOPPED ? 128 + stopped : status;
}
