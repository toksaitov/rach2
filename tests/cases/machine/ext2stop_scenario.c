/*
 * ext2stop_scenario.c: the scenario of machine.ext2stop, an ext2 volume
 * stopped under writes and judged against an orderly shutdown: Rach2's next
 * writable mount and its check first, then the host's e2fsck -fn, then the
 * sentinel files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The disks, each one MBR partition at sector 2048 that holds the volume. */
#define PART_OFFSET 1048576L
#define OFFSET_TEXT "1048576"
#define PART_RANGE  "2048:131072:83"
#define AT_PART     "?offset=" OFFSET_TEXT

/* The byte of a disk that has the state field of its volume's superblock:
 * 1 when unmounted cleanly, 0 while mounted writable, 2 with an error. */
#define STATE_AT (PART_OFFSET + 1024 + 58)

/* The files held open and removed, whose inodes are the documented state. */
#define HELD 4

/* The features of the volume, the profile tools/image/mkext2.sh gives the
 * installed root, and its fixed identifier and directory hash seed. */
static char features[] = "sparse_super,large_file,filetype,^dir_index,resize_inode,^ext_attr,"
                         "^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,"
                         "^dir_nlink,extra_isize";
static char extended[] = "offset=" OFFSET_TEXT ",hash_seed=52616368-3200-4578-7432-000000000015";
static char uuid[] = "52616368-3200-4578-7432-000000000015";

/* What e2fsck -fn says of a stopped volume that the kernel's check also
 * counts: the inodes no name reaches, each an orphan, and the files whose
 * size or i_blocks is behind their pointers, each kept. */
struct tally {
    long unattached; /* in use with links: `Unattached inode N` */
    long freed;      /* bit set, inode unused: in `Inode bitmap differences` */
    long sized;      /* `Inode N, i_size is` or `i_blocks is`, once per inode */
    long last;       /* the inode of the last such line */
};

/* What the kernel's check of the stopped volume counted, from its records:
 * `ext2: check: J blocks named by files were free, marked used`, which a
 * stop no longer leaves since a block's bit reaches the disk before any
 * pointer to the block, `ext2:
 * check: F files with a size or i_blocks that does not match their blocks,
 * kept for e2fsck` and `ext2: check: L lost blocks, M orphan inodes, kept
 * for e2fsck`, the first two only when there are any. */
struct counted {
    long marked;  /* J, or 0 */
    long sized;   /* F, or 0 */
    long orphans; /* M, or -1 when there is no record of that form */
};

/* The mount of the volume, which a boot types after `mount -t ext2 DEV`. */
static const char mount_tail[] = " /mnt/s; grep ' /mnt/s ' /proc/mounts; echo MOUNT\"\"END\r";

/* The sentinels: eight files of 64 KiB, each line of file N `keepN`, in a
 * directory of their own, written and synced before anything else. */
static const char keep_line[] =
    "mkdir /mnt/s/keep && i=0 && while [ $i -lt 8 ]; do yes keep$i | head -c 65536 > "
    "/mnt/s/keep/f$i; i=$((i+1)); done && sync && echo KEEP\"\"OK; echo KEEP\"\"END\r";

/* The held files: four of 40000 bytes, opened by the shell, their inode
 * numbers printed as `HELD=N`, then removed and synced, so that the volume
 * has four inodes with no name that the shell keeps until it ends. */
static const char held_line[] =
    "i=0; while [ $i -lt 4 ]; do yes held$i | head -c 40000 > /mnt/s/held$i; i=$((i+1)); done; "
    "exec 3</mnt/s/held0 4</mnt/s/held1 5</mnt/s/held2 6</mnt/s/held3; "
    "stat -c HELD=%i /mnt/s/held0 /mnt/s/held1 /mnt/s/held2 /mnt/s/held3 && "
    "rm /mnt/s/held0 /mnt/s/held1 /mnt/s/held2 /mnt/s/held3 && sync && echo HELD\"\"OK; "
    "echo HELD\"\"END\r";

/* The writes under way: two writers that each make a file of 20000 bytes,
 * append it to a log of their own and remove the file four before it, and a
 * loop of syncs, so that the stop finds blocks in flight and a sync part
 * done. Writer W prints `WRITINGW` at its hundredth file. */
static const char write_line[] =
    "w() { n=0; while :; do yes $1$n | head -c 20000 > /mnt/s/w$1$n; cat /mnt/s/w$1$n >> "
    "/mnt/s/log$1; rm -f /mnt/s/w$1$((n-4)); n=$((n+1)); [ $n -eq 100 ] && echo WRITING$1; "
    "done; }; w a & w b & while :; do sync; done & echo WRITERS\"\"UP\r";

/* The checks of the boot after the stop, on the volume mounted at /mnt/s:
 * the sentinels compared with what wrote them, a write, and the unmount. */
static const char same_line[] =
    "i=0; ok=1; while [ $i -lt 8 ]; do yes keep$i | head -c 65536 | cmp -s - /mnt/s/keep/f$i "
    "|| ok=0; i=$((i+1)); done; [ $ok -eq 1 ] && echo KEEP\"\"SAME; echo SAME\"\"END\r";
static const char after_line[] =
    "echo after > /mnt/s/after && sync && echo AFTER\"\"OK; echo AFTER\"\"END\r";
static const char umount_line[] = "umount /mnt/s && echo UMOUNT\"\"OK; echo UMOUNT\"\"END\r";

/* One boot of a machine, as the scenario drives it. */
struct boot {
    struct etude_case_machine *m;
    char console[48]; /* the evidence of its console, MACHINE.N.console */
    uint64_t at;      /* the offset of the console read so far */
};

/**
 * Read the first line of the artifact `demo-password` into `buf` of `size`
 * bytes, followed by a carriage return, as it is typed. @return 1 when it
 * was read.
 */
static int password(struct lc *l, char *buf, size_t size)
{
    const char *path = etude_artifact(l->c, "demo-password");
    FILE *f = path != NULL ? fopen(path, "r") : NULL;
    size_t n;

    if (f == NULL || fgets(buf, (int)size - 1, f) == NULL || (n = strcspn(buf, "\r\n")) == 0) {
        if (f != NULL)
            fclose(f);
        lc_halt(l, ETUDE_FAILED, "the read of the demo password");
        return 0;
    }
    fclose(f);
    buf[n] = '\r';
    buf[n + 1] = '\0';
    return 1;
}

/**
 * Read bytes `from` to `to` of the console that the evidence `console`
 * keeps.
 *
 * @return the bytes, NUL-ended, which the caller frees; or NULL, after
 *         which the scenario cannot go on.
 */
static char *window(struct lc *l, const char *console, uint64_t from, uint64_t to)
{
    const char *path = etude_evidence(l->c, console);
    FILE *f = path != NULL && from <= to ? fopen(path, "rb") : NULL;
    size_t n = (size_t)(to - from);
    char *buf = NULL;

    if (f != NULL && fseek(f, (long)from, SEEK_SET) == 0 && (buf = malloc(n + 1)) != NULL &&
        fread(buf, 1, n, f) == n) {
        buf[n] = '\0';
    } else {
        free(buf);
        buf = NULL;
        lc_halt(l, ETUDE_FAILED, "the read of the console's evidence");
    }
    if (f != NULL)
        fclose(f);
    return buf;
}

/**
 * Boot machine `machine` with the disks `disks`, a list ended by NULL, and
 * log in as root at its console.
 *
 * @return 1 when the shell's prompt came.
 */
static int start(struct lc *l, const char *machine, const char *const disks[], struct boot *b)
{
    struct etude_termination end;
    enum etude_status st;
    char secret[256], what[96];

    b->at = 0;
    b->m = NULL;
    snprintf(b->console, sizeof(b->console), "%s.1.console", machine);
    if (!lc_ok(l))
        return 0;
    end.end = ETUDE_END_LOST;
    if ((st = etude_boot(l->c, machine, disks, &b->m, &end)) != ETUDE_OK) {
        snprintf(what, sizeof(what), "the boot of %s", machine);
        lc_halt(l, st, what);
        return 0;
    }
    return lc_wait(l, b->m, "rach2 login:", &b->at, NULL) && lc_type(l, b->m, "root\r") &&
           lc_wait(l, b->m, "Password:", &b->at, NULL) && password(l, secret, sizeof(secret)) &&
           lc_type(l, b->m, secret) && lc_wait(l, b->m, "~ #", &b->at, NULL);
}

/**
 * Type `line` at the shell of boot `b` and wait for `end`, which the line
 * prints last whatever it found.
 *
 * @return the console from the typing to `end`, NUL-ended, which the caller
 *         frees; or NULL when the scenario cannot go on.
 */
static char *run(struct lc *l, struct boot *b, const char *line, const char *end)
{
    uint64_t from = b->at;

    if (!lc_ok(l) || !lc_type(l, b->m, line) || !lc_wait(l, b->m, end, &b->at, NULL))
        return NULL;
    return window(l, b->console, from, b->at);
}

/**
 * Report assertion `id` from `w`, what a command printed: it holds when
 * `text` is there. A NULL `w`, after which the scenario cannot go on,
 * reports nothing.
 */
static void seen(struct lc *l, const char *w, const char *id, const char *text)
{
    char why[160];

    if (w == NULL)
        return;
    snprintf(why, sizeof(why), "the console never says '%s'", text);
    lc_assert(l, id, strstr(w, text) != NULL, why);
}

/**
 * Type `line` at boot `b`, wait for `end` and report assertion `id` from
 * what it printed, as `seen` does.
 */
static void expect(struct lc *l, struct boot *b, const char *line, const char *end, const char *id,
                   const char *text)
{
    char *w = run(l, b, line, end);

    seen(l, w, id, text);
    free(w);
}

/**
 * Run host tool `argv[0]`, keeping its output as evidence `output`.
 *
 * @return the status it exited with, -1 for any other end, or -2 when it did
 *         not run, after which the scenario cannot go on.
 */
static int tool(struct lc *l, char *argv[], const char *output)
{
    struct etude_termination end;
    enum etude_status st;

    if (!lc_ok(l))
        return -2;
    st = etude_host(l->c, argv, output, &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, argv[0]);
        return -2;
    }
    return end.end == ETUDE_END_EXITED ? end.value : -1;
}

/**
 * Give in `out` the private copy of disk `disk`, with `suffix` after it.
 * @return 1 when the copy was made.
 */
static int disk_path(struct lc *l, const char *disk, const char *suffix, char *out, size_t size)
{
    const char *copy = etude_private_copy(l->c, disk);
    size_t n = copy != NULL ? strlen(copy) : 0;

    if (copy == NULL || n + strlen(suffix) >= size) {
        lc_halt(l, ETUDE_REFUSED, "the copy of the disk");
        return 0;
    }
    memcpy(out, copy, n);
    memcpy(out + n, suffix, strlen(suffix) + 1);
    return 1;
}

/**
 * Read file `path` whole. @return its bytes, NUL-ended, which the caller
 * frees; or NULL.
 */
static char *slurp(const char *path)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    size_t len = 0, cap = 0, got;
    char *buf = NULL, *grown;

    if (f == NULL)
        return NULL;
    do {
        if (len == cap) {
            cap = cap ? 2 * cap : 65536;
            if ((grown = realloc(buf, cap + 1)) == NULL) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = grown;
        }
        got = fread(buf + len, 1, cap - len, f);
        len += got;
    } while (got > 0);
    fclose(f);
    if (buf == NULL && (buf = malloc(1)) == NULL)
        return NULL;
    buf[len] = '\0';
    return buf;
}

/**
 * Give the state field of the volume on disk `disk`'s copy, or -1 when it
 * cannot be read.
 */
static int state_of(struct lc *l, const char *disk)
{
    const char *copy = etude_private_copy(l->c, disk);
    FILE *f = copy != NULL ? fopen(copy, "rb") : NULL;
    unsigned char b[2];
    int have = -1;

    if (f != NULL && fseek(f, STATE_AT, SEEK_SET) == 0 && fread(b, 1, 2, f) == 2)
        have = b[0] | b[1] << 8;
    if (f != NULL)
        fclose(f);
    return have;
}

/** Report assertion `id`: the volume on disk `disk` has the state `want`. */
static void state(struct lc *l, const char *disk, const char *id, int want)
{
    char why[96];
    int have;

    if (!lc_ok(l))
        return;
    have = state_of(l, disk);
    snprintf(why, sizeof(why), "the superblock's state is %d, not %d", have, want);
    lc_assert(l, id, have == want, why);
}

/**
 * Make the table of disk `disk` and its ext2 volume, as the Debian copy is
 * made: the same options, identifier and hash seed. @return 1 when both
 * tools exited 0.
 */
static int make_volume(struct lc *l, const char *disk)
{
    char path[1100], label[] = "stop", out[48];
    char *table[] = {"mkmbr", "--lba", "--id", "1a26e015", path, PART_RANGE, NULL};
    char *make[] = {"mke2fs", "-q", "-F",     "-t", "ext2", "-b", "4096", "-I", "256", "-O",
                    features, "-E", extended, "-U", uuid,   "-L", label,  path, "64M", NULL};

    if (!disk_path(l, disk, "", path, sizeof(path)))
        return 0;
    snprintf(out, sizeof(out), "%s.mkmbr", disk);
    if (tool(l, table, out) != 0)
        return 0;
    snprintf(out, sizeof(out), "%s.mke2fs", disk);
    return tool(l, make, out) == 0;
}

/**
 * Parse the `HELD=N` lines of `w` into `held`, HELD of them. @return 1 when
 * there are HELD, each a number above 10, the first inode mke2fs leaves
 * free.
 */
static int parse_held(const char *w, long held[HELD])
{
    const char *p = w;
    int n = 0;
    char *end;

    while (n < HELD && (p = strstr(p, "HELD=")) != NULL) {
        p += 5;
        if (*p >= '0' && *p <= '9') {
            held[n] = strtol(p, &end, 10);
            if (held[n] > 10)
                n++;
            p = end;
        }
    }
    return n == HELD;
}

/**
 * The workload of a boot of `machine` on disk0: the mount, its mode, the
 * sentinels, the held files and the writers, which run on when it returns.
 * The held files' inodes go in `held`. Assertions PREFIX-mounted,
 * PREFIX-kept, PREFIX-held and PREFIX-writing.
 *
 * @return 1 when both writers are under way.
 */
static int workload(struct lc *l, struct boot *b, const char *prefix, long held[HELD])
{
    char line[160], id[48], *w;
    uint64_t from;
    int ok;

    snprintf(line, sizeof(line), "mkdir -p /mnt/s && mount -t ext2 /dev/disk0p1%s", mount_tail);
    snprintf(id, sizeof(id), "%s-mounted", prefix);
    w = run(l, b, line, "MOUNTEND");
    seen(l, w, id, "/dev/disk0p1 /mnt/s ext2 rw 0 0");
    if (w != NULL && strstr(w, "/dev/disk0p1 /mnt/s ext2 ") != NULL)
        printf("ext2stop: %s: mode: %.*s\n", prefix,
               (int)strcspn(strstr(w, "/dev/disk0p1 /mnt/s ext2 "), "\r\n"),
               strstr(w, "/dev/disk0p1 /mnt/s ext2 "));
    free(w);
    snprintf(id, sizeof(id), "%s-kept", prefix);
    expect(l, b, keep_line, "KEEPEND", id, "KEEPOK");
    snprintf(id, sizeof(id), "%s-held", prefix);
    w = run(l, b, held_line, "HELDEND");
    if (w != NULL) {
        ok = strstr(w, "HELDOK") != NULL && parse_held(w, held);
        lc_assert(l, id, ok, "the four files were not made, held, numbered, removed and synced");
        if (ok)
            printf("ext2stop: %s: held inodes %ld %ld %ld %ld\n", prefix, held[0], held[1], held[2],
                   held[3]);
    }
    free(w);
    snprintf(id, sizeof(id), "%s-writing", prefix);
    from = b->at;
    ok = lc_ok(l) && lc_type(l, b->m, write_line) && lc_wait(l, b->m, "WRITINGa", &b->at, NULL) &&
         lc_wait(l, b->m, "WRITINGb", &from, NULL);
    if (from > b->at)
        b->at = from;
    if (!l->stopped)
        lc_assert(l, id, ok, "the writers never reached their hundredth file");
    return ok;
}

/**
 * The control: the volume made, the workload, and the machine powered off
 * as a user stops it, with the writers running and the held files open.
 * Assertions `control-volume`, the workload's, `control-unmounted`,
 * `control-valid` and, after the check boot, `control-e2fsck`.
 */
static void control(struct lc *l, long held[HELD])
{
    static const char *const disks[] = {"control", NULL};
    struct boot b;
    char *w;

    if (!lc_step(l, "volume"))
        return;
    lc_assert(l, "control-volume", make_volume(l, "control"), "mkmbr or mke2fs did not exit 0");
    if (!lc_ok(l) || !lc_step(l, "control") || !start(l, "control", disks, &b))
        return;
    if (!workload(l, &b, "control", held))
        return;
    w = run(l, &b, "poweroff\r", "Goodbye");
    seen(l, w, "control-unmounted", "shutdown: /dev/disk0p1: unmounted");
    free(w);
    if (!lc_quit(l, b.m, NULL))
        return;
    state(l, "control", "control-valid", 1);
}

/**
 * The stop: the volume made as the control's, the workload, and QEMU ended
 * by QMP's quit with the writers under way. Assertions `stop-volume`, the
 * workload's and `stop-dirty`; the stopped volume is copied whole into the
 * disk `stopped` before any boot mounts it.
 */
static void stop(struct lc *l, long held[HELD])
{
    static const char *const disks[] = {"stop", NULL};
    char from[1100], to[1100];
    struct boot b;
    FILE *in, *out;
    static unsigned char buf[1 << 20];
    size_t n;
    int ok;

    if (!lc_step(l, "stop"))
        return;
    lc_assert(l, "stop-volume", make_volume(l, "stop"), "mkmbr or mke2fs did not exit 0");
    if (!lc_ok(l) || !start(l, "stop", disks, &b) || !workload(l, &b, "stop", held) ||
        !lc_quit(l, b.m, NULL))
        return;
    state(l, "stop", "stop-dirty", 0);
    if (!lc_step(l, "kept") || !disk_path(l, "stop", "", from, sizeof(from)) ||
        !disk_path(l, "stopped", "", to, sizeof(to)))
        return;
    in = fopen(from, "rb");
    out = fopen(to, "r+b");
    ok = in != NULL && out != NULL;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    if (in != NULL && ferror(in))
        ok = 0;
    if (in != NULL)
        fclose(in);
    if (out != NULL && fclose(out) != 0)
        ok = 0;
    if (!ok)
        lc_halt(l, ETUDE_FAILED, "the copy of the stopped disk");
}

/**
 * Find the kernel's first record of a check in `w` and copy its line into
 * `out`. @return 1 when there is one.
 */
static int check_record(const char *w, char *out, size_t size)
{
    const char *p = strstr(w, "ext2: check: ");

    if (p == NULL)
        return 0;
    snprintf(out, size, "%.*s", (int)strcspn(p, "\r\n"), p);
    return 1;
}

/**
 * Read the kernel's records of a check in `w` into `*k`, printing each.
 * @return NULL when each has one of the forms `struct counted` names, else
 *         the first that has not, copied into `bad` of `size` bytes.
 */
static const char *check_counts(const char *w, struct counted *k, char *bad, size_t size)
{
    char record[200];
    const char *p;
    long a, b;
    int end;

    k->marked = k->sized = 0;
    k->orphans = -1;
    for (p = w; (p = strstr(p, "ext2: check: ")) != NULL; p += strlen("ext2: check: ")) {
        snprintf(record, sizeof(record), "%.*s", (int)strcspn(p, "\r\n"), p);
        printf("ext2stop: %s\n", record);
        end = -1;
        if (sscanf(record, "ext2: check: %ld blocks named by files were free, marked used%n", &a,
                   &end) == 1 &&
            end > 0 && record[end] == '\0') {
            k->marked = a;
            continue;
        }
        end = -1;
        if (sscanf(record,
                   "ext2: check: %ld files with a size or i_blocks that does not match their "
                   "blocks, kept for e2fsck%n",
                   &a, &end) == 1 &&
            end > 0 && record[end] == '\0') {
            k->sized = a;
            continue;
        }
        end = -1;
        if (sscanf(record, "ext2: check: %ld lost blocks, %ld orphan inodes, kept for e2fsck%n", &a,
                   &b, &end) == 2 &&
            end > 0 && record[end] == '\0') {
            k->orphans = b;
            continue;
        }
        snprintf(bad, size, "%s", record);
        return bad;
    }
    return NULL;
}

/**
 * Mount the volume on `dev` at the check boot `b`, judge the records of
 * its check against `checked`, a check expected or not, and run the checks
 * of the boot after a stop. A checked volume must keep the HELD inodes as
 * orphans, and may keep more: the files a writer removed whose inode or bit
 * the stop kept from the disk, after the entry that named them, which the
 * removal writes at once. It may also keep files whose size or i_blocks is
 * behind their pointers, which a stop leaves when a pointer block reached
 * the disk before the inode, but sets the bit of no block a file names,
 * since each bit reaches the disk before any pointer to its block. What it
 * counted goes in `*k`, whose orphans are -1 when there
 * is no record of that form. Assertions PREFIX-writable, PREFIX-check,
 * PREFIX-same, PREFIX-wrote and PREFIX-umount.
 */
static void mount_after(struct lc *l, struct boot *b, const char *dev, const char *prefix,
                        int checked, struct counted *k)
{
    char line[160], id[48], want[64], record[200], why[240];
    char *w;
    int have;

    snprintf(line, sizeof(line), "mkdir -p /mnt/s && mount -t ext2 %s%s", dev, mount_tail);
    if ((w = run(l, b, line, "MOUNTEND")) == NULL)
        return;
    snprintf(id, sizeof(id), "%s-writable", prefix);
    snprintf(want, sizeof(want), "%s /mnt/s ext2 rw 0 0", dev);
    seen(l, w, id, want);
    have = check_record(w, record, sizeof(record));
    snprintf(id, sizeof(id), "%s-check", prefix);
    if (!checked) {
        snprintf(why, sizeof(why), "the clean volume was checked: %s", have ? record : "");
        lc_assert(l, id, !have, why);
    } else if (check_counts(w, k, record, sizeof(record)) != NULL) {
        snprintf(why, sizeof(why), "the check said %s", record);
        lc_assert(l, id, 0, why);
    } else {
        if (k->orphans != -1)
            snprintf(why, sizeof(why), "the check counted %ld orphan inodes", k->orphans);
        else
            snprintf(why, sizeof(why), "the check said %s",
                     have ? "no lost blocks and orphan inodes" : "nothing");
        lc_assert(l, id, k->orphans >= HELD, why);
    }
    free(w);
    snprintf(id, sizeof(id), "%s-same", prefix);
    expect(l, b, same_line, "SAMEEND", id, "KEEPSAME");
    snprintf(id, sizeof(id), "%s-wrote", prefix);
    expect(l, b, after_line, "AFTEREND", id, "AFTEROK");
    snprintf(id, sizeof(id), "%s-umount", prefix);
    expect(l, b, umount_line, "UMOUNTEND", id, "UMOUNTOK");
}

/**
 * The boot after the stop: the stopped volume mounted first, the control
 * after, each checked as `mount_after` says, and the machine powered off.
 * What the check of the stopped volume counts goes in `*k`, its orphans -1
 * when it did not run.
 */
static void check(struct lc *l, struct counted *k)
{
    static const char *const disks[] = {"stop", "control", NULL};
    struct boot b;

    k->marked = k->sized = 0;
    k->orphans = -1;
    if (!lc_step(l, "check") || !start(l, "check", disks, &b))
        return;
    mount_after(l, &b, "/dev/disk0p1", "check", 1, k);
    mount_after(l, &b, "/dev/disk1p1", "again", 0, NULL);
    free(run(l, &b, "poweroff\r", "Goodbye"));
    lc_quit(l, b.m, NULL);
}

/** Say whether `ino` is one of the inodes `held`. */
static int is_held(long ino, const long held[HELD])
{
    int k;

    for (k = 0; k < HELD; k++)
        if (held[k] == ino)
            return 1;
    return 0;
}

/**
 * Say whether `list`, what follows `Block bitmap differences:` or `Inode
 * bitmap differences:` in e2fsck's output, names only numbers the bitmap
 * marks used and e2fsck found free, each `-N` or `-(N--M)`: lost blocks,
 * or inodes, which `*inodes`, when not NULL, counts. A `+`, a number the
 * bitmap calls free that a file uses, `+N` or `+(N--M)`, is a fault.
 */
static int only_freed(const char *list, long *inodes)
{
    long *count;
    const char *p = list;
    long from, to;
    char *end;

    while (*p != '\0') {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (*p == '-')
            count = inodes;
        else
            return 0;
        p++;
        if (*p == '(') {
            from = strtol(p + 1, &end, 10);
            if (strncmp(end, "--", 2) != 0)
                return 0;
            to = strtol(end + 2, &end, 10);
            if (*end++ != ')')
                return 0;
        } else {
            from = to = strtol(p, &end, 10);
        }
        if (end == p || from > to)
            return 0;
        if (count != NULL)
            *count += to - from + 1;
        p = end;
    }
    return 1;
}

/**
 * Judge `text`, a line of e2fsck's output, when it says a file's i_size or
 * i_blocks is not what its pointers need. @return -1 for a line of another
 * form; 1, the file counted in `t` once, when the value is below what the
 * pointers need, as the stop leaves it and the kernel's check keeps it;
 * else 0.
 */
static int size_line(const char *text, struct tally *t)
{
    long ino, was, should;
    int at = -1;

    if ((sscanf(text, "Inode %ld, i_size is %ld, should be %ld.  Fix? no%n", &ino, &was, &should,
                &at) != 3 &&
         sscanf(text, "Inode %ld, i_blocks is %ld, should be %ld.  Fix? no%n", &ino, &was, &should,
                &at) != 3) ||
        at <= 0 || text[at] != '\0')
        return -1;
    if (ino <= 10 || was >= should)
        return 0;
    if (ino != t->last)
        t->sized++;
    t->last = ino;
    return 1;
}

/**
 * `allowed` of the line `text`, a string the caller owns, which loses its
 * trailing spaces and carriage return here.
 */
static int allowed_text(char *text, const long held[HELD], struct tally *t)
{
    static const char *const fixed[] = {"Pass ",
                                        "Free blocks count wrong",
                                        "Free inodes count wrong",
                                        "Fix? no",
                                        "Connect to /lost+found? no",
                                        "stop: ",
                                        "e2fsck ",
                                        "",
                                        NULL};
    static const char deleted[] = "Deleted inode ", unattached[] = "Unattached inode ",
                      empty[] = "Unattached zero-length inode ",
                      blocks[] = "Block bitmap differences:",
                      inodes[] = "Inode bitmap differences:";
    size_t i, len = strlen(text);
    char *end;
    long ino;
    int ok;

    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == ' '))
        text[--len] = '\0';
    for (i = 0; fixed[i] != NULL; i++)
        if (fixed[i][0] == '\0' ? len == 0 : strncmp(text, fixed[i], strlen(fixed[i])) == 0)
            return 1;
    if (strncmp(text, deleted, strlen(deleted)) == 0) {
        ino = strtol(text + strlen(deleted), &end, 10);
        return ino > 10 && strcmp(end, " has zero dtime.  Fix? no") == 0;
    }
    if (strncmp(text, empty, strlen(empty)) == 0) {
        ino = strtol(text + strlen(empty), &end, 10);
        return ino > 10 && strcmp(end, ".  Clear? no") == 0 && !is_held(ino, held);
    }
    if (strncmp(text, unattached, strlen(unattached)) == 0) {
        ino = strtol(text + strlen(unattached), &end, 10);
        if (ino <= 10 || *end != '\0' || is_held(ino, held))
            return 0;
        t->unattached++;
        return 1;
    }
    if ((ok = size_line(text, t)) != -1)
        return ok;
    if (strncmp(text, blocks, strlen(blocks)) == 0)
        return only_freed(text + strlen(blocks), NULL);
    if (strncmp(text, inodes, strlen(inodes)) == 0)
        return only_freed(text + strlen(inodes), &t->freed);
    return 0;
}

/**
 * Say whether the line `line`, `len` bytes of e2fsck's output, is one the
 * documented state of an unclean stop allows, given the inodes `held`: a
 * pass's heading, a removed inode's zero deletion time, a block the bitmap
 * marks used and nothing points to, an inode the bitmap marks used and
 * nothing uses, an inode in use that no name reaches (`Unattached inode N`,
 * after `Unattached zero-length inode N` when it has no blocks) other than
 * a held one, the free counts, a question e2fsck asks and is told no, or
 * its summary; and a regular file's size or i_blocks behind its pointers,
 * which the kernel's check keeps. A block the bitmap calls free that a
 * file names is not allowed, on the volume as the stop left it too, since
 * each bit reaches the disk before any pointer to its block. The inodes no
 * name reaches and the files kept are counted in `*t`.
 *
 * Beyond the held inodes, those are the files a writer removed: the removal
 * writes the entry to the disk at once, so that no name there names a freed
 * inode, and leaves the inode's free to write back, its cleared pointers
 * first, then its zeroed slot and its bits. A stop before all of it is on
 * the disk leaves the inode unattached, or deleted with a zero deletion
 * time, or zeroed with its bit set.
 *
 * The line is judged whole, however long: e2fsck writes all its bitmap
 * differences on one line, and a `+N` past any fixed length would pass
 * unread. A line that cannot be copied is not allowed.
 */
static int allowed(const char *line, size_t len, const long held[HELD], struct tally *t)
{
    char *text = malloc(len + 1);
    int ok;

    if (text == NULL)
        return 0;
    memcpy(text, line, len);
    text[len] = '\0';
    ok = allowed_text(text, held, t);
    free(text);
    return ok;
}

/**
 * Say in `why`, of `size` bytes, where what e2fsck found, `t`, differs from
 * what the kernel's check counted, `k`: the inodes no name reaches, the
 * files kept, and, `before` the check ran, no bit it set of a block named
 * free. @return 1 when they agree, and when `k` is NULL or has no count of
 * orphans.
 */
static int agrees(const struct tally *t, const struct counted *k, int before, char *why,
                  size_t size)
{
    if (k == NULL || k->orphans == -1)
        return 1;
    if (t->unattached + t->freed != k->orphans) {
        snprintf(why, size,
                 "e2fsck -fn finds %ld unattached and %ld freed inodes, the kernel's check %ld "
                 "orphans",
                 t->unattached, t->freed, k->orphans);
        return 0;
    }
    if (t->sized != k->sized) {
        snprintf(why, size,
                 "e2fsck -fn finds %ld files with a size or i_blocks behind their pointers, the "
                 "kernel's check kept %ld",
                 t->sized, k->sized);
        return 0;
    }
    if (before && k->marked != 0) {
        snprintf(why, size, "the kernel's check set the bits of %ld blocks named but free",
                 k->marked);
        return 0;
    }
    return 1;
}

/**
 * Run e2fsck -fn on the volume of disk `disk`, keeping its output as
 * evidence `output`, NAME.e2fsck, and report assertion NAME-e2fsck: for a
 * volume a stop left,
 * whose inodes `held` are not NULL, it exits 0, or 4 with only what
 * `allowed` passes, `before` saying whether the kernel's check has run on
 * it; for a clean one, `held` NULL, it exits 0. Its first line that is not
 * allowed is the reason. When `k` is not NULL, what e2fsck finds must be
 * what the kernel's check counted: as many inodes no name reaches,
 * unattached or freed, as orphans, as many files with a size or i_blocks
 * behind their pointers as files kept, and, before the check, no bit set
 * of a block named free.
 */
static void judge(struct lc *l, const char *disk, const char *output, const long *held, int before,
                  const struct counted *k)
{
    char vol[1100], why[400], id[48];
    char *argv[] = {"e2fsck", "-fn", vol, NULL};
    char *text, *p;
    struct tally t = {0, 0, 0, 0};
    size_t n;
    int status, ok, stopped = held != NULL;

    snprintf(id, sizeof(id), "%.*s-e2fsck", (int)strcspn(output, "."), output);
    if (!disk_path(l, disk, AT_PART, vol, sizeof(vol)) || (status = tool(l, argv, output)) == -2)
        return;
    text = slurp(etude_evidence(l->c, output));
    snprintf(why, sizeof(why), "e2fsck -fn exits %d", status);
    ok = text != NULL && (status == 0 || (stopped && status == 4));
    for (p = text; ok && p != NULL && *p != '\0'; p += n + (p[n] == '\n')) {
        n = strcspn(p, "\n");
        if (stopped && !allowed(p, n, held, &t)) {
            snprintf(why, sizeof(why), "e2fsck -fn exits %d and says: %.*s", status,
                     (int)(n < 300 ? n : 300), p);
            ok = 0;
        }
    }
    if (ok && stopped)
        ok = agrees(&t, k, before, why, sizeof(why));
    lc_assert(l, id, ok, why);
    free(text);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ids[] = {"control-volume", "control-mounted", "control-kept",
                                      "control-held",   "control-writing", "control-unmounted",
                                      "control-valid",  "stop-volume",     "stop-mounted",
                                      "stop-kept",      "stop-held",       "stop-writing",
                                      "stop-dirty",     "check-writable",  "check-check",
                                      "check-same",     "check-wrote",     "check-umount",
                                      "again-writable", "again-check",     "again-same",
                                      "again-wrote",    "again-umount",    "stopped-e2fsck",
                                      "checked-e2fsck", "control-e2fsck",  NULL};
    long kept[HELD], held[HELD];
    struct counted k;
    struct lc l;

    lc_begin(&l, c, ids);
    control(&l, kept);
    stop(&l, held);
    check(&l, &k);
    if (lc_step(&l, "judge")) {
        judge(&l, "stopped", "stopped.e2fsck", held, 1, &k);
        judge(&l, "stop", "checked.e2fsck", held, 0, &k);
        judge(&l, "control", "control.e2fsck", NULL, 0, NULL);
    }
    return lc_finish(&l);
}
