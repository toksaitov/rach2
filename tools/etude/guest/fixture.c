/*
 * fixture.c: the fixtures of a native case beyond its workspace: mapped ids
 * and ports, volumes on loop devices, a tmpfs, namespaces, a private root,
 * a pseudo-terminal and the kernel's records. etude-guest parses them with
 * the invocation, makes them before the child runs and removes them after
 * it ends; tools/etude/Readme.md, "Native runs", gives the contract.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "fixture.h"
#include <etude/frame.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * The fixtures volume, root, tmpfs, mountns, ipcns, utsns, log and private
 * need the kernel's loop devices, mounts, namespaces or log, or its table of
 * mounts, which keeps the removal of `private` out of a mount left in it.
 * The code that makes them
 * compiles where FIX_KERNEL is 1: on Linux, and on Rach2, whose cross
 * compiler defines no operating system and whose build of etude-guest
 * defines __rach2__. macOS has none of them, and the host tests compile this
 * file there too: fixture_parse refuses those fixtures. Any other system does
 * not compile this file.
 */
#if defined(__linux__) || defined(__rach2__)
#define FIX_KERNEL 1
#include <sys/klog.h>
#include <sys/mount.h>

/* unshare(2) and the flags of the Linux ABI, declared here because a host
 * test's include path finds the kernel's own sched.h first. */
int unshare(int flags);
#ifndef CLONE_NEWNS
#define CLONE_NEWNS  0x00020000
#define CLONE_NEWUTS 0x04000000
#define CLONE_NEWIPC 0x08000000
#endif
#elif defined(__APPLE__)
#define FIX_KERNEL 0
#include <grp.h>
#include <pwd.h>
#include <sys/sysctl.h>
#else
#error "fixture.c: no fixtures for this system; a build for Rach2 defines __rach2__"
#endif

/* The kernel's log, read whole: SYSLOG_ACTION_READ_ALL, which leaves the
 * place the system log daemon reads from where it is. */
#define LOG_READ_ALL 3
#define LOG_MAX      (1 << 20)

/* The bytes at the end of the first reading of the log by which the second
 * finds where the records written meanwhile begin. */
#define LOG_ANCHOR 256

#if !defined(__APPLE__)
/** Format a path into `out` of FIX_PATH bytes; 0, or -1 when it does not
 * fit. Its callers read /proc or make the fixtures that need the kernel, and
 * macOS compiles none of them. */
static int fpath(char *out, const char *fmt, const char *a, const char *b)
{
    int n = snprintf(out, FIX_PATH, fmt, a, b);

    return n < 0 || n >= FIX_PATH ? -1 : 0;
}
#endif

/** Whether `kind` is one of the types a volume may have. */
static int volume_type(const char *t)
{
    return strcmp(t, "krffs") == 0 || strcmp(t, "ext2") == 0 || strcmp(t, "vfat") == 0 ||
           strcmp(t, "exfat") == 0;
}

/** A decimal number from `s` to its end or to `stop`, or -1. */
static long number(const char *s, char stop, const char **end)
{
    long v = 0;

    if (*s < '0' || *s > '9')
        return -1;
    while (*s >= '0' && *s <= '9') {
        if (v > 214748363L)
            return -1;
        v = v * 10 + (*s++ - '0');
    }
    if (*s != stop)
        return -1;
    if (end)
        *end = s;
    return v;
}

/** Parse the pairs OLD:NEW of `given` into `from` and `to`; their count, or
 * -1 for a pair that does not parse, repeats an OLD or a NEW, or is over
 * `max`. */
static int pairs(const struct eh_json *given, long *from, long *to, long max)
{
    const struct eh_json *g;
    const char *p;
    int n = 0, i;

    for (g = given->child; g; g = g->next) {
        if (g->type != 's' || n == FIX_PAIRS)
            return -1;
        from[n] = number(g->string, ':', &p);
        to[n] = from[n] < 0 ? -1 : number(p + 1, '\0', NULL);
        if (to[n] <= 0 || to[n] > max || from[n] > max)
            return -1;
        for (i = 0; i < n; i++)
            if (from[i] == from[n] || to[i] == to[n])
                return -1;
        n++;
    }
    return n;
}

/** The number of strings of `given`, or -1 when one is not a string. */
static int strings(const struct eh_json *given)
{
    const struct eh_json *g;
    int n = 0;

    for (g = given->child; g; g = g->next, n++)
        if (g->type != 's')
            return -1;
    return n;
}

/** The `i`th string of `given`. */
static const char *nth(const struct eh_json *given, int i)
{
    const struct eh_json *g = given->child;

    while (i-- > 0)
        g = g->next;
    return g->string;
}

/* A fixture being read: its values, their count, and the invocation's
 * files. */
struct reading {
    struct fixtures *fx;
    const struct eh_json *given;
    int n;
    fix_has_file has;
    const void *ctx;
};

/** workspace, given nothing. */
static int read_workspace(struct reading *r)
{
    return r->n == 0 ? 0 : -1;
}

/** stage, given files of the invocation. */
static int read_stage(struct reading *r)
{
    int i;

    for (i = 0; i < r->n; i++)
        if (!r->has(r->ctx, nth(r->given, i)))
            return -1;
    return 0;
}

/** ids, given OLD:NEW pairs, once. */
static int read_ids(struct reading *r)
{
    struct fixtures *fx = r->fx;

    if (fx->nids)
        return -1;
    fx->nids = pairs(r->given, fx->id_old, fx->id_new, 2147483646L);
    return fx->nids < 1 ? -1 : 0;
}

/** ports, given OLD:NEW pairs, once. */
static int read_ports(struct reading *r)
{
    struct fixtures *fx = r->fx;

    if (fx->nports)
        return -1;
    fx->nports = pairs(r->given, fx->port_old, fx->port_new, 65535);
    return fx->nports < 1 ? -1 : 0;
}

/** Fill `v` from TYPE and IMAGE, the first two values; 0, or -1 when the
 * image is not a file of the invocation. */
static int read_image(struct reading *r, struct fix_volume *v)
{
    if (!r->has(r->ctx, nth(r->given, 1)))
        return -1;
    snprintf(v->type, sizeof(v->type), "%s", nth(r->given, 0));
    snprintf(v->image, sizeof(v->image), "%s", nth(r->given, 1));
    v->loop = -1;
    return 0;
}

/** volume, given TYPE, IMAGE and `mounted` or `attached`. */
static int read_volume(struct reading *r)
{
    struct fixtures *fx = r->fx;
    struct fix_volume *v = &fx->vol[fx->nvol];

    if (r->n != 3 || fx->nvol + fx->has_root == FIX_VOLUMES || !volume_type(nth(r->given, 0)) ||
        read_image(r, v) < 0)
        return -1;
    v->mounted = strcmp(nth(r->given, 2), "mounted") == 0;
    if (!v->mounted && strcmp(nth(r->given, 2), "attached") != 0)
        return -1;
    fx->nvol++;
    return 0;
}

/** root, given TYPE, IMAGE and the program's absolute path in it, once. */
static int read_root(struct reading *r)
{
    struct fixtures *fx = r->fx;
    const char *path = r->n == 3 ? nth(r->given, 2) : "";

    if (r->n != 3 || fx->has_root || fx->nvol == FIX_VOLUMES ||
        (!volume_type(nth(r->given, 0)) && strcmp(nth(r->given, 0), "ext2-dir") != 0) ||
        path[0] != '/' || strstr(path, "/..") || strlen(path) >= sizeof(fx->root_path) ||
        read_image(r, &fx->root) < 0)
        return -1;
    snprintf(fx->root_path, sizeof(fx->root_path), "%s", path);
    fx->has_root = 1;
    fx->mountns = 1;
    return 0;
}

/** tmpfs, given its size in bytes, once. */
static int read_tmpfs(struct reading *r)
{
    if (r->n != 1 || r->fx->tmpfs_bytes)
        return -1;
    r->fx->tmpfs_bytes = number(nth(r->given, 0), '\0', NULL);
    return r->fx->tmpfs_bytes < 1 ? -1 : 0;
}

/** peer, given an address, a port and at most FIX_PEER_VALUES NAME=VALUE
 * arguments, once. */
static int read_peer(struct reading *r)
{
    struct fixtures *fx = r->fx;
    int i;

    if (r->n < 2 || r->n > 2 + FIX_PEER_VALUES || fx->peer[0] ||
        snprintf(fx->peer, sizeof(fx->peer), "%s:%s", nth(r->given, 0), nth(r->given, 1)) >=
            (int)sizeof(fx->peer))
        return -1;
    for (i = 2; i < r->n; i++) {
        const char *v = nth(r->given, i);
        size_t name = strspn(v, "abcdefghijklmnopqrstuvwxyz");
        if (name == 0 || v[name] != '=' || strlen(v) >= sizeof(fx->peer_value[0]))
            return -1;
        snprintf(fx->peer_value[fx->npeer_values++], sizeof(fx->peer_value[0]), "%s", v);
    }
    return 0;
}

/** A fixture given nothing that sets the flag of its name. */
static int read_flag(struct reading *r, const char *kind)
{
    struct fixtures *fx = r->fx;
    int *flag = strcmp(kind, "mountns") == 0   ? &fx->mountns
                : strcmp(kind, "ipcns") == 0   ? &fx->ipcns
                : strcmp(kind, "utsns") == 0   ? &fx->utsns
                : strcmp(kind, "pty") == 0     ? &fx->pty
                : strcmp(kind, "log") == 0     ? &fx->log
                : strcmp(kind, "alone") == 0   ? &fx->alone
                : strcmp(kind, "console") == 0 ? &fx->console
                : strcmp(kind, "private") == 0 ? &fx->private_dir
                                               : NULL;

    if (!flag || r->n != 0)
        return -1;
    *flag = 1;
    return 0;
}

/* The fixtures that take values, by their names. */
static const struct {
    const char *kind;
    int (*read)(struct reading *);
} readers[] = {{"workspace", read_workspace}, {"stage", read_stage},   {"ids", read_ids},
               {"ports", read_ports},         {"volume", read_volume}, {"root", read_root},
               {"tmpfs", read_tmpfs},         {"peer", read_peer}};

/** Whether the fixture `kind` needs the kernel's loop devices, mounts,
 * namespaces or log. */
static int kernel_kind(const char *kind)
{
    static const char *const kinds[] = {"volume", "root",  "tmpfs", "mountns",
                                        "ipcns",  "utsns", "log",   "private"};
    size_t i;

    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
        if (strcmp(kind, kinds[i]) == 0)
            return 1;
    return 0;
}

/**
 * Read the fixtures of an invocation, the array `list`, into `fx`. `has`
 * answers whether the invocation, `ctx`, has a file of a name.
 *
 * @return NULL, or `refused` with the fixture's name in `*why` when a
 * fixture is unknown, needs what the kernel of this system lacks, is given
 * twice where it may be given once, or is given values that do not fit its
 * form.
 */
const char *fixture_parse(struct fixtures *fx, const struct eh_json *list, fix_has_file has,
                          const void *ctx, const char **why)
{
    struct reading r;
    const struct eh_json *f;

    memset(fx, 0, sizeof(*fx));
    fx->pty_master = -1;
    fx->root.loop = -1;
    r.fx = fx;
    r.has = has;
    r.ctx = ctx;
    *why = "a fixture";
    for (f = list->child; f; f = f->next) {
        const struct eh_json *k = f->type == 'o' ? eh_json_get(f, "fixture") : NULL;
        size_t i;

        r.given = f->type == 'o' ? eh_json_get(f, "given") : NULL;
        r.n = r.given && r.given->type == 'a' ? strings(r.given) : -1;
        if (!k || k->type != 's' || r.n < 0)
            return ETUDE_ERR_REFUSED;
        *why = k->string;
        if (!FIX_KERNEL && kernel_kind(k->string))
            return ETUDE_ERR_REFUSED;
        for (i = 0; i < sizeof(readers) / sizeof(readers[0]); i++)
            if (strcmp(k->string, readers[i].kind) == 0)
                break;
        if (i < sizeof(readers) / sizeof(readers[0]) ? readers[i].read(&r) < 0
                                                     : read_flag(&r, k->string) < 0)
            return ETUDE_ERR_REFUSED;
    }
    return NULL;
}

/** Whether the fixtures are the workspace and staged files alone, all the
 * designated init's exec can have. */
int fixture_simple(const struct fixtures *fx)
{
    return !fx->nids && !fx->nports && !fx->nvol && !fx->has_root && !fx->tmpfs_bytes &&
           !fx->mountns && !fx->ipcns && !fx->utsns && !fx->pty && !fx->log && !fx->peer[0];
}

/** Whether `id` is field 3 of a line of the colon-separated file `path`. */
static int in_file(const char *path, long id)
{
    char line[1024];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f)
        return 0;
    while (!found && fgets(line, sizeof(line), f)) {
        char *p = line;
        int field;
        for (field = 0; field < 2 && p; field++) {
            p = strchr(p, ':');
            if (p)
                p++;
        }
        if (p && strtol(p, NULL, 10) == id && *p >= '0' && *p <= '9')
            found = 1;
    }
    fclose(f);
    return found;
}

/** Whether `id` is an account's or a group's: a line of /etc/passwd or
 * /etc/group has it, or, on macOS, which keeps most accounts in Directory
 * Services and not in those files, getpwuid or getgrgid finds it there. On
 * Linux and Rach2 the files alone are read. */
static int listed(long id)
{
#if defined(__APPLE__)
    if (getpwuid((uid_t)id) || getgrgid((gid_t)id))
        return 1;
#endif
    return in_file("/etc/passwd", id) || in_file("/etc/group", id);
}

#if defined(__APPLE__)
/** Whether a process of the machine has `id` among its real, effective or
 * saved user or group ids, by the sysctl KERN_PROC_ALL, which lists every
 * process. */
static int in_use(long id)
{
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    struct kinfo_proc *kp = NULL;
    size_t len = 0;
    size_t i;
    int found = 0;
    int tries;

    for (tries = 0; tries < 3 && !kp; tries++) {
        if (sysctl(mib, 3, NULL, &len, NULL, 0) < 0)
            return 0;
        len += len / 4;
        kp = malloc(len);
        if (kp && sysctl(mib, 3, kp, &len, NULL, 0) < 0) {
            free(kp);
            kp = NULL;
            if (errno != ENOMEM)
                return 0;
        }
    }
    for (i = 0; kp && !found && i < len / sizeof(*kp); i++) {
        const struct eproc *e = &kp[i].kp_eproc;
        found = e->e_pcred.p_ruid == id || e->e_ucred.cr_uid == id || e->e_pcred.p_svuid == id ||
                e->e_pcred.p_rgid == id || e->e_ucred.cr_groups[0] == id ||
                e->e_pcred.p_svgid == id;
    }
    free(kp);
    return found;
}
#else
/** Whether a process of the machine has `id` among its user or group ids,
 * by the Uid: and Gid: lines of each /proc/PID/status. */
static int in_use(long id)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int found = 0;

    if (!d)
        return 0;
    while (!found && (e = readdir(d)) != NULL) {
        char path[FIX_PATH];
        char line[256];
        FILE *f;
        if (e->d_name[0] < '1' || e->d_name[0] > '9' ||
            fpath(path, "/proc/%s/status%s", e->d_name, "") < 0 || !(f = fopen(path, "r")))
            continue;
        while (!found && fgets(line, sizeof(line), f)) {
            char *p = line + 4;
            int i;
            if (strncmp(line, "Uid:", 4) != 0 && strncmp(line, "Gid:", 4) != 0)
                continue;
            for (i = 0; i < 4 && !found; i++) {
                char *end;
                long v = strtol(p, &end, 10);
                if (end == p)
                    break;
                found = v == id;
                p = end;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}
#endif

/** Whether the TCP and the UDP port `port` of 127.0.0.1 are free now. */
static int port_free(long port)
{
    static const int kinds[2] = {SOCK_STREAM, SOCK_DGRAM};
    struct sockaddr_in a;
    int i, s, ok = 1;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(0x7F000001u);
    for (i = 0; i < 2 && ok; i++) {
        s = socket(AF_INET, kinds[i], 0);
        if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
            ok = 0;
        if (s >= 0)
            close(s);
    }
    return ok;
}

/** The reason an id or a port of `fx` cannot be given now, or NULL when
 * each id is no account's and no process's and each port is free. */
static const char *not_free(const struct fixtures *fx)
{
    int i;

    for (i = 0; i < fx->nids; i++)
        if (listed(fx->id_new[i]) || in_use(fx->id_new[i]))
            return "an id is an account's or a process's";
    for (i = 0; i < fx->nports; i++)
        if (!port_free(fx->port_new[i]))
            return "a port is in use";
    return NULL;
}

/** Open the master side of the case's pseudo-terminal and name its slave;
 * 0 or -1. */
static int open_terminal(struct fixtures *fx)
{
    const char *name;

    fx->pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fx->pty_master < 0 || grantpt(fx->pty_master) < 0 || unlockpt(fx->pty_master) < 0 ||
        !(name = ptsname(fx->pty_master)))
        return -1;
    return snprintf(fx->pty_slave, sizeof(fx->pty_slave), "%s", name) >= (int)sizeof(fx->pty_slave)
               ? -1
               : 0;
}

/** Make the slave side of the case's pseudo-terminal its descriptors 0 to
 * 2 and its controlling terminal; 0 or -1. */
static int enter_terminal(const struct fixtures *fx)
{
    int s = open(fx->pty_slave, O_RDWR);

    if (s < 0 || ioctl(s, TIOCSCTTY, 0) < 0 || dup2(s, 0) < 0 || dup2(s, 1) < 0 || dup2(s, 2) < 0)
        return -1;
    if (s > 2)
        close(s);
    return 0;
}

/** Append to `out`, of FIX_ARG bytes, NAME=OLD:NEW,... of `n` pairs. */
static void pair_arg(char *out, const char *name, const long *from, const long *to, int n)
{
    size_t len = (size_t)snprintf(out, FIX_ARG, "%s=", name);
    int i;

    for (i = 0; i < n && len < FIX_ARG; i++)
        len +=
            (size_t)snprintf(out + len, FIX_ARG - len, "%s%ld:%ld", i ? "," : "", from[i], to[i]);
}

/* The fixtures that need the kernel, where FIX_KERNEL is 1. */
#if FIX_KERNEL

/** Append `line` and a newline to the attempt's record of what its
 * fixtures made, `DIR/fixtures`, which `fixture_settle` reads; 0, or -1
 * when the line is not in the record whole. */
static int remember(const char *dir, const char *line)
{
    char path[FIX_PATH];
    size_t n = strlen(line);
    int fd, rc = 0;

    if (fpath(path, "%s/fixtures%s", dir, "") < 0)
        return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (write(fd, line, n) != (ssize_t)n || write(fd, "\n", 1) != 1 || fsync(fd) < 0)
        rc = -1;
    close(fd);
    return rc;
}

/** The first loop device that /proc/loop does not list, or -1. */
static int free_loop(void)
{
    char text[4096];
    int used[FIX_VOLUMES] = {0, 0, 0, 0};
    int fd = open("/proc/loop", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, text, sizeof(text) - 1);
    char *p;
    int i;

    if (fd >= 0)
        close(fd);
    if (n < 0)
        return -1;
    text[n] = '\0';
    for (p = text; (p = strstr(p, "loop")) != NULL; p += 4)
        if ((p == text || p[-1] == '\n') && p[4] >= '0' && p[4] < '0' + FIX_VOLUMES)
            used[p[4] - '0'] = 1;
    for (i = 0; i < FIX_VOLUMES; i++)
        if (!used[i])
            return i;
    return -1;
}

/** Write `line` to /dev/loop-control; 0, or -1 with errno. */
static int loop_command(const char *line)
{
    int fd = open("/dev/loop-control", O_WRONLY | O_CLOEXEC);
    ssize_t n;
    int e;

    if (fd < 0)
        return -1;
    n = write(fd, line, strlen(line));
    e = errno;
    close(fd);
    errno = e;
    return n < 0 ? -1 : 0;
}

/** Attach the workspace's copy of `v`'s image to a free loop device. */
static int attach(struct fix_volume *v, const char *dir)
{
    char path[FIX_PATH];
    char full[PATH_MAX];
    char line[FIX_PATH + 32];
    int n = free_loop();

    if (n < 0) {
        errno = EBUSY;
        return -1;
    }
    if (fpath(path, "%s/work/%s", dir, v->image) < 0 || !realpath(path, full) ||
        snprintf(line, sizeof(line), "attach %d %s", n, full) >= (int)sizeof(line))
        return -1;
    if (loop_command(line) < 0)
        return -1;
    v->loop = n;
    return 0;
}

/** Mount volume `v` on `at`, in the caller's mount namespace. */
static int mount_volume(const struct fix_volume *v, const char *at, unsigned long flags)
{
    char dev[32];

    snprintf(dev, sizeof(dev), "/dev/loop%d", v->loop);
    return mount(dev, at, v->type, flags, NULL);
}

/** Mount a tmpfs of `bytes` on `at` with the mount `flags`. */
static int mount_tmpfs(long bytes, const char *at, unsigned long flags)
{
    char data[32];

    snprintf(data, sizeof(data), "size=%ld", bytes);
    return mount("tmpfs", at, "tmpfs", flags, data);
}

/** The kernel's log as one reading, allocated, with its length in `*len`,
 * or NULL. */
static char *read_log(long *len)
{
    char *buf = malloc(LOG_MAX);
    int n = buf ? klogctl(LOG_READ_ALL, buf, LOG_MAX) : -1;

    if (n < 0) {
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

/** Make the directory `name` of the workspace in `dir` and, for a case
 * without a mount namespace, mount volume `v`, or the tmpfs when `v` is
 * NULL, on it; 0 or -1. The mount is recorded before it is made, so that a
 * later etude-guest finds every mount of an attempt whose etude-guest
 * ended, and is not made when the record cannot be written. */
static int mount_point(struct fixtures *fx, const char *dir, const char *name, struct fix_volume *v)
{
    char path[FIX_PATH];
    char line[FIX_PATH + 16];

    if (fpath(path, "%s/work/%s", dir, name) < 0 || mkdir(path, 0755) < 0)
        return -1;
    if (fx->mountns)
        return 0;
    snprintf(line, sizeof(line), "mount %s", path);
    if (remember(dir, line) < 0)
        return -1;
    if (v ? mount_volume(v, path, 0) < 0 : mount_tmpfs(fx->tmpfs_bytes, path, 0) < 0)
        return -1;
    if (v)
        v->global_mount = 1;
    else
        fx->tmpfs_global = 1;
    return 0;
}

/** Make the directory `path` unless it is there; 0 or -1. */
static int make_dir(const char *path)
{
    return mkdir(path, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/**
 * Set `fx->root_at` for the attempt in `dir`, `STATE/a/TOKEN`; 0 or -1.
 * An image is mounted on `STATE/r`, made here, in its case's own mount
 * namespace, so one place serves every case, and its path is short: the
 * kernel refuses a mount whose path, as the machine names it, is longer than
 * 63 bytes, and a program in the root mounts below it. An `ext2-dir` tree
 * is the root where it is copied, `STATE/a/TOKEN/r`, short for the same
 * reason, since a mount below a bind of it would be measured by the bind's
 * place and the tree's own path together.
 */
static int root_place(struct fixtures *fx, const char *dir)
{
    char full[PATH_MAX];
    const char *token = realpath(dir, full) ? strrchr(full, '/') : NULL;
    const char *a = token;

    if (token && strcmp(fx->root.type, "ext2-dir") == 0)
        return fpath(fx->root_at, "%s/r%s", full, "");
    while (a && a > full && *--a != '/')
        ;
    if (!token || *a != '/' || a + 2 != token || a[1] != 'a' ||
        snprintf(fx->root_at, sizeof(fx->root_at), "%.*s/r", (int)(a - full), full) >=
            (int)sizeof(fx->root_at))
        return -1;
    return make_dir(fx->root_at);
}

/** Make the directory `private` of the workspace of the attempt in `dir`,
 * recorded first, so that fixture_settle removes it should etude-guest end
 * before the case; 0 or -1. */
static int make_private(const char *dir)
{
    char path[FIX_PATH];

    if (fpath(path, "%s/work/private%s", dir, "") < 0 || remember(dir, "private") < 0)
        return -1;
    return mkdir(path, 0755);
}

/**
 * Whether the machine's table of mounts has a mount at `path`, an absolute
 * path with no symbolic link and no trailing slash, or below it.
 *
 * @return 1 or 0, or -1 when /proc/mounts cannot be read.
 */
static int mount_below(const char *path)
{
    size_t len = strlen(path);
    char line[2 * FIX_PATH];
    char at[FIX_PATH];
    FILE *f = fopen("/proc/mounts", "re");
    int found = 0;

    if (!f)
        return -1;
    while (!found && fgets(line, sizeof(line), f)) {
        const char *p = strchr(line, ' ');
        size_t n = 0;

        /* The table writes a space, a tab, a newline or a backslash of a
         * name as a backslash and three octal digits. */
        for (p = p ? p + 1 : ""; *p && *p != ' ' && *p != '\n' && n + 1 < sizeof(at); n++) {
            if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' &&
                p[3] >= '0' && p[3] <= '7') {
                at[n] = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
                p += 4;
            } else
                at[n] = *p++;
        }
        at[n] = '\0';
        found = strncmp(at, path, len) == 0 && (at[len] == '\0' || at[len] == '/');
    }
    fclose(f);
    return found;
}

/** Remove the tree at `path`, without following a symbolic link or entering
 * a directory of a file system other than `dev`'s; 0, or -1 when anything is
 * left. */
static int remove_below(const char *path, dev_t dev)
{
    char sub[FIX_PATH];
    struct stat st;
    struct dirent *e;
    DIR *d;
    int rc = 0;

    if (lstat(path, &st) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    if (st.st_dev != dev || !(d = opendir(path)))
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fpath(sub, "%s/%s", path, e->d_name) < 0 || remove_below(sub, dev) < 0)
            rc = -1;
    }
    closedir(d);
    return rmdir(path) < 0 ? -1 : rc;
}

/**
 * Remove the directory `private` of the workspace of the attempt in `dir`
 * and all it holds, once the case's processes have ended. A mount at or
 * below it in the machine's namespace leaves it whole, since the removal
 * would reach through a bind into the tree bound; a mount of the case's own
 * namespace has gone with the namespace.
 *
 * @return 0, or -1 when it is left in whole or in part.
 */
static int remove_private(const char *dir)
{
    char path[FIX_PATH];
    char full[PATH_MAX];
    struct stat st;

    if (fpath(path, "%s/work/private%s", dir, "") < 0)
        return -1;
    if (lstat(path, &st) < 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    if (!realpath(path, full) || mount_below(full) != 0)
        return -1;
    return remove_below(path, st.st_dev);
}

/** Make the fixtures of `fx` that need the kernel, as fixture_prepare
 * says; 0 or -1. */
static int kernel_prepare(struct fixtures *fx, const char *dir)
{
    int i;

    if (fx->log && !(fx->log_before = read_log(&fx->log_before_len)))
        return -1;
    if (fx->private_dir && make_private(dir) < 0)
        return -1;
    for (i = 0; i < fx->nvol; i++) {
        char name[16];
        snprintf(name, sizeof(name), "vol%d", i);
        if (attach(&fx->vol[i], dir) < 0 ||
            (fx->vol[i].mounted && mount_point(fx, dir, name, &fx->vol[i]) < 0))
            return -1;
    }
    if ((fx->has_root && (root_place(fx, dir) < 0 || attach(&fx->root, dir) < 0)) ||
        (fx->tmpfs_bytes && mount_point(fx, dir, "tmpfs", NULL) < 0))
        return -1;
    return 0;
}

/** Copy the file `from` to `to` with its mode and owner; 0 or -1. */
static int copy_file(const char *from, const char *to, const struct stat *st)
{
    char buf[65536];
    int in = open(from, O_RDONLY | O_CLOEXEC);
    int out;
    ssize_t r;

    if (in < 0)
        return -1;
    unlink(to);
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((r = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)r) != r) {
            r = -1;
            break;
        }
    close(in);
    if (r < 0 || fchown(out, st->st_uid, st->st_gid) < 0 || fchmod(out, st->st_mode & 07777) < 0) {
        close(out);
        return -1;
    }
    return close(out);
}

/** Copy the tree `from` into the new directory `to`, with each node's
 * mode, owner and set-id bits, and each symbolic link as a link; 0 or -1. */
static int copy_tree(const char *from, const char *to)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char a[FIX_PATH];
    char b[FIX_PATH];
    int rc = 0;

    if (lstat(from, &st) < 0 || mkdir(to, 0700) < 0 || !(d = opendir(from)))
        return -1;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        struct stat s;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0 ||
            strcmp(e->d_name, "lost+found") == 0)
            continue;
        if (fpath(a, "%s/%s", from, e->d_name) < 0 || fpath(b, "%s/%s", to, e->d_name) < 0 ||
            lstat(a, &s) < 0) {
            rc = -1;
        } else if (S_ISDIR(s.st_mode)) {
            rc = copy_tree(a, b);
        } else if (S_ISLNK(s.st_mode)) {
            char target[FIX_PATH];
            ssize_t n = readlink(a, target, sizeof(target) - 1);
            if (n < 0)
                rc = -1;
            else {
                target[n] = '\0';
                rc = symlink(target, b) < 0 || lchown(b, s.st_uid, s.st_gid) < 0 ? -1 : 0;
            }
        } else if (S_ISREG(s.st_mode)) {
            rc = copy_file(a, b, &s);
        }
    }
    closedir(d);
    if (rc == 0 && (chown(to, st.st_uid, st.st_gid) < 0 || chmod(to, st.st_mode & 07777) < 0))
        rc = -1;
    return rc;
}

/** Mount the root's image on `fx->root_at`, or for `ext2-dir` copy its tree
 * there through a read-only mount on `WORK/image`; 0 or -1. */
static int make_root(const struct fixtures *fx, const char *work)
{
    char path[FIX_PATH];
    struct fix_volume image = fx->root;

    if (strcmp(fx->root.type, "ext2-dir") != 0)
        return mount_volume(&fx->root, fx->root_at, 0);
    snprintf(image.type, sizeof(image.type), "ext2");
    if (fpath(path, "%s/image%s", work, "") < 0 || make_dir(path) < 0 ||
        mount_volume(&image, path, MS_RDONLY) < 0 || copy_tree(path, fx->root_at) < 0 ||
        umount2(path, 0) < 0)
        return -1;
    return 0;
}

/** Give the root `root` the machine's /proc, /dev and /sys by recursive bind
 * mounts; 0 or -1. Rach2 accepts a mount of proc only where proc already
 * is, and none on a name of /dev, so /proc is bound as the others are, and
 * /dev/shm and /dev/mqueue are the machine's, bound with /dev. */
static int root_mounts(const char *root)
{
    static const char *const bound[] = {"/proc", "/dev", "/sys"};
    char path[FIX_PATH];
    int i;

    for (i = 0; i < 3; i++)
        if (fpath(path, "%s%s", root, bound[i]) < 0 || make_dir(path) < 0 ||
            mount(bound[i], path, NULL, MS_BIND | MS_REC, NULL) < 0)
            return -1;
    return 0;
}

/**
 * Enter the case's root, in the child's own mount namespace: make it on
 * `fx->root_at`, give it its mounts, copy the program `prog` of the
 * workspace over the root's file at its path, and make it the root and the
 * working directory.
 */
static int enter_root(const struct fixtures *fx, const char *work, const char *prog)
{
    char path[FIX_PATH];
    struct stat st;

    if (make_root(fx, work) < 0 || root_mounts(fx->root_at) < 0)
        return -1;
    if (stat(prog, &st) < 0 || fpath(path, "%s%s", fx->root_at, fx->root_path) < 0 ||
        copy_file(prog, path, &st) < 0)
        return -1;
    if (chdir(fx->root_at) < 0 || chroot(".") < 0 || chdir("/") < 0)
        return -1;
    return 0;
}

/** Mount the volumes and the tmpfs of a case with a mount namespace of
 * its own in it; 0 or -1. */
static int mount_private(const struct fixtures *fx, const char *work)
{
    char path[FIX_PATH];
    int i;

    for (i = 0; i < fx->nvol; i++) {
        char name[16];
        snprintf(name, sizeof(name), "vol%d", i);
        if (fx->vol[i].mounted &&
            (fpath(path, "%s/%s", work, name) < 0 || mount_volume(&fx->vol[i], path, 0) < 0))
            return -1;
    }
    if (fx->tmpfs_bytes &&
        (fpath(path, "%s/tmpfs%s", work, "") < 0 || mount_tmpfs(fx->tmpfs_bytes, path, 0) < 0))
        return -1;
    return 0;
}

/** Make, in the child, the fixtures of `fx` that need the kernel, as
 * fixture_enter says; 0, or the errno of the step that failed. */
static int kernel_enter(const struct fixtures *fx, const char *work, char *prog, size_t cap,
                        const char **dir)
{
    errno = EIO;
    if ((fx->mountns && unshare(CLONE_NEWNS) < 0) || (fx->ipcns && unshare(CLONE_NEWIPC) < 0) ||
        (fx->utsns && unshare(CLONE_NEWUTS) < 0) || (fx->mountns && mount_private(fx, work) < 0))
        return errno;
    if (fx->has_root) {
        if (enter_root(fx, work, prog) < 0)
            return errno;
        if (snprintf(prog, cap, "%s", fx->root_path) >= (int)cap)
            return ENAMETOOLONG;
        *dir = "/";
    }
    return 0;
}

/** Keep the kernel's records written since `fx->log_before` as `DIR/log`;
 * 0 or -1. */
static int keep_log(struct fixtures *fx, const char *dir)
{
    char path[FIX_PATH];
    long len = 0, from = 0;
    char *after = read_log(&len);
    int fd, rc = -1;

    if (!after)
        return -1;
    if (fx->log_before_len > 0) {
        long n = fx->log_before_len < LOG_ANCHOR ? fx->log_before_len : LOG_ANCHOR;
        const char *anchor = fx->log_before + fx->log_before_len - n;
        long i;
        for (i = len - n; i >= 0; i--)
            if (memcmp(after + i, anchor, (size_t)n) == 0) {
                from = i + n;
                break;
            }
    }
    if (fpath(path, "%s/log%s", dir, "") == 0 &&
        (fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) >= 0) {
        rc = write(fd, after + from, (size_t)(len - from)) == len - from ? 0 : -1;
        close(fd);
    }
    free(after);
    return rc;
}

/** Detach loop device `n`; 0 or -1. */
static int detach(int n)
{
    char line[32];

    snprintf(line, sizeof(line), "detach %d", n);
    return loop_command(line);
}

/**
 * Give the file of a /proc/loop line, `loopN SIZE rw writes N syncs N FILE`,
 * whose FILE is the rest of the line and may have spaces; or NULL.
 */
static const char *loop_file(const char *line)
{
    int field;

    for (field = 0; field < 7 && line; field++) {
        line = strchr(line, ' ');
        if (line)
            line++;
    }
    return line;
}

/**
 * Detach each loop device that /proc/loop lists on a file under `dir`: those
 * the case's program attached itself and left. A device whose file is on
 * another such device's volume must go first, so each pass detaches what it
 * can and the passes repeat while one detaches something. 0, also where
 * there is no /proc/loop, as on Linux; or -1 when a device on a file under
 * `dir` is left or /proc/loop cannot be read.
 */
static int detach_left(const char *dir)
{
    char full[PATH_MAX];
    size_t len;
    int pass;

    if (!realpath(dir, full))
        return -1;
    len = strlen(full);
    for (pass = 0; pass <= FIX_VOLUMES; pass++) {
        char text[4096];
        int fd = open("/proc/loop", O_RDONLY | O_CLOEXEC);
        ssize_t n;
        int left = 0, gone = 0;
        char *line, *next;

        if (fd < 0)
            return errno == ENOENT ? 0 : -1;
        n = read(fd, text, sizeof(text) - 1);
        close(fd);
        if (n < 0)
            return -1;
        text[n] = '\0';
        for (line = text; *line; line = next) {
            const char *file;

            next = strchr(line, '\n');
            next = next ? (*next = '\0', next + 1) : line + strlen(line);
            file = loop_file(line);
            if (strncmp(line, "loop", 4) != 0 || !file || strncmp(file, full, len) != 0 ||
                file[len] != '/')
                continue;
            if (detach(atoi(line + 4)) == 0)
                gone++;
            else
                left++;
        }
        if (left == 0)
            return 0;
        if (gone == 0)
            return -1;
    }
    return -1;
}

/** Remove the fixtures of `fx` that need the kernel, as fixture_cleanup
 * says, and the record of what they made once all of it is removed; 0, or
 * -1 when something made could not be removed. */
static int kernel_cleanup(struct fixtures *fx, const char *dir)
{
    char path[FIX_PATH];
    int rc = 0, i;

    if (fx->tmpfs_global && (fpath(path, "%s/work/tmpfs%s", dir, "") < 0 || umount2(path, 0) < 0))
        rc = -1;
    for (i = fx->nvol - 1; i >= 0; i--) {
        char vol[16];
        snprintf(vol, sizeof(vol), "vol%d", i);
        if (fx->vol[i].global_mount &&
            (fpath(path, "%s/work/%s", dir, vol) < 0 || umount2(path, 0) < 0))
            rc = -1;
    }
    if (fx->private_dir && remove_private(dir) < 0)
        rc = -1;
    if (detach_left(dir) < 0)
        rc = -1;
    if (fx->log && keep_log(fx, dir) < 0)
        rc = -1;
    free(fx->log_before);
    fx->log_before = NULL;
    if (rc == 0 && fpath(path, "%s/fixtures%s", dir, "") == 0)
        unlink(path);
    return rc;
}
#endif

/**
 * Make the fixtures of the attempt in `dir`, whose workspace has the
 * invocation's files, in etude-guest itself: check that each id is no
 * account's and no process's, and that each port is free; read the
 * kernel's log; make the directory `private` of the workspace; attach each
 * volume and the root's image; mount the volumes
 * and the tmpfs of a case without a mount namespace of its own; and open the
 * pseudo-terminal. What is made is written to `DIR/fixtures` as it is made.
 *
 * @return NULL, or the error code with the reason in `*why`: `refused` for
 * an id or a port in use, `failed` for a fixture that could not be made.
 */
const char *fixture_prepare(struct fixtures *fx, const char *dir, const char **why)
{
    if ((*why = not_free(fx)) != NULL)
        return ETUDE_ERR_REFUSED;
    *why = "a fixture could not be made";
#if FIX_KERNEL
    if (kernel_prepare(fx, dir) < 0)
        return ETUDE_ERR_FAILED;
#else
    (void)dir;
#endif
    if (fx->pty && open_terminal(fx) < 0)
        return ETUDE_ERR_FAILED;
    return NULL;
}

/**
 * Append the arguments that give the fixtures' values to the program to
 * `argv`, which has `n` entries of `cap`: `ids=OLD:NEW,...`,
 * `ports=OLD:NEW,...`, `loops=DEVICE,...`, `peer=ADDRESS:PORT` and the
 * peer's NAME=VALUE arguments, and `root=PATH`, the private root's mount
 * point `STATE/r`, each when its fixture is there. The strings are allocated.
 *
 * @return the new count, or -1 when they do not fit.
 */
int fixture_args(const struct fixtures *fx, char **argv, int n, int cap)
{
    char arg[FIX_ARG];
    int i;

    if (n + 5 + fx->npeer_values >= cap)
        return -1;
    if (fx->nids) {
        pair_arg(arg, "ids", fx->id_old, fx->id_new, fx->nids);
        argv[n++] = strdup(arg);
    }
    if (fx->nports) {
        pair_arg(arg, "ports", fx->port_old, fx->port_new, fx->nports);
        argv[n++] = strdup(arg);
    }
    if (fx->nvol) {
        size_t len = (size_t)snprintf(arg, sizeof(arg), "loops=");
        for (i = 0; i < fx->nvol; i++)
            len += (size_t)snprintf(arg + len, sizeof(arg) - len, "%s/dev/loop%d", i ? "," : "",
                                    fx->vol[i].loop);
        argv[n++] = strdup(arg);
    }
    if (fx->peer[0]) {
        snprintf(arg, sizeof(arg), "peer=%s", fx->peer);
        argv[n++] = strdup(arg);
    }
    for (i = 0; i < fx->npeer_values; i++)
        argv[n++] = strdup(fx->peer_value[i]);
    if (fx->has_root) {
        if (snprintf(arg, sizeof(arg), "root=%s", fx->root_at) >= (int)sizeof(arg))
            return -1;
        argv[n++] = strdup(arg);
    }
    argv[n] = NULL;
    return n;
}

/**
 * Make, in the child before its exec, the fixtures that belong to its own
 * process: the pseudo-terminal as its descriptors 0 to 2 and controlling
 * terminal; its mount, IPC and UTS namespaces; the volumes and the tmpfs
 * mounted in its own mount namespace; and its private root. `prog`, of
 * `cap` bytes, is the program's path in the workspace, and becomes its path
 * in the root; `*dir` is the directory the child runs in.
 *
 * @return 0, or the errno of the step that failed.
 */
int fixture_enter(const struct fixtures *fx, const char *work, char *prog, size_t cap,
                  const char **dir)
{
    errno = EIO;
    if (fx->pty && enter_terminal(fx) < 0)
        return errno;
    *dir = work;
#if FIX_KERNEL
    return kernel_enter(fx, work, prog, cap, dir);
#else
    (void)prog;
    (void)cap;
    return 0;
#endif
}

/**
 * Remove the fixtures of the attempt in `dir` once its processes have
 * ended: close the pseudo-terminal, unmount what etude-guest mounted, remove
 * the directory `private` of the workspace with all it holds, detach the
 * loop devices on files of the attempt, its program's among them, and keep
 * the kernel's records as `DIR/log`. A mount namespace of the case's
 * own has ended with its last process, and the kernel unmounted its mounts
 * then.
 *
 * @return 0, or -1 when something made could not be removed.
 */
int fixture_cleanup(struct fixtures *fx, const char *dir)
{
    if (fx->pty_master >= 0) {
        close(fx->pty_master);
        fx->pty_master = -1;
    }
#if FIX_KERNEL
    return kernel_cleanup(fx, dir);
#else
    (void)dir;
    return 0;
#endif
}

/**
 * Remove what an attempt whose etude-guest ended before it could left: the
 * mounts the record `DIR/fixtures` names, in the order opposite to their
 * making, and the directory `private` of the workspace when it names that,
 * then every loop device on a file under `DIR`. A mount is recorded
 * before it is made, so a name of the record that is not a mount point is
 * passed by. Only the fixtures that need the kernel write the record, so
 * where FIX_KERNEL is 0 there is nothing to remove.
 *
 * @return 0, or -1 when something is left, or when the record exists and
 * cannot be read, since the mounts it names are then unknown.
 */
int fixture_settle(const char *dir)
{
#if FIX_KERNEL
    char path[FIX_PATH];
    char text[8192];
    char *line[64];
    int n = 0, rc = 0, fd;
    ssize_t len;

    if (fpath(path, "%s/fixtures%s", dir, "") < 0)
        return -1;
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return errno == ENOENT ? detach_left(dir) : -1;
    len = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (len < 0)
        return -1;
    text[len] = '\0';
    line[0] = strtok(text, "\n");
    while (n < 63 && line[n])
        line[++n] = strtok(NULL, "\n");
    while (n-- > 0) {
        if (strncmp(line[n], "mount ", 6) == 0 && umount2(line[n] + 6, 0) < 0 && errno != EINVAL &&
            errno != ENOENT)
            rc = -1;
        if (strcmp(line[n], "private") == 0 && remove_private(dir) < 0)
            rc = -1;
    }
    if (detach_left(dir) < 0)
        rc = -1;
    if (rc == 0)
        unlink(path);
    return rc;
#else
    (void)dir;
    return 0;
#endif
}
