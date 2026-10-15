/*
 * boot.h: the boot record, what the loader handed the kernel, in the form the
 * shared boot reads, filled by the architecture's entry from its loader's own
 * form (arch/x86/boot.c, from Multiboot or multiboot2) before it calls
 * `kernel_main`. Every string and table it points at is the kernel's own
 * copy, so the loader's memory is free once the entry has read it.
 */
#ifndef _BOOT_H_
#define _BOOT_H_

#include "lib/kint.h"

/* The most memory-map entries, modules, and bytes of a command line the
 * record keeps. A loader's map past BOOT_MMAP_MAX entries is cut there, and
 * the entry says so in the log. */
#define BOOT_MMAP_MAX           256
#define BOOT_MODULE_MAX         8
#define BOOT_CMDLINE_MAX        512
#define BOOT_MODULE_CMDLINE_MAX 128

/* The bytes of one memory-map entry in the record: Multiboot's form, a
 * 32-bit size of what follows (20), a 64-bit base, a 64-bit length and a
 * 32-bit type, 1 for usable memory, 3 for ACPI's reclaimable tables, 4 for
 * ACPI's non-volatile storage, 5 for bad memory and 2 for anything else. */
#define BOOT_MMAP_ENTRY 24

/* What the loader left on the screen. */
enum boot_fb_type {
    BOOT_FB_NONE = 0, /* nothing the loader described */
    BOOT_FB_RGB,      /* pixels of direct color, by the fields below */
    BOOT_FB_INDEXED,  /* pixels that index `palette` */
    BOOT_FB_TEXT      /* the VGA's text mode, cells at `addr` */
};

/* The screen the loader set up: where it is, its size, and how a pixel
 * holds a color. For BOOT_FB_TEXT, width and height count character cells. */
struct boot_framebuffer {
    uint64_t addr;  /* physical address of the first pixel or cell */
    uint32_t pitch; /* bytes from the start of one line to the next */
    uint32_t width;
    uint32_t height;
    uint8_t bpp; /* bits per pixel */
    uint8_t type;
    /* BOOT_FB_RGB: each color's lowest bit in a pixel and its width. */
    uint8_t red_shift, red_bits;
    uint8_t green_shift, green_bits;
    uint8_t blue_shift, blue_bits;
    /* BOOT_FB_INDEXED: the palette's entries, red, green and blue each from
     * 0 to 255, and how many the loader gave. */
    uint16_t colors;
    uint8_t palette[256][3];
};

/* One boot module: where the loader put it, and its command line. */
struct boot_module {
    uint64_t start; /* physical address of the first byte */
    uint64_t end;   /* one past the last */
    const char *cmdline;
};

struct boot_info {
    /* The kernel command line, or NULL when the loader gave none. */
    const char *cmdline;
    /* The loader's name for itself, or NULL. */
    const char *loader;
    /* The firmware's memory map, `mmap_len` bytes of entries of
     * BOOT_MMAP_ENTRY bytes, or NULL with no map. */
    const void *mmap;
    uint32_t mmap_len;
    /* The kibibytes of memory above 1 MiB, for a loader that gave no map. */
    uint32_t mem_upper_kb;
    /* The boot modules, which `init_modules` (module.h) records for
     * `get_module`: how many the record holds and how many the loader
     * passed, which is more when it passed over BOOT_MODULE_MAX. */
    uint32_t modules;
    uint32_t modules_passed;
    struct boot_module module[BOOT_MODULE_MAX];
    /* The physical address of ACPI's root system description pointer, the
     * firmware's own or the kernel's copy of the one a loader passed, or 0,
     * and the platform then searches the BIOS area for it. */
    uint64_t acpi_rsdp;
    /* The physical address of the UEFI system table a loader passed, or 0,
     * and whether its pointers are 64 bits wide: the firmware's
     * configuration table, which gives SMBIOS's entry point, is found
     * through it. */
    uint64_t efi_systab;
    int efi_systab_64;
    /* The screen. */
    struct boot_framebuffer fb;
    /* Whether the machine booted by UEFI, which the loader says by passing
     * the firmware's system table or memory map. */
    int uefi;
};

/** The shared boot, from the architecture's entry: brings the kernel up and
 * runs init or the debug shell. Does not return. */
void kernel_main(const struct boot_info *boot);

#endif /* _BOOT_H_ */
