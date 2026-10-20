/*
 * processor.c: the processor profiles of etude's QEMU machines, and QEMU's
 * `-cpu` value for each.
 */
#include <stdio.h>
#include <string.h>

#include "etude/processor.h"

/* The features a profile may declare, by QEMU's names, which are those of
 * Linux's /proc/cpuinfo. */
static const char *const features[] = {
    "rdrand",  /* CPUID leaf 1, ECX bit 30 */
    "rdseed",  /* leaf 7, EBX bit 18 */
    "xsave",   /* leaf 1, ECX bit 26 */
    "avx",     /* leaf 1, ECX bit 28 */
    "pdpe1gb", /* leaf 0x80000001, EDX bit 26: 1 GiB pages */
    NULL,
};

static const char *const max_features[] = {"rdrand", "rdseed", "xsave", "avx", NULL};
static const char *const no_1g[] = {"pdpe1gb", NULL};
static const char *const none[] = {NULL};

static const struct etude_processor profiles[] = {
    {"max", "max", max_features, none},
    {"max-nopdpe1gb", "max", max_features, no_1g},
    {"qemu64", "qemu64", none, max_features},
};

const struct etude_processor *etude_processor_find(const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < sizeof profiles / sizeof profiles[0]; i++) {
        if (strcmp(profiles[i].name, name) == 0)
            return &profiles[i];
    }
    return NULL;
}

/** Return 1 when `list`, ended by NULL, has `name`. */
static int listed(const char *const *list, const char *name)
{
    for (; *list != NULL; list++) {
        if (strcmp(*list, name) == 0)
            return 1;
    }
    return 0;
}

int etude_feature_known(const char *feature)
{
    return feature != NULL && listed(features, feature);
}

enum etude_feature_state etude_processor_feature(const struct etude_processor *p,
                                                 const char *feature)
{
    if (listed(p->present, feature))
        return ETUDE_FEATURE_PRESENT;
    if (listed(p->absent, feature))
        return ETUDE_FEATURE_ABSENT;
    return ETUDE_FEATURE_UNDECLARED;
}

/** Append `format` with `s` to `buf` at `*used`; return -1 when it does not
 * fit. */
static int append(char *buf, size_t size, size_t *used, const char *format, const char *s)
{
    int n = snprintf(buf + *used, size - *used, format, s);

    if (n < 0 || (size_t)n >= size - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}

int etude_processor_cpu(const struct etude_processor *p, char *buf, size_t size)
{
    const char *const *f;
    size_t used = 0;

    if (size == 0 || append(buf, size, &used, "%s", p->model) != 0)
        return -1;
    for (f = p->present; *f != NULL; f++) {
        if (append(buf, size, &used, ",%s=on", *f) != 0)
            return -1;
    }
    for (f = p->absent; *f != NULL; f++) {
        if (append(buf, size, &used, ",%s=off", *f) != 0)
            return -1;
    }
    if (*p->present != NULL && append(buf, size, &used, "%s", ",enforce=on") != 0)
        return -1;
    return 0;
}
