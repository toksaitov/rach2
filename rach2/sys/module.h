/*
 * module.h: the boot modules the loader passed, the first of them the root
 * image, as the kernel records them. arch/x86/module.c defines them.
 */
#ifndef _MODULE_H_
#define _MODULE_H_

#include "lib/kint.h"
#include "lib/kdef.h"
#include "boot.h"

/* The most boot modules recorded, the boot record's bound; the modules a
 * loader passes past it are left unread, with a record in the log saying how
 * many. */
#define MAX_MODULES BOOT_MODULE_MAX

/* One boot module the loader passed. */
struct module_info {
    uintptr_t start;     /* Where the kernel reads the module: its physical
                            address until the boot maps it, then the address
                            in the module region of the kernel's layout */
    uintptr_t end;       /* One past the module, in the same terms */
    uint32_t size;       /* Size in bytes (end - start) */
    uint32_t phys;       /* Physical start address, wherever the loader put it */
    const char *cmdline; /* Module command line (may be NULL) */
};

typedef struct module_info module_info_t;

/* Recorded modules, the first `module_count` of them valid. */
extern module_info_t module_table[MAX_MODULES];

/* Number of valid entries in `module_table`. */
extern uint32_t module_count;

/**
 * Record the modules of the boot record in `module_table`, their physical
 * ranges and command lines, and log each; returns the count recorded. The
 * command line is the boot record's own copy.
 */
uint32_t init_modules(const struct boot_info *boot);

/**
 * Return the module at `index`, or NULL if `index` is out of range.
 */
module_info_t *get_module(uint32_t index);

/**
 * Return the first module, the root image, or NULL if none were recorded.
 */
module_info_t *get_ramdisk(void);

#endif
