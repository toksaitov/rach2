/*
 * ext2_batch_lifecycle.h: rows of ext2_hosttest.c, which includes this file
 * after ext2_batch_cases.h, for the life of the file-write batch of
 * rach2/sys/fs/ext2.c: inodes that share a table block, a last close that
 * fails, an unlinked inode released in either order, and a drain stopped
 * at each of its writes. The rows read the image file directly, through
 * the open image of ext2_hosttest.c, to see what is on the disk apart from
 * the driver's pending state.
 */
#ifndef EXT2_BATCH_LIFECYCLE_H_
#define EXT2_BATCH_LIFECYCLE_H_

/**
 * The byte offset in the image of inode `ino`, found through the
 * superblock and the group descriptors on the disk, or 0. The volume has
 * 4 KiB blocks, 128-byte inodes and its descriptors at block 1.
 */
static uint64_t lifecycle_inode_at(uint64_t ino)
{
    unsigned char word[4];
    uint32_t per_group, table, index;

    if (fseek(image, 1024 + 40, SEEK_SET_) != 0 || fread(word, 1, 4, image) != 4) {
        return 0;
    }
    per_group = batch_word(word);
    if (!ino || !per_group) {
        return 0;
    }
    index = (uint32_t)(ino - 1);
    if (fseek(image, 4096 + (index / per_group) * 32 + 8, SEEK_SET_) != 0 ||
        fread(word, 1, 4, image) != 4) {
        return 0;
    }
    table = batch_word(word);
    return (uint64_t)table * 4096 + (index % per_group) * 128;
}

/** Read inode `ino` from the disk into `raw`, apart from pending state. */
static int lifecycle_inode(uint64_t ino, unsigned char *raw)
{
    uint64_t at = lifecycle_inode_at(ino);

    return at && fseek(image, (long)at, SEEK_SET_) == 0 && fread(raw, 1, 128, image) == 128;
}

/**
 * Two inodes in one table block have pending writes and set-id modes; a
 * setattr of a third inode, which clears its set-id bits and sets its
 * atime, must drain the whole batch first. Both sizes then reach the disk,
 * each inode after its data, both files keep their bytes, and the third
 * inode has its new mode and atime on the disk.
 */
static void lifecycle_setattr_row(void)
{
    void *nodes[3], *handles[3] = {0, 0, 0};
    struct vfs_stat st[3];
    unsigned char data[4096], raw[128];
    uint64_t at[3];
    char name[24];
    int i, a = 0, b = 1, third = 2, rc, before;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the shared-table lifecycle volume mounts");
    for (i = 0; i < 3; i++) {
        snprintf(name, sizeof(name), "attributes%d", i);
        nodes[i] = make_file(mnt.root, name, &rc);
        check(nodes[i] && OPS->open(&mnt, nodes[i], 2, &handles[i]) == 0,
              "a shared-table description opens");
        if (!nodes[i] || !handles[i]) {
            return;
        }
        check(OPS->getattr(&mnt, nodes[i], &st[i]) == 0, "the shared-table inode is known");
        at[i] = lifecycle_inode_at(st[i].ino);
        st[i].mode = 06755;
        check(OPS->setattr(&mnt, nodes[i], VFS_SET_MODE, &st[i]) == 0,
              "setid mode precedes pending writes");
    }
    if (at[0] / 4096 != at[1] / 4096) {
        a = 1;
        b = 2;
        third = 0;
    }
    check(at[a] && at[a] / 4096 == at[b] / 4096, "the pending inodes share one table block");
    memset(data, 0x57, sizeof(data));
    before = writes_made;
    wlog_reset();
    check(OPS->write(&mnt, handles[a], data, sizeof(data), 0) == sizeof(data) &&
              OPS->write(&mnt, handles[b], data, sizeof(data), 0) == sizeof(data) &&
              writes_made == before,
          "both shared-table writes remain pending");
    st[third].mode = 0755;
    st[third].atime_sec = 123456;
    check(OPS->setattr(&mnt, nodes[third], VFS_SET_MODE | VFS_SET_ATIME, &st[third]) == 0 &&
              writes_made > before,
          "third-inode mode clearing and atime drain the whole generation");
    for (i = 0; i < 3; i++) {
        check(lifecycle_inode(st[i].ino, raw), "the lifecycle inode reached the disk");
        if (i != third) {
            check(batch_word(raw + 4) == sizeof(data), "both pending sizes reach the disk");
            check(first_write_of(batch_word(raw + 40)) >= 0 &&
                      first_write_of(batch_word(raw + 40)) < first_write_of(at[i] / 4096),
                  "shared-table publication follows its data write");
            check(OPS->read(&mnt, handles[i], abuf, sizeof(data), 0) == sizeof(data) &&
                      memcmp(abuf, data, sizeof(data)) == 0,
                  "shared-table drain preserves both files");
        } else {
            check((batch_word(raw) & 07777) == 0755 && batch_word(raw + 8) == 123456,
                  "the third inode persists cleared setid bits and atime");
        }
        OPS->release(&mnt, handles[i]);
        OPS->put(&mnt, nodes[i]);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "shared-table lifecycle leaves a clean volume");
}

/**
 * A last close of a description whose drain fails records `EIO` on the
 * mount and writes nothing. After a reopen, an fsync retries the bytes and
 * reports the error, a second fsync reports it again with nothing to
 * write, and every byte the write took reads back.
 */
static void lifecycle_failed_close_row(void)
{
    fd_table_t table;
    struct open_file *ofd;
    void *node, *handle = 0;
    unsigned char data[4096];
    int fd, rc, before;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the failed-close volume mounts");
    node = make_file(mnt.root, "failed-close", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the failed-close description opens");
    if (!node || !handle) {
        return;
    }
    memset(&table, 0, sizeof(table));
    check(fd_table_init(&table) == 0, "the failed-close descriptor table allocates");
    fd = fd_alloc(&table);
    check(fd >= 0, "the failed-close descriptor allocates");
    if (fd < 0) {
        return;
    }
    table.entries[fd].type = FD_TYPE_FILE;
    ofd = table.entries[fd].ofd;
    ofd->vmnt = &mnt;
    ofd->vnode = node;
    ofd->fhandle = handle;
    memset(data, 0x81, sizeof(data));
    check(OPS->write(&mnt, handle, data, sizeof(data), 0) == sizeof(data),
          "the closing description accepts pending bytes");
    before = writes_made;
    stop_after = before;
    fd_free(&table, fd);
    check(mnt.write_error == -EIO && writes_made == before,
          "failed final-description release records its error");
    fd_table_free(&table);
    stop_after = -1;
    node = walk("/failed-close", &rc);
    handle = 0;
    check(node && OPS->open(&mnt, node, 0, &handle) == 0, "the failed-close file reopens");
    if (!node || !handle) {
        return;
    }
    check(OPS->fsync(&mnt, handle) == -EIO && writes_made > before,
          "reopened fsync retries bytes and reports the close error");
    before = writes_made;
    check(OPS->fsync(&mnt, handle) == -EIO && writes_made == before,
          "a second reopened fsync keeps the close error after completion");
    check(OPS->read(&mnt, handle, abuf, sizeof(data), 0) == sizeof(data) &&
              memcmp(abuf, data, sizeof(data)) == 0,
          "failed close preserves every accepted byte");
    OPS->release(&mnt, handle);
    OPS->put(&mnt, node);
    check(unmount_rw() == 0, "failed-close retry permits unmount");
    bcache_device_gone(&dev);
    dev.flags = BLOCK_FAULT_SAFE;
}

/**
 * An unlinked inode with pending appends is released by its description
 * and by its other reference, in the order `put_first` selects, and the
 * one left after the first release still works. The last release frees
 * every data block, and a new file that takes the inode has none of its
 * old size or bytes, even after the cache is emptied.
 */
static void lifecycle_unlinked_row(int put_first)
{
    void *node, *handle = 0;
    struct vfs_stat old, fresh;
    struct vfs_statfs before, after;
    unsigned char data[4096];
    uint32_t freed;
    int rc;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(mount_rw() == 0, "the unlinked lifecycle volume mounts");
    node = make_file(mnt.root, "unlinked-pending", &rc);
    check(node && OPS->open(&mnt, node, 2, &handle) == 0, "the unlinked description opens");
    if (!node || !handle) {
        return;
    }
    check(OPS->getattr(&mnt, node, &old) == 0 && OPS->statfs(&mnt, &before) == 0,
          "the unlinked inode and free count are known");
    memset(data, 0x29, sizeof(data));
    check(OPS->write(&mnt, handle, data, sizeof(data), 0) == sizeof(data) &&
              OPS->unlink(&mnt, mnt.root, "unlinked-pending") == 0 &&
              OPS->write(&mnt, handle, data, sizeof(data), 4096) == sizeof(data),
          "an unlinked inode accepts another pending append");
    if (put_first) {
        OPS->put(&mnt, node);
        check(OPS->read(&mnt, handle, abuf, sizeof(data), 4096) == sizeof(data) &&
                  memcmp(abuf, data, sizeof(data)) == 0,
              "the description survives the non-description put");
        OPS->release(&mnt, handle);
    } else {
        OPS->release(&mnt, handle);
        check(OPS->getattr(&mnt, node, &fresh) == 0 && fresh.size == 8192,
              "the non-description reference survives final release");
        OPS->put(&mnt, node);
    }
    check(OPS->statfs(&mnt, &after) == 0 && after.bfree == before.bfree,
          "the last unlinked reference frees every data block");
    node = make_file(mnt.root, "reused-inode", &rc);
    check(node && OPS->getattr(&mnt, node, &fresh) == 0 && fresh.ino == old.ino && fresh.size == 0,
          "the freed inode is reused without its old size");
    if (node) {
        memset(data, 0xC4, sizeof(data));
        check(write_at(node, data, sizeof(data), 0) == 0, "the reused inode accepts new bytes");
        bcache_shrink(bcache_resident(), 0, &freed);
        check(file_is("/reused-inode", data, sizeof(data)),
              "cache eviction cannot restore the unlinked inode's bytes");
        OPS->put(&mnt, node);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), "unlinked references leave no allocated orphan");
}

/**
 * Make 33 files and open the first and the last, whose inodes lie in two
 * table blocks of 32 inodes each, truncated to `first` blocks. Their nodes
 * and descriptions go to `nodes` and `handles`. Returns 1, or 0 when a
 * step fails.
 */
static int lifecycle_pair(void **nodes, void **handles, uint32_t first)
{
    struct vfs_stat st[2];
    void *node;
    char name[24];
    int i, rc;

    for (i = 0; i < 33; i++) {
        snprintf(name, sizeof(name), "interrupt%d", i);
        node = make_file(mnt.root, name, &rc);
        if (!node) {
            return 0;
        }
        if (i == 0 || i == 32) {
            int n = i != 0;

            nodes[n] = node;
            if (OPS->open(&mnt, node, 2, &handles[n]) != 0 ||
                OPS->truncate(&mnt, handles[n], (uint64_t)first * 4096) != 0 ||
                OPS->getattr(&mnt, node, &st[n]) != 0) {
                return 0;
            }
        } else {
            OPS->put(&mnt, node);
        }
    }
    check(lifecycle_inode_at(st[0].ino) / 4096 != lifecycle_inode_at(st[1].ino) / 4096,
          "the interrupted generation uses two inode-table blocks");
    return 1;
}

/**
 * Replay the captured drain over a volatile medium from `before`, once for
 * each of the two inodes of `st`, whose two blocks from `first` hold
 * `data`: no state may publish a size or a pointer before what it covers.
 */
static void lifecycle_replay(const unsigned char *before, const struct vfs_stat *st, uint32_t first,
                             const unsigned char *data)
{
    uint64_t pointers[8], bitmaps[8];
    int n, nbm, i;

    n = debugfs_numbers("stat /interrupt0", "(IND):", pointers, 8);
    n += debugfs_numbers("stat /interrupt0", "(DIND):", pointers + n, 8 - n);
    n += debugfs_numbers("stat /interrupt32", "(IND):", pointers + n, 8 - n);
    n += debugfs_numbers("stat /interrupt32", "(DIND):", pointers + n, 8 - n);
    nbm = debugfs_numbers("stats", "block bitmap at ", bitmaps, 8);
    check(n >= 2 && nbm > 0 && !wlog_overflow, "the lifecycle trace records both pointer trees");
    batch_publication_first = first;
    batch_publication_blocks = 2;
    batch_publication_data = data;
    for (i = 0; i < 2; i++) {
        batch_publication_ino = (uint32_t)st[i].ino;
        check(before && nbm > 0 &&
                  media_publication_ok(before, wbytes / 4096, pointers, n, bitmaps[0]),
              "both inode tables preserve volatile publication dependencies");
    }
    batch_publication_ino = 0;
    batch_publication_data = 0;
    batch_publication_blocks = 32;
}

/**
 * Append two blocks from block `first` to each of the two files of
 * `lifecycle_pair` and fsync. With `stop` of 0 or more, the write numbered
 * `stop` of the drain fails: the fsync is `EIO`, a retry keeps the error,
 * every byte reads back, and the volume ends consistent. With `stop` -1,
 * the drain is captured and replayed by `lifecycle_replay`. Returns the
 * count of writes the fsync made.
 */
static int lifecycle_interrupt_one(uint32_t first, int stop)
{
    void *nodes[2] = {0, 0}, *handles[2] = {0, 0};
    unsigned char data[8192], *snapshot = 0;
    struct vfs_stat st[2];
    int i, rc, before, count;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    check(seed_free_blocks(0) > 0, "interruption free space contains stale bytes");
    check(mount_rw() == 0 && lifecycle_pair(nodes, handles, first),
          "the interrupted generation's two descriptions open");
    if (!handles[0] || !handles[1]) {
        return 0;
    }
    for (i = 0; i < 2; i++) {
        check(OPS->getattr(&mnt, nodes[i], &st[i]) == 0, "the interrupted inode is known");
    }
    if (stop < 0) {
        snapshot = malloc(wbytes);
        check(snapshot && fseek(image, 0, SEEK_SET_) == 0 &&
                  fread(snapshot, 1, wbytes, image) == wbytes,
              "the lifecycle trace saves its initial medium");
        wlog_reset();
        wlog_capture = 1;
    }
    memset(data, 0x37, 4096);
    memset(data + 4096, 0xA6, 4096);
    before = writes_made;
    for (i = 0; i < 2; i++) {
        check(OPS->write(&mnt, handles[i], data, 4096, (uint64_t)first * 4096) == 4096 &&
                  OPS->write(&mnt, handles[i], data + 4096, 4096, ((uint64_t)first + 1) * 4096) ==
                      4096,
              "four calls join the interrupted generation");
    }
    check(writes_made == before, "the interruption generation remains private before sync");
    if (stop >= 0) {
        stop_after = before + stop;
    }
    rc = OPS->fsync(&mnt, handles[0]);
    count = writes_made - before;
    check(stop < 0 ? rc == 0 : rc == -EIO && count == stop,
          "the selected generation write stops fsync");
    stop_after = -1;
    if (stop >= 0) {
        check(OPS->fsync(&mnt, handles[0]) == -EIO,
              "the interrupted generation retries with its delayed error retained");
    }
    for (i = 0; i < 2; i++) {
        check(OPS->read(&mnt, handles[i], abuf, sizeof(data), (uint64_t)first * 4096) ==
                      sizeof(data) &&
                  memcmp(abuf, data, sizeof(data)) == 0,
              "every interrupted inode retains every accepted byte");
        OPS->release(&mnt, handles[i]);
        OPS->put(&mnt, nodes[i]);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg),
          "interrupted retry leaves consistent allocation");
    if (stop < 0) {
        lifecycle_replay(snapshot, st, first, data);
        wlog_capture = 0;
        wlog_reset();
        free(snapshot);
    }
    if (stop >= 0) {
        bcache_device_gone(&dev);
        dev.flags = BLOCK_FAULT_SAFE;
    }
    return count;
}

/**
 * Run the lifecycle rows, and for appends from blocks 11 and 1035, run the
 * drain once whole and then once stopped at each of its writes.
 */
static void batch_lifecycle_rows(void)
{
    static const uint32_t first[] = {11, 1035};
    int i, stop, count;

    lifecycle_setattr_row();
    lifecycle_failed_close_row();
    lifecycle_unlinked_row(0);
    lifecycle_unlinked_row(1);
    for (i = 0; i < 2; i++) {
        count = lifecycle_interrupt_one(first[i], -1);
        check(count > 0, "the successful generation supplies interruption stops");
        for (stop = 0; stop < count; stop++) {
            lifecycle_interrupt_one(first[i], stop);
        }
        printf("ext2-lifecycle: result: boundary %u, %d write interruptions checked\n", first[i],
               count);
    }
}

#endif
