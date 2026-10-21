/*
 * parts_scenario.c: the scenario of machine.parts, the partition tables the
 * kernel reads, the root it chooses by identity, and the recovery shell's
 * `disk write`, which must leave a GUID table's backup header and a
 * partition's last sector alone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lifecycle.h"

/* Where the volumes partsprobe writes begin, as mtools takes them: disk A's
 * logical partition 6 at sector 112642, and disk B's GUID entry 3 at sector
 * 8192. */
#define LOGICAL_AT "@@57672704"
#define DATA3_AT   "@@4194304"

/* The far partition of the 5 TiB disk, past sector 2^32, and the seed of
 * its table, another than the one disks B and C share. */
#define FAR_SPEC "4294969344:2048:data=FAR"
#define FAR_SEED "5"

/* The answers of the recovery shell's `disk write`. */
#define GPT_REFUSAL   "disk: GPT disk; the last sector is its backup header"
#define MBR_REFUSAL   "disk: the last sector belongs to partition 1, not a scratch sector"
#define ROUND_TRIP    "last-sector round-trip"
#define ROUND_TRIP_OK "disk: last-sector round-trip OK"
#define NOT_SCRATCH   "not a scratch sector"

/* The line of mkgpt's report that gives entry 4's GUID. */
#define ENTRY4 "mkgpt: entry 4 guid "

#define SECTOR    512
#define GUID_TEXT 36

/**
 * Run host tool or artifact `argv[0]`, keeping its output as evidence
 * `output`.
 *
 * @return 1 when it exited 0; 0 when it ended otherwise, with how in `why`;
 *         and -1, with the scenario halted, when it could not be run.
 */
static int tool(struct lc *l, char *argv[], const char *output, char *why, size_t size)
{
    struct etude_termination end;
    enum etude_status st;

    if (!lc_ok(l))
        return -1;
    st = etude_host(l->c, argv, output, &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, argv[0]);
        return -1;
    }
    if (end.end == ETUDE_END_EXITED && end.value == 0)
        return 1;
    snprintf(why, size, "%s ended with %s %d", argv[0],
             end.end == ETUDE_END_EXITED ? "status" : "end", end.value);
    return 0;
}

/** Say whether file `path` has `text` in its first KiB. */
static int has(const char *path, const char *text)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    char buf[1025];
    size_t n;

    if (f == NULL)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, text) != NULL;
}

/** Give the size of file `path` in bytes, or -1 when it has none. */
static long long file_size(const char *path)
{
    struct stat st;

    return path != NULL && stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

/**
 * Read the `n` bytes at offset `at` of file `path` into `buf`.
 *
 * @return 0, or -1 when the file cannot be read there.
 */
static int read_at(const char *path, long long at, unsigned char *buf, size_t n)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    int rc = -1;

    if (f == NULL)
        return -1;
    if (fseeko(f, (off_t)at, SEEK_SET) == 0 && fread(buf, 1, n, f) == n)
        rc = 0;
    fclose(f);
    return rc;
}

/**
 * Compare the first `n` bytes of files `a` and `b`.
 *
 * @return -1 when they are the same; the offset of the first byte that
 *         differs; or -2 when either cannot be read that far.
 */
static long long first_difference(const char *a, const char *b, long long n)
{
    static unsigned char x[65536], y[65536];
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    long long at = 0, found = -1;
    size_t want, i;

    if (fa == NULL || fb == NULL)
        found = -2;
    while (found == -1 && at < n) {
        want = n - at < (long long)sizeof(x) ? (size_t)(n - at) : sizeof(x);
        if (fread(x, 1, want, fa) != want || fread(y, 1, want, fb) != want) {
            found = -2;
            break;
        }
        for (i = 0; i < want && found == -1; i++)
            if (x[i] != y[i])
                found = at + (long long)i;
        at += (long long)want;
    }
    if (fa != NULL)
        fclose(fa);
    if (fb != NULL)
        fclose(fb);
    return found;
}

/**
 * Wait after `*at` for partsprobe's line, which begins `PARTSPROBE `, and
 * report assertion `id` from it: it holds when the line is `OK`. `console`
 * is the evidence of the boot's console.
 */
static void probe_line(struct lc *l, struct etude_case_machine *m, const char *console,
                       uint64_t *at, const char *id)
{
    uint64_t from = *at;
    char line[200], detail[240];

    if (!lc_wait(l, m, "PARTSPROBE ", at, NULL) || !lc_wait(l, m, "\n", at, NULL)) {
        if (!l->stopped)
            lc_assert(l, id, 0, l->why);
        return;
    }
    if (!lc_text(l, console, "PARTSPROBE ", from, line, sizeof(line))) {
        lc_halt(l, ETUDE_FAILED, "the probe's line in the console's file");
        return;
    }
    snprintf(detail, sizeof(detail), "partsprobe says PARTSPROBE %.200s", line);
    lc_assert(l, id, strcmp(line, "OK") == 0, detail);
}

/**
 * Report assertion `id` from file `file` of the FAT32 volume at `offset` of
 * the private copy of disk `disk`: mtype must print `text` out of it.
 */
static void on_copy(struct lc *l, const char *disk, const char *offset, const char *file,
                    const char *text, const char *id)
{
    const char *copy = etude_private_copy(l->c, disk);
    char volume[1100], path[64], output[64], why[160];
    char mtype[] = "mtype", i[] = "-i";
    char *argv[] = {mtype, i, volume, path, NULL};
    int rc;

    if (!lc_ok(l))
        return;
    if (copy == NULL || strlen(copy) + strlen(offset) >= sizeof(volume)) {
        lc_halt(l, ETUDE_REFUSED, "the copy of the disk");
        return;
    }
    strcpy(volume, copy);
    strcat(volume, offset);
    snprintf(path, sizeof(path), "::/%s", file);
    snprintf(output, sizeof(output), "mtype-%s", file);
    if ((rc = tool(l, argv, output, why, sizeof(why))) == 1) {
        snprintf(why, sizeof(why), "%s of the volume the machine wrote does not say '%s'", file,
                 text);
        rc = has(etude_evidence(l->c, output), text);
    }
    if (rc >= 0)
        lc_assert(l, id, rc, why);
}

/**
 * Run the case of the four tables: partsprobe over disks A to D on the
 * AHCI controller, then the two files it wrote, read by the host out of the
 * copies of A and B.
 */
static void tables(struct lc *l)
{
    struct etude_case_machine *m;
    uint64_t at;

    if ((m = lc_shell(l, "probe", "tables", &at)) == NULL || !lc_type(l, m, "exec /partsprobe\r"))
        return;
    probe_line(l, m, "tables.1.console", &at, "probe-ok");
    if (!lc_quit(l, m, NULL) || !lc_step(l, "files"))
        return;
    on_copy(l, "parts-a", LOGICAL_AT, "logical.txt", "logical six says hello", "logical-file");
    on_copy(l, "parts-b", DATA3_AT, "gpt.txt", "gpt three says hello", "gpt-file");
}

/**
 * Run the case of the 5 TiB disk: its table written by mkgpt into the
 * empty disk, a sparse file the store never hashes, and `partsprobe large`
 * over it.
 */
static void large(struct lc *l)
{
    struct etude_case_machine *m;
    const char *copy;
    char path[1024], why[160];
    char mkgpt[] = "mkgpt", seed[] = "--seed", five[] = FAR_SEED, far[] = FAR_SPEC;
    char *argv[] = {mkgpt, seed, five, path, far, NULL};
    uint64_t at;
    int rc;

    if (!lc_step(l, "layout"))
        return;
    copy = etude_private_copy(l->c, "parts-e");
    if (copy == NULL || strlen(copy) >= sizeof(path)) {
        lc_halt(l, ETUDE_REFUSED, "the empty 5 TiB disk");
        return;
    }
    strcpy(path, copy);
    if ((rc = tool(l, argv, "mkgpt", why, sizeof(why))) != 1) {
        if (rc == 0)
            lc_halt(l, ETUDE_FAILED, why);
        return;
    }
    if ((m = lc_shell(l, "probe", "large", &at)) == NULL ||
        !lc_type(l, m, "exec /partsprobe large\r"))
        return;
    probe_line(l, m, "large.1.console", &at, "probe-ok");
    lc_quit(l, m, NULL);
}

/* One case of a root named by identity: the boot's options name the
 * identity, and its profile the disks. */
struct root_row {
    const char *name;  /* the case, after `machine.parts.` */
    const char *found; /* the record the kernel must print, or NULL for the
                          clone's refusal, which names entry 4's GUID */
    const char *never; /* text that must not be on the console, or NULL */
};

static const struct root_row roots[] = {
    {"root-partuuid", "ext2: root: /dev/disk1p4", NULL},
    {"root-swapped", "ext2: root: /dev/disk0p4", NULL},
    {"root-clone", NULL, "ext2: root:"},
    {"root-mbrid", "root: MBRID=0a0a0a0a,1: /dev/disk1p1, KRFFS", NULL},
    {"root-label", "root: LABEL=DATA3: ambiguous: /dev/disk0p3 and /dev/disk1p3", "root: /dev"},
    {"root-node", "ext2: root: /dev/disk1p4", NULL},
    {"root-unknown", "root: MBRID=deadbeef,1: not found on any of 2 disks", "root: /dev"},
};

/**
 * Copy entry 4's GUID out of mkgpt's report of disk B, the artifact
 * `parts-gpt-report`, into `guid`.
 *
 * @return 1 when the report gives it, else 0 with the scenario halted.
 */
static int entry4_guid(struct lc *l, char guid[GUID_TEXT + 1])
{
    const char *path = etude_artifact(l->c, "parts-gpt-report");
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    char line[200];
    int found = 0;

    while (f != NULL && !found && fgets(line, sizeof(line), f) != NULL)
        if (strncmp(line, ENTRY4, strlen(ENTRY4)) == 0 &&
            strspn(line + strlen(ENTRY4), "0123456789abcdef-") == GUID_TEXT) {
            memcpy(guid, line + strlen(ENTRY4), GUID_TEXT);
            guid[GUID_TEXT] = '\0';
            found = 1;
        }
    if (f != NULL)
        fclose(f);
    if (!found)
        lc_halt(l, ETUDE_FAILED, "entry 4's GUID in mkgpt's report of disk B");
    return found;
}

/**
 * Run root case `w`: boot to the debug shell, end the machine, and look on
 * its console for the record of the root, and for the text that must not
 * be there, since an absence is shown only on a machine that has ended.
 */
static void root(struct lc *l, const struct root_row *w)
{
    struct etude_case_machine *m;
    char guid[GUID_TEXT + 1], clone[160];
    const char *found = w->found;
    uint64_t at;

    if (found == NULL) {
        if (!entry4_guid(l, guid))
            return;
        snprintf(clone, sizeof(clone),
                 "root: PARTUUID=%s: ambiguous: /dev/disk1p4 and /dev/disk2p4", guid);
        found = clone;
    }
    if ((m = lc_shell(l, "boot", "root", &at)) == NULL) {
        if (!l->stopped)
            lc_assert(l, w->never != NULL ? "refused" : "root-found", 0, l->why);
        return;
    }
    if (!lc_quit(l, m, NULL))
        return;
    lc_find(l, m, w->never != NULL ? "refused" : "root-found", found, 0, 1);
    if (w->never != NULL)
        lc_find(l, m, "none-mounted", w->never, 0, 0);
}

/* One case of the recovery shell's `disk write` on a disk of its own, the
 * only disk of the machine. tests/system/parts_disks.sh says why each disk
 * exists: on the two GUID disks without a protective MBR the GUID checks
 * alone keep the backup header, and on the other three the MBR check would
 * refuse the write too, with another answer. */
struct write_row {
    const char *name;   /* the case, after `machine.parts.` */
    const char *disk;   /* the disk's artifact */
    int gpt;            /* the disk has a GUID table, whose backup header
                           must be its last sector */
    const char *answer; /* the refusal, or NULL for the disk whose last
                           sector the command must write */
};

static const struct write_row writes[] = {
    {"write-gpt-nombr", "parts-gpt-nombr", 1, GPT_REFUSAL},
    {"write-gpt-nombr-noprimary", "parts-gpt-nombr-noprimary", 1, GPT_REFUSAL},
    {"write-gpt", "parts-gpt", 1, GPT_REFUSAL},
    {"write-gpt-badprimary", "parts-c", 1, GPT_REFUSAL},
    {"write-gpt-noprimary", "parts-gpt-noprimary", 1, GPT_REFUSAL},
    {"write-mbr-full", "parts-mbr-full", 0, MBR_REFUSAL},
    {"write-mbr-scratch", "parts-mbr-scratch", 0, NULL},
};

/**
 * Report assertion `backup-header` from the last sector of disk `disk`'s
 * artifact, which must begin with a GUID header's signature, so that the
 * case offers the command the disk it means to.
 */
static void backup_header(struct lc *l, const char *disk)
{
    const char *path = etude_artifact(l->c, disk);
    long long size = file_size(path);
    unsigned char last[SECTOR];
    int held;

    held = size >= SECTOR && read_at(path, size - SECTOR, last, SECTOR) == 0 &&
           memcmp(last, "EFI PART", 8) == 0;
    lc_assert(l, "backup-header", held, "the disk's last sector is not its backup GPT header");
}

/**
 * Report the assertions of the disk after the command: for a refusal,
 * `unchanged`, every byte of the copy as the artifact has it; for the
 * written disk, `others-unchanged`, every sector before the last as it was,
 * and `pattern`, the last sector the bytes the command writes, `i ^ 0x5A`
 * at offset `i`.
 */
static void compare(struct lc *l, const struct write_row *w)
{
    const char *copy = etude_private_copy(l->c, w->disk);
    const char *disk = etude_artifact(l->c, w->disk);
    long long size = file_size(disk), diff;
    unsigned char last[SECTOR];
    char why[160];
    int i, held;

    if (!lc_ok(l))
        return;
    if (copy == NULL || size < SECTOR || file_size(copy) != size) {
        lc_halt(l, ETUDE_FAILED, "the copy of the disk, or its size");
        return;
    }
    diff = first_difference(disk, copy, w->answer != NULL ? size : size - SECTOR);
    if (diff == -2) {
        lc_halt(l, ETUDE_FAILED, "the comparison of the copy with the disk");
        return;
    }
    snprintf(why, sizeof(why), "the copy differs from the disk at byte %lld, in sector %lld", diff,
             diff / SECTOR);
    if (w->answer != NULL) {
        lc_assert(l, "unchanged", diff == -1, why);
        return;
    }
    lc_assert(l, "others-unchanged", diff == -1, why);
    held = read_at(copy, size - SECTOR, last, SECTOR) == 0;
    for (i = 0; held && i < SECTOR; i++)
        held = last[i] == ((i ^ 0x5A) & 0xFF);
    lc_assert(l, "pattern", held, "the last sector does not hold the round-trip's pattern");
}

/**
 * Run write case `w`: for a GUID disk, look at its last sector first; then
 * boot the recovery kernel with the disk on IDE, type `disk write` at the
 * debug shell, end the machine once the shell asks again, judge the answer
 * on its console, and compare the copy with the disk.
 */
static void disk_write(struct lc *l, const struct write_row *w)
{
    struct etude_case_machine *m;
    uint64_t at, from;

    if (w->gpt) {
        if (!lc_step(l, "backup"))
            return;
        backup_header(l, w->disk);
    }
    if ((m = lc_shell(l, "write", "write", &at)) == NULL || !lc_type(l, m, "disk write\r"))
        return;
    from = at;
    if (!lc_wait(l, m, "\n> ", &at, NULL) || !lc_quit(l, m, NULL))
        return;
    if (w->answer != NULL) {
        lc_find(l, m, "refused", w->answer, from, 1);
        lc_find(l, m, "no-round-trip", ROUND_TRIP, from, 0);
    } else {
        lc_find(l, m, "round-trip", ROUND_TRIP_OK, from, 1);
        lc_find(l, m, "no-refusal", NOT_SCRATCH, from, 0);
    }
    if (lc_step(l, "compare"))
        compare(l, w);
}

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

int etude_scenario(struct etude_case *c)
{
    static const char *const tabless[] = {"probe-ok", "logical-file", "gpt-file", NULL};
    static const char *const larges[] = {"probe-ok", NULL};
    static const char *const founds[] = {"root-found", NULL};
    static const char *const refuseds[] = {"refused", "none-mounted", NULL};
    static const char *const gpts[] = {"backup-header", "refused", "no-round-trip", "unchanged",
                                       NULL};
    static const char *const mbrs[] = {"refused", "no-round-trip", "unchanged", NULL};
    static const char *const scratches[] = {"round-trip", "no-refusal", "others-unchanged",
                                            "pattern", NULL};
    const char *name = case_name(c);
    struct lc l;
    size_t i;

    if (strcmp(name, "tables") == 0) {
        lc_begin(&l, c, tabless);
        tables(&l);
        return lc_finish(&l);
    }
    if (strcmp(name, "large") == 0) {
        lc_begin(&l, c, larges);
        large(&l);
        return lc_finish(&l);
    }
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (strcmp(name, roots[i].name) == 0) {
            lc_begin(&l, c, roots[i].never != NULL ? refuseds : founds);
            root(&l, &roots[i]);
            return lc_finish(&l);
        }
    for (i = 0; i < sizeof(writes) / sizeof(writes[0]); i++)
        if (strcmp(name, writes[i].name) == 0) {
            lc_begin(&l, c, writes[i].gpt ? gpts : writes[i].answer != NULL ? mbrs : scratches);
            disk_write(&l, &writes[i]);
            return lc_finish(&l);
        }
    return 1;
}
