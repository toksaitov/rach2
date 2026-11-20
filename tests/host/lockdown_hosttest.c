/* The kernel lockdown level of rach2/sys/lockdown.c: the boot's `lockdown=`,
 * the text of /sys/kernel/security/lockdown, the writes it takes and
 * refuses, and the operations each level refuses.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "lockdown.c"

int printf(const char *format, ...);

static int failures;
/* The model's command line, a word `kcmdline_has` finds, or "". */
static const char *cmdline = "";
/* What the model's sysfs was given. */
static const struct sysfs_attr *added;
static int dir_parent;

#define CHECK(c, s)                                 \
    do {                                            \
        if (!(c)) {                                 \
            printf("lockdown-test: fail: %s\n", s); \
            failures++;                             \
        }                                           \
    } while (0)

/** Whether the model's command line is the word `flag`. */
int kcmdline_has(const char *flag)
{
    return text_is(flag, (uint32_t)k_strlen(flag), cmdline);
}

/** The log is not kept. */
void klog(int lvl, const char *subsystem, const char *format, ...)
{
    (void)lvl;
    (void)subsystem;
    (void)format;
}

/** The anchor of /sys/kernel is 1 in the model. */
int sysfs_anchor(int node, int key)
{
    (void)key;
    return node == SYSFS_KERNEL ? 1 : -ENOENT;
}

/** Keep the parent of the one directory made; its handle is 2. */
int sysfs_add_dir(int parent, const char *name)
{
    dir_parent = parent;
    return text_is(name, (uint32_t)k_strlen(name), "security") ? 2 : -EINVAL;
}

/** Keep the one attribute added below the directory 2. */
int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)obj;
    (void)arg;
    added = parent == 2 ? attr : NULL;
    return 3;
}

/** The file's text now, in `buf` of 64 bytes, NUL-terminated. */
static void shown(char *buf)
{
    int32_t n = added->show(NULL, 0, buf, 63);

    buf[n > 0 && n < 64 ? n : 0] = '\0';
}

/** Write the string `s` to the file as root or another caller. */
static int32_t written(const char *s)
{
    return added->store(NULL, 0, s, (uint32_t)k_strlen(s));
}

int main(void)
{
    char buf[64];

    cmdline = "lockdown=integrity";
    CHECK(lockdown_refuses(LOCKDOWN_DEBUGFS) == -EPERM &&
              lockdown_refuses(LOCKDOWN_DEV_MEM) == -EPERM,
          "lockdown=integrity refuses debugfs and /dev/mem");
    level = -1;
    cmdline = "";
    CHECK(lockdown_refuses(LOCKDOWN_DEBUGFS) == 0 && lockdown_refuses(LOCKDOWN_MSR) == 0,
          "with no lockdown= nothing is refused");
    lockdown_sysfs_start();
    CHECK(dir_parent == 1 && added != NULL && added->mode == 0644u,
          "/sys/kernel/security/lockdown is made, mode 0644");
    if (!added) {
        return 1;
    }
    shown(buf);
    CHECK(text_is(buf, (uint32_t)k_strlen(buf), "[none] integrity confidentiality\n"),
          "the file shows none in force");
    CHECK(written("integrit") == -EINVAL && written("none \n") == -EINVAL,
          "text that names no level is EINVAL");
    CHECK(written("none") == -EPERM, "none is not above none");
    CHECK(written("integrity\n") == 10, "integrity is taken with its newline");
    shown(buf);
    CHECK(text_is(buf, (uint32_t)k_strlen(buf), "none [integrity] confidentiality\n"),
          "the file shows integrity in force");
    CHECK(lockdown_refuses(LOCKDOWN_DEBUGFS) == -EPERM && lockdown_refuses(LOCKDOWN_MSR) == -EPERM,
          "and debugfs and MSR writes are refused");
    CHECK(written("integrity") == -EPERM && written("none") == -EPERM,
          "no write lowers the level or sets it again");
    CHECK(written("confidentiality") == 15, "confidentiality is taken");
    shown(buf);
    CHECK(text_is(buf, (uint32_t)k_strlen(buf), "none integrity [confidentiality]\n"),
          "the file shows confidentiality in force");
    CHECK(added->show(NULL, 0, buf, 4) == 33, "a short buffer is told the length");
    if (!failures) {
        printf("lockdown-test: passed\n");
    }
    return failures != 0;
}
