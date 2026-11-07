/*
 * ext2_service_rows.h: rows of ext2_hosttest.c, which includes this file after
 * ext2_pair_rows.h, for the service points of a free of a file's blocks
 * (`punch`), where the big lock is let go and another process runs: a real
 * competing operation at each one, a sync and e2fsck of the device at each
 * one, and a device that fails at one.
 */
#ifndef EXT2_SERVICE_ROWS_H_
#define EXT2_SERVICE_ROWS_H_

/* The file the rows free: `SVC_KEPT` blocks at its start, one block in each
 * of `SVC_SPANS` single-indirect trees, so that the walk reads a whole empty
 * pointer block for each, and dense blocks past them, which fill rounds. A
 * file of no spans has its dense blocks straight after the kept ones, where
 * the first walk's list fills before it has read `PUNCH_VISITS` pointers. */
#define SVC_KEPT   20u
#define SVC_FIRST  1036u /* the first data index under the double indirect */
#define SVC_SPANS  64u
#define SVC_TAIL   (SVC_FIRST + SVC_SPANS * 1024u)
#define SVC_DENSE  1400u
#define SVC_NEAR   900u
#define SVC_VICTIM 1u /* the stamp of the file freed */

/* The competing files: each of `SVC_BLOCKS` blocks, every second one removed
 * again, and the stamp of file number i is SVC_BASE + i. */
#define SVC_BLOCKS 40u
#define SVC_FILES  256
#define SVC_BASE   100u

static unsigned char svc_bytes[4096], svc_back[4096];
static int svc_points;  /* service points passed in the row */
static int svc_made;    /* competing files made */
static int svc_stop;    /* the row's free has ended */
static int svc_bad;     /* competing operations that failed */
static int svc_found;   /* the snapshot row's findings that are not allowed */
static int svc_fail_at; /* the service point at which the device fails */
static int svc_writes;  /* the writes made when it did */

/** Fill `svc_bytes` with the words of block `index` of file `stamp`. */
static void svc_stamp(uint32_t stamp, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < 1024; i++) {
        uint32_t word = stamp * 2654435761u + index * 40503u + i;

        svc_bytes[i * 4] = (unsigned char)word;
        svc_bytes[i * 4 + 1] = (unsigned char)(word >> 8);
        svc_bytes[i * 4 + 2] = (unsigned char)(word >> 16);
        svc_bytes[i * 4 + 3] = (unsigned char)(word >> 24);
    }
}

/** Write block `index` of file `stamp` through `handle`; 1 when it went. */
static int svc_write(void *handle, uint32_t stamp, uint32_t index)
{
    svc_stamp(stamp, index);
    return OPS->write(&mnt, handle, svc_bytes, 4096, (uint64_t)index * 4096) == 4096;
}

/** Whether block `index` of `node` holds the words of file `stamp`. */
static int svc_holds(void *node, uint32_t stamp, uint32_t index)
{
    svc_stamp(stamp, index);
    return read_at(node, svc_back, 4096, (uint64_t)index * 4096) == 4096 &&
           memcmp(svc_back, svc_bytes, 4096) == 0;
}

/**
 * Write the file the rows free through `handle`: its kept blocks, a block
 * under each of the first `spans` pointer blocks, and `dense` blocks after
 * them. Whether every write went.
 */
static int svc_fill(void *handle, uint32_t spans, uint32_t dense)
{
    uint32_t i;
    int ok = 1;

    for (i = 0; i < SVC_KEPT; i++) {
        ok = ok && svc_write(handle, SVC_VICTIM, i);
    }
    for (i = 0; i < spans; i++) {
        ok = ok && svc_write(handle, SVC_VICTIM, SVC_FIRST + i * 1024 + 7);
    }
    for (i = 0; i < dense; i++) {
        ok = ok && svc_write(handle, SVC_VICTIM, (spans ? SVC_TAIL : SVC_KEPT) + i);
    }
    return ok;
}

/** Give the name of competing file `i`, in `name`. */
static void svc_name(char *name, unsigned long size, int i)
{
    snprintf(name, size, "c%d", i);
}

/**
 * One competing operation, as process B: a new file in the root directory,
 * written whole, so that it takes blocks of the groups the free gives up;
 * every second time the file before it removed, which frees them in those
 * groups beside the row's free; every fourth time a sync of the mount.
 */
static void svc_compete(void)
{
    int i = svc_made++, rc = 0;
    void *node, *handle = 0;
    char name[16];
    uint32_t b;

    svc_name(name, sizeof(name), i);
    node = make_file(mnt.root, name, &rc);
    if (!node || OPS->open(&mnt, node, 1, &handle) != 0) {
        svc_bad++;
        return;
    }
    for (b = 0; b < SVC_BLOCKS; b++) {
        svc_bad += !svc_write(handle, SVC_BASE + (uint32_t)i, b);
    }
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    if (i % 2 == 1) {
        svc_name(name, sizeof(name), i - 1);
        svc_bad += OPS->unlink(&mnt, mnt.root, name) != 0;
    }
    if (i % 4 == 3) {
        svc_bad += OPS->sync(&mnt) != 0;
    }
}

/** Process B of a competing row: a competing operation at each turn it is
 * given, until the free has ended or the row's files are all made. */
static void svc_second(void)
{
    while (!svc_stop && svc_made < SVC_FILES - 1) {
        svc_compete();
        pass_turn();
    }
}

/** At a service point, give the other process the turn. */
static void svc_turn(void)
{
    svc_points++;
    pass_turn();
}

/** Whether every competing file not removed reads whole, the odd ones and
 * the last, and every removed one is gone. */
static int svc_competitors_ok(void)
{
    int i, err;
    int ok = 1;

    for (i = 0; i < svc_made; i++) {
        char name[16], path[20];
        void *node;
        uint32_t b;

        svc_name(name, sizeof(name), i);
        snprintf(path, sizeof(path), "/%s", name);
        node = walk(path, &err);
        if (i % 2 == 0 && i + 1 < svc_made) {
            ok = ok && !node;
            if (node) {
                OPS->put(&mnt, node);
            }
            continue;
        }
        if (!node) {
            printf("  %s is gone\n", path);
            ok = 0;
            continue;
        }
        for (b = 0; b < SVC_BLOCKS; b++) {
            if (!svc_holds(node, SVC_BASE + (uint32_t)i, b)) {
                printf("  %s: block %u differs\n", path, (unsigned)b);
                ok = 0;
                break;
            }
        }
        OPS->put(&mnt, node);
    }
    return ok;
}

/** Whether the file left by a cut from `SVC_KEPT` blocks on is that long,
 * holds its kept blocks, and has nothing past them. */
static int svc_cut_ok(void *node)
{
    struct vfs_stat st;
    uint32_t i;

    if (OPS->getattr(&mnt, node, &st) != 0 || st.size != (uint64_t)SVC_KEPT * 4096) {
        return 0;
    }
    for (i = 0; i < SVC_KEPT; i++) {
        if (!svc_holds(node, SVC_VICTIM, i)) {
            return 0;
        }
    }
    return read_at(node, svc_back, 4096, (uint64_t)SVC_FIRST * 4096) == 0;
}

/** Free the row's file in directory `dir`: cut it to its kept blocks through
 * `handle`, or with `unlink`, close it and remove its name, which frees it
 * at the last put. 0, or the free's -errno. */
static int svc_free(void *dir, void *node, void *handle, int unlink_it)
{
    int rc;

    if (unlink_it) {
        OPS->release(&mnt, handle);
        OPS->put(&mnt, node);
        return OPS->unlink(&mnt, dir, "victim");
    }
    OPS->lock_node(&mnt, node);
    rc = OPS->truncate(&mnt, handle, (uint64_t)SVC_KEPT * 4096);
    OPS->unlock_node(&mnt, node);
    return rc;
}

/**
 * Mount a new volume with `flags`, and make the row's file of `spans` and
 * `dense` blocks in a directory of its own, on the device. Its directory,
 * node and open description go in `*dir`, `*node` and `*handle`. 1 when all
 * of that went.
 */
static int svc_volume(uint32_t flags, uint32_t spans, uint32_t dense, void **dir, void **node,
                      void **handle)
{
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 64, "tree");
    use_image(wimg, wbytes);
    rw_flags = flags;
    rc = mount_rw();
    rw_flags = VFS_MS_DIRSYNC;
    check(rc == 0, "the service rows' volume mounts");
    if (rc != 0) {
        return 0;
    }
    svc_points = svc_made = svc_stop = svc_bad = svc_found = svc_fail_at = 0;
    *handle = 0;
    rc = OPS->mkdir(&mnt, mnt.root, "v", 0755);
    *dir = rc == 0 ? walk("/v", &rc) : 0;
    *node = *dir ? make_file(*dir, "victim", &rc) : 0;
    check(*node && OPS->open(&mnt, *node, 2, handle) == 0 && svc_fill(*handle, spans, dense) &&
              OPS->sync(&mnt) == 0 && bcache_sync(&dev) == 0,
          "the file to free is on the device");
    return *handle != 0;
}

/**
 * Free a file of 64 sparse pointer blocks and 1400 dense blocks on a mount
 * with `flags`, by a cut or by the unlink of its last name, while process B
 * runs a competing operation at each service point: files made, written,
 * removed and synced on the same mount and the same group, and at B's own
 * frees the turn comes back to A inside them. Every file left reads whole
 * and the cut keeps its bytes, before and after a remount, and e2fsck finds
 * the volume clean.
 */
static void svc_compete_row(uint32_t flags, int unlink_it)
{
    void *dir, *node, *handle;
    int rc;

    if (!svc_volume(flags, SVC_SPANS, SVC_DENSE, &dir, &node, &handle)) {
        return;
    }
    service_pause = svc_turn;
    start_turns(svc_second);
    rc = svc_free(dir, node, handle, unlink_it);
    svc_stop = 1;
    finish_turns();
    service_pause = 0;
    check(rc == 0, "the free ends beside the competing operations");
    check(svc_bad == 0, "and every competing operation succeeds");
    check(svc_points >= (int)SVC_SPANS && svc_made >= (int)SVC_SPANS / 2,
          "they ran at the service points of the walk and of the rounds");
    check(svc_competitors_ok(), "the files they left read whole");
    if (!unlink_it) {
        check(svc_cut_ok(node), "the cut file keeps its bytes and has nothing past them");
        OPS->release(&mnt, handle);
        OPS->put(&mnt, node);
    }
    OPS->put(&mnt, dir);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "e2fsck -fn finds the volume clean");
    check(mount_rw() == 0, "the volume mounts again");
    check(svc_competitors_ok(), "and the files read whole after the remount");
    if (!unlink_it) {
        node = walk("/v/victim", &rc);
        check(node && svc_cut_ok(node), "and so does the cut file");
        if (node) {
            OPS->put(&mnt, node);
        }
    }
    check(unmount_rw() == 0, "the volume unmounts again");
    printf("ext2-service: flags %u %s: %d service points, %d competing files\n", (unsigned)flags,
           unlink_it ? "unlink" : "cut", svc_points, svc_made);
}

/** Whether fsck.out lists a block as in use whose bit is clear, which is what
 * a pointer on the device to a freed block would show as. */
static int svc_block_lost_bit(void)
{
    void *f = fopen(fix_path("fsck.out"), "r");
    char line[512];
    int found = 0;

    if (!f) {
        return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "Block bitmap differences") && strstr(line, "+")) {
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/** Process B of the snapshot row: at each turn, write everything dirty and
 * judge the device as a power cut there would leave it. */
static void svc_snapshot(void)
{
    while (!svc_stop) {
        int status;

        svc_bad += bcache_sync(&dev) != 0;
        fflush(image);
        status = e2fsck_status(wimg);
        if (status > 4 || svc_block_lost_bit() || !findings_allowed(0)) {
            printf("  a service point's device: e2fsck says %d\n", status);
            svc_found++;
        }
        svc_made++;
        pass_turn();
    }
}

/**
 * At every service point of a free, a sync puts all that is dirty on the
 * device, and e2fsck -fn reads it as a power cut there would leave it: only
 * repairs the order allows, and no block named by a pointer whose bit is
 * clear. The free ends, and the volume is clean.
 */
static void svc_snapshot_row(uint32_t flags, int unlink_it)
{
    void *dir, *node, *handle;
    int rc;

    if (!svc_volume(flags, SVC_SPANS / 8u, SVC_DENSE, &dir, &node, &handle)) {
        return;
    }
    service_pause = svc_turn;
    start_turns(svc_snapshot);
    rc = svc_free(dir, node, handle, unlink_it);
    svc_stop = 1;
    finish_turns();
    service_pause = 0;
    check(rc == 0 && svc_bad == 0, "a free ends with a sync at each service point");
    check(svc_made >= 4, "they ran");
    check(svc_found == 0, "and the device passes e2fsck at each of them");
    if (!unlink_it) {
        OPS->release(&mnt, handle);
        OPS->put(&mnt, node);
    }
    OPS->put(&mnt, dir);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "e2fsck -fn finds the volume clean after it");
    printf("ext2-service: flags %u %s: %d devices judged\n", (unsigned)flags,
           unlink_it ? "unlink" : "cut", svc_made);
}

/** At service point `svc_fail_at`, mark the device failed, as a write of
 * another process does, and let no later write reach it. */
static void svc_fail(void)
{
    if (++svc_points == svc_fail_at) {
        svc_writes = writes_made;
        stop_after = writes_made;
        dev.flags |= BLOCK_WRITE_FAILED;
    }
}

/**
 * The device fails at the first service point of a cut on a mount with
 * `flags`. With `spans` pointer blocks and no dense blocks that point is
 * inside the walk of the only round: the round frees nothing. With dense
 * blocks and no spans it is the end of the first round, of PUNCH_BLOCKS: no
 * other round walks. The cut answers -EIO, the mount's free count is what
 * the rounds before gave, no write reaches the device, and the device is as
 * a power cut leaves it: e2fsck names no block that a pointer holds and a
 * bit does not.
 */
static void svc_fail_row(uint32_t flags, uint32_t spans, uint32_t dense, uint64_t freed)
{
    struct vfs_statfs before, after;
    struct vfs_stat was, now;
    void *dir, *node, *handle;
    int rc, points;

    if (!svc_volume(flags, spans, dense, &dir, &node, &handle)) {
        return;
    }
    check(OPS->statfs(&mnt, &before) == 0 && OPS->getattr(&mnt, node, &was) == 0,
          "the free count before the cut is read");
    svc_fail_at = 1;
    service_pause = svc_fail;
    rc = svc_free(dir, node, handle, 0);
    points = svc_points;
    service_pause = 0;
    check(rc == -EIO, "a device that fails at a service point stops the free with -EIO");
    check(points >= 1, "and the service point came");
    check(OPS->statfs(&mnt, &after) == 0 && after.bfree == before.bfree + freed,
          spans ? "the round frees nothing" : "and no round after it frees anything");
    check(writes_made == svc_writes, "nothing is written after the failure");
    check(spans || (OPS->getattr(&mnt, node, &now) == 0 && was.blocks - now.blocks == freed * 8),
          "and no later walk clears a pointer");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    OPS->put(&mnt, dir);
    (void)unmount_rw();
    bcache_device_gone(&dev);
    dev.flags = BLOCK_FAULT_SAFE;
    stop_after = -1;
    fflush(image);
    check(e2fsck_status(wimg) <= 4 && !svc_block_lost_bit() && findings_allowed(0),
          "the device holds no pointer to a free block");
}

/** The rows of the service points of a free, on a mount that writes back and
 * on one that writes through. */
static void service_rows(void)
{
    static const uint32_t mounts[] = {0, VFS_MS_DIRSYNC};
    unsigned m;

    for (m = 0; m < sizeof(mounts) / sizeof(mounts[0]); m++) {
        svc_compete_row(mounts[m], 0);
        svc_compete_row(mounts[m], 1);
        svc_snapshot_row(mounts[m], 0);
        svc_snapshot_row(mounts[m], 1);
        svc_fail_row(mounts[m], 8, 0, 0);
        svc_fail_row(mounts[m], 0, SVC_NEAR, 512);
    }
}

#endif
