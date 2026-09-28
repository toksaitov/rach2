/**
 * Check the fixture directory of the host tests (compat/fixdir_host.c).
 *
 * host-deps:
 *
 * The helper is included whole. $TMPDIR is taken only when it is an absolute
 * path of at most 100 characters, each one a shell passes through unchanged,
 * since the tests put the fixture paths in their commands unquoted: a
 * semicolon, a space, a redirection, an ampersand, a wildcard, a quote or a
 * dollar sign sends the directory to /tmp, and so do a relative path and one
 * of 101 characters.
 */
#include <stdio.h>
#include <string.h>

#include "compat/fixdir_host.c"

static int failures;

#define CHECK(condition, detail)                             \
    do {                                                     \
        if (!(condition)) {                                  \
            printf("fixdir-hosttest: failed: %s\n", detail); \
            failures++;                                      \
        }                                                    \
    } while (0)

/** The $TMPDIR values the helper takes and the ones it sends to /tmp. */
static void plain_bases(void)
{
    static const char *refused[] = {"/tmp/a;b", "/tmp/a b", "/tmp/a>b",   "/tmp/a&b",
                                    "/tmp/*",   "/tmp/a'b", "/tmp/$HOME", "/tmp/a|b",
                                    "tmp/rel",  "",         "/tmp/a\nb"};
    char longest[FIX_TMP_MAX + 2];
    size_t i;

    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        CHECK(!fix_plain(refused[i]), "a $TMPDIR a shell would change, or relative, is not taken");
    }
    CHECK(!fix_plain(NULL), "no $TMPDIR is not taken");
    CHECK(fix_plain("/var/folders/32/q1rc49295sg4rbt0fb9z0psh0000gn/T/") &&
              fix_plain("/home/user_1/tmp-2.x+y"),
          "a $TMPDIR of letters, digits and / . _ + - is taken");
    memset(longest, 'a', sizeof(longest));
    longest[0] = '/';
    longest[FIX_TMP_MAX] = '\0';
    CHECK(fix_plain(longest), "a $TMPDIR of 100 characters is taken");
    longest[FIX_TMP_MAX] = 'a';
    longest[FIX_TMP_MAX + 1] = '\0';
    CHECK(!fix_plain(longest), "a $TMPDIR of 101 characters is not taken");
}

int main(void)
{
    plain_bases();
    printf("fixdir-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
