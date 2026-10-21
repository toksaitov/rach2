/*
 * installer_stick.c: the scenario of machine.installer.stick: the product's
 * install stick boots through the firmware, with no kernel given to QEMU, and
 * its volume holds the sealed disk its manifest describes.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "drive.h"

/**
 * Boot the stick, read its manifest and the image's length at the console,
 * and judge them against the sealed disk on the host.
 *
 * Three lengths must agree: the image the host built, the length the
 * manifest declares and the length of the file on the volume. The image's
 * name is the manifest's, since it says whether the recovery system was
 * built with a key.
 */
int etude_scenario(struct etude_case *c)
{
    const char *sealed = etude_artifact(c, "sealed"), *stick;
    char image[128] = "", bytes[32] = "", sum[80] = "", size[32] = "", host[65], want[96];
    char detail[200];
    struct stat st;
    struct drive d;
    char *text = NULL;
    int ok;

    if (etude_step(c, "boot") != ETUDE_OK)
        return 1;
    stick = etude_private_copy(c, "stick");
    drive_boot(&d, c, "stick", NULL);
    drive_expect(&d, "recovery image");
    drive_expect(&d, "# ");
    drive_send(&d, "mkdir -p /mnt && mount -t exfat /dev/disk0p2 /mnt && echo MOUNT\"\"ED");
    drive_expect(&d, "MOUNTED");
    drive_send(&d, "cat /mnt/rach2-install.manifest; echo MANI\"\"FEST");
    drive_expect(&d, "MANIFEST");
    if (!d.failed && (text = drive_console(&d)) != NULL)
        text_field(text, "image ", 1, image, sizeof(image));
    free(text);
    text = NULL;
    drive_send(&d, "set -- `ls -l /mnt/%s`; echo SIZE\"\":$5", image);
    drive_expect(&d, "SIZE:");
    drive_send(&d, "umount /mnt && echo STICK\"\"DONE");
    drive_expect(&d, "STICKDONE");
    ok = !d.failed && image[0] != '\0';
    if (ok)
        text = drive_console(&d);
    drive_stop(&d);
    if (drive_halted())
        return 1;
    etude_assert(c, "boots", ok && text != NULL,
                 d.failed ? d.why : "the manifest names no image");

    if (etude_step(c, "judge") != ETUDE_OK) {
        free(text);
        return 1;
    }
    if (text != NULL) {
        text_field(text, "bytes ", 1, bytes, sizeof(bytes));
        text_field(text, "sha256 ", 1, sum, sizeof(sum));
        text_field(text, "SIZE:", 1, size, sizeof(size));
    }
    if (sealed == NULL || stat(sealed, &st) != 0 || file_digest(sealed, 0, UINT64_MAX, host) != 0) {
        etude_assert(c, "length", 0, "the sealed disk cannot be read");
        etude_assert(c, "hash", 0, "the sealed disk cannot be read");
    } else {
        snprintf(want, sizeof(want), "%" PRIu64, (uint64_t)st.st_size);
        snprintf(detail, sizeof(detail),
                 "the host built %s bytes, the manifest declares %s, the volume holds %s", want, bytes,
                 size);
        etude_assert(c, "length", strcmp(want, bytes) == 0 && strcmp(want, size) == 0, detail);
        etude_assert(c, "hash", strcmp(host, sum) == 0, "the manifest's hash is not the image's");
    }
    if (stick == NULL || stat(stick, &st) != 0) {
        etude_assert(c, "own-disk", 0, "the stick's copy cannot be read");
    } else {
        snprintf(want, sizeof(want), "usbdisk: disk0: mbr: %" PRIu64 " MiB, 3 partitions",
                 (uint64_t)st.st_size / 1048576);
        etude_assert(c, "own-disk", text != NULL && strstr(text, want) != NULL, want);
    }
    free(text);
    return 0;
}
