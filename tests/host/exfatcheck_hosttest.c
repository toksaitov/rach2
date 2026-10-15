/*
 * exfatcheck_hosttest.c: the checker and reader of mkexfatfixture.c,
 * `--check` and `--read`, over the volumes its builder makes.
 *
 * host-deps: tests/host/compat/fixdir_host.c
 *
 * The exFAT tests judge what the kernel wrote with this checker, a second
 * implementation of the format apart from the kernel's driver, so every
 * well-formed volume must pass it and every malformed one fail with the
 * finding named for its fault. The test compiles the builder on every run,
 * into its own fixture directory (compat/fixdir_host.c).
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

    fix_format(text, sizeof(text), "ASAN_OPTIONS=detect_leaks=0 %s", fix_path("mkexfatfixture"));
    return text;
}

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("exfatcheck_hosttest: FAIL: %s\n", what);
    }
}

/** Build a volume with the builder's option `opt`, and require `--check` to
 *  exit with `want_rc` and print `want`. */
static void row(const char *opt, int want_rc, const char *want)
{
    char cmd[4096], line[512], out[8192];
    FILE *p;
    int rc;
    size_t n = 0;

    fix_format(cmd, sizeof(cmd), "%s %s %s", tool(), fix_path("exfatcheck.img"), opt);
    if (system(cmd) != 0) {
        printf("exfatcheck_hosttest: the builder failed for '%s'\n", opt);
        fails++;
        return;
    }
    fix_format(cmd, sizeof(cmd), "%s --check %s", tool(), fix_path("exfatcheck.img"));
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
    rc = rc != -1 && WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    fix_format(cmd, sizeof(cmd), "'%s' exits %d and says '%s'", opt[0] ? opt : "(default)", want_rc,
               want);
    check(rc == want_rc && strstr(out, want) != NULL, cmd);
    if (rc != want_rc || strstr(out, want) == NULL) {
        printf("exfatcheck_hosttest: got exit %d and:\n%s", rc, out);
    }
}

/** The little-endian 32-bit field at `p`, read apart from the checker. */
static uint32_t field32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Store `value` at `p` as a little-endian field of `bytes` bytes. */
static void field_put(unsigned char *p, uint64_t value, unsigned bytes)
{
    unsigned i;

    for (i = 0; i < bytes; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}

/** The entry set of the file `name`, a short ASCII name, in the first
 *  cluster of the root; NULL, and a failed check, when there is none. */
static unsigned char *file_set(unsigned char *img, const char *name)
{
    size_t heap = (size_t)field32(img + 88) << img[108];
    size_t cb = (size_t)1 << (img[108] + img[109]);
    unsigned char *root = img + heap + (field32(img + 96) - 2) * cb;
    size_t i, k, len = strlen(name);

    for (i = 0; i + 96 <= cb; i += 32) {
        unsigned char *e = root + i;

        if (e[0] != 0x85 || e[32] != 0xc0 || e[35] != len || e[64] != 0xc1)
            continue;
        for (k = 0; k < len; k++)
            if (e[66 + k * 2] != (unsigned char)name[k] || e[67 + k * 2] != 0)
                break;
        if (k == len)
            return e;
    }
    check(0, "find fixture stream");
    return NULL;
}

/** Write the `bytes` bytes of `img` to exfatread.img. Returns 1, or 0 when
 *  the write fails. */
static int write_image(const unsigned char *img, size_t bytes)
{
    FILE *f = fopen(fix_path("exfatread.img"), "wb");
    int ok;

    if (!f)
        return 0;
    ok = fwrite(img, 1, bytes, f) == bytes;
    return fclose(f) == 0 && ok;
}

/**
 * Run `--check --read path` over exfatread.img. With `diagnostic` NULL it
 * must exit 0, print nothing on stderr and write the `size` bytes of `want`
 * and no more; otherwise it must exit 1 with `diagnostic` on stderr. No run
 * may print a sanitizer's report. `label` names the check.
 */
static void read_row(const char *path, const unsigned char *want, size_t size,
                     const char *diagnostic, const char *label)
{
    char cmd[4096], errors[8192];
    FILE *f;
    int status, ok, byte;
    size_t i, n;

    fix_format(cmd, sizeof(cmd), "%s --check %s --read '%s' > %s 2> %s", tool(),
               fix_path("exfatread.img"), path, fix_path("exfatread.out"),
               fix_path("exfatread.err"));
    status = system(cmd);
    ok = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == (diagnostic ? 1 : 0);
    f = fopen(fix_path("exfatread.err"), "rb");
    if (!f) {
        check(0, "open reader diagnostics");
        return;
    }
    n = fread(errors, 1, sizeof(errors) - 1, f);
    errors[n] = 0;
    fclose(f);
    ok = ok && (diagnostic ? strstr(errors, diagnostic) != NULL : n == 0);
    ok = ok && strstr(errors, "Sanitizer") == NULL && strstr(errors, "runtime error:") == NULL;
    f = fopen(fix_path("exfatread.out"), "rb");
    if (!f) {
        check(0, "open reader output");
        return;
    }
    if (!diagnostic) {
        for (i = 0; i < size; i++) {
            byte = fgetc(f);
            if (byte != want[i])
                ok = 0;
        }
        if (fgetc(f) != EOF || ferror(f))
            ok = 0;
    }
    fclose(f);
    check(ok, label);
    if (!ok && errors[0])
        fprintf(stderr, "exfatcheck_hosttest: reader diagnostics: %s", errors);
}

/**
 * Write exfatread.img with the stream extension of `name` changed to the
 * first cluster `first`, the length `length` and the valid length `valid`,
 * and the set's checksum made again; `img` keeps the original entries.
 * Returns 1, or 0 when there is no such set or the write fails.
 */
static int write_stream(unsigned char *img, size_t bytes, const char *name, uint32_t first,
                        uint64_t length, uint64_t valid)
{
    unsigned char *set = file_set(img, name);
    unsigned char saved[32];
    unsigned i, count;
    uint16_t sum = 0, oldsum;
    int ok;

    if (!set)
        return 0;
    memcpy(saved, set + 32, sizeof(saved));
    oldsum = (uint16_t)(set[2] | set[3] << 8);
    field_put(set + 52, first, 4);
    field_put(set + 56, length, 8);
    field_put(set + 40, valid, 8);
    count = (set[1] + 1) * 32;
    for (i = 0; i < count; i++)
        if (i != 2 && i != 3)
            sum = (uint16_t)((sum >> 1) | (sum << 15)) + set[i];
    field_put(set + 2, sum, 2);
    ok = write_image(img, bytes);
    check(ok, "write modified fixture");
    memcpy(set + 32, saved, sizeof(saved));
    field_put(set + 2, oldsum, 2);
    return ok;
}

/** A read through the contiguous directory "dir" fails with the diagnostic
 *  of an extent outside the heap when its first cluster is 0, 1, past the
 *  heap or 0xFFFFFFFF, when its extent ends past the heap, when its length
 *  overflows, and when its extent is empty. */
static void directory_rows(unsigned char *img, size_t bytes)
{
    uint32_t clusters;
    static const char extent[] = "mkexfatfixture: directory extent outside the heap";

    clusters = field32(img + 92);
    if (write_stream(img, bytes, "dir", 0, 4096, 4096))
        read_row("dir/sub/leaf.txt", NULL, 0, extent, "reject zero directory cluster");
    if (write_stream(img, bytes, "dir", 1, 4096, 4096))
        read_row("dir/sub/leaf.txt", NULL, 0, extent, "reject reserved directory cluster");
    if (write_stream(img, bytes, "dir", clusters + 2, 4096, 4096))
        read_row("dir/sub/leaf.txt", NULL, 0, extent, "reject directory after heap");
    if (write_stream(img, bytes, "dir", UINT32_MAX, 4096, 4096))
        read_row("dir/sub/leaf.txt", NULL, 0, extent, "reject maximum directory cluster");
    if (write_stream(img, bytes, "dir", clusters + 1, 8192, 8192))
        read_row("dir/sub/leaf.txt", NULL, 0, extent, "reject directory ending past heap");
    if (write_stream(img, bytes, "dir", 2, UINT64_MAX, UINT64_MAX))
        read_row("dir/sub/leaf.txt", NULL, 0, extent, "reject overflowing directory length");
    if (write_stream(img, bytes, "dir", 2, 0, 0))
        read_row("dir/sub/leaf.txt", NULL, 0, extent, "reject empty directory extent");
}

/**
 * The rows of `--read` over the default volume: a contiguous, a nested and
 * an empty file; a component of 599, 600, 601 or 2048 bytes, which is not
 * found; zeros past the valid length; a fragmented file, with its tail
 * zeroed past the valid length in a partial last cluster, and with a chain
 * shorter than its length, which fails; a file with no valid bytes; a valid
 * length past the length, which fails; and the rows of `directory_rows`.
 */
static void read_rows(void)
{
    FILE *f;
    unsigned char *img, expected[12288], *set;
    char path[2049], cmd[2400];
    size_t bytes, i;
    uint32_t first;
    static const size_t path_lengths[] = {599, 600, 601, 2048};

    fix_format(cmd, sizeof(cmd), "%s %s", tool(), fix_path("exfatread.img"));
    if (system(cmd) != 0) {
        check(0, "build read fixture");
        return;
    }
    f = fopen(fix_path("exfatread.img"), "rb");
    if (!f) {
        check(0, "open read fixture");
        return;
    }
    fseek(f, 0, SEEK_END);
    bytes = (size_t)ftell(f);
    rewind(f);
    img = malloc(bytes);
    if (!img || fread(img, 1, bytes, f) != bytes) {
        check(0, "load read fixture");
        free(img);
        fclose(f);
        return;
    }
    fclose(f);
    read_row("hello.txt", (const unsigned char *)"hello exfat\n", 12, NULL, "read contiguous file");
    read_row("/dir/sub/leaf.txt", (const unsigned char *)"leaf\n", 5, NULL, "read nested file");
    read_row("empty", NULL, 0, NULL, "read empty file");
    for (i = 0; i < sizeof(path_lengths) / sizeof(path_lengths[0]); i++) {
        memset(path, 'a', path_lengths[i]);
        path[path_lengths[i]] = 0;
        read_row(path, NULL, 0, "mkexfatfixture: path not found", "oversized path component");
    }
    for (i = 0; i < 8192; i++)
        expected[i] = i < 5000 ? (unsigned char)(0x40 + i) : 0;
    read_row("valid.bin", expected, 8192, NULL, "zero tail through DataLength");
    for (i = 0; i < sizeof(expected); i++)
        expected[i] = (unsigned char)(0x10 * (1 + i / 4096) + i % 4096);
    read_row("chained.bin", expected, sizeof(expected), NULL, "read fragmented file");
    set = file_set(img, "chained.bin");
    if (set) {
        first = field32(set + 52);
        memset(expected + 5000, 0, sizeof(expected) - 5000);
        if (write_stream(img, bytes, "chained.bin", first, 9001, 5000))
            read_row("chained.bin", expected, 9001, NULL,
                     "zero fragmented tail with partial last cluster");
        if (write_stream(img, bytes, "chained.bin", first, 16384, 12288))
            read_row("chained.bin", NULL, 0, "mkexfatfixture: file chain does not cover its length",
                     "reject short file chain");
    }
    set = file_set(img, "valid.bin");
    if (set) {
        first = field32(set + 52);
        memset(expected, 0, sizeof(expected));
        if (write_stream(img, bytes, "valid.bin", first, 8192, 0))
            read_row("valid.bin", expected, 8192, NULL, "zero wholly uninitialized file");
        if (write_stream(img, bytes, "valid.bin", first, 8192, 8193))
            read_row("valid.bin", NULL, 0, "mkexfatfixture: invalid file extent or valid length",
                     "reject valid length past size");
    }
    directory_rows(img, bytes);
    free(img);
}

/**
 * Compile the builder into the fixture directory with the sanitizers and
 * run the rows. The checker must accept the well-formed volume and its
 * variants: a large and an uncompressed up-case table, 4 KiB sectors,
 * timestamps with no valid UTC offset and with an offset and hundredths of
 * a second, a dirty volume, a volume whose free clusters hold junk, and a
 * dirty volume with a label entry inside a directory, which the checker
 * passes over. Each malformed volume breaks one rule and must exit 1 with
 * the finding named for it, and so must each state a crash leaves under the
 * dirty bit. The rows of `read_rows` follow. Returns 0 when every check
 * passed and 1 otherwise.
 */
int main(void)
{
    char cmd[2048];
    const char *cc = getenv("CC");

    fix_format(cmd, sizeof(cmd),
               "%s -std=c99 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined "
               "-fno-sanitize-recover=all -o %s tests/host/mkexfatfixture.c",
               cc ? cc : "cc", fix_path("mkexfatfixture"));
    if (system(cmd) != 0) {
        printf("exfatcheck_hosttest: cannot build the fixture builder\n");
        return 1;
    }

    row("", 0, "check: OK: ");
    row("--big-upcase", 0, "check: OK: ");
    row("--uncompressed-upcase", 0, "check: OK: ");
    row("--sector-4k", 0, "check: OK: ");
    row("--no-utc", 0, "check: OK: ");
    row("--zoned", 0, "check: OK: ");
    row("--dirty", 0, ", dirty");

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
    row("--dirty-label-below", 0, "check: OK: ");

    read_rows();

    if (fails == 0) {
        printf("exfatcheck_hosttest: OK\n");
        fix_passed();
        return 0;
    }
    printf("exfatcheck_hosttest: %d failure(s)\n", fails);
    return 1;
}
