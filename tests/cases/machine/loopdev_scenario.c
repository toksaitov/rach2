/*
 * loopdev_scenario.c: the scenario of machine.loopdev, a file system in a
 * file through a loop device: loopdevprobe at the kernel's debug shell over
 * a disk whose volume holds the images, and the host's tools reading the
 * images the machine wrote.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lifecycle.h"

/* Where the volume of each disk begins, sector 2048, as mtools takes it. */
#define VOLUME_AT "@@1048576"

/* What the probe writes into made.txt of the image it mounts. */
#define MADE "written through a loop device"

/* The kernel's record of an attachment, and of a pull under one. */
#define ATTACHED "loop: loop0: /m/"
#define GONE     "loop: loop0: the device under its file has gone"

/* The most files the scenario writes itself in one attempt. */
#define MADE_FILES 4

/* One attempt, as the scenario runs it. */
struct run {
    struct lc l;
    int failures;                /* assertions reported failed */
    char made[MADE_FILES][1024]; /* the files the scenario wrote */
    int nmade;
};

/** Report assertion `id` through lc_assert, counting a failure. */
static void check(struct run *r, const char *id, int held, const char *detail)
{
    if (!held)
        r->failures++;
    lc_assert(&r->l, id, held, detail);
}

/**
 * Give the path of evidence `name` in `buf`, recording it as a file the
 * scenario writes, which it removes after a passing attempt.
 *
 * @return `buf`, or NULL, with the scenario halted, when there is no room.
 */
static char *own(struct run *r, const char *name, char *buf, size_t size)
{
    const char *path = etude_evidence(r->l.c, name);

    if (path == NULL || strlen(path) >= size || r->nmade == MADE_FILES ||
        strlen(path) >= sizeof(r->made[0])) {
        lc_halt(&r->l, ETUDE_REFUSED, "a file of the scenario's own");
        return NULL;
    }
    strcpy(buf, path);
    strcpy(r->made[r->nmade++], path);
    return buf;
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

/**
 * Run host tool or artifact `argv[0]`, keeping its output as evidence
 * `output`.
 *
 * @return 1 when it exited 0; 0 when it ended otherwise, with how in `why`;
 *         and -1, with the scenario halted, when it could not be run.
 */
static int tool(struct run *r, char *argv[], const char *output, char *why, size_t size)
{
    struct etude_termination end;
    enum etude_status st;

    if (!lc_ok(&r->l))
        return -1;
    st = etude_host(r->l.c, argv, output, &end);
    if (st != ETUDE_OK) {
        lc_halt(&r->l, st, argv[0]);
        return -1;
    }
    if (end.end == ETUDE_END_EXITED && end.value == 0)
        return 1;
    snprintf(why, size, "%s ended with %s %d", argv[0],
             end.end == ETUDE_END_EXITED ? "status" : "end", end.value);
    return 0;
}

/**
 * Boot machine `machine` in step `step` and wait for the debug shell's
 * prompt.
 */
static struct etude_case_machine *shell(struct run *r, const char *step, const char *machine,
                                        uint64_t *at)
{
    struct etude_case_machine *m;

    *at = 0;
    if (!lc_step(&r->l, step) || (m = lc_boot(&r->l, machine)) == NULL ||
        !lc_wait(&r->l, m, "Type 'help'", at, NULL) || !lc_wait(&r->l, m, "\n> ", at, NULL))
        return NULL;
    return m;
}

/**
 * Wait after `*at` for the next line of the probe, which begins
 * `LOOPPROBE `, and put the rest of it in `line`; `console` is the evidence
 * of the boot's console.
 *
 * @return 1 when the line came whole, else 0 with the scenario halted.
 */
static int probe_line(struct run *r, struct etude_case_machine *m, const char *console,
                      uint64_t *at, char *line, size_t size)
{
    uint64_t from = *at;

    if (!lc_wait(&r->l, m, "LOOPPROBE ", at, NULL) || !lc_wait(&r->l, m, "\n", at, NULL))
        return 0;
    if (!lc_text(&r->l, console, "LOOPPROBE ", from, line, size)) {
        lc_halt(&r->l, ETUDE_FAILED, "the probe's line in the console's file");
        return 0;
    }
    return 1;
}

/**
 * Report assertion `id` from the probe's line `line`: it holds when the line
 * is `OK (WHAT)`.
 */
static void probe_ok(struct run *r, const char *id, const char *line, const char *what)
{
    char want[64], detail[200];

    snprintf(want, sizeof(want), "OK (%s)", what);
    snprintf(detail, sizeof(detail), "loopdevprobe says LOOPPROBE %.150s", line);
    check(r, id, strcmp(line, want) == 0, detail);
}

/**
 * Report assertion `id` from a search of the console after `from` for the
 * kernel's record `text`, which comes before the probe's last line.
 */
static void said(struct run *r, struct etude_case_machine *m, const char *id, const char *text,
                 uint64_t from)
{
    enum etude_status st;
    uint64_t at;
    char detail[160];

    if (!lc_ok(&r->l))
        return;
    st = etude_console_find(m, text, from, &at);
    snprintf(detail, sizeof(detail), "the kernel never says '%s'", text);
    check(r, id, st == ETUDE_OK, detail);
}

/* One boot of a case that runs loopdevprobe and reads its line. */
struct row {
    const char *step;     /* the step of the manifest */
    const char *machine;  /* the machine the step boots */
    int boot;             /* which boot of the machine it is, from 1 */
    const char *mode;     /* loopdevprobe's argument */
    const char *ok;       /* the assertion of the probe's line */
    const char *attached; /* the assertion of the kernel's record of the
                             attachment, or NULL */
};

/**
 * Run row `w`: boot its machine, run `loopdevprobe MODE` at the debug shell
 * and report the row's assertions. The machine is then ended with QMP's
 * quit, the volume unmounted.
 */
static void probe(struct run *r, const struct row *w)
{
    struct etude_case_machine *m;
    char command[64], console[64], line[160];
    uint64_t at, from;

    if ((m = shell(r, w->step, w->machine, &at)) == NULL)
        return;
    snprintf(command, sizeof(command), "exec /loopdevprobe %s\r", w->mode);
    snprintf(console, sizeof(console), "%s.%d.console", w->machine, w->boot);
    if (!lc_type(&r->l, m, command))
        return;
    from = at;
    if (!probe_line(r, m, console, &at, line, sizeof(line))) {
        if (!r->l.stopped)
            check(r, w->ok, 0, r->l.why);
        return;
    }
    probe_ok(r, w->ok, line, w->mode);
    if (w->attached != NULL)
        said(r, m, w->attached, ATTACHED, from);
    lc_quit(&r->l, m, NULL);
}

/**
 * Report assertion `id` from the FAT32 image on the copy of disk `disk`:
 * mcopy takes fat.img out of the volume, and mtype must find the probe's
 * text in its made.txt.
 */
static void fat_made(struct run *r, const char *disk, const char *id)
{
    const char *copy = etude_private_copy(r->l.c, disk);
    char image[1024], volume[1100], from[] = "::/fat.img", made[] = "::/made.txt", why[160];
    char mcopy[] = "mcopy", mtype[] = "mtype", n[] = "-n", i[] = "-i";
    char *out[] = {mcopy, n, i, volume, from, image, NULL};
    char *type[] = {mtype, i, image, made, NULL};
    int rc;

    if (!lc_ok(&r->l))
        return;
    if (copy == NULL || own(r, "fat.img", image, sizeof(image)) == NULL) {
        lc_halt(&r->l, ETUDE_REFUSED, "the copy of the disk");
        return;
    }
    snprintf(volume, sizeof(volume), "%s" VOLUME_AT, copy);
    if ((rc = tool(r, out, "mcopy-fat", why, sizeof(why))) == 1 &&
        (rc = tool(r, type, "mtype", why, sizeof(why))) == 1) {
        snprintf(why, sizeof(why), "made.txt of the FAT32 image does not say '%s'", MADE);
        rc = has(etude_evidence(r->l.c, "mtype"), MADE);
    }
    if (rc >= 0)
        check(r, id, rc, why);
}

/**
 * Report assertions `clean` and `made_id` from the KRFFS image on the copy
 * of disk `disk`, which mcopy takes out of the volume: fsck.krffs must call
 * it clean under --strict, and readfile.krffs must read the probe's text
 * out of its /made.txt.
 */
static void krffs_made(struct run *r, const char *disk, const char *clean, const char *made_id)
{
    const char *copy = etude_private_copy(r->l.c, disk);
    char image[1024], volume[1100], text[1024], why[160];
    char from[] = "::/krffs.img", file[] = "/made.txt", strict[] = "--strict";
    char mcopy[] = "mcopy", fsck[] = "fsck.krffs", readfile[] = "readfile.krffs", n[] = "-n",
         i[] = "-i";
    char *out[] = {mcopy, n, i, volume, from, image, NULL};
    char *check_argv[] = {fsck, image, strict, NULL};
    char *read_argv[] = {readfile, image, file, text, NULL};
    int rc;

    if (!lc_ok(&r->l))
        return;
    if (copy == NULL || own(r, "krffs.img", image, sizeof(image)) == NULL ||
        own(r, "made.txt", text, sizeof(text)) == NULL) {
        lc_halt(&r->l, ETUDE_REFUSED, "the copy of the disk");
        return;
    }
    snprintf(volume, sizeof(volume), "%s" VOLUME_AT, copy);
    if ((rc = tool(r, out, "mcopy-krffs", why, sizeof(why))) == 1)
        rc = tool(r, check_argv, "fsck", why, sizeof(why));
    if (rc < 0)
        return;
    check(r, clean, rc, why);
    if ((rc = tool(r, read_argv, "readfile", why, sizeof(why))) == 1) {
        snprintf(why, sizeof(why), "/made.txt of the KRFFS image does not say '%s'", MADE);
        rc = has(text, MADE);
    }
    if (rc >= 0)
        check(r, made_id, rc, why);
}

/**
 * Run the stick's case: once the probe reads through an image on the
 * stick, the stick is pulled with QMP's device_del, and the probe must then
 * see an error and let go of both volumes, with the kernel's record that
 * the device under the file has gone.
 */
static void pull(struct run *r)
{
    struct etude_case_machine *m;
    enum etude_status st;
    char line[160], detail[200], *reply = NULL;
    uint64_t at, from;

    if ((m = shell(r, "pull", "stick", &at)) == NULL ||
        !lc_type(&r->l, m, "exec /loopdevprobe pull\r"))
        return;
    from = at;
    if (!probe_line(r, m, "stick.1.console", &at, line, sizeof(line))) {
        if (!r->l.stopped)
            check(r, "holding", 0, r->l.why);
        return;
    }
    snprintf(detail, sizeof(detail), "loopdevprobe says LOOPPROBE %.150s", line);
    check(r, "holding", strcmp(line, "HOLDING") == 0, detail);
    if (strcmp(line, "HOLDING") != 0) {
        check(r, "probe-ok", 0, detail);
        lc_quit(&r->l, m, NULL);
        return;
    }
    st = etude_qmp(m, "device_del", "{\"id\": \"st0\"}", &reply);
    snprintf(detail, sizeof(detail), "QMP's device_del of the stick answered %.150s",
             reply != NULL ? reply : "nothing");
    free(reply);
    if (st == ETUDE_STOPPED || st == ETUDE_LOST || st == ETUDE_REFUSED) {
        lc_halt(&r->l, st, "QMP's device_del");
        return;
    }
    check(r, "pulled", st == ETUDE_OK, detail);
    if (!probe_line(r, m, "stick.1.console", &at, line, sizeof(line))) {
        if (!r->l.stopped)
            check(r, "probe-ok", 0, r->l.why);
        return;
    }
    probe_ok(r, "probe-ok", line, "pull");
    said(r, m, "gone-said", GONE, from);
    lc_quit(&r->l, m, NULL);
}

/**
 * End the attempt: when every declared assertion was reported and held,
 * remove the files the scenario wrote, and keep them otherwise; then report
 * what was not reached as not run.
 *
 * @return etude_scenario's status, as lc_finish gives it.
 */
static int finish(struct run *r)
{
    int passed = lc_ok(&r->l) && r->failures == 0, i;

    for (i = 0; r->l.ids[i] != NULL; i++)
        passed = passed && r->l.told[i];
    for (i = 0; passed && i < r->nmade; i++)
        if (unlink(r->made[i]) != 0 && errno != ENOENT)
            fprintf(stderr, "loopdev_scenario: %s: not removed: %s\n", r->made[i], strerror(errno));
    return lc_finish(&r->l);
}

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

int etude_scenario(struct etude_case *c)
{
    static const char *const vfats[] = {"probe-ok", "attach-said", "made-in-image", NULL};
    static const char *const krffss[] = {"probe-ok",    "attach-said",   "durable-ok",
                                         "fsck-strict", "made-in-image", NULL};
    static const char *const lags[] = {"probe-ok", "made-in-image", NULL};
    static const char *const ext2backs[] = {"probe-ok", NULL};
    static const char *const pulls[] = {"holding", "pulled", "probe-ok", "gone-said", NULL};
    static const struct row vfat = {"vfat", "volume", 1, "vfat", "probe-ok", "attach-said"};
    static const struct row krffs = {"krffs", "volume", 1, "krffs", "probe-ok", "attach-said"};
    static const struct row durable = {"durable", "volume", 2, "durable", "durable-ok", NULL};
    static const struct row lag = {"lag", "lagging", 1, "vfat", "probe-ok", NULL};
    static const struct row ext2back = {"ext2back", "ext2back", 1, "ext2back", "probe-ok", NULL};
    const char *name = case_name(c);
    struct run r;

    memset(&r, 0, sizeof(r));
    if (strcmp(name, "vfat") == 0) {
        lc_begin(&r.l, c, vfats);
        probe(&r, &vfat);
        if (lc_step(&r.l, "image"))
            fat_made(&r, "loopdev-disk", "made-in-image");
    } else if (strcmp(name, "krffs") == 0) {
        lc_begin(&r.l, c, krffss);
        probe(&r, &krffs);
        probe(&r, &durable);
        if (lc_step(&r.l, "image"))
            krffs_made(&r, "loopdev-disk", "fsck-strict", "made-in-image");
    } else if (strcmp(name, "lag") == 0) {
        lc_begin(&r.l, c, lags);
        probe(&r, &lag);
        if (lc_step(&r.l, "image"))
            fat_made(&r, "loopdev-disk", "made-in-image");
    } else if (strcmp(name, "ext2back") == 0) {
        lc_begin(&r.l, c, ext2backs);
        probe(&r, &ext2back);
    } else if (strcmp(name, "pull") == 0) {
        lc_begin(&r.l, c, pulls);
        pull(&r);
    } else {
        return 1;
    }
    return finish(&r);
}
