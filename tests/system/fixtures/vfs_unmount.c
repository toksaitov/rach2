/**
 * Run the production unmount path with a missing ancestor and counted releases.
 * The generated include contains whole functions from vfs.c. Filesystem and
 * scheduler substitutes check ownership and writes; the USB rows test real I/O.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VFS_ACTIVE     2
#define VFS_UNMOUNTING 3
#define BLOCK_GONE     1
#define KLOG_INFO      0
#define klog(...)      ((void)0)
/* This model's permanent entries, the root and two others; the kernel has
 * more, and the rows need the three slots after them for mounts. */
#define VFS_PERMANENT  3

struct block_device {
    int flags, backing_flags;
};
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
    int state, refs, inprogress, ns_wq, claim, opens, releases;
};
struct vfs_ref {
    struct vfs_mount *mnt;
    void *node;
};
static struct vfs_mount records[6], *mounts[6];
static struct block_device disks[6];
static int mount_slots = 6;
static int lookup_error, resolve_calls, syncs, prepares, flushes, invalidates;
static int puts_count, releases, claims, locks, waits, sync_error, failures;

static unsigned block_flags(struct block_device *d)
{
    return d->backing_flags;
}
static int sync_mount(struct vfs_mount *m)
{
    (void)m;
    syncs++;
    return sync_error;
}
static int prepare(struct vfs_mount *m)
{
    (void)m;
    prepares++;
    return 0;
}
static void put(struct vfs_mount *m, void *node)
{
    (void)m;
    (void)node;
    puts_count++;
}
static void unmount_fs(struct vfs_mount *m)
{
    (void)m;
    releases++;
}
static struct vfs_ops ops = {sync_mount, prepare, NULL, put, unmount_fs};
static struct vfs_type type = {&ops};

/** No second pathname attempt is permitted after the simulated EIO. */
static int resolve_path(int a, const char *path, int b, struct vfs_ref *ref, int c, int d,
                        int unmount)
{
    (void)a;
    (void)path;
    (void)b;
    (void)c;
    (void)d;
    (void)unmount;
    resolve_calls++;
    ref->mnt = &records[3];
    ref->node = records[3].root;
    return lookup_error;
}
static void vfs_ref_release(struct vfs_ref *r)
{
    (void)r;
}
static void vfs_ref_put(struct vfs_ref *r)
{
    put(r->mnt, r->node);
}
static void vfs_ns_lock(struct vfs_mount *m)
{
    (void)m;
    locks++;
}
static void vfs_ns_unlock(struct vfs_mount *m)
{
    (void)m;
    locks--;
}
static int drained(struct vfs_mount *m)
{
    return m->inprogress == 0;
}
static void wq_wait_event_nointr(int *q, int (*ready)(struct vfs_mount *), struct vfs_mount *m)
{
    (void)q;
    if (m->state != VFS_UNMOUNTING || ready(m))
        abort();
    waits++;
    m->inprogress = 0;
}
static int bcache_sync(struct block_device *d)
{
    (void)d;
    return 0;
}
static int block_flush(struct block_device *d)
{
    (void)d;
    flushes++;
    return 0;
}
static int bcache_invalidate(struct block_device *d)
{
    (void)d;
    invalidates++;
    return 0;
}
static void devfs_release_claim(int claim)
{
    claims += claim;
}
static void mount_release(struct vfs_mount *m)
{
    m->state = 0;
}

/* The unmount path reports a failed flush to the device layer, which this
 * fixture has no device to carry out. */
static void vfs_device_failed(struct block_device *dev, unsigned long long lba, int rc)
{
    (void)dev;
    (void)lba;
    (void)rc;
}

#include "unmount.inc"

/** A missing parent, its live child and a spare record for deeper trees. */
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
}

static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "vfs-unmount: failed: %s\n", name);
        failures++;
    }
}

int main(void)
{
    setup();
    records[4].refs = 1;
    check(vfs_do_umount("/mnt/a/b") == -EBUSY, "child description remains busy");
    check(records[4].state == VFS_ACTIVE && !syncs && !claims, "busy child stays intact");
    records[4].refs = 0;
    records[4].inprogress = 1;
    check(vfs_do_umount("/mnt/a/b") == 0, "live child below removed parent unmounts");
    check(records[3].refs == 0 && !records[4].state && claims == 1 && puts_count == 2 &&
              releases == 1 && invalidates == 1 && !locks && waits == 1,
          "child releases its claim, nodes and parent after draining");
    check(syncs == 1 && prepares == 1 && flushes == 1 && resolve_calls == 2,
          "live child flushes without another ancestor lookup");
    lookup_error = 0;
    check(vfs_do_umount("/mnt/a") == 0 && records[0].refs == 0 && claims == 2 && syncs == 1 &&
              flushes == 1,
          "removed parent releases without writes");
    setup();
    disks[4].backing_flags = BLOCK_GONE;
    check(vfs_do_umount("/mnt/a/b") == 0 && !syncs && !prepares && !flushes,
          "removed child backing receives no writes");
    setup();
    sync_error = -EIO;
    check(vfs_do_umount("/mnt/a/b") == -EIO && records[3].refs == 1 && !claims &&
              records[4].state == VFS_ACTIVE && !locks,
          "live child sync failure retains ownership");
    setup();
    lookup_error = -ENOENT;
    check(vfs_do_umount("/mnt/a/b") == -ENOENT, "ordinary lookup error is retained");
    setup();
    disks[3].flags = 0;
    check(vfs_do_umount("/mnt/a/b") == -EIO, "present ancestor cannot use recovery lookup");
    setup();
    check(vfs_do_umount("/mnt/a/b/file") == -EIO, "target must match the whole recorded name");
    records[5].parent = &records[3];
    records[5].covered_path = records[4].covered_path;
    check(vfs_do_umount("/mnt/a/b") == -EIO, "ambiguous recorded names are refused");
    setup();
    records[5].parent = &records[4];
    records[5].covered_path = "/mnt/a/b/c";
    records[4].refs = 1;
    check(vfs_do_umount("/mnt/a/b/c") == 0 && !records[4].refs,
          "removed grandparent permits deepest mount to unmount");
    if (failures)
        return 1;
    puts("vfs-unmount: result: 13 checks passed");
    return 0;
}
