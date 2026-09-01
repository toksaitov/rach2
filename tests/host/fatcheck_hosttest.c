/*
 * fatcheck_hosttest.c: `mkfatfixture --check` against every fixture.
 *
 * host-deps:
 *
 * The check is the reader every write test relies on, so it is proved
 * before the writer exists: every malformed fixture must be refused with
 * the finding named for its fault, and every well-formed one accepted.
 * The builder is compiled here on every run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXDIR "/tmp/rach2_verify"

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("fatcheck_hosttest: FAIL: %s\n", what);
    }
}

/* Build the fixture `opt`, run the check on it, and require the exit
   status `want_rc` and the text `want` somewhere in the output. */
static void row(const char *opt, int want_rc, const char *want)
{
    char cmd[512], line[512], out[8192];
    FILE *p;
    int rc;
    size_t n = 0;

    snprintf(cmd, sizeof(cmd), "%s/mkfatfixture %s/fatcheck.img %s", FIXDIR, FIXDIR, opt);
    if (system(cmd) != 0) {
        printf("fatcheck_hosttest: the builder failed for '%s'\n", opt);
        fails++;
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s/mkfatfixture --check %s/fatcheck.img", FIXDIR, FIXDIR);
    p = popen(cmd, "r");
    if (p == NULL) {
        printf("fatcheck_hosttest: cannot run the check\n");
        fails++;
        return;
    }
    out[0] = '\0';
    while (fgets(line, sizeof(line), p) != NULL && n + strlen(line) < sizeof(out) - 1) {
        strcpy(out + n, line);
        n += strlen(line);
    }
    rc = pclose(p);
    rc = rc == -1 ? -1 : (rc >> 8) & 0xff;
    snprintf(cmd, sizeof(cmd), "'%s' exits %d and says '%s'", opt[0] ? opt : "(default)", want_rc,
             want);
    check(rc == want_rc && strstr(out, want) != NULL, cmd);
    if (rc != want_rc || strstr(out, want) == NULL) {
        printf("fatcheck_hosttest: got exit %d and:\n%s", rc, out);
    }
}

int main(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "cc -O2 -o %s/mkfatfixture tests/host/mkfatfixture.c", FIXDIR);
    if (system(cmd) != 0) {
        printf("fatcheck_hosttest: cannot build the fixture builder\n");
        return 1;
    }

    /* Well-formed volumes, in every geometry the builder has. */
    row("", 0, "check: OK: ");
    row("--cluster8", 0, "check: OK: ");
    row("--fragmented", 0, "check: OK: ");
    row("--fsinfo", 0, "check: OK: ");
    row("--unmasked", 0, "check: OK: ");
    row("--active-second", 0, "check: OK: ");
    row("--small-candidate", 0, "check: OK: ");

    /* Each fault named for what it is. */
    row("--loop", 1, "loop: /BOOT/KERNEL");
    row("--out-of-range", 1, "out of range: /BOOT/KERNEL");
    row("--cluster8-past-end", 1, "out of range: /BOOT/KERNEL");
    row("--short-chain", 1, "short chain: /BOOT/KERNEL");
    row("--overlong-chain", 1, "surplus: /BOOT/KERNEL (4 clusters)");
    row("--cross-link", 1, "cross-link: ");
    row("--fats-differ", 1, "mirrors differ: 1 entries");
    row("--lost-chain", 1, "lost: 3 clusters (first 6000)");
    row("--bad-dotdot", 1, "bad ..: /BOOT/GRUB");
    row("--dup-name", 1, "duplicate name: EMPTY in /BOOT/EXTRA");
    row("--fsinfo-wrong", 1, "fsinfo: free count 12345, FAT says ");
    row("--bpb-sector-size", 1, "bpb: sector size");
    row("--bpb-root-out", 1, "bpb: root cluster");
    row("--bpb-fat-oversized", 1, "bpb: ");
    row("--bpb-overflow", 1, "bpb: ");
    row("--vfs", 1, "out of range: /SUB/OUTRANGE.BIN");
    row("--vfs", 1, "short chain: /SUB/SHORTCH.BIN");
    row("--vfs", 1, "orphan long-name entries before /badsum.txt");
    row("--dir-loop", 1, "loop: /SUB");

    if (fails == 0) {
        printf("fatcheck_hosttest: OK\n");
    }
    return fails;
}
