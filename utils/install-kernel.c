/*
 * install-kernel: put a newly built kernel into the boot partition's
 * candidate slot, as a one-boot trial GRUB revokes on any failure.
 *
 * `install-kernel <kernel>` validates the named file and the machine's
 * fallback kernel, writes `saved_entry=fallback`, writes the candidate slot,
 * reads it back byte for byte, and sets `next_entry=candidate`, so exactly
 * one reboot tries the new kernel and a power cycle after a crash returns to
 * the fallback. Once the new kernel has proved itself, `install-kernel
 * --commit` makes it the saved default. `install-kernel --where` prints the
 * three files' sector extents (the harness corrupts the candidate through
 * them).
 *
 * The tool runs only on a machine that GRUB booted from a slot: it requires a
 * `rach2.slot=` token on /proc/cmdline before installing and
 * `rach2.slot=candidate` before committing, which also covers a QEMU -kernel
 * boot that happens to have a boot-partition disk attached. The device it
 * writes, /dev/bootpart, exists only on such a disk and opens for root alone.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bootslot.h"

#define SLOT_MAX (4 * 1024 * 1024)

static uint8_t image[SLOT_MAX];
static char errbuf[128];

/* The device fd behind the bootslot_io callbacks. */
static int devfd = -1;

static int dev_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    if (lseek(devfd, (off_t)lba * BOOTSLOT_SECTOR, SEEK_SET) < 0) {
        return -1;
    }
    return read(devfd, buf, BOOTSLOT_SECTOR) == BOOTSLOT_SECTOR ? 0 : -1;
}

static int dev_write(void *ctx, uint32_t lba, const void *buf)
{
    (void)ctx;
    if (lseek(devfd, (off_t)lba * BOOTSLOT_SECTOR, SEEK_SET) < 0) {
        return -1;
    }
    return write(devfd, buf, BOOTSLOT_SECTOR) == BOOTSLOT_SECTOR ? 0 : -1;
}

/** Whether /proc/cmdline carries `token` as a whole word. */
static int cmdline_has(const char *token)
{
    static char line[512];
    FILE *f = fopen("/proc/cmdline", "r");
    size_t n, tlen = strlen(token);
    char *p;

    if (!f) {
        return 0;
    }
    n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    line[n] = '\0';
    for (p = line; *p != '\0'; p++) {
        if ((p == line || p[-1] == ' ') && strncmp(p, token, tlen) == 0 &&
            (p[tlen] == ' ' || p[tlen] == '\n' || p[tlen] == '\0')) {
            return 1;
        }
    }
    return 0;
}

static struct bootslot bs;

static int open_slot(void)
{
    struct bootslot_io io;

    devfd = open("/dev/bootpart", O_RDWR);
    if (devfd < 0) {
        fprintf(stderr, "install-kernel: this machine has no boot partition "
                        "(no /dev/bootpart)\n");
        return -1;
    }
    io.read = dev_read;
    io.write = dev_write;
    io.ctx = NULL;
    if (bootslot_open(&bs, &io, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "install-kernel: %s\n", errbuf);
        return -1;
    }
    return 0;
}

static void print_extent(const char *name, const struct bootslot_file *f, uint32_t cluster_sectors,
                         uint32_t data_start)
{
    uint32_t first = data_start + (f->chain[0] - 2) * cluster_sectors;

    printf("install-kernel: %s: first sector %u, %u sectors in %u clusters, "
           "%u bytes\n",
           name, first, f->chain_len * cluster_sectors, f->chain_len, f->size);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: install-kernel <kernel> | --commit | --where\n");
        return 1;
    }

    if (strcmp(argv[1], "--where") == 0) {
        if (open_slot() != 0) {
            return 1;
        }
        print_extent("candidate", &bs.kernel, bs.cluster_sectors, bs.data_start);
        print_extent("fallback", &bs.fallback, bs.cluster_sectors, bs.data_start);
        print_extent("grubenv", &bs.grubenv, bs.cluster_sectors, bs.data_start);
        return 0;
    }

    if (strcmp(argv[1], "--commit") == 0) {
        if (!cmdline_has("rach2.slot=candidate")) {
            fprintf(stderr, "install-kernel: only a machine running the "
                            "candidate may commit it (rach2.slot=candidate "
                            "is not on /proc/cmdline)\n");
            return 1;
        }
        if (open_slot() != 0) {
            return 1;
        }
        if (bootslot_commit(&bs, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "install-kernel: %s\n", errbuf);
            return 1;
        }
        printf("install-kernel: committed: the candidate is the saved "
               "default\n");
        return 0;
    }

    {
        FILE *f;
        size_t size;

        if (!cmdline_has("rach2.slot=candidate") && !cmdline_has("rach2.slot=fallback")) {
            fprintf(stderr, "install-kernel: this boot did not come from a "
                            "kernel slot (no rach2.slot= on /proc/cmdline)\n");
            return 1;
        }
        f = fopen(argv[1], "rb");
        if (!f) {
            fprintf(stderr, "install-kernel: cannot open '%s'\n", argv[1]);
            return 1;
        }
        size = fread(image, 1, sizeof(image), f);
        if (ferror(f)) {
            fclose(f);
            fprintf(stderr, "install-kernel: reading '%s' failed\n", argv[1]);
            return 1;
        }
        if (size == sizeof(image) && fgetc(f) != EOF) {
            fclose(f);
            fprintf(stderr, "install-kernel: '%s' is larger than any slot\n", argv[1]);
            return 1;
        }
        fclose(f);
        if (open_slot() != 0) {
            return 1;
        }
        if (bootslot_install(&bs, image, (uint32_t)size, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "install-kernel: %s\n", errbuf);
            return 1;
        }
        printf("install-kernel: installed: %u bytes in the candidate slot; "
               "the next boot tries it once, and a failure falls back\n",
               (unsigned)size);
        printf("install-kernel: run `install-kernel --commit` after the new "
               "kernel boots\n");
        return 0;
    }
}
