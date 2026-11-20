/* POSIX ACL rows: the xattr handlers, the permission check, inheritance,
 * chmod's update, and the format against e2fsprogs' own conversion. */
#ifndef EXT2_ACL_ROWS_H_
#define EXT2_ACL_ROWS_H_

#define ACL_A "system.posix_acl_access"
#define ACL_D "system.posix_acl_default"

/* The directories and umask of the rows' process, which creates files. */
static struct fs_struct acl_fs;

/** Start an ACL of the xattr form, version 2, in `v`; its length. */
static uint32_t acl_head(unsigned char *v)
{
    memset(v, 0, 4);
    v[0] = 2;
    return 4;
}

/** Append the entry `tag`, `perm` and `id` to the ACL `v` of `len` bytes;
 * the new length. */
static uint32_t acl_entry(unsigned char *v, uint32_t len, unsigned tag, unsigned perm, uint32_t id)
{
    v[len] = (unsigned char)tag;
    v[len + 1] = 0;
    v[len + 2] = (unsigned char)perm;
    v[len + 3] = 0;
    v[len + 4] = (unsigned char)id;
    v[len + 5] = (unsigned char)(id >> 8);
    v[len + 6] = (unsigned char)(id >> 16);
    v[len + 7] = (unsigned char)(id >> 24);
    return len + 8;
}

/** The ACL user::P1,user:200:P2,group::P3,mask::P4,other::P5 into `v`;
 * its length. */
static uint32_t acl_named(unsigned char *v, unsigned p1, unsigned p2, unsigned p3, unsigned p4,
                          unsigned p5)
{
    uint32_t len = acl_head(v);

    len = acl_entry(v, len, 0x01, p1, 0xFFFFFFFFu);
    len = acl_entry(v, len, 0x02, p2, 200);
    len = acl_entry(v, len, 0x04, p3, 0xFFFFFFFFu);
    len = acl_entry(v, len, 0x10, p4, 0xFFFFFFFFu);
    return acl_entry(v, len, 0x20, p5, 0xFFFFFFFFu);
}

/** Whether the ACL `name` of node `n` reads back as the `len` bytes of
 * `want`, at a size query too. */
static int acl_reads(void *n, const char *name, const unsigned char *want, uint32_t len)
{
    unsigned char got[256];

    return xa_call(n, VFS_XATTR_GET, name, got, 0, 0) == (int)len &&
           xa_call(n, VFS_XATTR_GET, name, got, sizeof(got), 0) == (int)len &&
           memcmp(got, want, len) == 0;
}

/** The permission bits of node `n`, or 07777 + 1 when it cannot be read. */
static uint32_t acl_mode(void *n)
{
    struct vfs_stat st;

    return OPS->getattr(&mnt, n, &st) == 0 ? st.mode & 07777u : 010000u;
}

/** Act as user `uid` of group `gid`, root for 0. */
static void acl_as(uint32_t uid, uint32_t gid)
{
    proc_a.euid = uid;
    proc_a.egid = gid;
}

/** Whether `list` of `len` bytes, a listxattr answer, holds `name`. */
static int acl_listed(const char *list, int len, const char *name)
{
    int at = 0;

    while (at < len) {
        if (!strcmp(list + at, name))
            return 1;
        at += (int)strlen(list + at) + 1;
    }
    return 0;
}

/**
 * The access ACL of /dir/sub/deep.txt, owned by 100:100: set as setfacl
 * sets it, with the group bits its mask's, read back whole, listed, judged
 * for a named user, a member of the group and another, given chmod's bits,
 * refused to another user and in a malformed form, and removed when the
 * mode says all of it.
 */
static void acl_access_rows(void)
{
    unsigned char v[256], got[256];
    char list[256];
    struct vfs_stat st;
    uint32_t len;
    int rc;
    void *n = walk("/dir/sub/deep.txt", &rc);

    if (!n) {
        check(0, "deep.txt exists for the ACL rows");
        return;
    }
    memset(&st, 0, sizeof(st));
    st.uid = 100;
    st.gid = 100;
    st.mode = 0100640;
    check(OPS->setattr(&mnt, n, VFS_SET_UID | VFS_SET_GID | VFS_SET_MODE, &st) == 0,
          "the ACL file takes its owner and mode");
    len = acl_named(v, 6, 4, 4, 6, 0);
    check(xa_call(n, VFS_XATTR_SET, ACL_A, v, len, 0) == 0 && acl_mode(n) == 0660,
          "setfacl's access ACL is kept, and its mask becomes the group bits");
    check(acl_reads(n, ACL_A, v, len) &&
              xa_call(n, VFS_XATTR_GET, ACL_A, got, len - 1, 0) == -ERANGE,
          "getfacl reads the ACL back whole, and a short buffer is ERANGE");
    rc = xa_call(n, VFS_XATTR_LIST, 0, list, sizeof(list), 0);
    check(rc > 0 && acl_listed(list, rc, ACL_A), "listxattr names the access ACL");
    acl_as(200, 300);
    check(OPS->check_acl(&mnt, n, 4, 0) == 0 && OPS->check_acl(&mnt, n, 2, 0) == -EACCES,
          "the named user reads and may not write");
    acl_as(300, 100);
    check(OPS->check_acl(&mnt, n, 4, 0) == 0 && OPS->check_acl(&mnt, n, 6, 0) == -EACCES,
          "a member of the owning group has the group entry");
    acl_as(300, 300);
    check(OPS->check_acl(&mnt, n, 4, 0) == -EACCES, "anyone else has the others' entry");
    check(xa_call(n, VFS_XATTR_SET, ACL_A, v, len, 0) == -EPERM,
          "a caller who does not own the file may not set its ACL");
    acl_as(0, 0);
    st.mode = 0100600;
    check(OPS->setattr(&mnt, n, VFS_SET_MODE, &st) == 0 && acl_mode(n) == 0600,
          "chmod 600 of a file with an ACL succeeds");
    len = acl_named(v, 6, 4, 4, 0, 0);
    acl_as(200, 300);
    check(acl_reads(n, ACL_A, v, len) && OPS->check_acl(&mnt, n, 4, 0) == -EACCES,
          "and empties the mask, which then refuses the named user");
    acl_as(0, 0);
    v[0] = 1;
    check(xa_call(n, VFS_XATTR_SET, ACL_A, v, len, 0) == -EOPNOTSUPP,
          "an xattr form of another version is EOPNOTSUPP");
    len = acl_head(v);
    len = acl_entry(v, len, 0x01, 6, 0);
    len = acl_entry(v, len, 0x02, 4, 200);
    len = acl_entry(v, len, 0x04, 4, 0);
    len = acl_entry(v, len, 0x20, 0, 0);
    check(xa_call(n, VFS_XATTR_SET, ACL_A, v, len, 0) == -EINVAL,
          "a named entry without a mask is EINVAL");
    len = acl_head(v);
    len = acl_entry(v, len, 0x01, 7, 0);
    len = acl_entry(v, len, 0x04, 5, 0);
    len = acl_entry(v, len, 0x20, 4, 0);
    check(xa_call(n, VFS_XATTR_SET, ACL_A, v, len, 0) == 0 && acl_mode(n) == 0754 &&
              xa_call(n, VFS_XATTR_GET, ACL_A, got, sizeof(got), 0) == -ENODATA,
          "an ACL the mode says all of sets the mode and is not kept");
    check(xa_call(n, VFS_XATTR_REMOVE, ACL_A, 0, 0, 0) == 0,
          "removing an access ACL the file does not have succeeds");
    check(xa_call(n, VFS_XATTR_SET, ACL_D, v, len, 0) == -EACCES &&
              xa_call(n, VFS_XATTR_REMOVE, ACL_D, 0, 0, 0) == 0,
          "a default ACL is EACCES on a file, and its removal succeeds");
    OPS->put(&mnt, n);
}

/**
 * Inheritance from /dir's default ACL: a file created 0666 under it takes
 * the default as its access ACL, fitted to the mode, and no umask; a
 * directory takes it as both; a file created in /many, which has none,
 * takes the umask.
 */
static void acl_inherit_rows(void)
{
    unsigned char d[256], v[256];
    uint32_t dlen = acl_named(d, 7, 7, 5, 7, 0), len;
    void *dir, *n;
    int rc;

    dir = walk("/dir", &rc);
    if (!dir) {
        check(0, "/dir exists for the inheritance rows");
        return;
    }
    check(xa_call(dir, VFS_XATTR_SET, ACL_D, d, dlen, 0) == 0 && acl_reads(dir, ACL_D, d, dlen),
          "a directory keeps a default ACL");
    n = 0;
    rc = OPS->create(&mnt, dir, "inherit.txt", 0666, &n);
    len = acl_named(v, 6, 7, 5, 6, 0);
    check(rc == 0 && n && acl_mode(n) == 0660 && acl_reads(n, ACL_A, v, len),
          "a new file takes the default ACL fitted to its mode, without the umask");
    if (n) {
        check(xa_call(n, VFS_XATTR_GET, ACL_D, v, sizeof(v), 0) == -ENODATA,
              "and no default ACL of its own");
        OPS->put(&mnt, n);
    }
    rc = OPS->mkdir(&mnt, dir, "inherit.d", 0777);
    n = rc == 0 ? walk("/dir/inherit.d", &rc) : 0;
    len = acl_named(v, 7, 7, 5, 7, 0);
    check(n && acl_mode(n) == 0770 && acl_reads(n, ACL_A, v, len) && acl_reads(n, ACL_D, d, dlen),
          "a new directory takes the default ACL as its access and default ACLs");
    if (n)
        OPS->put(&mnt, n);
    OPS->put(&mnt, dir);
    dir = walk("/many", &rc);
    n = 0;
    rc = dir ? OPS->create(&mnt, dir, "plain.txt", 0666, &n) : rc;
    check(rc == 0 && n && acl_mode(n) == 0644 &&
              xa_call(n, VFS_XATTR_GET, ACL_A, v, sizeof(v), 0) == -ENODATA,
          "a file made where no default ACL is takes the umask");
    if (n)
        OPS->put(&mnt, n);
    if (dir)
        OPS->put(&mnt, dir);
}

/** A symbolic link keeps no ACL, and a mount without ACLs answers
 * EOPNOTSUPP and lists none. */
static void acl_refusal_rows(void)
{
    unsigned char v[256];
    char list[256];
    uint32_t len = acl_named(v, 6, 4, 4, 6, 0);
    int rc;
    void *n = walk("/fast", &rc);

    if (n) {
        check(xa_call(n, VFS_XATTR_GET, ACL_A, v, sizeof(v), 0) == -EOPNOTSUPP &&
                  xa_call(n, VFS_XATTR_SET, ACL_A, v, len, 0) == -EOPNOTSUPP,
              "a symbolic link has no ACL");
        OPS->put(&mnt, n);
    }
    n = walk("/dir/inherit.txt", &rc);
    if (!n)
        return;
    mnt.sb_posixacl = 0;
    rc = xa_call(n, VFS_XATTR_LIST, 0, list, sizeof(list), 0);
    check(xa_call(n, VFS_XATTR_GET, ACL_A, v, sizeof(v), 0) == -EOPNOTSUPP &&
              xa_call(n, VFS_XATTR_SET, ACL_A, v, len, 0) == -EOPNOTSUPP && rc >= 0 &&
              !acl_listed(list, rc, ACL_A),
          "a mount without ACLs refuses them and lists none");
    mnt.sb_posixacl = 1;
    OPS->put(&mnt, n);
}

/** Whether debugfs's `ea_get`, which converts e2fsprogs' way, reads the
 * access ACL of `path` in `img` as the `len` bytes of `want`. The id of an
 * entry without one is not compared, since e2fsprogs may write 0 there,
 * where Linux writes ACL_UNDEFINED_ID. */
static int acl_debugfs_reads(const char *img, const char *path, const unsigned char *want,
                             uint32_t len)
{
    char request[512];
    unsigned char got[256];
    unsigned long n, at;
    void *f;

    snprintf(request, sizeof(request), "ea_get -f %s %s " ACL_A, fix_path("acl.out"), path);
    if (!ext2_fixture_debugfs(img, request, 0, fix_path("debugfs.txt")))
        return 0;
    f = fopen(fix_path("acl.out"), "rb");
    if (!f)
        return 0;
    n = fread(got, 1, sizeof(got), f);
    fclose(f);
    for (at = 4; at + 8 <= n; at += 8)
        if (got[at] != 0x02 && got[at] != 0x08)
            memset(got + at + 4, 0xFF, 4);
    return n == len && memcmp(got, want, len) == 0;
}

/**
 * Mount a volume with ACLs and require the rows above, then that e2fsck
 * finds it clean, that e2fsprogs reads the ACLs written here as they were
 * set, and that an ACL e2fsprogs writes reads here as it was given.
 */
static void acl_rows(void)
{
    const char *img = fix_path("acl.ext2");
    uint64_t bytes = build_image(img, 4096, 128, ATTR_PROFILE, 16, "tree");
    unsigned char v[256];
    uint32_t len = acl_named(v, 6, 7, 5, 6, 0);
    void *f, *n;
    int rc;

    use_image(img, bytes);
    check(mount_rw() == 0, "the ACL volume mounts");
    if (!mnt.root)
        return;
    mnt.sb_posixacl = 1;
    acl_fs.umask = 022;
    proc_a.fs = &acl_fs;
    acl_access_rows();
    acl_inherit_rows();
    acl_refusal_rows();
    acl_as(0, 0);
    proc_a.fs = 0;
    check(unmount_rw() == 0 && e2fsck_clean(img), "e2fsck finds the volume with ACLs clean");
    check(acl_debugfs_reads(img, "/dir/inherit.txt", v, len),
          "e2fsprogs reads an inherited ACL as the driver gave it");
    len = acl_named(v, 6, 4, 4, 6, 0);
    f = fopen(fix_path("acl.in"), "wb");
    if (f) {
        fwrite(v, 1, len, f);
        fclose(f);
    }
    {
        char script[512];

        snprintf(script, sizeof(script), "ea_set -f %s /many/file1.txt " ACL_A "\n",
                 fix_path("acl.in"));
        check(f && debugfs_script(img, script), "e2fsprogs writes an access ACL");
    }
    use_image(img, bytes);
    check(mount_rw() == 0, "the volume mounts again");
    if (!mnt.root)
        return;
    mnt.sb_posixacl = 1;
    n = walk("/many/file1.txt", &rc);
    check(n && acl_reads(n, ACL_A, v, len), "the driver reads e2fsprogs' ACL as it was given");
    if (n)
        OPS->put(&mnt, n);
    check(unmount_rw() == 0, "the volume unmounts");
}
#endif
