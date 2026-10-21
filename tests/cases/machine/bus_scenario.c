/*
 * bus_scenario.c: the scenario of machine.bus (tests/cases/machine.bus.json),
 * the disk drivers on every position they serve: four IDE drives on the two
 * channels, an AHCI disk and an IDE disk with the root named on each in
 * turn, and a disk of 130 GiB whose one partition lies past what the 28-bit
 * commands address. The host's mtools read the files the machine wrote off
 * the copies of the disks.
 */
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"

/* Where the volume of each small disk begins, sector 2048, as mtools takes
 * it. */
#define VOLUME_AT "@@1048576"

/* The partition of the big disk: its first sector, past 2^28, its size in
 * sectors, and its first byte as mtools takes it. */
#define FAR_START   "268437504"
#define FAR_SECTORS "65536"
#define FAR_AT      "@@137440002048"

/* What busprobe big writes into far.txt. */
#define FAR_TEXT "written past 128 GiB"

/* One IDE position of the four-drive case. */
struct place {
    const char *disk;    /* the disk's artifact */
    const char *name;    /* the position, as the probe writes it in drive.txt */
    const char *record;  /* the ATA driver's record of the drive there */
    const char *rec_id;  /* the assertion of the record */
    const char *file_id; /* the assertion of drive.txt on the host */
};

static const struct place places[] = {
    {"bus-pm", "primary master", "ata: primary master: 32768 sectors", "pm-record", "pm-file"},
    {"bus-ps", "primary slave", "ata: primary slave: 40960 sectors", "ps-record", "ps-file"},
    {"bus-sm", "secondary master", "ata: secondary master: 49152 sectors", "sm-record", "sm-file"},
    {"bus-ss", "secondary slave", "ata: secondary slave: 57344 sectors", "ss-record", "ss-file"}};

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

/**
 * Read the first KiB of evidence `name` into `buf`, NUL-ended.
 *
 * @return 1 when the file was read, else 0 with `buf` empty.
 */
static int evidence(struct lc *l, const char *name, char *buf, size_t size)
{
    const char *path = etude_evidence(l->c, name);
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    size_t n;

    buf[0] = '\0';
    if (f == NULL)
        return 0;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 1;
}

/**
 * Give in `buf` the path of the copy of disk `disk` followed by `at`, the
 * offset of its volume as mtools takes it.
 *
 * @return 1, or 0 with the scenario halted when there is no copy.
 */
static int volume(struct lc *l, const char *disk, const char *at, char *buf, size_t size)
{
    const char *copy = etude_private_copy(l->c, disk);

    if (copy == NULL || strlen(copy) + strlen(at) >= size) {
        lc_halt(l, ETUDE_REFUSED, "the copy of a disk");
        return 0;
    }
    strcpy(buf, copy);
    strcat(buf, at);
    return 1;
}

/**
 * Report assertion `id` from the first line that begins with `key` after
 * console offset `*at` of machine `machine`, `m`, where a command typed at
 * its debug shell has started a probe: it holds when the rest of the line
 * is `OK`.
 */
static void probe_line(struct lc *l, struct etude_case_machine *m, const char *machine,
                       const char *key, uint64_t *at, const char *id)
{
    char console[64], line[160], detail[200];
    uint64_t from = *at;

    snprintf(console, sizeof(console), "%s.1.console", machine);
    if (!lc_wait(l, m, key, at, NULL) || !lc_wait(l, m, "\n", at, NULL)) {
        if (!l->stopped)
            lc_assert(l, id, 0, l->why);
        return;
    }
    if (!lc_text(l, console, key, from, line, sizeof(line))) {
        lc_halt(l, ETUDE_FAILED, "the probe's line in the console's file");
        return;
    }
    snprintf(detail, sizeof(detail), "the probe says %s%.140s", key, line);
    lc_assert(l, id, strcmp(line, "OK") == 0, detail);
}

/**
 * The four IDE drives: busprobe four mounts the partition of each, writes
 * its position's name into drive.txt and reads it back; each drive has the
 * ATA driver's record of its size, and mtype finds the position's name at
 * the head of drive.txt on that drive's copy alone.
 */
static void four(struct lc *l)
{
    struct etude_case_machine *m;
    char vol[1100], text[1025], why[200], out[16];
    char mtype[] = "mtype", i[] = "-i", file[] = "::/drive.txt";
    char *argv[] = {mtype, i, vol, file, NULL};
    size_t k, n;
    uint64_t at;
    int rc;

    if ((m = lc_shell(l, "probe", "four", &at)) == NULL)
        return;
    if (lc_type(l, m, "exec /busprobe four\r"))
        probe_line(l, m, "four", "BUSPROBE ", &at, "four-drives");
    for (k = 0; k < sizeof(places) / sizeof(places[0]); k++)
        lc_find(l, m, places[k].rec_id, places[k].record, 0, 1);
    lc_quit(l, m, NULL);
    if (!lc_step(l, "host"))
        return;
    for (k = 0; k < sizeof(places) / sizeof(places[0]); k++) {
        if (!volume(l, places[k].disk, VOLUME_AT, vol, sizeof(vol)))
            return;
        snprintf(out, sizeof(out), "mtype-%s", places[k].disk + 4);
        if ((rc = tool(l, argv, out, why, sizeof(why))) < 0)
            return;
        if (rc == 1) {
            evidence(l, out, text, sizeof(text));
            n = strlen(places[k].name);
            rc = strncmp(text, places[k].name, n) == 0 && (text[n] == '\n' || text[n] == '\r');
            snprintf(why, sizeof(why), "drive.txt of the %s begins '%.40s', not its name",
                     places[k].name, text);
        }
        lc_assert(l, places[k].file_id, rc, why);
    }
}

/**
 * A machine with an AHCI disk and an IDE disk, each a KRFFS disk of its own
 * MBR id, booted with the root named by the id of one: the kernel numbers
 * the AHCI disk 0 and the IDE disk 1, says which partition it took as the
 * root, `root: RECORD`, and blockprobe runs on that root.
 */
static void root(struct lc *l, const char *machine, const char *record)
{
    struct etude_case_machine *m;
    char console[64], line[200], detail[300];
    uint64_t at = 0;

    if (!lc_step(l, "root") || (m = lc_boot(l, machine)) == NULL ||
        !lc_wait(l, m, "] root: ", &at, NULL) || !lc_wait(l, m, "\n", &at, NULL))
        return;
    snprintf(console, sizeof(console), "%s.1.console", machine);
    if (!lc_text(l, console, "] root: ", 0, line, sizeof(line))) {
        lc_halt(l, ETUDE_FAILED, "the root's record in the console's file");
        return;
    }
    snprintf(detail, sizeof(detail), "the kernel says root: %.180s", line);
    lc_assert(l, "root-record", strcmp(line, record) == 0, detail);
    lc_find(l, m, "disk0-ahci", "disk: disk0: ahci 0: ", 0, 1);
    lc_find(l, m, "disk1-ata", "disk: disk1: ata 0: ", 0, 1);
    if (strcmp(line, record) != 0)
        lc_assert(l, "blockprobe-ok", 0, "not run: the root is not the partition named");
    else if (lc_wait(l, m, "Type 'help'", &at, NULL) && lc_wait(l, m, "\n> ", &at, NULL) &&
             lc_type(l, m, "exec /blockprobe\r"))
        probe_line(l, m, machine, "BLOCKPROBE ", &at, "blockprobe-ok");
    lc_quit(l, m, NULL);
}

/**
 * The disk of 130 GiB: an empty sparse disk given an MBR whose one FAT32
 * partition starts at sector 268437504, past 2^28. busprobe big must find
 * the disk at its size and write far.txt there through the 48-bit commands,
 * which the ATA driver's record says it has, and mtype must read the file
 * back off the copy.
 */
static void big(struct lc *l)
{
    struct etude_case_machine *m;
    char vol[1100], text[1025], why[200], disk[1100];
    char mkmbr[] = "mkmbr", lba[] = "--lba", id[] = "--id", hex[] = "12345678",
         part[] = FAR_START ":" FAR_SECTORS ":0c";
    char mformat[] = "mformat", i[] = "-i", fat32[] = "-F", t[] = "-T", count[] = FAR_SECTORS,
         v[] = "-v", label[] = "FARAWAY", root_dir[] = "::";
    char mtype[] = "mtype", file[] = "::/far.txt";
    char *table[] = {mkmbr, lba, id, hex, disk, part, NULL};
    char *format[] = {mformat, i, vol, fat32, t, count, v, label, root_dir, NULL};
    char *type[] = {mtype, i, vol, file, NULL};
    const char *copy;
    uint64_t at;
    int rc;

    if (!lc_step(l, "layout"))
        return;
    copy = etude_private_copy(l->c, "bus-big");
    if (copy == NULL || strlen(copy) >= sizeof(disk)) {
        lc_halt(l, ETUDE_REFUSED, "the empty disk");
        return;
    }
    strcpy(disk, copy);
    if (!volume(l, "bus-big", FAR_AT, vol, sizeof(vol)))
        return;
    if ((rc = tool(l, table, "mkmbr", why, sizeof(why))) == 1)
        rc = tool(l, format, "mformat", why, sizeof(why));
    if (rc == 0)
        lc_halt(l, ETUDE_FAILED, why);
    if (rc != 1 || (m = lc_shell(l, "probe", "big", &at)) == NULL)
        return;
    if (lc_type(l, m, "exec /busprobe big\r"))
        probe_line(l, m, "big", "BUSPROBE ", &at, "far-partition");
    lc_find(l, m, "lba48-record", "ata: primary master: 272629760 sectors, 133120 MiB, LBA48", 0,
            1);
    lc_quit(l, m, NULL);
    if (!lc_step(l, "host") || (rc = tool(l, type, "mtype", why, sizeof(why))) < 0)
        return;
    if (rc == 1) {
        evidence(l, "mtype", text, sizeof(text));
        rc = strstr(text, FAR_TEXT) != NULL;
        snprintf(why, sizeof(why), "far.txt does not say '%s'", FAR_TEXT);
    }
    lc_assert(l, "far-file", rc, why);
}

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

int etude_scenario(struct etude_case *c)
{
    static const char *const fours[] = {"four-drives", "pm-record", "ps-record", "sm-record",
                                        "ss-record",   "pm-file",   "ps-file",   "sm-file",
                                        "ss-file",     NULL};
    static const char *const roots[] = {"root-record", "disk0-ahci", "disk1-ata", "blockprobe-ok",
                                        NULL};
    static const char *const bigs[] = {"far-partition", "lba48-record", "far-file", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "four") == 0) {
        lc_begin(&l, c, fours);
        four(&l);
    } else if (strcmp(name, "ide-root") == 0) {
        lc_begin(&l, c, roots);
        root(&l, "ide-root", "MBRID=bbbb0002,1: /dev/disk1p1, KRFFS");
    } else if (strcmp(name, "ahci-root") == 0) {
        lc_begin(&l, c, roots);
        root(&l, "ahci-root", "MBRID=aaaa0001,1: /dev/disk0p1, KRFFS");
    } else if (strcmp(name, "big") == 0) {
        lc_begin(&l, c, bigs);
        big(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
