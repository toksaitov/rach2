#ifndef _MODULE_H_
#define _MODULE_H_

#include "lib/kint.h"
#include "lib/kdef.h"
#include "../vendor/multiboot.h"

/* Policy: the boot modules recorded. A boot here passes one or two, the
 * root image and a second file system; what a loader passes past eight is
 * left unread, with a record saying how many. */
#define MAX_MODULES 8

/* One boot module the bootloader handed us. */
struct module_info {
    uint32_t start;     /* Where the kernel reads the module: its physical
                           address until the boot maps it, then the address
                           in the module region of the kernel's layout */
    uint32_t end;       /* One past the module, in the same terms */
    uint32_t size;      /* Size in bytes (end - start) */
    uint32_t phys;      /* Physical start address, wherever the loader put it */
    const char *cmdline;/* Module command line (may be NULL) */
};

typedef struct module_info module_info_t;

/* Recorded modules, the first `module_count` of them valid. */
extern module_info_t module_table[MAX_MODULES];

/* Number of valid entries in `module_table`. */
extern uint32_t module_count;

/** Record the boot modules from Multiboot; returns the count recorded. */
uint32_t init_modules(multiboot_uint32_t magic, multiboot_info_t *info);

/**
 * Return the module at `index`, or NULL if `index` is out of range.
 */
module_info_t *get_module(uint32_t index);

/**
 * Return the first module, or NULL if none were recorded.
 *
 * A convenience for the single-module case such as the RAM disk.
 */
module_info_t *get_ramdisk(void);

#endif
