/* family_metal_password.c: real PTYs and nine credential transcript cases. */
#define _POSIX_C_SOURCE 200809L
#include "family_metal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static int count_lines(const char *path)
{
    char *text = fx_read(path), *p;
    int n = 0;
    for (p = text; p && *p; p++)
        n += *p == '\n';
    free(text);
    return n;
}
static int transcripts(struct fixture *f, const char *audit)
{
    static const char *names[] = {"raw", "text", "returned", "errors"};
    char path[8192];
    unsigned i;
    for (i = 0; i < 4; i++) {
        snprintf(path, sizeof(path), "%s/%s", audit, names[i]);
        if (fx_has(path, "Q7pL9xZ2") || fx_has(path, "mK4vT8sN")) {
            fprintf(stderr, "password: credential bytes reached %s\n", path);
            return 0;
        }
    }
    (void)f;
    return 1;
}
/* Require a readable, nonempty normalized transcript. */
static int text_present(const char *path)
{
    FILE *fp = fopen(path, "rb");
    struct stat st;
    int ok;

    if (!fp)
        return 0;
    ok = fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && fgetc(fp) != EOF &&
         !ferror(fp);
    if (fclose(fp) != 0)
        ok = 0;
    return ok;
}
static int check(struct fixture *f, const char *mode, int expected, const char *command,
                 const char *output)
{
    char audit[4096], path[8192];
    const char *a[] = {"/bin/sh", family_path(f, "password/tests/system/accept/su_command.sh"),
                       "key",     "fixture",
                       "22",      "test",
                       "known",   command,
                       NULL};
    int ok;
    snprintf(audit, sizeof(audit), "%s/audit.%s", f->work, mode);
    if (!fx_mkdir(audit))
        return 0;
    fx_env("PTY_SECRET_CASE", mode);
    fx_env("PTY_SECRET_AUDIT", audit);
    snprintf(path, sizeof(path), "%s/reads", audit);
    fx_env("PTYDRIVE_READS", path);
    family_run(f, a, f->work, NULL, NULL);
    snprintf(path, sizeof(path), "%s/returned", audit);
    ok = fx_text(path, f->out);
    snprintf(path, sizeof(path), "%s/errors", audit);
    ok &= fx_text(path, f->err) && f->status == expected && transcripts(f, audit);
    if (!strcmp(mode, "repeat") || !strcmp(mode, "joined")) {
        snprintf(path, sizeof(path), "%s/second", audit);
        ok &= !fx_exists(path);
        snprintf(path, sizeof(path), "%s/raw", audit);
        ok &= fx_has(path, "PROMPTED=2\n") && fx_has(path, "ANSWERED=1\n") && !*f->out;
    } else if (!strcmp(mode, "prefix")) {
        snprintf(path, sizeof(path), "%s/raw", audit);
        ok &= fx_has(path, "[redacted]");
    } else if (!strcmp(mode, "refused")) {
        snprintf(path, sizeof(path), "%s/tries", audit);
        ok &= count_lines(path) == 4 && !*f->out &&
              fx_regex(f->err, "^accept-su: session: .*Connection refused$");
    } else {
        snprintf(path, sizeof(path), "%s/first", audit);
        ok &= fx_equal(path, "one answer\n");
        snprintf(path, sizeof(path), "%s/raw", audit);
        ok &= fx_has(path, "PROMPTED=1\n") && fx_has(path, "ANSWERED=1\n");
        snprintf(path, sizeof(path), "%s/text", audit);
        ok &= text_present(path) && !strcmp(f->out, output);
    }
    fprintf(stderr, "password: %s: %s\n", mode, ok ? "passed" : "failed");
    return ok;
}
int family_metal_password(struct fixture *f)
{
    static const struct {
        const char *mode;
        int status;
        const char *command, *output;
    } rows[] = {{"good", 7, "printf '%s' \"a 'quote' and a dollar \\$\"; exit 7",
                 "a 'quote' and a dollar $"},
                {"echo", 0, "printf \"%s\" \"$PTY_DUMMY_RECEIVED\"", "[redacted]"},
                {"split", 0,
                 "after_read \"$PTY_DUMMY_FIRST\"; printf \"\\r%s\" \"$PTY_DUMMY_LAST\"",
                 "[redacted]"},
                {"repeat", 1,
                 "after_read Pass; printf word:; if IFS= read -r value; then echo answered > "
                 "\"$PTY_SECRET_AUDIT/second\"; fi",
                 ""},
                {"joined", 1, "printf safe", ""},
                {"long", 0, "printf safe", "safe"},
                {"prefix", 1, "printf safe", ""},
                {"reset", 0, "printf safe", "safe"},
                {"refused", 1, "printf safe", ""}};
    unsigned i;
    int ok = fx_copy(family_source(f, "tests/system/accept/su_command.sh"),
                     family_path(f, "password/tests/system/accept/su_command.sh")) &&
             fx_copy("/ptydrive", family_path(f, "password/build/tests/system/ptydrive")) &&
             fx_copy("/host-products/pty-secret", family_path(f, "bin/ssh")) && fx_mock(f, "su") &&
             fx_mock(f, "rm") && fx_mock(f, "after_read");
    if (!ok)
        return 0;
    fx_env("FAMILY_METAL", "password");
    fx_env("RACH2_ACCEPT_PASSWORD", "Q7pL9xZ2mK4vT8sN");
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        ok &= check(f, rows[i].mode, rows[i].status, rows[i].command, rows[i].output);
    family_row(f, "password", ok);
    return 1;
}
