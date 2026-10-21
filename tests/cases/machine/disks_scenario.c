/*
 * disks_scenario.c: the scenario of machine.disks (tests/cases/
 * machine.disks.json): two disks of different sizes on one AHCI controller,
 * disksprobe at the kernel's debug shell, and mtype reading the file the
 * probe wrote through /dev/disk1p1 off the copy of the second disk.
 */
#include <stdio.h>
#include <string.h>

#include "lifecycle.h"

/* Where the second disk's FAT32 volume begins, sector 2048, as mtools takes
 * it. */
#define VOLUME_AT "@@1048576"

/* What disksprobe writes into one.txt of the second disk's volume. */
#define ONE "disk one says hello"

/* The kernel's record of each disk: its number, its port and its size. */
#define DISK0 "disk: disk0: ahci 0: 131072 sectors"
#define DISK1 "disk: disk1: ahci 1: 40960 sectors"

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

/**
 * Boot the machine, run disksprobe at the debug shell and report the probe's
 * line and the kernel's record of each disk, then end the machine with QMP's
 * quit, both volumes unmounted.
 */
static void probe(struct lc *l)
{
    struct etude_case_machine *m;
    char line[160], detail[200];
    uint64_t at, from;

    if ((m = lc_shell(l, "probe", "pair", &at)) == NULL || !lc_type(l, m, "exec /disksprobe\r"))
        return;
    from = at;
    if (!lc_wait(l, m, "DISKSPROBE ", &at, NULL) || !lc_wait(l, m, "\n", &at, NULL)) {
        if (!l->stopped)
            lc_assert(l, "disksprobe-ok", 0, l->why);
        return;
    }
    if (!lc_text(l, "pair.1.console", "DISKSPROBE ", from, line, sizeof(line))) {
        lc_halt(l, ETUDE_FAILED, "the probe's line in the console's file");
        return;
    }
    snprintf(detail, sizeof(detail), "disksprobe says DISKSPROBE %.150s", line);
    lc_assert(l, "disksprobe-ok", strcmp(line, "OK") == 0, detail);
    lc_find(l, m, "disk0-port0", DISK0, 0, 1);
    lc_find(l, m, "disk1-port1", DISK1, 0, 1);
    lc_quit(l, m, NULL);
}

/**
 * Report assertion `second-disk-file` from the copy of the second disk:
 * mtype must find the probe's text in one.txt of its volume.
 */
static void second_disk(struct lc *l)
{
    const char *copy = etude_private_copy(l->c, "disks-one");
    char volume[1100], file[] = "::/one.txt", mtype[] = "mtype", i[] = "-i", why[160];
    char *argv[] = {mtype, i, volume, file, NULL};
    struct etude_termination end;
    enum etude_status st;

    if (!lc_step(l, "host"))
        return;
    if (copy == NULL || strlen(copy) + sizeof(VOLUME_AT) > sizeof(volume)) {
        lc_halt(l, ETUDE_REFUSED, "the copy of the second disk");
        return;
    }
    snprintf(volume, sizeof(volume), "%s" VOLUME_AT, copy);
    st = etude_host(l->c, argv, "mtype", &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, "mtype");
        return;
    }
    if (end.end != ETUDE_END_EXITED || end.value != 0)
        snprintf(why, sizeof(why), "mtype of one.txt ended with %s %d",
                 end.end == ETUDE_END_EXITED ? "status" : "end", end.value);
    else
        snprintf(why, sizeof(why), "one.txt of the second disk does not say '%s'", ONE);
    lc_assert(l, "second-disk-file",
              end.end == ETUDE_END_EXITED && end.value == 0 &&
                  has(etude_evidence(l->c, "mtype"), ONE),
              why);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"disksprobe-ok", "disk0-port0", "disk1-port1",
                                      "second-disk-file", NULL};
    struct lc l;

    if (strcmp(etude_case_id(c), "machine.disks.pair") != 0)
        return 1;
    lc_begin(&l, c, ids);
    probe(&l);
    second_disk(&l);
    return lc_finish(&l);
}
