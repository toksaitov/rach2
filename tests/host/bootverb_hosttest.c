/*
 * bootverb_hosttest.c: bootctl's command line, every line it takes and every
 * line it refuses.
 *
 * host-deps: utils/bootverb.c
 *
 * bootctl rewrites a boot partition, so a line it does not fully understand
 * is an error rather than a guess. Two groups of refusals matter most. A verb
 * given the wrong number of operands fails rather than ignoring the surplus,
 * so that a typo that drops an operand does not install something else. And
 * `--commit`, `--where` and a bare kernel path name no verb and fail, so that
 * no undocumented spelling works and has to be kept.
 */

extern int printf(const char *fmt, ...);

#include <string.h>

#include "../../utils/bootverb.h"

/** The number of failed checks, which is the exit status. */
static int fails;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** The second operand the last `parse` returned, or NULL. */
static const char *arg2_out;

/**
 * Run `bootverb_parse` on the line `bootctl a1 a2 a3`, where a NULL word and
 * the words after it are left off. The first operand goes to `*arg` when
 * `arg` is not NULL, and the second to `arg2_out`.
 */
static enum bootverb parse(char *a1, char *a2, char *a3, const char **arg)
{
    char *argv[4];
    int argc = 1;

    argv[0] = (char *)"bootctl";
    if (a1) {
        argv[argc++] = a1;
    }
    if (a2) {
        argv[argc++] = a2;
    }
    if (a3) {
        argv[argc++] = a3;
    }
    arg2_out = 0;
    return bootverb_parse(argc, argv, arg, &arg2_out);
}

/**
 * Check the verbs, their operand counts, the spellings that are not verbs,
 * and `bootverb_known`. `env` is the one verb whose operand count varies: a
 * key alone reads, a key and a value write, and the caller tells them apart
 * by the second operand. Returns the number of failures, at most 255.
 */
int main(void)
{
    const char *arg;

    CHECK(parse((char *)"status", 0, 0, &arg) == BOOTVERB_STATUS, "status is a verb");
    CHECK(parse((char *)"commit", 0, 0, &arg) == BOOTVERB_COMMIT, "commit is a verb");
    CHECK(parse((char *)"kernel", (char *)"/tmp/k", 0, &arg) == BOOTVERB_KERNEL,
          "kernel with a path is a verb");

    arg = 0;
    (void)parse((char *)"kernel", (char *)"/tmp/k", 0, &arg);
    CHECK(arg != 0 && strcmp(arg, "/tmp/k") == 0,
          "kernel yields the path, not the verb that preceded it");

    arg = (const char *)1;
    (void)parse((char *)"status", 0, 0, &arg);
    CHECK(arg == 0, "a verb with no operand clears the operand out");

    arg = (const char *)1;
    (void)parse((char *)"kernel", 0, 0, &arg);
    CHECK(arg == 0, "a refusal clears the operand instead of leaving it stale");

    CHECK(parse(0, 0, 0, &arg) == BOOTVERB_NONE, "no verb at all is refused");
    CHECK(parse((char *)"kernel", 0, 0, &arg) == BOOTVERB_NONE,
          "kernel without a path is refused, not treated as status");
    CHECK(parse((char *)"kernel", (char *)"/tmp/k", (char *)"extra", &arg) == BOOTVERB_NONE,
          "kernel with a surplus argument is refused, not silently trimmed");

    CHECK(parse((char *)"config", (char *)"/tmp/m", 0, &arg) == BOOTVERB_CONFIG,
          "config with a path is a verb");
    CHECK(parse((char *)"grub", (char *)"/tmp/g", 0, &arg) == BOOTVERB_GRUB,
          "grub with a path is a verb");
    CHECK(parse((char *)"fallback", 0, 0, &arg) == BOOTVERB_FALLBACK, "fallback is a verb");
    CHECK(parse((char *)"trial", (char *)"candidate", 0, &arg) == BOOTVERB_TRIAL,
          "trial with an entry is a verb");
    CHECK(parse((char *)"trial", (char *)"candidate", (char *)"x", &arg) == BOOTVERB_NONE,
          "trial with a surplus argument is refused");
    CHECK(parse((char *)"config", (char *)"/tmp/m", (char *)"x", &arg) == BOOTVERB_NONE,
          "config with a surplus argument is refused");
    {
        char *argv[] = {(char *)"bootctl", (char *)"env", (char *)"a", (char *)"b"};

        CHECK(bootverb_parse(4, argv, 0, 0) == BOOTVERB_ENV,
              "a caller that wants neither operand may pass NULL for both");
    }
    CHECK(parse((char *)"cancel", 0, 0, &arg) == BOOTVERB_CANCEL, "cancel is a verb");

    CHECK(parse((char *)"env", (char *)"next_entry", 0, &arg) == BOOTVERB_ENV,
          "env with a key alone is a verb");
    CHECK(arg2_out == 0, "and yields no value");
    CHECK(parse((char *)"env", (char *)"next_entry", (char *)"candidate", &arg) == BOOTVERB_ENV,
          "env with a key and a value is a verb");
    CHECK(arg2_out != 0 && strcmp(arg2_out, "candidate") == 0, "and yields the value");
    CHECK(parse((char *)"env", 0, 0, &arg) == BOOTVERB_NONE, "env with no key is refused");
    CHECK(parse((char *)"config", 0, 0, &arg) == BOOTVERB_NONE, "config with no path is refused");
    CHECK(parse((char *)"trial", 0, 0, &arg) == BOOTVERB_NONE, "trial with no entry is refused");
    CHECK(parse((char *)"cancel", (char *)"x", 0, &arg) == BOOTVERB_NONE,
          "cancel with a surplus argument is refused");
    CHECK(parse((char *)"status", (char *)"extra", 0, &arg) == BOOTVERB_NONE,
          "status with a surplus argument is refused");
    CHECK(parse((char *)"commit", (char *)"extra", 0, &arg) == BOOTVERB_NONE,
          "commit with a surplus argument is refused");

    CHECK(parse((char *)"--commit", 0, 0, &arg) == BOOTVERB_NONE, "--commit is no longer a verb");
    CHECK(parse((char *)"--where", 0, 0, &arg) == BOOTVERB_NONE, "--where is no longer a verb");
    CHECK(parse((char *)"--status", 0, 0, &arg) == BOOTVERB_NONE, "--status was never a verb");
    CHECK(parse((char *)"/src/rach2/kernel", 0, 0, &arg) == BOOTVERB_NONE,
          "a bare kernel path is refused, which is how the old install spelling dies");

    CHECK(parse((char *)"", 0, 0, &arg) == BOOTVERB_NONE, "an empty argument is not a verb");
    CHECK(parse((char *)"stat", 0, 0, &arg) == BOOTVERB_NONE, "a prefix of a verb is not the verb");
    CHECK(parse((char *)"statuss", 0, 0, &arg) == BOOTVERB_NONE,
          "a verb with a suffix is not the verb");
    CHECK(parse((char *)"STATUS", 0, 0, &arg) == BOOTVERB_NONE, "verbs are not case-folded");
    CHECK(parse((char *)"commit", 0, 0, 0) == BOOTVERB_COMMIT,
          "a caller that wants no operand may pass NULL for it");

    CHECK(bootverb_known("status") && bootverb_known("commit") && bootverb_known("kernel"),
          "every verb is known by name");
    CHECK(!bootverb_known("--commit") && !bootverb_known("statuss") && !bootverb_known(""),
          "a spelling the parser refuses is not a known verb either");
    CHECK(!bootverb_known(0), "a null word is not a verb");
    CHECK(bootverb_known("kernel") && parse((char *)"kernel", 0, 0, &arg) == BOOTVERB_NONE,
          "a known verb can still fail to parse, which is what separates the two diagnostics");

    {
        static char *const names[] = {(char *)"status", (char *)"commit", (char *)"cancel",
                                      (char *)"kernel", (char *)"config", (char *)"trial",
                                      (char *)"env",    (char *)"grub",   (char *)"fallback"};
        unsigned i;

        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            CHECK(bootverb_known(names[i]),
                  "every verb the parser takes is also one the diagnostic knows");
        }
    }

    return fails > 255 ? 255 : fails;
}
