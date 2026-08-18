/*
 * bootctl: manage the boot partition of a machine nobody will open again.
 *
 * The name is systemd's, because that tool manages a boot partition's kernels
 * and defaults on most Linux machines and the knowledge transfers. The verbs
 * are this project's own trial and commit vocabulary from milestone 23.
 *
 *     bootctl status         what is installed, committed, and booting next
 *     bootctl kernel FILE    stage a kernel into the candidate slot
 *     bootctl commit         make the booted trial the saved default
 *
 * `kernel` validates the named file and the machine's fallback kernel, writes
 * `saved_entry=fallback`, writes the candidate slot, reads it back byte for
 * byte, and sets `next_entry=candidate`, so exactly one reboot tries the new
 * kernel and a power cycle after a crash returns to the fallback. Once it has
 * proved itself, `commit` makes it the saved default.
 *
 * `status` is the only window onto this disk. It prints the three files'
 * sector extents, what the environment block holds, whether a trial is armed
 * or consumed, and which slot this boot came from. Its extent lines are a
 * test contract: `tools/install_test.sh` reads the candidate's first sector
 * from them to corrupt it, so their field order is fixed.
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
#include "bootverb.h"

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
        fprintf(stderr, "bootctl: this machine has no boot partition "
                        "(no /dev/bootpart)\n");
        return -1;
    }
    io.read = dev_read;
    io.write = dev_write;
    io.ctx = NULL;
    if (bootslot_open(&bs, &io, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return -1;
    }
    return 0;
}

static void print_extent(const char *name, const struct bootslot_file *f, uint32_t cluster_sectors,
                         uint32_t data_start)
{
    uint32_t first = data_start + (f->chain[0] - 2) * cluster_sectors;

    printf("bootctl: %s: first sector %u, %u sectors in %u clusters, "
           "%u bytes\n",
           name, first, f->chain_len * cluster_sectors, f->chain_len, f->size);
}

/**
 * Print one environment value, or a stated absence.
 *
 * The block is 1024 bytes of `key=value` lines with no checksum, so a torn
 * write still parses. `trial_marker` says whether an empty value carries the
 * consumed-trial meaning `bootslot_commit` looks for, which is true of
 * `next_entry` and of nothing else: an empty `saved_entry` says only that the
 * key is there with no value.
 */
static void print_env(const uint8_t *env, const char *key, int trial_marker)
{
    char pat[32];
    const char *p = (const char *)env;
    const char *end = p + BOOTSLOT_ENV_BYTES;
    size_t klen;

    pat[0] = '\n';
    strcpy(pat + 1, key);
    strcat(pat, "=");
    klen = strlen(pat);

    while (p < end) {
        if ((size_t)(end - p) >= klen && memcmp(p, pat, klen) == 0) {
            const char *v = p + klen;
            const char *e = v;

            while (e < end && *e != '\n' && *e != '\0') {
                e++;
            }
            if (e == v) {
                if (trial_marker) {
                    printf("bootctl: %s: empty (a trial was consumed)\n", key);
                } else {
                    printf("bootctl: %s: empty\n", key);
                }
            } else {
                printf("bootctl: %s: %.*s\n", key, (int)(e - v), v);
            }
            return;
        }
        p++;
    }
    printf("bootctl: %s: absent\n", key);
}

/** Report the slot this boot came from, which is menu text, not a kernel flag. */
static void print_booted(void)
{
    if (cmdline_has("rach2.slot=candidate")) {
        printf("bootctl: booted: candidate\n");
    } else if (cmdline_has("rach2.slot=fallback")) {
        printf("bootctl: booted: fallback\n");
    } else {
        printf("bootctl: booted: not from a slot (no rach2.slot= on /proc/cmdline)\n");
    }
}

static int cmd_status(void)
{
    static uint8_t env[BOOTSLOT_ENV_BYTES];

    if (open_slot() != 0) {
        return 1;
    }
    print_extent("candidate", &bs.kernel, bs.cluster_sectors, bs.data_start);
    print_extent("fallback", &bs.fallback, bs.cluster_sectors, bs.data_start);
    print_extent("grubenv", &bs.grubenv, bs.cluster_sectors, bs.data_start);

    if (bootslot_read_env(&bs, env, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    print_env(env, "saved_entry", 0);
    print_env(env, "next_entry", 1);
    print_booted();
    return 0;
}

static int cmd_commit(void)
{
    if (!cmdline_has("rach2.slot=candidate")) {
        fprintf(stderr, "bootctl: only a machine running the candidate may "
                        "commit it (rach2.slot=candidate is not on "
                        "/proc/cmdline)\n");
        return 1;
    }
    if (open_slot() != 0) {
        return 1;
    }
    if (bootslot_commit(&bs, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: committed: the candidate is the saved default\n");
    return 0;
}

static int cmd_kernel(const char *path)
{
    FILE *f;
    size_t size;

    if (!cmdline_has("rach2.slot=candidate") && !cmdline_has("rach2.slot=fallback")) {
        fprintf(stderr, "bootctl: this boot did not come from a kernel slot "
                        "(no rach2.slot= on /proc/cmdline)\n");
        return 1;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bootctl: cannot open '%s'\n", path);
        return 1;
    }
    size = fread(image, 1, sizeof(image), f);
    if (ferror(f)) {
        fclose(f);
        fprintf(stderr, "bootctl: reading '%s' failed\n", path);
        return 1;
    }
    if (size == sizeof(image) && fgetc(f) != EOF) {
        fclose(f);
        fprintf(stderr, "bootctl: '%s' is larger than any slot\n", path);
        return 1;
    }
    fclose(f);
    if (open_slot() != 0) {
        return 1;
    }
    if (bootslot_install(&bs, image, (uint32_t)size, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "bootctl: %s\n", errbuf);
        return 1;
    }
    printf("bootctl: installed: %u bytes in the candidate slot; the next boot "
           "tries it once, and a failure falls back\n",
           (unsigned)size);
    printf("bootctl: run `bootctl commit` after the new kernel boots\n");
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: bootctl status\n");
    fprintf(stderr, "       bootctl kernel <file>\n");
    fprintf(stderr, "       bootctl commit\n");
}

/** Resolve the command line and run the verb it names. */
int main(int argc, char **argv)
{
    const char *arg;

    switch (bootverb_parse(argc, argv, &arg)) {
    case BOOTVERB_STATUS:
        return cmd_status();
    case BOOTVERB_COMMIT:
        return cmd_commit();
    case BOOTVERB_KERNEL:
        return cmd_kernel(arg);
    default:
        break;
    }

    if (argc >= 2 && !bootverb_known(argv[1])) {
        fprintf(stderr, "bootctl: not a command this tool takes: '%s'\n", argv[1]);
    }
    usage();
    return 1;
}
