/**
 * printk_hosttest.c: the printk module's parameters of rach2/sys/printk.c,
 * read from the boot line through rach2/sys/cmdline.c and published in
 * /sys/module/printk/parameters through rach2/sys/fs/sysfs.c.
 *
 * host-deps: rach2/sys/cmdline.c rach2/sys/fs/sysfs.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 *
 * The test includes printk.c whole, to read and reset its parameters. Its
 * allocator fails the allocation a row names, so that a row reaches each
 * step at which /sys/module/printk/parameters can fail: the anchor of /sys
 * and the table's first growth, the directories module, printk and
 * parameters, and each of the five files.
 */
#include "printk.c"

#include "fs/synthfs.h"
#include "lib/kstring.h"
#include "mm/kheap.h"

/* The host's C library, declared here since its headers define the
 * kernel's types again. */
extern int printf(const char *format, ...);
extern void *malloc(unsigned long size);
extern void free(void *ptr);

static int failures;

/* klog's two parameters that printk.c reaches through its setters. */
static int ignore_loglevel_on;
static int time_on = 1;

void klog_set_ignore_loglevel(int on)
{
    ignore_loglevel_on = on;
}

int klog_ignore_loglevel(void)
{
    return ignore_loglevel_on;
}

void klog_set_time(int on)
{
    time_on = on;
}

int klog_time(void)
{
    return time_on;
}

/* The warnings logged, which printk_params_start gives for a failure. */
static int warnings;

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level == KLOG_WARN) {
        warnings++;
    }
}

void netlink_uevent(const uint8_t *msg, uint32_t len)
{
    (void)msg;
    (void)len;
}

/** No row reaches a path or a PCI function's name; each is empty. */
void synth_sys_path(int node, int key, char *out, int cap)
{
    (void)node;
    (void)key;
    if (cap > 0) {
        out[0] = '\0';
    }
}

void synth_sys_device_name(int node, int key, char *out, int cap)
{
    synth_sys_path(node, key, out, cap);
}

/* The allocations left before the one that fails, or -1 for none. */
static int fail_in = -1;

/** The host's allocation, or NULL for the one `fail_in` names. */
void *kmalloc(size_t size)
{
    if (fail_in >= 0 && fail_in-- == 0) {
        return NULL;
    }
    return malloc(size);
}

/** The host's zeroed allocation, failing as `kmalloc`. */
void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
    return p;
}

/** Free what `kmalloc` or `kzalloc` gave. */
void kfree(void *ptr)
{
    free(ptr);
}

/** Count a row, and name it when it fails. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("printk-hosttest: failed: %s\n", what);
        failures++;
    }
}

/** Read the boot line `line` from the defaults, as a boot does; the value
 * of `console_suspend` it leaves. */
static int suspend_after(const char *line)
{
    console_suspend = 1;
    kcmdline_capture(line);
    printk_params_early();
    return console_suspend;
}

/**
 * `no_console_suspend` and `printk.console_suspend=` in the line's order,
 * as Linux's parse_args takes them, the last counting; and the early
 * `ignore_loglevel`, which is read before every `printk.` word.
 */
static void order_rows(void)
{
    check(suspend_after("printk.console_suspend=Y no_console_suspend") == 0,
          "printk.console_suspend=Y then no_console_suspend leaves N");
    check(suspend_after("no_console_suspend printk.console_suspend=Y") == 1,
          "no_console_suspend then printk.console_suspend=Y leaves Y");
    check(suspend_after("no_console_suspend printk.console_suspend") == 1,
          "and so does a bare printk.console_suspend after it");
    check(suspend_after("printk.console_suspend=N no_console_suspendx printk.time=N") == 0 &&
              time_on == 0,
          "a word that begins with no_console_suspend counts, as obsolete_checksetup");
    check(suspend_after("printk.console_suspend=Y -- no_console_suspend") == 1,
          "and none after a bare --, which is init's");
    ignore_loglevel_on = 0;
    check(suspend_after("printk.ignore_loglevel=N ignore_loglevel") == 1 && ignore_loglevel_on == 0,
          "printk.ignore_loglevel=N counts over an early ignore_loglevel after it");
    time_on = 1;
    kcmdline_capture("");
}

/** The directory `name` below `dir`, or 0. */
static int child(int dir, const char *name)
{
    int h = sysfs_node_child(dir, name, (int)k_strlen(name));

    return h > 0 ? h : 0;
}

/** /sys/module, or 0. */
static int module_dir(void)
{
    int root = sysfs_anchor_find(SYSFS_ROOT, 0);

    return root > 0 ? child(root, "module") : 0;
}

/** Whether /sys/module/printk/parameters has its five files. */
static int printk_whole(void)
{
    int dir = child(child(module_dir(), "printk"), "parameters"), i;

    if (!dir || sysfs_node_count(dir) != PRINTK_NR_PARAMS) {
        return 0;
    }
    for (i = 0; i < PRINTK_NR_PARAMS; i++) {
        if (!child(dir, params[i].name)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Fail each allocation of printk_params_start in turn, from the first,
 * until one call succeeds: each failure is logged and leaves no part of
 * printk's tree, and with `kvm` 1 leaves /sys/module/kvm/parameters/tdp_mmu
 * and the /sys/module above it, which a module made before; with `kvm` 0,
 * /sys/module is not there before the call, and a failure leaves none. The
 * number of steps that failed, or -1 when a row failed.
 */
static int rollback_rows(int kvm, int kvm_file)
{
    int k, steps = 0, ok = 1, mod;

    for (k = 0; k < 64; k++) {
        warnings = 0;
        fail_in = k;
        printk_params_start();
        fail_in = -1;
        if (warnings == 0) {
            break;
        }
        steps++;
        mod = module_dir();
        if (kvm) {
            ok = ok && mod && sysfs_node_count(mod) == 1 && !child(mod, "printk") &&
                 sysfs_node_kind(kvm_file) == SYSFS_NODE_ATTR &&
                 child(child(child(mod, "kvm"), "parameters"), "tdp_mmu") == kvm_file;
        } else {
            ok = ok && !mod;
        }
    }
    check(k < 64 && warnings == 0 && printk_whole(), "a call with no failure makes the five files");
    return ok ? steps : -1;
}

/**
 * A failure at each step of /sys/module/printk/parameters leaves no part of
 * the tree: a failure to make /sys's anchor or to grow the table for it,
 * which stays once made, leaves neither; one at a later step leaves none of
 * the directories the call made, the parent of `parameters` included, and
 * none of the files made before the one that failed. Made after KVM's, a
 * failure leaves /sys/module and KVM's file as they were.
 */
static void sysfs_rows(void)
{
    static const struct sysfs_attr tdp = {"tdp_mmu", 0444u, NULL, NULL};
    int steps, made, dir, file, k, ok = 1;

    for (k = 0; k < 3; k++) {
        warnings = 0;
        fail_in = k;
        printk_params_start();
        fail_in = -1;
        ok = ok && warnings == 1 && sysfs_anchor_find(SYSFS_ROOT, 0) == 0;
    }
    check(ok, "a failure to make /sys's anchor, or to grow the table for it, is logged and makes "
              "neither");
    check(sysfs_anchor(SYSFS_ROOT, 0) > 0, "and the anchor is made with no failure");
    steps = rollback_rows(0, 0);
    check(steps == 3 + PRINTK_NR_PARAMS,
          "with no /sys/module, the three directories and the five files each fail alone and "
          "leave nothing");
    sysfs_remove(module_dir());

    dir = sysfs_module_params("kvm", &made);
    file = dir > 0 ? sysfs_add_attr(dir, &tdp, NULL, 0) : -1;
    check(dir > 0 && made == module_dir() && file > 0, "KVM makes /sys/module/kvm/parameters");
    steps = rollback_rows(1, file);
    check(steps == 2 + PRINTK_NR_PARAMS,
          "after KVM's, the two directories and the five files each fail alone, leaving KVM's");
    check(sysfs_module_params("kvm", &made) == dir && made == 0,
          "a second call finds KVM's directory and makes none");
}

int main(void)
{
    order_rows();
    sysfs_rows();
    if (failures) {
        printf("printk-hosttest: result: fail: %d\n", failures);
        return 1;
    }
    printf("printk-hosttest: result: pass: boot-line order of no_console_suspend and "
           "printk.console_suspend, and /sys/module/printk/parameters left whole or not at all "
           "at each allocation failure\n");
    return 0;
}
