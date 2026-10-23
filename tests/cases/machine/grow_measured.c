/*
 * grow_measured.c: the scenario of machine.grow.measured: growroot on the
 * recovery system grows a 1 GiB ext2 root to the end of a 16 GiB disk at
 * the memory the grow was measured to need, writing only inside the set
 * growset computes; and the same grow stopped inside resize2fs at a write
 * it reports is finished by running growroot again.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../system/checkers/records.h"
#include "lib_grow.h"

/* The disk, its one entry and the volume's groups, as grow_fixture.sh
 * makes the base disk. */
#define DISK_SECTORS 33554432u
#define START        2048u
#define SIZE         2097152u
#define END          (DISK_SECTORS - START)
#define GROUP_BLOCKS 32768u
#define ID           "47470001"

/* The head of the volume that growset reads: the superblock, the
 * descriptors and the resize inode's blocks all lie in the first group. */
#define HEAD_BYTES ((uint64_t)GROUP_BLOCKS * 4096)

/* The first group the grow adds, and the byte of its descriptor on the
 * disk: 32 bytes for each group from the volume's block 1. */
#define ADDED (SIZE / 8 / GROUP_BLOCKS)
#define DESC  ((uint64_t)START * 512 + 4096 + ADDED * 32)

/** The line growroot prints once it has grown the root to the disk's end. */
static const char grown_line[] = "growroot: result: /dev/disk0p1 grown to 33552384 sectors";

/**
 * Read file `path` whole, with a NUL after it, and give its length in `len`.
 *
 * @return the bytes, which the caller frees, or NULL.
 */
static char *read_file(const char *path, size_t *len)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    char *buf = NULL;
    long n;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = malloc((size_t)n + 1)) != NULL) {
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        } else {
            buf[n] = '\0';
            *len = (size_t)n;
        }
    }
    fclose(f);
    return buf;
}

/** Say whether `len` bytes at `buf` hold `text`. */
static int has_bytes(const char *buf, size_t len, const char *text)
{
    size_t n = strlen(text), i;

    for (i = 0; i + n <= len; i++)
        if (memcmp(buf + i, text, n) == 0)
            return 1;
    return 0;
}

/**
 * Type the guest's wrapper, the artifact `tool`, into the recovery machine's
 * RAM root as /tmp/grow-tool, in base64 lines of 160 characters, as the
 * shell's echo carries only text.
 */
static void type_tool(struct drive *d, const char *tool)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *bytes, *code;
    size_t len = 0, i, o = 0;
    unsigned v;

    if ((bytes = read_file(tool, &len)) == NULL || (code = malloc(len / 3 * 4 + 8)) == NULL) {
        free(bytes);
        if (!d->failed) {
            d->failed = 1;
            snprintf(d->why, sizeof(d->why), "the guest's wrapper cannot be read");
        }
        return;
    }
    for (i = 0; i < len; i += 3) {
        v = (unsigned)(unsigned char)bytes[i] << 16;
        if (i + 1 < len)
            v |= (unsigned)(unsigned char)bytes[i + 1] << 8;
        if (i + 2 < len)
            v |= (unsigned char)bytes[i + 2];
        code[o++] = abc[v >> 18 & 63];
        code[o++] = abc[v >> 12 & 63];
        code[o++] = i + 1 < len ? abc[v >> 6 & 63] : '=';
        code[o++] = i + 2 < len ? abc[v & 63] : '=';
    }
    code[o] = '\0';
    grow_run(d, ": > /tmp/grow-tool.b64");
    for (i = 0; i < o; i += 160)
        grow_run(d, "echo '%.160s' >> /tmp/grow-tool.b64", code + i);
    grow_run(d, "base64 -d /tmp/grow-tool.b64 > /tmp/grow-tool && chmod +x /tmp/grow-tool");
    free(code);
    free(bytes);
}

/** Count the lines of `text` that are a checksum of 32768 bytes as cksum
 * prints it, and how many different ones there are, up to two. */
static void checksums(const char *text, int *lines, int *distinct)
{
    char first[32] = "", sum[32];
    const char *line, *eol;
    unsigned long long value;
    int used;

    *lines = *distinct = 0;
    for (line = text; line != NULL && *line != '\0'; line = eol != NULL ? eol + 1 : NULL) {
        eol = strchr(line, '\n');
        used = 0;
        if (sscanf(line, "%llu 32768%n", &value, &used) != 1 || used == 0 ||
            (line[used] != '\n' && line[used] != '\0') || line[0] < '0' || line[0] > '9')
            continue;
        snprintf(sum, sizeof(sum), "%llu", value);
        if (++*lines == 1) {
            snprintf(first, sizeof(first), "%s", sum);
            *distinct = 1;
        } else if (strcmp(first, sum) != 0) {
            *distinct = 2;
        }
    }
}

/** Run e2fsck -fn on the root in the machine of `d`; give 1 when clean. */
static int fsck_clean(struct drive *d)
{
    return grow_shell(d, "FSCK", NULL, "/sbin/e2fsck -fn /dev/disk0p1 > /dev/null 2>&1") == 0;
}

/** Read the added group's inode-table block from its disk descriptor. */
static uint32_t descriptor_table(struct etude_case *c, const char *one)
{
    uint32_t itable = 0;
    char count[32], of[600], in[600];
    char *const argv[] = {"qemu-img", "dd",  "-f", "qcow2", "-O", "raw",
                          "bs=512",   count, in,   of,      NULL};
    unsigned char *bytes;

    snprintf(count, sizeof(count), "count=%" PRIu64, DESC / 512 + 1);
    snprintf(in, sizeof(in), "if=%s", one);
    snprintf(of, sizeof(of), "of=%s", etude_evidence(c, "desc.raw"));
    grow_made(of + 3);
    size_t len = 0;
    bytes = drive_host(c, argv, "desc.out") == 0
                ? (unsigned char *)read_file(etude_evidence(c, "desc.raw"), &len)
                : NULL;
    if (bytes != NULL && len >= DESC + 12)
        itable = (uint32_t)bytes[DESC + 8] | (uint32_t)bytes[DESC + 9] << 8 |
                 (uint32_t)bytes[DESC + 10] << 16 | (uint32_t)bytes[DESC + 11] << 24;
    free(bytes);
    return itable;
}

static int measured_records(struct etude_case *c, const char *measures)
{
    size_t len = 0;
    char *text;
    int ok;
    FILE *report = fopen(etude_evidence(c, "measures.txt"), "w");
    text = read_file(measures, &len);
    ok = report != NULL && text != NULL && check_grow_records(text, report);
    if (report != NULL && fclose(report) != 0)
        ok = 0;
    free(text);
    text = NULL;
    return ok;
}

/**
 * Grow on the overlay `one`, judge its writes and its measures, write a file
 * at the end of the grown volume, read the descriptor of the first added
 * group, and stop a grow on the overlay `two` inside resize2fs at the first
 * write of that group's inode table.
 */
int etude_scenario(struct etude_case *c)
{
    static const char *const one_disks[] = {"one", NULL}, *const two_disks[] = {"two", NULL};
    static const char *const every[] = {
        "measured-image", "grown",       "entry",          "clean",   "inside-set",     "measures",
        "end-file",       "end-clean",   "descriptor",     "stopped", "stopped-report", "refused",
        "finished",       "rerun-entry", "finished-clean", NULL};
    const char *base = etude_artifact(c, "base"), *one;
    char why[300], last_text[32] = "", head[1024], map[1024], measures[1024], *image, *text = NULL;
    char *out = NULL;
    unsigned long long last = 0;
    int set_ok, grown, clean, st, lines, distinct, ok;
    uint32_t itable = 0;
    struct drive d;
    size_t len = 0;

    if (etude_step(c, "set") != ETUDE_OK)
        return 1;
    if (grow_evidence(c, "head.vol", head, sizeof(head)) == NULL ||
        grow_evidence(c, "one.map", map, sizeof(map)) == NULL ||
        grow_evidence(c, "one.text", measures, sizeof(measures)) == NULL) {
        grow_fail_all(c, every, "the evidence's paths are too long");
        return 1;
    }
    grow_made(head);
    grow_made(measures);
    image = read_file(etude_artifact(c, "image"), &len);
    grow_assert(c, "measured-image", image != NULL && has_bytes(image, len, "growroot: measure:"),
                "the recovery image has no grow measurements");
    free(image);
    set_ok = grow_copy_range(base, (uint64_t)START * 512, HEAD_BYTES, head) == 0;
    if (set_ok) {
        char *const argv[] = {"growset", "--lba", "2048", (char *)head, "33552384", NULL};

        set_ok = drive_host(c, argv, "set.txt") == 0;
    }
    if (set_ok && (text = evidence_text(c, "set.txt")) != NULL &&
        text_field(text, "blocks ", 1, last_text, sizeof(last_text)) == 0)
        last = strtoull(last_text, NULL, 10);
    free(text);
    text = NULL;

    if (etude_step(c, "grow") != ETUDE_OK)
        return 1;
    one = etude_private_copy(c, "one");
    grow_boot(&d, c, "recovery", one_disks, "~ #");
    type_tool(&d, etude_artifact(c, "grow_tool"));
    grow_run(&d, "mv /sbin/e2fsck /sbin/e2fsck.actual && cp /tmp/grow-tool /sbin/e2fsck");
    grow_run(&d, "mv /sbin/resize2fs /sbin/resize2fs.actual && cp /tmp/grow-tool /sbin/resize2fs");
    grow_run(&d, "/tmp/grow-tool whole before");
    st = grow_shell(&d, "GROW", &out, "growroot MBRID=%s,1 --measure", ID);
    grown = out != NULL && strstr(out, grown_line) != NULL;
    free(out);
    out = NULL;
    grow_run(&d, "/tmp/grow-tool whole after");
    grow_run(&d,
             "mv /sbin/e2fsck.actual /sbin/e2fsck && mv /sbin/resize2fs.actual /sbin/resize2fs");
    clean = fsck_clean(&d);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    snprintf(why, sizeof(why), "growroot --measure ended with status %d without saying %s", st,
             grown_line);
    grow_assert(c, "grown", grown, st < 0 ? d.why : why);
    grow_assert(
        c, "entry",
        grow_console_has(&d, "growroot: entry: /dev/disk0p1 from 2097152 to 33552384 sectors"),
        "growroot's record of the entry is not of 2097152 sectors grown to 33552384");
    grow_assert(c, "clean", clean,
                d.failed ? d.why : "e2fsck -fn found the grown volume not clean");

    if (etude_step(c, "writes") != ETUDE_OK)
        return 1;
    if (!set_ok) {
        snprintf(why, sizeof(why), "growset computed no grow set from the volume's head");
        ok = 0;
    } else {
        char *const map_argv[] = {"qemu-img", "map", "--output=json", (char *)one, NULL};
        char *const cover_argv[] = {"growset", "--lba", "2048",     "--map",
                                    map,       head,    "33552384", NULL};

        ok = drive_host(c, map_argv, "one.map") == 0;
        snprintf(why, sizeof(why), "qemu-img map of the overlay failed");
        if (ok && !(ok = drive_host(c, cover_argv, "one.cover") == 0)) {
            text = evidence_text(c, "one.cover");
            if (text == NULL || text_field(text, "outside ", 1, why + 8, sizeof(why) - 8) != 0)
                snprintf(why, sizeof(why), "growset could not judge the written extents");
            else
                memcpy(why, "outside ", 8);
            free(text);
            text = NULL;
        }
    }
    grow_assert(c, "inside-set", ok, why);
    text = drive_console(&d);
    ok = text != NULL && grow_write_text(measures, text) == 0;
    free(text);
    text = NULL;
    if (ok)
        ok = measured_records(c, measures);
    grow_assert(c, "measures", ok,
                "growroot's phase measures do not match the wrapper's counters or show no work");

    if (etude_step(c, "end") != ETUDE_OK)
        return 1;
    grow_boot(&d, c, "recovery", one_disks, "~ #");
    if (last <= 40 && !d.failed) {
        d.failed = 1;
        snprintf(d.why, sizeof(d.why), "the grow set gives no block count");
    }
    grow_run(&d, "mkdir -p /mnt && mount -t ext2 /dev/disk0p1 /mnt");
    grow_run(&d, "dd if=/dev/urandom of=/tmp/end bs=4096 count=8 2>/dev/null; cksum < /tmp/end");
    grow_run(&d, "dd if=/tmp/end of=/mnt/end.bin bs=4096 seek=%llu conv=notrunc 2>/dev/null; sync",
             last - 40);
    grow_run(&d, "dd if=/mnt/end.bin bs=4096 skip=%llu count=8 2>/dev/null | cksum", last - 40);
    grow_run(&d, "umount /mnt");
    clean = fsck_clean(&d);
    ok = !d.failed;
    drive_stop(&d);
    if (drive_halted())
        return 1;
    text = drive_console(&d);
    checksums(text != NULL ? text : "", &lines, &distinct);
    free(text);
    text = NULL;
    snprintf(why, sizeof(why), "%d checksums of 32768 bytes, %d distinct", lines, distinct);
    grow_assert(c, "end-file", ok && lines == 2 && distinct == 1, ok ? why : d.why);
    grow_assert(c, "end-clean", clean, d.failed ? d.why : "e2fsck -fn found the volume not clean");

    if (etude_step(c, "descriptor") != ETUDE_OK)
        return 1;
    itable = descriptor_table(c, one);
    snprintf(why, sizeof(why), "the first added group's inode table is at block %" PRIu32, itable);
    ok = itable >= ADDED * GROUP_BLOCKS && itable < (ADDED + 1) * GROUP_BLOCKS;
    grow_assert(c, "descriptor", ok, why);

    if (etude_step(c, "stopped") != ETUDE_OK)
        return 1;
    grow_boot(&d, c, "recovery", two_disks, "~ #");
    if (!ok && !d.failed) {
        d.failed = 1;
        snprintf(d.why, sizeof(d.why), "no block of the added group's inode table to stop at");
    }
    st = grow_shell(&d, "GROW", &out,
                    "TEST_IO_BLOCK=%" PRIu32 " TEST_IO_WRITE_ABORT=1 growroot MBRID=%s,1", itable,
                    ID);
    snprintf(why, sizeof(why), "Aborting due to I/O to block %" PRIu32, itable);
    ok = st == 1 && out != NULL && strstr(out, why) != NULL;
    free(out);
    out = NULL;
    grow_shell(&d, "PIDOF", &out, "pidof resize2fs || echo NO\"\"RESIZER");
    ok = ok && out != NULL && strstr(out, "NORESIZER") != NULL;
    free(out);
    out = NULL;
    snprintf(why, sizeof(why),
             "the stopped grow did not end with status 1 after resize2fs said Aborting "
             "due to I/O to block %" PRIu32 ", or left resize2fs running",
             itable);
    grow_assert(c, "stopped", ok, d.failed ? d.why : why);
    st = grow_shell(&d, "RESIZE", &out, "/sbin/resize2fs /dev/disk0p1");
    ok = st == 1 && out != NULL && strstr(out, "Please run 'e2fsck -f /dev/disk0p1' first") != NULL;
    free(out);
    out = NULL;
    st = grow_shell(&d, "GROW", &out, "growroot MBRID=%s,1", ID);
    grown = out != NULL && strstr(out, grown_line) != NULL;
    free(out);
    out = NULL;
    clean = fsck_clean(&d);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    grow_assert(c, "stopped-report",
                grow_console_has(&d, "growroot: /dev/disk0p1: resize2fs status -1"),
                "growroot did not report the stopped resizer's status -1");
    grow_assert(
        c, "refused", ok,
        d.failed ? d.why
                 : "resize2fs alone did not refuse the volume with status 1, asking for e2fsck -f");
    snprintf(why, sizeof(why), "growroot run again ended with status %d without saying %s", st,
             grown_line);
    grow_assert(c, "finished", grown, d.failed ? d.why : why);
    grow_assert(
        c, "rerun-entry",
        grow_console_has(
            &d, "growroot: entry: /dev/disk0p1 already fills the 33552384 sectors after it"),
        "growroot run again did not find the entry already at the disk's end");
    grow_assert(c, "finished-clean", clean,
                d.failed ? d.why : "e2fsck -fn found the volume not clean");
    grow_clean();
    return 0;
}
