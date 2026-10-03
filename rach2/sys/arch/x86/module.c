#include "../../klog.h"
#include "../../module.h"
#include "../../lib/kstring.h"

module_info_t module_table[MAX_MODULES];

uint32_t module_count = 0;

/**
 * Record the boot modules of the boot record in `module_table`: each one's
 * physical range, where the kernel reads it until the boot maps it, and its
 * command line, the record's own copy. Both of Multiboot's forms give a
 * module's addresses in 32 bits.
 *
 * @return the number of modules recorded.
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

    /* The loader's view. `modules:` later says what the machine made of them,
       which is the line a person wants; two subsystems reporting the same
       count under near-identical names is what a boot log looked like. */
    klog(KLOG_DEBUG, "module", "found %d", boot->modules_passed);

    for (i = 0; i < boot->modules && i < MAX_MODULES; i++) {
        const struct boot_module *bm = &boot->module[i];
        module_info_t *mod = &module_table[i];

        mod->start = (uintptr_t)bm->start;
        mod->end = (uintptr_t)bm->end;
        mod->size = (uint32_t)(bm->end - bm->start);
        mod->phys = (uint32_t)bm->start;
        mod->cmdline = bm->cmdline;

        /* The empty brackets at the end of this line are a module with no
           command line, which is every module this loader is given. */
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
