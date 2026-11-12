/*
 * large_scenario.c: the scenario of machine.large, a file of 256 MiB on ext2
 * and on exFAT, a mebibyte astride the 4 GiB line of a sparse ext2 file, a
 * write past each format's limit refused by name, and an ext2 file read
 * twice through the file page cache, on volumes that the independent tools
 * make in an empty AHCI disk and check after the machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lifecycle.h"

/* The empty disk every case boots. */
static const char disk[] = "large";

/* Where its partitions start and how long they are, as the host's tools take
 * them: the 6 GiB ext2 volume by byte, the 6 GiB exFAT one by sector and the
 * 64 MiB FAT32 one by byte, with its length in sectors. */
#define EXT2_OFFSET "1048576"
#define EXT2_SIZE   "6144M"
#define EXFAT_START "12584960"
#define EXFAT_COUNT "12582912"
#define FAT_OFFSET  "12885950464"
#define FAT_SECTORS "131072"

/* The size of the small ext2 volume that case `cache` makes where the FAT32
 * one would start, for the row that changes a volume while it is unmounted. */
#define SMALL_SIZE "16M"

/* The features of the ext2 volume: sparse superblocks, large files, file
 * types in directory entries and the resize inode, and none of the later
 * ones. */
static char ext2_features[] = "sparse_super,large_file,filetype,^dir_index,resize_inode,"
                              "^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,"
                              "^huge_file,^dir_nlink,^extra_isize";

/* The size of the file that the write and the verify take, in MiB. */
#define FILE_MIB "256"

/* The size of the file that case `cache` reads twice, in MiB: more than the
 * buffer cache keeps, half the kernel's heap of 256 MiB. */
#define CACHE_MIB "256"

/* The room for the disk's path with a suffix that names an offset in it. */
#define PATH_LEN 4200

/** Give the last part of the attempt's case identity. */
static const char *case_name(struct etude_case *c)
{
    const char *id = etude_case_id(c), *dot = strrchr(id, '.');

    return dot != NULL ? dot + 1 : id;
}

/**
 * Give the path of the attempt's empty disk, made at the first call, or
 * NULL, after which the scenario cannot go on.
 */
static char *disk_path(struct lc *l)
{
    const char *copy = etude_private_copy(l->c, disk);

    if (copy == NULL || strlen(copy) + 32 > PATH_LEN) {
        lc_halt(l, ETUDE_REFUSED, "the empty disk");
        return NULL;
    }
    return (char *)copy;
}

/**
 * Copy into `buf` the first line of evidence `name`, without its line
 * ending, or the empty string when there is none.
 */
static void first_line(struct lc *l, const char *name, char *buf, size_t size)
{
    const char *path = etude_evidence(l->c, name);
    FILE *f = path != NULL ? fopen(path, "r") : NULL;

    buf[0] = '\0';
    if (f == NULL)
        return;
    if (fgets(buf, (int)size, f) == NULL)
        buf[0] = '\0';
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
}

/**
 * Run host tool or artifact `argv[0]` with its standard output kept as
 * evidence `output`, and say in `why` how it ended when not with status 0,
 * with its first stdout line, or stderr when stdout is empty.
 *
 * @return 1 when it exited 0; 0 otherwise, after which the scenario cannot
 *         go on when the library could not run it to its end.
 */
static int host(struct lc *l, char *argv[], const char *output, char *why, size_t size)
{
    struct etude_termination end;
    enum etude_status st;
    char said[120], error[256];

    if (!lc_ok(l))
        return 0;
    st = etude_host(l->c, argv, output, &end);
    if (st != ETUDE_OK) {
        lc_halt(l, st, argv[0]);
        snprintf(why, size, "%s", l->why);
        return 0;
    }
    if (end.end == ETUDE_END_EXITED && end.value == 0)
        return 1;
    first_line(l, output, said, sizeof(said));
    if (said[0] == '\0') {
        int n = snprintf(error, sizeof(error), "%s.err", output);
        if (n > 0 && (size_t)n < sizeof(error))
            first_line(l, error, said, sizeof(said));
    }
    snprintf(why, size, "%s ended %s %d: %s", argv[0],
             end.end == ETUDE_END_EXITED ? "with status" : "by signal", end.value, said);
    return 0;
}

/**
 * Write the disk's MBR, its identity and three partitions, into the empty
 * disk `path`, and make the case's volume in one of them with `argv`, and a
 * second with `more` unless it is NULL. Assertion `volume` holds when each
 * exits 0; otherwise the scenario cannot go on.
 */
static void volume(struct lc *l, char *path, char *argv[], const char *output, char *more[])
{
    char *table[] = {"mkmbr",
                     "--lba",
                     "--id",
                     "1a26e001",
                     path,
                     "2048:12582912:83",
                     EXFAT_START ":" EXFAT_COUNT ":07",
                     "25167872:" FAT_SECTORS ":0c",
                     NULL};
    char why[200];
    int made;

    if (!lc_step(l, "volume"))
        return;
    made = host(l, table, "mkmbr", why, sizeof(why)) && host(l, argv, output, why, sizeof(why)) &&
           (more == NULL || host(l, more, "mke2fs-small", why, sizeof(why)));
    if (!l->stopped)
        lc_assert(l, "volume", made, why);
    if (!made)
        lc_halt(l, ETUDE_FAILED, "the volume");
}

/**
 * Run `bigfileprobe MODE ARGS` at the debug shell of machine `machine`, `m`,
 * whose console has been read to `*at` and shows the prompt, and wait for the
 * prompt again. Assertion `MODE-exit` holds when the kernel's record of the
 * probe's exit gives status 0, and `MODE-ok` when the probe says `BIGFILE OK
 * (MODE)` and never `BIGFILE FAIL`. The write's rate is printed for a person
 * and not judged.
 *
 * @return the console from the typing to the kernel's record, NUL-ended,
 *         which the caller frees; or NULL when the scenario cannot go on.
 */
static char *bigfile(struct lc *l, struct etude_case_machine *m, const char *machine,
                     const char *mode, const char *args, uint64_t *at)
{
    char command[160], ok[48], why[160], exit_id[32], ok_id[32];
    const char *failed, *rate;
    char *w;

    snprintf(command, sizeof(command), "exec /bigfileprobe %s %s\r", mode, args);
    snprintf(ok, sizeof(ok), "BIGFILE OK (%s)", mode);
    snprintf(exit_id, sizeof(exit_id), "%s-exit", mode);
    snprintf(ok_id, sizeof(ok_id), "%s-ok", mode);
    if ((w = lc_run(l, m, machine, command, at, exit_id)) == NULL)
        return NULL;
    failed = strstr(w, "BIGFILE FAIL");
    if (failed != NULL)
        snprintf(why, sizeof(why), "the probe says %.*s", (int)strcspn(failed, "\r\n"), failed);
    else
        snprintf(why, sizeof(why), "the probe never says %s", ok);
    lc_assert(l, ok_id, failed == NULL && strstr(w, ok) != NULL, why);
    if ((rate = strstr(w, "bigfile: rate: ")) != NULL)
        printf("large: %s: %.*s\n", case_name(l->c), (int)strcspn(rate, "\r\n"), rate);
    if (!lc_wait(l, m, "Type 'help'", at, NULL) || !lc_wait(l, m, "\n> ", at, NULL)) {
        free(w);
        return NULL;
    }
    return w;
}

/**
 * Run bigfileprobe's limit mode with `args` at machine `m` in step `limit`,
 * and report assertion `limit-named`: the kernel's log has `record`, its
 * refusal of the write by name.
 */
static void limit(struct lc *l, struct etude_case_machine *m, const char *machine, const char *args,
                  const char *record, uint64_t *at)
{
    char why[200];
    char *w;

    if (!lc_step(l, "limit") || (w = bigfile(l, m, machine, "limit", args, at)) == NULL)
        return;
    snprintf(why, sizeof(why), "the kernel never says %s", record);
    lc_assert(l, "limit-named", strstr(w, record) != NULL, why);
    free(w);
}

/**
 * Boot machine `write` in step `write`, write the file with bigfileprobe on
 * volume `node` of type `type`, and end the machine.
 */
static void write_file(struct lc *l, const char *node, const char *type)
{
    struct etude_case_machine *m;
    uint64_t at;
    char args[96], *w;

    snprintf(args, sizeof(args), "%s %s %s", node, type, FILE_MIB);
    if ((m = lc_shell(l, "write", "write", &at)) == NULL)
        return;
    if ((w = bigfile(l, m, "write", "write", args, &at)) != NULL)
        free(w);
    lc_quit(l, m, NULL);
}

/**
 * Boot machine `check` in step `verify` and read the file back whole after
 * the remount that the new boot makes, on volume `node` of type `type`.
 *
 * @return the machine, still running, or NULL when the scenario cannot go
 *         on.
 */
static struct etude_case_machine *verify(struct lc *l, const char *node, const char *type,
                                         uint64_t *at)
{
    struct etude_case_machine *m;
    char args[96], *w;

    snprintf(args, sizeof(args), "%s %s %s", node, type, FILE_MIB);
    if ((m = lc_shell(l, "verify", "check", at)) == NULL)
        return NULL;
    if ((w = bigfile(l, m, "check", "verify", args, at)) == NULL)
        return NULL;
    free(w);
    return m;
}

/**
 * End machine `m` and run the host's checker `argv` on the volume it left,
 * in step `fsck`. Assertion `fsck` holds when the checker exits 0.
 */
static void check(struct lc *l, struct etude_case_machine *m, char *argv[])
{
    char why[200];
    int clean;

    if (!lc_quit(l, m, NULL) || !lc_step(l, "fsck"))
        return;
    clean = host(l, argv, "fsck", why, sizeof(why));
    if (!l->stopped)
        lc_assert(l, "fsck", clean, why);
}

/**
 * Write the disk's table into the empty disk `path` and make the ext2 volume
 * in its first partition with mke2fs, in step `volume`, and with `small` a
 * second of SMALL_SIZE at the start of the third partition.
 */
static void ext2_volume(struct lc *l, char *path, int small)
{
    char *make[] = {"mke2fs",
                    "-q",
                    "-F",
                    "-t",
                    "ext2",
                    "-b",
                    "4096",
                    "-I",
                    "128",
                    "-O",
                    ext2_features,
                    "-E",
                    "offset=" EXT2_OFFSET,
                    path,
                    EXT2_SIZE,
                    NULL};
    char *more[] = {"mke2fs",
                    "-q",
                    "-F",
                    "-t",
                    "ext2",
                    "-b",
                    "4096",
                    "-I",
                    "128",
                    "-O",
                    ext2_features,
                    "-E",
                    "offset=" FAT_OFFSET,
                    path,
                    SMALL_SIZE,
                    NULL};

    volume(l, path, make, "mke2fs", small ? more : NULL);
}

/**
 * Run case `ext2`: the volume made by mke2fs; the file written, and read
 * back in a second boot; a mebibyte written astride the 4 GiB line of a
 * sparse file; a write past the file limit for 4096-byte blocks; and
 * `e2fsck -fn` on the volume after the machine.
 */
static void ext2(struct lc *l)
{
    char *path = disk_path(l), at_offset[PATH_LEN];
    char *fsck[] = {"e2fsck", "-fn", at_offset, NULL};
    struct etude_case_machine *m;
    uint64_t at;
    char *w;

    if (path == NULL)
        return;
    snprintf(at_offset, sizeof(at_offset), "%s?offset=" EXT2_OFFSET, path);
    ext2_volume(l, path, 0);
    write_file(l, "/dev/disk0p1", "ext2");
    if ((m = verify(l, "/dev/disk0p1", "ext2", &at)) == NULL)
        return;
    if (lc_step(l, "sparse") &&
        (w = bigfile(l, m, "check", "sparse", "/dev/disk0p1 ext2 0", &at)) != NULL)
        free(w);
    limit(l, m, "check", "/dev/disk0p1 ext2 2196873666560",
          "ext2: write: offset 2095101 MiB is past the file limit of 2095101 MiB for 4096-byte "
          "blocks",
          &at);
    check(l, m, fsck);
}

/**
 * Run case `cache`: the volume made by mke2fs, and a small one in the third
 * partition; pagecachemusl at the debug shell of machine `cache`, which
 * reads a file of CACHE_MIB MiB twice and requires the second read to take
 * nothing from the disks, then changes a small file every way there is
 * between two reads, a change to the small volume while it is unmounted
 * among them, and requires the second read to see the change; and `e2fsck
 * -fn` on the large volume after the machine.
 * Assertion `cache-exit` holds when the kernel's record of the program's
 * exit gives status 0, and `cache-ok` when it says PAGECACHEMUSL OK and
 * never PAGECACHEMUSL FAIL. Its line of counts is printed for a person.
 */
static void cache(struct lc *l)
{
    char *path = disk_path(l), at_offset[PATH_LEN], why[200];
    char *fsck[] = {"e2fsck", "-fn", at_offset, NULL};
    struct etude_case_machine *m;
    const char *failed, *counts;
    uint64_t at;
    char *w;

    if (path == NULL)
        return;
    snprintf(at_offset, sizeof(at_offset), "%s?offset=" EXT2_OFFSET, path);
    ext2_volume(l, path, 1);
    if ((m = lc_shell(l, "cache", "cache", &at)) == NULL)
        return;
    w = lc_run(l, m, "cache", "exec /musl/pagecachemusl /dev/disk0p1 " CACHE_MIB " /dev/disk0p3\r",
               &at, "cache-exit");
    if (w == NULL)
        return;
    failed = strstr(w, "PAGECACHEMUSL FAIL");
    if (failed != NULL)
        snprintf(why, sizeof(why), "the program says %.*s", (int)strcspn(failed, "\r\n"), failed);
    else
        snprintf(why, sizeof(why), "the program never says PAGECACHEMUSL OK");
    lc_assert(l, "cache-ok", failed == NULL && strstr(w, "PAGECACHEMUSL OK") != NULL, why);
    if ((counts = strstr(w, "pagecachemusl: twice: ")) != NULL)
        printf("large: cache: %.*s\n", (int)strcspn(counts, "\r\n"), counts);
    free(w);
    if (!lc_wait(l, m, "Type 'help'", &at, NULL) || !lc_wait(l, m, "\n> ", &at, NULL))
        return;
    check(l, m, fsck);
}

/**
 * Run case `exfat`: the volume made by the host's own exFAT formatter
 * through fstool; the file written, and read back in a second boot; a write
 * past the format's limit, the largest offset a seek can name; and the
 * host's own exFAT checker on the volume after the machine.
 */
static void exfat(struct lc *l)
{
    char *path = disk_path(l);
    char *make[] = {"fstool", "exfat-mkfs", path, "2", EXFAT_START, EXFAT_COUNT, "LARGEX", NULL};
    char *fsck[] = {"fstool", "exfat-fsck", path, EXFAT_START, EXFAT_COUNT, NULL};
    struct etude_case_machine *m;
    uint64_t at;

    if (path == NULL)
        return;
    volume(l, path, make, "fstool-mkfs", NULL);
    write_file(l, "/dev/disk0p2", "exfat");
    if ((m = verify(l, "/dev/disk0p2", "exfat", &at)) == NULL)
        return;
    limit(l, m, "check", "/dev/disk0p2 exfat 9223372036854775807",
          "exfat: write: offset past the format's limit", &at);
    check(l, m, fsck);
}

/**
 * Run case `fat32`: the volume made by mformat, and a write at the last
 * byte of 4 GiB, past the format's limit.
 */
static void fat32(struct lc *l)
{
    char *path = disk_path(l), at_offset[PATH_LEN];
    char *make[] = {"mformat",   "-i", at_offset,  "-F", "-T",
                    FAT_SECTORS, "-v", "LARGEFAT", "::", NULL};
    struct etude_case_machine *m;
    uint64_t at;

    if (path == NULL)
        return;
    snprintf(at_offset, sizeof(at_offset), "%s@@" FAT_OFFSET, path);
    volume(l, path, make, "mformat", NULL);
    if ((m = lc_shell(l, "boot", "fixture", &at)) == NULL)
        return;
    limit(l, m, "fixture", "/dev/disk0p3 vfat 4294967295",
          "vfat: write: offset 4095 MiB is past the format's limit of 4 GiB", &at);
    lc_quit(l, m, NULL);
}

int etude_scenario(struct etude_case *c)
{
    static const char *const ext2_ids[] = {"volume",    "write-exit",  "write-ok",  "verify-exit",
                                           "verify-ok", "sparse-exit", "sparse-ok", "limit-exit",
                                           "limit-ok",  "limit-named", "fsck",      NULL};
    static const char *const exfat_ids[] = {"volume",    "write-exit", "write-ok", "verify-exit",
                                            "verify-ok", "limit-exit", "limit-ok", "limit-named",
                                            "fsck",      NULL};
    static const char *const fat32_ids[] = {"volume", "limit-exit", "limit-ok", "limit-named",
                                            NULL};
    static const char *const cache_ids[] = {"volume", "cache-exit", "cache-ok", "fsck", NULL};
    const char *name = case_name(c);
    struct lc l;

    if (strcmp(name, "ext2") == 0) {
        lc_begin(&l, c, ext2_ids);
        ext2(&l);
    } else if (strcmp(name, "exfat") == 0) {
        lc_begin(&l, c, exfat_ids);
        exfat(&l);
    } else if (strcmp(name, "fat32") == 0) {
        lc_begin(&l, c, fat32_ids);
        fat32(&l);
    } else if (strcmp(name, "cache") == 0) {
        lc_begin(&l, c, cache_ids);
        cache(&l);
    } else {
        return 1;
    }
    return lc_finish(&l);
}
