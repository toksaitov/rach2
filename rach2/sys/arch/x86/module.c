/* module.c: the boot modules the loader passed, as module.h describes them. */
#include "../../klog.h"
#include "../../module.h"
#include "../../lib/kstring.h"

module_info_t module_table[MAX_MODULES];

uint32_t module_count = 0;

/**
 * Each module's start and end are its physical range, where the kernel reads
 * it until the boot maps it, and its command line is the boot record's own
 * copy. Both of Multiboot's forms give a module's addresses in 32 bits. The
 * count the loader passed is logged as the loader's view; the boot's
 * `modules:` line later says what the kernel made of them.
 */
uint32_t init_modules(const struct boot_info *boot)
{
    uint32_t i;

    k_memset(module_table, 0, sizeof(module_table));
    module_count = 0;

    if (boot->modules == 0) {
        klog(KLOG_DEBUG, "module", "none: the loader carried no module");
        return 0;
    }

    klog(KLOG_DEBUG, "module", "found %d", boot->modules_passed);

    for (i = 0; i < boot->modules && i < MAX_MODULES; i++) {
        const struct boot_module *bm = &boot->module[i];
        module_info_t *mod = &module_table[i];

        mod->start = (uintptr_t)bm->start;
        mod->end = (uintptr_t)bm->end;
        mod->size = (uint32_t)(bm->end - bm->start);
        mod->phys = (uint32_t)bm->start;
        mod->cmdline = bm->cmdline;

        klog(KLOG_DEBUG, "module", "%d: 0x%X to 0x%X, %d bytes [%s]", i, mod->start, mod->end,
             mod->size, mod->cmdline ? mod->cmdline : "");

        module_count++;
    }

    if (boot->modules_passed > boot->modules) {
        klog(KLOG_WARN, "module", "%d ignored: the table holds %d",
             boot->modules_passed - boot->modules, MAX_MODULES);
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
