/*
 * installer_rehearse_sitting.c: the scenario of
 * machine.installer-rehearse.sitting, the Lenovo's install sitting rehearsed
 * on a disk of 256 MiB: the save of the save set, the install with the grow,
 * and the restore, each boot's writes judged against the set by the map of
 * the qcow2 overlay the target is.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drive.h"
#include "sha256.h"

#define RANGES_MAX 256

/* The save set, as the fixture's `ranges` file gives it, in sectors. */
static struct range {
    uint64_t start, count;
} ranges[RANGES_MAX];
static int nranges;

/* The root's first sector and the target's length, from the fixture's
 * `params` file. */
static uint64_t root_start, disk_sectors;

/** Give the line after the one at `line`, or NULL at the end of the text. */
static char *next_line(char *line)
{
    char *eol = strchr(line, '\n');

    return eol != NULL && eol[1] != '\0' ? eol + 1 : NULL;
}

/** Read the fixture's ranges and parameters; return 0, or -1. */
static int read_fixture(struct etude_case *c)
{
    const char *rpath = etude_artifact(c, "ranges"), *ppath = etude_artifact(c, "params");
    unsigned long long a, b;
    char name[32];
    FILE *f;

    if (rpath == NULL || ppath == NULL || (f = fopen(rpath, "r")) == NULL)
        return -1;
    while (nranges < RANGES_MAX && fscanf(f, "%llu %llu", &a, &b) == 2) {
        ranges[nranges].start = a;
        ranges[nranges].count = b;
        nranges++;
    }
    fclose(f);
    if ((f = fopen(ppath, "r")) == NULL)
        return -1;
    while (fscanf(f, "%31s %llu", name, &a) == 2) {
        if (strcmp(name, "root_start") == 0)
            root_start = a;
        else if (strcmp(name, "disk_sectors") == 0)
            disk_sectors = a;
    }
    fclose(f);
    return nranges > 0 && root_start > 0 && disk_sectors > root_start + 33 ? 0 : -1;
}

/**
 * Boot the machine with its three disks and wait for the recovery system's
 * shell, which the stick boots with nothing typed.
 */
static void boot(struct drive *d, struct etude_case *c)
{
    drive_boot(d, c, "sitting", NULL);
    drive_expect(d, "recovery image");
    drive_expect(d, "# ");
}

/** Say whether a line of the boot's console begins with `prefix`. */
static int console_has_line(const struct drive *d, const char *prefix)
{
    char *text = drive_console(d), rest[8];
    int found = text != NULL && text_field(text, prefix, 1, rest, sizeof(rest)) == 0;

    free(text);
    return found;
}

/**
 * Mount the save disk, the second fixed disk as /proc/disk lists them, on
 * /save. The loops find the target and the save disk by that list, not by the
 * numbers the kernel gave them.
 */
static void mount_save(struct drive *d)
{
    drive_send(d,
               "v=$(grep ^identity /proc/disk | grep \" fixed \" | sed -n 2p | cut -d\" \" -f2); "
               "mkdir -p /save && mount -t exfat /dev/disk${v}p1 /save && echo SAVE\"\"MOUNTED");
    drive_expect(d, "SAVEMOUNTED");
}

/**
 * Save each range of the target into a file of its own on the save disk.
 *
 * The input block is the largest the range's alignment allows, since dd is a
 * system call per block; the output block is a mebibyte, since the file
 * system flushes the disk at every write call.
 */
static void save(struct etude_case *c)
{
    char want[32];
    struct drive d;

    boot(&d, c);
    mount_save(&d);
    drive_send(&d, "d=$(grep ^identity /proc/disk | grep \" fixed \" | head -1 | cut -d\" \" -f2)");
    drive_send(&d, "unit() { u=1; for t in 2048 8; do if [ $(($1 %% t)) = 0 ] && "
                   "[ $(($2 %% t)) = 0 ] && [ $u = 1 ]; then u=$t; fi; done; }");
    drive_send(&d,
               "i=0; while read s c; do unit $s $c; dd if=/dev/disk$d of=/save/range-$i "
               "ibs=$((u * 512)) obs=1M skip=$((s / u)) count=$((c / u)) 2>/dev/null || "
               "echo SAVEFAIL $i; i=$((i + 1)); done < /save/ranges; sync; echo SAVED\"\"ALL $i");
    snprintf(want, sizeof(want), "SAVEDALL %d", nranges);
    drive_expect(&d, want);
    drive_send(&d, "umount /save && echo SAVE\"\"DONE");
    drive_expect(&d, "SAVEDONE");
    drive_stop(&d);
    if (d.failed)
        etude_assert(c, "saved", 0, d.why);
    else
        etude_assert(c, "saved", !console_has_line(&d, "SAVEFAIL "), "a range could not be read");
}

/** Install from the stick onto the target, the first disk, with the root
 * grown into the rest of it. */
static void install(struct etude_case *c)
{
    static const char *const dialogue[][2] = {
        {"disk shall Rach2 be written to", "/dev/disk0"},
        {"the password for root", "an-installed-machine"},
        {"again", "an-installed-machine"},
        {"The name of the first account", "alice"},
        {"the password for alice", "alices-own-password"},
        {"again", "alices-own-password"},
        {"uthorized keys", ""},
        {"key for the recovery system", ""},
        {"row the root", ""},
        {"to go on, or stop to stop", "/dev/disk0"},
    };
    static const char *const said[] = {
        "the image on the stick is the one the manifest describes",
        "what reached the disk is the image",
        "the table is the one the image carries",
        "the accounts are root and alice",
        "growing the root into the rest of the disk",
        "installed:",
    };
    struct drive d;
    size_t i;

    boot(&d, c);
    drive_send(&d, "rach2-install");
    for (i = 0; i < sizeof(dialogue) / sizeof(dialogue[0]); i++) {
        drive_expect(&d, dialogue[i][0]);
        drive_send(&d, "%s", dialogue[i][1]);
    }
    for (i = 0; i < sizeof(said) / sizeof(said[0]); i++)
        drive_expect(&d, said[i]);
    drive_send(&d, "echo INSTALL\"\"DONE");
    drive_expect(&d, "INSTALLDONE");
    drive_stop(&d);
    etude_assert(c, "installed", !d.failed, d.why);
}

/**
 * Judge the overlay's map: growset lists each written extent outside the
 * grow's own ranges as `outside START COUNT`, and each must lie inside the
 * save set, which also has the image's extent and the disk's end.
 *
 * @return 1 when every write lies in the set, 0 when one does not, with the
 *         reason in `why`; `wrote` says whether the map lists any extent.
 */
static int judge_map(struct etude_case *c, const char *tag, int *wrote, char *why, size_t size)
{
    char map_name[64], check_name[64], lba[24], length[24], *map, *check, *line;
    char *qemu_img[] = {"qemu-img", "map", "--output=json", NULL, NULL};
    char *growset[] = {"growset", "--lba", lba, "--map", NULL, NULL, length, NULL};
    unsigned long long start, count;
    uint64_t x;
    int status, i, inside = 1;

    *wrote = 0;
    snprintf(map_name, sizeof(map_name), "%s.json", tag);
    snprintf(check_name, sizeof(check_name), "%s.check", tag);
    qemu_img[3] = (char *)etude_private_copy(c, "target");
    if (qemu_img[3] == NULL || drive_host(c, qemu_img, map_name) != 0 ||
        (map = evidence_text(c, map_name)) == NULL) {
        snprintf(why, size, "qemu-img did not map the overlay");
        return 0;
    }
    *wrote = strstr(map, "\"start\"") != NULL;
    free(map);
    snprintf(lba, sizeof(lba), "%" PRIu64, root_start);
    snprintf(length, sizeof(length), "%" PRIu64, disk_sectors - 33 - root_start);
    growset[4] = (char *)etude_evidence(c, map_name);
    growset[5] = (char *)etude_artifact(c, "head");
    status = drive_host(c, growset, check_name);
    check = evidence_text(c, check_name);
    if (check == NULL || (status != 0 && strstr(check, "outside ") == NULL)) {
        snprintf(why, size, "growset failed on the map, status %d", status);
        free(check);
        return 0;
    }
    for (line = check; line != NULL; line = next_line(line)) {
        if (sscanf(line, "outside %llu %llu", &start, &count) != 2)
            continue;
        for (x = start, i = 0; i < nranges; i++)
            if (ranges[i].start <= x && x < ranges[i].start + ranges[i].count)
                x = ranges[i].start + ranges[i].count;
        if (x < start + count) {
            snprintf(why, size, "sector %" PRIu64 " was written outside the save set", x);
            inside = 0;
            break;
        }
    }
    free(check);
    return inside;
}

/** Put each saved range back, read each back into /save/verify, and unmount. */
static void restore(struct etude_case *c)
{
    char want[32];
    struct drive d;

    boot(&d, c);
    mount_save(&d);
    drive_send(&d, "d=$(grep ^identity /proc/disk | grep \" fixed \" | head -1 | cut -d\" \" -f2); "
                   "i=0; while read s c; do reflash /dev/disk$d $((s * 512)) $((c * 512)) "
                   "< /save/range-$i || echo PUTFAIL $i; i=$((i + 1)); done < /save/ranges; "
                   "echo PUT\"\"ALL $i");
    snprintf(want, sizeof(want), "PUTALL %d", nranges);
    drive_expect(&d, want);
    drive_send(&d, ": > /save/verify; while read s c; do reflash --verify /dev/disk$d $((s * 512)) "
                   "$((c * 512)) >> /save/verify; done < /save/ranges; sync; echo VERIFY\"\"ALL");
    drive_expect(&d, "VERIFYALL");
    drive_send(&d, "umount /save && echo RESTORE\"\"DONE");
    drive_expect(&d, "RESTOREDONE");
    drive_stop(&d);
    if (d.failed)
        etude_assert(c, "restored", 0, d.why);
    else
        etude_assert(c, "restored", !console_has_line(&d, "PUTFAIL "),
                     "a range could not be written back");
}

/** Give in `hex` the digest of range `r` as the target held it when it was
 * saved: its mark, and zeros to its length. */
static void saved_digest(const struct range *r, char hex[65])
{
    static const unsigned char zeros[65536];
    unsigned char sum[32];
    char mark[64];
    struct sha256 h;
    uint64_t left;
    size_t n =
        (size_t)snprintf(mark, sizeof(mark), "range %" PRIu64 " %" PRIu64 "\n", r->start, r->count);

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)mark, n);
    for (left = r->count * 512 - n; left > 0; left -= n) {
        n = left < sizeof(zeros) ? (size_t)left : sizeof(zeros);
        sha256_update(&h, zeros, n);
    }
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
}

/** Judge the digests the machine read back against the saved bytes'. */
static void judge_readback(struct etude_case *c)
{
    char *read_back[] = {"mkexfatfixture", "--check", NULL, "2048", "--read", "/verify", NULL};
    char field[80], hex[65], why[200] = "", *text, *line;
    int i = 0;

    read_back[2] = (char *)etude_private_copy(c, "save");
    if (read_back[2] == NULL || drive_host(c, read_back, "verify") != 0 ||
        (text = evidence_text(c, "verify")) == NULL) {
        etude_assert(c, "readback", 0, "the digests could not be read off the save disk");
        return;
    }
    for (line = text; line != NULL && why[0] == '\0'; line = next_line(line)) {
        if (sscanf(line, "%*s %*s %79s", field) != 1) {
            snprintf(why, sizeof(why), "digest line %d has no digest in its third field", i + 1);
            break;
        }
        if (i == nranges) {
            snprintf(why, sizeof(why), "more digests than the %d ranges", nranges);
            break;
        }
        saved_digest(&ranges[i], hex);
        if (strcmp(field, hex) != 0)
            snprintf(why, sizeof(why), "range %d at sector %" PRIu64 " read back as %.16s", i,
                     ranges[i].start, field);
        i++;
    }
    if (why[0] == '\0' && i != nranges)
        snprintf(why, sizeof(why), "%d digests for %d ranges", i, nranges);
    free(text);
    etude_assert(c, "readback", why[0] == '\0', why);
}

int etude_scenario(struct etude_case *c)
{
    char why[200] = "the fixture's ranges or parameters cannot be read";
    int ok = read_fixture(c) == 0, wrote = 0, inside;

    if (etude_step(c, "save") != ETUDE_OK)
        return 1;
    if (ok)
        save(c);
    else
        etude_assert(c, "saved", 0, why);
    if (drive_halted() || etude_step(c, "install") != ETUDE_OK)
        return 1;
    if (ok)
        install(c);
    else
        etude_assert(c, "installed", 0, why);
    if (drive_halted() || etude_step(c, "install-map") != ETUDE_OK)
        return 1;
    inside = ok && judge_map(c, "after-install", &wrote, why, sizeof(why));
    etude_assert(c, "install-wrote", wrote,
                 "the overlay's map lists no extent, and the install wrote");
    etude_assert(c, "install-inside", inside, why);
    if (drive_halted() || etude_step(c, "restore") != ETUDE_OK)
        return 1;
    if (ok)
        restore(c);
    else
        etude_assert(c, "restored", 0, why);
    if (drive_halted() || etude_step(c, "restore-check") != ETUDE_OK)
        return 1;
    if (ok)
        judge_readback(c);
    else
        etude_assert(c, "readback", 0, why);
    inside = ok && judge_map(c, "after-restore", &wrote, why, sizeof(why));
    etude_assert(c, "restore-inside", inside, why);
    return drive_halted();
}
