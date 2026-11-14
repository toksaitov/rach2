/*
 * ext2_service_rows.h: rows of ext2_hosttest.c, which includes this file after
 * ext2_pair_rows.h, for the service points of a free of a file's blocks
 * (`ext2_punch`) and of a write of a file and its drains, where the big lock is
 * let go and another process runs: a real competing operation at each one, a
 * sync and e2fsck of the device at each one, and a device that fails at one.
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

/* The blocks of the one long write of the rows of a write's service points,
 * enough for several drains, and the write points process A then passes:
 * one each 16 pieces, none after the last. */
#define SVC_LONG         1024u
#define SVC_WRITE_POINTS (SVC_LONG / 16u - 1u)

/* A service point's site, told by the locks its process holds: a write's
 * holds the node's lock alone, and a drain's `em->batch` too. */
enum { SVC_SITE_WRITE, SVC_SITE_DRAIN, SVC_SITES };

/* What process B does to the file of a long write, at its first turn. */
enum { SVC_SAME_WRITE, SVC_SAME_READ, SVC_SAME_CUT, SVC_SAME_FSYNC, SVC_SAME_UNMOUNT };

static unsigned char svc_bytes[4096], svc_back[4096];
static unsigned char svc_long[SVC_LONG * 4096], svc_read[SVC_LONG * 4096];
static int svc_points;           /* service points passed in the row */
static int svc_made;             /* competing files begun */
static int svc_done;             /* competing files ended */
static int svc_stop;             /* the row's free has ended */
static int svc_bad;              /* competing operations that failed */
static int svc_found;            /* the snapshot row's findings that are not allowed */
static int svc_fail_at;          /* the service point at which the device fails */
static int svc_writes;           /* the writes made when it did */
static int svc_tries;            /* the writes asked of the device when it did */
static int svc_sites[SVC_SITES]; /* process A's service points, by site */
static int svc_want;             /* the site at which the device fails */
static int svc_failed;           /* it has failed */
static int svc_same;             /* what process B does, SVC_SAME_* */
static int svc_same_ok;          /* it went as it should */
static void *svc_node;           /* the file of the long write */
static void *svc_handle;         /* and the description of its writer */

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
    svc_done++;
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

/** The site of the service point the caller stands at (`SVC_SITE_WRITE`). */
static int svc_site(void)
{
    return current_proc->file_locks >= 2 ? SVC_SITE_DRAIN : SVC_SITE_WRITE;
}

/** At a service point, count process A's by its site and give the other
 * process the turn. */
static void svc_turn(void)
{
    svc_points++;
    if (current_proc != &proc_b) {
        svc_sites[svc_site()]++;
    }
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
    svc_points = svc_made = svc_done = svc_stop = svc_bad = svc_found = svc_fail_at = 0;
    svc_sites[SVC_SITE_WRITE] = svc_sites[SVC_SITE_DRAIN] = 0;
    svc_failed = svc_same_ok = 0;
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
    check(svc_points >= (int)SVC_SPANS && svc_made >= 4 && svc_done == svc_made,
          "they ran at the service points of the walk and of the rounds, and each ended");
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
        svc_tries = write_attempts;
        stop_after = writes_made;
        dev.flags |= BLOCK_WRITE_FAILED;
    }
}

/**
 * The device fails at the first service point of a cut on a mount with
 * `flags`. With `spans` pointer blocks and no dense blocks that point is
 * inside the walk of the only round: the round frees nothing. With dense
 * blocks and no spans it is the end of the first round, of PUNCH_BLOCKS: no
 * other round walks. The cut stops with -EROFS, when its next write meets
 * the funnel of the failed mount (`ext2_write_owned`), or with -EIO, when
 * it meets the device's failure first; the mount's free count is what the
 * rounds before gave, no write reaches the device, and the device is as
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
    check(rc == -EIO || rc == -EROFS,
          "a device that fails at a service point stops the free with -EIO or -EROFS");
    check(points >= 1, "and the service point came");
    check(OPS->statfs(&mnt, &after) == 0 && after.bfree == before.bfree + freed,
          spans ? "the round frees nothing" : "and no round after it frees anything");
    check(writes_made == svc_writes && write_attempts == svc_tries,
          "nothing is written or tried after the failure");
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

/** Stamp `svc_long` with the blocks a long write puts after the kept ones. */
static void svc_long_stamp(void)
{
    uint32_t i, j;

    for (i = 0; i < SVC_LONG; i++) {
        svc_stamp(SVC_VICTIM, SVC_KEPT + i);
        for (j = 0; j < 4096; j++) {
            svc_long[i * 4096 + j] = svc_bytes[j];
        }
    }
}

/** Write the long piece through `handle`, one call of `SVC_LONG` blocks after
 * the kept ones, which are the file's whole length: what the call returned. */
static int32_t svc_long_write(void *handle)
{
    svc_long_stamp();
    return OPS->write(&mnt, handle, svc_long, SVC_LONG * 4096, (uint64_t)SVC_KEPT * 4096);
}

/** Whether `node` is `SVC_KEPT` blocks and the first `blocks` of the long
 * piece, each holding its words, with nothing past them. */
static int svc_prefix_ok(void *node, uint32_t blocks)
{
    struct vfs_stat st;
    uint32_t i;

    if (OPS->getattr(&mnt, node, &st) != 0 || st.size != (uint64_t)(SVC_KEPT + blocks) * 4096) {
        printf("  the file is not %u blocks long\n", (unsigned)(SVC_KEPT + blocks));
        return 0;
    }
    for (i = 0; i < SVC_KEPT + blocks; i++) {
        if (!svc_holds(node, SVC_VICTIM, i)) {
            printf("  block %u of the long write differs\n", (unsigned)i);
            return 0;
        }
    }
    return 1;
}

/** Whether `node` is `SVC_KEPT` blocks and the first `blocks` of the long
 * piece long, and each block reads its words or says -EIO, as one the cache
 * cannot give does once a drain has failed: never other bytes. */
static int svc_prefix_safe(void *node, uint32_t blocks)
{
    struct vfs_stat st;
    uint32_t i;

    if (OPS->getattr(&mnt, node, &st) != 0 || st.size != (uint64_t)(SVC_KEPT + blocks) * 4096) {
        return 0;
    }
    for (i = 0; i < SVC_KEPT + blocks; i++) {
        int got = read_at(node, svc_back, 4096, (uint64_t)i * 4096);

        svc_stamp(SVC_VICTIM, i);
        if (got != -5 && (got != 4096 || memcmp(svc_back, svc_bytes, 4096) != 0)) {
            printf("  block %u of the long write reads %d\n", (unsigned)i, got);
            return 0;
        }
    }
    return 1;
}

/** Whether `node` holds the whole long write. */
static int svc_long_ok(void *node)
{
    return svc_prefix_ok(node, SVC_LONG);
}

/** Put the row's references away, and judge the volume clean after an
 * unmount. */
static void svc_write_end(void *dir, void *node, void *handle)
{
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    OPS->put(&mnt, dir);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "e2fsck -fn finds the volume clean");
}

/** At process A's service points of site `svc_want` give process B the turn,
 * and at process B's own give it back; count A's at both sites. */
static void svc_turn_site(void)
{
    svc_points++;
    if (current_proc == &proc_b) {
        pass_turn();
        return;
    }
    svc_sites[svc_site()]++;
    if (svc_site() == svc_want) {
        pass_turn();
    }
}

/**
 * One write of `SVC_LONG` blocks on a mount with `flags`, while process B
 * runs a competing operation at each of its service points of `site`: files
 * made, written, removed and synced on the same mount. At the write's own
 * points B's drains take A's batch, and at B's the turn comes back to A,
 * whose next piece waits for it. At a drain's B waits for `em->batch`, which
 * A keeps. The sites count apart, so that one cannot stand for the other.
 * The write goes whole, every file reads whole before and after a remount,
 * and e2fsck finds the volume clean.
 */
static void svc_write_compete_row(uint32_t flags, int site)
{
    void *dir, *node, *handle;
    int32_t n;
    int rc;

    if (!svc_volume(flags, 0, 0, &dir, &node, &handle)) {
        return;
    }
    svc_want = site;
    service_pause = svc_turn_site;
    start_turns(svc_second);
    n = svc_long_write(handle);
    svc_stop = 1;
    finish_turns();
    service_pause = 0;
    check(n == (int32_t)(SVC_LONG * 4096), "the long write goes whole beside the others");
    check(svc_bad == 0 && svc_done == svc_made && svc_made >= (site == SVC_SITE_WRITE ? 4 : 1),
          "and every competing operation ends and succeeds");
    check(svc_sites[SVC_SITE_WRITE] == (int)SVC_WRITE_POINTS,
          "the write offers the lock each 16 pieces");
    check(site == SVC_SITE_WRITE ||
              (svc_sites[SVC_SITE_DRAIN] >= 15 && second_wait && streq(second_wait, "ext2 batch")),
          "and its drains, where the others wait for em->batch");
    check(svc_competitors_ok(), "the files they left read whole");
    check(svc_long_ok(node), "and so does the long write");
    svc_write_end(dir, node, handle);
    check(mount_rw() == 0, "the volume mounts again");
    check(svc_competitors_ok(), "and the files read whole after the remount");
    node = walk("/v/victim", &rc);
    check(node && svc_long_ok(node), "and so does the long write");
    if (node) {
        OPS->put(&mnt, node);
    }
    check(unmount_rw() == 0, "the volume unmounts again");
    printf("ext2-service: flags %u write: %d and %d points, %d competing files\n", (unsigned)flags,
           svc_sites[SVC_SITE_WRITE], svc_sites[SVC_SITE_DRAIN], svc_made);
}

/**
 * At every service point of a long write and of its drains a sync puts all
 * that is dirty on the device, and e2fsck -fn reads it as a power cut there
 * would leave it: only repairs the order allows, and no block named by a
 * pointer whose bit is clear.
 */
static void svc_write_snapshot_row(uint32_t flags)
{
    void *dir, *node, *handle;
    int32_t n;

    if (!svc_volume(flags, 0, 0, &dir, &node, &handle)) {
        return;
    }
    service_pause = svc_turn;
    start_turns(svc_snapshot);
    n = svc_long_write(handle);
    svc_stop = 1;
    finish_turns();
    service_pause = 0;
    check(n == (int32_t)(SVC_LONG * 4096) && svc_bad == 0,
          "a long write ends with a sync at each service point");
    check(svc_sites[SVC_SITE_WRITE] == (int)SVC_WRITE_POINTS &&
              svc_sites[SVC_SITE_DRAIN] >= (int)(SVC_LONG / 256u - 1u) * 15,
          "both sites were judged");
    check(svc_found == 0, "and the device passes e2fsck at each of them");
    check(svc_long_ok(node), "the long write reads whole");
    svc_write_end(dir, node, handle);
    printf("ext2-service: flags %u write: %d devices judged\n", (unsigned)flags, svc_made);
}

/** At a service point of site `svc_want`, the first, mark the device failed,
 * as a write of another process does, and let no later write reach it. */
static void svc_fail_site(void)
{
    svc_sites[svc_site()]++;
    if (!svc_failed && svc_site() == svc_want) {
        svc_failed = 1;
        svc_writes = writes_made;
        svc_tries = write_attempts;
        stop_after = writes_made;
        dev.flags |= BLOCK_WRITE_FAILED;
    }
}

/**
 * The device fails at the first service point of site `site` of a long write
 * on a mount with `flags`, after `blocks` pieces: the 16th for the write's,
 * the 256th, whose room check drains the batch, for the drain's. The write
 * says it took exactly those pieces, and no write reaches the device after
 * the failure. Between pieces none is even tried, and the file reads those
 * pieces whole. In a drain the cache must still try plain data to give a
 * buffer, and a block it then cannot give reads -EIO, not other bytes. The
 * sync that follows answers an error, and the device is as a power cut
 * leaves it.
 */
static void svc_write_fail_row(uint32_t flags, int site, uint32_t blocks)
{
    void *dir, *node, *handle;
    int32_t n;

    if (!svc_volume(flags, 0, 0, &dir, &node, &handle)) {
        return;
    }
    svc_want = site;
    service_pause = svc_fail_site;
    n = svc_long_write(handle);
    service_pause = 0;
    printf("ext2-service: flags %u site %d: took %d, %d writes tried after the failure\n",
           (unsigned)flags, site, (int)n, write_attempts - svc_tries);
    check(svc_failed, "the service point of the site came");
    check(n == (int32_t)(blocks * 4096), "the write says it took the pieces before it");
    check(writes_made == svc_writes, "nothing reaches the device after the failure");
    check(site != SVC_SITE_WRITE || write_attempts == svc_tries,
          "and between pieces nothing is even tried");
    check(site == SVC_SITE_WRITE ? svc_prefix_ok(node, blocks) : svc_prefix_safe(node, blocks),
          "the file is those pieces, with their bytes");
    check(OPS->sync(&mnt) != 0, "the sync after it says so");
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

/** Process B of a row on one file: one operation, that the node's lock holds
 * off until A's write ends, or that does not need it. */
static void svc_same_body(void)
{
    void *h = 0;

    switch (svc_same) {
    case SVC_SAME_WRITE:
        svc_same_ok =
            OPS->open(&mnt, svc_node, 2, &h) == 0 && svc_write(h, SVC_VICTIM, SVC_KEPT + SVC_LONG);
        OPS->release(&mnt, h);
        break;
    case SVC_SAME_READ:
        svc_same_ok = svc_holds(svc_node, SVC_VICTIM, SVC_KEPT);
        break;
    case SVC_SAME_CUT:
        OPS->lock_node(&mnt, svc_node);
        svc_same_ok = OPS->truncate(&mnt, svc_handle, (uint64_t)SVC_KEPT * 4096) == 0;
        OPS->unlock_node(&mnt, svc_node);
        break;
    case SVC_SAME_FSYNC:
        svc_same_ok = OPS->fsync(&mnt, svc_handle) == 0;
        break;
    default:
        svc_same_ok = OPS->prepare_unmount(&mnt) == 0;
        OPS->abort_unmount(&mnt);
        break;
    }
}

/**
 * At the first service point of a long write process B works on the same
 * file or the whole mount (`SVC_SAME_*`). A write of the file or a cut waits
 * for the node's lock and lands after the write, the read sees the pieces
 * taken so far whole, and an fsync or an unmount that is given up leaves the
 * write to go on. What is left is what those say, and the volume is clean.
 */
static void svc_same_row(uint32_t flags, int what)
{
    void *dir, *node, *handle;
    int32_t n;

    if (!svc_volume(flags, 0, 0, &dir, &node, &handle)) {
        return;
    }
    svc_same = what;
    svc_node = node;
    svc_handle = handle;
    service_pause = svc_turn;
    start_turns(svc_same_body);
    n = svc_long_write(handle);
    finish_turns();
    service_pause = 0;
    check(n == (int32_t)(SVC_LONG * 4096), "the long write goes whole beside its file's user");
    check(svc_sites[SVC_SITE_WRITE] == (int)SVC_WRITE_POINTS, "at every write point");
    check(svc_same_ok, "and the other operation succeeds");
    if (what == SVC_SAME_WRITE || what == SVC_SAME_CUT) {
        check(second_wait && streq(second_wait, "ext2 node"), "after a wait for the node's lock");
    }
    if (what == SVC_SAME_CUT) {
        check(svc_cut_ok(node), "the cut comes after the write");
    } else if (what == SVC_SAME_WRITE) {
        check(svc_prefix_ok(node, SVC_LONG + 1), "the other write lands after it");
    } else {
        check(svc_long_ok(node), "and the file is whole");
    }
    svc_write_end(dir, node, handle);
}

/** Process B of the row of a full volume: a file of 1 MiB writes until no
 * more fit, which takes the blocks the write's check of the space counted. */
static void svc_fill_body(void)
{
    uint64_t at = 0;
    void *node, *h = 0;
    int rc = 0;

    node = make_file(mnt.root, "fill", &rc);
    svc_same_ok = node && OPS->open(&mnt, node, 1, &h) == 0;
    if (!svc_same_ok) {
        return;
    }
    while (OPS->write(&mnt, h, svc_long, 1024u * 1024u, at) > 0) {
        at += 1024u * 1024u;
    }
    OPS->release(&mnt, h);
    OPS->put(&mnt, node);
}

/** At process A's service points, give process B the turn. */
static void svc_once(void)
{
    if (current_proc == &proc_b) {
        return;
    }
    svc_sites[svc_site()]++;
    pass_turn();
}

/**
 * Process B takes every free block at the first write point of a long write,
 * which `ext2_writeback_space` had passed whole. The write stops short, with
 * what it took and those bytes in the file, and nothing past them, and the
 * volume is clean. The write sees what B changed at that point, so this row has
 * no pass without the point.
 */
static void svc_enospc_row(uint32_t flags)
{
    void *dir, *node, *handle;
    int32_t n;

    if (!svc_volume(flags, 0, 0, &dir, &node, &handle)) {
        return;
    }
    service_pause = svc_once;
    start_turns(svc_fill_body);
    n = svc_long_write(handle);
    finish_turns();
    service_pause = 0;
    check(svc_same_ok && svc_sites[SVC_SITE_WRITE] >= 1, "the volume filled at a write point");
    check(n > 0 && n < (int32_t)(SVC_LONG * 4096) && n % 4096 == 0,
          "the write stops short at a whole piece");
    check(n > 0 && svc_prefix_ok(node, (uint32_t)n / 4096), "and the file is what it says it took");
    svc_write_end(dir, node, handle);
    printf("ext2-service: flags %u a full volume: the write took %d blocks\n", (unsigned)flags,
           (int)(n / 4096));
}

/** At the first write point, take away the page of the user's buffer the
 * next pieces read, as another thread's unmap does. */
static void svc_unmap(void)
{
    svc_sites[svc_site()]++;
    if (!svc_failed && svc_site() == SVC_SITE_WRITE) {
        svc_failed = 1;
        gone_from = (uintptr_t)svc_long + 64 * 4096;
        gone_to = gone_from + 4096;
    }
}

/**
 * The page a long write reads next goes at its first write point. The next
 * pieces that reach it fault it in and go on from where they were: the write
 * goes whole, with the right bytes.
 */
static void svc_unmap_row(uint32_t flags)
{
    void *dir, *node, *handle;
    int32_t n;

    if (!svc_volume(flags, 0, 0, &dir, &node, &handle)) {
        return;
    }
    prefaults = 0;
    service_pause = svc_unmap;
    n = svc_long_write(handle);
    service_pause = 0;
    gone_from = gone_to = 0;
    check(svc_failed && prefaults >= 1, "the page went at a write point and came back");
    check(n == (int32_t)(SVC_LONG * 4096), "the write goes whole");
    check(svc_long_ok(node), "with the right bytes");
    svc_write_end(dir, node, handle);
}

/**
 * A long write that does not begin on a block, beside process B's competing
 * operations: with `gap` 0 it continues a file whose last block is part
 * full, and joins the batch; otherwise it begins `gap` bytes past the end
 * and drains after each piece. Its bytes and what is before them read back,
 * and the volume is clean.
 */
static void svc_odd_row(uint32_t flags, uint32_t gap)
{
    uint64_t start = (uint64_t)SVC_KEPT * 4096;
    uint64_t at = start + (gap ? gap : 100);
    void *dir, *node, *handle;
    int32_t n;
    uint32_t i;
    int same = 1;

    if (!svc_volume(flags, 0, 0, &dir, &node, &handle)) {
        return;
    }
    svc_long_stamp();
    if (!gap) {
        check(OPS->write(&mnt, handle, svc_long, 100, start) == 100, "the short piece goes");
    }
    service_pause = svc_turn;
    start_turns(svc_second);
    n = OPS->write(&mnt, handle, svc_long, SVC_LONG * 4096, at);
    svc_stop = 1;
    finish_turns();
    service_pause = 0;
    check(n == (int32_t)(SVC_LONG * 4096), "a write that begins inside a block goes whole");
    check(svc_bad == 0 && svc_done == svc_made, "beside competing operations that end");
    check(svc_sites[SVC_SITE_WRITE] == 64, "and it offers the lock each 16 pieces");
    check(read_at(node, svc_read, SVC_LONG * 4096, at) == (int)(SVC_LONG * 4096) &&
              memcmp(svc_read, svc_long, SVC_LONG * 4096) == 0,
          "its bytes read back");
    check(read_at(node, svc_back, 100, start) == 100, "and those before it are there");
    for (i = 0; i < 100; i++) {
        same = same && svc_back[i] == (gap ? 0 : svc_long[i]);
    }
    check(same, "as the short piece or as the gap's zeros");
    svc_write_end(dir, node, handle);
}

/** The rows of the service points of a write, and of its drains, on a mount
 * with `flags`. */
static void svc_write_rows(uint32_t flags)
{
    int what;

    svc_write_compete_row(flags, SVC_SITE_WRITE);
    svc_write_compete_row(flags, SVC_SITE_DRAIN);
    svc_write_snapshot_row(flags);
    svc_write_fail_row(flags, SVC_SITE_WRITE, 16);
    svc_write_fail_row(flags, SVC_SITE_DRAIN, 256);
    for (what = SVC_SAME_WRITE; what <= SVC_SAME_UNMOUNT; what++) {
        svc_same_row(flags, what);
    }
    svc_enospc_row(flags);
    svc_unmap_row(flags);
    svc_odd_row(flags, 0);
    svc_odd_row(flags, 100);
}

/** The rows of the service points of a free and of a write, on a mount that
 * writes back and on one that writes through. */
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
        svc_write_rows(mounts[m]);
    }
}

#endif
