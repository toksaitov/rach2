/*
 * fatcheck_hosttest.c: `mkfatfixture --check` against every fixture.
 *
 * host-deps: tests/host/compat/fixdir_host.c
 *
 * The check is the reader every write test relies on, so it is proved
 * before the writer exists: every malformed fixture must be refused with
 * the finding named for its fault, and every well-formed one accepted.
 * The builder is compiled here on every run, into the test's own fixture
 * directory (compat/fixdir_host.c).
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

    snprintf(text, sizeof(text), "ASAN_OPTIONS=detect_leaks=0 %s", fix_path("mkfatfixture"));
    return text;
}

/** Record an assertion failure. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("fatcheck_hosttest: FAIL: %s\n", what);
    }
}

/** Require the checker to exit normally with the expected finding. */
static void judge(int want_rc, const char *want, const char *label)
{
    char cmd[2400], line[512], out[8192];
    FILE *p;
    int rc;
    size_t n = 0;

    snprintf(cmd, sizeof(cmd), "%s --check %s", tool(), fix_path("fatcheck.img"));
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
    snprintf(cmd, sizeof(cmd), "'%s' exits %d and says '%s'", label, want_rc, want);
    check(rc == want_rc && strstr(out, want) != NULL, cmd);
    if (rc != want_rc || strstr(out, want) == NULL) {
        printf("fatcheck_hosttest: got exit %d and:\n%s", rc, out);
    }
}

/** Build one standard fixture and check its verdict. */
static void row(const char *opt, int want_rc, const char *want)
{
    char cmd[2400];

    snprintf(cmd, sizeof(cmd), "%s %s %s", tool(), fix_path("fatcheck.img"), opt);
    if (system(cmd) != 0) {
        check(0, "build fixture");
        return;
    }
    judge(want_rc, want, opt[0] ? opt : "(default)");
}

/** Write a long-name sequence after the default root's BOOT entry. */
static void long_name_row(unsigned count, int restart, int want_rc, const char *label)
{
    static const unsigned at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    static const unsigned char alias[] = "BOUND   TXT";
    unsigned char entries[23 * 32], sum = 0;
    char cmd[2400];
    unsigned i, k;
    FILE *f;
    int ok;

    snprintf(cmd, sizeof(cmd), "%s %s --cluster8", tool(), fix_path("fatcheck.img"));
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

/** Run the standard fixtures and the long-name bounds. */
int main(void)
{
    char cmd[2048];
    const char *cc = getenv("CC");

    snprintf(cmd, sizeof(cmd),
             "%s -std=c99 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined "
             "-fno-sanitize-recover=all -o %s tests/host/mkfatfixture.c",
             cc ? cc : "cc", fix_path("mkfatfixture"));
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
    } else {
        fix_keep();
    }
    return fails > 255 ? 255 : fails;
}
