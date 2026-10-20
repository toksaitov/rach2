/*
 * probe_args.h: the values a guest program takes in a native run, for the
 * freestanding probes and the musl programs alike.
 *
 * The test image runs a program with none of these arguments, and the
 * program then uses the ids, ports and paths that image gives it. A native
 * run, on a Rach2 that is already up, gives NAME=VALUE arguments in their
 * place, as tests/native/Readme.md says: `ids=OLD:NEW,...` maps each user or
 * group id the program would use to one that no account and no process of
 * the machine has, `ports=OLD:NEW,...` maps each local port to one that
 * etude-guest reserved, `machine=native` asks the program to leave out the
 * checks its native plan keeps for a machine of QEMU's, and other names are
 * a program's own, given by its case in tests/native/cases. An argument
 * without `=` is a positional one, such as a mode.
 *
 * The program calls `probe_args` once with its argc and argv; the other
 * functions read what it kept. The header needs no C library.
 */
#ifndef PROBE_ARGS_H
#define PROBE_ARGS_H

static int probe_argc;
static char **probe_argv;

/** Keep the program's arguments for the functions below. */
static __inline__ void probe_args(int argc, char **argv)
{
    probe_argc = argc;
    probe_argv = argv;
}

/**
 * The value of the argument `name`=VALUE, or `fallback` when no argument
 * has that name. The first such argument wins.
 */
static __inline__ const char *probe_arg(const char *name, const char *fallback)
{
    int i, k;

    for (i = 1; i < probe_argc; i++) {
        const char *a = probe_argv[i];

        for (k = 0; name[k] != '\0' && a[k] == name[k]; k++) {
        }
        if (name[k] == '\0' && a[k] == '=') {
            return a + k + 1;
        }
    }
    return fallback;
}

/**
 * The `n`th positional argument, counted from 1 and passing over every
 * NAME=VALUE argument, or NULL when there are fewer.
 */
static __inline__ const char *probe_word(int n)
{
    int i, k;

    for (i = 1; i < probe_argc; i++) {
        for (k = 0; probe_argv[i][k] != '\0' && probe_argv[i][k] != '='; k++) {
        }
        if (probe_argv[i][k] == '\0' && --n == 0) {
            return probe_argv[i];
        }
    }
    return 0;
}

/** The number of positional arguments. */
static __inline__ int probe_words(void)
{
    int n = 0;

    while (probe_word(n + 1) != 0) {
        n++;
    }
    return n;
}

/**
 * The decimal number at `*s`, which is advanced past it, or -1 when `*s`
 * has no digit.
 */
static __inline__ long probe_number(const char **s)
{
    long v = 0;

    if (**s < '0' || **s > '9') {
        return -1;
    }
    while (**s >= '0' && **s <= '9') {
        v = v * 10 + (**s - '0');
        (*s)++;
    }
    return v;
}

/**
 * `old` as the map of the argument `name` gives it: the NEW of the pair
 * OLD:NEW whose OLD is `old`, or `old` itself when the argument is absent
 * or has no such pair.
 */
static __inline__ long probe_map(const char *name, long old)
{
    const char *s = probe_arg(name, 0);

    while (s != 0 && *s != '\0') {
        long from = probe_number(&s), to;

        if (from < 0 || *s++ != ':' || (to = probe_number(&s)) < 0) {
            return old;
        }
        if (from == old) {
            return to;
        }
        if (*s == ',') {
            s++;
        }
    }
    return old;
}

/** The user or group id that a native run gives in place of `id`. */
static __inline__ long probe_id(long id)
{
    return probe_map("ids", id);
}

/** The local port that a native run gives in place of `port`. */
static __inline__ long probe_port(long port)
{
    return probe_map("ports", port);
}

/**
 * 1 in a native run, which leaves out the checks of the machine's own
 * counts, init and boot that its plan keeps for QEMU, and 0 otherwise.
 */
static __inline__ int probe_native(void)
{
    const char *m = probe_arg("machine", "");

    return m[0] == 'n' && m[1] == 'a' && m[2] == 't' && m[3] == 'i' && m[4] == 'v' && m[5] == 'e' &&
           m[6] == '\0';
}

/**
 * The path the program was started by, argv[0], when it is absolute, and
 * `fallback` otherwise: etude-guest starts a native case by its absolute
 * path, and the test image's debug shell by the name it was typed as.
 */
static __inline__ const char *probe_self(const char *fallback)
{
    return probe_argc > 0 && probe_argv[0][0] == '/' ? probe_argv[0] : fallback;
}

#endif /* PROBE_ARGS_H */
