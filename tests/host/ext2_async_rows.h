/*
 * ext2_async_rows.h: rows of ext2_hosttest.c, which includes this file after
 * the interruption matrix, for the operations on names of a mount that
 * writes back, which keep their order in the cache (`bcache_order_after`):
 * a create waits for no write, a block's image reaches the device before
 * the block, a new file's fsync takes its name with it, and every sequence
 * of the matrix, synced, is stopped after each of its writes.
 */
#ifndef EXT2_ASYNC_ROWS_H_
#define EXT2_ASYNC_ROWS_H_

/**
 * The interruption matrix on a mount that writes back: each sequence runs
 * and then syncs the device, once to count the writes and then once for
 * each count k on the junk volume, stopped after write k, and the stop is
 * judged by the matrix's judges (`matrix_judge`). Nothing reaches the
 * device before the sync but what an eviction or a removal writes, so the
 * stops fall among the sync's writes, in the order the cache keeps; no
 * stop must keep a name, since a name is owed only once a sync or an
 * fsync ends.
 */
static void async_matrix_rows(void)
{
    int i, k, total = 0;

    for (i = 0; i < (int)(sizeof(sequences) / sizeof(sequences[0])); i++) {
        int n;

        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        use_image(wimg, wbytes);
        stop_after = -1;
        if (mount_flags(0) != 0) {
            check(0, "the write-back matrix's volume mounts");
            return;
        }
        writes_made = 0;
        op_mark = -1;
        check(sequences[i].run() == 0 && OPS->sync(&mnt) == 0,
              "a sequence of the write-back matrix runs and syncs");
        n = writes_made;
        check(unmount_rw() == 0 && e2fsck_clean(wimg),
              "and its volume unmounts clean on a mount that writes back");
        for (k = 1; k <= n; k++) {
            wbytes = build_junk_image();
            use_image(wimg, wbytes);
            stop_after = -1;
            if (mount_flags(0) != 0) {
                printf("  stop %d of '%s': %s\n", k, sequences[i].name, last_warn);
                check(0, "the write-back matrix's volume mounts again");
                return;
            }
            writes_made = 0;
            stop_after = k;
            (void)sequences[i].run();
            (void)OPS->sync(&mnt);
            OPS->put(&mnt, mnt.root);
            OPS->unmount(&mnt);
            mnt.root = 0;
            bcache_device_gone(&dev);
            dev.flags = BLOCK_FAULT_SAFE;
            stop_after = -1;
            use_image(wimg, wbytes);
            matrix_judge(i, k, n, -1, " written back");
            total++;
        }
    }
    printf("ext2_hosttest: %d stops of the write-back matrix walked\n", total);
}

/** Make `name` in the root by the operation `op` of `async_create_row`:
 * 0 a create, 1 a mkdir, 2 a fast symlink, 3 a slow one, 4 a FIFO, 5 a
 * link of /hello.txt. 0, or -errno. */
static int async_make(int op, const char *name)
{
    static char target[200];
    void *n = 0;
    int rc;

    switch (op) {
    case 0:
        n = make_file(mnt.root, name, &rc);
        break;
    case 1:
        return OPS->mkdir(&mnt, mnt.root, name, 0755);
    case 2:
        return OPS->symlink(&mnt, mnt.root, name, "hello.txt");
    case 3:
        memset(target, 't', 199);
        target[199] = 0;
        return OPS->symlink(&mnt, mnt.root, name, target);
    case 4:
        rc = OPS->mknod(&mnt, mnt.root, name, 0010644, 0, &n);
        break;
    case 5:
        n = walk("/hello.txt", &rc);
        if (n) {
            rc = OPS->link(&mnt, mnt.root, name, n);
        }
        break;
    default:
        return -EINVAL;
    }
    if (n) {
        OPS->put(&mnt, n);
    }
    return rc;
}

/**
 * On a mount that writes back, under a cache of 8192 buffers, 64 files
 * made by each of a create, a mkdir, a fast and a slow symlink, a FIFO and
 * a link, in a directory that grows past its first block: none writes the
 * device, where each waited for two writes, its inode's table block and
 * its directory's block, while they were written before the call
 * returned. The disk requests each waited for, reads of blocks the cache
 * lacks among them, and the writes are printed, as `ext2-create: result:`.
 * A sync then writes them all, and the volume is clean.
 */
static void async_create_row(void)
{
    static const char *const ops[] = {"create",       "mkdir", "fast symlink",
                                      "slow symlink", "mknod", "link"};
    char name[40], what[120];
    int op, i, rc = 0;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    stop_after = -1;
    bcache_set_limit(8192);
    if (mount_flags(0) != 0) {
        check(0, "the create row's volume mounts so that it writes back");
        bcache_set_limit(0);
        return;
    }
    for (op = 0; op < (int)(sizeof(ops) / sizeof(ops[0])); op++) {
        uint32_t waits = current_proc->disk_waits;
        int made = writes_made;

        for (i = 0; rc == 0 && i < 64; i++) {
            snprintf(name, sizeof name, "%c%d", 'a' + op, i);
            rc = async_make(op, name);
        }
        printf("ext2-create: result: 64 of %s: %u disk requests waited for, %d writes\n", ops[op],
               (unsigned)(current_proc->disk_waits - waits), writes_made - made);
        snprintf(what, sizeof what, "64 of %s on a mount that writes back write nothing", ops[op]);
        check(rc == 0 && writes_made == made, what);
    }
    check(OPS->sync(&mnt) == 0 && unmount_rw() == 0 && e2fsck_clean(wimg),
          "and a sync writes them, the volume clean");
    bcache_set_limit(0);
}

/**
 * On a mount that writes back, a file made and written, then synced by an
 * fsync of the file alone, keeps its name and bytes when the device stops
 * right after the call, and the stop is clean (`fsync_stop_judge`): the
 * fsync writes the directory block that waits for the file's table block
 * (`bcache_sync_waiting`). A neighbour made after the file in the same
 * directory block and table block goes with it.
 */
static void async_fsync_row(void)
{
    static unsigned char data[3 * 4096];
    void *a, *b, *h = 0;
    int rc;

    if (!fsync_mount(0)) {
        return;
    }
    memset(data, 0x5f, sizeof(data));
    a = make_file(mnt.root, "kept", &rc);
    b = a ? make_file(mnt.root, "beside", &rc) : 0;
    check(a && b && OPS->open(&mnt, a, 2, &h) == 0 &&
              OPS->write(&mnt, h, data, sizeof(data), 0) == (int32_t)sizeof(data),
          "a new file of three blocks and a neighbour, both in the cache");
    if (!a || !b || !h) {
        return;
    }
    check(OPS->fsync(&mnt, h) == 0, "an fsync of the file alone");
    if (fsync_stop_judge(h, a, b)) {
        check(file_is("/kept", data, sizeof(data)), "and the file keeps its name and its bytes");
        a = walk("/beside", &rc);
        check(a != 0, "and its neighbour its name");
        if (a) {
            OPS->put(&mnt, a);
        }
        unmount_image();
    }
}

/**
 * A rename within one directory block, on a mount that writes back, adds
 * the new name, ordered after the raised count, and removes the old one,
 * written at once: the block's bytes with both names are kept in an image
 * (`bcache_change`), and the image reaches the device after the table
 * block and before the block. Two writes of the root's block are made, and
 * a stop after the first leaves both names, naming the inode in use.
 */
static void async_image_row(void)
{
    uint64_t root = 0, table = 0;
    uint32_t slot = 0;
    int pass, first = -1, second = -1, at_table = -1, i, rc, ok;
    void *n;

    for (pass = 0; pass < 2; pass++) {
        wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
        (void)debugfs_numbers("stat /", "(0):", &root, 1);
        use_image(wimg, wbytes);
        stop_after = -1;
        if (mount_flags(0) != 0) {
            check(0, "the image row's volume mounts so that it writes back");
            return;
        }
        n = make_file(mnt.root, "old.txt", &rc);
        if (n) {
            table = node_table_lba(n, &slot);
            OPS->put(&mnt, n);
        }
        check(n && OPS->sync(&mnt) == 0, "a file synced in the root");
        wlog_reset();
        writes_made = 0;
        stop_after = pass == 0 ? -1 : first + 1;
        rc = OPS->rename(&mnt, mnt.root, "old.txt", mnt.root, "new.txt");
        if (pass == 0) {
            for (i = 0; i < wlog_n; i++) {
                if (wlog_lba[i] == root * 8) {
                    if (first < 0) {
                        first = i;
                    } else if (second < 0) {
                        second = i;
                    }
                } else if (wlog_lba[i] == table && first < 0) {
                    at_table = i;
                }
            }
            ok = rc == 0 && !wlog_overflow && at_table >= 0 && first > at_table && second > first;
            if (!ok) {
                printf("  rc %d: table block at write %d, the root's at %d and %d of %d\n", rc,
                       at_table, first, second, wlog_n);
            }
            check(ok, "a rename in one directory block writes the table block, then the "
                      "block with both names, then the block with the new one");
            check(unmount_rw() == 0 && e2fsck_clean(wimg), "and its volume unmounts clean");
            if (!ok) {
                return;
            }
            continue;
        }
        drop_as_crash();
        fflush(image);
        check(debugfs_lists(wimg, "/", "old.txt") && debugfs_lists(wimg, "/", "new.txt") &&
                  debugfs_says(wimg, "testi /new.txt", "is marked in use"),
              "a stop after the first write of the root's block leaves both names, in use");
        check(fsck_judged(wimg) >= 0 &&
                  findings_among(growth_findings,
                                 sizeof(growth_findings) / sizeof(growth_findings[0])),
              "and e2fsck asks only for counts and bitmaps");
    }
}

/** The rows of the operations on names of a mount that writes back. */
static void async_rows(void)
{
    async_create_row();
    async_image_row();
    async_fsync_row();
    async_matrix_rows();
}

#endif
