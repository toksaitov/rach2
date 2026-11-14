/*
 * printk.c: the printk module's parameters, read from the boot line and
 * shown in /sys/module/printk/parameters with Linux's names, modes and text.
 */

#include "printk.h"

#include "cmdline.h"
#include "fs/sysfs.h"
#include "klog.h"
#include "lib/kdef.h"
#include "lib/kerrno.h"

/*
 * The parameters that change nothing in Rach2, kept so that a program that
 * saves one, sets it and puts it back, as IGT's suspend tests do with
 * `console_suspend`, reads what it wrote. `console_suspend`, Linux's Y,
 * would stop the consoles while the machine sleeps (`suspend_console`);
 * Rach2's suspend to idle (proc/suspend.c) leaves the screen, the serial
 * port and netconsole running. `console_no_auto_verbose`, N, would keep an
 * oops from raising the console's level (`console_verbose`); Rach2
 * survives no oops, and its panic draws its own report on the screen while
 * the port takes every level. `always_kmsg_dump`, N, would let a dumper
 * write the log at a reboot, a halt or a power-off; ramoops, Rach2's only
 * one, writes it at a panic alone.
 */
static int console_suspend = 1;
static int console_no_auto_verbose;
static int always_kmsg_dump;

/* The parameters in Linux's order of /sys/module/printk/parameters, each
 * with its variable or its klog setter and getter. */
static const struct printk_param {
    const char *name;
    int *held;           /* the value, for one that changes nothing */
    void (*set)(int on); /* or klog's setter */
    int (*get)(void);    /* and getter */
} params[] = {{"always_kmsg_dump", &always_kmsg_dump, NULL, NULL},
              {"console_no_auto_verbose", &console_no_auto_verbose, NULL, NULL},
              {"console_suspend", &console_suspend, NULL, NULL},
              {"ignore_loglevel", NULL, klog_set_ignore_loglevel, klog_ignore_loglevel},
              {"time", NULL, klog_set_time, klog_time}};

#define PRINTK_NR_PARAMS ((int)(sizeof(params) / sizeof(params[0])))

/* The entries of `params` that a boot-line word of their own also sets. */
#define PARAM_CONSOLE_SUSPEND 2
#define PARAM_IGNORE_LOGLEVEL 3

/** The value of `params[i]`, 1 or 0. */
static int param_get(int i)
{
    return params[i].held ? *params[i].held : params[i].get();
}

/** Set `params[i]` to `on`, 1 or 0. */
static void param_set(int i, int on)
{
    if (params[i].held) {
        *params[i].held = on;
    } else {
        params[i].set(on);
    }
}

void printk_params_early(void)
{
    char key[40] = "printk.";
    int i, j;

    if (kcmdline_early("ignore_loglevel")) {
        param_set(PARAM_IGNORE_LOGLEVEL, 1);
    }
    if (kcmdline_setup("no_console_suspend")) {
        param_set(PARAM_CONSOLE_SUSPEND, 0);
    }
    for (i = 0; i < PRINTK_NR_PARAMS; i++) {
        for (j = 0; params[i].name[j] != '\0'; j++) {
            key[7 + j] = params[i].name[j];
        }
        key[7 + j] = '\0';
        param_set(i, kcmdline_param_bool(key, param_get(i)));
    }
}

/** A parameter's file: `Y` or `N` and a newline, as Linux's param_get_bool
 * writes it. */
static int32_t param_show(void *obj, int arg, char *buf, uint32_t cap)
{
    (void)obj;
    if (cap >= 2u) {
        buf[0] = param_get(arg) ? 'Y' : 'N';
        buf[1] = '\n';
    }
    return 2;
}

/** A write of a parameter's file, which Linux's param_set_bool reads with
 * `kstrtobool`, from its first two bytes: `len`, or -EINVAL for a text that
 * is no boolean, which leaves the value as it was. */
static int32_t param_store(void *obj, int arg, const char *buf, uint32_t len)
{
    char text[3];
    uint32_t i;
    int on;

    (void)obj;
    for (i = 0; i < 2u && i < len; i++) {
        text[i] = buf[i];
    }
    text[i] = '\0';
    if (kcmdline_parse_bool(text, &on) != 0) {
        return -EINVAL;
    }
    param_set(arg, on);
    return (int32_t)len;
}

void printk_params_start(void)
{
    struct sysfs_attr attr;
    int dir = sysfs_module_params("printk"), i, h = 0;

    attr.mode = 0644u;
    attr.show = param_show;
    attr.store = param_store;
    for (i = 0; dir >= 0 && h >= 0 && i < PRINTK_NR_PARAMS; i++) {
        attr.name = params[i].name;
        h = sysfs_add_attr(dir, &attr, NULL, i);
    }
    if (dir < 0 || h < 0) {
        klog(KLOG_WARN, "printk", "failed: /sys/module/printk not made: %d", dir < 0 ? dir : h);
    }
}
