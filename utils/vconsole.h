/* Console keyboard settings read by init before the first login. */
#ifndef VCONSOLE_H
#define VCONSOLE_H
#include <stdio.h>

#define VCONSOLE_VALUE 256
#define VCONSOLE_MAPS  16

struct vconsole {
    char keymap[VCONSOLE_VALUE];
    char toggle[VCONSOLE_VALUE];
    char options[VCONSOLE_VALUE]; /* Space-separated names of overlay maps. */
    char delay[16];               /* Milliseconds, empty to omit the option. */
    char rate[16];                /* Repeats per second, empty to omit the option. */
    int numlock;                  /* -1 keeps the default; 0 off, 1 on. */
};

/** Set the default US map, with repeat and Num Lock left unchanged. */
void vconsole_defaults(struct vconsole *conf);
/**
 * Read assignments from stream, without taking ownership of it.
 * Return 0 on success, -1 on malformed input or an I/O error. On failure
 * conf holds defaults; no part of a rejected file is applied.
 */
int vconsole_read(FILE *stream, struct vconsole *conf);
/**
 * Apply conf through run(argv, data), which returns 0 only for success.
 * Each argv ends with NULL and is valid only during the call. After a
 * failure try loadkeys -u us and return -1 even if that fallback succeeds.
 */
int vconsole_apply(struct vconsole *conf, int (*run)(char **argv, void *data), void *data);
#endif
