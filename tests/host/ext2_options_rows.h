/* The mount options of Linux's ext2: what each takes and shows. */
#ifndef EXT2_OPTIONS_ROWS_H_
#define EXT2_OPTIONS_ROWS_H_

/* The options a row's `show_options` emitted, comma-separated. */
static char shown[256];

/** Append one emitted option to `shown`, as the mount table writes it. */
static void show_into(void *out, const char *name, uint64_t value, enum vfs_option_format f)
{
    size_t at = strlen(shown);

    (void)out;
    if (f == VFS_OPT_WORD) {
        snprintf(shown + at, sizeof(shown) - at, "%s%s", at ? "," : "", name);
    } else {
        snprintf(shown + at, sizeof(shown) - at, "%s%s=%lu", at ? "," : "", name,
                 (unsigned long)value);
    }
}

/** The mounted volume's options as the mount table shows them. */
static const char *options_shown(void)
{
    shown[0] = 0;
    if (mnt.show_options) {
        mnt.show_options(&mnt, show_into, NULL);
    }
    return shown;
}

/** Make a tree volume of 16 MiB and `block`-byte blocks whose
 * s_default_mount_opts are user_xattr and acl, as mke2fs.conf's default
 * makes them. */
static uint64_t options_image(const char *img, int block)
{
    uint64_t bytes = build_image(img, block, 128, PROFILE, 16, "tree");
    unsigned char defm[4] = {0x0c, 0, 0, 0};

    poke(img, 1024 + 256, defm, 4);
    return bytes;
}

/**
 * Every word of Linux's table is taken and each shown as Linux's
 * ext2_show_options shows it, in its order; an unknown word, a bad number
 * and a malformed leading `sb=` are EINVAL; `minixdf` counts every block in
 * statfs; `grpid` gives a new file its directory's group; `nouser_xattr`
 * refuses a user attribute; `nouid32` keeps
 * only 16 bits of an id on the medium; and `resuid=` and `resgid=` let
 * their user and group take the reserved blocks.
 */
static void option_word_rows(void)
{
    const char *img = fix_path("options.ext2");
    uint64_t bytes = options_image(img, 4096);
    struct vfs_statfs sf;
    struct vfs_stat st;
    struct ext2_mount *em;
    uint32_t plain_blocks = 0;
    void *n = 0;
    int err;

    use_image(img, bytes);
    check(mount_options(rw_flags, "bogus") == -EINVAL &&
              streq(last_warn, "refused: unrecognized mount option \"bogus\" or missing value"),
          "an unknown option is EINVAL by name");
    check(mount_options(rw_flags, "resuid=1x") == -EINVAL &&
              mount_options(rw_flags, "resuid=") == -EINVAL &&
              mount_options(rw_flags, "resgid=4294967295") == -EINVAL,
          "a malformed or out-of-range id is EINVAL");
    check(mount_options(rw_flags, "sb=1x,debug") == -EINVAL, "a malformed sb= is EINVAL");
    check(mount_rw() == 0 && streq(options_shown(), "errors=continue,user_xattr,acl"),
          "a volume of mke2fs's defaults shows its error policy and default options");
    if (mnt.root) {
        check(OPS->statfs(&mnt, &sf) == 0, "statfs answers");
        plain_blocks = (uint32_t)sf.blocks;
        check(unmount_rw() == 0, "the plain mount unmounts");
    }
    check(mount_options(rw_flags, ",debug,oldalloc,nobh,noquota,quota,grpquota,noreservation,,"
                                  "nouser_xattr,resuid=1000,resgid=0x64,sb=1,nouid32,minixdf,"
                                  "grpid,xip") == 0,
          "every word of Linux's table is taken");
    check(streq(options_shown(), "minixdf,grpid,resuid=1000,resgid=100,errors=continue,nouid32,"
                                 "debug,oldalloc,nouser_xattr,acl,usrquota,grpquota,xip,"
                                 "noreservation"),
          "the options show in ext2_show_options's order, dax turned off");
    if (!mnt.root) {
        return;
    }
    em = mnt.priv;
    check(OPS->statfs(&mnt, &sf) == 0 && sf.blocks == em->blocks && plain_blocks < em->blocks,
          "minixdf counts the volume's own blocks");
    proc_a.euid = 70000;
    proc_a.egid = 500;
    check(OPS->create(&mnt, mnt.root, "grp", 0100644, &n) == 0 && OPS->getattr(&mnt, n, &st) == 0 &&
              st.gid == 0,
          "grpid gives a new file its directory's group");
    if (n) {
        struct vfs_xattr xa = {VFS_XATTR_SET, "user.a", "v", 1, 0};
        struct vfs_xattr xl = {VFS_XATTR_LIST, NULL, NULL, 0, 0};

        check(OPS->xattr(&mnt, n, &xa) == -EOPNOTSUPP && OPS->xattr(&mnt, n, &xl) == 0,
              "nouser_xattr refuses a user attribute and lists none");
        OPS->put(&mnt, n);
    }
    proc_a.euid = 1000;
    proc_a.egid = 7;
    check(ext2_may_use_reserved(em), "resuid lets its user take the reserved blocks");
    proc_a.euid = 1001;
    check(!ext2_may_use_reserved(em), "and no other user");
    proc_a.egid = 100;
    check(ext2_may_use_reserved(em), "resgid lets its group take them");
    proc_a.euid = proc_a.egid = 0;
    check(OPS->remount(&mnt, rw_flags, "user_xattr,nogrpid,dax") == 0 &&
              streq(options_shown(), "minixdf,resuid=1000,resgid=100,errors=continue,nouid32,"
                                     "debug,oldalloc,user_xattr,acl,usrquota,grpquota,xip,"
                                     "noreservation"),
          "a remount starts from the options in force and refuses a change of dax");
    check(OPS->remount(&mnt, rw_flags, "noacl,frob") == -EINVAL &&
              streq(options_shown(), "minixdf,resuid=1000,resgid=100,errors=continue,nouid32,"
                                     "debug,oldalloc,user_xattr,acl,usrquota,grpquota,xip,"
                                     "noreservation"),
          "a remount with an unknown option changes nothing");
    check(unmount_rw() == 0, "the options volume unmounts");
    check(mount_rw() == 0, "and mounts again without options");
    n = walk("/grp", &err);
    check(n && OPS->getattr(&mnt, n, &st) == 0 && st.uid == 65534,
          "nouid32 stored the overflow id in 16 bits and nothing above them");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_rw();
}

/**
 * `sb=` mounts from a backup superblock, here group 1's on a 1 KiB volume,
 * block 8193, when the primary's magic is gone, which a mount without it
 * refuses; the mount table shows it.
 */
static void option_sb_rows(void)
{
    const char *img = fix_path("options1k.ext2");
    uint64_t bytes = options_image(img, 1024);
    unsigned char zero[2] = {0, 0};
    void *n;
    int err;

    poke(img, 1024 + 56, zero, 2);
    use_image(img, bytes);
    check(mount_ro() == -EINVAL, "a volume whose primary superblock is gone does not mount");
    check(mount_options(VFS_MS_RDONLY, "sb=8193,debug") == 0 &&
              strstr(options_shown(), "sb=8193,") == shown,
          "sb= mounts from group 1's backup and shows it");
    n = mnt.root ? walk("/hello.txt", &err) : 0;
    check(n != 0, "and finds the tree through the backup's descriptors");
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
}

/** Run the option rows. */
static void option_rows(void)
{
    option_word_rows();
    option_sb_rows();
}

#endif
