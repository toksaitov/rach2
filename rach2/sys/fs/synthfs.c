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

/* The components of the deepest path under /dev or /proc,
 * /proc/PID/task/TID/fd/N. A path is split into one more than this, so a
 * longer one is counted past every depth a rule accepts and answers ENOENT
 * or ENOTDIR, where one cut at this depth would name a shorter path. */
#define SYNTH_MAXC 6

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

/** Append /proc/<pid>, or /proc/<pid>/task/<tid> for a thread's directory,
 * to out at *pos, bounded by cap. */
static void synth_put_proc(char *out, int cap, int *pos, int pid, int tid)
{
    synth_put(out, cap, pos, "/proc/");
    synth_put_uint(out, cap, pos, (unsigned int)pid);
    if (tid != 0) {
        synth_put(out, cap, pos, "/task/");
        synth_put_uint(out, cap, pos, (unsigned int)tid);
    }
}

/* The /proc/sys table (synthfs.h): the limits of io/inotify.c, and the
 * kernel's name, its release and the bound of its process IDs. */
static const struct {
    int parent;
    const char *name;
    const char *text;
} sysents[] = {{-1, "sys", 0},
               {0, "fs", 0},
               {1, "inotify", 0},
               {2, "max_queued_events", "16384\n"},
               {2, "max_user_instances", "128\n"},
               {2, "max_user_watches", "8192\n"},
               {0, "kernel", 0},
               {6, "ostype", "Rach2\n"},
               {6, "osrelease", "1.0.0\n"},
               {6, "pid_max", "2000000000\n"}};

int synth_sys_count(void)
{
    return (int)(sizeof(sysents) / sizeof(sysents[0]));
}

const char *synth_sys_name(int entry)
{
    return entry >= 0 && entry < synth_sys_count() ? sysents[entry].name : 0;
}

int synth_sys_parent(int entry)
{
    return entry >= 0 && entry < synth_sys_count() ? sysents[entry].parent : -1;
}

const char *synth_sys_text(int entry)
{
    return entry >= 0 && entry < synth_sys_count() ? sysents[entry].text : 0;
}

/** Append /proc and the path of the /proc/sys entry `entry`. */
static void synth_put_sys(char *out, int cap, int *pos, int entry)
{
    if (entry < 0 || entry >= synth_sys_count()) {
        synth_put(out, cap, pos, "/proc/sys");
        return;
    }
    if (sysents[entry].parent < 0) {
        synth_put(out, cap, pos, "/proc");
    } else {
        synth_put_sys(out, cap, pos, sysents[entry].parent);
    }
    synth_put(out, cap, pos, "/");
    synth_put(out, cap, pos, sysents[entry].name);
}

/**
 * Render the canonical directory path for a synthetic vnode kind into `out`.
 *
 * The inverse of `synth_classify` for the directory kinds: it writes the
 * path a directory or symlink target should report (for example
 * `/proc/<pid>/fd`). `pid` is used only for the per-process kinds, and
 * `tid`, when it is not 0, makes one a thread's, `/proc/<pid>/task/<tid>`
 * and its `fd`. When `cap` is positive the result is NUL-terminated and
 * never exceeds `cap` bytes; otherwise nothing is written. An unrecognized
 * kind yields "/".
 */
void synth_dir_path(int kind, int pid, int tid, char *out, int cap)
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
    } else if (kind == VNODE_PROC_SYS_DIR) {
        synth_put_sys(out, cap, &pos, pid);
    } else if (kind == VNODE_PROC_PID) {
        synth_put_proc(out, cap, &pos, pid, tid);
    } else if (kind == VNODE_PROC_FD_DIR) {
        synth_put_proc(out, cap, &pos, pid, tid);
        synth_put(out, cap, &pos, "/fd");
    } else if (kind == VNODE_PROC_TASK_DIR) {
        synth_put(out, cap, &pos, "/proc/");
        synth_put_uint(out, cap, &pos, (unsigned int)pid);
        synth_put(out, cap, &pos, "/task");
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
    } files[] = {{"maps", SYNTH_PROC_MAPS},
                 {"status", SYNTH_PROC_STATUS},
                 {"statm", SYNTH_PROC_STATM},
                 {"auxv", SYNTH_PROC_AUXV},
                 {"environ", SYNTH_PROC_ENVIRON},
                 {"comm", SYNTH_PROC_COMM},
                 {"mounts", SYNTH_PROC_PIDMOUNTS},
                 {"mem", SYNTH_PROC_MEM},
                 {"oom_score_adj", SYNTH_PROC_OOM_ADJ}};
    uint32_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (comp_eq(name, files[i].name)) {
            return files[i].kind;
        }
    }
    return 0;
}

/**
 * Classify the part of a /proc path from its pid component on: `c[0]` is
 * the pid, already in `vp->pid`, and `n` counts the components from it. A
 * thread's directory under `task` is classified as the process's directory
 * with the thread's id in `vp->tid`: it has the files of /proc/<tid>, the
 * thread's own where a thread has any, and no `task` of its own (`task` is
 * 0 then). No thread has id 0, which is the kernel's idle context.
 */
static int synth_classify_pid(const struct comp *c, int n, struct vpath *vp, int task)
{
    int f = n == 2 ? synth_pid_file(c[1]) : 0;

    if (n == 1) {
        vp->kind = VNODE_PROC_PID;
        return vp->kind;
    }
    if (n == 2 && comp_eq(c[1], "stat")) {
        vp->kind = VNODE_PROC_FILE;
        vp->procfile = SYNTH_PROC_STAT;
        return vp->kind;
    }
    if (n == 2 && comp_eq(c[1], "cmdline")) {
        vp->kind = VNODE_PROC_FILE;
        vp->procfile = SYNTH_PROC_CMDLINE;
        return vp->kind;
    }
    if (n == 2 && comp_eq(c[1], "exe")) {
        vp->kind = VNODE_PROC_EXE;
        return vp->kind;
    }
    if (f != 0) {
        vp->kind = VNODE_PROC_FILE;
        vp->procfile = f;
        return vp->kind;
    }
    if (comp_eq(c[1], "fd")) {
        if (n == 2) {
            vp->kind = VNODE_PROC_FD_DIR;
            return vp->kind;
        }
        if (n == 3) {
            int fd = comp_uint(c[2]);
            if (fd < 0) {
                vp->kind = VNODE_ENOENT;
                return vp->kind;
            }
            vp->procfd = fd;
            vp->kind = VNODE_PROC_FD_LINK;
            return vp->kind;
        }
    }
    if (task && comp_eq(c[1], "task")) {
        int tid;

        if (n == 2) {
            vp->kind = VNODE_PROC_TASK_DIR;
            return vp->kind;
        }
        tid = comp_uint(c[2]);
        if (tid <= 0) {
            vp->kind = VNODE_ENOENT;
            return vp->kind;
        }
        vp->tid = tid;
        return synth_classify_pid(c + 2, n - 2, vp, 0);
    }
    vp->kind = VNODE_ENOENT; /* unrecognised /proc entry */
    return vp->kind;
}

/** Classify the components `c` of `n` below /proc/sys by the table. */
static int synth_classify_sys(const struct comp *c, int n, struct vpath *vp)
{
    int at = 0, i, k;

    for (k = 0; k < n; k++) {
        if (sysents[at].text) {
            vp->kind = VNODE_ENOTDIR;
            return vp->kind;
        }
        for (i = 1;
             i < synth_sys_count() && !(sysents[i].parent == at && comp_eq(c[k], sysents[i].name));
             i++) {
        }
        if (i == synth_sys_count()) {
            vp->kind = VNODE_ENOENT;
            return vp->kind;
        }
        at = i;
    }
    vp->sysent = at;
    vp->kind = sysents[at].text ? VNODE_PROC_SYS_FILE : VNODE_PROC_SYS_DIR;
    return vp->kind;
}

/**
 * /dev/fd, /dev/fd/N, /dev/stdin, /dev/stdout and /dev/stderr, which Linux
 * makes links to /proc/self/fd and its first three entries: here they are
 * those nodes of the caller, classified as /proc/self/fd and its links are.
 * Returns 1 with `vp` filled for one of them, 0 for any other /dev path.
 */
static int synth_classify_dev_fd(const struct comp *c, int n, struct vpath *vp)
{
    static const char *const std[] = {"stdin", "stdout", "stderr"};
    int i;

    vp->pid = SYNTH_PID_SELF;
    if (comp_eq(c[1], "fd")) {
        if (n == 2) {
            vp->kind = VNODE_PROC_FD_DIR;
        } else if (n == 3 && comp_uint(c[2]) >= 0) {
            vp->kind = VNODE_PROC_FD_LINK;
            vp->procfd = comp_uint(c[2]);
        } else {
            vp->kind = n == 3 ? VNODE_ENOENT : VNODE_ENOTDIR;
        }
        return 1;
    }
    for (i = 0; i < 3; i++) {
        if (comp_eq(c[1], std[i])) {
            vp->kind = n == 2 ? VNODE_PROC_FD_LINK : VNODE_ENOTDIR;
            vp->procfd = i;
            return 1;
        }
    }
    vp->pid = 0;
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
                 {"cpuinfo", VNODE_PROC_CPUINFO},
                 {"loadavg", VNODE_PROC_LOADAVG},
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
    struct comp c[SYNTH_MAXC + 1];
    int n;

    vp->name[0] = '\0';
    vp->pid = 0;
    vp->tid = 0;
    vp->procfd = -1;
    vp->procfile = 0;
    vp->pts_index = -1;
    vp->sysent = 0;

    n = synth_split(path, c, SYNTH_MAXC + 1);

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
        if (synth_classify_dev_fd(c, n, vp)) {
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
        if (comp_eq(c[1], "sys")) {
            return synth_classify_sys(c + 2, n - 2, vp);
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
            if (pid <= 0) {
                vp->kind = VNODE_ENOENT; /* /proc/<not-a-pid>, and the idle process's 0 */
                return vp->kind;
            }
        }
        vp->pid = pid;
        return synth_classify_pid(c + 1, n - 1, vp, 1);
    }

    vp->kind = VNODE_KRFFS;
    return vp->kind;
}
