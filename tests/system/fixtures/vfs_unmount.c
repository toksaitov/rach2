/**
 * vfs_unmount: the unmount functions of rach2/sys/fs/vfs.c, compiled whole
 * with substitutes for the calls they make, and checked in 26 cases, for
 * the vfs-unmount case of tests/run usbdisk.
 *
 * Preparation copies the functions into unmount.inc. The substitutes for the
 * file system, the block cache and the scheduler count what the path does,
 * so that a case can check which mounts it releases and that nothing is
 * written to a device that is gone. The model is a root, a mount at /mnt/a
 * whose device is gone, a mount at /mnt/a/b below it, and a spare. The cases
 * also check that a caller with a root of its own names the target from that
 * root, and that a target named as a directory, by a path that ends in one,
 * must reach a directory, the node of the one walk being the node checked.
 * The pages in memory of a mount's files are ended (`filemap_unmount`) only
 * for a mount that is no bind, once nothing holds it and its operations
 * have drained, and before its file system is synced, and a bind's unmount
 * leaves them to its base. The failure cases check that a failed sync, an
 * earlier failed write, and a writeback or a flush that fails after the
 * preparation each let the unmount complete, as Linux's does, with no
 * preparation or writeback after the failure, the preparation put back after
 * it, and the device's buffers dropped. The USB disk rows test the same path
 * with a real device.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VFS_ACTIVE           2
#define VFS_UNMOUNTING       3
#define VFS_COVERED_PATH_MAX 256
#define BLOCK_GONE           1
#define BLOCK_WRITE_FAILED   2
#define KLOG_INFO            0
#define KLOG_WARN            1
#define k_strlen             strlen
#define k_memcpy             memcpy
/* This model's permanent entries, the root and two others; the kernel has
 * more, and the rows need the three slots after them for mounts. */
#define VFS_PERMANENT        3
/* The flag of an unmount's walk, as vfs.c defines it. */
#define WALK_UNMOUNT         0x100

/* The parts of the kernel's structures that the unmount path reads. */
struct block_device {
    int flags, backing_flags;
};
typedef struct block_device block_device_t;
struct vfs_mount;
struct vfs_ops {
    int (*sync)(struct vfs_mount *);
    int (*prepare_unmount)(struct vfs_mount *);
    void (*abort_unmount)(struct vfs_mount *);
    void (*put)(struct vfs_mount *, void *);
    void (*unmount)(struct vfs_mount *);
};
struct vfs_type {
    struct vfs_ops *ops;
};
struct vfs_mount {
    struct block_device *dev;
    struct vfs_type *type;
    struct vfs_mount *parent;
    void *root, *covered;
    const char *covered_path;
    int state, refs, inprogress, drain_wq, claim, opens, releases;
    struct vfs_mount *bind_of;
};
struct vfs_ref {
    struct vfs_mount *mnt;
    void *node;
};
struct vfs_stat {
    uint32_t mode;
};
#define VFS_IS_DIR(mode) (((mode) & 0170000u) == 0040000u)
struct fs_struct {
    struct vfs_mount *root_mnt;
    void *root_node;
};
struct process {
    struct fs_struct *fs;
};
static struct vfs_mount records[6], *mounts[6];
static struct block_device disks[6];
static int mount_slots = 6;
static int lookup_error, resolve_calls, syncs, prepares, flushes, invalidates;
static int puts_count, releases, claims, locks, waits, sync_error, failures;
static int writeback_error, flush_error, aborts, warnings, dirty_left;
static struct fs_struct caller_fs;
static struct process caller = {&caller_fs};
static struct process *current_proc = &caller;
static const char *root_path; /* the path of the caller's root, when it has one */
static int root_holds, ref_releases;
static uint32_t node_mode; /* the mode the node a walk finds has */
static int getattrs;
static int file_node;   /* a node that is no mount's root */
static void *walk_node; /* the node a walk finds, or NULL for the mount's root */

/** The flags of the device under `d`. */
static unsigned block_flags(struct block_device *d)
{
    return d->backing_flags;
}
/** Count a warning the unmount logs; the text is not read. */
static void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level == KLOG_WARN)
        warnings++;
}

/** Count a file system sync; `sync_error` is its result. */
static int sync_mount(struct vfs_mount *m)
{
    (void)m;
    syncs++;
    return sync_error;
}
/** Count a file system's preparation for unmount. */
static int prepare(struct vfs_mount *m)
{
    (void)m;
    prepares++;
    return 0;
}
/** Count a node released. */
static void put(struct vfs_mount *m, void *node)
{
    (void)m;
    (void)node;
    puts_count++;
}
/** Count a file system released. */
static void unmount_fs(struct vfs_mount *m)
{
    (void)m;
    releases++;
}
/** Count the preparation put back (`abort_unmount`). */
static void abort_prepare(struct vfs_mount *m)
{
    (void)m;
    aborts++;
}
static struct vfs_ops ops = {sync_mount, prepare, abort_prepare, put, unmount_fs};
static struct vfs_type type = {&ops};

/**
 * The walk to `path`, which always reaches the mount at /mnt/a, at the node
 * in `walk_node` or at its root, and returns `lookup_error`. Each call is
 * counted, so that a case can check that no second walk follows an EIO.
 */
static int resolve_path(int a, const char *path, int flags, struct vfs_ref *ref, int c, int d,
                        int real)
{
    (void)a;
    (void)path;
    (void)flags;
    (void)c;
    (void)d;
    (void)real;
    resolve_calls++;
    ref->mnt = &records[3];
    ref->node = walk_node ? walk_node : records[3].root;
    return lookup_error;
}
/** Count a reference released. */
static void vfs_ref_release(struct vfs_ref *r)
{
    (void)r;
    ref_releases++;
}
/** The attributes of `node`: the mode in `node_mode`, a directory unless a
 * case sets another, or EIO when the device of `m` is gone. Counted. */
static int vfs_getattr(struct vfs_mount *m, void *node, struct vfs_stat *st)
{
    (void)node;
    getattrs++;
    if (m->dev->flags & BLOCK_GONE) {
        return -EIO;
    }
    st->mode = node_mode;
    return 0;
}
/** Entering a mount always succeeds. */
static int vfs_enter(struct vfs_mount *m)
{
    (void)m;
    return 0;
}
/** Count a hold on the caller's root. */
static void vfs_ref_hold(const struct vfs_ref *r)
{
    (void)r;
    root_holds++;
}
/** The path of the caller's root, `root_path`; the path asks for no other
 * node, and the fixture aborts if it does. */
static int vfs_node_path(struct vfs_mount *m, void *node, char *out, uint32_t cap)
{
    (void)m;
    if (node != caller_fs.root_node || strlen(root_path) + 1 > cap)
        abort();
    strcpy(out, root_path);
    return 0;
}
/** Release the node of `r`. */
static void vfs_ref_put(struct vfs_ref *r)
{
    put(r->mnt, r->node);
}
/** Count the namespace lock taken. */
static void vfs_ns_lock(struct vfs_mount *m)
{
    (void)m;
    locks++;
}
/** Count the namespace lock released. */
static void vfs_ns_unlock(struct vfs_mount *m)
{
    (void)m;
    locks--;
}
/** Whether no operation is in progress on `m`. */
static int drained(struct vfs_mount *m)
{
    return m->inprogress == 0;
}
/** The wait for `m` to drain: it must be unmounting and not yet drained,
 * or the fixture aborts; the wait is counted and sets `inprogress` to 0. */
static void wq_wait_event_nointr(int *q, int (*ready)(struct vfs_mount *), struct vfs_mount *m)
{
    (void)q;
    if (m->state != VFS_UNMOUNTING || ready(m))
        abort();
    waits++;
    m->inprogress = 0;
}
/** The block cache's writeback of `d`, which `writeback_error` fails. */
static int bcache_sync(struct block_device *d)
{
    (void)d;
    return writeback_error;
}
/** Count a device flush, which records a failure itself (`bcache_flush`);
 * `flush_error` is its result. */
static int bcache_flush(struct block_device *d)
{
    (void)d;
    flushes++;
    return flush_error;
}
/** Count the block cache of a device dropped, `dirty_left` dirty buffers
 * among its buffers, and clear its failed-write mark. */
static int bcache_drop(struct block_device *d)
{
    d->flags &= ~BLOCK_WRITE_FAILED;
    invalidates++;
    return dirty_left;
}
/** Count the claims given back. */
static void devfs_release_claim(int claim)
{
    claims += claim;
}
/** Free the mount record `m`. */
static void mount_release(struct vfs_mount *m)
{
    m->state = 0;
}

/** Drop the inotify watches on the files of `m`; there are none. */
static void inotify_unmount(struct vfs_mount *m)
{
    (void)m;
}

/** Drop the page cache's copies of the files of `m`; there are none. */
static void filecache_forget_mount(const struct vfs_mount *m)
{
    (void)m;
}

/** Forget the name cache's entries of `m`; the fixture keeps none. */
static void dcache_forget_mount(struct vfs_mount *m)
{
    (void)m;
}

/* The calls of `filemap_unmount`, the mount of the last, and whether each
 * came as it must: on a mount that is no bind, unmounting, drained, held by
 * nothing, with no namespace lock held and before any sync of this case. */
static int fm_unmounts, fm_misplaced;
static struct vfs_mount *fm_last;

/** Count the end of the pages in memory of the files of `m`, and whether
 * it came in its place in the unmount. */
static void filemap_unmount(struct vfs_mount *m)
{
    if (m->refs != 0 || m->bind_of || m->state != VFS_UNMOUNTING || m->inprogress != 0 ||
        locks != 0 || syncs != 0 || prepares != 0)
        fm_misplaced++;
    fm_unmounts++;
    fm_last = m;
}

/** Let a loop device set to clear itself go; no loop device is mounted. */
static void loop_unmounted(struct block_device *d)
{
    (void)d;
}

/** Whether another mount namespace sees `m`: none, as the caller's is the
 * only one, so that an unmount never only hides the mount from the caller. */
static int vfs_mount_shared(const struct vfs_mount *m)
{
    (void)m;
    return 0;
}
/** Take `m` from the caller's view; never reached, as no mount is shared. */
static void vfs_mount_hide(struct vfs_mount *m)
{
    (void)m;
    failures++;
}

/** Put one of a mount's references, which vfs.c counts atomically; the
 * fixture has one thread. Returns the count left. */
static uint32_t vfs_mount_put(struct vfs_mount *m)
{
    return (uint32_t)--m->refs;
}

/** Store `value` at `*p` and return what was there, as the kernel's full
 * barrier exchange does for one thread. */
static unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

/* The tree's sequence, which an unmount moves. */
static uint32_t tree_seq;

static int unmount_mount(struct vfs_mount *m);

#include "unmount.inc"

/**
 * Reset the model: the root, /mnt/a whose device is gone and which a walk
 * cannot read past (EIO), its live child /mnt/a/b, a spare record for deeper
 * trees, and every count at 0.
 */
static void setup(void)
{
    int i;
    memset(records, 0, sizeof(records));
    memset(disks, 0, sizeof(disks));
    for (i = 0; i < 6; i++) {
        mounts[i] = &records[i];
        records[i].dev = &disks[i];
        records[i].type = &type;
        records[i].root = &records[i];
        records[i].parent = i ? &records[0] : NULL;
        records[i].covered_path = "/unused";
        records[i].state = VFS_ACTIVE;
        records[i].claim = 1;
    }
    records[3].parent = &records[0];
    records[3].covered_path = "/mnt/a";
    records[3].refs = 1;
    records[4].parent = &records[3];
    records[4].covered_path = "/mnt/a/b";
    disks[3].flags = BLOCK_GONE;
    records[0].refs = 1;
    lookup_error = -EIO;
    resolve_calls = syncs = prepares = flushes = invalidates = 0;
    puts_count = releases = claims = locks = waits = sync_error = 0;
    writeback_error = flush_error = aborts = warnings = dirty_left = 0;
    memset(&caller_fs, 0, sizeof(caller_fs));
    root_path = NULL;
    root_holds = ref_releases = 0;
    node_mode = 0040755;
    getattrs = 0;
    walk_node = NULL;
    fm_unmounts = fm_misplaced = 0;
    fm_last = NULL;
}

/** Count a failed case, `name`, and report it. */
static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "vfs-unmount: failed: %s\n", name);
        failures++;
    }
}

/** The cases of a busy and a live child below a mount whose device has
 * gone, and of that mount's own unmount after them. */
static void child_cases(void)
{
    setup();
    records[4].refs = 1;
    check(vfs_do_umount("/mnt/a/b", 0) == -EBUSY, "child description remains busy");
    check(records[4].state == VFS_ACTIVE && !syncs && !claims && !fm_unmounts,
          "busy child stays intact, its files' pages untouched");
    records[4].refs = 0;
    records[4].inprogress = 1;
    check(vfs_do_umount("/mnt/a/b", 0) == 0, "live child below removed parent unmounts");
    check(records[3].refs == 0 && !records[4].state && claims == 1 && puts_count == 2 &&
              releases == 1 && invalidates == 1 && !locks && waits == 1,
          "child releases its claim, nodes and parent after draining");
    check(syncs == 1 && prepares == 1 && flushes == 1 && resolve_calls == 2,
          "live child flushes without another ancestor lookup");
    check(fm_unmounts == 1 && fm_last == &records[4] && !fm_misplaced,
          "live child ends its files' pages once nothing holds it, before its sync");
    lookup_error = 0;
    check(vfs_do_umount("/mnt/a", 0) == 0 && records[0].refs == 0 && claims == 2 && syncs == 1 &&
              flushes == 1,
          "removed parent releases without writes");
}

/** The cases of a bind's unmount, held and not. */
static void bind_cases(void)
{
    setup();
    records[5].bind_of = &records[4];
    records[5].covered_path = "/mnt/bind";
    records[4].refs = 1;
    records[5].refs = 1;
    check(unmount_mount(&records[5]) == -EBUSY && records[5].state == VFS_ACTIVE &&
              records[4].refs == 1 && !fm_unmounts,
          "a held bind stays mounted, its base's files' pages untouched");
    records[5].refs = 0;
    check(unmount_mount(&records[5]) == 0 && !records[5].state && records[4].refs == 0 &&
              records[4].state == VFS_ACTIVE && !fm_unmounts && !syncs,
          "a bind's unmount lets its base go and leaves the base's files' pages to the base");
}

/** The cases of an unmount whose sync failed, or whose device's earlier
 * write did, which completes, as Linux's does, with no preparation or
 * writeback after the failure, the device's buffers dropped and one
 * warning. */
static void failure_cases(void)
{
    setup();
    sync_error = -EIO;
    dirty_left = 2;
    check(vfs_do_umount("/mnt/a/b", 0) == 0 && records[3].refs == 0 && claims == 1 &&
              !records[4].state && !locks && releases == 1,
          "a failed sync does not keep the mount");
    check(syncs == 1 && !prepares && flushes == 1 && !aborts && invalidates == 1 && warnings == 1,
          "a failed sync skips the preparation and the writeback, flushes and drops the buffers");
    setup();
    disks[4].flags = BLOCK_WRITE_FAILED;
    check(vfs_do_umount("/mnt/a/b", 0) == 0 && !records[4].state && syncs == 1 && !prepares &&
              flushes == 1 && invalidates == 1 && warnings == 1 && !disks[4].flags,
          "a device whose earlier write failed is unmounted without the preparation");
}

/** The cases of an unmount whose writeback or flush fails after the
 * preparation, which the type puts back before the unmount completes, and of
 * one whose writes all succeed, which logs nothing. */
static void prepared_failure_cases(void)
{
    setup();
    writeback_error = -EIO;
    check(vfs_do_umount("/mnt/a/b", 0) == 0 && !records[4].state && prepares == 1 && aborts == 1 &&
              flushes == 1 && invalidates == 1 && warnings == 1,
          "a writeback failed after the preparation puts it back and unmounts");
    setup();
    flush_error = -EIO;
    check(vfs_do_umount("/mnt/a/b", 0) == 0 && !records[4].state && prepares == 1 && aborts == 1 &&
              invalidates == 1 && warnings == 1,
          "a flush failed after the preparation puts it back and unmounts");
    setup();
    check(vfs_do_umount("/mnt/a/b", 0) == 0 && !aborts && !warnings,
          "an unmount that writes everything logs no warning");
}

/** Run the cases; 0 when all pass, 1 otherwise. */
int main(void)
{
    child_cases();
    setup();
    disks[4].backing_flags = BLOCK_GONE;
    check(vfs_do_umount("/mnt/a/b", 0) == 0 && !syncs && !prepares && !flushes,
          "removed child backing receives no writes");
    failure_cases();
    prepared_failure_cases();
    setup();
    lookup_error = -ENOENT;
    check(vfs_do_umount("/mnt/a/b", 0) == -ENOENT, "ordinary lookup error is retained");
    setup();
    disks[3].flags = 0;
    check(vfs_do_umount("/mnt/a/b", 0) == -EIO, "present ancestor cannot use recovery lookup");
    setup();
    check(vfs_do_umount("/mnt/a/b/file", 0) == -EIO, "target must match the whole recorded name");
    records[5].parent = &records[3];
    records[5].covered_path = records[4].covered_path;
    check(vfs_do_umount("/mnt/a/b", 0) == -EIO, "ambiguous recorded names are refused");
    setup();
    records[5].parent = &records[4];
    records[5].covered_path = "/mnt/a/b/c";
    records[4].refs = 1;
    check(vfs_do_umount("/mnt/a/b/c", 0) == 0 && !records[4].refs,
          "removed grandparent permits deepest mount to unmount");
    setup();
    caller_fs.root_mnt = &records[0];
    caller_fs.root_node = &caller_fs;
    root_path = "/mnt";
    check(vfs_do_umount("/a/b", 0) == 0 && !records[4].state && root_holds == 1 &&
              ref_releases == 1,
          "a target named from the caller's root is found below it");
    setup();
    caller_fs.root_mnt = &records[0];
    caller_fs.root_node = &caller_fs;
    root_path = "/elsewhere";
    check(vfs_do_umount("/a/b", 0) == -EIO && records[4].state == VFS_ACTIVE,
          "a target outside the caller's root matches nothing");
    setup();
    lookup_error = 0;
    disks[3].flags = 0;
    walk_node = &file_node;
    node_mode = 0100644;
    check(vfs_do_umount("/mnt/a/f", 1) == -ENOTDIR && records[3].state == VFS_ACTIVE &&
              resolve_calls == 1 && getattrs == 1 && ref_releases == 1,
          "a target named as a directory that reaches a file is ENOTDIR from the one walk");
    setup();
    lookup_error = 0;
    records[3].refs = 0;
    check(vfs_do_umount("/mnt/a", 1) == 0 && !records[3].state && resolve_calls == 1 &&
              getattrs == 0,
          "a target named as a directory that reaches a removed device's mount point unmounts it");
    setup();
    check(vfs_do_umount("/mnt/a/b", 1) == 0 && !records[4].state && !getattrs,
          "a target named as a directory below a removed device unmounts by its recorded name");
    bind_cases();
    if (failures)
        return 1;
    puts("vfs-unmount: result: 26 checks passed");
    return 0;
}
