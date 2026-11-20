/*
 * inode.c: the pstore file system, one directory holding a file for each
 * record ramoops found at boot, as Linux 6.12's fs/pstore/inode.c shows
 * them; removing a file erases its record.
 */
#include "pstore.h"

#include "../fs/synthfs.h"
#include "../fs/sysfs.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/kstd.h"
#include "../mm/kheap.h"
#include "../rtc.h"

/* The magic number statfs reports, Linux's PSTOREFS_MAGIC. */
#define PSTOREFS_MAGIC 0x6165676cu

#define PS_IFDIR 0040000u
#define PS_IFREG 0100000u

/*
 * A file, or the root directory. `refs` counts the walks and the open
 * descriptions that hold a file; one unlinked is off the list and is freed
 * by its last put. The root lasts the life of the kernel.
 */
struct pstore_file {
    char name[48];
    int type;
    uint32_t id;
    char *buf;           /* the record, kmalloc'd */
    uint32_t size;       /* saved record length */
    uint64_t inode_size; /* stat size after truncation */
    uint64_t ino;
    int64_t time_s, time_ns;   /* mtime: the record's, else made_ */
    int64_t ctime_s, ctime_ns; /* ctime: mtime's until a setattr sets it */
    int64_t made_s, made_ns;   /* atime: when the file was made, the first mount */
    uint32_t mode;             /* the permission bits: 0444, the root's 0750 */
    uint32_t uid, gid;         /* root's until a chown */
    uint32_t refs;
    int removed;
    struct pstore_file *next;
};

static struct pstore_file root;
static struct pstore_file *files; /* in the order they were found */
static uint64_t last_ino = 1;

/** Whether the strings `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Append the decimal digits of `v` to `s` at `*at`, within `cap`. */
static void put_num(char *s, uint32_t cap, uint32_t *at, uint32_t v)
{
    char d[10];
    int n = 0;

    do {
        d[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n > 0 && *at + 1 < cap) {
        s[(*at)++] = d[--n];
    }
}

/** Append `t` to `s` at `*at`, within `cap`. */
static void put_str(char *s, uint32_t cap, uint32_t *at, const char *t)
{
    while (*t && *at + 1 < cap) {
        s[(*at)++] = *t++;
    }
}

int pstore_record_add(const struct pstore_record *r)
{
    struct pstore_file *f = kzalloc(sizeof(*f)), **link;
    uint32_t at = 0;

    if (!f) {
        kfree(r->buf);
        return -ENOMEM;
    }
    put_str(f->name, sizeof(f->name), &at, r->type == PSTORE_TYPE_DMESG ? "dmesg" : "console");
    put_str(f->name, sizeof(f->name), &at, "-ramoops-");
    put_num(f->name, sizeof(f->name), &at, r->id);
    if (r->compressed) {
        put_str(f->name, sizeof(f->name), &at, ".enc.z");
    }
    f->name[at] = '\0';
    f->type = r->type;
    f->id = r->id;
    f->buf = r->buf;
    f->inode_size = f->size = r->size;
    f->ino = ++last_ino;
    f->time_s = f->ctime_s = r->sec;
    f->time_ns = f->ctime_ns = r->sec != 0 ? r->nsec : 0;
    f->mode = 0444u;
    for (link = &files; *link; link = &(*link)->next) {
    }
    *link = f;
    return 0;
}

/** The node `node` names, the root for NULL. */
static struct pstore_file *node_of(void *node)
{
    return node ? (struct pstore_file *)node : &root;
}

/** Free a file taken off the list once nothing holds it. */
static void settle(struct pstore_file *f)
{
    if (f != &root && f->removed && f->refs == 0) {
        kfree(f->buf);
        kfree(f);
    }
}

/** The `lookup` of pstore: `.` and `..`, which are the root, or a file,
 * held; -ENOTDIR below a file, -ENOENT for a name not there. */
static int ps_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct pstore_file *f;

    (void)mnt;
    if (node_of(dir) != &root) {
        return -ENOTDIR;
    }
    if (same(name, ".") || same(name, "..")) {
        *node = &root;
        return 0;
    }
    for (f = files; f; f = f->next) {
        if (same(f->name, name)) {
            f->refs++;
            *node = f;
            return 0;
        }
    }
    return -ENOENT;
}

/** The `hold` of pstore. */
static void ps_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    node_of(node)->refs++;
}

/** The `put` of pstore: an unlinked file goes with its last hold. */
static void ps_put(struct vfs_mount *mnt, void *node)
{
    struct pstore_file *f = node_of(node);

    (void)mnt;
    if (f != &root) {
        f->refs--;
        settle(f);
    }
}

/** The `getattr` of pstore: the root 0750 and each file 0444, root's, as
 * Linux's pstore_get_inode and pstore_mkfile make them (inode.c:360, 424);
 * a file's size is its record's, its atime the time it was made, and its
 * mtime and ctime the record's when ramoops wrote one, else the same. */
static int ps_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct pstore_file *f = node_of(node);

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->mode = (f == &root ? PS_IFDIR : PS_IFREG) | f->mode;
    st->uid = f->uid;
    st->gid = f->gid;
    st->nlink = f == &root ? 2 : f->removed ? 0 : 1;
    st->size = f->inode_size;
    st->ino = f->ino;
    st->atime_sec = f->made_s;
    st->atime_nsec = f->made_ns;
    st->mtime_sec = f->time_s;
    st->mtime_nsec = f->time_ns;
    st->ctime_sec = f->ctime_s;
    st->ctime_nsec = f->ctime_ns;
    return 0;
}

/**
 * The `setattr` of pstore, as Linux's simple_setattr, which pstore's inodes
 * take: the permission bits of `st->mode`, the owner, the group and the
 * access and modification times of a file or the root, as `what` selects,
 * and the change time set to the clock for every call, a VFS_SET_CTIME call
 * with nothing else among them. The caller has judged the permission. A
 * size or inode flags are -EINVAL and -EOPNOTSUPP, and change nothing; 0
 * otherwise.
 */
static int ps_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct pstore_file *f = node_of(node);

    (void)mnt;
    if (what & VFS_SET_FLAGS) {
        return -EOPNOTSUPP;
    }
    if (what & VFS_SET_SIZE) {
        return -EINVAL;
    }
    if (what & VFS_SET_MODE) {
        f->mode = st->mode & 07777u;
    }
    if (what & VFS_SET_UID) {
        f->uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        f->gid = st->gid;
    }
    if (what & VFS_SET_ATIME) {
        f->made_s = st->atime_sec;
        f->made_ns = st->atime_nsec;
    }
    if (what & VFS_SET_MTIME) {
        f->time_s = st->mtime_sec;
        f->time_ns = st->mtime_nsec;
    }
    rtc_coarse_realtime(&f->ctime_s, &f->ctime_ns);
    return 0;
}

/** Set the stat size of a record, keeping the saved bytes for reads. */
static int ps_truncate(struct vfs_mount *mnt, void *handle, uint64_t size, int stamp_times)
{
    struct pstore_file *f = node_of(handle);

    (void)mnt;
    if (f == &root) {
        return -EISDIR;
    }
    if (size > (sizeof(long) == 4 ? 0xFFFFFFFF000ULL : 0x7FFFFFFFFFFFFFFFULL)) {
        return -EFBIG;
    }
    f->inode_size = size;
    if (stamp_times) {
        rtc_coarse_realtime(&f->ctime_s, &f->ctime_ns);
        f->time_s = f->ctime_s;
        f->time_ns = f->ctime_ns;
    }
    return 0;
}

/** The `open` of pstore: the handle is the node. */
static int ps_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)mnt;
    (void)flags;
    *handle = node_of(node);
    return 0;
}

/** The `release` of pstore, which keeps nothing per open. */
static void ps_release(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    (void)handle;
}

/** The `read` of pstore: the record's bytes from `offset`, into a kernel
 * buffer; -EISDIR for the root. */
static int32_t ps_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                       uint64_t offset)
{
    struct pstore_file *f = node_of(handle);

    (void)mnt;
    if (f == &root) {
        return -EISDIR;
    }
    if (offset >= f->size) {
        return 0;
    }
    if (count > f->size - offset) {
        count = f->size - (uint32_t)offset;
    }
    k_memcpy(buf, f->buf + offset, count);
    return (int32_t)count;
}

/** The `readdir` of pstore: 0 is ".", 1 is "..", and the files follow in
 * the order they were found, the cursor counting them. */
static int ps_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    struct pstore_file *f;
    uint32_t i;

    (void)mnt;
    if (node_of(handle) != &root) {
        return -ENOTDIR;
    }
    k_memset(ent, 0, sizeof(*ent));
    if (*cursor < 2) {
        ent->name[0] = '.';
        ent->name[1] = *cursor == 1 ? '.' : '\0';
        ent->ino = root.ino;
        ent->is_dir = 1;
        ent->type = VFS_DT_DIR;
        (*cursor)++;
        return 1;
    }
    i = 2;
    for (f = files; f && i < *cursor; f = f->next) {
        i++;
    }
    if (!f) {
        return 0;
    }
    k_strncpy(ent->name, f->name, VFS_NAME_MAX);
    ent->ino = f->ino;
    ent->type = VFS_DT_REG;
    (*cursor)++;
    return 1;
}

/** The `unlink` of pstore: take the file off the list and erase its
 * record, as pstore_unlink (inode.c:182) with ramoops_pstore_erase. */
static int ps_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct pstore_file **link, *f;

    (void)mnt;
    if (node_of(dir) != &root) {
        return -ENOTDIR;
    }
    for (link = &files; *link; link = &(*link)->next) {
        if (same((*link)->name, name)) {
            f = *link;
            *link = f->next;
            f->removed = 1;
            ramoops_erase(f->type, f->id);
            settle(f);
            return 0;
        }
    }
    return -ENOENT;
}

/** The `statfs` of pstore, which has no blocks. */
static int ps_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->magic = PSTOREFS_MAGIC;
    st->bsize = 4096;
    st->namelen = VFS_NAME_MAX;
    return 0;
}

/** The `path` of pstore, whose one directory is its root. */
static int ps_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    (void)mnt;
    if (node_of(node) != &root) {
        return -ENOTDIR;
    }
    if (cap < 2) {
        return -ENAMETOOLONG;
    }
    buf[0] = '/';
    buf[1] = '\0';
    return 0;
}

/* The anonymous mount lasts while a visible mount or a file holds it. */
static struct vfs_mount *pstore_base;

/** Drop the singleton once its last mount and file have gone. */
static void ps_unmount(struct vfs_mount *mnt)
{
    (void)mnt;
    pstore_base = NULL;
}

#define PSTORE_DEFAULT_KMSG_BYTES 10240u
static uint32_t kmsg_bytes = PSTORE_DEFAULT_KMSG_BYTES;

int pstore_dump_enabled(void)
{
    return kmsg_bytes != 0;
}

/** Apply valid kmsg_bytes values in order, as Linux's parse_options does. */
static int ps_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    const char key[] = "kmsg_bytes=";

    (void)mnt;
    (void)flags;
    while (options && *options) {
        const char *end = options;
        size_t i = 0;
        unsigned long value;

        while (*end && *end != ',') {
            end++;
        }
        while (key[i] && key[i] == options[i]) {
            i++;
        }
        if (!key[i] && options[i] >= '0' && options[i] <= '9' && end[-1] != '\n' &&
            k_strtoul(options + i, end - (options + i), &value) == 0 && value <= 0x7fffffffu) {
            kmsg_bytes = (uint32_t)value;
        }
        options = *end ? end + 1 : end;
    }
    return 0;
}

/** Print the shared dump threshold when it differs from its default. */
static void ps_options(const struct vfs_mount *mnt, vfs_option_emit emit, void *out)
{
    (void)mnt;
    if (kmsg_bytes != PSTORE_DEFAULT_KMSG_BYTES) {
        emit(out, "kmsg_bytes", kmsg_bytes, VFS_OPT_DEC);
    }
}

static const struct vfs_ops pstore_vops = {
    ps_lookup,   0,                                                                /* lookup_rest */
    ps_hold,     ps_put,  ps_getattr, ps_setattr, ps_open, ps_release, ps_read, 0, /* write */
    ps_truncate, 0,                                                                /* fsync */
    ps_readdir,  0,                                                                /* create */
    ps_unlink,   0,                                                                /* mkdir */
    0,                                                                             /* rmdir */
    0,                                                                             /* rename */
    0,                                                                             /* link */
    0,                                                                             /* readlink */
    ps_statfs,   0,                                                                /* sync */
    0,                       /* prepare_unmount */
    0,                       /* abort_unmount */
    ps_unmount,  ps_path, 0, /* symlink */
    0,                       /* mknod */
    0,                       /* pin */
    0,                       /* unpin */
    ps_remount,  0,          /* fallocate */
    0,                       /* fdatasync */
    0,                       /* lock_node */
    0,                       /* unlock_node */
    0,                       /* shutdown */
    0,                       /* xattr */
    0,                       /* cachestat */
    0,                       /* put_unless_last */
    0,                       /* readlink_cached */
    0,                       /* read_frames */
    0,                       /* handle_node */
    0,                       /* writeback */
    0,                       /* lock_write */
    0,                       /* unlock_write */
    0,                       /* rename_flags */
    0,                       /* tmpfile */
    0,                       /* uncache */
    0,                       /* phase */
};

/** The constructor: every mount shows the one directory, as Linux's
 * single superblock does. Its dump threshold is shared across mounts. */
static int pstore_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                        const char *options)
{
    struct pstore_file *f;

    int rc;

    (void)dev;
    if (pstore_base) {
        rc = vfs_remount(pstore_base, flags, options);
        if (rc != 0) {
            return rc;
        }
        vfs_mount_get(pstore_base);
    } else {
        pstore_base = vfs_kernel_mount(&vfs_pstore_type, &root, flags);
        if (!pstore_base) {
            return -ENOMEM;
        }
        pstore_base->show_options = ps_options;
        ps_remount(pstore_base, flags, options);
    }
    mnt->bind_of = pstore_base;
    k_strncpy(mnt->bind_root, "/", sizeof(mnt->bind_root));
    if (root.ino == 0) {
        root.ino = 1;
        rtc_coarse_realtime(&root.time_s, &root.time_ns);
        root.made_s = root.ctime_s = root.time_s;
        root.made_ns = root.ctime_ns = root.time_ns;
        root.mode = 0750u;
        /* Each file is made now, as Linux's pstore_fill_super makes the
         * files at mount (inode.c:436): pstore_get_inode sets its three
         * times to the clock (inode.c:223), and pstore_mkfile then sets
         * only the mtime and ctime of a timed record to the record's
         * (inode.c:380-382). ramoops finds the records before the clock
         * is read. */
        for (f = files; f; f = f->next) {
            f->made_s = root.time_s;
            f->made_ns = root.time_ns;
            if (f->time_s == 0) {
                f->time_s = f->ctime_s = root.time_s;
                f->time_ns = f->ctime_ns = root.time_ns;
            }
        }
    }
    mnt->priv = 0;
    mnt->root = &root;
    return 0;
}

const struct vfs_type vfs_pstore_type = {"pstore", pstore_mount,   &pstore_vops, 1, 1, 0, 0, 0, 0,
                                         0,        VFS_DIRECT_NONE};

void pstore_start(void)
{
    int sys, fs;

    if (!ramoops_ready()) {
        return;
    }
    sys = sysfs_anchor(SYSFS_ROOT, 0);
    fs = sys > 0 ? sysfs_add_dir(sys, "fs") : sys;
    if (fs <= 0 || sysfs_add_dir(fs, "pstore") <= 0 ||
        vfs_mount_in_sys(&vfs_pstore_type, "/sys/fs/pstore", "pstore",
                         VFS_MS_NOSUID | VFS_MS_NODEV | VFS_MS_NOEXEC) != 0) {
        klog(KLOG_ERR, "pstore", "refused: /sys/fs/pstore could not be made");
    }
}
