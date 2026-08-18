/*
 * bootverb_hosttest.c: bootctl's command line, every acceptance and refusal.
 *
 * host-deps: utils/bootverb.c
 *
 * This tool rewrites a boot partition on a machine nobody will open again, so
 * a command line it half understands is worth an error rather than a guess.
 * Two groups of refusals matter most. A verb given the wrong number of
 * operands must fail rather than ignore the extra, because a typo that drops
 * an operand should not silently install something else. And the spellings the
 * tool shipped under its previous name must fail rather than work, because a
 * compatibility spelling nobody documents is one that has to be carried
 * forever.
 */

extern int printf(const char *fmt, ...);

#include <string.h>

#include "../../utils/bootverb.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

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
    return bootverb_parse(argc, argv, arg);
}

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
    CHECK(arg == 0, "a refusal clears the operand rather than leaving it stale");

    CHECK(parse(0, 0, 0, &arg) == BOOTVERB_NONE, "no verb at all is refused");
    CHECK(parse((char *)"kernel", 0, 0, &arg) == BOOTVERB_NONE,
          "kernel without a path is refused, not treated as status");
    CHECK(parse((char *)"kernel", (char *)"/tmp/k", (char *)"extra", &arg) == BOOTVERB_NONE,
          "kernel with a surplus argument is refused, not silently trimmed");
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
        static char *const names[] = {(char *)"status", (char *)"commit", (char *)"kernel"};
        int i;

        for (i = 0; i < 3; i++) {
            CHECK(bootverb_known(names[i]) &&
                      parse(names[i], (char *)"/tmp/k", (char *)"/tmp/j", &arg) == BOOTVERB_NONE,
                  "every verb the parser takes is also one the diagnostic knows");
        }
    }

    return fails;
}
