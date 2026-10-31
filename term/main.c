/*
 * main.c: term's command line and its loop, which owns every dispatch: the
 * child's output, the keys and the framebuffer's events. Task 16 writes the
 * loop.
 */

#include <stdio.h>
#include <string.h>

#include "term.h"

/** Print the usage line to `f`. */
static void usage(FILE *f)
{
    fprintf(f, "usage: term [-l | -H] [PROGRAM [ARG...]]\n"
               "       term -V\n");
}

/**
 * Read the options into `o`.
 *
 * Returns the index of the first operand, -1 after printing a usage error,
 * or -2 when -V or -h has been answered.
 */
static int parse(int argc, char **argv, struct term_options *o)
{
    int i;

    o->login = false;
    o->host = false;
    o->conf = "/etc/term.conf";
    o->argv = NULL;
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "--") == 0)
            return i + 1;
        if (strcmp(argv[i], "-V") == 0) {
            printf("libtsm %s\n", TSM_VERSION);
            return -2;
        } else if (strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return -2;
        } else if (strcmp(argv[i], "-l") == 0) {
            o->login = true;
        } else if (strcmp(argv[i], "-H") == 0) {
            o->host = true;
        } else {
            fprintf(stderr, "term: %s: unknown option\n", argv[i]);
            usage(stderr);
            return -1;
        }
    }
    return i;
}

int main(int argc, char **argv)
{
    struct term_options o;
    struct term_screen s;
    int first;

    first = parse(argc, argv, &o);
    if (first == -2)
        return 0;
    if (first < 0)
        return 2;
    if (o.login && o.host) {
        fprintf(stderr, "term: -l and -H exclude each other\n");
        return 2;
    }
    if (first < argc)
        o.argv = argv + first;
    if (o.host)
        return term_host_run(&o);

    if (term_screen_init(&s, NULL, 80, 25) < 0) {
        fprintf(stderr, "term: screen: out of memory\n");
        return 1;
    }
    term_screen_free(&s);
    fprintf(stderr, "term: not implemented\n");
    return 1;
}
