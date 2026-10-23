/*
 * processor.h: the processor profiles of etude's QEMU machines, each a QEMU
 * processor model and the features it declares present or absent, which
 * `etude check` reads to decide a case's branches and the QEMU launcher gives
 * QEMU. tools/etude/Readme.md lists the profiles.
 */
#ifndef ETUDE_PROCESSOR_H
#define ETUDE_PROCESSOR_H

#include <stddef.h>

/* What a profile says of a feature. */
enum etude_feature_state {
    ETUDE_FEATURE_UNDECLARED, /* nothing: the accelerator decides */
    ETUDE_FEATURE_PRESENT,    /* QEMU gives it or does not start */
    ETUDE_FEATURE_ABSENT      /* QEMU turns it off */
};

/* A processor profile, which a manifest's `requirements.qemu.cpu` names. */
struct etude_processor {
    const char *name;
    const char *model;          /* QEMU's processor model */
    const char *const *present; /* features, ended by NULL */
    const char *const *absent;  /* features, ended by NULL */
};

/** Find the profile called `name`, or return NULL for none. */
const struct etude_processor *etude_processor_find(const char *name);

/** Return 1 when `feature` is one a profile can declare, and 0 when not. */
int etude_feature_known(const char *feature);

/** Say what profile `p` declares of `feature`. */
enum etude_feature_state etude_processor_feature(const struct etude_processor *p,
                                                 const char *feature);

/**
 * Write QEMU's `-cpu` value for profile `p` into `buf` of `size` bytes: the
 * model, `F=on` for each present feature, `F=off` for each absent one, and
 * `enforce=on` when any is present, so that an accelerator that cannot give
 * a present feature keeps QEMU from starting.
 *
 * @return 0, or -1 when the value does not fit.
 */
int etude_processor_cpu(const struct etude_processor *p, char *buf, size_t size);

#endif
