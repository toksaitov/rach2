/* The mount options of Linux's vfat and exfat: what each takes, shows and
 * changes. */
#ifndef FAT_OPTIONS_ROWS_H_
#define FAT_OPTIONS_ROWS_H_

/* The minutes west of UTC that `rtc_minuteswest` reports, settimeofday's
 * zone. */
static int32_t tz_west;

/** The zone settimeofday last set, as the rows set it. */
int32_t rtc_minuteswest(void)
{
    return tz_west;
}

/* The options a row's `fat_show_options` emitted, comma-separated. */
static char fat_shown[512];

/** Append one emitted option to `fat_shown`, as the mount table writes it. */
static void fat_show_into(void *out, const char *name, uint64_t value, enum vfs_option_format f)
{
    size_t at = strlen(fat_shown);
    const char *sep = at ? "," : "";

    (void)out;
    if (f == VFS_OPT_WORD) {
        snprintf(fat_shown + at, sizeof(fat_shown) - at, "%s%s", sep, name);
    } else if (f == VFS_OPT_OCT4) {
        snprintf(fat_shown + at, sizeof(fat_shown) - at, "%s%s=%04lo", sep, name,
                 (unsigned long)value);
    } else {
        snprintf(fat_shown + at, sizeof(fat_shown) - at, "%s%s=%lu", sep, name,
                 (unsigned long)value);
    }
}

/** The options `options` parse to on vfat, or exfat when `exfat` is set,
 * as the mount table shows them, or "EINVAL" when they are refused. */
static const char *fat_parsed(const char *options, int exfat)
{
    struct fat_options o;

    fat_shown[0] = 0;
    if (fat_parse_options(options, exfat, &o) != 0) {
        return "EINVAL";
    }
    fat_show_options(&o, exfat, fat_show_into, NULL);
    return fat_shown;
}

/** The minutes east of UTC the options `options` read the stamps in. */
static int32_t fat_east(const char *options, int exfat)
{
    struct fat_options o;

    return fat_parse_options(options, exfat, &o) == 0 ? fat_minutes_east(&o) : 9999;
}

/**
 * Every word of Linux 6.12's vfat and exfat tables is taken, a word of the
 * other type's or none is EINVAL, and each shows as Linux's
 * fat_show_options and exfat_show_options show it, in their order.
 */
static void fat_option_word_rows(void)
{
    struct fat_options o;

    proc_a.ruid = 1000;
    proc_a.rgid = 100;
    check(fat_parse_options(NULL, 0, &o) == 0 && o.uid == 1000 && o.gid == 100 && o.fmask == 022 &&
              o.dmask == 022,
          "the defaults are the caller's ids and umask, as Linux's fat_init_fs_context");
    proc_a.ruid = proc_a.rgid = 0;
    check(streq(fat_parsed("", 0),
                "fmask=0022,dmask=0022,codepage=437,iocharset=utf8,shortname=mixed,"
                "errors=remount-ro"),
          "vfat shows Linux's defaults");
    check(streq(fat_parsed("uid=7,gid=0x9,umask=0,fmask=0133,check=s,usefree,quiet,showexec,"
                           "sys_immutable,utf8,nonumtail=yes,rodir,flush,time_offset=-300,"
                           "errors=continue,nfs,discard,dos1xfloppy,shortname=winnt,debug,"
                           "nocase,codepage=437,iocharset=utf8,uni_xlate=0,,ro",
                           0),
                "uid=7,gid=9,fmask=0133,dmask=0000,allow_utime=0022,codepage=437,"
                "iocharset=utf8,shortname=winnt,check=s,usefree,quiet,showexec,"
                "sys_immutable,utf8,nonumtail,rodir,flush,time_offset=-300,errors=continue,"
                "nfs=stale_rw,discard,dos1xfloppy"),
          "every vfat word is taken and shown in fat_show_options's order");
    check(streq(fat_parsed("tz=UTC,nfs=nostale_ro,allow_utime=0777,uni_xlate,utf8", 0),
                "fmask=0022,dmask=0022,allow_utime=0022,codepage=437,iocharset=utf8,"
                "shortname=mixed,uni_xlate,tz=UTC,errors=remount-ro,nfs=nostale_ro"),
          "tz=UTC, nfs=nostale_ro and allow_utime's two bits show, and uni_xlate drops utf8");
    check(streq(fat_parsed("conv=binary,posix,fat=16,blocksize=512,cvf_format=x", 0),
                "fmask=0022,dmask=0022,codepage=437,iocharset=utf8,shortname=mixed,"
                "errors=remount-ro"),
          "vfat's deprecated options are taken and change nothing");
    check(streq(fat_parsed("nosuch", 0), "EINVAL") && streq(fat_parsed("uid=", 0), "EINVAL") &&
              streq(fat_parsed("uid=1x", 0), "EINVAL") &&
              streq(fat_parsed("uid=4294967296", 0), "EINVAL") &&
              streq(fat_parsed("quiet=1", 0), "EINVAL") &&
              streq(fat_parsed("check=x", 0), "EINVAL") &&
              streq(fat_parsed("utf8=maybe", 0), "EINVAL") &&
              streq(fat_parsed("time_offset=1441", 0), "EINVAL") &&
              streq(fat_parsed("time_offset=-1441", 0), "EINVAL") &&
              streq(fat_parsed("codepage=850", 0), "EINVAL") &&
              streq(fat_parsed("iocharset=iso8859-1", 0), "EINVAL") &&
              streq(fat_parsed("conv=bogus", 0), "EINVAL") &&
              streq(fat_parsed("keep_last_dots", 0), "EINVAL") &&
              streq(fat_parsed("ro=1", 0), "EINVAL"),
          "an unknown vfat word, a bad value or a missing character set is EINVAL");
    check(streq(fat_parsed("", 1), "fmask=0022,dmask=0022,iocharset=utf8,errors=remount-ro"),
          "exfat shows Linux's defaults");
    check(streq(fat_parsed("uid=5,umask=077,dmask=0,allow_utime=2,iocharset=utf8,errors=panic,"
                           "discard,keep_last_dots,sys_tz,time_offset=60,zero_size_dir,utf8,"
                           "debug,namecase=1,codepage=437",
                           1),
                "uid=5,fmask=0077,dmask=0000,allow_utime=0002,iocharset=utf8,errors=panic,"
                "discard,keep_last_dots,sys_tz,zero_size_dir"),
          "every exfat word is taken and shown in exfat_show_options's order");
    check(streq(fat_parsed("time_offset=-90", 1),
                "fmask=0022,dmask=0022,iocharset=utf8,errors=remount-ro,time_offset=-90"),
          "exfat shows a time offset without sys_tz");
    check(streq(fat_parsed("showexec", 1), "EINVAL") && streq(fat_parsed("tz=UTC", 1), "EINVAL") &&
              streq(fat_parsed("namecase", 1), "EINVAL"),
          "a vfat word or a deprecated word without its value is EINVAL on exfat");
    tz_west = -360;
    check(fat_east("", 0) == 360 && fat_east("tz=UTC", 0) == 0 &&
              fat_east("time_offset=-60", 0) == -60 && fat_east("", 1) == 0 &&
              fat_east("sys_tz", 1) == 360 && fat_east("time_offset=30", 1) == 30,
          "vfat reads stamps in settimeofday's zone unless tz= or time_offset= is given, "
          "and exfat in time_offset unless sys_tz is");
    tz_west = 0;
}

/** Create the file `name` in the root of the mounted volume. 0 or -errno. */
static int fat_touch(const char *name)
{
    void *n;
    int rc = OPS->create(&mnt, mnt.root, name, 0644, &n);

    if (rc == 0) {
        OPS->put(&mnt, n);
    }
    return rc;
}

/** The permission bits of `path`, or 07777 when it cannot be read. */
static uint32_t fat_perm(const char *path)
{
    struct vfs_stat st;
    int err;
    void *n = walk(path, &err);
    uint32_t perm = 07777;

    if (n && OPS->getattr(&mnt, n, &st) == 0) {
        perm = st.mode & 07777;
    }
    if (n) {
        OPS->put(&mnt, n);
    }
    return perm;
}

/** chmod `path` to `mode` through the driver's setattr. Its result. */
static int fat_chmod(const char *path, uint32_t mode)
{
    struct vfs_stat st;
    int err, rc = -ENOENT;
    void *n = walk(path, &err);

    if (n) {
        __builtin_memset(&st, 0, sizeof(st));
        st.mode = mode;
        rc = OPS->setattr(&mnt, n, VFS_SET_MODE, &st);
        OPS->put(&mnt, n);
    }
    return rc;
}

/**
 * On a mounted volume: fmask and dmask apart, showexec, rodir, the chmod
 * Linux drops without an error, quiet, the stamps' zone, nfs=nostale_ro's
 * read-only mount, allow_utime on the mount, and the mount table's line.
 */
static void fat_option_mount_rows(void)
{
    struct vfs_stat st;
    int err;
    void *n;

    load("");
    check(mount_rw("fmask=0022,dmask=0077,showexec,rodir") == 0, "a mount with masks apart");
    check(fat_touch("RUN.EXE") == 0 && fat_touch("doc.txt") == 0 &&
              OPS->mkdir(&mnt, mnt.root, "dir", 0755) == 0,
          "a program, a document and a directory are made");
    check(fat_perm("/RUN.EXE") == 0755 && fat_perm("/doc.txt") == 0644 && fat_perm("/dir") == 0700,
          "fmask and dmask apply apart, and showexec leaves execute bits to EXE alone");
    check(fat_chmod("/doc.txt", 0755) == 0 && fat_perm("/doc.txt") == 0644,
          "a chmod the rule refuses succeeds and changes nothing, as on Linux");
    check(fat_chmod("/doc.txt", 0444) == 0 && fat_perm("/doc.txt") == 0444 &&
              fat_chmod("/doc.txt", 0644) == 0 && fat_perm("/doc.txt") == 0644,
          "the write bits go and come back through the read-only attribute");
    check(fat_chmod("/dir", 0500) == 0 && fat_perm("/dir") == 0500,
          "under rodir a directory takes the read-only attribute");
    check(fat_chmod("/doc.txt", 04644) == -EPERM, "a set-user-ID bit is EPERM");
    n = walk("/doc.txt", &err);
    if (n) {
        st.uid = 5;
        check(OPS->setattr(&mnt, n, VFS_SET_UID, &st) == -EPERM, "another owner is EPERM");
        OPS->put(&mnt, n);
    }
    tz_west = -360;
    n = walk("/doc.txt", &err);
    if (n) {
        st.mtime_sec = 1600000000;
        st.atime_sec = 1600000000;
        check(OPS->setattr(&mnt, n, VFS_SET_MTIME, &st) == 0 && OPS->getattr(&mnt, n, &st) == 0 &&
                  st.mtime_sec == 1600000000,
              "a stamp written in settimeofday's zone reads back");
        OPS->put(&mnt, n);
    }
    check(mnt.allow_utime == 0, "allow_utime follows a dmask that leaves no group write bit");
    unmount_image();

    check(mount_rw("quiet,tz=UTC,dmask=0,uid=3") == 0, "a mount with quiet and tz=UTC");
    check(fat_perm("/dir") == 0777, "without rodir a directory's read-only attribute is ignored");
    check(fat_chmod("/doc.txt", 04644) == 0 && fat_perm("/doc.txt") == 0755,
          "under quiet a set-user-ID bit is 0 and changes nothing");
    n = walk("/doc.txt", &err);
    if (n) {
        st.gid = 5;
        check(OPS->setattr(&mnt, n, VFS_SET_GID, &st) == 0, "and so is another group");
        check(OPS->getattr(&mnt, n, &st) == 0 && st.mtime_sec == 1600000000 + 6 * 3600,
              "tz=UTC reads the stamp written six hours east as six hours later");
        OPS->put(&mnt, n);
    }
    check(mnt.allow_utime == 022, "allow_utime follows dmask=0 to 0022");
    fat_shown[0] = 0;
    mnt.show_options(&mnt, fat_show_into, NULL);
    check(streq(fat_shown, "uid=3,fmask=0022,dmask=0000,allow_utime=0022,codepage=437,"
                           "iocharset=utf8,shortname=mixed,quiet,tz=UTC,errors=remount-ro"),
          "the mount table shows the mount's options");
    unmount_image();
    tz_west = 0;

    check(mount_rw("nfs=nostale_ro") == 0 && (mnt.flags & VFS_MS_RDONLY) &&
              fat_touch("NEW.TXT") == -EROFS,
          "nfs=nostale_ro mounts read-only, as Linux's fat_fill_super does");
    unmount_image();
    check(mount_rw("codepage=850") == -EINVAL, "a code page other than 437 fails the mount");
}

/**
 * sys_immutable: a system file is immutable, as Linux's fat_fill_inode sets
 * S_IMMUTABLE, which the VFS then enforces, and FAT_IOCTL_SET_ATTRIBUTES
 * sets or clears the system bit only with CAP_LINUX_IMMUTABLE; without the
 * option the bit is an attribute like any other.
 */
static void fat_option_immutable_rows(void)
{
    struct vfs_stat st;
    int err;
    void *n, *root;

    load("");
    check(mount_rw("sys_immutable") == 0 && fat_touch("SYS.BIN") == 0,
          "a mount with sys_immutable");
    n = walk("/SYS.BIN", &err);
    root = walk("/", &err);
    if (n && root) {
        check(fat_attr_get(&mnt, n) == 0x20 && fat_attr_get(&mnt, root) == 0x10,
              "a new file has the archive bit alone, and the root the directory bit");
        check(fat_attr_set(&mnt, n, 0x24, FAT_ATTR_MAY_CHANGE) == -EPERM,
              "the system bit needs CAP_LINUX_IMMUTABLE under sys_immutable");
        check(OPS->getattr(&mnt, n, &st) == 0 && st.flags_known == 0,
              "and a file without it reports no flags");
        check(fat_attr_set(&mnt, n, 0x24, FAT_ATTR_MAY_CHANGE | FAT_ATTR_MAY_SYS) == 0 &&
                  fat_attr_get(&mnt, n) == 0x24,
              "with it the system bit is stored");
        check(OPS->getattr(&mnt, n, &st) == 0 && st.flags_known == VFS_FLAGS_ENFORCED &&
                  st.flags == VFS_FL_IMMUTABLE,
              "a system file is immutable, enforced and not kept for FS_IOC_GETFLAGS");
        check(fat_attr_set(&mnt, n, 0x20, FAT_ATTR_MAY_CHANGE) == -EPERM &&
                  fat_attr_get(&mnt, n) == 0x24,
              "clearing the bit needs the capability too");
        check(fat_attr_set(&mnt, root, 0x02, FAT_ATTR_MAY_CHANGE | FAT_ATTR_MAY_SYS) == -EINVAL &&
                  fat_attr_set(&mnt, root, 0x10, FAT_ATTR_MAY_CHANGE) == 0,
              "the root takes the directory bit alone");
        check(fat_attr_set(&mnt, n, 0x21, FAT_ATTR_MAY_CHANGE | FAT_ATTR_MAY_SYS) == 0 &&
                  OPS->getattr(&mnt, n, &st) == 0 && st.flags_known == 0 && (st.mode & 0222) == 0 &&
                  fat_attr_get(&mnt, n) == 0x21,
              "clearing it ends the immutability, and the read-only bit takes the write bits");
    }
    if (root) {
        OPS->put(&mnt, root);
    }
    if (n) {
        OPS->put(&mnt, n);
    }
    unmount_image();
    check(mount_rw("") == 0, "a mount without sys_immutable");
    n = walk("/SYS.BIN", &err);
    if (n) {
        check(fat_attr_set(&mnt, n, 0x24, FAT_ATTR_MAY_CHANGE) == 0 &&
                  OPS->getattr(&mnt, n, &st) == 0 && st.flags_known == 0,
              "the system bit is anyone's to set and makes nothing immutable");
        check(fat_attr_set(&mnt, n, 0x20, 0) == -EPERM, "a caller who may not chmod is EPERM");
        OPS->put(&mnt, n);
    }
    unmount_image();
}

/**
 * A mask that leaves the mode no write bit makes no read-only attribute:
 * under fmask=0222, FAT_IOCTL_SET_ATTRIBUTES of the archive bit alone
 * stores it alone and FAT_IOCTL_GET_ATTRIBUTES reads it so, and a mount
 * without the mask finds the file writable, as Linux's fat_save_attrs and
 * fat_make_attrs keep the bit apart from a mode that cannot hold it.
 */
static void fat_option_mask_attr_rows(void)
{
    struct vfs_stat st;
    int err;
    void *n;

    load("");
    check(mount_rw("fmask=0222") == 0 && fat_touch("MASKED.TXT") == 0, "a mount with fmask=0222");
    n = walk("/MASKED.TXT", &err);
    if (n) {
        check(fat_attr_get(&mnt, n) == 0x20, "the mask makes no read-only bit for GET");
        check(fat_attr_set(&mnt, n, 0x20, FAT_ATTR_MAY_CHANGE) == 0 &&
                  fat_attr_get(&mnt, n) == 0x20,
              "SET of the archive bit alone stores it alone under the mask");
        OPS->put(&mnt, n);
    }
    unmount_image();
    check(mount_rw("") == 0, "a mount without the mask");
    n = walk("/MASKED.TXT", &err);
    if (n) {
        check(fat_attr_get(&mnt, n) == 0x20 && OPS->getattr(&mnt, n, &st) == 0 &&
                  (st.mode & 0222) != 0,
              "the file is writable without the mask");
        OPS->put(&mnt, n);
    }
    unmount_image();
}

/** Run the option rows. */
static void fat_option_rows(void)
{
    fat_option_word_rows();
    fat_option_mount_rows();
    fat_option_immutable_rows();
    fat_option_mask_attr_rows();
}

#endif
