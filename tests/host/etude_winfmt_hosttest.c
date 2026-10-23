/*
 * etude_winfmt_hosttest.c: the command lines, environment order and time
 * conversions of etude's Windows adapter, judged on any host.
 *
 * host-deps: tools/etude/win/winfmt.c
 *
 * A command line is judged by parsing it back with a model of the rules the
 * UCRT follows, from Microsoft's "Parsing C command-line arguments", and by
 * comparing the arguments. The model is a second reading of those rules, so
 * the fixture `arguments` of tools/etude/wincheck.sh, which runs a program
 * that the real UCRT parses, is the check that the model is right.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tools/etude/win/winfmt.h"

static int failures;

/** Report a failed check. */
static void fail(const char *what, const char *detail)
{
    printf("etude_winfmt_hosttest: FAIL: %s: %s\n", what, detail);
    failures++;
}

/**
 * Parse LINE as the UCRT does, after the program's name, into ARGV of at most
 * MAX strings in BUF; return the count, or -1 when the name is not quoted.
 */
static int parse(const char *line, char **argv, int max, char *buf)
{
    const char *p = line;
    char *o = buf;
    int quoted;
    int n = 0;
    size_t slashes;
    size_t i;

    if (*p++ != '"')
        return -1;
    while (*p != '\0' && *p != '"')
        p++;
    if (*p++ != '"')
        return -1;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || n == max)
            return n;
        argv[n++] = o;
        quoted = 0;
        for (;;) {
            slashes = 0;
            while (*p == '\\') {
                slashes++;
                p++;
            }
            if (*p == '"') {
                for (i = 0; i < slashes / 2; i++)
                    *o++ = '\\';
                if (slashes % 2 == 1) {
                    *o++ = '"';
                    p++;
                } else if (quoted && p[1] == '"') {
                    *o++ = '"';
                    p += 2;
                } else {
                    quoted = !quoted;
                    p++;
                }
                continue;
            }
            for (i = 0; i < slashes; i++)
                *o++ = '\\';
            if (*p == '\0' || (!quoted && (*p == ' ' || *p == '\t')))
                break;
            *o++ = *p++;
        }
        *o++ = '\0';
    }
}

/** Quote ARGV after PROGRAM, parse the line back, and compare. */
static void round_trip(const char *what, const char *program, const char *const *argv)
{
    char *got[64];
    char *line;
    char *buf;
    int n;
    int i;

    if (etude_win_cmdline(program, argv, &line) != 0) {
        fail(what, "no command line");
        return;
    }
    buf = malloc(strlen(line) + 1);
    if (buf == NULL) {
        fail(what, "out of memory");
        free(line);
        return;
    }
    n = parse(line, got, 64, buf);
    for (i = 0; argv[i] != NULL; i++) {
        if (i >= n || strcmp(got[i], argv[i]) != 0) {
            fail(what, line);
            break;
        }
    }
    if (argv[i] == NULL && n != i)
        fail(what, line);
    free(buf);
    free(line);
}

/** Every argument that needs quoting, and some that do not. */
static void test_cmdline(void)
{
    static const char *const hard[] = {"plain",
                                       "two words",
                                       "single'quote",
                                       "",
                                       "quote\"in",
                                       "back\\slash",
                                       "trail\\",
                                       "trail space\\",
                                       "\\\\\"",
                                       "tab\there",
                                       "\xc3\xa9 \xd0\xb6",
                                       "a\\\\b\\\"c",
                                       "\"",
                                       " ",
                                       "\\\\server\\share\\",
                                       "\"\"",
                                       "\\",
                                       "\\\\",
                                       "x\"\"y",
                                       NULL};
    static const char *const msys[] = {"single'quote", "*", "?", "[a-z]", NULL};
    static const char *const none[] = {NULL};
    static const char *const plain[] = {"a", "b\\c", "-x=1", NULL};
    char *line;

    round_trip("hard arguments", "C:\\Program Files\\x\\y.exe", hard);
    round_trip("no arguments", "y.exe", none);
    if (etude_win_cmdline("y.exe", msys, &line) != 0 ||
        strcmp(line, "\"y.exe\" \"single'quote\" \"*\" \"?\" \"[a-z]\"") != 0)
        fail("apostrophes and wildcards are quoted for MSYS programs",
             line != NULL ? line : "none");
    free(line);
    if (etude_win_cmdline("C:\\t\\y.exe", plain, &line) != 0 ||
        strcmp(line, "\"C:\\t\\y.exe\" a b\\c -x=1") != 0)
        fail("plain arguments are left as they are", line != NULL ? line : "none");
    free(line);
    if (etude_win_cmdline("a\"b.exe", none, &line) != EINVAL || line != NULL)
        fail("a quote in the program's name", "accepted");
    if (etude_win_cmdline("", none, &line) != EINVAL)
        fail("an empty program", "accepted");
}

/** Arguments drawn from the bytes that matter to the rules, by a fixed seed. */
static void test_cmdline_random(void)
{
    static const char alphabet[] = "ab \t\"\\\\\\\xc3";
    char text[8][24];
    const char *argv[9];
    unsigned long seed = 12345;
    int round;
    int i;
    int j;
    int len;

    for (round = 0; round < 20000; round++) {
        for (i = 0; i < 8; i++) {
            seed = seed * 1103515245u + 12345u;
            len = (int)((seed >> 16) % 23);
            for (j = 0; j < len; j++) {
                seed = seed * 1103515245u + 12345u;
                text[i][j] = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
            }
            text[i][len] = '\0';
            argv[i] = text[i];
        }
        argv[8] = NULL;
        round_trip("random arguments", "p.exe", argv);
        if (failures > 0)
            return;
    }
}

/** Entries sort by name without regard to case, and bad ones are refused. */
static void test_envsort(void)
{
    const char *vars[] = {"zeta=1", "Path=x", "ALPHA=2", "a_b=3", "SystemRoot=C:\\W"};
    const char *bad1[] = {"=C:=C:\\"};
    const char *bad2[] = {"novalue"};
    const char *dup[] = {"Path=1", "PATH=2"};
    const char *prefix[] = {"AB=1", "A=2"};

    if (etude_win_envsort(vars, 5) != 0 || strcmp(vars[0], "ALPHA=2") != 0 ||
        strcmp(vars[1], "a_b=3") != 0 || strcmp(vars[2], "Path=x") != 0 ||
        strcmp(vars[3], "SystemRoot=C:\\W") != 0 || strcmp(vars[4], "zeta=1") != 0)
        fail("environment order", vars[0]);
    if (etude_win_envsort(prefix, 2) != 0 || strcmp(prefix[0], "A=2") != 0)
        fail("a name before the longer name it begins", prefix[0]);
    if (etude_win_envsort(bad1, 1) != EINVAL)
        fail("an entry with no name", "accepted");
    if (etude_win_envsort(bad2, 1) != EINVAL)
        fail("an entry with no =", "accepted");
    if (etude_win_envsort(dup, 2) != EINVAL)
        fail("names that differ in case", "accepted");
}

/** The counter's ticks and FILETIMEs convert without overflow. */
static void test_time(void)
{
    if (etude_win_ticks_us(10000000u * 5u, 10000000u) != 5000000u)
        fail("ticks", "five seconds at 10 MHz");
    if (etude_win_ticks_us(9u, 10000000u) != 0u || etude_win_ticks_us(10u, 10000000u) != 1u)
        fail("ticks", "rounding down");
    if (etude_win_ticks_us(0x7fffffffffffffffull, 10000000u) != 922337203685477580ull)
        fail("ticks", "the largest count at 10 MHz");
    if (etude_win_ticks_us(3000000000ull * 86400u * 365u, 3000000000ull) !=
        86400ull * 365u * 1000000u)
        fail("ticks", "a year at 3 GHz");
    if (etude_win_ticks_us(2999999999ull, 3000000000ull) != 999999u)
        fail("ticks", "just under a second at 3 GHz");
    if (etude_win_filetime_unix_us(ETUDE_WIN_EPOCH_SECONDS * 10000000u) != 0)
        fail("FILETIME", "the Unix epoch");
    if (etude_win_filetime_unix_us(133000000000000000ull) != 1655526400000000ll)
        fail("FILETIME", "2022-06-18");
    if (etude_win_filetime_unix_us(ETUDE_WIN_EPOCH_SECONDS * 10000000u - 10u) != -1)
        fail("FILETIME", "a microsecond before 1970");
}

int main(void)
{
    test_cmdline();
    test_cmdline_random();
    test_envsort();
    test_time();
    if (failures != 0) {
        printf("etude_winfmt_hosttest: %d failures\n", failures);
        return 1;
    }
    printf("etude_winfmt_hosttest: all checks passed\n");
    return 0;
}
