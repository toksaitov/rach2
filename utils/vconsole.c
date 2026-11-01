/* Read console keyboard assignments and form the tools' argument vectors. */
#include "vconsole.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/** Set defaults without making a request to the console. */
void vconsole_defaults(struct vconsole *conf)
{
    memset(conf, 0, sizeof(*conf));
    strcpy(conf->keymap, "us");
    conf->numlock = -1;
}

/** Remove leading and trailing horizontal white space in place. */
static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t')
        s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        --end;
    *end = 0;
    return s;
}

/** Accept map names, or a space-separated list, with no tool options or
 * paths. */
static int maps_valid(const char *s, int list)
{
    int start = 1, count = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (list && (c == ' ' || c == '\t')) {
            start = 1;
        } else {
            if (start) {
                if (!isalnum(c) || ++count > VCONSOLE_MAPS)
                    return 0;
                start = 0;
            }
            if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c != '+')
                return 0;
        }
    }
    return 1;
}

/** Accept decimal input within inclusive limits, without overflowing. */
static int number_valid(const char *s, unsigned min, unsigned max)
{
    unsigned value = 0;
    if (!*s)
        return 1;
    for (; *s; ++s) {
        unsigned digit = (unsigned char)*s - '0';
        if (digit > 9 || digit > max || value > (max - digit) / 10)
            return 0;
        value = value * 10 + digit;
    }
    return value >= min;
}

/** Parse NUMLOCK's value into `conf`: yes or 1, no or 0, or empty to keep
 * the default; -1 for any other. */
static int numlock(const char *value, struct vconsole *conf)
{
    if (!strcmp(value, "yes") || !strcmp(value, "1"))
        conf->numlock = 1;
    else if (!strcmp(value, "no") || !strcmp(value, "0"))
        conf->numlock = 0;
    else if (!*value)
        conf->numlock = -1;
    else
        return -1;
    return 0;
}

/** Parse one assignment; unknown keys are ignored for file-format
 * compatibility. */
static int assignment(char *line, struct vconsole *conf)
{
    char *key = trim(line), *value, *end, quote;
    char *dest = NULL;
    size_t size = VCONSOLE_VALUE;
    if (!*key || *key == '#' || *key == ';')
        return 0;
    value = strchr(key, '=');
    if (!value)
        return -1;
    *value++ = 0;
    key = trim(key);
    value = trim(value);
    if (*value == '\'' || *value == '"') {
        quote = *value++;
        end = strchr(value, quote);
        if (!end || *trim(end + 1))
            return -1;
        *end = 0;
    }
    if (!strcmp(key, "KEYMAP"))
        dest = conf->keymap;
    else if (!strcmp(key, "KEYMAP_TOGGLE"))
        dest = conf->toggle;
    else if (!strcmp(key, "KEYMAP_OPTIONS"))
        dest = conf->options;
    else if (!strcmp(key, "REPEAT_DELAY")) {
        if (!number_valid(value, 1, 60000))
            return -1;
        dest = conf->delay;
        size = sizeof(conf->delay);
    } else if (!strcmp(key, "REPEAT_RATE")) {
        if (!number_valid(value, 1, 1000))
            return -1;
        dest = conf->rate;
        size = sizeof(conf->rate);
    } else if (!strcmp(key, "NUMLOCK")) {
        return numlock(value, conf);
    } else
        return 0;
    if (dest == conf->keymap || dest == conf->toggle || dest == conf->options) {
        if (!maps_valid(value, dest == conf->options))
            return -1;
    }
    if (strlen(value) >= size)
        return -1;
    strcpy(dest, value);
    return 0;
}

/** Read bounded lines byte by byte so embedded NULs cannot hide assignments. */
int vconsole_read(FILE *stream, struct vconsole *conf)
{
    struct vconsole parsed;
    char line[1024];
    size_t used = 0;
    int c;
    vconsole_defaults(conf);
    vconsole_defaults(&parsed);
    while ((c = fgetc(stream)) != EOF) {
        if (!c || used == sizeof(line) - 1)
            return -1;
        if (c == '\n') {
            line[used] = 0;
            if (assignment(line, &parsed))
                return -1;
            used = 0;
        } else
            line[used++] = (char)c;
    }
    line[used] = 0;
    if (ferror(stream) || assignment(line, &parsed))
        return -1;
    if (!parsed.keymap[0])
        strcpy(parsed.keymap, "us");
    *conf = parsed;
    return 0;
}

/** Apply maps first, then repeat and locks; any failure reloads the US map. */
int vconsole_apply(struct vconsole *conf, int (*run)(char **argv, void *data), void *data)
{
    char options[VCONSOLE_VALUE], *word;
    char *args[VCONSOLE_MAPS + 5];
    char *fallback[] = {"/bin/loadkeys", "-u", "us", NULL};
    int n = 0;
    args[n++] = "/bin/loadkeys";
    args[n++] = "-u";
    args[n++] = conf->keymap;
    if (*conf->toggle)
        args[n++] = conf->toggle;
    strcpy(options, conf->options);
    word = strtok(options, " \t");
    while (word && n < VCONSOLE_MAPS + 4) {
        args[n++] = word;
        word = strtok(NULL, " \t");
    }
    args[n] = NULL;
    if (run(args, data))
        goto fail;
    if (*conf->delay || *conf->rate) {
        n = 0;
        args[n++] = "/bin/kbdrate";
        if (*conf->delay) {
            args[n++] = "-d";
            args[n++] = conf->delay;
        }
        if (*conf->rate) {
            args[n++] = "-r";
            args[n++] = conf->rate;
        }
        args[n] = NULL;
        if (run(args, data))
            goto fail;
    }
    if (conf->numlock >= 0) {
        args[0] = "/bin/setleds";
        args[1] = "-D";
        args[2] = conf->numlock ? "+num" : "-num";
        args[3] = NULL;
        if (run(args, data))
            goto fail;
    }
    return 0;
fail:
    (void)run(fallback, data);
    return -1;
}
