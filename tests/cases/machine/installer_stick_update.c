/*
 * installer_stick_update.c: the scenario of machine.installer.stick-update:
 * the product's install stick, its volume checked on the host and then
 * booted three times beside a copy of the sealed disk.
 *
 * The volume must have the root image byte for byte and a manifest that
 * describes the sealed disk. The first boot, by USB, mounts the volume and
 * sees both images. The second, by IDE, saves the disk's boot and recovery
 * partitions to the volume, updates the disk's root from the stick with
 * `rach2-install --update`, which must keep the machine's groups, replace
 * its files and arm the candidate for the next boot, and then zeroes both
 * partitions. The third writes the saved copies back. Both partitions must
 * then be as they were, the saved copies the same, and the volume clean.
 * A command that fails prints `STICKFAIL` and powers the machine off.
 */
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib_installer.h"

/* The name mkinstallstick.sh gives the root image on the volume, the name
 * of the build's rach2-metal-disk.ext2. */
#define ROOT_NAME "rach2-metal-disk.ext2"

/* The length of each partition the boots save and restore. */
#define PART_BYTES 67108864u
#define MIB        1048576u

/* What the steps share. */
struct state {
    char start[16];      /* the volume's first sector, in decimal */
    char image[128];     /* the sealed disk's name on the volume */
    uint64_t root_bytes; /* the root image's length */
    uint64_t at[2];      /* the boot and recovery partitions' offsets in MiB */
    char before[2][65];  /* their digests in the sealed disk */
    int ready;           /* the volume and the disk were read */
};

/**
 * Read file `path` off the stick image `stick`'s volume with
 * installer_volume.sh into the evidence `name`, give its SHA-256 in `hex`,
 * and remove it.
 *
 * @return 0, or -1 when the volume does not check clean or lacks the file.
 */
static int volume_digest(struct etude_case *c, const struct state *s, const char *stick,
                         const char *path, const char *name, char hex[65])
{
    char out[4096];
    char *argv[] = {"sh",
                    (char *)etude_artifact(c, "installer_volume"),
                    (char *)etude_artifact(c, "mkexfatfixture"),
                    (char *)stick,
                    (char *)s->start,
                    (char *)path,
                    out,
                    NULL};
    char *text;
    int status, got;

    snprintf(out, sizeof(out), "%s", etude_evidence(c, name));
    text = inst_host_text(c, argv, "volume.read", &status);
    free(text);
    got = status == 0 ? file_digest(out, 0, UINT64_MAX, hex) : -1;
    unlink(out);
    return got;
}

/** Give the line of the boot's console that says which command failed, or
 * the first call that did not complete. */
static void stick_failure(struct drive *d, char *buf, size_t size)
{
    char *text = drive_console(d);

    if (text == NULL || text_field(text, "STICKFAIL: ", 1, buf, size) != 0)
        snprintf(buf, size, "%s", d->why);
    free(text);
}

/**
 * Read the stick's volume and the sealed disk on the host: the root image
 * on the volume, the manifest, and the sealed disk's two partitions that the
 * boots save.
 */
static void read_volume(struct etude_case *c, struct state *s)
{
    const char *stick = etude_artifact(c, "stick"), *sealed = etude_artifact(c, "sealed");
    const char *root = etude_artifact(c, "root");
    char want[65] = "", got[65] = "", bytes[32] = "", sum[80] = "", name[128], detail[640];
    char *argv[] = {"mkexfatfixture",          "--check", (char *)stick, s->start, "--read",
                    "/rach2-install.manifest", NULL};
    unsigned char entry[4];
    uint64_t first, length;
    struct stat st;
    char *text;
    int fd, status, i, laid = 1;

    if (stick != NULL && (fd = open(stick, O_RDONLY | O_CLOEXEC)) >= 0) {
        if (pread(fd, entry, 4, 470) == 4)
            snprintf(s->start, sizeof(s->start), "%" PRIu32,
                     (uint32_t)entry[0] | (uint32_t)entry[1] << 8 | (uint32_t)entry[2] << 16 |
                         (uint32_t)entry[3] << 24);
        close(fd);
    }

    if (root != NULL && stat(root, &st) == 0)
        s->root_bytes = (uint64_t)st.st_size;
    i = root != NULL && file_digest(root, 0, UINT64_MAX, want) == 0 &&
        volume_digest(c, s, stick, "/" ROOT_NAME, "root.actual", got) == 0;
    inst_assert(c, "root-on-volume", i && strcmp(want, got) == 0,
                i ? "the root image on the volume differs" : "the volume has no root image");

    text = inst_host_text(c, argv, "manifest", &status);
    if (status == 0 && text != NULL) {
        text_field(text, "image ", 1, s->image, sizeof(s->image));
        text_field(text, "bytes ", 1, bytes, sizeof(bytes));
        text_field(text, "sha256 ", 1, sum, sizeof(sum));
    }
    free(text);
    if (inst_artifact_line(c, "sealed_name", name, sizeof(name)) != 0)
        name[0] = '\0';
    want[0] = '\0';
    if (sealed == NULL || stat(sealed, &st) != 0 || file_digest(sealed, 0, UINT64_MAX, want) != 0)
        st.st_size = 0;
    snprintf(detail, sizeof(detail),
             "the manifest names '%s' of %s bytes and hash %s; the sealed disk '%s' has %" PRIu64
             " bytes and hash %s",
             s->image, bytes, sum, name, (uint64_t)st.st_size, want);
    snprintf(got, sizeof(got), "%" PRIu64, (uint64_t)st.st_size);
    inst_assert(c, "manifest",
                name[0] != '\0' && strcmp(s->image, name) == 0 && want[0] != '\0' &&
                    strcmp(bytes, got) == 0 && strcmp(sum, want) == 0,
                detail);

    for (i = 0; i < 2; i++) {
        if (inst_gpt_entry(c, sealed, i + 1, &first, &length) != 0 || length != PART_BYTES ||
            first % MIB != 0 || file_digest(sealed, first, PART_BYTES, s->before[i]) != 0) {
            laid = 0;
            continue;
        }
        s->at[i] = first / MIB;
    }
    inst_assert(c, "layout", laid, "expected two aligned 64 MiB partitions in the sealed disk");
    s->ready = laid && s->image[0] != '\0' && s->root_bytes > 0;
}

/** Boot by USB, mount the volume and look for both images on it. */
static int boot_usb(struct etude_case *c, struct state *s)
{
    char why[200];
    struct drive d;

    drive_boot(&d, c, "usb", NULL);
    if (!s->ready)
        inst_refuse(&d, "the stick's volume or the sealed disk could not be read");
    drive_expect(&d, "recovery image");
    drive_expect(&d, "# ");
    drive_send(&d,
               "mkdir -p /stick && mount -t exfat /dev/disk1p2 /stick && test -f /stick/%s && "
               "test $(stat -c %%s /stick/" ROOT_NAME ") = %" PRIu64 " && echo USB\"\"FILES || "
               "{ echo STICK\"\"FAIL: usb files; poweroff -f; }",
               s->image, s->root_bytes);
    drive_expect(&d, "USBFILES");
    drive_send(&d, "umount /stick && echo USB\"\"DONE || "
                   "{ echo STICK\"\"FAIL: usb unmount; poweroff -f; }");
    drive_expect(&d, "USBDONE");
    stick_failure(&d, why, sizeof(why));
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "usb", !d.failed, why);
    return 0;
}

/**
 * Boot by IDE: save both partitions to the volume, change the root, update
 * it from the stick, read what the update kept and armed, and zero both
 * partitions; then compare them on the host with the sealed disk's.
 */
static int boot_update(struct etude_case *c, struct state *s)
{
    const char *disk = etude_private_copy(c, "sealed");
    char why[200], now[65];
    struct drive d;
    int saved, updated, armed, i, damaged = 1;

    drive_boot(&d, c, "ide", NULL);
    if (!s->ready)
        inst_refuse(&d, "the stick's volume or the sealed disk could not be read");
    drive_expect(&d, "recovery image");
    drive_expect(&d, "# ");
    drive_send(&d, "mkdir -p /stick /old && mount -t exfat /dev/disk1p2 /stick && "
                   "echo STICK\"\"MOUNTED || { echo STICK\"\"FAIL: mount; poweroff -f; }");
    drive_expect(&d, "STICKMOUNTED");
    drive_send(&d, "df -k /stick; test $(df -k /stick | awk 'END {print $4}') -ge 131072 && "
                   "echo SPACE\"\"OK || { echo STICK\"\"FAIL: space; poweroff -f; }");
    drive_expect(&d, "SPACEOK");
    drive_send(&d,
               "mkdir /stick/saved && dd if=/dev/disk0 of=/stick/saved/boot.img bs=1M skip=%" PRIu64
               " count=64 && test $(stat -c %%s /stick/saved/boot.img) = 67108864 && "
               "echo BOOT\"\"SAVED || { echo STICK\"\"FAIL: boot save; poweroff -f; }",
               s->at[0]);
    drive_expect(&d, "BOOTSAVED");
    drive_send(&d,
               "dd if=/dev/disk0 of=/stick/saved/recovery.img bs=1M skip=%" PRIu64
               " count=64 && test $(stat -c %%s /stick/saved/recovery.img) = 67108864 && "
               "echo RECOVERY\"\"SAVED || { echo STICK\"\"FAIL: recovery save; poweroff -f; }",
               s->at[1]);
    drive_expect(&d, "RECOVERYSAVED");
    saved = !d.failed;
    drive_send(&d, "mount -t ext2 /dev/disk0p3 /old && echo stickproof:x:1234: >> /old/etc/group "
                   "&& echo old-root > /old/stick-before && umount /old && echo ROOT\"\"CHANGED || "
                   "{ echo STICK\"\"FAIL: root setup; poweroff -f; }");
    drive_expect(&d, "ROOTCHANGED");
    if (!d.failed)
        etude_progress(c, "update", 0);
    drive_send(&d, "rach2-install --update /stick/" ROOT_NAME " /dev/disk0 && echo UPDATE\"\"OK || "
                   "{ echo STICK\"\"FAIL: update; poweroff -f; }");
    drive_expect(&d, "UPDATEOK");
    if (!d.failed)
        etude_progress(c, "update", 1);
    drive_send(&d, "mount -t ext2 /dev/disk0p3 /old && test ! -e /old/stick-before && "
                   "grep -qx stickproof:x:1234: /old/etc/group && umount /old && "
                   "echo ROOT\"\"CHECKED || { echo STICK\"\"FAIL: root check; poweroff -f; }");
    drive_expect(&d, "ROOTCHECKED");
    updated = !d.failed;
    drive_send(&d, "bootctl --target /dev/disk0p1 status");
    drive_expect(&d, "saved_entry: recovery");
    drive_expect(&d, "next_entry: candidate");
    armed = !d.failed;
    drive_send(&d,
               "dd if=/dev/zero of=/dev/disk0 bs=1M seek=%" PRIu64
               " count=64 conv=notrunc && dd if=/dev/zero of=/dev/disk0 bs=1M seek=%" PRIu64
               " count=64 conv=notrunc && sync && umount /stick && echo DAMAGE\"\"DONE || "
               "{ echo STICK\"\"FAIL: damage; poweroff -f; }",
               s->at[0], s->at[1]);
    drive_expect(&d, "DAMAGEDONE");
    stick_failure(&d, why, sizeof(why));
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "saved", saved, why);
    inst_assert(c, "update", updated, why);
    inst_assert(c, "armed", armed, why);
    for (i = 0; i < 2; i++)
        if (d.failed || file_digest(disk, s->at[i] * MIB, PART_BYTES, now) != 0 ||
            strcmp(now, s->before[i]) == 0)
            damaged = 0;
    inst_assert(c, "damaged", damaged, d.failed ? why : "a partition was not damaged");
    return 0;
}

/** Boot by IDE again and write the saved copies back. */
static int boot_restore(struct etude_case *c, struct state *s)
{
    char why[200];
    struct drive d;

    drive_boot(&d, c, "ide", NULL);
    if (!s->ready)
        inst_refuse(&d, "the stick's volume or the sealed disk could not be read");
    drive_expect(&d, "recovery image");
    drive_expect(&d, "# ");
    drive_send(&d,
               "mkdir -p /stick && mount -t exfat /dev/disk1p2 /stick && reflash /dev/disk0 "
               "%" PRIu64 " 67108864 < /stick/saved/boot.img && echo BOOT\"\"RESTORED || "
               "{ echo STICK\"\"FAIL: boot restore; poweroff -f; }",
               s->at[0] * MIB);
    drive_expect(&d, "BOOTRESTORED");
    drive_send(&d,
               "reflash /dev/disk0 %" PRIu64 " 67108864 < /stick/saved/recovery.img && sync && "
               "umount /stick && echo RESTORE\"\"DONE || "
               "{ echo STICK\"\"FAIL: recovery restore; poweroff -f; }",
               s->at[1] * MIB);
    drive_expect(&d, "RESTOREDONE");
    stick_failure(&d, why, sizeof(why));
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "restore", !d.failed, why);
    return 0;
}

/**
 * Compare both partitions and the copies saved on the volume with the
 * sealed disk's, and check the volume whole.
 */
static void judge(struct etude_case *c, struct state *s)
{
    const char *disk = etude_private_copy(c, "sealed"), *stick = etude_private_copy(c, "stick");
    static const char *const saved[2] = {"/saved/boot.img", "/saved/recovery.img"};
    char *argv[] = {"mkexfatfixture", "--check", (char *)stick, s->start, NULL};
    char now[65], detail[200] = "";
    int restored = s->ready, kept = s->ready, i, status;
    char *text;

    for (i = 0; s->ready && i < 2; i++) {
        if (file_digest(disk, s->at[i] * MIB, PART_BYTES, now) != 0 ||
            strcmp(now, s->before[i]) != 0)
            restored = 0;
        if (volume_digest(c, s, stick, saved[i], "saved.actual", now) != 0 ||
            strcmp(now, s->before[i]) != 0) {
            kept = 0;
            snprintf(detail, sizeof(detail), "partition %d save differs", i + 1);
        }
    }
    inst_assert(c, "restored", restored,
                s->ready ? "a restored partition differs" : "the volume could not be read");
    inst_assert(c, "saves", kept, s->ready ? detail : "the volume could not be read");
    text = inst_host_text(c, argv, "final.check", &status);
    inst_assert(c, "volume-clean", status == 0, text != NULL ? text : "the check did not run");
    free(text);
}

/** Run the steps in order. */
int etude_scenario(struct etude_case *c)
{
    struct state s;

    memset(&s, 0, sizeof(s));
    if (etude_step(c, "volume") != ETUDE_OK)
        return 1;
    read_volume(c, &s);
    if (etude_step(c, "usb") != ETUDE_OK || boot_usb(c, &s) != 0)
        return 1;
    if (etude_step(c, "update") != ETUDE_OK || boot_update(c, &s) != 0)
        return 1;
    if (etude_step(c, "restore") != ETUDE_OK || boot_restore(c, &s) != 0)
        return 1;
    if (etude_step(c, "judge") != ETUDE_OK)
        return 1;
    judge(c, &s);
    inst_finish();
    return 0;
}
