/*
 * fswrite_scenario.c: the scenario of machine.fatwrite and machine.exfat,
 * FAT32 and exFAT written by the kernel and judged on the host by readers
 * and checkers that share nothing with its drivers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The volume is MBR partition 2 of each disk, from this sector on. */
#define PART      "100352"
#define PART_BYTE (100352L * 512)

/* What the two families differ in. */
struct kind {
    const char *type;    /* the type mount(2) takes, and the probes' argument */
    const char *disk;    /* the disk artifact */
    const char *sectors; /* the volume's sectors */
    const char *walker;  /* the artifact that walks the volume */
    const char *checker; /* fstool's verb for the external checker */
    int fat;             /* 1 for FAT32 */
};

static const struct kind fat_kind = {"vfat",         "fatwrite-disk", "28672",
                                     "mkfatfixture", "fat-fsck",      1};
static const struct kind exfat_kind = {"exfat",          "exfat-disk", "32768",
                                       "mkexfatfixture", "exfat-fsck", 0};

/* One attempt of a case. */
struct fs {
    struct lc l;
    const struct kind *k;
    char copy[1024];     /* the disk's private copy */
    char lost[200];      /* the repaired walk's lost-cluster line, or empty */
    uint64_t from;       /* the console offset before the last probe */
    int branch;          /* the checker: 0 unknown, 1 present, 2 absent */
    const char **checks; /* the checker's assertions, ended by NULL */
    int checked[4];      /* checks[i] has been reported */
};

/* The record with which the kernel hands its debug shell back. */
static const char back[] = ", returning to kernel shell";

/**
 * Read the file `path` whole, NUL-ended, giving its length in `*len` when
 * `len` is not NULL. @return the bytes, which the caller frees, or NULL.
 */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = path != NULL ? fopen(path, "rb") : NULL;
    size_t n = 0, cap = 65536;
    char *buf = NULL, *grown;

    if (f == NULL)
        return NULL;
    while ((grown = realloc(buf, cap + 1)) != NULL) {
        buf = grown;
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap || cap >= (64u << 20))
            break;
        cap *= 2;
    }
    if (ferror(f) || buf == NULL || grown == NULL) {
        free(buf);
        buf = NULL;
    } else {
        buf[n] = '\0';
        if (len != NULL)
            *len = n;
    }
    fclose(f);
    return buf;
}

/**
 * Copy into `buf` the line of `text` at `p`, without its line ending.
 * @return the start of the next line, or NULL at the end of `text`.
 */
static const char *line_at(const char *p, char *buf, size_t size)
{
    size_t n = 0;

    if (*p == '\0')
        return NULL;
    while (*p != '\0' && *p != '\n') {
        if (*p != '\r' && n + 1 < size)
            buf[n++] = *p;
        p++;
    }
    buf[n] = '\0';
    return *p == '\n' ? p + 1 : p;
}

/** Say whether a line of `text` is `want` whole. */
static int has_line(const char *text, const char *want)
{
    char line[512];

    while (text != NULL && (text = line_at(text, line, sizeof(line))) != NULL)
        if (strcmp(line, want) == 0)
            return 1;
    return 0;
}

/** Give the path of evidence `name` in `buf`. @return `buf`. */
static char *evidence(struct fs *f, const char *name, char *buf, size_t size)
{
    const char *path = etude_evidence(f->l.c, name);

    snprintf(buf, size, "%s", path != NULL ? path : "");
    return buf;
}

/**
 * Run host tool or artifact `argv[0]`, keeping its standard output as
 * evidence `output`; a tool that could not run halts the scenario.
 * @return 1 when it ran, with its end in `end`.
 */
static int tool(struct fs *f, char *const argv[], const char *output, struct etude_termination *end)
{
    enum etude_status st;

    if (!lc_ok(&f->l))
        return 0;
    st = etude_host(f->l.c, argv, output, end);
    if (st != ETUDE_OK)
        lc_halt(&f->l, st, argv[0]);
    return st == ETUDE_OK;
}

/** Say whether `end` is an exit with status `status`. */
static int exited(const struct etude_termination *end, int status)
{
    return end->end == ETUDE_END_EXITED && end->value == status;
}

/**
 * Judge the walk in `out`, which ended as `end`, putting the reason in
 * `why`: it found nothing, or found only what the volume may show: FAT32's
 * dirty flag, and the line `lost` when it is not empty.
 */
static int walk_clean(const struct fs *f, const char *out, const struct etude_termination *end,
                      char *why, size_t size)
{
    const char *p = out;
    char line[512];

    if (exited(end, 0))
        return 1;
    if (out == NULL || !exited(end, 1) || strstr(out, "check: FAIL: ") == NULL) {
        snprintf(why, size, "%s did not walk the volume", f->k->walker);
        return 0;
    }
    while ((p = line_at(p, line, sizeof(line))) != NULL) {
        if (strncmp(line, "check: ", 7) != 0 || strncmp(line, "check: FAIL: ", 13) == 0 ||
            (f->k->fat && strncmp(line, "check: dirty: ", 14) == 0) ||
            (f->lost[0] != '\0' && strcmp(line, f->lost) == 0))
            continue;
        snprintf(why, size, "the walk says %s", line);
        return 0;
    }
    return 1;
}

/**
 * Walk the volume of the disk's copy with the walker, keeping its output as
 * evidence `name`, and report assertion `id` as walk_clean judges it. With
 * `keep_lost` the walk's line of lost clusters, which the crash may leave
 * and the mount's check leaves for a host tool, is first kept in `lost`.
 */
static void walk(struct fs *f, const char *name, int keep_lost, const char *id)
{
    char path[1100], *argv[] = {(char *)f->k->walker, "--check", f->copy, PART, NULL};
    struct etude_termination end;
    const char *p;
    char *out, why[600];

    if (!tool(f, argv, name, &end))
        return;
    out = slurp(evidence(f, name, path, sizeof(path)), NULL);
    if (keep_lost && out != NULL && (p = strstr(out, "check: lost: ")) != NULL &&
        (p == out || p[-1] == '\n'))
        line_at(p, f->lost, sizeof(f->lost));
    lc_assert(&f->l, id, walk_clean(f, out, &end, why, sizeof(why)), why);
    free(out);
}

/**
 * Report assertion `id`: file `path` is the pattern file the probe wrote,
 * 65536 bytes, byte i being (i * 31 + 7) mod 256. The file is removed when
 * it is.
 */
static void pattern(struct fs *f, const char *path, const char *id)
{
    size_t n = 0, i;
    char *b = slurp(path, &n);
    int held = b != NULL && n == 65536;

    for (i = 0; held && i < n; i++)
        held = (size_t)(unsigned char)b[i] == ((i * 31 + 7) & 0xFF);
    lc_assert(&f->l, id, held,
              b == NULL ? "the host wrote no copy of pattern.bin"
                        : "the host's copy of pattern.bin is not the pattern");
    free(b);
    if (held)
        remove(path);
}

/* The names the write mode leaves in the root, as the host lists them. */
static const char *const fat_names[] = {"::/pattern.bin", "::/Long Name File.txt", "::/Sub Dir/",
                                        "::/renamed.txt", "::/synced.txt",         NULL};
static const char *const exfat_names[] = {"/pattern.bin", "/Long Name File.txt", "/Sub Dir",
                                          "/renamed.txt", "/synced.txt",         NULL};

/**
 * Report assertion `id` from `names`, the host's listing, one name a line:
 * it has each of `want` and no gone.txt.
 */
static void listed(struct fs *f, const char *names, const char *const want[], const char *id)
{
    char why[160];
    size_t i;

    snprintf(why, sizeof(why),
             names == NULL ? "the host's listing failed"
                           : "the host lists gone.txt, which the probe removed");
    for (i = 0; names != NULL && want[i] != NULL; i++)
        if (!has_line(names, want[i])) {
            snprintf(why, sizeof(why), "the host does not list %s", want[i]);
            break;
        }
    lc_assert(&f->l, id, names != NULL && want[i] == NULL && strstr(names, "gone.txt") == NULL,
              why);
}

/**
 * Read the written FAT32 volume with mtools: mcopy's copy of pattern.bin
 * for assertion `mtools-pattern`, and mdir's names for `mtools-names`.
 */
static void mtools(struct fs *f)
{
    char vol[1100], dest[1100], *names;
    char *copy[] = {"mcopy", "-n", "-i", vol, "::/pattern.bin", dest, NULL};
    char *list[] = {"mdir", "-i", vol, "-b", "::", NULL};
    struct etude_termination end;

    snprintf(vol, sizeof(vol), "%s@@%ld", f->copy, PART_BYTE);
    evidence(f, "pattern.bin", dest, sizeof(dest));
    if (tool(f, copy, "mcopy-pattern", &end))
        pattern(f, dest, "mtools-pattern");
    if (!tool(f, list, "mdir", &end))
        return;
    names = slurp(evidence(f, "mdir", dest, sizeof(dest)), NULL);
    listed(f, exited(&end, 0) ? names : NULL, fat_names, "mtools-names");
    free(names);
}

/**
 * Turn the walker's `--list` output in `text` into the names it lists, one
 * a line, in place: each line `check: entry PATH TYPE CLUSTER LENGTH`, with
 * ` nofat` after it for a contiguous file, becomes PATH.
 */
static void entry_names(char *text)
{
    char *in = text, *out = text, *eol, *sp;
    int i;

    while (*in != '\0') {
        eol = strchr(in, '\n');
        if (eol != NULL)
            *eol = '\0';
        if (strncmp(in, "check: entry ", 13) == 0) {
            in += 13;
            sp = in + strlen(in);
            if (sp - in > 6 && strcmp(sp - 6, " nofat") == 0)
                *(sp -= 6) = '\0';
            for (i = 0; i < 3 && (sp = strrchr(in, ' ')) != NULL; i++)
                *sp = '\0';
            memmove(out, in, strlen(in));
            out += strlen(in);
            *out++ = '\n';
        }
        if (eol == NULL)
            break;
        in = eol + 1;
    }
    *out = '\0';
}

/**
 * Read the written exFAT volume with the walker: pattern.bin's bytes for
 * assertion `reader-pattern`, and the names it lists for `reader-names`.
 */
static void reader(struct fs *f)
{
    char dest[1100], *names;
    char *copy[] = {"mkexfatfixture", "--check", f->copy, PART, "--read",
                    "/pattern.bin",   "--out",   dest,    NULL};
    char *list[] = {"mkexfatfixture", "--check", f->copy, PART, "--list", NULL};
    struct etude_termination end;

    evidence(f, "pattern.bin", dest, sizeof(dest));
    if (tool(f, copy, "read-pattern", &end))
        pattern(f, dest, "reader-pattern");
    if (!tool(f, list, "list", &end))
        return;
    names = slurp(evidence(f, "list", dest, sizeof(dest)), NULL);
    if (names != NULL && strstr(names, "check: ") != NULL)
        entry_names(names);
    else {
        free(names);
        names = NULL;
    }
    listed(f, names, exfat_names, "reader-names");
    free(names);
}

/** Report assertion `id` of the checker's group, once. */
static void check_told(struct fs *f, const char *id, int held, const char *detail)
{
    size_t i;

    for (i = 0; f->checks[i] != NULL; i++)
        if (strcmp(f->checks[i], id) == 0 && !f->checked[i]) {
            f->checked[i] = 1;
            etude_assert(f->l.c, id, held, held ? NULL : detail);
        }
}

/**
 * Report the checker's branch from fstool's output `out` and its end, the
 * first time: `absent` when fstool found no checker, else `present`.
 * @return what this run found: 1 for a checker, 2 for none.
 */
static int decide(struct fs *f, const char *out, const struct etude_termination *end)
{
    int absent = exited(end, 3) && out != NULL && strncmp(out, "fstool: absent: ", 16) == 0;

    if (f->branch == 0) {
        f->branch = absent ? 2 : 1;
        etude_branch(f->l.c, "checker", absent ? "absent" : "present");
    }
    return absent ? 2 : 1;
}

/**
 * Run fstool's external checker over a copy of the volume, keeping its
 * output as evidence `name`, and report the branch of the group `checker`
 * and, when the host has a checker, assertion `id`: the checker calls the
 * copy clean. With `repair` it repairs the copy first and must call the
 * repaired copy clean.
 */
static void checker(struct fs *f, const char *name, int repair, const char *id)
{
    char path[1100], why[200], *out;
    char *argv[] = {"fstool", (char *)f->k->checker, f->copy,
                    PART,     (char *)f->k->sectors, repair ? "repair" : NULL,
                    NULL};
    struct etude_termination end;
    int found;

    if (!tool(f, argv, name, &end))
        return;
    out = slurp(evidence(f, name, path, sizeof(path)), NULL);
    found = decide(f, out, &end);
    free(out);
    if (f->branch == 2)
        return;
    snprintf(why, sizeof(why), "%s; its output is the evidence %s",
             found == 2 ? "fstool found no checker" : "the checker calls the copy not clean", name);
    check_told(f, id, found == 1 && exited(&end, 0), why);
}

/**
 * Before the scenario ends, report the checker's branch where no check
 * reported it, by fstool's answer over the disk's artifact, and each
 * assertion of a present checker not reported as not run.
 */
static void checker_finish(struct fs *f)
{
    char *argv[] = {"fstool", (char *)f->k->checker, NULL, PART, (char *)f->k->sectors, NULL};
    char path[1100], *out;
    struct etude_termination end;
    size_t i;

    argv[2] = (char *)etude_artifact(f->l.c, f->k->disk);
    if (f->branch == 0 && !f->l.stopped && argv[2] != NULL &&
        etude_host(f->l.c, argv, "fstool-branch", &end) == ETUDE_OK) {
        out = slurp(evidence(f, "fstool-branch", path, sizeof(path)), NULL);
        decide(f, out, &end);
        free(out);
    }
    for (i = 0; f->branch == 1 && f->checks[i] != NULL; i++)
        if (!f->checked[i]) {
            f->checked[i] = 1;
            etude_notrun(f->l.c, f->checks[i],
                         f->l.why[0] ? f->l.why : "the scenario passed it by");
        }
}

/**
 * Type `command` at the debug shell of boot `m`, whose console has been read
 * to `*at`, wait for the record that hands the shell back, and report
 * assertion `id`: the window from the typing has `ok` and the program's exit
 * with status 0. A program that fails halts the scenario, since what it
 * left on the disk is not known. @return 1 when it passed.
 */
static int probe(struct fs *f, struct etude_case_machine *m, const char *command, const char *ok,
                 uint64_t *at, const char *id)
{
    static const char zero[] = " exited with status 0, returning to kernel shell";
    struct lc *l = &f->l;
    uint64_t from = *at, x;
    char line[128], why[200];
    int said, held;

    snprintf(line, sizeof(line), "%s\r", command);
    if (!lc_type(l, m, line) || !lc_wait(l, m, back, at, NULL)) {
        if (!l->stopped)
            lc_assert(l, id, 0, l->why);
        return 0;
    }
    said = etude_console_find(m, ok, from, &x) == ETUDE_OK;
    held = said && etude_console_find(m, zero, from, &x) == ETUDE_OK;
    snprintf(why, sizeof(why), said ? "%s did not exit with status 0" : "%s did not say %s",
             command, ok);
    lc_assert(l, id, held, why);
    if (!held)
        lc_halt(l, ETUDE_FAILED, command);
    return held;
}

/**
 * Begin step `step`, boot the test image on machine `machine` and run the
 * probe `command` at its debug shell, with the type `exfat` after it on
 * exFAT; `from` is then the console's offset before the typing. Assertion
 * `id` is the probe's, as probe() judges it.
 * @return the machine, still running, or NULL when the scenario cannot go
 *         on.
 */
static struct etude_case_machine *boot_probe(struct fs *f, const char *step, const char *machine,
                                             const char *command, const char *ok, const char *id)
{
    struct etude_case_machine *m;
    char line[96];
    uint64_t at;

    if ((m = lc_shell(&f->l, step, machine, &at)) == NULL)
        return NULL;
    snprintf(line, sizeof(line), "%s%s%s", command, f->k->fat ? "" : " ",
             f->k->fat ? "" : f->k->type);
    f->from = at;
    return probe(f, m, line, ok, &at, id) ? m : NULL;
}

/**
 * Say whether `text` is on boot `m`'s console after `from`, which a wait
 * has read past.
 */
static int seen(struct etude_case_machine *m, const char *text, uint64_t from)
{
    uint64_t x;

    return etude_console_find(m, text, from, &x) == ETUDE_OK;
}

/**
 * The crash: boot the test image, start the probe's endless rewrites and end
 * the machine with QMP's quit once it says the first file is written;
 * assertion `crash-writing`. Then, in step `crashed`, assertion
 * `crash-dirty`: the host finds the volume marked dirty.
 */
static void crash(struct fs *f)
{
    char line[64], path[1100], *out;
    char *dump[] = {"mkexfatfixture", "--check", f->copy, PART, "--dump", NULL};
    char *flags[] = {"mkfatfixture", "--check", f->copy, PART, NULL};
    struct etude_case_machine *m;
    struct etude_termination end;
    uint64_t at, from;

    snprintf(line, sizeof(line), "exec /fatwriteprobe crash%s%s\r", f->k->fat ? "" : " ",
             f->k->fat ? "" : f->k->type);
    if ((m = lc_shell(&f->l, "crash", "probe", &at)) != NULL && lc_type(&f->l, m, line)) {
        from = at;
        if (lc_wait(&f->l, m, "FATWRITE ", &at, NULL) && lc_wait(&f->l, m, "\n", &at, NULL)) {
            lc_find(&f->l, m, "crash-writing", "FATWRITE WRITING", from, 1);
            if (seen(m, "FATWRITE WRITING", from))
                lc_quit(&f->l, m, NULL);
            else
                lc_halt(&f->l, ETUDE_FAILED, "fatwriteprobe crash");
        } else if (!f->l.stopped) {
            lc_assert(&f->l, "crash-writing", 0, f->l.why);
        }
    }
    if (!lc_step(&f->l, "crashed") || !tool(f, f->k->fat ? flags : dump, "crashed", &end))
        return;
    out = slurp(evidence(f, "crashed", path, sizeof(path)), NULL);
    lc_assert(&f->l, "crash-dirty",
              out != NULL && (f->k->fat ? strstr(out, "check: dirty: ") != NULL
                                        : strstr(out, "dump: dirty 1,") != NULL),
              "the machine killed in the middle of the sequence left the volume marked clean");
    free(out);
}

/**
 * The repair: boot the test image on what the crash left, whose writable
 * mount checks the volume; assertions `repaired` and `repair-recorded`.
 * Then, in step `repaired`, the walk, its lost clusters aside;
 * `walk-repaired`.
 */
static void repair(struct fs *f)
{
    char checked[96], consistent[64];
    struct etude_case_machine *m;

    snprintf(checked, sizeof(checked), "%s: check: the volume was not unmounted cleanly",
             f->k->type);
    snprintf(consistent, sizeof(consistent), "%s: check: consistent", f->k->type);
    m = boot_probe(f, "repair", "probe", "exec /fatwriteprobe repair", "FATWRITE OK (repaired)",
                   "repaired");
    if (m != NULL) {
        lc_assert(&f->l, "repair-recorded",
                  seen(m, checked, f->from) && seen(m, consistent, f->from),
                  "the mount after the crash did not record its check of the volume");
        lc_quit(&f->l, m, NULL);
    }
    if (lc_step(&f->l, "repaired"))
        walk(f, "walk-repaired", 1, "walk-repaired");
}

/**
 * Give the demo account's password, the artifact `demo-password`'s first
 * line, in `buf` with a carriage return after it, as it is typed.
 * @return 1 when it was read.
 */
static int password(struct fs *f, char *buf, size_t size)
{
    const char *path = etude_artifact(f->l.c, "demo-password");
    FILE *in = path != NULL ? fopen(path, "r") : NULL;
    size_t n = 0;

    if (in != NULL) {
        if (fgets(buf, (int)size - 1, in) == NULL)
            buf[0] = '\0';
        n = strcspn(buf, "\r\n");
        fclose(in);
    }
    if (n == 0) {
        lc_halt(&f->l, ETUDE_FAILED, "the read of the demo account's password");
        return 0;
    }
    buf[n] = '\r';
    buf[n + 1] = '\0';
    return 1;
}

/**
 * Type `line` at the shell of boot `m`, whose console has been read to
 * `*at`, and wait for `end`, which the line prints last, whatever it found;
 * `*from` is then the offset before the typing. Each word a line prints is
 * written split in it, `A""B`, so that the echo of the line is not taken
 * for it. @return 1 when `end` came.
 */
static int typed(struct fs *f, struct etude_case_machine *m, uint64_t *at, const char *line,
                 const char *end, uint64_t *from)
{
    *from = *at;
    return lc_type(&f->l, m, line) && lc_wait(&f->l, m, end, at, NULL);
}

/* The lines of the BusyBox boot after the mount. */
static const char bb_files[] =
    "cp /bin/busybox /mnt/bb && mkdir /mnt/d && mv /mnt/bb '/mnt/d/Busy Box' && "
    "echo hi > /mnt/d/h.txt && rm /mnt/renamed.txt && cmp '/mnt/d/Busy Box' /bin/busybox && "
    "echo SAME\"\"BYTES; echo END\"\"_FILES\r";
static const char bb_dd[] = "A=$(awk '/^disk-commands /{print $2}' /proc/disk); dd if=/dev/zero "
                            "of=/mnt/big.bin bs=64k count=64 2>/dev/null && sync && "
                            "echo DD\"\"OK; echo END\"\"_DD\r";
static const char bb_big[] = "if [ \"$(stat -c %s /mnt/big.bin)\" = 4194304 ]; then "
                             "B=$(awk '/^disk-commands /{print $2}' /proc/disk); "
                             "echo BIG\"\"WRITTEN; fi; echo END\"\"_BIG\r";
static const char bb_count[] = "if rm /mnt/big.bin && sync && [ ! -e /mnt/big.bin ]; then "
                               "C=$(awk '/^disk-commands /{print $2}' /proc/disk); "
                               "echo CMD\"\"S $((B-A)) $((C-B)); fi; echo END\"\"_COUNT\r";
static const char bb_umount[] = "umount /mnt && echo UN\"\"MOUNTED; echo END\"\"_UMOUNT\r";

/**
 * FAT32's rows of the BusyBox boot: 4 MiB written with dd and synced, and
 * the disk commands the write and the removal took, which the kernel counts
 * in /proc/disk. The FAT entries of a run are written a block at a time:
 * 4 MiB is 8192 clusters of 512 bytes here, whose entries fill eight 4 KiB
 * FAT blocks a copy, beside 1024 data blocks written and, since the cache
 * fills a block before it is zeroed, read once each. Fewer than 5000
 * commands for the write is the bound; an entry written through on its own
 * would cost 16384 more. The removal frees the chain the same way, in fewer
 * than 256. Assertions `bb-big`, `bb-write-commands` and `bb-free-commands`.
 */
static void bb_counts(struct fs *f, struct etude_case_machine *m, uint64_t *at)
{
    uint64_t dd, big, count;
    long wrote = 0, freed = 0;
    char text[64], why[96];

    if (!typed(f, m, at, bb_dd, "END_DD", &dd) || !typed(f, m, at, bb_big, "END_BIG", &big))
        return;
    lc_assert(&f->l, "bb-big", seen(m, "DDOK", dd) && seen(m, "BIGWRITTEN", big),
              "dd did not write and sync a file of 4194304 bytes");
    if (!typed(f, m, at, bb_count, "END_COUNT", &count))
        return;
    if (lc_text(&f->l, "busybox.1.console", "CMDS ", count, text, sizeof(text)) &&
        sscanf(text, "%ld %ld", &wrote, &freed) != 2)
        wrote = freed = 0;
    snprintf(why, sizeof(why), "the write took %ld disk commands", wrote);
    lc_assert(&f->l, "bb-write-commands", wrote > 0 && wrote < 5000, why);
    snprintf(why, sizeof(why), "the removal took %ld disk commands", freed);
    lc_assert(&f->l, "bb-free-commands", freed > 0 && freed < 256, why);
}

/**
 * The BusyBox boot: os-real logs root in at the console, mounts the volume,
 * copies, moves, writes and removes on it, and unmounts it; assertions
 * `bb-files` and `bb-unmounted`, and on FAT32 bb_counts()'s.
 */
static void busybox(struct fs *f)
{
    struct etude_case_machine *m;
    uint64_t at = 0, mount, files, umount;
    char secret[256], line[128];

    if (!lc_step(&f->l, "busybox") || (m = lc_boot(&f->l, "busybox")) == NULL ||
        !lc_wait(&f->l, m, "rach2 login:", &at, NULL) || !lc_type(&f->l, m, "root\r") ||
        !lc_wait(&f->l, m, "Password:", &at, NULL) || !password(f, secret, sizeof(secret)) ||
        !lc_type(&f->l, m, secret) || !lc_wait(&f->l, m, "# ", &at, NULL))
        return;
    snprintf(line, sizeof(line),
             "mkdir /mnt && mount -t %s /dev/disk0p2 /mnt && echo MOUNT\"\"ED; "
             "echo END\"\"_MOUNT\r",
             f->k->type);
    if (!typed(f, m, &at, line, "END_MOUNT", &mount) ||
        !typed(f, m, &at, bb_files, "END_FILES", &files))
        return;
    lc_assert(&f->l, "bb-files", seen(m, "MOUNTED", mount) && seen(m, "SAMEBYTES", files),
              "BusyBox did not mount the volume, or copy, move, write and remove on it");
    if (f->k->fat)
        bb_counts(f, m, &at);
    if (!typed(f, m, &at, bb_umount, "END_UMOUNT", &umount))
        return;
    lc_assert(&f->l, "bb-unmounted", seen(m, "UNMOUNTED", umount), "umount /mnt failed");
    lc_quit(&f->l, m, NULL);
}

/**
 * Report assertion `host-copy`: the host reads d/Busy Box, which BusyBox
 * copied onto the volume, as the bytes of os-real's /bin/busybox, which
 * readfile.krffs takes out of the image.
 */
static void host_copy(struct fs *f)
{
    char got[1100], want[1100], vol[1100], *a, *b;
    char *fat[] = {"mcopy", "-n", "-i", vol, "::/d/Busy Box", got, NULL};
    char *exfat[] = {"mkexfatfixture", "--check", f->copy, PART, "--read",
                     "/d/Busy Box",    "--out",   got,     NULL};
    char *take[] = {"readfile.krffs", NULL, "/bin/busybox", want, NULL};
    struct etude_termination end;
    size_t na = 0, nb = 0;
    int held;

    snprintf(vol, sizeof(vol), "%s@@%ld", f->copy, PART_BYTE);
    evidence(f, "busybox.got", got, sizeof(got));
    evidence(f, "busybox.want", want, sizeof(want));
    take[1] = (char *)etude_artifact(f->l.c, "os-real");
    if (!tool(f, f->k->fat ? fat : exfat, "copy-busybox", &end) || !tool(f, take, "readfile", &end))
        return;
    a = slurp(got, &na);
    b = slurp(want, &nb);
    held = a != NULL && b != NULL && na == nb && na > 0 && memcmp(a, b, na) == 0;
    lc_assert(&f->l, "host-copy", held,
              "the host reads d/Busy Box otherwise than os-real's /bin/busybox");
    free(a);
    free(b);
    if (held) {
        remove(got);
        remove(want);
    }
}

/**
 * The case `volume`: the boots that carry the volume's state, in order,
 * each followed by what the host reads of it.
 */
static void volume(struct fs *f)
{
    struct etude_case_machine *m;

    m = boot_probe(f, "write", "probe", "exec /fatwriteprobe write", "FATWRITE OK (wrote)",
                   "wrote");
    if (m != NULL)
        lc_quit(&f->l, m, NULL);
    if (lc_step(&f->l, "written")) {
        walk(f, "walk-written", 0, "walk-written");
        if (f->k->fat)
            mtools(f);
        else
            reader(f);
        checker(f, "checker-written", 0, f->k->fat ? "fsck-written" : "checker-written");
    }
    m = boot_probe(f, "readback", "probe", "exec /fatwriteprobe readback",
                   "FATWRITE OK (read back)", "read-back");
    if (m != NULL)
        lc_quit(&f->l, m, NULL);
    crash(f);
    repair(f);
    busybox(f);
    if (lc_step(&f->l, "final")) {
        walk(f, "walk-busybox", 0, "walk-busybox");
        host_copy(f);
        if (!f->k->fat)
            checker(f, "checker-final", f->lost[0] != '\0', "checker-final");
    }
}

/**
 * The case `writers`: two writers and a sync at once under the lag device,
 * on a fresh copy of the disk; then the walk, and on exFAT the external
 * checker.
 */
static void writers(struct fs *f)
{
    struct etude_case_machine *m;

    m = boot_probe(f, "writers", "lag", "exec /lagprobe writers", "LAGPROBE OK", "writers");
    if (m != NULL)
        lc_quit(&f->l, m, NULL);
    if (!lc_step(&f->l, "host"))
        return;
    walk(f, "walk-writers", 0, "walk-writers");
    if (!f->k->fat)
        checker(f, "checker-writers", 0, "checker-writers");
}

/**
 * The case `latefail`: under the lag a write fails after the mount set
 * VolumeDirty; the mount goes read-only and unmounts, and VolumeFlags, byte
 * 106 of the main boot sector, keeps bit 1, VolumeDirty, on the disk.
 */
static void latefail(struct fs *f)
{
    struct etude_case_machine *m;
    FILE *in;
    int flags = -1;

    m = boot_probe(f, "latefail", "lag", "exec /lagprobe latefail", "LAGPROBE OK", "latefail");
    if (m != NULL) {
        lc_find(&f->l, m, "latefail-read-only", "vfs: read-only: /lb", f->from, 1);
        lc_quit(&f->l, m, NULL);
    }
    if (!lc_step(&f->l, "host"))
        return;
    if ((in = fopen(f->copy, "rb")) != NULL) {
        if (fseek(in, PART_BYTE + 106, SEEK_SET) == 0)
            flags = getc(in);
        fclose(in);
    }
    lc_assert(&f->l, "latefail-dirty", flags >= 0 && (flags & 2) != 0,
              "VolumeFlags on the disk does not have VolumeDirty after the failed write");
}

int etude_scenario(struct etude_case *c)
{
    static const char *const fat_ids[] = {"wrote",
                                          "walk-written",
                                          "mtools-pattern",
                                          "mtools-names",
                                          "read-back",
                                          "crash-writing",
                                          "crash-dirty",
                                          "repaired",
                                          "repair-recorded",
                                          "walk-repaired",
                                          "bb-files",
                                          "bb-big",
                                          "bb-write-commands",
                                          "bb-free-commands",
                                          "bb-unmounted",
                                          "walk-busybox",
                                          "host-copy",
                                          NULL};
    static const char *const exfat_ids[] = {
        "wrote",         "walk-written", "reader-pattern", "reader-names",    "read-back",
        "crash-writing", "crash-dirty",  "repaired",       "repair-recorded", "walk-repaired",
        "bb-files",      "bb-unmounted", "walk-busybox",   "host-copy",       NULL};
    static const char *const writer_ids[] = {"writers", "walk-writers", NULL};
    static const char *const latefail_ids[] = {"latefail", "latefail-read-only", "latefail-dirty",
                                               NULL};
    static const char *fat_checks[] = {"fsck-written", NULL};
    static const char *exfat_checks[] = {"checker-written", "checker-final", NULL};
    static const char *writer_checks[] = {"checker-writers", NULL};
    static const char *none[] = {NULL};
    const char *id = etude_case_id(c), *name = strrchr(id, '.');
    const char *copy;
    struct fs f;
    int exfat = strstr(id, ".exfat.") != NULL, rc;

    memset(&f, 0, sizeof(f));
    f.k = exfat ? &exfat_kind : &fat_kind;
    name = name != NULL ? name + 1 : id;
    if (strcmp(name, "volume") == 0) {
        lc_begin(&f.l, c, exfat ? exfat_ids : fat_ids);
        f.checks = exfat ? exfat_checks : fat_checks;
    } else if (strcmp(name, "writers") == 0) {
        lc_begin(&f.l, c, writer_ids);
        f.checks = exfat ? writer_checks : none;
    } else if (strcmp(name, "latefail") == 0 && exfat) {
        lc_begin(&f.l, c, latefail_ids);
        f.checks = none;
    } else {
        return 1;
    }
    copy = etude_private_copy(c, f.k->disk);
    if (copy == NULL || strlen(copy) >= sizeof(f.copy))
        lc_halt(&f.l, ETUDE_REFUSED, "the copy of the disk");
    else
        snprintf(f.copy, sizeof(f.copy), "%s", copy);
    if (strcmp(name, "volume") == 0)
        volume(&f);
    else if (strcmp(name, "writers") == 0)
        writers(&f);
    else
        latefail(&f);
    rc = lc_finish(&f.l);
    if (f.checks[0] != NULL)
        checker_finish(&f);
    return rc;
}
