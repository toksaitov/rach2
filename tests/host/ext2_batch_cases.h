/*
 * ext2_batch_cases.h: rows of ext2_hosttest.c, which includes this file, for
 * the file-write batch of rach2/sys/fs/ext2.c: its limits of 1 MiB, 256 data
 * blocks, 32 metadata images and 16 inodes; the close of a shared
 * description; a failed admission and a device removed; a hand-off of the
 * namespace lock; an abandoned unmount; partial blocks and holes; reused
 * blocks; and writers queued at each stage of a drain. The rows use the
 * device, the mount and the helpers of ext2_hosttest.c.
 */
#ifndef EXT2_BATCH_CASES_H_
#define EXT2_BATCH_CASES_H_

/* fd.c compares a mount's type with the KRFFS root's; no row mounts one. */
const struct vfs_type vfs_krffs_type = {0};

/** The last close of a description: release the handle and put the node. */
void vfs_desc_close(struct vfs_mount *mount, void *node, void *handle)
{
    mount->type->ops->release(mount, handle);
    mount->type->ops->put(mount, node);
}

/**
 * Check the batch's limits on both sides of a drain: 1 MiB of bytes, 256
 * distinct data blocks and 16 inodes. The write that reaches a limit
 * drains the batch, and the next write starts a new one with nothing
 * written.
 */
static void batch_limit_rows(void)
{
    unsigned char *data = malloc(1024 * 1024);
    void *node, *handle = 0, *nodes[17], *handles[17];
    int i, rc, before;
    char name[32];

    check(data != 0, "batch limit workspace exists");
    if (!data) {
        return;
    }
    memset(data, 0x39, 1024 * 1024);
    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the batch limit volume mounts");
    node = make_file(mnt.root, "limits", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the limit handle opens");
    if (!node || !handle) {
        free(data);
        return;
    }
    check(OPS->write(&mnt, handle, data, 1024 * 1024, 0) == 1024 * 1024,
          "one full batch is accepted and drained");
    before = writes_made;
    for (i = 0; i < 255; i++) {
        check(OPS->write(&mnt, handle, data, 4096, 0) == 4096, "a byte-limit piece is accepted");
    }
    check(writes_made == before, "one byte batch minus one block remains pending");
    check(OPS->write(&mnt, handle, data, 4096, 0) == 4096 && writes_made > before,
          "the byte limit drains on its last block");
    before = writes_made;
    check(OPS->write(&mnt, handle, data, 1, 0) == 1 && writes_made == before,
          "a byte beyond the limit starts a new batch");
    check(OPS->fsync(&mnt, handle) == 0, "the new byte batch syncs");
    before = writes_made;
    for (i = 0; i < 255; i++) {
        check(OPS->write(&mnt, handle, data, 1, (uint64_t)i * 4096) == 1,
              "a distinct-block piece is accepted");
    }
    check(writes_made == before, "255 distinct blocks remain pending");
    check(OPS->write(&mnt, handle, data, 1, 255u * 4096) == 1 && writes_made > before,
          "256 distinct blocks drain without reaching the byte limit");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    for (i = 0; i < 17; i++) {
        snprintf(name, sizeof(name), "inode%d", i);
        nodes[i] = make_file(mnt.root, name, &rc);
        handles[i] = 0;
        check(nodes[i] && OPS->open(&mnt, nodes[i], 2, &handles[i]) == 0,
              "an inode-limit handle opens");
    }
    before = writes_made;
    for (i = 0; i < 15; i++) {
        check(handles[i] && OPS->write(&mnt, handles[i], data, 1, 0) == 1,
              "an inode-limit piece is accepted");
    }
    check(writes_made == before, "fifteen pending inodes remain private");
    check(handles[15] && OPS->write(&mnt, handles[15], data, 1, 0) == 1 && writes_made > before,
          "the sixteenth inode drains the batch");
    before = writes_made;
    check(handles[16] && OPS->write(&mnt, handles[16], data, 1, 0) == 1 && writes_made == before,
          "the seventeenth inode starts a new generation");
    for (i = 0; i < 17; i++) {
        if (handles[i]) {
            OPS->release(&mnt, handles[i]);
        }
        if (nodes[i]) {
            OPS->put(&mnt, nodes[i]);
        }
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "limit drains leave a clean volume");
    free(data);
}

/**
 * Share one description with pending bytes among a parent's descriptor
 * table, a child's copy of it, which takes its reference as fork does, and
 * one more reference: no close but the last drains the batch, and the last
 * does, while a second description of the same inode is open and then
 * reads the bytes.
 */
static void batch_description_row(void)
{
    fd_table_t parent, *child;
    struct open_file *ofd;
    void *node, *handle = 0, *second = 0;
    unsigned char data[4096];
    int fd, rc, before;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the description volume mounts");
    node = make_file(mnt.root, "description", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0 && OPS->open(&mnt, node, 2, &second) == 0,
          "independent descriptions open one inode");
    if (!node || !handle || !second) {
        return;
    }
    memset(&parent, 0, sizeof(parent));
    check(fd_table_init(&parent) == 0, "the parent descriptor table allocates");
    fd = fd_alloc(&parent);
    check(fd >= 0, "the pending file gets a descriptor");
    if (fd < 0) {
        return;
    }
    parent.entries[fd].type = FD_TYPE_FILE;
    ofd = parent.entries[fd].ofd;
    ofd->vmnt = &mnt;
    ofd->vnode = node;
    ofd->fhandle = handle;
    child = fd_table_dup(&parent);
    check(child != NULL, "fork copies the description pointer");
    if (!child) {
        return;
    }
    fd_ofd_ref(ofd);
    fd_ofd_ref(ofd);
    memset(data, 0x5B, sizeof(data));
    before = writes_made;
    check(OPS->write(&mnt, handle, data, sizeof(data), 0) == sizeof(data),
          "the shared description accepts pending bytes");
    fd_free(&parent, fd);
    check(writes_made == before, "the first shared close does not drain");
    fd_free(child, fd);
    check(writes_made == before, "the inherited close leaves the duplicate pending");
    fd_ofd_unref(ofd);
    check(writes_made > before, "the last shared close drains with an independent handle open");
    check(OPS->read(&mnt, second, abuf, sizeof(data), 0) == sizeof(data) &&
              memcmp(abuf, data, sizeof(data)) == 0,
          "the independent description sees the completed bytes");
    OPS->release(&mnt, second);
    fd_table_free(&parent);
    fd_table_delete(child);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "description release leaves a clean volume");
}

/**
 * A write past the format's limit fails with `EFBIG` without draining the
 * pending write; a setattr whose drain fails is `EIO` and changes nothing;
 * and once the device is gone, an fsync is `EIO`, and neither it nor the
 * unmount writes anything.
 */
static void batch_loss_row(void)
{
    void *node, *handle = 0;
    struct vfs_stat before, after;
    unsigned char data[4096];
    int rc, writes;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the removal volume mounts");
    node = make_file(mnt.root, "removed", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the removal handle opens");
    if (!node || !handle) {
        return;
    }
    memset(data, 0x7C, sizeof(data));
    check(OPS->write(&mnt, handle, data, sizeof(data), 0) == sizeof(data),
          "the removed device has private pending state");
    check(OPS->getattr(&mnt, node, &before) == 0, "pending attributes are visible");
    writes = writes_made;
    check(OPS->write(&mnt, handle, data, sizeof(data), (uint64_t)1 << 50) == -EFBIG &&
              writes_made == writes,
          "a range refusal does not drain earlier writes");
    stop_after = writes;
    after = before;
    after.mode = 0600;
    check(OPS->setattr(&mnt, node, VFS_SET_MODE, &after) == -EIO,
          "a failed drain prevents the attribute change");
    check(OPS->getattr(&mnt, node, &after) == 0 && after.mode == before.mode,
          "a failed drain leaves attributes unchanged");
    stop_after = -1;
    bcache_device_gone(&dev);
    check(OPS->fsync(&mnt, handle) == -EIO && writes_made == writes,
          "device removal refuses retry of private images");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    OPS->put(&mnt, mnt.root);
    OPS->unmount(&mnt);
    mnt.root = 0;
    check(writes_made == writes, "removed-device teardown writes nothing");
    dev.flags = BLOCK_FAULT_SAFE;
}

/**
 * Write one byte into each of up to 31 indirect leaves allocated before,
 * so that the batch reaches its limit of 32 metadata images and not its
 * data limit: the write that reaches 32 drains the batch, and the next
 * starts a new one with nothing written.
 */
static void batch_metadata_row(void)
{
    void *node, *handle = 0;
    unsigned char byte = 0x49;
    int i, rc, before;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the metadata-limit volume mounts");
    node = make_file(mnt.root, "metadata-limit", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the metadata-limit handle opens");
    if (!node || !handle) {
        return;
    }
    for (i = 0; i < 31; i++) {
        check(OPS->write(&mnt, handle, &byte, 1, (1036ull + (unsigned)i * 1024) * 4096) == 1,
              "a sparse fixture allocates a distinct indirect leaf");
    }
    check(OPS->fsync(&mnt, handle) == 0, "the metadata-limit fixture syncs");
    before = writes_made;
    for (i = 0; i < 29; i++) {
        check(OPS->write(&mnt, handle, &byte, 1, (1036ull + (unsigned)i * 1024) * 4096) == 1,
              "an allocated leaf joins the metadata-limited batch");
    }
    check(writes_made == before, "31 metadata images remain pending");
    check(OPS->write(&mnt, handle, &byte, 1, (1036ull + 29 * 1024) * 4096) == 1 &&
              writes_made > before,
          "32 metadata images drain before another admission");
    before = writes_made;
    check(OPS->write(&mnt, handle, &byte, 1, (1036ull + 30 * 1024) * 4096) == 1 &&
              writes_made == before,
          "the next metadata image starts a new generation");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "metadata limits leave a clean volume");
}

/* The description `batch_queued_writer` writes through. */
static void *handoff_handle;

/**
 * Write a byte at 4096 as another process, called when the namespace lock
 * is released; the write must be admitted.
 */
static void batch_queued_writer(void)
{
    unsigned char byte = 0x63;
    uint32_t pid = current_proc->pid;

    current_proc->pid = pid + 1;
    check(OPS->write(&mnt, handoff_handle, &byte, 1, 4096) == 1,
          "the queued writer is admitted after namespace release");
    current_proc->pid = pid;
}

/**
 * A setattr keeps the mount's lock from the drain of a pending append
 * through the store of the inode, so a writer queued on the lock runs only
 * after it: the file ends with the new mode and the writer's byte.
 */
static void batch_handoff_row(void)
{
    void *node;
    unsigned char data[4096];
    struct vfs_stat st;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the namespace handoff volume mounts");
    node = make_file(mnt.root, "handoff", &rc);
    check(node && OPS->open(&mnt, node, 2, &handoff_handle) == 0, "the handoff handle opens");
    if (!node || !handoff_handle) {
        return;
    }
    memset(data, 0x31, sizeof(data));
    check(OPS->write(&mnt, handoff_handle, data, sizeof(data), 0) == sizeof(data),
          "an append precedes the competing namespace operation");
    vfs_ns_unlock(&mnt);
    namespace_ready = batch_queued_writer;
    memset(&st, 0, sizeof(st));
    st.mode = 0600;
    check(OPS->setattr(&mnt, node, VFS_SET_MODE, &st) == 0,
          "setattr retains the mount lock from drain through store");
    check(!namespace_ready && OPS->getattr(&mnt, node, &st) == 0 && st.size == 4097 &&
              (st.mode & 0777) == 0600,
          "the queued writer follows the completed attribute change");
    vfs_ns_lock(&mnt);
    OPS->release(&mnt, handoff_handle);
    OPS->put(&mnt, node);
    handoff_handle = 0;
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "namespace handoff leaves a clean volume");
}

/**
 * An unmount prepared and then abandoned clears the valid bit on disk
 * again, and the unmount can finish later.
 */
static void batch_abort_row(void)
{
    unsigned char state[2];

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0 && OPS->prepare_unmount(&mnt) == 0,
          "an unmount reaches its prepared state");
    check(OPS->abort_unmount != 0, "ext2 can abandon a prepared unmount");
    if (OPS->abort_unmount) {
        OPS->abort_unmount(&mnt);
    }
    check(fseek(image, 1024 + 58, SEEK_SET_) == 0 && fread(state, 1, 2, image) == 2 &&
              !(state[0] & 1),
          "an abandoned unmount clears the valid bit on disk");
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "an abandoned unmount can finish later");
}

/**
 * Replay the durable states of a partial block over a volatile medium. With
 * `hole` 0, two one-byte appends at 0 and 1 share their block, whose rest
 * must be zeros; with `hole` set, a byte fills a hole at block 12, below a
 * size and an indirect block already on the disk. The free blocks hold
 * stale bytes, and in no state may the size or the hole's pointer be
 * published before the bytes and the zeros.
 */
static void batch_partial_row(int hole)
{
    unsigned char expected[4096];
    unsigned char *before;
    uint64_t pointers[4], bitmaps[8];
    void *node, *handle = 0;
    struct vfs_stat st;
    int rc, nptr, nbm;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(seed_free_blocks(0) > 0, "partial-block free space contains stale bytes");
    check(mount_rw() == 0, "the partial-block volume mounts");
    node = make_file(mnt.root, "partial", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the partial-block handle opens");
    if (!node || !handle) {
        return;
    }
    memset(expected, 0, sizeof(expected));
    expected[hole ? 100 : 0] = 0x41;
    if (hole) {
        check(OPS->write(&mnt, handle, expected + 100, 1, 13ull * 4096) == 1 &&
                  OPS->fsync(&mnt, handle) == 0,
              "the hole fixture has a durable indirect parent and size");
    } else {
        expected[1] = 0x42;
    }
    before = malloc(wbytes);
    check(before && fseek(image, 0, SEEK_SET_) == 0 && fread(before, 1, wbytes, image) == wbytes,
          "the partial-block durable baseline is saved");
    check(OPS->getattr(&mnt, node, &st) == 0, "the partial-block inode is known");
    batch_publication_ino = (uint32_t)st.ino;
    batch_publication_first = hole ? 12 : 0;
    batch_publication_hole = hole;
    batch_publication_blocks = 1;
    batch_publication_data = expected;
    wlog_reset();
    wlog_capture = 1;
    if (hole) {
        check(OPS->write(&mnt, handle, expected + 100, 1, 12ull * 4096 + 100) == 1,
              "a byte fills an existing indirect hole");
    } else {
        check(OPS->write(&mnt, handle, expected, 1, 0) == 1 &&
                  OPS->write(&mnt, handle, expected + 1, 1, 1) == 1,
              "two partial appends share their initialized block");
    }
    check(OPS->fsync(&mnt, handle) == 0, "the partial-block generation syncs");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(unmount_rw() == 0, "the partial-block volume unmounts");
    nptr = debugfs_numbers("stat /partial", "(IND):", pointers, 4);
    nbm = debugfs_numbers("stats", "block bitmap at ", bitmaps, 8);
    check(nbm > 0 && before &&
              media_publication_ok(before, wbytes / 4096, pointers, nptr, bitmaps[0]),
          "partial bytes and zeroing precede durable size or hole publication");
    batch_publication_ino = 0;
    batch_publication_data = 0;
    batch_publication_hole = 0;
    batch_publication_blocks = 32;
    wlog_capture = 0;
    wlog_reset();
    free(before);
    check(e2fsck_clean(wimg), "partial-block writes leave a clean volume");
}

static int fill_volume(void);

/**
 * Fill the volume while a file of sixteen blocks and an indirect block
 * holds space, so that a second file's write fails with `ENOSPC` and keeps
 * the first file's pending write. Truncating the first file drains it and
 * frees its blocks; the second file then takes at least fifteen of them,
 * and after the cache is emptied and synced, they hold the new owner's
 * bytes.
 */
static void batch_reuse_row(void)
{
    unsigned char old[16 * 4096], replacement[16 * 4096];
    uint64_t previous[20], next[20];
    void *victim, *newnode, *handle = 0, *newhandle = 0;
    uint32_t freed;
    int rc, oldn, newn, i, j, before, shared = 0;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the reuse volume mounts");
    victim = make_file(mnt.root, "victim", &rc);
    newnode = make_file(mnt.root, "replacement", &rc);
    check(victim && newnode && OPS->open(&mnt, victim, 2, &handle) == 0 &&
              OPS->open(&mnt, newnode, 2, &newhandle) == 0,
          "the reuse handles open");
    if (!victim || !newnode || !handle || !newhandle) {
        return;
    }
    memset(old, 0x17, sizeof(old));
    memset(replacement, 0xE5, sizeof(replacement));
    check(OPS->write(&mnt, handle, old, sizeof(old), 0) == sizeof(old) &&
              OPS->fsync(&mnt, handle) == 0,
          "the victim owns data and an indirect block");
    oldn = debugfs_list("blocks /victim", "", previous, 20);
    check(oldn > 16 && fill_volume() == 0, "the volume fills with the victim still allocated");
    check(OPS->write(&mnt, handle, replacement, sizeof(replacement), 0) == sizeof(replacement),
          "the victim has pending writes before truncation");
    before = writes_made;
    check(OPS->write(&mnt, newhandle, old, sizeof(old), 0) == -ENOSPC && writes_made == before,
          "allocation refusal preserves the earlier pending generation");
    check(OPS->truncate(&mnt, handle, 123) == 0 && OPS->truncate(&mnt, handle, 0) == 0,
          "truncate drains pending bytes and tail zeroing before freeing");
    check(write_at(newnode, old, sizeof(old), 0) == 0, "the new owner reuses freed space");
    newn = debugfs_list("blocks /replacement", "", next, 20);
    for (i = 0; i < oldn; i++) {
        for (j = 0; j < newn; j++) {
            if (previous[i] == next[j]) {
                shared++;
            }
        }
    }
    check(shared >= 15, "the replacement actually reuses the freed blocks");
    bcache_shrink(bcache_resident(), 0, &freed);
    check(OPS->fsync(&mnt, handle) == 0 && file_is("/replacement", old, sizeof(old)),
          "eviction and sync leave reused blocks with their new owner's bytes");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, victim);
    OPS->release(&mnt, newhandle);
    OPS->put(&mnt, newnode);
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "block reuse leaves a clean volume");
}

/* The stage rows' queued writer's description and the node whose mode the
   queued setattr changes; the flush at which to queue them, the count of
   flushes seen, and the count of times the queued work ran. */
static void *stage_writer, *stage_attribute;
static int stage_target, stage_seen, stage_ran;

/**
 * Run the queued work as another process, called when the draining owner
 * releases the namespace lock: a write and a setattr, both of which must
 * succeed.
 */
static void batch_stage_waiters(void)
{
    unsigned char byte = 0x51;
    struct vfs_stat st;
    uint32_t pid = current_proc->pid;

    current_proc->pid = pid + 1;
    check(OPS->write(&mnt, stage_writer, &byte, 1, 0) == 1,
          "the queued writer enters after the frozen generation finishes");
    vfs_ns_lock(&mnt);
    memset(&st, 0, sizeof(st));
    st.mode = 0600;
    check(OPS->setattr(&mnt, stage_attribute, VFS_SET_MODE, &st) == 0,
          "the queued attribute change follows the drain");
    vfs_ns_unlock(&mnt);
    stage_ran++;
    current_proc->pid = pid;
}

/**
 * At the flush numbered `stage_target`, check that the draining process
 * still holds the namespace lock, queue `batch_stage_waiters` for its
 * release, and check that the queued work has not changed the file.
 */
static void batch_stage_pause(void)
{
    struct vfs_stat st;

    if (++stage_seen != stage_target) {
        return;
    }
    flush_pause = 0;
    check(mnt.ns_depth > 0 && mnt.ns_holder == (int)current_proc->pid,
          "a draining generation retains mount ownership across I/O");
    if (mnt.ns_depth == 0 || mnt.ns_holder != (int)current_proc->pid) {
        return;
    }
    namespace_ready = batch_stage_waiters;
    check(OPS->getattr(&mnt, stage_writer, &st) == 0 && st.size == 0 && stage_ran == 0,
          "a queued writer cannot change the frozen generation");
}

/**
 * For each of the first five flushes of a drain that crosses an indirect
 * level, queue a writer and a setattr at that flush: both must run once,
 * after the drain finishes, and the volume must end clean.
 */
static void batch_stage_rows(void)
{
    unsigned char bytes[8192];
    void *node, *writer, *handle = 0;
    int barrier, rc;

    memset(bytes, 0x24, sizeof(bytes));
    for (barrier = 1; barrier <= 5; barrier++) {
        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        check(mount_rw() == 0, "the stage scheduling volume mounts");
        node = make_file(mnt.root, "draining", &rc);
        writer = make_file(mnt.root, "queued", &rc);
        stage_attribute = make_file(mnt.root, "attribute", &rc);
        check(node && writer && stage_attribute && OPS->open(&mnt, node, 2, &handle) == 0 &&
                  OPS->open(&mnt, writer, 2, &stage_writer) == 0,
              "the stage scheduling handles open");
        if (!node || !writer || !stage_attribute || !handle || !stage_writer) {
            return;
        }
        check(OPS->truncate(&mnt, handle, 1035ull * 4096) == 0 &&
                  OPS->write(&mnt, handle, bytes, 4096, 1035ull * 4096) == 4096 &&
                  OPS->write(&mnt, handle, bytes + 4096, 4096, 1036ull * 4096) == 4096,
              "the frozen generation crosses an indirect level");
        vfs_ns_unlock(&mnt);
        stage_seen = stage_ran = 0;
        stage_target = barrier;
        flush_pause = batch_stage_pause;
        check(OPS->fsync(&mnt, handle) == 0, "a paused drain finishes");
        flush_pause = 0;
        check(stage_ran == 1 && !namespace_ready,
              "both waiters run after the selected stage completes");
        namespace_ready = 0;
        vfs_ns_lock(&mnt);
        OPS->release(&mnt, handle);
        OPS->put(&mnt, node);
        OPS->release(&mnt, stage_writer);
        OPS->put(&mnt, writer);
        OPS->put(&mnt, stage_attribute);
        check(unmount_rw() == 0 && e2fsck_clean(wimg), "stage scheduling leaves a clean volume");
    }
}

#endif
