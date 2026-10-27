/*
 * winfmt.c: command lines, environment order and time conversions for etude's
 * Windows adapter, in plain C so that the host tests run them on any host.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "winfmt.h"

/** Whether ARG reaches a program unchanged without quotes. MSYS programs
 * also need quotes around apostrophes and wildcard characters. */
static int plain(const char *arg)
{
    return arg[0] != '\0' && strpbrk(arg, " \t\n\v\"'*?[]") == NULL;
}

/**
 * Write ARG to OUT as the UCRT's rules read it back, and return the bytes
 * written; with OUT null, count them alone.
 *
 * Backslashes are literal unless a quote follows them. So a run of N
 * backslashes before a quote of the argument becomes 2N + 1 backslashes and
 * the quote, and a run before the closing quote becomes 2N; any other run is
 * copied as it is.
 */
static size_t quote(char *out, const char *arg)
{
    size_t len = 0;
    size_t slashes;
    size_t i;
    const char *p;

    if (plain(arg)) {
        len = strlen(arg);
        if (out != NULL)
            memcpy(out, arg, len);
        return len;
    }
    if (out != NULL)
        out[len] = '"';
    len++;
    for (p = arg;; p++) {
        slashes = 0;
        while (*p == '\\') {
            slashes++;
            p++;
        }
        if (*p == '\0' || *p == '"') {
            slashes = slashes * 2 + (*p == '"');
        }
        for (i = 0; i < slashes; i++) {
            if (out != NULL)
                out[len] = '\\';
            len++;
        }
        if (*p == '\0')
            break;
        if (out != NULL)
            out[len] = *p;
        len++;
    }
    if (out != NULL)
        out[len] = '"';
    return len + 1;
}

int etude_win_cmdline(const char *program, const char *const *argv, char **out)
{
    size_t len;
    size_t n;
    size_t i;
    char *line;

    *out = NULL;
    if (program[0] == '\0' || strchr(program, '"') != NULL)
        return EINVAL;
    len = strlen(program) + 2;
    for (i = 0; argv[i] != NULL; i++)
        len += 1 + quote(NULL, argv[i]);
    line = malloc(len + 1);
    if (line == NULL)
        return ENOMEM;
    n = 0;
    line[n++] = '"';
    memcpy(line + n, program, strlen(program));
    n += strlen(program);
    line[n++] = '"';
    for (i = 0; argv[i] != NULL; i++) {
        line[n++] = ' ';
        n += quote(line + n, argv[i]);
    }
    line[n] = '\0';
    *out = line;
    return 0;
}

/** Compare the names of two `NAME=VALUE` entries, folding ASCII case. */
static int namecmp(const char *a, const char *b)
{
    int ca;
    int cb;

    for (;; a++, b++) {
        ca = *a == '=' ? 0 : (unsigned char)*a;
        cb = *b == '=' ? 0 : (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z')
            ca -= 'a' - 'A';
        if (cb >= 'a' && cb <= 'z')
            cb -= 'a' - 'A';
        if (ca != cb || ca == 0)
            return ca - cb;
    }
}

/** qsort's comparison of two entries of an environment. */
static int entrycmp(const void *a, const void *b)
{
    return namecmp(*(const char *const *)a, *(const char *const *)b);
}

int etude_win_envsort(const char **vars, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (vars[i][0] == '=' || strchr(vars[i], '=') == NULL)
            return EINVAL;
    }
    qsort(vars, n, sizeof vars[0], entrycmp);
    for (i = 1; i < n; i++) {
        if (namecmp(vars[i - 1], vars[i]) == 0)
            return EINVAL;
    }
    return 0;
}

uint64_t etude_win_ticks_us(uint64_t ticks, uint64_t freq)
{
    return ticks / freq * 1000000u + ticks % freq * 1000000u / freq;
}

int64_t etude_win_filetime_unix_us(uint64_t filetime)
{
    return (int64_t)(filetime / 10u) - (int64_t)(ETUDE_WIN_EPOCH_SECONDS * 1000000u);
}

uint64_t etude_win_wall_mono_us(int64_t wall_us, uint64_t now_mono_us, int64_t now_wall_us)
{
    uint64_t back;

    if (wall_us >= now_wall_us)
        return now_mono_us;
    back = (uint64_t)now_wall_us - (uint64_t)wall_us;
    return back < now_mono_us ? now_mono_us - back : 0;
}
