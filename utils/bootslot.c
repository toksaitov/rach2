/*
 * bootslot.c: the install-kernel core over caller-supplied sector I/O.
 *
 * The shape follows dev-log/23-install/plan.md. Nothing here trusts the
 * volume: the BPB is validated field by field with subtraction-based bounds,
 * the two FAT copies must be identical, every chain the namespace reaches is
 * walked under a visited bitmap (a cluster reached twice is a cross-link and
 * an error), a regular file's chain length must match its directory size, and
 * the three named files must exist as short directory entries. Only then does
 * an install write anything, in the order the interruption matrix documents,
 * so a power cut at any point leaves a machine that boots.
 */

#include "bootslot.h"

#include <string.h>

/* FAT32 on-disk constants. */
#define DIRENT_SIZE      32
#define ATTR_LFN         0x0F
#define ATTR_VOLUME      0x08
#define ATTR_DIR         0x10
#define FAT_ENTRY_MASK   0x0FFFFFFFu
#define FAT_EOC_MIN      0x0FFFFFF8u
#define ENV_BYTES        1024
#define ENV_HEADER       "# GRUB Environment Block\n"
#define MULTIBOOT_MAGIC  0x1BADB002u
#define MULTIBOOT_SEARCH 8192
#define DIR_DEPTH_MAX    4

/** Copy `msg` into the caller's error buffer, always NUL-terminated. */
static int fail(char *err, size_t errlen, const char *msg)
{
    size_t i;

    for (i = 0; msg[i] != '\0' && i + 1 < errlen; i++) {
        err[i] = msg[i];
    }
    if (errlen > 0) {
        err[i] = '\0';
    }
    return -1;
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** The FAT entry for `cluster`, from the copy held in memory. */
static uint32_t fat_entry(const struct bootslot *bs, uint32_t cluster)
{
    return le32(bs->fat + cluster * 4) & FAT_ENTRY_MASK;
}

/** Whether `cluster` names a legal data cluster on this volume. */
static int cluster_legal(const struct bootslot *bs, uint32_t cluster)
{
    return cluster >= 2 && cluster < bs->cluster_count + 2;
}

/** The first partition-relative sector of `cluster`. */
static uint32_t cluster_sector(const struct bootslot *bs, uint32_t cluster)
{
    return bs->data_start + (cluster - 2) * bs->cluster_sectors;
}

/** Mark `cluster` visited; -1 if it already was (a cross-linked chain). */
static int visit(struct bootslot *bs, uint32_t cluster)
{
    uint8_t bit = (uint8_t)(1u << (cluster % 8));

    if (bs->visited[cluster / 8] & bit) {
        return -1;
    }
    bs->visited[cluster / 8] |= bit;
    return 0;
}

/**
 * Walk the chain from `first`, marking every cluster visited and recording it
 * into `file` when one is given (the directories pass NULL and are bounded
 * the same way). The bound is the volume's own cluster count, so a loop that
 * somehow survives the visited bitmap still terminates.
 */
static int walk_chain(struct bootslot *bs, uint32_t first, struct bootslot_file *file,
                      uint32_t *len_out, char *err, size_t errlen)
{
    uint32_t cluster = first;
    uint32_t steps = 0;
    uint32_t len = 0;

    if (file) {
        file->chain_len = 0;
    }
    while (1) {
        if (!cluster_legal(bs, cluster)) {
            return fail(err, errlen, "a chain leaves the volume");
        }
        if (visit(bs, cluster) != 0) {
            return fail(err, errlen, "two chains share a cluster");
        }
        len++;
        if (file) {
            if (file->chain_len >= BOOTSLOT_MAX_FILE_CLUSTERS) {
                return fail(err, errlen, "a chain is longer than the walk holds");
            }
            file->chain[file->chain_len++] = cluster;
        }
        cluster = fat_entry(bs, cluster);
        if (cluster >= FAT_EOC_MIN) {
            if (len_out) {
                *len_out = len;
            }
            return 0;
        }
        if (cluster == 0 || cluster == 0x0FFFFFF7u) {
            return fail(err, errlen, "a chain runs into a free or bad entry");
        }
        if (++steps > bs->cluster_count) {
            return fail(err, errlen, "a chain does not terminate");
        }
    }
}

/** Read the `n`-th sector of `file`'s chain. */
static int file_read_sector(struct bootslot *bs, const struct bootslot_file *file, uint32_t n,
                            uint8_t *buf)
{
    uint32_t cluster = file->chain[n / bs->cluster_sectors];
    uint32_t within = n % bs->cluster_sectors;

    return bs->io.read(bs->io.ctx, cluster_sector(bs, cluster) + within, buf);
}

/** Write the `n`-th sector of `file`'s chain. */
static int file_write_sector(struct bootslot *bs, const struct bootslot_file *file, uint32_t n,
                             const uint8_t *buf)
{
    uint32_t cluster = file->chain[n / bs->cluster_sectors];
    uint32_t within = n % bs->cluster_sectors;

    return bs->io.write(bs->io.ctx, cluster_sector(bs, cluster) + within, buf);
}

/** Match a directory entry's 11-byte padded name case-insensitively. */
static int name11_eq(const uint8_t *entry, const char *name11)
{
    int i;

    for (i = 0; i < 11; i++) {
        uint8_t a = entry[i];
        uint8_t b = (uint8_t)name11[i];

        if (a >= 'a' && a <= 'z') {
            a = (uint8_t)(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = (uint8_t)(b - 'a' + 'A');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

/* The recursive scan's per-file result: where the three names were found. */
struct scan_state {
    struct bootslot_file dir;
    uint32_t kernel_cluster, kernel_size;
    uint32_t fallback_cluster, fallback_size;
    uint32_t grubenv_cluster, grubenv_size;
    int have_kernel, have_fallback, have_grubenv;
};

/**
 * Scan one directory chain: mark every child chain visited (so disjointness
 * is proved across the whole reachable namespace), remember the three named
 * files when this directory is their parent, and recurse into subdirectories.
 * `path` selects which names are expected here: 0 the root (BOOT), 1 /boot
 * (KERNEL, FALLBACK, GRUB), 2 /boot/grub (GRUBENV), 3 anything deeper.
 */
static int scan_dir(struct bootslot *bs, uint32_t first_cluster, int path, int depth,
                    struct scan_state *st, char *err, size_t errlen)
{
    struct bootslot_file dir;
    /* On the stack, not static: the recursion below must not clobber the
       sector this frame is still iterating. Depth is bounded, so the cost is
       a few sectors of stack. */
    uint8_t sector[BOOTSLOT_SECTOR];
    uint32_t s;
    int i;

    if (depth > DIR_DEPTH_MAX) {
        return fail(err, errlen, "the directory tree is deeper than the walk");
    }
    if (walk_chain(bs, first_cluster, &dir, NULL, err, errlen) != 0) {
        return -1;
    }
    for (s = 0; s < dir.chain_len * bs->cluster_sectors; s++) {
        if (file_read_sector(bs, &dir, s, sector) != 0) {
            return fail(err, errlen, "a directory sector does not read");
        }
        for (i = 0; i < BOOTSLOT_SECTOR / DIRENT_SIZE; i++) {
            const uint8_t *e = sector + i * DIRENT_SIZE;
            uint32_t cluster, size;
            int is_dir;

            if (e[0] == 0x00) {
                return 0; /* end of the directory */
            }
            if (e[0] == 0xE5 || e[0] == '.') {
                continue; /* deleted, or . and .. */
            }
            if ((e[11] & ATTR_LFN) == ATTR_LFN || (e[11] & ATTR_VOLUME)) {
                continue; /* long-name or volume-label entries */
            }
            cluster = (le16(e + 20) << 16) | le16(e + 26);
            size = le32(e + 28);
            is_dir = (e[11] & ATTR_DIR) != 0;

            if (cluster == 0) {
                if (!is_dir && size == 0) {
                    continue; /* an empty file owns no chain */
                }
                return fail(err, errlen, "an entry names cluster zero");
            }

            if (path == 0 && name11_eq(e, "BOOT       ") && is_dir) {
                if (scan_dir(bs, cluster, 1, depth + 1, st, err, errlen) != 0) {
                    return -1;
                }
                continue;
            }
            if (path == 1 && name11_eq(e, "GRUB       ") && is_dir) {
                if (scan_dir(bs, cluster, 2, depth + 1, st, err, errlen) != 0) {
                    return -1;
                }
                continue;
            }
            if (path == 1 && name11_eq(e, "KERNEL     ") && !is_dir) {
                st->kernel_cluster = cluster;
                st->kernel_size = size;
                st->have_kernel = 1;
            } else if (path == 1 && name11_eq(e, "FALLBACK   ") && !is_dir) {
                st->fallback_cluster = cluster;
                st->fallback_size = size;
                st->have_fallback = 1;
            } else if (path == 2 && name11_eq(e, "GRUBENV    ") && !is_dir) {
                st->grubenv_cluster = cluster;
                st->grubenv_size = size;
                st->have_grubenv = 1;
            }

            if (is_dir) {
                if (scan_dir(bs, cluster, 3, depth + 1, st, err, errlen) != 0) {
                    return -1;
                }
            } else {
                uint32_t len, cap;

                if (walk_chain(bs, cluster, NULL, &len, err, errlen) != 0) {
                    return -1;
                }
                cap = len * bs->cluster_sectors * BOOTSLOT_SECTOR;
                if (size > cap || cap - size >= bs->cluster_sectors * BOOTSLOT_SECTOR) {
                    return fail(err, errlen, "a file's size does not match its chain");
                }
            }
        }
    }
    return 0;
}

/** Resolve one named file after the scan: walk its own chain into `file`
 *  (the scan marked it visited through a directory, so the walk here starts
 *  from a cleared bitmap slice; see bootslot_open) and hold its chain to its
 *  directory size. */
static int resolve_file(struct bootslot *bs, uint32_t first, uint32_t size,
                        struct bootslot_file *file, const char *what, char *err, size_t errlen)
{
    uint32_t capacity;

    if (walk_chain(bs, first, file, NULL, err, errlen) != 0) {
        return -1;
    }
    file->size = size;
    capacity = file->chain_len * bs->cluster_sectors * BOOTSLOT_SECTOR;
    if (size > capacity || capacity - size >= bs->cluster_sectors * BOOTSLOT_SECTOR) {
        (void)what;
        return fail(err, errlen, "a file's size does not match its chain");
    }
    return 0;
}

int bootslot_open(struct bootslot *bs, const struct bootslot_io *io, char *err, size_t errlen)
{
    static uint8_t bpb[BOOTSLOT_SECTOR];
    static uint8_t fat2[BOOTSLOT_SECTOR];
    struct scan_state st;
    uint32_t bytes_per_sector, reserved, nfats, total, fatsz, root_cluster;
    uint32_t s;

    memset(bs->visited, 0, sizeof(bs->visited));
    memset(&st, 0, sizeof(st));
    bs->io = *io;

    if (io->read(io->ctx, 0, bpb) != 0) {
        return fail(err, errlen, "the boot sector does not read");
    }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        return fail(err, errlen, "no boot-sector signature");
    }
    bytes_per_sector = le16(bpb + 11);
    bs->cluster_sectors = bpb[13];
    reserved = le16(bpb + 14);
    nfats = bpb[16];
    total = le16(bpb + 19);
    if (total == 0) {
        total = le32(bpb + 32);
    }
    fatsz = le16(bpb + 22);
    if (fatsz == 0) {
        fatsz = le32(bpb + 36);
    }
    root_cluster = le32(bpb + 44);

    /* The acceptance rule is the observed product (mformat's small FAT32 on
       the 16 MiB partition), stated as bounds rather than a type threshold. */
    if (bytes_per_sector != BOOTSLOT_SECTOR) {
        return fail(err, errlen, "the volume's sector size is not 512");
    }
    if (nfats != 2) {
        return fail(err, errlen, "the volume does not carry two FATs");
    }
    if (bs->cluster_sectors == 0 || bs->cluster_sectors > 128 ||
        (bs->cluster_sectors & (bs->cluster_sectors - 1)) != 0) {
        return fail(err, errlen, "the cluster size is not a legal power of two");
    }
    if (reserved == 0 || fatsz == 0 || fatsz > BOOTSLOT_FAT_MAX_SECTORS) {
        return fail(err, errlen, "the reserved or FAT region is out of bounds");
    }
    if (total == 0 || reserved >= total || fatsz > (total - reserved) / 2) {
        return fail(err, errlen, "the FAT regions do not fit the volume");
    }
    bs->total_sectors = total;
    bs->fat_start = reserved;
    bs->fat_sectors = fatsz;
    bs->data_start = reserved + 2 * fatsz;
    bs->cluster_count = (total - bs->data_start) / bs->cluster_sectors;
    if (bs->cluster_count == 0) {
        return fail(err, errlen, "the volume has no data clusters");
    }
    if (bs->cluster_count + 2 > BOOTSLOT_MAX_CLUSTERS) {
        return fail(err, errlen, "the volume has more clusters than the walk");
    }
    if (fatsz * (BOOTSLOT_SECTOR / 4) < bs->cluster_count + 2) {
        return fail(err, errlen, "the FAT is too small for the data region");
    }
    if (!cluster_legal(bs, root_cluster)) {
        return fail(err, errlen, "the root cluster is out of range");
    }

    /* Both FAT copies, compared sector by sector as the first is kept. */
    for (s = 0; s < fatsz; s++) {
        if (io->read(io->ctx, bs->fat_start + s, bs->fat + s * BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the first FAT does not read");
        }
        if (io->read(io->ctx, bs->fat_start + fatsz + s, fat2) != 0) {
            return fail(err, errlen, "the second FAT does not read");
        }
        if (memcmp(bs->fat + s * BOOTSLOT_SECTOR, fat2, BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the two FAT copies disagree");
        }
    }

    /* The whole reachable namespace, visited once, cross-links refused. */
    if (scan_dir(bs, root_cluster, 0, 0, &st, err, errlen) != 0) {
        return -1;
    }
    if (!st.have_kernel) {
        return fail(err, errlen, "no /boot/kernel on the volume");
    }
    if (!st.have_fallback) {
        return fail(err, errlen, "no /boot/fallback on the volume");
    }
    if (!st.have_grubenv) {
        return fail(err, errlen, "no /boot/grub/grubenv on the volume");
    }

    /* Resolve the three chains for I/O. The scan already visited them, so the
       bitmap is cleared first and the walk re-proves each chain alone; the
       cross-link property was proved by the scan above. */
    memset(bs->visited, 0, sizeof(bs->visited));
    if (resolve_file(bs, st.kernel_cluster, st.kernel_size, &bs->kernel, "kernel", err, errlen) !=
            0 ||
        resolve_file(bs, st.fallback_cluster, st.fallback_size, &bs->fallback, "fallback", err,
                     errlen) != 0 ||
        resolve_file(bs, st.grubenv_cluster, st.grubenv_size, &bs->grubenv, "grubenv", err,
                     errlen) != 0) {
        return -1;
    }
    if (bs->grubenv.size != ENV_BYTES) {
        return fail(err, errlen, "the environment block is not 1024 bytes");
    }
    return 0;
}

int bootslot_validate_kernel(const uint8_t *image, uint32_t present, uint32_t size, char *err,
                             size_t errlen)
{
    uint32_t limit, off;
    uint32_t e_entry, e_phoff, e_phnum, i;
    uint32_t nload = 0;
    int found = 0;
    int entry_in_load = 0;

    if (present > size) {
        present = size;
    }
    if (present < 52) {
        return fail(err, errlen, "the image is smaller than an ELF header");
    }
    if (image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        return fail(err, errlen, "the image is not an ELF");
    }
    if (image[4] != 1 || image[5] != 1) {
        return fail(err, errlen, "the image is not 32-bit little-endian");
    }
    if (le16(image + 16) != 2 || le16(image + 18) != 3) {
        return fail(err, errlen, "the image is not an i386 executable");
    }
    limit = present < MULTIBOOT_SEARCH ? present : MULTIBOOT_SEARCH;
    for (off = 0; off + 12 <= limit; off += 4) {
        if (le32(image + off) == MULTIBOOT_MAGIC &&
            le32(image + off) + le32(image + off + 4) + le32(image + off + 8) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        return fail(err, errlen, "no multiboot header in the first 8192 bytes");
    }

    /* The loader surface, the way tools/check_kernel.py judges it on the
       host: a sane program-header table, at least one load that stays inside
       the file and the megabyte-up address space, and an entry point inside
       a load's file-backed bytes. `present` is how much of the image is in
       memory (a fallback check reads only the head); every range is judged
       against the full `size`, and the headers themselves must sit in the
       bytes at hand. */
    e_entry = le32(image + 24);
    e_phoff = le32(image + 28);
    e_phnum = le16(image + 44);
    if (le16(image + 42) != 32 || e_phnum == 0 || e_phnum > 16) {
        return fail(err, errlen, "the program-header table is not a loader's");
    }
    if (e_phoff > present || e_phnum * 32 > present - e_phoff) {
        return fail(err, errlen, "the program headers are not in the bytes read");
    }
    for (i = 0; i < e_phnum; i++) {
        const uint8_t *ph = image + e_phoff + i * 32;
        uint32_t p_offset = le32(ph + 4);
        uint32_t p_vaddr = le32(ph + 8);
        uint32_t p_filesz = le32(ph + 16);
        uint32_t p_memsz = le32(ph + 20);

        if (le32(ph) != 1) {
            continue; /* only PT_LOAD is judged */
        }
        nload++;
        if (p_filesz > p_memsz) {
            return fail(err, errlen, "a load's file size exceeds its memory size");
        }
        if (p_offset > size || p_filesz > size - p_offset) {
            return fail(err, errlen, "a load leaves the file");
        }
        if (p_vaddr < 0x100000u) {
            return fail(err, errlen, "a load sits below the megabyte");
        }
        if (e_entry >= p_vaddr && e_entry - p_vaddr < p_filesz) {
            entry_in_load = 1;
        }
    }
    if (nload == 0) {
        return fail(err, errlen, "no loadable segment");
    }
    if (!entry_in_load) {
        return fail(err, errlen, "the entry point is outside every load");
    }
    return 0;
}

/**
 * Set `key=value` in the environment block: read the 1024 bytes, require
 * GRUB's header line, drop any existing `key=` line, append the new one, pad
 * with '#', and write both sectors back in order. GRUB's own save_env edits
 * the same block the same way; neither writer moves or resizes the file.
 */
static int env_edit(struct bootslot *bs, const char *set_key, const char *set_val,
                    const char *drop_key, char *err, size_t errlen)
{
    const char *key = set_key;
    const char *value = set_val;
    static uint8_t env[ENV_BYTES];
    static uint8_t out[ENV_BYTES];
    uint32_t n = 0, i = 0, s;
    size_t keylen = strlen(key);

    uint32_t end = 0;

    if (bootslot_read_env(bs, env, err, errlen) != 0) {
        return -1;
    }
    if (memcmp(env, ENV_HEADER, sizeof(ENV_HEADER) - 1) != 0) {
        return fail(err, errlen, "the environment block header is not GRUB's");
    }

    /* The block is newline-terminated lines (comments and variables) followed
       by a '#' pad with no newline in it; everything up to the last newline is
       the line region. Keep every line but the key's own. */
    for (i = 0; i < ENV_BYTES; i++) {
        if (env[i] == '\n') {
            end = (uint32_t)i + 1;
        }
    }
    if (end == 0) {
        return fail(err, errlen, "the environment block holds no lines");
    }
    i = 0;
    while ((uint32_t)i < end) {
        uint32_t start = (uint32_t)i, len;

        while (env[i] != '\n') {
            i++;
        }
        i++;
        len = (uint32_t)i - start;
        if (len > keylen && memcmp(env + start, key, keylen) == 0 && env[start + keylen] == '=') {
            continue; /* the line being replaced */
        }
        if (drop_key != NULL) {
            size_t dlen = strlen(drop_key);

            if (len > dlen && memcmp(env + start, drop_key, dlen) == 0 &&
                env[start + dlen] == '=') {
                continue; /* the line being dropped (a stale trial) */
            }
        }
        if (n + len > ENV_BYTES) {
            return fail(err, errlen, "the environment block overflows");
        }
        memcpy(out + n, env + start, len);
        n += len;
    }
    if (n + keylen + 1 + strlen(value) + 1 > ENV_BYTES) {
        return fail(err, errlen, "the environment block overflows");
    }
    memcpy(out + n, key, keylen);
    n += keylen;
    out[n++] = '=';
    memcpy(out + n, value, strlen(value));
    n += strlen(value);
    out[n++] = '\n';
    memset(out + n, '#', ENV_BYTES - n);

    for (s = 0; s < ENV_BYTES / BOOTSLOT_SECTOR; s++) {
        if (file_write_sector(bs, &bs->grubenv, s, out + s * BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the environment block does not write");
        }
    }
    if (bootslot_read_env(bs, env, err, errlen) != 0) {
        return -1;
    }
    if (memcmp(env, out, ENV_BYTES) != 0) {
        return fail(err, errlen, "the environment block did not read back as written");
    }
    return 0;
}

int bootslot_read_env(struct bootslot *bs, uint8_t *out, char *err, size_t errlen)
{
    uint32_t s;

    for (s = 0; s < ENV_BYTES / BOOTSLOT_SECTOR; s++) {
        if (file_read_sector(bs, &bs->grubenv, s, out + s * BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the environment block does not read");
        }
    }
    return 0;
}

int bootslot_install(struct bootslot *bs, const uint8_t *image, uint32_t size, char *err,
                     size_t errlen)
{
    static uint8_t sector[BOOTSLOT_SECTOR];
    static uint8_t head[MULTIBOOT_SEARCH];
    uint32_t slot_bytes = bs->kernel.size;
    uint32_t nsectors = slot_bytes / BOOTSLOT_SECTOR;
    uint32_t s, b, want;

    if (slot_bytes % BOOTSLOT_SECTOR != 0) {
        return fail(err, errlen, "the candidate slot is not sector-sized");
    }
    if (bootslot_validate_kernel(image, size, size, err, errlen) != 0) {
        return -1;
    }
    if (size > slot_bytes) {
        return fail(err, errlen, "the kernel does not fit the candidate slot");
    }

    /* The fallback must be a bootable kernel BEFORE the install points the
       saved default at it; a machine whose fallback does not validate gets no
       install at all. Its first 8192 bytes carry everything the check reads. */
    want = bs->fallback.size < MULTIBOOT_SEARCH ? bs->fallback.size : MULTIBOOT_SEARCH;
    for (s = 0; s * BOOTSLOT_SECTOR < want; s++) {
        if (file_read_sector(bs, &bs->fallback, s, head + s * BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the fallback file does not read");
        }
    }
    if (bootslot_validate_kernel(head, want, bs->fallback.size, err, errlen) != 0) {
        return fail(err, errlen, "the fallback file is not a bootable kernel");
    }

    /* One transition: the saved default moves to the fallback AND any stale
       next_entry from an earlier install is dropped, so a retry cannot leave
       a half-written slot armed for the next boot. */
    if (env_edit(bs, "saved_entry", "fallback", "next_entry", err, errlen) != 0) {
        return -1;
    }

    for (s = 0; s < nsectors; s++) {
        for (b = 0; b < BOOTSLOT_SECTOR; b++) {
            uint32_t src = s * BOOTSLOT_SECTOR + b;

            sector[b] = src < size ? image[src] : 0;
        }
        if (file_write_sector(bs, &bs->kernel, s, sector) != 0) {
            return fail(err, errlen, "the candidate slot does not write");
        }
    }

    /* Read the whole padded slot back and compare byte for byte: structural
       validation alone would pass a slot missing a load sector away from the
       headers. */
    for (s = 0; s < nsectors; s++) {
        if (file_read_sector(bs, &bs->kernel, s, sector) != 0) {
            return fail(err, errlen, "the candidate slot does not read back");
        }
        for (b = 0; b < BOOTSLOT_SECTOR; b++) {
            uint32_t src = s * BOOTSLOT_SECTOR + b;
            uint8_t expect = src < size ? image[src] : 0;

            if (sector[b] != expect) {
                return fail(err, errlen, "the slot's bytes do not match what was written");
            }
        }
    }

    return env_edit(bs, "next_entry", "candidate", NULL, err, errlen);
}

int bootslot_commit(struct bootslot *bs, char *err, size_t errlen)
{
    static uint8_t env[ENV_BYTES];
    uint32_t i;

    /* An armed trial means an install happened after this machine booted, so
       the slot no longer holds the kernel that is running: committing would
       bless bytes that never booted. The arming line is exact; the emptied
       form GRUB's consumption leaves behind does not match it. */
    if (bootslot_read_env(bs, env, err, errlen) != 0) {
        return -1;
    }
    for (i = 0; i + 21 <= ENV_BYTES; i++) {
        if (memcmp(env + i, "next_entry=candidate\n", 21) == 0) {
            return fail(err, errlen,
                        "an installed candidate has not booted; boot it "
                        "before committing");
        }
    }
    return env_edit(bs, "saved_entry", "candidate", NULL, err, errlen);
}
