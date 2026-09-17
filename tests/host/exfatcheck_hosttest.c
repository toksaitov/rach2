/*
 * exfatcheck_hosttest.c: `mkexfatfixture --check` against every fixture.
 *
 * host-deps:
 *
 * The check is the reader every exFAT write test relies on, and the pair of
 * the kernel's own check, so it is proved before the reader in the kernel
 * exists: every well-formed fixture is accepted, and every malformed one
 * refused with the finding named for its fault. The builder is compiled
 * here on every run.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXDIR "/tmp/rach2_verify"

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("exfatcheck_hosttest: FAIL: %s\n", what);
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

    snprintf(cmd, sizeof(cmd), "%s/mkexfatfixture %s/exfatcheck.img %s", FIXDIR, FIXDIR, opt);
    if (system(cmd) != 0) {
        printf("exfatcheck_hosttest: the builder failed for '%s'\n", opt);
        fails++;
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s/mkexfatfixture --check %s/exfatcheck.img", FIXDIR, FIXDIR);
    p = popen(cmd, "r");
    if (p == NULL) {
        printf("exfatcheck_hosttest: cannot run the check\n");
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
        printf("exfatcheck_hosttest: got exit %d and:\n%s", rc, out);
    }
}

int main(void)
{
    char cmd[256];

    snprintf(cmd, sizeof(cmd), "cc -O2 -o %s/mkexfatfixture tests/host/mkexfatfixture.c", FIXDIR);
    if (system(cmd) != 0) {
        printf("exfatcheck_hosttest: cannot build the fixture builder\n");
        return 1;
    }

    /* Well-formed volumes. */
    row("", 0, "check: OK: ");
    row("--big-upcase", 0, "check: OK: ");
    row("--uncompressed-upcase", 0, "check: OK: ");
    row("--sector-4k", 0, "check: OK: ");
    row("--no-utc", 0, "check: OK: ");
    row("--zoned", 0, "check: OK: ");
    row("--dirty", 0, ", dirty");

    /* Each malformed one breaks one rule and is named for it. */
    row("--bad-main-checksum", 1, "boot: main checksum");
    row("--main-bad-root", 1, "boot: main checksum");
    row("--bad-backup-checksum", 1, "boot: backup checksum");
    row("--regions-disagree", 1, "boot: the backup boot sector differs");
    row("--short-fat", 1, "boot: a FAT shorter than the clusters");
    row("--fat-in-boot", 1, "boot: the FAT region does not fit");
    row("--media-failure", 1, "boot: media failure flagged");
    row("--short-bitmap", 1, "bitmap entry outside the heap or too short");
    row("--bad-upcase-checksum", 1, "up-case checksum");
    row("--truncated-upcase-run", 1, "up-case table expands to");
    row("--second-bitmap", 1, "a second bitmap entry");
    row("--unknown-critical", 1, "unknown primary entry 0x86");
    row("--bad-set-checksum", 1, "checksum-only: /hello.txt");
    row("--junk", 0, "check: OK: ");
    row("--wrong-secondary-count", 1, "set with 1 secondaries");
    row("--wrong-name-length", 1, "name entries with units past NameLength 8");
    row("--false-hash", 1, "name hash");
    row("--vdl-past-dl", 1,
        "valid length past the length, or a directory's short of it: /valid.bin");
    row("--alloc-impossible", 1, "stream without AllocationPossible: /hello.txt");
    row("--chain-out", 1, "out of range: /chained.bin");
    row("--loop", 1, "loop: /chained.bin");
    row("--short-stream", 1, "short chain: /chained.bin");
    row("--reached-free", 1, "cluster free in the bitmap: /hello.txt");
    row("--lost-bit", 1, "lost: 1 cluster(s)");
    row("--surplus", 1, "surplus: /chained.bin (2 clusters)");
    row("--overlap", 1, "cross-link: /hello.txt");

    /* The states a crash leaves under the dirty bit: the check's repairs
       and its faults, each named. */
    row("--dirty-bad-set-checksum", 1, "checksum-only: /hello.txt");
    row("--dirty-orphan", 1, "orphan secondaries: 2 in /");
    row("--dirty-surplus", 1, "surplus: /chained.bin (1 clusters)");
    row("--dirty-lost", 1, "lost: 2 cluster(s)");
    row("--dirty-dup", 1, "duplicate name: /HELLO.txt");
    row("--dirty-critical", 1, "unknown primary entry 0x86 in /dir");
    row("--dirty-overlap", 1, "cross-link: /hello.txt");
    row("--dirty-short-stream", 1, "short chain: /chained.bin");
    row("--dirty-reached-free", 1, "cluster free in the bitmap: /hello.txt");
    row("--dirty-wrong-name-length", 1, "name entries with units past NameLength");
    row("--dirty-loop", 1, "loop: /chained.bin");
    row("--dirty-hash-and-checksum", 1, "name hash");
    row("--dirty-all", 1, "checksum-only: /hello.txt");
    row("--swallow", 1, "an entry of a set that is not an in-use secondary");
    row("--dirty-nofat-orphan", 1, "orphan secondaries: 2 in /Wide Dir");
    row("--dirty-dup-checksum", 1, "duplicate name: /HELLO.txt");
    row("--dirty-label-below", 0, "check: OK: "); /* the reader passes a label over */

    if (fails == 0) {
        printf("exfatcheck_hosttest: OK\n");
        return 0;
    }
    printf("exfatcheck_hosttest: %d failure(s)\n", fails);
    return 1;
}
