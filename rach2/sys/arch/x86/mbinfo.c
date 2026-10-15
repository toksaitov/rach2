/*
 * mbinfo.c: the loader's information into the boot record, as mbinfo.h
 * describes it. Multiboot's is in the Multiboot Specification 0.6.96 and
 * multiboot2's in the Multiboot2 Specification 2.0; every field is read by
 * its offset, a byte at a time, so a tag list need not be aligned for the
 * fields in it.
 */

#include "mbinfo.h"

#include "../../lib/kstring.h"

/* Multiboot's flag for the framebuffer's fields, which follow the VBE ones
 * at offset 88 of the structure. */
#define MB1_INFO_FRAMEBUFFER 0x00001000u
#define MB1_FB_FIELDS        88u
#define MB1_INFO_SIZE        116u

/* multiboot2's tag types read here. */
#define MB2_TAG_END      0u
#define MB2_TAG_CMDLINE  1u
#define MB2_TAG_LOADER   2u
#define MB2_TAG_MODULE   3u
#define MB2_TAG_MEMINFO  4u
#define MB2_TAG_MMAP     6u
#define MB2_TAG_FB       8u
#define MB2_TAG_EFI32    11u
#define MB2_TAG_EFI64    12u
#define MB2_TAG_ACPI_OLD 14u
#define MB2_TAG_ACPI_NEW 15u
#define MB2_TAG_EFI_MMAP 17u

/* The framebuffer's types, in both forms. */
#define MB_FB_INDEXED 0u
#define MB_FB_RGB     1u
#define MB_FB_TEXT    2u

/* Multiboot's memory types the record uses. */
#define MB_MEM_USABLE   1u
#define MB_MEM_RESERVED 2u
#define MB_MEM_ACPI     3u
#define MB_MEM_NVS      4u
#define MB_MEM_BAD      5u

/** The little-endian 16-bit field at `p`. */
static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/** The little-endian 32-bit field at `p`. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** The little-endian 64-bit field at `p`. */
static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/** Store `v` little-endian at `p`; `put64` stores 64 bits. */
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/** Copy the string of at most `avail` bytes at `src` into `dst` of `size`
 * bytes, cut short and terminated, and return `dst`. */
static char *copy_string(char *dst, uint32_t size, const uint8_t *src, uint32_t avail)
{
    uint32_t i;

    for (i = 0; i + 1 < size && i < avail && src[i] != '\0'; i++) {
        dst[i] = (char)src[i];
    }
    dst[i] = '\0';
    return dst;
}

/** Clear the record and the copies, before a read fills them. */
static void reset(struct boot_info *boot, struct boot_store *st)
{
    k_memset(boot, 0, sizeof(*boot));
    k_memset(st, 0, sizeof(*st));
}

/**
 * Append a range to the map, as one entry of Multiboot's form, or as more of
 * the last entry when it has the same type and ends where this one begins:
 * the EFI map gives a range for every allocation, and the record keeps a
 * range for every kind of memory. A merge whose length would pass 2^64 is
 * not made, so that no reserved range wraps to a length of zero.
 */
static void map_add(struct boot_info *boot, struct boot_store *st, uint64_t base, uint64_t len,
                    uint32_t type)
{
    uint8_t *e;

    if (len == 0) {
        return;
    }
    if (st->mmap_entries > 0) {
        uint64_t ebase, elen;

        e = st->mmap + (st->mmap_entries - 1) * BOOT_MMAP_ENTRY;
        ebase = le64(e + 4);
        elen = le64(e + 12);
        if (le32(e + 20) == type && elen <= ~(uint64_t)0 - ebase && ebase + elen == base &&
            len <= ~(uint64_t)0 - elen) {
            put64(e + 12, elen + len);
            return;
        }
    }
    if (st->mmap_entries == BOOT_MMAP_MAX) {
        st->mmap_cut++;
        return;
    }
    e = st->mmap + st->mmap_entries * BOOT_MMAP_ENTRY;
    put32(e, BOOT_MMAP_ENTRY - 4);
    put64(e + 4, base);
    put64(e + 12, len);
    put32(e + 20, type);
    st->mmap_entries++;
    boot->mmap = st->mmap;
    boot->mmap_len = st->mmap_entries * BOOT_MMAP_ENTRY;
}

/** Empty the map, for a map tag that replaces what came before it. */
static void map_clear(struct boot_info *boot, struct boot_store *st)
{
    st->mmap_entries = 0;
    st->mmap_cut = 0;
    boot->mmap = NULL;
    boot->mmap_len = 0;
}

/** Multiboot's memory type for EFI's type `efi`. */
static uint32_t efi_type(uint32_t efi)
{
    switch (efi) {
    case 1: /* EfiLoaderCode */
    case 2: /* EfiLoaderData */
    case 3: /* EfiBootServicesCode */
    case 4: /* EfiBootServicesData */
    case 7: /* EfiConventionalMemory */
        return MB_MEM_USABLE;
    case 8: /* EfiUnusableMemory */
        return MB_MEM_BAD;
    case 9: /* EfiACPIReclaimMemory */
        return MB_MEM_ACPI;
    case 10: /* EfiACPIMemoryNVS */
        return MB_MEM_NVS;
    default:
        return MB_MEM_RESERVED;
    }
}

/**
 * Record a module from `start` to `end`, an end before the start taken as
 * the start, with a copy of the command line of at most `avail` bytes at
 * `cmdline`, which may be NULL. Every module is counted in `modules_passed`;
 * one past BOOT_MODULE_MAX is not kept.
 */
static void add_module(struct boot_info *boot, struct boot_store *st, uint32_t start, uint32_t end,
                       const uint8_t *cmdline, uint32_t avail)
{
    struct boot_module *m;

    boot->modules_passed++;
    if (boot->modules == BOOT_MODULE_MAX) {
        return;
    }
    m = &boot->module[boot->modules];
    m->start = start;
    m->end = end < start ? start : end;
    m->cmdline = cmdline != NULL ? copy_string(st->module_cmdline[boot->modules],
                                               BOOT_MODULE_CMDLINE_MAX, cmdline, avail)
                                 : NULL;
    boot->modules++;
}

/**
 * The framebuffer from the fields both forms share: the address, the pitch,
 * the size, the depth and the type, then `color`, `avail` bytes of the
 * type's color fields, and for an indexed one its palette at `palette`.
 */
static void set_framebuffer(struct boot_framebuffer *fb, const uint8_t *common,
                            const uint8_t *color, uint32_t avail, const uint8_t *palette,
                            uint32_t colors)
{
    uint32_t i;

    fb->addr = le64(common);
    fb->pitch = le32(common + 8);
    fb->width = le32(common + 12);
    fb->height = le32(common + 16);
    fb->bpp = common[20];
    switch (common[21]) {
    case MB_FB_RGB:
        if (avail < 6) {
            return;
        }
        fb->red_shift = color[0];
        fb->red_bits = color[1];
        fb->green_shift = color[2];
        fb->green_bits = color[3];
        fb->blue_shift = color[4];
        fb->blue_bits = color[5];
        fb->type = BOOT_FB_RGB;
        break;
    case MB_FB_INDEXED:
        if (colors > 256) {
            colors = 256;
        }
        for (i = 0; palette != NULL && i < colors; i++) {
            fb->palette[i][0] = palette[i * 3];
            fb->palette[i][1] = palette[i * 3 + 1];
            fb->palette[i][2] = palette[i * 3 + 2];
        }
        fb->colors = palette != NULL ? (uint16_t)colors : 0;
        fb->type = BOOT_FB_INDEXED;
        break;
    case MB_FB_TEXT:
        fb->type = BOOT_FB_TEXT;
        break;
    default:
        break;
    }
}

int mb1_read(const multiboot_info_t *info, mb_reach_fn reach, struct boot_info *boot,
             struct boot_store *st)
{
    const uint8_t *raw = (const uint8_t *)info;
    const uint8_t *p;
    const uint8_t *end;
    uint32_t i, count;
    int status = 0;

    reset(boot, st);
    if (info->flags & MULTIBOOT_INFO_CMDLINE) {
        p = reach(info->cmdline, BOOT_CMDLINE_MAX);
        if (p != NULL) {
            boot->cmdline = copy_string(st->cmdline, BOOT_CMDLINE_MAX, p, BOOT_CMDLINE_MAX);
        } else {
            status = -1;
        }
    }
    if (info->flags & MULTIBOOT_INFO_BOOT_LOADER_NAME) {
        p = reach(info->boot_loader_name, sizeof(st->loader));
        if (p != NULL) {
            boot->loader = copy_string(st->loader, sizeof(st->loader), p, sizeof(st->loader));
        }
    }
    if (info->flags & MULTIBOOT_INFO_MEMORY) {
        boot->mem_upper_kb = info->mem_upper;
    }
    if ((info->flags & MULTIBOOT_INFO_MODS) && info->mods_count > 0) {
        count = info->mods_count < BOOT_MODULE_MAX ? info->mods_count : BOOT_MODULE_MAX;
        p = reach(info->mods_addr, count * sizeof(multiboot_module_t));
        if (p != NULL) {
            for (i = 0; i < count; i++) {
                const uint8_t *mod = p + i * sizeof(multiboot_module_t);
                const uint8_t *cmdline = NULL;

                if (le32(mod + 8) != 0) {
                    cmdline = reach(le32(mod + 8), BOOT_MODULE_CMDLINE_MAX);
                }
                add_module(boot, st, le32(mod), le32(mod + 4), cmdline, BOOT_MODULE_CMDLINE_MAX);
            }
            boot->modules_passed = info->mods_count;
        } else {
            status = -1;
        }
    }
    if ((info->flags & MULTIBOOT_INFO_MEM_MAP) && info->mmap_length > 0) {
        p = reach(info->mmap_addr, info->mmap_length);
        end = p != NULL ? p + info->mmap_length : NULL;
        if (p == NULL) {
            status = -1;
        }
        while (p != NULL && p < end) {
            uint32_t size = (uint32_t)(end - p) >= 24 ? le32(p) : 0;

            if (size < 20 || size > (uint32_t)(end - p) - 4) {
                status = -1;
                break;
            }
            map_add(boot, st, le64(p + 4), le64(p + 12), le32(p + 20));
            p += size + 4;
        }
    }
    if (info->flags & MB1_INFO_FRAMEBUFFER) {
        const uint8_t *fbf = raw + MB1_FB_FIELDS;
        const uint8_t *palette = NULL;
        uint32_t colors = 0;

        if (fbf[21] == MB_FB_INDEXED) {
            colors = le16(fbf + 26);
            if (colors > 256) {
                colors = 256;
            }
            palette = reach(le32(fbf + 22), colors * 3);
        }
        set_framebuffer(&boot->fb, fbf, fbf + 22, MB1_INFO_SIZE - MB1_FB_FIELDS - 22, palette,
                        colors);
    }
    return status;
}

/** One multiboot2 tag of `size` bytes at `t`, its header among them. */
static void mb2_tag(const uint8_t *t, uint32_t size, struct boot_info *boot, struct boot_store *st)
{
    uint32_t i, n, step;

    switch (le32(t)) {
    case MB2_TAG_CMDLINE:
        boot->cmdline = copy_string(st->cmdline, BOOT_CMDLINE_MAX, t + 8, size - 8);
        break;
    case MB2_TAG_LOADER:
        boot->loader = copy_string(st->loader, sizeof(st->loader), t + 8, size - 8);
        break;
    case MB2_TAG_MODULE:
        if (size >= 16) {
            add_module(boot, st, le32(t + 8), le32(t + 12), t + 16, size - 16);
        }
        break;
    case MB2_TAG_MEMINFO:
        if (size >= 16) {
            boot->mem_upper_kb = le32(t + 12);
        }
        break;
    case MB2_TAG_MMAP:
        step = size >= 16 ? le32(t + 8) : 0;
        if (st->mmap_efi || step < 24) {
            break;
        }
        map_clear(boot, st);
        for (i = 16; step <= size - i; i += step) {
            map_add(boot, st, le64(t + i), le64(t + i + 8), le32(t + i + 16));
        }
        break;
    case MB2_TAG_EFI_MMAP:
        step = size >= 16 ? le32(t + 8) : 0;
        if (step < 40) {
            break;
        }
        map_clear(boot, st);
        st->mmap_efi = 1;
        boot->uefi = 1;
        for (i = 16; step <= size - i; i += step) {
            uint64_t pages = le64(t + i + 24);

            if (pages > 0x000FFFFFFFFFFFFFULL) {
                continue;
            }
            map_add(boot, st, le64(t + i + 8), pages * 4096, efi_type(le32(t + i)));
        }
        break;
    case MB2_TAG_FB:
        if (size < 32) {
            break;
        }
        n = 0;
        if (t[29] == MB_FB_INDEXED && size >= 34) {
            n = le16(t + 32);
            if (n > (size - 34) / 3) {
                n = (size - 34) / 3;
            }
        }
        set_framebuffer(&boot->fb, t + 8, t + 32, size - 32, t + 34, n);
        break;
    case MB2_TAG_EFI32:
        boot->uefi = 1;
        if (size >= 12) {
            boot->efi_systab = le32(t + 8);
            boot->efi_systab_64 = 0;
        }
        break;
    case MB2_TAG_EFI64:
        boot->uefi = 1;
        if (size >= 16) {
            boot->efi_systab = le64(t + 8);
            boot->efi_systab_64 = 1;
        }
        break;
    case MB2_TAG_ACPI_OLD:
        if (size >= 8 + 20 && st->rsdp_len == 0) {
            k_memcpy(st->rsdp, t + 8, 20);
            st->rsdp_len = 20;
        }
        break;
    case MB2_TAG_ACPI_NEW:
        if (size >= 8 + 36) {
            k_memcpy(st->rsdp, t + 8, 36);
            st->rsdp_len = 36;
        }
        break;
    default:
        break;
    }
}

int mb2_read(const void *mbi, struct boot_info *boot, struct boot_store *st)
{
    const uint8_t *p = (const uint8_t *)mbi;
    uint32_t total = le32(p);
    uint32_t off, size;

    reset(boot, st);
    if (total < 16 || total > MB2_INFO_MAX) {
        return -1;
    }
    for (off = 8; off + 8 <= total; off += (size + 7) & ~7u) {
        size = le32(p + off + 4);
        if (size < 8 || size > total - off) {
            return -1;
        }
        if (le32(p + off) == MB2_TAG_END) {
            return 0;
        }
        mb2_tag(p + off, size, boot, st);
    }
    return -1;
}
