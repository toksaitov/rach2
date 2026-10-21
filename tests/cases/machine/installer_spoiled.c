/*
 * installer_spoiled.c: the scenario of machine.installer.spoiled: a stick
 * whose image is not the one its manifest describes. One digit of the hash
 * in the manifest of a copy of the test stick is changed on the host, which
 * is what a stick damaged in the writing or in a pocket looks like from the
 * machine's side; the installer must refuse it and write nothing to the
 * patterned disk beside it.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib_installer.h"

/* The size of the patterned disk, as the old suite made it. */
#define DISK_BYTES 268435456u

/* The exchange at the stick's console, up to the refusal. */
static const struct inst_line spoiled[] = {
    {0, "recovery image"},
    {0, "# "},
    {1, "rach2-install"},
    {0, "disk shall Rach2 be written to"},
    {1, "/dev/disk0"},
    {0, "the password for root"},
    {1, INST_NEWPASS},
    {0, "again"},
    {1, INST_NEWPASS},
    {0, "The name of the first account"},
    {1, INST_USER},
    {0, "the password for " INST_USER},
    {1, INST_USERPASS},
    {0, "again"},
    {1, INST_USERPASS},
    {0, "uthorized keys"},
    {1, ""},
    {0, "key for the recovery system"},
    {1, ""},
    {0, "row the root"},
    {1, "no"},
    {0, "to go on, or stop to stop"},
    {1, "/dev/disk0"},
    {0, "and the manifest says"},
    {0, "# "},
    {1, "echo BADHASH\"\"DONE"},
    {0, "BADHASHDONE"},
    {0, NULL},
};

/** Say whether the `len` bytes at `p` have `image ` in them. */
static int image_line(const char *p, size_t len)
{
    size_t k;

    for (k = 0; k + 6 <= len; k++)
        if (memcmp(p + k, "image ", 6) == 0)
            return 1;
    return 0;
}

/**
 * Change one digit of the hash that the manifest on stick image `stick`
 * declares for the disk image of `bytes` bytes. The manifest is a few lines
 * of text in the volume, `image NAME`, `bytes N` and `sha256 HEX`, found by
 * its `bytes` and `sha256` lines after an `image` line, since the word
 * `sha256` appears in the programs the volume carries as well. The first
 * digit becomes `b` if it was `a`, and `a` otherwise.
 *
 * @return 0, or -1 with `why` saying what was not found or not written.
 */
static int spoil_manifest(const char *stick, uint64_t bytes, char *why, size_t size)
{
    static char buf[(1 << 20) + 256];
    char needle[64], now;
    size_t n, keep, have = 0, i;
    uint64_t base = 0;
    ssize_t r;
    int fd;

    n = (size_t)snprintf(needle, sizeof(needle), "\nbytes %" PRIu64 "\nsha256 ", bytes);
    keep = n + 128;
    if (stick == NULL || (fd = open(stick, O_RDWR | O_CLOEXEC)) < 0) {
        snprintf(why, size, "the copy of the stick cannot be opened");
        return -1;
    }
    for (;;) {
        r = read(fd, buf + have, sizeof(buf) - have);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        have += (size_t)r;
        for (i = 128; i + n < have; i++) {
            if (memcmp(buf + i, needle, n) != 0 || !image_line(buf + i - 128, 128))
                continue;
            now = buf[i + n] == 'a' ? 'b' : 'a';
            if (pwrite(fd, &now, 1, (off_t)(base + i + n)) != 1) {
                close(fd);
                snprintf(why, size, "the manifest's hash could not be written");
                return -1;
            }
            return close(fd);
        }
        if (have >= keep) {
            memmove(buf, buf + have - keep, keep);
            base += have - keep;
            have = keep;
        }
    }
    close(fd);
    snprintf(why, size, "no manifest of a %" PRIu64 "-byte image is on the stick", bytes);
    return -1;
}

/**
 * Spoil the manifest of the stick's copy, boot it beside the patterned disk,
 * run the installer to its refusal, and compare the disk with what it was.
 */
int etude_scenario(struct etude_case *c)
{
    const char *image = etude_artifact(c, "image");
    char why[200] = "", before[65] = "", after[65] = "";
    struct stat st;
    struct drive d;
    int spoiled_ok;

    if (etude_step(c, "spoil") != ETUDE_OK)
        return 1;
    spoiled_ok = image != NULL && stat(image, &st) == 0 &&
                 spoil_manifest(etude_private_copy(c, "teststick"), (uint64_t)st.st_size, why,
                                sizeof(why)) == 0;
    inst_assert(c, "spoiled", spoiled_ok, why[0] != '\0' ? why : "the test disk cannot be read");
    if (inst_pattern_disk(c, "four", DISK_BYTES, before) != 0)
        before[0] = '\0';

    if (etude_step(c, "install") != ETUDE_OK)
        return 1;
    if (!spoiled_ok || before[0] == '\0') {
        inst_fail(c, "refused",
                  spoiled_ok ? "the patterned disk could not be made"
                             : "the stick's manifest was not changed, so the stick was not booted");
        inst_fail(c, "untouched", "the stick was not booted");
        inst_finish();
        return 0;
    }
    drive_boot(&d, c, "stick", NULL);
    inst_exchange(&d, spoiled);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    inst_assert(c, "refused", !d.failed, d.why);
    if (file_digest(etude_private_copy(c, "four"), 0, UINT64_MAX, after) != 0)
        after[0] = '\0';
    inst_assert(c, "untouched", before[0] != '\0' && strcmp(before, after) == 0,
                "the disk was written from a stick whose image did not match its manifest");
    inst_finish();
    return 0;
}
