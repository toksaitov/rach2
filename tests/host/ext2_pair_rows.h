/*
 * ext2_pair_rows.h: the nested interleavings of rach2/sys/fs/ext2.c, rows
 * of ext2_hosttest.c, which includes this file after ext2_lock_rows.h.
 *
 * Process A makes one operation; at its Nth disk read or write, for each N
 * the operation reaches, process B makes another, and runs until it ends or
 * sleeps. A row names what B must do there: end, or sleep on the lock it
 * names (`wait_kind`). Every pair of thirteen operations runs on two
 * layouts. On different files, in different directories, B ends at every
 * request; on one file and one directory, B sleeps on a node lock exactly
 * when A holds, across its requests, a node lock that B needs, as the
 * table `pair_ops` says, and ends otherwise. A rename between directories
 * beside another such rename sleeps on the mount's rename lock. On any
 * layout B may instead sleep on the buffer of a block that A's request is
 * moving, which the cache holds for that one request (bcache.h), and on no
 * other buffer.
 *
 * The volume lives in memory (`ram_image`) and starts each run from one
 * copy, with the cache empty; the mount writes back, as a default mount
 * does.
 */
#ifndef EXT2_PAIR_ROWS_H_
#define EXT2_PAIR_ROWS_H_

extern int mkdir(const char *path, unsigned mode);

/* The nodes a pair's lock sets name: one side's directory, its file, the
   file an unlink removes, the directory an rmdir removes, the file a rename
   moves, and the unlinked file whose last put each process makes. */
#define PL_DIR    0x01
#define PL_FILE   0x02
#define PL_VICTIM 0x04
#define PL_SUB    0x08
#define PL_SOURCE 0x10
#define PL_DOOMED 0x20 /* process A's; process B's is another */

/* The blocks of each side's file, past the twelve direct pointers. */
#define PAIR_FILE_BLOCKS 14

/* One process's objects in a run. In the layout of one file, process B's
   are process A's, but for the names it makes and the file it puts last. */
struct pair_side {
    void *dir;
    void *file;
    void *handle;
    void *doomed; /* held, with no link; NULL once put */
    void *to;     /* the directory a rename between directories moves to */
    const char *made, *linked, *moved;
};

/* An operation: its name, the nodes it holds across every disk request it
   makes (what process A holds), the nodes it locks (what process B needs),
   and whether a run gives its file dirty blocks before it starts. */
struct pair_op {
    const char *name;
    int (*run)(struct pair_side *s);
    int holds;
    int needs;
    int dirty_first;
};

static unsigned char pair_bytes_buf[PAIR_FILE_BLOCKS * 4096];

/** Read the whole file. */
static int pair_read(struct pair_side *s)
{
    int32_t got = OPS->read(&mnt, s->handle, pair_bytes_buf, sizeof(pair_bytes_buf), 0);

    return got == (int32_t)sizeof(pair_bytes_buf) ? 0 : (got < 0 ? got : -1);
}

/** Write two pieces, one under the direct pointers and one past them,
 * under the file's lock, as `fs_write` holds it. */
static int pair_write(struct pair_side *s)
{
    int rc = 0;

    OPS->lock_node(&mnt, s->file);
    if (OPS->write(&mnt, s->handle, "pair", 4, 5000) != 4 ||
        OPS->write(&mnt, s->handle, "pair", 4, 13 * 4096 + 50) != 4) {
        rc = -1;
    }
    OPS->unlock_node(&mnt, s->file);
    return rc;
}

/** Write 3000 bytes at the size, read under the file's lock as an
 * `O_APPEND` write reads it. */
static int pair_append(struct pair_side *s)
{
    struct vfs_stat st;
    int rc = -1;

    OPS->lock_node(&mnt, s->file);
    if (OPS->getattr(&mnt, s->file, &st) == 0 &&
        OPS->write(&mnt, s->handle, pair_bytes_buf, 3000, st.size) == 3000) {
        rc = 0;
    }
    OPS->unlock_node(&mnt, s->file);
    return rc;
}

/** Truncate the file to five blocks and a piece, under its lock. */
static int pair_truncate(struct pair_side *s)
{
    int rc;

    OPS->lock_node(&mnt, s->file);
    rc = OPS->truncate(&mnt, s->handle, 5 * 4096 + 100);
    OPS->unlock_node(&mnt, s->file);
    return rc;
}

/** Make a file of the side's new name in its directory. */
static int pair_create(struct pair_side *s)
{
    void *n = 0;
    int rc = OPS->create(&mnt, s->dir, s->made, 0644, &n);

    if (rc == 0) {
        OPS->put(&mnt, n);
    }
    return rc;
}

/** Remove the name "u" of the side's directory. */
static int pair_unlink(struct pair_side *s)
{
    return OPS->unlink(&mnt, s->dir, "u");
}

/** Remove the empty directory "s" of the side's directory. */
static int pair_rmdir(struct pair_side *s)
{
    return OPS->rmdir(&mnt, s->dir, "s");
}

/** Rename "r" in the side's directory to its new name there. */
static int pair_rename(struct pair_side *s)
{
    return OPS->rename(&mnt, s->dir, "r", s->dir, s->moved);
}

/** Link the side's file under its new name in its directory. */
static int pair_link(struct pair_side *s)
{
    return OPS->link(&mnt, s->dir, s->linked, s->file);
}

/** Change the file's mode, under its lock. */
static int pair_setattr(struct pair_side *s)
{
    struct vfs_stat st;
    int rc;

    memset(&st, 0, sizeof(st));
    st.mode = 0600;
    OPS->lock_node(&mnt, s->file);
    rc = OPS->setattr(&mnt, s->file, VFS_SET_MODE, &st);
    OPS->unlock_node(&mnt, s->file);
    return rc;
}

/** fsync the file. */
static int pair_fsync(struct pair_side *s)
{
    return OPS->fsync(&mnt, s->handle);
}

/** Sync the mount. */
static int pair_sync(struct pair_side *s)
{
    (void)s;
    return OPS->sync(&mnt);
}

/** Put the last hold of the side's unlinked file, which frees it. */
static int pair_put(struct pair_side *s)
{
    void *n = s->doomed;

    s->doomed = 0;
    if (!n) {
        return -1;
    }
    OPS->put(&mnt, n);
    return 0;
}

/** Move "r" of the side's directory to the side's other directory. */
static int pair_move(struct pair_side *s)
{
    return OPS->rename(&mnt, s->dir, "r", s->to, "r");
}

/*
 * The operations and their lock sets, from the lock table at the top of
 * ext2.c: a read and a sync let every lock go around the disk, a change of
 * a file holds the file's, and a change of a name holds the directory's and
 * then the node it names, which an unlink, an rmdir and a rename load
 * before they lock it; a last put holds the dying node's.
 */
static const struct pair_op pair_ops[] = {
    {"read", pair_read, 0, PL_FILE, 0},
    {"write", pair_write, PL_FILE, PL_FILE, 0},
    {"append", pair_append, PL_FILE, PL_FILE, 0},
    {"truncate", pair_truncate, PL_FILE, PL_FILE, 0},
    {"create", pair_create, PL_DIR, PL_DIR, 0},
    {"unlink", pair_unlink, PL_DIR, PL_DIR | PL_VICTIM, 0},
    {"rmdir", pair_rmdir, PL_DIR, PL_DIR | PL_SUB, 0},
    {"rename", pair_rename, PL_DIR, PL_DIR | PL_SOURCE, 0},
    {"link", pair_link, PL_DIR | PL_FILE, PL_DIR | PL_FILE, 0},
    {"setattr", pair_setattr, PL_FILE, PL_FILE, 0},
    {"fsync", pair_fsync, 0, 0, 1},
    {"sync", pair_sync, 0, 0, 1},
    {"last put", pair_put, PL_DOOMED, 0, 0},
};

#define PAIR_OPS ((int)(sizeof(pair_ops) / sizeof(pair_ops[0])))

/* The volume every run starts from, and its size. */
static unsigned char *pair_pristine;
static uint64_t pair_bytes;

/* The run under way: its two sides, process B's operation, the request of
   process A at which B runs and the count A has made, and what B did:
   whether it had ended when A went on, the kind of its first wait, whether
   that was the buffer of a block A's request moved, and its result. */
static struct pair_side pair_a, pair_b;
static const struct pair_op *pair_second_op;
static int pair_target, pair_seen, pair_fired, pair_ended, pair_in_flight, pair_b_rc;
static uint64_t pair_block; /* the first block of A's request */
static const char *pair_slept;

/** Process B: its operation on its side. */
static void pair_second(void)
{
    pair_b_rc = pair_second_op->run(&pair_b);
}

/** At process A's disk request numbered `pair_target`, give process B the
 * turn, and note whether it ended or what it slept on, and whether that was
 * a buffer of this request. */
static void pair_request(void)
{
    uint64_t first = request_block, count = request_blocks;

    if (current_proc != &proc_a || second_inside || pair_fired || ++pair_seen != pair_target) {
        return;
    }
    pair_fired = 1;
    pair_block = first;
    pass_turn();
    pair_ended = !turns_active();
    pair_slept = second_wait;
    pair_in_flight = second_wait && streq(second_wait, "buffer") && second_wait_block >= first &&
                     second_wait_block < first + count;
}

/** Make a file of `blocks` blocks of the pattern at `path`'s name under
 * `dir`; 0, or -errno. */
static int pair_file(void *dir, const char *name, int blocks)
{
    void *n;
    int rc;

    n = make_file(dir, name, &rc);
    if (!n) {
        return rc;
    }
    rc = blocks ? write_at(n, pattern, (uint32_t)blocks * 4096, 0) : 0;
    OPS->put(&mnt, n);
    return rc;
}

/** Make the objects of the side whose directory is `name`: its file, the
 * files "u", "r", "p" and "q", and the empty directory "s"; 0 or -errno. */
static int pair_side_make(const char *name)
{
    void *d;
    int err, rc;

    rc = OPS->mkdir(&mnt, mnt.root, name, 0755);
    d = rc == 0 ? walk(name, &err) : 0;
    if (!d) {
        return rc ? rc : err;
    }
    rc = pair_file(d, "x", PAIR_FILE_BLOCKS);
    if (rc == 0) {
        rc = pair_file(d, "u", 1);
    }
    if (rc == 0) {
        rc = pair_file(d, "r", 1);
    }
    if (rc == 0) {
        rc = pair_file(d, "p", 3);
    }
    if (rc == 0) {
        rc = pair_file(d, "q", 3);
    }
    if (rc == 0) {
        rc = OPS->mkdir(&mnt, d, "s", 0755);
    }
    OPS->put(&mnt, d);
    return rc;
}

/**
 * Make the volume every run starts from, 8 MiB from an empty tree, with
 * the directories "a" and "b" of `pair_side_make` and the empty "c" and "d",
 * and keep it in `pair_pristine`. 1, or 0 with a failed check.
 */
static int pair_volume(void)
{
    void *f;
    int ok;

    mkdir(fix_path("pairs-tree"), 0755);
    pair_bytes = 8ull << 20;
    ok = ext2_fixture_image(wimg, 4096, 128, PROFILE, 8, fix_path("pairs-tree"));
    check(ok, "the pairs' volume is made");
    if (!ok) {
        return 0;
    }
    use_image(wimg, pair_bytes);
    rw_flags = 0;
    ok = mount_rw() == 0;
    rw_flags = VFS_MS_DIRSYNC;
    ok = ok && pair_side_make("a") == 0 && pair_side_make("b") == 0 &&
         OPS->mkdir(&mnt, mnt.root, "c", 0755) == 0 && OPS->mkdir(&mnt, mnt.root, "d", 0755) == 0;
    ok = unmount_rw() == 0 && ok;
    check(ok, "the pairs' files are made");
    if (!ok) {
        return 0;
    }
    pair_pristine = malloc(pair_bytes);
    f = fopen(wimg, "rb");
    ok = pair_pristine && f && fread(pair_pristine, 1, pair_bytes, f) == pair_bytes;
    if (f) {
        fclose(f);
    }
    check(ok, "the pairs' volume is read into memory");
    return ok;
}

/** Fill `s` with the objects of the directory `name` for a run: the
 * directory, its file and a description of it, and its file `doomed`,
 * held and unlinked. 1, or 0. */
static int pair_side_open(struct pair_side *s, const char *name, const char *doomed)
{
    char path[16];
    int err;

    s->dir = walk(name, &err);
    snprintf(path, sizeof(path), "/%s/x", name);
    s->file = walk(path, &err);
    snprintf(path, sizeof(path), "/%s/%s", name, doomed);
    s->doomed = walk(path, &err);
    s->handle = 0;
    if (!s->dir || !s->file || !s->doomed || OPS->open(&mnt, s->file, 2, &s->handle) != 0) {
        return 0;
    }
    return OPS->unlink(&mnt, s->dir, doomed) == 0;
}

/** Give back what `pair_side_open` took that the run left. */
static void pair_side_close(struct pair_side *s)
{
    if (s->handle) {
        OPS->release(&mnt, s->handle);
    }
    if (s->doomed) {
        OPS->put(&mnt, s->doomed);
    }
    if (s->file) {
        OPS->put(&mnt, s->file);
    }
    if (s->dir) {
        OPS->put(&mnt, s->dir);
    }
    if (s->to) {
        OPS->put(&mnt, s->to);
    }
    memset(s, 0, sizeof(*s));
}

/* The layouts of a run. */
#define PAIR_APART 0 /* different files in different directories */
#define PAIR_ONE   1 /* one file and one directory */
#define PAIR_MOVES 2 /* two renames between directories */

/**
 * Run `a` as process A and `b` as process B, at A's disk request `k`, on
 * the layout `layout`, from the pristine volume; with `fsck`, check the
 * volume after. Whether the request came, with what B did in `pair_ended`,
 * `pair_slept` and `pair_b_rc`; `*a_rc` is A's result.
 */
static int pair_run(const struct pair_op *a, const struct pair_op *b, int layout, int k, int fsck,
                    int *a_rc)
{
    void *f;
    int ok, err;

    __builtin_memcpy(ram_image, pair_pristine, pair_bytes);
    (void)bcache_invalidate(&dev);
    rw_flags = 0;
    ok = mount_rw() == 0;
    rw_flags = VFS_MS_DIRSYNC;
    memset(&pair_a, 0, sizeof(pair_a));
    memset(&pair_b, 0, sizeof(pair_b));
    ok = ok && pair_side_open(&pair_a, "a", "p");
    ok = ok && pair_side_open(&pair_b, layout == PAIR_ONE ? "a" : "b", "q");
    pair_a.made = "n";
    pair_a.linked = "l";
    pair_a.moved = "r2";
    pair_b.made = "m";
    pair_b.linked = "l2";
    pair_b.moved = "r3";
    if (layout == PAIR_MOVES) {
        pair_a.to = walk("/c", &err);
        pair_b.to = walk("/d", &err);
        ok = ok && pair_a.to && pair_b.to;
    }
    check(ok, "a pair's run opens its files");
    ok = ok && OPS->sync(&mnt) == 0 && bcache_sync(&dev) == 0 && bcache_invalidate(&dev) == 0;
    if (ok && a->dirty_first) {
        ok = pair_write(&pair_a) == 0;
    }
    pair_target = k;
    pair_seen = pair_fired = pair_ended = 0;
    pair_b_rc = 0;
    pair_slept = 0;
    pair_second_op = b;
    read_pause = write_pause = pair_request;
    start_turns(pair_second);
    *a_rc = ok ? a->run(&pair_a) : -1;
    read_pause = write_pause = 0;
    finish_turns();
    pair_side_close(&pair_a);
    pair_side_close(&pair_b);
    ok = unmount_rw() == 0 && ok;
    check(ok, "a pair's run unmounts");
    if (fsck) {
        f = fopen(wimg, "wb");
        ok = f && fwrite(ram_image, 1, pair_bytes, f) == pair_bytes;
        if (f) {
            fclose(f);
        }
        check(ok && e2fsck_clean(wimg), "a pair's run leaves a clean volume");
    }
    return pair_fired;
}

/* The disk requests the rows gave process B at, and how many of them it
   slept on a node lock, on the rename lock and on a buffer of the request. */
static int pair_requests, pair_node_waits, pair_rename_waits, pair_buffer_waits;

/**
 * Run `a` against `b` on `layout` at each disk request of `a` in turn and
 * judge what B did at each against `want`: NULL for an end, or the kind of
 * lock B must sleep on; a sleep on a buffer of A's request passes either.
 * Both must succeed on a layout of different files; on one file, B may
 * change what A reads or removes first.
 */
static void pair_row(const struct pair_op *a, const struct pair_op *b, int layout, const char *want)
{
    static const char *const layouts[] = {"apart", "in one file", "moving"};
    char what[160];
    int k, a_rc, good = 1, requests = 0;

    for (k = 1; k <= 64; k++) {
        int fired = pair_run(a, b, layout, k, k == 1, &a_rc);

        if (!fired) {
            break;
        }
        requests++;
        if (layout != PAIR_ONE && (a_rc != 0 || pair_b_rc != 0)) {
            snprintf(what, sizeof(what), "%s then %s %s at request %d: %d and %d", a->name, b->name,
                     layouts[layout], k, a_rc, pair_b_rc);
            check(0, what);
            good = 0;
        }
        if (!pair_in_flight &&
            (want ? (pair_ended || !pair_slept || !streq(pair_slept, want)) : !pair_ended)) {
            snprintf(what, sizeof(what), "%s then %s %s at request %d, of block %lu: %s, not %s",
                     a->name, b->name, layouts[layout], k, (unsigned long)pair_block,
                     pair_ended ? "ended" : (pair_slept ? pair_slept : "slept"),
                     want ? want : "an end");
            check(0, what);
            good = 0;
        }
        if (!pair_ended && pair_slept && streq(pair_slept, "ext2 node")) {
            pair_node_waits++;
        }
        if (!pair_ended && pair_slept && streq(pair_slept, "rename")) {
            pair_rename_waits++;
        }
        pair_buffer_waits += pair_in_flight;
    }
    pair_requests += requests;
    if (a->holds && requests == 0 && good) {
        snprintf(what, sizeof(what), "%s then %s %s: no disk request", a->name, b->name,
                 layouts[layout]);
        check(0, what);
    }
}

/**
 * Every pair of `pair_ops` on both layouts, and two renames between
 * directories, with the volume in memory; the counts are printed, and the
 * image file is the device again after.
 */
static void pair_rows(void)
{
    int i, j;

    fill_pattern();
    if (!pair_volume()) {
        return;
    }
    if (image) {
        bcache_sync(&dev);
        fclose(image);
        image = 0;
    }
    ram_image = malloc(pair_bytes);
    check(ram_image != 0, "the pairs' device is had in memory");
    if (!ram_image) {
        free(pair_pristine);
        return;
    }
    pair_requests = pair_node_waits = pair_rename_waits = pair_buffer_waits = 0;
    for (i = 0; i < PAIR_OPS; i++) {
        for (j = 0; j < PAIR_OPS; j++) {
            const struct pair_op *a = &pair_ops[i], *b = &pair_ops[j];

            pair_row(a, b, PAIR_APART, 0);
            pair_row(a, b, PAIR_ONE, (a->holds & b->needs) ? "ext2 node" : 0);
        }
    }
    {
        static const struct pair_op move = {"move", pair_move, 0, 0, 0};

        pair_row(&move, &move, PAIR_MOVES, "rename");
    }
    printf("ext2-pairs: result: %d requests, %d slept on a node lock, %d on the rename lock, "
           "%d on a buffer of the request\n",
           pair_requests, pair_node_waits, pair_rename_waits, pair_buffer_waits);
    (void)bcache_invalidate(&dev);
    free(ram_image);
    ram_image = 0;
    free(pair_pristine);
    pair_pristine = 0;
    wlog_reset();
}

#endif
