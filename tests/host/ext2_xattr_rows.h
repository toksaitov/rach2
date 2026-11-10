/* User attribute rows, Linux-format fixtures and every write's crash state. */
#ifndef EXT2_XATTR_ROWS_H_
#define EXT2_XATTR_ROWS_H_

/** Call the actual driver's operation with a held node and kernel buffers. */
static int xa_call(void *n, int op, const char *name, void *buf, uint32_t size, int flags)
{
    struct vfs_xattr arg = {op, name, buf, size, flags};
    return OPS->xattr(&mnt, n, &arg);
}

/** Force bad attribute layouts through the same codec the driver uses. */
static void xattr_parser_rows(void)
{
    unsigned char *a = malloc(4096), *b = malloc(4096);
    char value[] = "value";
    struct vfs_xattr arg = {VFS_XATTR_SET, "a", value, 5, 0};

    check(a && b, "the parser row has private blocks");
    if (!a || !b)
        goto done;
    check(ext2_xattr_edit(0, a, 4096, &arg) == 1 && ext2_xattr_check(a, 4096) == 0,
          "the codec builds a valid one-entry block");
    arg.name = "long";
    check(ext2_xattr_edit(a, b, 4096, &arg) == 1 && ext2_xattr_check(b, 4096) == 0,
          "the codec sorts entries by index, length and name");
    b[54] = b[34];
    b[55] = b[35];
    check(ext2_xattr_check(b, 4096) == -EIO, "overlapping values are refused");
    a[34] = 0;
    a[35] = 0;
    check(ext2_xattr_check(a, 4096) == -EIO, "a value over the header is refused");
    check(ext2_xattr_edit(0, a, 4096, &arg) == 1, "the next malformed block starts valid");
    a[36] = 1;
    check(ext2_xattr_check(a, 4096) == -EIO, "external value blocks are refused");
    a[36] = 0;
    a[40] = 255;
    a[41] = 255;
    check(ext2_xattr_check(a, 4096) == -EIO, "a value beyond the block is refused");
    check(ext2_xattr_edit(0, a, 4096, &arg) == 1, "the bad-magic block starts valid");
    a[3] = 0;
    check(ext2_xattr_check(a, 4096) == -EIO, "bad magic is refused");
done:
    free(a);
    free(b);
}

/** Read and split Linux's shared block; preserve its other inode and hashes. */
static void xattr_value_rows(void)
{
    const char *img = fix_path("xattr.ext2");
    uint64_t bytes = attr_image(img, 2, 2);
    void *a, *b;
    unsigned char value[4096];
    struct vfs_stat before, after;
    int rc;

    use_image(img, bytes);
    check(mount_rw() == 0, "the Linux-format attribute volume mounts");
    if (!mnt.root)
        return;
    a = walk("/many/file0.txt", &rc);
    b = walk("/many/file1.txt", &rc);
    if (!a || !b) {
        check(0, "both sharers exist");
        goto done;
    }
    check(xa_call(a, VFS_XATTR_GET, "user.a", value, sizeof(value), 0) == 1 && value[0] == '1',
          "a value written by debugfs reads through the driver");
    check(xa_call(a, VFS_XATTR_LIST, 0, value, 0, 0) == 7 &&
              xa_call(a, VFS_XATTR_LIST, 0, value, 6, 0) == -ERANGE &&
              xa_call(a, VFS_XATTR_LIST, 0, value, 7, 0) == 7 && memcmp(value, "user.a\0", 7) == 0,
          "list queries and short buffers count the full NUL-terminated name");
    check(xa_call(a, VFS_XATTR_SET, "user.a", "2", 1, 1) == -EEXIST &&
              xa_call(a, VFS_XATTR_SET, "user.absent", "2", 1, 2) == -ENODATA,
          "create and replace enforce presence");
    OPS->getattr(&mnt, a, &before);
    check(xa_call(a, VFS_XATTR_SET, "user.a", "changed", 7, 2) == 0 &&
              xa_call(a, VFS_XATTR_GET, "user.a", value, 0, 0) == 7 &&
              xa_call(a, VFS_XATTR_GET, "user.a", value, 6, 0) == -ERANGE &&
              xa_call(b, VFS_XATTR_GET, "user.a", value, sizeof(value), 0) == 1 && value[0] == '1',
          "a shared block splits on replacement and its other inode is unchanged");
    OPS->getattr(&mnt, a, &after);
    check(before.blocks == after.blocks, "replacement counts one attribute block");
    check(xa_call(a, VFS_XATTR_SET, "user.DOSATTRIB", "\0\1\0\377", 4, 0) == 0 &&
              xa_call(a, VFS_XATTR_GET, "user.DOSATTRIB", value, sizeof(value), 0) == 4 &&
              memcmp(value, "\0\1\0\377", 4) == 0,
          "Wine's attribute name preserves binary values");
    check(xa_call(a, VFS_XATTR_SET, "user.empty", "", 0, 0) == 0 &&
              xa_call(a, VFS_XATTR_GET, "user.empty", value, sizeof(value), 0) == 0,
          "an empty value exists and is distinct from removal");
    memset(value, 'z', sizeof(value));
    check(xa_call(a, VFS_XATTR_SET, "user.large", value, sizeof(value), 0) == -ENOSPC &&
              xa_call(a, VFS_XATTR_GET, "user.a", value, sizeof(value), 0) == 7 &&
              memcmp(value, "changed", 7) == 0,
          "a value that does not fit leaves the existing block unchanged");
    check(xa_call(a, VFS_XATTR_REMOVE, "user.a", 0, 0, 0) == 0 &&
              xa_call(a, VFS_XATTR_REMOVE, "user.a", 0, 0, 0) == -ENODATA,
          "remove drops exactly one name and reports a missing name");
done:
    if (a)
        OPS->put(&mnt, a);
    if (b)
        OPS->put(&mnt, b);
    check(unmount_rw() == 0 && e2fsck_clean(img),
          "independent e2fsck accepts the rewritten hashes and layout");
    check(debugfs_says(img, "ea_list /many/file0.txt", "DOSATTRIB"),
          "the independent Linux format reader finds Wine's attribute");
}

/** Accept only allocation counts, lost blocks or an old block's high count.
 * Questions are parsed from the read-only fsck transcript, before repair. */
static int xattr_findings(const char *out)
{
    void *f;
    char line[512];
    int ok = 1;

    if (!ext2_fixture_findings(out, fix_path("xa-findings.txt")))
        return 0;
    f = fopen(fix_path("xa-findings.txt"), "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        if (!matches_pattern(line, "Free blocks count wrong") &&
            !matches_pattern(line, "Block bitmap differences") &&
            !matches_pattern(line,
                             "Extended attribute block # has reference count #, should be #")) {
            printf("ext2-xattr: unexpected repair: %s", line);
            ok = 0;
        }
    }
    fclose(f);
    return ok;
}

/** Replay every write prefix of create, replace and remove on both mounts.
 * Repair a copy first, demand a clean second fsck, then read all base data.
 * The untouched sharer's value must remain '1' in every repaired copy. */
static void xattr_stop_rows(void)
{
    const char *img = fix_path("xa-stop.ext2");
    const char *base = fix_path("xa-before.ext2");
    const char *trial = fix_path("xa-trial.ext2");
    const char *out = fix_path("xa-fsck.out");
    int mode, op, rc, k, count, j;
    uint64_t bytes;
    void *n;

    for (mode = 0; mode < 2; mode++) {
        for (op = 0; op < 6; op++) {
            bytes = attr_image(img, 2, 2);
            use_image(img, bytes);
            check((mount_flags(mode ? 0 : VFS_MS_DIRSYNC)) == 0, "the stop volume mounts");
            if (!mnt.root)
                return;
            n = walk(op == 0 ? "/hello.txt" : "/many/file0.txt", &rc);
            if (!n) {
                check(0, "the stop row holds its inode");
                unmount_rw();
                return;
            }
            if (op >= 3)
                check(xa_call(n, VFS_XATTR_SET, "user.b", "kept", 4, 0) == 0,
                      "the non-last removal has a second attribute");
            if (op >= 4)
                check(xa_call(n, VFS_XATTR_REMOVE, "user.b", 0, 0, 0) == 0,
                      "the private block has only the attribute the row changes");
            check(copy_file(img, base), "the pre-operation disk is copied");
            wlog_reset();
            wlog_capture = 1;
            rc = xa_call(n, op >= 2 && op <= 4 ? VFS_XATTR_REMOVE : VFS_XATTR_SET,
                         op == 0 ? "user.DOSATTRIB" : "user.a", "new", op >= 2 && op <= 4 ? 0 : 3,
                         0);
            count = wlog_n;
            wlog_capture = 0;
            check(rc == 0 && count > 0 && !wlog_overflow, "every attribute write was captured");
            OPS->put(&mnt, n);
            unmount_rw();
            for (k = 0; k <= count; k++) {
                int status;
                check(copy_file(base, trial), "a fresh crash copy is made");
                for (j = 0; j < k; j++)
                    poke(trial, (long)(wlog_lba[j] * 512), wlog_data[j], wlog_count[j] * 512);
                status = ext2_fixture_fsck(trial, "-fn", out);
                check((status == 0 || status == 4) && xattr_findings(out),
                      "the stopped disk has only allocation or attribute count faults");
                status = ext2_fixture_fsck(trial, "-fy", out);
                check(status >= 0 && status <= 2, "the first writable fsck repairs the crash copy");
                check(e2fsck_clean(trial), "a second read-only fsck finds the crash copy clean");
                use_image(trial, bytes);
                check(mount_ro() == 0 && base_intact(), "a repaired crash retains every base file");
                if (mnt.root) {
                    unsigned char v[8];
                    n = walk("/many/file1.txt", &rc);
                    check(n && xa_call(n, VFS_XATTR_GET, "user.a", v, sizeof(v), 0) == 1 &&
                              v[0] == '1',
                          "the other sharer's value survives every stop");
                    if (n)
                        OPS->put(&mnt, n);
                    unmount_image();
                }
            }
            wlog_reset();
        }
    }
}

/** A private attribute block can change even when no free block remains. */
static void xattr_full_rows(void)
{
    const char *img = fix_path("xa-full.ext2");
    uint64_t bytes = build_image(img, 4096, 128, ATTR_PROFILE, 4, "tree");
    void *n;
    struct vfs_statfs before, after;
    char value[] = "old", replacement[] = "new";
    int rc;

    use_image(img, bytes);
    check(mount_rw() == 0, "the full-disk attribute row mounts");
    if (!mnt.root)
        return;
    n = walk("/hello.txt", &rc);
    check(n && xa_call(n, VFS_XATTR_SET, "user.a", value, 3, 0) == 0,
          "the full-disk row first owns an attribute block");
    check(fill_volume() == 0, "the volume fills to ENOSPC");
    OPS->statfs(&mnt, &before);
    check(n && before.bfree == 0 && xa_call(n, VFS_XATTR_SET, "user.a", replacement, 3, 2) == 0,
          "an unshared block replaces a value with no free block");
    OPS->statfs(&mnt, &after);
    check(after.bfree == before.bfree, "replacement allocates no block");
    if (n)
        OPS->put(&mnt, n);
    check(unmount_rw() == 0 && e2fsck_clean(img), "the full volume ends clean");
}

/** Adding the first attribute promotes a revision-zero volume safely. */
static void xattr_revision_rows(void)
{
    const char *img = fix_path("xa-rev0.ext2");
    uint64_t bytes = build_image(img, 4096, 128,
                                 "^sparse_super,^large_file,^filetype,^dir_index,^resize_inode,"
                                 "^ext_attr,^has_journal,^extent,^64bit,^flex_bg,"
                                 "^metadata_csum,^huge_file,^dir_nlink,^extra_isize",
                                 4, "tree");
    void *n;
    char value[] = "old revision";
    int rc;

    check(debugfs_script(img, "ssv rev_level 0\n"), "the fixture has revision zero");
    use_image(img, bytes);
    check(mount_rw() == 0, "the revision-zero attribute row mounts");
    if (!mnt.root)
        return;
    n = walk("/hello.txt", &rc);
    check(n && xa_call(n, VFS_XATTR_SET, "user.a", value, sizeof(value), 0) == 0,
          "the first attribute enables the dynamic revision and its feature");
    if (n)
        OPS->put(&mnt, n);
    check(unmount_rw() == 0 && e2fsck_clean(img), "the promoted superblock is valid to e2fsck");
}

#ifdef __linux__
extern int setxattr(const char *path, const char *name, const void *value, unsigned long size,
                    int flags);
extern int removexattr(const char *path, const char *name);

/** mke2fs copies a real Linux user attribute from its host file into ext2. */
static void xattr_linux_rows(void)
{
    const char *file = fix_path("tree/hello.txt"), *img = fix_path("xa-linux.ext2");
    uint64_t bytes;
    void *n;
    unsigned char value[16];
    int rc;

    check(setxattr(file, "user.DOSATTRIB", "linux", 5, 0) == 0,
          "Linux stores the fixture's host attribute");
    bytes = build_image(img, 4096, 128, ATTR_PROFILE, 16, "tree");
    check(removexattr(file, "user.DOSATTRIB") == 0, "the host attribute is removed after staging");
    use_image(img, bytes);
    check(mount_ro() == 0, "mke2fs's copied Linux attribute volume mounts");
    if (!mnt.root)
        return;
    n = walk("/hello.txt", &rc);
    check(n && xa_call(n, VFS_XATTR_GET, "user.DOSATTRIB", value, sizeof(value), 0) == 5 &&
              memcmp(value, "linux", 5) == 0,
          "a real Linux attribute reads unchanged through ext2");
    if (n)
        OPS->put(&mnt, n);
    unmount_image();
}
#endif

/** The attribute slice, callable alone for a reviewed checkpoint. */
static void xattr_rows(void)
{
#ifdef __linux__
    xattr_linux_rows();
#endif
    xattr_revision_rows();
    xattr_full_rows();
    xattr_parser_rows();
    xattr_value_rows();
    xattr_stop_rows();
}
#endif
