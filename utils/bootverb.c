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
    int min_operands;
    int max_operands;
} verbs[] = {{"status", BOOTVERB_STATUS, 0, 0},
             {"commit", BOOTVERB_COMMIT, 0, 0},
             {"cancel", BOOTVERB_CANCEL, 0, 0},
             {"kernel", BOOTVERB_KERNEL, 1, 1},
             {"config", BOOTVERB_CONFIG, 1, 1},
             {"grub", BOOTVERB_GRUB, 1, 1},
             {"trial", BOOTVERB_TRIAL, 1, 1},
             {"env", BOOTVERB_ENV, 1, 2}};

#define VERB_COUNT ((int)(sizeof(verbs) / sizeof(verbs[0])))

enum bootverb bootverb_parse(int argc, char **argv, const char **out_arg, const char **out_arg2)
{
    int i;

    if (out_arg) {
        *out_arg = 0;
    }
    if (out_arg2) {
        *out_arg2 = 0;
    }
    if (argc < 2 || !argv || !argv[1]) {
        return BOOTVERB_NONE;
    }

    for (i = 0; i < VERB_COUNT; i++) {
        int operands = argc - 2;

        if (strcmp(argv[1], verbs[i].name) != 0) {
            continue;
        }
        if (operands < verbs[i].min_operands || operands > verbs[i].max_operands) {
            return BOOTVERB_NONE;
        }
        if (operands >= 1) {
            if (!argv[2]) {
                return BOOTVERB_NONE;
            }
            if (out_arg) {
                *out_arg = argv[2];
            }
        }
        if (operands >= 2) {
            if (!argv[3]) {
                return BOOTVERB_NONE;
            }
            if (out_arg2) {
                *out_arg2 = argv[3];
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
