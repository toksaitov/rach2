/*
 * synthfs.c: the pure path classifier for the synthetic /dev and /proc mounts.
 *
 * No kernel dependencies: it turns a path string into a vnode kind plus the
 * coordinates the glue needs. The component splitting and the
 * /proc/<pid>/fd/<n> parsing are the fence-post-prone parts, so this
 * compiles freestanding AND hosted and is unit-tested under ASan/UBSan
 * (tests/host/synthfs_hosttest.c).
 */

#include "synthfs.h"

/* A path component as a (pointer, length) slice; the path is not modified. */
struct comp {
    const char *s;
    int len;
};

/* Policy: the components a path under /dev or /proc is split into. */
#define SYNTH_MAXC 5

/*
 * Split a slash path into up to max components, skipping empty ones (so
 * leading, trailing and doubled slashes do not produce empties). Returns the
 * count; if the path has MORE than max components the surplus is left unread
 * and the count is max (the caller treats "too many" as a miss).
 */
static int synth_split(const char *path, struct comp *c, int max)
{
    int n = 0;
    const char *p = path;

    while (*p && n < max) {
        const char *start;
        while (*p == '/') {
            p++;
        }
        if (!*p) {
            break;
        }
        start = p;
        while (*p && *p != '/') {
            p++;
        }
        c[n].s = start;
        c[n].len = (int)(p - start);
        n++;
    }
    return n;
}

/* Does component c equal the NUL-terminated literal? */
static int comp_eq(struct comp c, const char *lit)
{
    int i;
    for (i = 0; i < c.len; i++) {
        if (lit[i] == '\0' || c.s[i] != lit[i]) {
            return 0;
        }
    }
    return lit[c.len] == '\0';
}

/* Parse a non-negative decimal from a component; -1 if empty or non-digit, or
 * if it overflows a sane bound (so a giant /proc/<n> cannot wrap). */
static int comp_uint(struct comp c)
{
    int v = 0;
    int i;

    if (c.len == 0) {
        return -1;
    }
    for (i = 0; i < c.len; i++) {
        if (c.s[i] < '0' || c.s[i] > '9') {
            return -1;
        }
        if (v > 214748363) {
            return -1; /* the next *10 would overflow int; no pid is this large */
        }
        v = v * 10 + (c.s[i] - '0');
    }
    return v;
}

/* Copy a component into vp->name (NUL-terminated, bounded). */
static void synth_copy_name(struct vpath *vp, struct comp c)
{
    int i;
    int n = c.len;

    if (n > (int)sizeof(vp->name) - 1) {
        n = (int)sizeof(vp->name) - 1;
    }
    for (i = 0; i < n; i++) {
        vp->name[i] = c.s[i];
    }
    vp->name[n] = '\0';
}

/* Append a NUL-terminated string to out at *pos, bounded by cap. */
static void synth_put(char *out, int cap, int *pos, const char *s)
{
    while (*s && *pos < cap - 1) {
        out[*pos] = *s++;
        (*pos)++;
    }
}

/* Append an unsigned decimal to out at *pos, bounded by cap. */
static void synth_put_uint(char *out, int cap, int *pos, unsigned int v)
{
    char tmp[12];
    int n = 0;

    if (v == 0) {
        if (*pos < cap - 1) {
            out[(*pos)++] = '0';
        }
        return;
    }
    while (v) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n && *pos < cap - 1) {
        out[(*pos)++] = tmp[--n];
    }
}

/**
 * Render the canonical directory path for a synthetic vnode kind into `out`.
 *
 * The inverse of `synth_classify` for the directory kinds: it writes the
 * path a directory or symlink target should report (for example
 * `/proc/<pid>/fd`). `pid` is used only for the per-process kinds. When
 * `cap` is positive the result is NUL-terminated and never exceeds `cap`
 * bytes; otherwise nothing is written. An unrecognized kind yields "/".
 */
void synth_dir_path(int kind, int pid, char *out, int cap)
{
    int pos = 0;

    if (cap <= 0) {
        return;
    }
    if (kind == VNODE_DEV_DIR) {
        synth_put(out, cap, &pos, "/dev");
    } else if (kind == VNODE_PTS_DIR) {
        synth_put(out, cap, &pos, "/dev/pts");
    } else if (kind == VNODE_PROC_DIR) {
        synth_put(out, cap, &pos, "/proc");
    } else if (kind == VNODE_PROC_NET_DIR) {
        synth_put(out, cap, &pos, "/proc/net");
    } else if (kind == VNODE_PROC_PID) {
        synth_put(out, cap, &pos, "/proc/");
        synth_put_uint(out, cap, &pos, (unsigned int)pid);
    } else if (kind == VNODE_PROC_FD_DIR) {
        synth_put(out, cap, &pos, "/proc/");
        synth_put_uint(out, cap, &pos, (unsigned int)pid);
        synth_put(out, cap, &pos, "/fd");
    } else {
        synth_put(out, cap, &pos, "/");
    }
    out[pos] = '\0';
}

/** The kind of the /proc/<pid> file `name` beside stat and cmdline, or 0. */
static int synth_pid_file(struct comp name)
{
    static const struct {
        const char *name;
        int kind;
    } files[] = {{"maps", SYNTH_PROC_MAPS},   {"status", SYNTH_PROC_STATUS},
                 {"statm", SYNTH_PROC_STATM}, {"auxv", SYNTH_PROC_AUXV},
                 {"environ", SYNTH_PROC_ENVIRON}, {"comm", SYNTH_PROC_COMM},
                 {"mem", SYNTH_PROC_MEM}};
    uint32_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (comp_eq(name, files[i].name)) {
            return files[i].kind;
        }
    }
    return 0;
}

/** Classify system-wide proc files by their literal leaf names. */
static int synth_global_file(struct comp name)
{
    static const struct {
        const char *name;
        int kind;
    } files[] = {{"uptime", VNODE_PROC_UPTIME},
                 {"stat", VNODE_PROC_STAT_SYS},
                 {"meminfo", VNODE_PROC_MEMINFO},
                 {"interrupts", VNODE_PROC_INTERRUPTS},
                 {"console", VNODE_PROC_CONSOLE},
                 {"mounts", VNODE_PROC_MOUNTS},
                 {"audio", VNODE_PROC_AUDIO},
                 {"usb", VNODE_PROC_USB},
                 {"filesystems", VNODE_PROC_FILESYSTEMS},
                 {"loop", VNODE_PROC_LOOP}};
    unsigned i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (comp_eq(name, files[i].name))
            return files[i].kind;
    return VNODE_ENOENT;
}

/**
 * Classify a normalized path under `/dev` or `/proc` and fill `vp`.
 *
 * Leading and repeated slashes are accepted. The caller must resolve `.` and
 * `..` before classification.
 *
 * `VNODE_KRFFS` identifies a path outside these namespaces. A two-component
 * device path other than `/dev/pts` returns `VNODE_DEV`; devfs checks
 * device existence.
 * Process and descriptor existence are checked by the caller. `/proc/self`
 * uses `SYNTH_PID_SELF`. Return the kind stored in `vp->kind`.
 */
int synth_classify(const char *path, struct vpath *vp)
{
    struct comp c[SYNTH_MAXC];
    int n;

    vp->name[0] = '\0';
    vp->pid = 0;
    vp->procfd = -1;
    vp->procfile = 0;
    vp->pts_index = -1;

    n = synth_split(path, c, SYNTH_MAXC);

    if (n == 0) {
        vp->kind = VNODE_KRFFS;
        return vp->kind;
    }

    if (comp_eq(c[0], "dev")) {
        if (n == 1) {
            vp->kind = VNODE_DEV_DIR;
            return vp->kind;
        }
        /* /dev/pts (the pty slave directory) and /dev/pts/N (a slave). */
        if (comp_eq(c[1], "pts")) {
            if (n == 2) {
                vp->kind = VNODE_PTS_DIR;
            } else if (n == 3) {
                int idx = comp_uint(c[2]);
                if (idx < 0) {
                    vp->kind = VNODE_ENOENT; /* /dev/pts/<not-a-number> */
                } else {
                    vp->kind = VNODE_PTS;
                    vp->pts_index = idx;
                }
            } else {
                vp->kind = VNODE_ENOENT; /* /dev/pts/N/<more> */
            }
            return vp->kind;
        }
        if (n == 2) {
            vp->kind = VNODE_DEV;
            synth_copy_name(vp, c[1]);
        } else {
            vp->kind = VNODE_ENOENT; /* /dev/<name>/<more>: no such thing */
        }
        return vp->kind;
    }

    if (comp_eq(c[0], "proc")) {
        int pid;

        if (n == 1) {
            vp->kind = VNODE_PROC_DIR;
            return vp->kind;
        }
        /* /proc/cmdline: the boot command line, a global file beside the pid
           tree (checked before pid parsing, since "cmdline" is not a pid). */
        if (comp_eq(c[1], "cmdline")) {
            vp->kind = (n == 2) ? VNODE_PROC_CMDLINE : VNODE_ENOTDIR;
            return vp->kind;
        }
        /* /proc/disk: the same shape, a global file beside the pid tree. */
        if (comp_eq(c[1], "disk")) {
            vp->kind = (n == 2) ? VNODE_PROC_DISK : VNODE_ENOTDIR;
            return vp->kind;
        }
        vp->kind = synth_global_file(c[1]);
        if (vp->kind != VNODE_ENOENT) {
            if (n != 2)
                vp->kind = VNODE_ENOTDIR;
            return vp->kind;
        }
        /* /proc/net and its four files: dev, route, arp and tcp. */
        if (comp_eq(c[1], "net")) {
            if (n == 2) {
                vp->kind = VNODE_PROC_NET_DIR;
            } else if (n == 3 && comp_eq(c[2], "dev")) {
                vp->kind = VNODE_PROC_NET_DEV;
            } else if (n == 3 && comp_eq(c[2], "route")) {
                vp->kind = VNODE_PROC_NET_ROUTE;
            } else if (n == 3 && comp_eq(c[2], "arp")) {
                vp->kind = VNODE_PROC_NET_ARP;
            } else if (n == 3 && comp_eq(c[2], "tcp")) {
                vp->kind = VNODE_PROC_NET_TCP;
            } else {
                vp->kind = (n == 3) ? VNODE_ENOENT : VNODE_ENOTDIR;
            }
            return vp->kind;
        }
        if (comp_eq(c[1], "self")) {
            pid = SYNTH_PID_SELF;
        } else {
            pid = comp_uint(c[1]);
            if (pid < 0) {
                vp->kind = VNODE_ENOENT; /* /proc/<not-a-pid> */
                return vp->kind;
            }
        }
        vp->pid = pid;

        if (n == 2) {
            vp->kind = VNODE_PROC_PID;
            return vp->kind;
        }
        if (comp_eq(c[2], "stat") && n == 3) {
            vp->kind = VNODE_PROC_FILE;
            vp->procfile = SYNTH_PROC_STAT;
            return vp->kind;
        }
        if (comp_eq(c[2], "cmdline") && n == 3) {
            vp->kind = VNODE_PROC_FILE;
            vp->procfile = SYNTH_PROC_CMDLINE;
            return vp->kind;
        }
        if (n == 3) {
            int f = synth_pid_file(c[2]);

            if (f != 0) {
                vp->kind = VNODE_PROC_FILE;
                vp->procfile = f;
                return vp->kind;
            }
        }
        if (comp_eq(c[2], "fd")) {
            if (n == 3) {
                vp->kind = VNODE_PROC_FD_DIR;
                return vp->kind;
            }
            if (n == 4) {
                int fd = comp_uint(c[3]);
                if (fd < 0) {
                    vp->kind = VNODE_ENOENT;
                    return vp->kind;
                }
                vp->procfd = fd;
                vp->kind = VNODE_PROC_FD_LINK;
                return vp->kind;
            }
        }
        vp->kind = VNODE_ENOENT; /* unrecognised /proc entry */
        return vp->kind;
    }

    vp->kind = VNODE_KRFFS;
    return vp->kind;
}
