#ifndef _FATOPTS_H_
#define _FATOPTS_H_

/*
 * fatopts: the mount options of vfat and exfat, as Linux 6.12 takes and
 * shows them, and the zone their stamps are read in.
 */

#include "../lib/kint.h"
#include "vfs.h"

/* The bits of `fat_options.flags`, each one of Linux's flag options. */
#define FAT_OPT_QUIET          0x1u  /* vfat: a refused chown or chmod succeeds */
#define FAT_OPT_SHOWEXEC       0x2u  /* vfat: execute bits for EXE, COM and BAT */
#define FAT_OPT_SYS_IMMUTABLE  0x4u  /* vfat */
#define FAT_OPT_FLUSH          0x8u  /* vfat */
#define FAT_OPT_TZ_SET         0x10u /* the stamps are in time_offset */
#define FAT_OPT_DISCARD        0x20u
#define FAT_OPT_DOS1XFLOPPY    0x40u    /* vfat */
#define FAT_OPT_USEFREE        0x80u    /* vfat */
#define FAT_OPT_DEBUG          0x100u   /* vfat */
#define FAT_OPT_RODIR          0x200u   /* vfat: a directory keeps the read-only bit */
#define FAT_OPT_UTF8           0x400u   /* vfat */
#define FAT_OPT_UNI_XLATE      0x800u   /* vfat */
#define FAT_OPT_NONUMTAIL      0x1000u  /* vfat */
#define FAT_OPT_NOCASE         0x2000u  /* vfat */
#define FAT_OPT_KEEP_LAST_DOTS 0x4000u  /* exfat */
#define FAT_OPT_SYS_TZ         0x8000u  /* exfat: the stamps are in sys_tz */
#define FAT_OPT_ZERO_SIZE_DIR  0x10000u /* exfat */

/* `fat_options.errors`, Linux's FAT_ERRORS_ and EXFAT_ERRORS_. */
#define FAT_ERRORS_CONT  1
#define FAT_ERRORS_PANIC 2
#define FAT_ERRORS_RO    3

/* `fat_options.nfs`, Linux's FAT_NFS_. */
#define FAT_NFS_STALE_RW   1
#define FAT_NFS_NOSTALE_RO 2

/* The options of a vfat or an exfat mount, Linux's fat_mount_options and
 * exfat_mount_options. */
struct fat_options {
    uint32_t uid, gid;     /* the owner of every entry, kernel ids */
    uint32_t fmask, dmask; /* the permission bits files and directories lack */
    uint32_t allow_utime;  /* 0020 and 0002: who besides the owner sets times */
    int32_t time_offset;   /* minutes east of UTC of the volume's stamps */
    uint32_t flags;        /* FAT_OPT_ */
    uint32_t codepage;     /* vfat: the short names' code page, 437 */
    int errors;            /* FAT_ERRORS_ */
    int nfs;               /* FAT_NFS_, or 0 */
    char check;            /* vfat: 'r', 'n' or 's' */
    char shortname[6];     /* vfat: "lower", "win95", "winnt" or "mixed" */
};

/**
 * Parse `options`, mount(2)'s data or NULL, into `*o`, the options of a
 * vfat mount, or of an exfat one when `exfat` is set, as Linux 6.12's
 * fat_parse_param and exfat_parse_param take them. The defaults are
 * Linux's: the caller's user and group, its umask for both masks, an
 * `allow_utime` of the group and other write bits that `dmask` leaves,
 * `errors=remount-ro`, and on vfat code page 437, `iocharset=utf8`,
 * `shortname=mixed` and `check=normal`. An empty item is skipped, and `ro`
 * is the mount flag given as data. A deprecated option of Linux's table is
 * taken with the record `TYPE: Deprecated parameter 'NAME'`.
 *
 * @return 0; -EINVAL for an option the type does not have, a bad value, an
 *         id the caller's user namespace does not map, a time offset past a
 *         day, or a code page or I/O character set other than 437 and utf8.
 */
int fat_parse_options(const char *options, int exfat, struct fat_options *o);

/** Emit the options `o` of a vfat mount, or of an exfat one when `exfat`
 * is set, as Linux's fat_show_options and exfat_show_options write them,
 * in their order. */
void fat_show_options(const struct fat_options *o, int exfat, vfs_option_emit emit, void *out);

/** The minutes east of UTC that the volume's stamps of `o` are in, as
 * Linux's fat_tz_offset and exfat_tz_offset read them: `time_offset` under
 * FAT_OPT_TZ_SET, which vfat's tz= and time_offset= set and exfat has
 * unless sys_tz is given, else the time zone settimeofday last set. */
int32_t fat_minutes_east(const struct fat_options *o);

#endif
