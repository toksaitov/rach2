/*
 * fatcheck_hosttest.c: `mkfatfixture --check` against every fixture.
 *
 * host-deps: tests/host/compat/fixdir_host.c
 *
 * The check is the reader every FAT write test relies on, so it is tested
 * on its own: every malformed fixture must fail it with the finding named
 * for its fault, and every well-formed one must pass. The builder is
 * compiled here on every run, into the test's own fixture directory
 * (compat/fixdir_host.c).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "fixdir_host.h"

static int fails;

/** The builder's command, run with leak detection off. */
static const char *tool(void)
{
    static char text[1200];

    fix_format(text, sizeof(text), "ASAN_OPTIONS=detect_leaks=0 %s", fix_path("mkfatfixture"));
    return text;
}

/** Count and print a failed row. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("fatcheck_hosttest: FAIL: %s\n", what);
    }
}

/**
 * Run the check over the fixture image and require it to exit with
 * `want_rc` and to print `want`; `label` names the row. The output is
 * printed when the row fails.
 */
static void judge(int want_rc, const char *want, const char *label)
{
    char cmd[2400], line[512], out[8192];
    FILE *p;
    int rc;
    size_t n = 0;

    fix_format(cmd, sizeof(cmd), "%s --check %s", tool(), fix_path("fatcheck.img"));
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
    rc = rc != -1 && WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    fix_format(cmd, sizeof(cmd), "'%s' exits %d and says '%s'", label, want_rc, want);
    check(rc == want_rc && strstr(out, want) != NULL, cmd);
    if (rc != want_rc || strstr(out, want) == NULL) {
        printf("fatcheck_hosttest: got exit %d and:\n%s", rc, out);
    }
}

/** Build the fixture the builder's option `opt` names and judge it. */
static void row(const char *opt, int want_rc, const char *want)
{
    char cmd[2400];

    fix_format(cmd, sizeof(cmd), "%s %s %s", tool(), fix_path("fatcheck.img"), opt);
    if (system(cmd) != 0) {
        check(0, "build fixture");
        return;
    }
    judge(want_rc, want, opt[0] ? opt : "(default)");
}

/**
 * Build the volume of 8-sector clusters and write after the root's BOOT
 * entry `count` long-name entries of the letter a, with their checksum,
 * and the short entry BOUND.TXT they belong to. A name of 20 entries ends
 * at 255 units with the terminator and padding. With `restart`, the first
 * entry is a start of its own, sequence 1 with the last-entry flag, and
 * the second starts the name again. The check must exit with `want_rc`:
 * 0 with no finding, or 1 naming orphan long-name entries.
 */
static void long_name_row(unsigned count, int restart, int want_rc, const char *label)
{
    static const unsigned at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    static const unsigned char alias[] = "BOUND   TXT";
    unsigned char entries[23 * 32], sum = 0;
    char cmd[2400];
    unsigned i, k;
    FILE *f;
    int ok;

    fix_format(cmd, sizeof(cmd), "%s %s --cluster8", tool(), fix_path("fatcheck.img"));
    if (system(cmd) != 0) {
        check(0, "build long-name fixture");
        return;
    }
    for (i = 0; i < sizeof(alias) - 1; i++)
        sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + alias[i]);
    memset(entries, 0, sizeof(entries));
    for (i = 0; i < count; i++) {
        unsigned char *e = entries + i * 32;
        unsigned seq = restart && i == 0 ? 1 : count - i;

        e[0] = (unsigned char)(seq | (i == 0 || (restart && i == 1) ? 0x40 : 0));
        e[11] = 0x0f;
        e[13] = sum;
        for (k = 0; k < 13; k++) {
            unsigned unit = (seq - 1) * 13 + k;
            unsigned value = count == 20 && unit >= 255 ? (unit == 255 ? 0 : 0xffff) : 'a';

            e[at[k]] = (unsigned char)value;
            e[at[k] + 1] = (unsigned char)(value >> 8);
        }
    }
    memcpy(entries + count * 32, alias, sizeof(alias) - 1);
    entries[count * 32 + 11] = 0x20;
    f = fopen(fix_path("fatcheck.img"), "r+b");
    if (!f) {
        check(0, "open long-name fixture");
        return;
    }
    ok = fseek(f, 160 * 512 + 96, SEEK_SET) == 0 &&
         fwrite(entries, 1, sizeof(entries), f) == sizeof(entries);
    ok = fclose(f) == 0 && ok;
    check(ok, "write long-name fixture");
    if (ok)
        judge(want_rc,
              want_rc ? "orphan long-name entries before /BOUND.TXT" : "check: OK: ", label);
}

/**
 * Build the fixture builder, then run the rows: each well-formed geometry
 * the builder has, and a volume whose free clusters hold junk, must pass;
 * each fault must fail with the finding named for it; and the long names
 * of 20 entries must pass and those of 21 entries or with a repeated start
 * must fail. Returns the count of failed rows, at most 255.
 */
int main(void)
{
    char cmd[2048];
    const char *cc = getenv("CC");

    fix_format(cmd, sizeof(cmd),
               "%s -std=c99 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined "
               "-fno-sanitize-recover=all -o %s tests/host/mkfatfixture.c",
               cc ? cc : "cc", fix_path("mkfatfixture"));
    if (system(cmd) != 0) {
        printf("fatcheck_hosttest: cannot build the fixture builder\n");
        return 1;
    }

    row("", 0, "check: OK: ");
    row("--cluster8", 0, "check: OK: ");
    row("--fragmented", 0, "check: OK: ");
    row("--fsinfo", 0, "check: OK: ");
    row("--unmasked", 0, "check: OK: ");
    row("--active-second", 0, "check: OK: ");
    row("--small-candidate", 0, "check: OK: ");

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
    row("--junk", 0, "check: OK: ");
    row("--dirty", 1, "dirty: the FAT flag");
    row("--dirty", 1, "dirty: the boot-sector flag");
    row("--dirty-surplus", 1, "surplus: /BOOT/GRUB/GRUB.CFG (2 clusters)");
    row("--dirty-orphan", 1, "orphan long-name entries before the end in /BOOT/EXTRA");
    row("--dirty-mirror", 1, "mirrors differ: 1 entries");
    row("--dirty-cross", 1, "cross-link: ");
    row("--dirty-surplus-cross", 1, "cross-link: ");

    long_name_row(20, 0, 0, "accept twenty-entry name");
    long_name_row(21, 0, 1, "reject twenty-one-entry name");
    long_name_row(21, 1, 1, "reject repeated start before twenty entries");
    long_name_row(2, 1, 1, "reject repeated one-entry start");

    if (fails == 0) {
        printf("fatcheck_hosttest: OK\n");
        fix_passed();
    }
    return fails > 255 ? 255 : fails;
}
