/*
 * bootverb.c: bootctl's command line, resolved to a verb and nothing else.
 *
 * Split from the tool so a host test can drive every acceptance and refusal
 * without a boot partition to open. No I/O, no state.
 */

#include <string.h>

#include "bootverb.h"

/* One catalog serves both entry points. Parsing and diagnosis asked the same
 * question of two separate lists before, so a verb added to one and missed in
 * the other would compile and then report itself as no verb at all. */
static const struct {
    const char *name;
    enum bootverb verb;
    int operands;
} verbs[] = {
    {"status", BOOTVERB_STATUS, 0}, {"commit", BOOTVERB_COMMIT, 0}, {"kernel", BOOTVERB_KERNEL, 1}};

#define VERB_COUNT ((int)(sizeof(verbs) / sizeof(verbs[0])))

enum bootverb bootverb_parse(int argc, char **argv, const char **out_arg)
{
    int i;

    if (out_arg) {
        *out_arg = 0;
    }
    if (argc < 2 || !argv || !argv[1]) {
        return BOOTVERB_NONE;
    }

    for (i = 0; i < VERB_COUNT; i++) {
        if (strcmp(argv[1], verbs[i].name) != 0) {
            continue;
        }
        if (argc != 2 + verbs[i].operands) {
            return BOOTVERB_NONE;
        }
        if (verbs[i].operands == 1) {
            if (!argv[2]) {
                return BOOTVERB_NONE;
            }
            if (out_arg) {
                *out_arg = argv[2];
            }
        }
        return verbs[i].verb;
    }
    return BOOTVERB_NONE;
}

int bootverb_known(const char *word)
{
    int i;

    if (!word) {
        return 0;
    }
    for (i = 0; i < VERB_COUNT; i++) {
        if (strcmp(word, verbs[i].name) == 0) {
            return 1;
        }
    }
    return 0;
}
