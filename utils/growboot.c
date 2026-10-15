/**
 * growboot: write, from a backup of the boot partition, a GRUB environment
 * block whose saved entry is `recovery` and which has no `next_entry`.
 *
 * Usage: `growboot BOOT.IMG ENV.IMG`. The input must be the complete 64 MiB
 * partition. ENV.IMG must not exist. The boot-slot editor runs against a
 * two-sector memory overlay, so no write reaches BOOT.IMG. The output is the
 * 1024-byte environment and stdout reports its partition-relative sector.
 * Recovery has a 5 MiB RAM file system. Copying the complete boot image
 * there would fail even on a machine with ample RAM.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bootslot.h"

static struct bootslot slot;
static FILE *source;
static uint32_t first;   /* the environment's first sector, partition-relative */
static unsigned present; /* a bit for each environment sector in `environment` */
static unsigned char environment[BOOTSLOT_ENV_BYTES];

/** Read edited sectors from memory and every other sector from the backup. */
static int read_sector(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    if (lba >= first && lba - first < 2 && (present & (1u << (lba - first)))) {
        memcpy(buf, environment + (lba - first) * 512, 512);
        return 0;
    }
    return fseeko(source, (off_t)lba * 512, SEEK_SET) == 0 && fread(buf, 1, 512, source) == 512
               ? 0
               : -1;
}

/** Accept only the environment sectors, retaining both entirely in memory;
 *  -1 for any other sector. */
static int write_sector(void *ctx, uint32_t lba, const void *buf)
{
    (void)ctx;
    if (lba < first || lba - first >= 2) {
        return -1;
    }
    memcpy(environment + (lba - first) * 512, buf, 512);
    present |= 1u << (lba - first);
    return 0;
}

/**
 * Whether the environment of `s` is 1024 bytes in two consecutive sectors:
 * one cluster of two sectors or more, as on a boot partition of an MBR disk,
 * or two adjacent clusters of one sector, as on the EFI system partition of
 * a GUID table, which has one sector to a cluster.
 */
static int consecutive(const struct bootslot *s)
{
    const struct bootslot_file *env = &s->grubenv;

    if (env->size != BOOTSLOT_ENV_BYTES) {
        return 0;
    }
    if (s->cluster_sectors >= 2) {
        return env->chain_len == 1;
    }
    return s->cluster_sectors == 1 && env->chain_len == 2 && env->chain[1] == env->chain[0] + 1;
}

/** Check the backup, prepare both sectors and create a new file. Exits 0, 1
 *  on a failure and 2 on a usage error. */
int main(int argc, char **argv)
{
    struct bootslot_io io;
    struct stat st;
    char error[128];
    int fd;
    size_t done;

    if (argc != 3) {
        fprintf(stderr, "usage: growboot BOOT.IMG ENV.IMG\n");
        return 2;
    }
    source = fopen(argv[1], "rb");
    if (!source || fstat(fileno(source), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size != 67108864) {
        fprintf(stderr, "growboot: the input must be a complete 64 MiB regular file\n");
        return 1;
    }
    io.read = read_sector;
    io.write = write_sector;
    io.ctx = NULL;
    if (bootslot_open(&slot, &io, error, sizeof(error)) != 0) {
        fprintf(stderr, "growboot: %s\n", error);
        return 1;
    }
    if (!consecutive(&slot)) {
        fprintf(stderr, "growboot: the saved environment must occupy two consecutive sectors\n");
        return 1;
    }
    first = slot.data_start + (slot.grubenv.chain[0] - 2) * slot.cluster_sectors;
    if (first == 0 || first > 131070) {
        fprintf(stderr, "growboot: the saved environment is outside the boot partition\n");
        return 1;
    }
    if (bootslot_env_set(&slot, "saved_entry", "recovery", error, sizeof(error)) != 0 ||
        bootslot_env_drop(&slot, "next_entry", error, sizeof(error)) != 0) {
        fprintf(stderr, "growboot: %s\n", error);
        return 1;
    }
    if (present != 3) {
        fprintf(stderr, "growboot: both environment sectors were not prepared\n");
        return 1;
    }
    fclose(source);
    fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        perror("growboot: creating the new environment file");
        return 1;
    }
    done = 0;
    while (done < sizeof(environment)) {
        ssize_t n = write(fd, environment + done, sizeof(environment) - done);

        if (n <= 0) {
            fprintf(stderr, "growboot: writing the environment failed\n");
            close(fd);
            return 1;
        }
        done += (size_t)n;
    }
    if (close(fd) != 0) {
        perror("growboot: closing the environment file");
        return 1;
    }
    printf("growboot: environment: %lu\n", (unsigned long)first);
    return fflush(stdout) == 0 ? 0 : 1;
}
