/* Run Doom's freestanding library fixture on each ABI's own kernel. */
#include "host_doom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win/files.h"
#endif

static int format_line(struct etude_case *c, const char *output)
{
    static const char expected[] =
        "doom-libc-format: -9223372036854775808 18446744073709551615 ffffffffffffffff 37\n";
    const char *path = etude_evidence(c, output);
    FILE *f = path ? fopen(path, "rb") : NULL;
    char line[4096];
    int found = 0, ok;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f))
        if (!strcmp(line, expected))
            found = 1;
    ok = found && !ferror(f);
    if (fclose(f))
        ok = 0;
    return ok;
}

static int command(struct etude_case_machine *m, const char *const argv[], const char *output)
{
    struct etude_guest_result r;
    return etude_guest_command(m, argv, NULL, NULL, output, &r) == ETUDE_OK && r.complete &&
           r.termination.end == ETUDE_END_EXITED && r.termination.value == 0 &&
           r.cleanup == ETUDE_CLEANUP_CONFIRMED;
}

static int run(struct etude_case *c, struct etude_case_machine *m, const char *program,
               const char *data, const char *output)
{
    const char *clear[] = {"/bin/busybox", "rm", "-f", data, NULL};
    const char *argv[] = {program, data, NULL};
    char label[80];
    snprintf(label, sizeof(label), "%s.data-before", output);
    int ok = command(m, clear, label);

    if (ok)
        ok = command(m, argv, output) && format_line(c, output);
    snprintf(label, sizeof(label), "%s.data-after", output);
    if (!command(m, clear, label))
        ok = 0;
    fprintf(stderr, "doom-libc-check: %s: %s\n", output, ok ? "pass" : "fail");
    return ok;
}

static int stop_helper(struct etude_case_machine *m)
{
    struct etude_termination stopped;
    char *reply = NULL;
    enum etude_status status = etude_qmp(m, "quit", NULL, &reply);

    free(reply);
    if (status == ETUDE_OK || status == ETUDE_LOST)
        status = etude_wait_end(m, &stopped);
    return status == ETUDE_OK && stopped.end == ETUDE_END_QEMU_EXITED && !stopped.value;
}

int host_doom_check(struct etude_case *c, struct etude_case_machine *products)
{
    const char *binary = etude_artifact(c, "doom-i686");
    const char *remove_program[] = {"/bin/busybox", "rm", "-f", "/tmp/doom-libc", NULL};
    struct etude_case_machine *i686 = NULL;
    struct etude_termination stopped;
    int booted, a, b = 0, cleaned = 0;

    booted = binary && etude_boot(c, "doom-i686", NULL, &i686, &stopped) == ETUDE_OK;
    if (etude_step(c, "doom_libc") != ETUDE_OK) {
        if (booted)
            stop_helper(i686);
        return 0;
    }
    a = run(c, products, "/host-products/doom_libc-x86_64", "/tmp/doom-libc-x86_64.data",
            "doom-libc-x86_64");
    if (booted) {
        if (etude_guest_put(i686, binary, "/tmp/doom-libc", "0755") == ETUDE_OK)
            b = run(c, i686, "/tmp/doom-libc", "/tmp/doom-libc-i686.data", "doom-libc-i686");
        cleaned = command(i686, remove_program, "doom-program-cleanup");
        if (!stop_helper(i686))
            cleaned = 0;
    }
    return a && b && cleaned;
}
