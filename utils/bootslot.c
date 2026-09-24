/**
 * bootslot.c: the boot-slot core bootctl drives, over caller-supplied sector
 * I/O.
 *
 * The shape follows dev-log/23-install/plan.md. Nothing here trusts the
 * volume: the BPB is validated field by field with subtraction-based bounds,
 * the two FAT copies must be identical, every chain the namespace reaches is
 * walked under a visited bitmap (a cluster reached twice is a cross-link and
 * an error), a regular file's chain length must match its directory size,
 * and the three named files must exist as short directory entries. Only then
 * does an install write anything, in the order the interruption matrix
 * documents, so a power cut at any point leaves a machine that boots.
 */

#include "bootslot.h"

#include <string.h>

/* On-disk constants: FAT32, GRUB's environment block, and Multiboot. */
#define DIRENT_SIZE      32
#define ATTR_LFN         0x0F
#define ATTR_VOLUME      0x08
#define ATTR_DIR         0x10
#define FAT_ENTRY_MASK   0x0FFFFFFFu
#define FAT_EOC_MIN      0x0FFFFFF8u
#define ENV_BYTES        BOOTSLOT_ENV_BYTES
#define ENV_HEADER       "# GRUB Environment Block\n"
#define MULTIBOOT_MAGIC  0x1BADB002u
#define MULTIBOOT_SEARCH 8192
#define DIR_DEPTH_MAX    4

/* The banners diskcmd.c embeds, one per build, which name the disk policy a
 * kernel was compiled with. A machine's kernel must be the read-write build:
 * the read-only build refuses every write command, so the mount of a disk
 * root fails and the boot stops with nothing mounted. */
#define POLICY_READ_WRITE "rach2: disk policy: read-write"
#define POLICY_READ_ONLY  "rach2: disk policy: read-only"

/**
 * Copy `msg` into the caller's error buffer, always NUL-terminated, and
 * return -1.
 */
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
 * Walk the chain from `first`, marking every cluster visited.
 *
 * Record clusters when `file` is non-NULL; otherwise return only the chain
 * length through `len_out` when supplied. The bound is the volume's own
 * cluster count, so a loop that survives the visited bitmap still terminates.
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
                return fail(err, errlen, "a chain is longer than the walk has");
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

/* The recursive scan's result: where the named files were found. */
struct scan_state {
    struct bootslot_file dir;
    uint32_t kernel_cluster, kernel_size;
    uint32_t fallback_cluster, fallback_size;
    uint32_t grubenv_cluster, grubenv_size;
    uint32_t menu_cluster, menu_size;
    int have_kernel, have_fallback, have_grubenv, have_menu;
};

/**
 * Scan one directory chain: mark every child chain visited (so disjointness
 * is proved across the whole reachable namespace), remember the named files
 * when this directory is their parent, and recurse into subdirectories.
 * `path` selects which names are expected here: 0 the root (BOOT), 1 /boot
 * (KERNEL, FALLBACK, BOOT.CFG, GRUB), 2 /boot/grub (GRUBENV), 3 anything
 * deeper. The sector buffer is on the stack, not static, so the recursion
 * does not clobber the sector a frame is still reading; the depth bound keeps
 * that to a few sectors.
 */
static int scan_dir(struct bootslot *bs, uint32_t first_cluster, int path, int depth,
                    struct scan_state *st, char *err, size_t errlen)
{
    struct bootslot_file dir;
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
            } else if (path == 1 && name11_eq(e, "BOOT    CFG") && !is_dir) {
                st->menu_cluster = cluster;
                st->menu_size = size;
                st->have_menu = 1;
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
 *  (the scan marked it visited, so `bootslot_open` clears the bitmap before
 *  the files are resolved) and hold its chain to its directory size. */
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

/**
 * Read and validate the volume; the contract is in `bootslot.h`.
 *
 * The BPB bounds are those of mformat's FAT32 on the 16 MiB and the 64 MiB
 * boot partitions `tools/image/mkdisk_grub.sh` builds, stated as bounds and
 * not as a FAT-type threshold. The scan marks every chain it reaches, so the
 * bitmap is cleared before each named file's chain is walked again alone;
 * the scan has already proved them disjoint. A menu slot that is empty or not
 * a whole number of sectors is a disk built wrong and cannot be rewritten
 * whole, so it is refused here, where the volume is judged, and not later,
 * where a write would half-succeed.
 */
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
        return fail(err, errlen,
                    "the first sector does not read, so the device is gone "
                    "or failing");
    }
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) {
        return fail(err, errlen,
                    "this is not a boot partition: its first sector has no "
                    "FAT signature");
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

    memset(bs->visited, 0, sizeof(bs->visited));
    if (resolve_file(bs, st.kernel_cluster, st.kernel_size, &bs->kernel, "kernel", err, errlen) !=
            0 ||
        resolve_file(bs, st.fallback_cluster, st.fallback_size, &bs->fallback, "fallback", err,
                     errlen) != 0 ||
        resolve_file(bs, st.grubenv_cluster, st.grubenv_size, &bs->grubenv, "grubenv", err,
                     errlen) != 0 ||
        (st.have_menu && resolve_file(bs, st.menu_cluster, st.menu_size, &bs->menu, "boot.cfg", err,
                                      errlen) != 0)) {
        return -1;
    }
    bs->have_menu = st.have_menu;
    if (bs->have_menu && (bs->menu.size == 0 || (bs->menu.size % BOOTSLOT_SECTOR) != 0)) {
        return fail(err, errlen, "the menu slot is not a whole number of sectors");
    }
    if (bs->grubenv.size != ENV_BYTES) {
        return fail(err, errlen, "the environment block is not 1024 bytes");
    }
    return 0;
}

/** Whether `needle` appears in the `size` bytes at `image`. */
static int image_carries(const uint8_t *image, uint32_t size, const char *needle)
{
    uint32_t n = (uint32_t)strlen(needle);
    uint32_t i;

    if (n == 0 || size < n) {
        return 0;
    }
    for (i = 0; i + n <= size; i++) {
        if (image[i] == (uint8_t)needle[0] && memcmp(image + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Judge the disk policy an image names, when the whole image is in memory.
 *
 * A machine's kernel must be the read-write build. The read-only build
 * refuses every write command, so its mount of a disk root fails and the
 * boot stops with nothing mounted; an image naming neither policy, or both,
 * is not a kernel this tool can answer for. An image of which only a prefix
 * is present, as when a slot already installed is checked, passes unjudged,
 * since its banner may lie past the prefix.
 */
static int policy_judged(const uint8_t *image, uint32_t present, uint32_t size, char *err,
                         size_t errlen)
{
    int rw, ro;

    if (present < size) {
        return 0;
    }
    rw = image_carries(image, size, POLICY_READ_WRITE);
    ro = image_carries(image, size, POLICY_READ_ONLY);

    if (ro && !rw) {
        return fail(err, errlen,
                    "the image is the read-only disk build, which cannot mount a disk root");
    }
    if (!rw || ro) {
        return fail(err, errlen, "the image names no one disk policy");
    }
    return 0;
}

/**
 * Validate the in-memory headers of an ELF32 i386 kernel.
 *
 * Load file ranges must fit `size`, and load memory ranges must fit the
 * 32-bit address space at or above 1 MiB. `present` gives the available
 * prefix; the ELF and program headers must fit that prefix. The entry point
 * must lie in a load's file-backed bytes. The exclusive memory end must
 * be representable in 32 bits. A Multiboot header must be in the first 8192
 * bytes, and its flags must be ALIGN|MEMINFO; other flags change how the
 * loader reads the image. Each load's physical address must equal its
 * virtual one, and a whole image must name the read-write disk policy
 * (`policy_judged`).
 */
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
    if (le32(image + off + 4) != 3) {
        return fail(err, errlen, "the multiboot flags are not ALIGN|MEMINFO");
    }

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
        if (p_memsz > 0xFFFFFFFFu - p_vaddr) {
            return fail(err, errlen, "a load leaves the 32-bit address space");
        }
        if (le32(ph + 12) != p_vaddr) {
            return fail(err, errlen, "a load's physical address differs from its virtual");
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
    return policy_judged(image, present, size, err, errlen);
}

/**
 * Set `key=value` in the environment block, and drop any `drop_key=` line:
 * read the 1024 bytes, require GRUB's header line, drop any existing `key=`
 * line, append the new one, pad with '#', write both sectors back in order,
 * and read them back. GRUB's own save_env edits the same block the same way;
 * neither writer moves or resizes the file.
 *
 * The block is newline-terminated lines, comments and variables, followed by
 * a '#' pad with no newline in it, so everything up to the last newline is
 * the line region. A NULL `set_key` writes no line at all, which is how a key
 * is removed rather than emptied; for `next_entry` the two are different
 * states, since an empty value is the marker a trial boot leaves and a commit
 * requires.
 */
static int env_edit(struct bootslot *bs, const char *set_key, const char *set_val,
                    const char *drop_key, char *err, size_t errlen)
{
    const char *key = set_key;
    const char *value = set_val;
    static uint8_t env[ENV_BYTES];
    static uint8_t out[ENV_BYTES];
    uint32_t n = 0, i = 0, s;
    size_t keylen = (key != NULL) ? strlen(key) : 0;

    uint32_t end = 0;

    if (bootslot_read_env(bs, env, err, errlen) != 0) {
        return -1;
    }
    if (memcmp(env, ENV_HEADER, sizeof(ENV_HEADER) - 1) != 0) {
        return fail(err, errlen, "the environment block header is not GRUB's");
    }

    for (i = 0; i < ENV_BYTES; i++) {
        if (env[i] == '\n') {
            end = (uint32_t)i + 1;
        }
    }
    if (end == 0) {
        return fail(err, errlen, "the environment block has no lines");
    }
    i = 0;
    while ((uint32_t)i < end) {
        uint32_t start = (uint32_t)i, len;

        while (env[i] != '\n') {
            i++;
        }
        i++;
        len = (uint32_t)i - start;
        if (key != NULL && len > keylen && memcmp(env + start, key, keylen) == 0 &&
            env[start + keylen] == '=') {
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
    if (key != NULL) {
        if (n + keylen + 1 + strlen(value) + 1 > ENV_BYTES) {
            return fail(err, errlen, "the environment block overflows");
        }
        memcpy(out + n, key, keylen);
        n += keylen;
        out[n++] = '=';
        memcpy(out + n, value, strlen(value));
        n += strlen(value);
        out[n++] = '\n';
    }
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

int bootslot_write_menu(struct bootslot *bs, const uint8_t *text, uint32_t size, char *err,
                        size_t errlen)
{
    static uint8_t sector[BOOTSLOT_SECTOR];
    static uint8_t back[BOOTSLOT_SECTOR];
    uint32_t slot = bs->menu.size;
    uint32_t sectors = slot / BOOTSLOT_SECTOR;
    uint32_t n, i;

    if (!bs->have_menu) {
        return fail(err, errlen, "this disk has no menu slot");
    }
    if (size > slot) {
        return fail(err, errlen, "the menu does not fit the slot");
    }

    for (n = 0; n < sectors; n++) {
        for (i = 0; i < BOOTSLOT_SECTOR; i++) {
            uint32_t at = n * BOOTSLOT_SECTOR + i;

            sector[i] = (at < size) ? text[at] : (uint8_t)'#';
        }
        if (file_write_sector(bs, &bs->menu, n, sector) != 0) {
            return fail(err, errlen, "the menu slot did not write");
        }
        if (file_read_sector(bs, &bs->menu, n, back) != 0) {
            return fail(err, errlen, "the menu slot did not read back");
        }
        for (i = 0; i < BOOTSLOT_SECTOR; i++) {
            if (back[i] != sector[i]) {
                return fail(err, errlen, "the menu slot did not read back what was written");
            }
        }
    }
    return 0;
}

int bootslot_env_set(struct bootslot *bs, const char *key, const char *value, char *err,
                     size_t errlen)
{
    return env_edit(bs, key, value, NULL, err, errlen);
}

/**
 * Remove a key without writing an empty value.
 *
 * Removing `next_entry` cancels a trial without supplying the empty marker
 * that `bootslot_commit` requires. That marker is written when GRUB selects
 * a trial, before loading its kernel.
 */
int bootslot_env_drop(struct bootslot *bs, const char *key, char *err, size_t errlen)
{
    return env_edit(bs, NULL, NULL, key, err, errlen);
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

/**
 * Install `image` into the candidate slot, in the order that keeps the
 * machine bootable at every step.
 *
 * The image is validated whole. The fallback must validate as a bootable
 * kernel before the saved default is pointed at it, so a machine whose
 * fallback does not validate gets no install at all; its first 8192 bytes
 * hold everything that check reads. One environment write then moves the
 * saved default to the fallback and drops any stale `next_entry` from an
 * earlier install, so a retry cannot leave a half-written slot armed. The
 * slot is written, padded with zeros, and read back whole and compared,
 * since structural validation alone would pass a slot missing a load sector
 * away from the headers. Only then is `next_entry=candidate` set.
 */
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

    want = bs->fallback.size < MULTIBOOT_SEARCH ? bs->fallback.size : MULTIBOOT_SEARCH;
    for (s = 0; s * BOOTSLOT_SECTOR < want; s++) {
        if (file_read_sector(bs, &bs->fallback, s, head + s * BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the fallback file does not read");
        }
    }
    if (bootslot_validate_kernel(head, want, bs->fallback.size, err, errlen) != 0) {
        return fail(err, errlen, "the fallback file is not a bootable kernel");
    }

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

/**
 * Whether the environment block names `id` as its saved default.
 *
 * `want` is the caller's scratch buffer, filled with the line the block must
 * hold, so this allocates nothing.
 */
static int env_names_default(const uint8_t *env, const char *id, char *want, size_t want_size)
{
    uint32_t i, n = 13;

    memcpy(want, "\nsaved_entry=", 13);
    for (i = 0; id[i] != '\0' && n + 2 < want_size; i++) {
        want[n++] = id[i];
    }
    want[n++] = '\n';
    want[n] = '\0';
    for (i = 0; i + n <= ENV_BYTES; i++) {
        if (memcmp(env + i, want, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether the environment block has a nonempty `next_entry`: a trial armed
 * and not yet taken by GRUB.
 */
static int env_trial_armed(const uint8_t *env)
{
    uint32_t i;

    for (i = 0; i + 13 <= ENV_BYTES; i++) {
        if (memcmp(env + i, "\nnext_entry=", 12) == 0 && env[i + 12] != '\n') {
            return 1;
        }
    }
    return 0;
}

/**
 * Copy the candidate slot into the fallback file and read it back.
 *
 * Require `saved_entry=candidate` and no nonempty `next_entry` before
 * copying the candidate slot, both slots a whole number of sectors, a
 * fallback file at least as long as the slot, and a candidate whose headers
 * validate.
 *
 * Invalidate the fallback's first sector, copy and verify the remaining
 * sectors, then restore and verify the first sector. A failure before that
 * final write leaves no valid ELF header. A failure during the final write
 * may leave a partial header.
 */
int bootslot_fallback(struct bootslot *bs, char *err, size_t errlen)
{
    static uint8_t env[ENV_BYTES];
    static uint8_t sector[BOOTSLOT_SECTOR];
    static uint8_t back[BOOTSLOT_SECTOR];
    static uint8_t head[MULTIBOOT_SEARCH];
    char want[BOOTSLOT_ENTRY_MAX + 16];
    const char *id = "candidate";
    uint32_t nsectors = bs->kernel.size / BOOTSLOT_SECTOR;
    uint32_t present, s;
    int committed = 0;

    if (bs->kernel.size % BOOTSLOT_SECTOR != 0 || bs->fallback.size % BOOTSLOT_SECTOR != 0) {
        return fail(err, errlen, "a slot is not sector-sized");
    }
    if (bs->fallback.size < bs->kernel.size) {
        return fail(err, errlen,
                    "the fallback file is shorter than the candidate slot: a disk built "
                    "before the fallback was rewritable");
    }
    if (bootslot_read_env(bs, env, err, errlen) != 0) {
        return -1;
    }
    committed = env_names_default(env, id, want, sizeof(want));
    if (env_trial_armed(env)) {
        return fail(err, errlen,
                    "a trial is armed: the candidate slot is not the kernel "
                    "that was committed");
    }
    if (!committed) {
        return fail(err, errlen,
                    "the candidate is not the saved default; boot and commit it "
                    "before it becomes the fallback");
    }
    present = bs->kernel.size < MULTIBOOT_SEARCH ? bs->kernel.size : MULTIBOOT_SEARCH;
    for (s = 0; s * BOOTSLOT_SECTOR < present; s++) {
        if (file_read_sector(bs, &bs->kernel, s, head + s * BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the candidate slot does not read");
        }
    }
    if (bootslot_validate_kernel(head, present, bs->kernel.size, err, errlen) != 0) {
        return -1;
    }
    memset(sector, 0, sizeof(sector));
    if (file_write_sector(bs, &bs->fallback, 0, sector) != 0) {
        return fail(err, errlen, "the fallback file does not write");
    }
    for (s = nsectors; s-- > 1;) {
        if (file_read_sector(bs, &bs->kernel, s, sector) != 0) {
            return fail(err, errlen, "the candidate slot does not read");
        }
        if (file_write_sector(bs, &bs->fallback, s, sector) != 0) {
            return fail(err, errlen, "the fallback file does not write");
        }
    }
    for (s = 1; s < nsectors; s++) {
        if (file_read_sector(bs, &bs->kernel, s, sector) != 0 ||
            file_read_sector(bs, &bs->fallback, s, back) != 0) {
            return fail(err, errlen, "the fallback file does not read back");
        }
        if (memcmp(sector, back, BOOTSLOT_SECTOR) != 0) {
            return fail(err, errlen, "the fallback's bytes do not match the candidate's");
        }
    }
    if (file_write_sector(bs, &bs->fallback, 0, head) != 0) {
        return fail(err, errlen, "the fallback file does not write");
    }
    if (file_read_sector(bs, &bs->fallback, 0, back) != 0) {
        return fail(err, errlen, "the fallback file does not read back");
    }
    if (memcmp(head, back, BOOTSLOT_SECTOR) != 0) {
        return fail(err, errlen, "the fallback's bytes do not match the candidate's");
    }
    return 0;
}

/**
 * Set `saved_entry=candidate` when the environment contains an empty
 * `next_entry` and no nonempty occurrence. The caller must establish that
 * this boot runs the candidate. GRUB writes the empty marker when selecting
 * a trial, before loading its kernel.
 */
int bootslot_commit(struct bootslot *bs, char *err, size_t errlen)
{
    static uint8_t env[ENV_BYTES];
    uint32_t i;

    int consumed = 0;

    if (bootslot_read_env(bs, env, err, errlen) != 0) {
        return -1;
    }
    for (i = 0; i + 13 <= ENV_BYTES; i++) {
        if (memcmp(env + i, "\nnext_entry=", 12) != 0) {
            continue;
        }
        if (env[i + 12] == '\n') {
            consumed = 1;
        } else {
            return fail(err, errlen,
                        "an installed candidate has not booted; boot it "
                        "before committing");
        }
    }
    if (!consumed) {
        return fail(err, errlen,
                    "no consumed trial in the environment block; install and "
                    "boot a candidate before committing");
    }
    return env_edit(bs, "saved_entry", "candidate", NULL, err, errlen);
}
