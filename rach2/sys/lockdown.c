/*
 * lockdown.c: Linux's kernel lockdown level, from the boot's `lockdown=` and
 * /sys/kernel/security/lockdown, and the check of an operation against it.
 */
#include "lockdown.h"

#include "cmdline.h"
#include "fs/sysfs.h"
#include "fs/synthfs.h"
#include "klog.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"

/* The level in force, a LOCKDOWN_* level, or -1 before the boot's is read. */
static int level = -1;

/* The names of the three levels, Linux's, in their order. */
static const struct {
    const char *name;
    int level;
} levels[] = {{"none", LOCKDOWN_NONE},
              {"integrity", LOCKDOWN_INTEGRITY_MAX},
              {"confidentiality", LOCKDOWN_CONFIDENTIALITY_MAX}};

/* What the log names an operation refused for, by its number: Linux's
 * words, and for /proc/acpi/call, which Linux has only from the unsigned
 * acpi_call module that lockdown keeps from loading, the module's. */
static const char *const reasons[] = {
    "none",           "/dev/mem,kmem,port",      "direct PCI access",
    "raw MSR access", "unsigned module loading", "debugfs access",
    "integrity",      "confidentiality"};

/** The level in force, read from the command line on the first call. */
static int level_now(void)
{
    if (level < 0) {
        level = kcmdline_has("lockdown=confidentiality") ? LOCKDOWN_CONFIDENTIALITY_MAX
                : kcmdline_has("lockdown=integrity")     ? LOCKDOWN_INTEGRITY_MAX
                                                         : LOCKDOWN_NONE;
    }
    return level;
}

int lockdown_refuses(int what)
{
    if (what <= LOCKDOWN_NONE || what >= LOCKDOWN_CONFIDENTIALITY_MAX || level_now() < what) {
        return 0;
    }
    klog(KLOG_WARN, "lockdown", "refused: %s is restricted; see man kernel_lockdown.7",
         reasons[what]);
    return -EPERM;
}

/** Show the levels, the one in force in brackets, as Linux's `lockdown_read`:
 * "[none] integrity confidentiality" and a newline. */
static int32_t lockdown_show(void *obj, int arg, char *buf, uint32_t cap)
{
    char text[64];
    uint32_t n = 0, i, len;

    (void)obj;
    (void)arg;
    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        int now = levels[i].level == level_now();

        len = (uint32_t)k_strlen(levels[i].name);
        if (now) {
            text[n++] = '[';
        }
        k_memcpy(text + n, levels[i].name, len);
        n += len;
        if (now) {
            text[n++] = ']';
        }
        text[n++] = ' ';
    }
    text[n - 1] = '\n';
    if (n <= cap) {
        k_memcpy(buf, text, n);
    }
    return (int32_t)n;
}

/** Whether the `n` bytes at `buf` are the string `name`. */
static int text_is(const char *buf, uint32_t n, const char *name)
{
    uint32_t i;

    for (i = 0; i < n && name[i] != '\0'; i++) {
        if (buf[i] != name[i]) {
            return 0;
        }
    }
    return i == n && name[i] == '\0';
}

/**
 * Take a level written to the file, with or without a newline, as Linux's
 * `lockdown_write`: `len`, or -EPERM for a level not above the one in
 * force, which no write lowers, and -EINVAL for text that names no level.
 * The writer is whoever the file's mode lets open it for writing, root
 * alone at its 0644, with no capability asked, as on Linux.
 */
static int32_t lockdown_store(void *obj, int arg, const char *buf, uint32_t len)
{
    uint32_t n = len, i;

    (void)obj;
    (void)arg;
    if (n > 0 && buf[n - 1] == '\n') {
        n--;
    }
    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (text_is(buf, n, levels[i].name)) {
            if (level_now() >= levels[i].level) {
                return -EPERM;
            }
            level = levels[i].level;
            klog(KLOG_INFO, "lockdown", "kernel: locked down by securityfs");
            return (int32_t)len;
        }
    }
    return -EINVAL;
}

void lockdown_sysfs_start(void)
{
    static const struct sysfs_attr attr = {"lockdown", 0644u, lockdown_show, lockdown_store};
    int dir = sysfs_add_dir(sysfs_anchor(SYSFS_KERNEL, 0), "security");

    if (dir > 0) {
        (void)sysfs_add_attr(dir, &attr, NULL, 0);
    }
}
