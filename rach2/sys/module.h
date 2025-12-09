#ifndef _MODULE_H_
#define _MODULE_H_

#include "lib/kint.h"
#include "lib/kdef.h"
#include "../vendor/multiboot.h"

/*
 * Maximum number of modules we support
 */
#define MAX_MODULES 8

/*
 * Module information structure
 */
struct module_info {
    uint32_t start;     /* Physical start address */
    uint32_t end;       /* Physical end address (exclusive) */
    uint32_t size;      /* Size in bytes (end - start) */
    const char *cmdline;/* Module command line (may be NULL) */
};

typedef struct module_info module_info_t;

/*
 * Module table
 */
extern module_info_t module_table[MAX_MODULES];

/*
 * Number of loaded modules
 */
extern uint32_t module_count;

/*
 * Initialize module subsystem
 * Parses multiboot info structure and populates module_table
 * Returns number of modules found, or 0 if none/error
 */
uint32_t init_modules(multiboot_uint32_t magic, multiboot_info_t *info);

/*
 * Get module by index
 * Returns pointer to module_info, or NULL if index out of range
 */
module_info_t *get_module(uint32_t index);

/*
 * Get first module (convenience for single-module use case like RAM disk)
 * Returns pointer to module_info, or NULL if no modules
 */
module_info_t *get_ramdisk(void);

#endif
