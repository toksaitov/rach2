/*
 * ext2_names_rows.h: the rows of ext2's operations on names without the big
 * lock, included by ext2_hosttest.c: two processes create, unlink, rename
 * and remove names in the same directories at once, and the entries, the
 * link counts, the free counts and e2fsck must agree after.
 *
 * The operations run as callers without the big lock (`bkl_host_free`) and
 * take it only for their steps that still need it; each time they give it
 * back to nobody, where another processor could run, and at each disk
 * request, where the kernel sleeps, the other process is given the turn
 * (`bkl_host_gone`, `read_pause`, `write_pause`). Between those points an
 * operation reads and writes its directory under the directory's lock
 * alone, which the rows take as the VFS does (`held_ops`): without that
 * lock the second process would add or remove an entry in the block the
 * first is changing, between its scan and its write, and an entry would be
 * lost or a record overlap another, which the counts and e2fsck find.
 */

#ifndef EXT2_NAMES_ROWS_H_
#define EXT2_NAMES_ROWS_H_

/* compat/bkl_held.c: the hook run at each release of the big lock to
 * nobody. */
extern void (*bkl_host_gone)(void);
extern int *(*bkl_host_depth_of)(void);

/* Each process's depth of the big lock, since a turn may pass inside a
 * hold, at a disk request. */
static int names_depth[2];

/** The depth of the big lock of the process that runs. */
static int *names_depth_of(void)
{
    return &names_depth[current_proc == &proc_b];
}

/* The names each process of the first row makes or removes. */
#define NAMES_EACH 40

/* The lock that process A must hold before a release of the big lock gives
 * process B the turn, or NULL to give it at every release; and the turns
 * given. */
static const struct sleep_lock *names_gate;
static int names_turns;

/* Process B's directories and node, and the results of its operations. */
static void *names_b_dir, *names_b_other;
static int names_b_rc[4];

/** At a release of the big lock to nobody, give the other process the
 * turn, unless process A runs and does not yet hold `names_gate`. */
static void names_turn(void)
{
    if (names_gate && current_proc == &proc_a && !sleep_lock_held(names_gate)) {
        return;
    }
    names_turns++;
    pass_turn();
}

/** Run the operations from here on as callers without the big lock, the
 * processes taking turns at each release of it and at each disk request,
 * where the kernel sleeps; `gate` as `names_gate`. */
static void names_free(const struct sleep_lock *gate)
{
    names_gate = gate;
    names_turns = 0;
    names_depth[0] = names_depth[1] = 0;
    bkl_host_depth_of = names_depth_of;
    bkl_host_gone = names_turn;
    read_pause = names_turn;
    write_pause = names_turn;
    bkl_host_free = 1;
}

/** Give the big lock back to the rows' callers, who hold it. */
static void names_held(void)
{
    check(names_depth[0] == 0 && names_depth[1] == 0,
          "each process gives back every take of the big lock it made");
    bkl_host_free = 0;
    bkl_host_depth_of = 0;
    bkl_host_gone = 0;
    read_pause = 0;
    write_pause = 0;
    names_gate = 0;
}

/** The node `name` in `dir`, held, or NULL. */
static void *names_find(void *dir, const char *name)
{
    void *n = 0;

    return OPS->lookup(&mnt, dir, name, &n) == 0 ? n : 0;
}

/** Whether `name` in `dir` names a node with `nlink` links. */
static int names_has(void *dir, const char *name, long nlink)
{
    void *n = names_find(dir, name);
    long got = lock_nlink(n);

    if (n) {
        OPS->put(&mnt, n);
    }
    return got == nlink;
}

/** Whether `name` is absent from `dir`. */
static int names_gone(void *dir, const char *name)
{
    void *n = 0;
    int rc = OPS->lookup(&mnt, dir, name, &n);

    if (rc == 0) {
        OPS->put(&mnt, n);
    }
    return rc == -ENOENT;
}

/** The inode number `name` in `dir` names, or 0. */
static uint64_t names_ino(void *dir, const char *name)
{
    void *n = names_find(dir, name);
    uint64_t ino = lock_ino(n);

    if (n) {
        OPS->put(&mnt, n);
    }
    return ino;
}

/** A new volume, mounted with `dirsync`, and a directory `name` made in its
 * root, held; NULL when a step failed. */
static void *names_volume(const char *name)
{
    void *d;

    wbytes = build_image(wimg, 4096, 128, PROFILE, 16, "tree");
    use_image(wimg, wbytes);
    if (mount_rw() != 0) {
        check(0, "a volume for the names rows mounts writable");
        return 0;
    }
    if (OPS->mkdir(&mnt, mnt.root, name, 0755) != 0 || !(d = names_find(mnt.root, name))) {
        check(0, "a directory for the names rows is made");
        return 0;
    }
    return d;
}

/** Put `d` and `other`, unmount and judge the volume with e2fsck. */
static void names_end_volume(void *d, void *other, const char *what)
{
    if (other) {
        OPS->put(&mnt, other);
    }
    if (d) {
        OPS->put(&mnt, d);
    }
    check(unmount_rw() == 0 && e2fsck_clean(wimg), what);
}

/** The name `prefix` and two digits of `i`, into `buf`. */
static const char *names_name(char *buf, char prefix, int i)
{
    buf[0] = prefix;
    buf[1] = (char)('0' + i / 10);
    buf[2] = (char)('0' + i % 10);
    buf[3] = '\0';
    return buf;
}

/** Process B of `names_create_unlink_row`: every u name of its directory
 * removed, the first failure kept. */
static void names_unlink_all(void)
{
    char name[4];
    int i, rc;

    names_b_rc[0] = 0;
    for (i = 0; i < NAMES_EACH; i++) {
        rc = OPS->unlink(&mnt, names_b_dir, names_name(name, 'u', i));
        if (rc != 0 && names_b_rc[0] == 0) {
            names_b_rc[0] = rc;
        }
    }
}

/**
 * Two processes in one directory: A creates NAMES_EACH names while B
 * unlinks NAMES_EACH others, each giving the other the turn at every
 * release of the big lock. Every create and unlink succeeds, the made names
 * are there with one link each and the removed ones are gone, the
 * directory keeps two links, the free inodes are as before, and e2fsck
 * finds the volume clean.
 */
static void names_create_unlink_row(void)
{
    char name[4];
    uint64_t free_before;
    void *d = names_volume("one");
    int i, rc, made = 1, kept = 1, gone = 1, first = 0;

    if (!d) {
        return;
    }
    for (i = 0; i < NAMES_EACH; i++) {
        void *n = make_file(d, names_name(name, 'u', i), &rc);

        made = made && n;
        if (n) {
            OPS->put(&mnt, n);
        }
    }
    check(made, "forty names to remove are made in one directory");
    free_before = lock_free_inodes();
    names_b_dir = d;
    names_free(0);
    start_turns(names_unlink_all);
    for (i = 0; i < NAMES_EACH; i++) {
        void *n = make_file(d, names_name(name, 'c', i), &rc);

        if (rc != 0 && first == 0) {
            first = rc;
        }
        if (n) {
            OPS->put(&mnt, n);
        }
    }
    finish_turns();
    names_held();
    check(first == 0 && names_b_rc[0] == 0 && names_turns > 2 * NAMES_EACH,
          "forty creates and forty unlinks in one directory take turns and all succeed");
    for (i = 0; i < NAMES_EACH; i++) {
        kept = kept && names_has(d, names_name(name, 'c', i), 1);
        gone = gone && names_gone(d, names_name(name, 'u', i));
    }
    check(kept && gone, "each made name has one link and each removed one is gone");
    check(lock_nlink(d) == 2 && lock_free_inodes() == free_before,
          "the directory keeps two links and the free inodes are as before");
    names_end_volume(d, 0, "e2fsck finds the volume of creates and unlinks clean");
}

/* The files each process of the rename row moves, and makes. */
#define NAMES_MOVES 12

/** Rounds of `names_rename_row` for one process: the directory `from` into
 * `to`, then each round the file `prefix`i moved from `from` to `to` and
 * the file `made`i made in `from`, where the other process moves its files
 * in. The first failure. */
static int names_cross(void *from, void *to, const char *dir, char prefix, char made)
{
    char name[4];
    void *n;
    int i, rc, first = OPS->rename(&mnt, from, dir, to, dir);

    for (i = 0; i < NAMES_MOVES; i++) {
        rc = OPS->rename(&mnt, from, names_name(name, prefix, i), to, name);
        first = first ? first : rc;
        n = 0;
        rc = OPS->create(&mnt, from, names_name(name, made, i), 0644, &n);
        first = first ? first : rc;
        if (n) {
            OPS->put(&mnt, n);
        }
    }
    return first;
}

/** Process B of `names_rename_row`: the right directory's into the left. */
static void names_rename_back(void)
{
    names_b_rc[0] = names_cross(names_b_other, names_b_dir, "b", 'q', 'r');
}

/**
 * Renames between two directories in both directions at once: A moves a
 * directory and then NAMES_MOVES files from the left to the right, making
 * a file in the left after each, while B moves another directory and as
 * many files from the right to the left, making a file in the right after
 * each; so each create runs inside the other's renames, which the mount's
 * rename lock does not order against it, and only the directories' locks
 * do. Nothing waits on the other for good, every name is where its rename
 * or create put it with one link, each moved directory's `..` names its new
 * parent, both parents keep three links, and e2fsck finds the volume clean.
 * This row judges the order of the locks; the turns here fall at no point
 * between a rename's search of a directory and its write there, so the
 * next row is the one that a missing directory lock fails.
 */
static void names_rename_row(void)
{
    char name[4];
    void *left = names_volume("left"), *right = 0, *n = 0;
    int i, rc, made = 1, moved = 1;

    if (!left) {
        return;
    }
    if (OPS->mkdir(&mnt, mnt.root, "right", 0755) != 0 ||
        !(right = names_find(mnt.root, "right")) || OPS->mkdir(&mnt, left, "a", 0755) != 0 ||
        OPS->mkdir(&mnt, right, "b", 0755) != 0) {
        check(0, "two directories each hold a directory");
        names_end_volume(left, right, "the rename volume unmounts");
        return;
    }
    for (i = 0; i < NAMES_MOVES; i++) {
        made = made && OPS->create(&mnt, left, names_name(name, 'p', i), 0644, &n) == 0;
        if (made) {
            OPS->put(&mnt, n);
        }
        made = made && OPS->create(&mnt, right, names_name(name, 'q', i), 0644, &n) == 0;
        if (made) {
            OPS->put(&mnt, n);
        }
    }
    check(made, "and each a dozen files to move");
    names_b_dir = left;
    names_b_other = right;
    names_free(0);
    start_turns(names_rename_back);
    rc = names_cross(left, right, "a", 'p', 'm');
    finish_turns();
    names_held();
    check(rc == 0 && names_b_rc[0] == 0 && names_turns > 0,
          "renames between two directories in both directions take turns and all succeed");
    for (i = 0; i < NAMES_MOVES; i++) {
        moved = moved && names_has(right, names_name(name, 'p', i), 1) && names_gone(left, name) &&
                names_has(left, names_name(name, 'q', i), 1) && names_gone(right, name) &&
                names_has(left, names_name(name, 'm', i), 1) &&
                names_has(right, names_name(name, 'r', i), 1);
    }
    check(moved && names_has(right, "a", 2) && names_has(left, "b", 2) && names_gone(left, "a") &&
              names_gone(right, "b"),
          "every moved and made name is in its new directory with its links, and none is left");
    {
        void *a = names_find(right, "a"), *b = names_find(left, "b");

        check(a && b && names_ino(a, "..") == lock_ino(right) &&
                  names_ino(b, "..") == lock_ino(left),
              "each moved directory's .. names its new parent");
        if (a) {
            OPS->put(&mnt, a);
        }
        if (b) {
            OPS->put(&mnt, b);
        }
    }
    check(lock_nlink(left) == 3 && lock_nlink(right) == 3, "both parents keep three links");
    names_end_volume(left, right, "e2fsck finds the volume of crossed renames clean");
}

/** Process B of `names_over_row`: each round the name v`i` of the right
 * directory removed and the name r`i` made there, in the slot it left. */
static void names_remove_reuse(void)
{
    char name[4];
    void *n;
    int i, rc;

    names_b_rc[0] = 0;
    for (i = 0; i < NAMES_MOVES; i++) {
        rc = OPS->unlink(&mnt, names_b_other, names_name(name, 'v', i));
        names_b_rc[0] = names_b_rc[0] ? names_b_rc[0] : rc;
        n = 0;
        rc = OPS->create(&mnt, names_b_other, names_name(name, 'r', i), 0644, &n);
        names_b_rc[0] = names_b_rc[0] ? names_b_rc[0] : rc;
        if (n) {
            OPS->put(&mnt, n);
        }
    }
}

/**
 * Renames from the left directory over names of the right while B removes
 * those names and makes others in the slots they leave: A finds the name it
 * replaces, has both nodes, which gives B the turn, and rewrites that entry;
 * only the right directory's lock keeps B from reusing the entry between.
 * Each pair ends one of two ways, the rename first, after which B's unlink
 * removes the moved file, or the unlink first, after which the rename makes
 * the name anew: so every name A moves is gone from the left, and in the
 * right a name A moved over has one link or none, each name B made has one,
 * and e2fsck finds the volume clean, with no entry naming a freed inode.
 */
static void names_over_row(void)
{
    char name[4];
    void *left = names_volume("left"), *right = 0, *n = 0;
    int i, rc = 0, made = 1, whole = 1;

    if (!left) {
        return;
    }
    if (OPS->mkdir(&mnt, mnt.root, "right", 0755) != 0 ||
        !(right = names_find(mnt.root, "right"))) {
        check(0, "two directories for renames over names");
        names_end_volume(left, right, "the rename volume unmounts");
        return;
    }
    for (i = 0; i < NAMES_MOVES; i++) {
        made = made && OPS->create(&mnt, left, names_name(name, 'p', i), 0644, &n) == 0;
        if (made) {
            OPS->put(&mnt, n);
        }
        made = made && OPS->create(&mnt, right, names_name(name, 'v', i), 0644, &n) == 0;
        if (made) {
            OPS->put(&mnt, n);
        }
    }
    check(made, "each directory has a dozen files");
    names_b_other = right;
    names_free(0);
    start_turns(names_remove_reuse);
    for (i = 0; i < NAMES_MOVES; i++) {
        char to[4];
        int got = OPS->rename(&mnt, left, names_name(name, 'p', i), right, names_name(to, 'v', i));

        rc = rc ? rc : got;
    }
    finish_turns();
    names_held();
    check(rc == 0 && names_b_rc[0] == 0 && names_turns > 0,
          "renames over names another process removes and reuses take turns and all succeed");
    for (i = 0; i < NAMES_MOVES; i++) {
        whole = whole && names_gone(left, names_name(name, 'p', i)) &&
                (names_gone(right, names_name(name, 'v', i)) || names_has(right, name, 1)) &&
                names_has(right, names_name(name, 'r', i), 1);
    }
    check(whole, "each moved name left its directory, and each name in the other has its link");
    names_end_volume(left, right, "e2fsck finds the volume of renames over reused names clean");
}

/** Process B of `names_rmdir_row`: a file made in the directory A removes. */
static void names_create_inside(void)
{
    void *n = 0;

    names_b_rc[0] = OPS->create(&mnt, names_b_other, "f", 0644, &n);
    if (n) {
        OPS->put(&mnt, n);
    }
}

/**
 * rmdir of `parent/sub` while B makes a file in `sub`, which B holds. With
 * `rmdir_first`, B is given the turn only once A holds `sub`'s lock, so the
 * removal wins and the create finds a removed directory, -ENOENT; without,
 * B takes `sub`'s lock first and the removal finds it not empty. Either way
 * the outcome is whole: the links of the parent and the file, the free
 * inodes and e2fsck agree with the one that won. A removed directory B
 * still holds keeps its inode until B's last put, as an open one does.
 */
static void names_rmdir_one(int rmdir_first)
{
    void *p = names_volume("parent"), *s = 0;
    uint64_t free_before;
    int rc;

    if (!p) {
        return;
    }
    free_before = lock_free_inodes();
    if (OPS->mkdir(&mnt, p, "sub", 0755) != 0 || !(s = names_find(p, "sub"))) {
        check(0, "a directory holds an empty directory");
        names_end_volume(p, 0, "the rmdir volume unmounts");
        return;
    }
    names_b_other = s;
    names_free(rmdir_first ? &((struct ext2_node *)s)->lock : 0);
    start_turns(names_create_inside);
    rc = OPS->rmdir(&mnt, p, "sub");
    finish_turns();
    names_held();
    if (rmdir_first) {
        check(
            rc == 0 && names_b_rc[0] == -ENOENT,
            "an rmdir that holds the directory first removes it, and the create in it is -ENOENT");
        check(names_gone(p, "sub") && lock_nlink(p) == 2 && lock_nlink(s) == 0 &&
                  lock_free_inodes() == free_before - 1,
              "the parent has two links, and the removed directory none but its inode while held");
        OPS->put(&mnt, s);
        s = 0;
        check(lock_free_inodes() == free_before,
              "and its last put frees the inode, the free inodes as before the directory");
    } else {
        check(
            rc == -ENOTEMPTY && names_b_rc[0] == 0,
            "a create that holds the directory first makes its file, and the rmdir is -ENOTEMPTY");
        check(names_has(p, "sub", 2) && names_has(s, "f", 1) && lock_nlink(p) == 3 &&
                  lock_free_inodes() == free_before - 2,
              "the directory and its file are there with their links");
    }
    names_end_volume(p, s,
                     rmdir_first ? "e2fsck finds the volume of the removal that won clean"
                                 : "e2fsck finds the volume of the create that won clean");
}

/** rmdir racing a create inside the directory, with each winning once. */
static void names_rmdir_row(void)
{
    names_rmdir_one(0);
    names_rmdir_one(1);
}

/**
 * Resume two listings at byte 36 after removing a and b and reusing their
 * space for a twelve-byte name at byte 24. Both must skip to c at byte 48,
 * even with a write pending in the batch, without mistaking name bytes for
 * a corrupt header or marking the volume read-only. A seek into the same
 * name also realigns, even when the directory has not changed again.
 */
static void names_readdir_reuse_row(void)
{
    struct vfs_dirent ent;
    uint32_t cursor[2] = {0, 0};
    void *d = 0, *n = 0, *h[2] = {0, 0}, *writer = 0;
    int i, j, rc, ok = 1;
    const char *old[3] = {"a", "b", "c"};
    const char *first[3] = {".", "..", "a"};

    if (!fsync_mount(0)) {
        return;
    }
    if (OPS->mkdir(&mnt, mnt.root, "listing", 0755) != 0 ||
        !(d = names_find(mnt.root, "listing"))) {
        check(0, "the listing directory is made");
        goto out;
    }
    for (i = 0; i < 3; i++) {
        n = make_file(d, old[i], &rc);
        ok = ok && n;
        if (n) {
            OPS->put(&mnt, n);
            n = 0;
        }
    }
    for (i = 0; i < 2; i++) {
        rc = OPS->open(&mnt, d, 0, &h[i]);
        ok = ok && rc == 0;
        if (rc == 0) {
            for (j = 0; j < 3; j++) {
                rc = OPS->readdir(&mnt, h[i], &cursor[i], &ent);
                ok = ok && rc == 1 && streq(ent.name, first[j]);
            }
        }
    }
    check(ok && cursor[0] == 36 && cursor[1] == 36,
          "two independent listings stop after a at byte 36");
    if (!ok || cursor[0] != 36 || cursor[1] != 36) {
        goto out;
    }
    rc = OPS->unlink(&mnt, d, "a");
    ok = rc == 0 && OPS->unlink(&mnt, d, "b") == 0;
    n = make_file(d, "abcdefghijkl", &rc);
    ok = ok && n && rc == 0;
    if (n) {
        OPS->put(&mnt, n);
    }
    n = names_find(d, "c");
    if (n) {
        rc = OPS->open(&mnt, n, 2, &writer);
        ok = ok && rc == 0;
    } else {
        ok = 0;
    }
    check(ok, "a and b are removed and their space reused for a twelve-byte name");
    if (!ok) {
        goto out;
    }
    writes_made = 0;
    check(OPS->write(&mnt, writer, "pending", 7, 0) == 7 && writes_made == 0,
          "a file write stays pending while the listings resume");
    for (i = 0; i < 2; i++) {
        rc = OPS->readdir(&mnt, h[i], &cursor[i], &ent);
        check(rc == 1 && streq(ent.name, "c") && cursor[i] == 4096,
              "each listing resumes at c after realigning past the reused name");
        check(OPS->readdir(&mnt, h[i], &cursor[i], &ent) == 0, "and reaches the directory's end");
    }
    cursor[0] = 36;
    check(OPS->readdir(&mnt, h[0], &cursor[0], &ent) == 1 && streq(ent.name, "c"),
          "a seek into a name realigns even without another directory change");
    check(!(mnt.flags & VFS_MS_RDONLY) && !mnt.write_error,
          "the reused cursor sets no error and leaves the volume writable");
    check(OPS->fsync(&mnt, writer) == 0, "the pending file write syncs without error");
out:
    if (writer) {
        OPS->release(&mnt, writer);
    }
    if (n) {
        OPS->put(&mnt, n);
    }
    for (i = 0; i < 2; i++) {
        if (h[i]) {
            OPS->release(&mnt, h[i]);
        }
    }
    names_end_volume(d, 0, "e2fsck finds the volume of the resumed listings clean");
}

/** Replace a and b with a longer name once lookup has its inode and has
 * let the directory lock go. The hook runs at the node table's big-lock
 * release, with no node lock held, and disables itself before any change. */
static void names_lookup_reuse(void)
{
    void *n;
    int rc;

    if (sleep_lock_held(&((struct ext2_node *)names_b_dir)->lock)) {
        return;
    }
    bkl_host_gone = 0;
    names_b_rc[0] = OPS->unlink(&mnt, names_b_dir, "a");
    rc = OPS->unlink(&mnt, names_b_dir, "b");
    names_b_rc[0] = names_b_rc[0] ? names_b_rc[0] : rc;
    n = make_file(names_b_dir, "abcdefghijkl", &rc);
    names_b_rc[0] = names_b_rc[0] ? names_b_rc[0] : rc;
    if (n) {
        OPS->put(&mnt, n);
    }
}

/** A lookup whose saved offset is reused while it gets the inode must
 * restart the search and return ENOENT, without parsing the name bytes
 * now at that offset as a directory header. */
static void names_lookup_reuse_row(void)
{
    void *d = names_volume("lookup"), *a = 0, *b = 0, *c = 0, *found = 0;
    int rc, ok;

    if (!d) {
        return;
    }
    a = make_file(d, "a", &rc);
    b = make_file(d, "b", &rc);
    c = make_file(d, "c", &rc);
    ok = a && b && c;
    check(ok, "three files for a lookup whose offset will be reused are made");
    if (ok) {
        names_b_dir = d;
        names_b_rc[0] = -1;
        bkl_host_free = 1;
        bkl_host_gone = names_lookup_reuse;
        rc = OPS->lookup(&mnt, d, "b", &found);
        bkl_host_gone = 0;
        bkl_host_free = 0;
        check(names_b_rc[0] == 0 && rc == -ENOENT && !found,
              "a lookup retries after its offset is reused, and the removed name is ENOENT");
        check(!(mnt.flags & VFS_MS_RDONLY) && !mnt.write_error,
              "the lookup's stale offset sets no error and leaves the volume writable");
    }
    if (found) {
        OPS->put(&mnt, found);
    }
    if (a) {
        OPS->put(&mnt, a);
    }
    if (b) {
        OPS->put(&mnt, b);
    }
    if (c) {
        OPS->put(&mnt, c);
    }
    names_end_volume(d, 0, "e2fsck finds the volume of the retried lookup clean");
}

/* The changes `names_lookup_churn` has made, and the most it makes, so that
 * a lookup that would go round for ever under them ends. */
static int names_churns;
#define NAMES_CHURN_MAX 64

/** At each release of the big lock while the lookup's directory is not
 * locked, make the name x if it is absent and remove it if present: a change
 * of a name the lookup does not seek, on every round of it. The hook is off
 * while it changes the directory. */
static void names_lookup_churn(void)
{
    void *n;
    int rc;

    if (names_churns >= NAMES_CHURN_MAX ||
        sleep_lock_held(&((struct ext2_node *)names_b_dir)->lock)) {
        return;
    }
    bkl_host_gone = 0;
    if (names_churns++ % 2 == 0) {
        n = make_file(names_b_dir, "x", &rc);
        if (n) {
            OPS->put(&mnt, n);
        }
    } else {
        rc = OPS->unlink(&mnt, names_b_dir, "x");
    }
    names_b_rc[0] = names_b_rc[0] ? names_b_rc[0] : rc;
    bkl_host_gone = names_lookup_churn;
}

/** A lookup of a name that stays while another name of its directory
 * changes on every round must end, with that name's node, after a bounded
 * number of changes and not at the hook's limit. */
static void names_lookup_churn_row(void)
{
    void *d = names_volume("churn"), *c = 0, *found = 0;
    int rc;

    if (!d) {
        return;
    }
    c = make_file(d, "c", &rc);
    check(c != 0, "a file for a lookup in a directory that keeps changing is made");
    if (c) {
        names_b_dir = d;
        names_b_rc[0] = 0;
        names_churns = 0;
        bkl_host_free = 1;
        bkl_host_gone = names_lookup_churn;
        rc = OPS->lookup(&mnt, d, "c", &found);
        bkl_host_gone = 0;
        bkl_host_free = 0;
        check(rc == 0 && found == c && names_b_rc[0] == 0,
              "a lookup finds its name while another name changes on every round");
        check(names_churns > 0 && names_churns < NAMES_CHURN_MAX,
              "and ends after a bounded number of changes, holding the directory's lock");
    }
    if (found) {
        OPS->put(&mnt, found);
    }
    if (c) {
        OPS->put(&mnt, c);
    }
    names_end_volume(d, 0, "e2fsck finds the volume of the lookup under changes clean");
}

/** The rows of operations on names without the big lock. */
static void names_rows(void)
{
    names_create_unlink_row();
    names_rename_row();
    names_over_row();
    names_rmdir_row();
    names_readdir_reuse_row();
    names_lookup_reuse_row();
    names_lookup_churn_row();
}

#endif
