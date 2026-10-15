#ifndef _X86_MBINFO_H_
#define _X86_MBINFO_H_

/*
 * mbinfo: the loader's information, in Multiboot's form or multiboot2's,
 * read into the boot record (boot.h).
 *
 * A Multiboot loader passes one structure of fixed fields, with pointers to
 * the command line, the modules and the memory map wherever it put them. A
 * multiboot2 loader passes a list of tags, each a type, a size and its data
 * in place, 8-byte aligned, ending in a tag of type 0. Either way the record
 * gets copies of what it keeps, in `struct boot_store`, since the loader's
 * memory goes to the frame allocator once the boot has read it.
 *
 * No kernel service is called, so tests/host/mbinfo_hosttest.c reads tag
 * lists the test builds.
 */

#include "../../boot.h"
#include "../../../vendor/multiboot.h"

/* The magic numbers a loader leaves in EAX: Multiboot's and multiboot2's. */
#define MB1_LOADER_MAGIC 0x2BADB002u
#define MB2_LOADER_MAGIC 0x36D76289u

/* The longest multiboot2 tag list read. A loader's is a few kibibytes, the
 * EFI memory map most of it; a length past this is a corrupt one. */
#define MB2_INFO_MAX 0x100000u

/* The kernel's copies of what the record points at. */
struct boot_store {
    char cmdline[BOOT_CMDLINE_MAX];
    char loader[64];
    char module_cmdline[BOOT_MODULE_MAX][BOOT_MODULE_CMDLINE_MAX];
    /* The map in Multiboot's form: BOOT_MMAP_ENTRY bytes an entry, a size
     * word first, then the base, the length and the type. */
    uint8_t mmap[BOOT_MMAP_MAX * BOOT_MMAP_ENTRY];
    uint32_t mmap_entries;
    /* Entries past BOOT_MMAP_MAX that were not kept. */
    uint32_t mmap_cut;
    /* Whether the map came from the EFI memory map. */
    int mmap_efi;
    /* ACPI's RSDP as the loader passed it: 20 bytes of the first revision
     * or 36 of the second, or 0 when it passed none. */
    uint8_t rsdp[36];
    uint32_t rsdp_len;
};

/* A physical address the loader gave, as a pointer the entry can read, or
 * NULL when `len` bytes there are past what it reaches. */
typedef const void *(*mb_reach_fn)(uint64_t phys, uint32_t len);

/**
 * Fill `boot` and `st` from a Multiboot information structure, following its
 * pointers through `reach`. Returns 0, or -1 when the command line, the
 * module list or the memory map cannot be read or the map is malformed,
 * with the rest read. This and `mb2_read` clear `boot` and `st` first, and
 * the strings and the map `boot` points at are the copies in `st`.
 */
int mb1_read(const multiboot_info_t *info, mb_reach_fn reach, struct boot_info *boot,
             struct boot_store *st);

/**
 * Fill `boot` and `st` from a multiboot2 tag list. Returns 0, or -1 when the
 * list's length is under 16 bytes or past MB2_INFO_MAX, or the list is cut
 * short or a tag runs past its end, with the tags before it read.
 *
 * The EFI memory map, which a loader on UEFI passes beside its own map,
 * takes the place of that map: it is the firmware's at the exit from its
 * boot services. Its types become Multiboot's, the memory the loader and
 * the boot services held counted as usable, since neither runs again.
 */
int mb2_read(const void *mbi, struct boot_info *boot, struct boot_store *st);

#endif /* _X86_MBINFO_H_ */
