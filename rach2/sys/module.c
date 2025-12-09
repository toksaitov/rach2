#include "module.h"
#include "serial.h"
#include "lib/kstring.h"

/*
 * Module table - stores info about all loaded modules
 */
module_info_t module_table[MAX_MODULES];

/*
 * Number of modules found
 */
uint32_t module_count = 0;

uint32_t init_modules(multiboot_uint32_t magic, multiboot_info_t *info)
{
    multiboot_module_t *mods;
    uint32_t i;

    /* Clear module table */
    k_memset(module_table, 0, sizeof(module_table));
    module_count = 0;

    /* Verify multiboot magic */
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        serial_printf("Module: Invalid multiboot magic: 0x%X\n", magic);
        return 0;
    }

    /* Check if module info is available */
    if (!(info->flags & MULTIBOOT_INFO_MODS)) {
        serial_printf("Module: No module info flag set\n");
        return 0;
    }

    /* Check if any modules were loaded */
    if (info->mods_count == 0) {
        serial_printf("Module: No modules loaded\n");
        return 0;
    }

    serial_printf("Module: Found %d module(s)\n", info->mods_count);

    /* Get pointer to module array */
    mods = (multiboot_module_t *)info->mods_addr;

    /* Parse each module */
    for (i = 0; i < info->mods_count && i < MAX_MODULES; i++) {
        module_table[i].start = mods[i].mod_start;
        module_table[i].end = mods[i].mod_end;
        module_table[i].size = mods[i].mod_end - mods[i].mod_start;
        module_table[i].cmdline = (mods[i].cmdline != 0) ?
                                  (const char *)mods[i].cmdline : NULL;

        serial_printf("  Module %d: 0x%X - 0x%X (%d bytes)",
                      i, module_table[i].start, module_table[i].end,
                      module_table[i].size);

        if (module_table[i].cmdline) {
            serial_printf(" [%s]", module_table[i].cmdline);
        }
        serial_printf("\n");

        module_count++;
    }

    if (info->mods_count > MAX_MODULES) {
        serial_printf("Module: Warning - %d modules ignored (max %d)\n",
                      info->mods_count - MAX_MODULES, MAX_MODULES);
    }

    return module_count;
}

module_info_t *get_module(uint32_t index)
{
    if (index >= module_count) {
        return NULL;
    }
    return &module_table[index];
}

module_info_t *get_ramdisk(void)
{
    return get_module(0);
}
