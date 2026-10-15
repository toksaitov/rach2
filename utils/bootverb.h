#ifndef BOOTVERB_H
#define BOOTVERB_H

/* bootverb: bootctl's command line, resolved to one verb. */

/** The verbs bootctl accepts, and the one value that means it accepted none. */
enum bootverb {
    BOOTVERB_NONE = 0,
    BOOTVERB_STATUS,
    BOOTVERB_KERNEL,
    BOOTVERB_COMMIT,
    BOOTVERB_CONFIG,
    BOOTVERB_GRUB,
    BOOTVERB_ENV,
    BOOTVERB_TRIAL,
    BOOTVERB_CANCEL,
    BOOTVERB_FALLBACK
};

/**
 * Resolve a command line to one verb, or to `BOOTVERB_NONE`.
 *
 * Argument counts are exact, not minimum: a tool that rewrites a boot
 * partition should not act on a line it half understands, so a surplus
 * operand is an error and not something to ignore. A word that is not a
 * verb's name, such as `--commit` or a kernel's path, names no verb.
 *
 * `argv[0]` is the program name and is not examined. Pure: it reads nothing
 * but the arguments handed to it.
 *
 * `env` is the one verb whose operand count varies: with a key alone it reads,
 * with a key and a value it writes. Both are accepted and the caller tells them
 * apart by whether the second operand is there.
 *
 * @param out_arg  receives the first operand for a verb that takes one, else
 *                 NULL. May itself be NULL if the caller does not want it.
 * @param out_arg2 receives the second operand where there is one, else NULL.
 *                 May itself be NULL.
 */
enum bootverb bootverb_parse(int argc, char **argv, const char **out_arg, const char **out_arg2);

/**
 * Whether `word` names a verb, whatever operands followed it.
 *
 * `bootverb_parse` returns `BOOTVERB_NONE` both for an unknown verb and for
 * a known verb given the wrong number of operands, since neither line can
 * run. They deserve different diagnostics: telling somebody who typed
 * `bootctl kernel` that `kernel` is not a command this tool takes is a worse
 * answer than showing them the usage line. Pure.
 */
int bootverb_known(const char *word);

#endif /* BOOTVERB_H */
