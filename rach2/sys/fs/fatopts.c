/*
 * fatopts.c: the mount options of vfat and exfat, as fatopts.h says.
 */

#include "fatopts.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/ns.h"
#include "../proc/process.h"
#include "../rtc.h"

/** Whether the text from `key` to `end` is exactly `word`. */
static int key_is(const char *key, const char *end, const char *word)
{
    while (key < end && *word && *key == *word) {
        key++;
        word++;
    }
    return key == end && *word == '\0';
}

/* How an option's value is read, as Linux's fs_parser types read it. */
enum fat_kind {
    FK_FLAG,  /* no value */
    FK_BOOL,  /* no value, or one of `fat_bool`, Linux's bool_names */
    FK_U32,   /* kstrtouint, base 0 */
    FK_OCT,   /* kstrtouint, base 8 */
    FK_S32,   /* kstrtoint, base 0 */
    FK_ENUM,  /* a word of the option's table */
    FK_STRING /* any value */
};

/* What an option sets. */
enum fat_what {
    FW_FLAG, /* `bit` of `flags` */
    FW_UID,
    FW_GID,
    FW_UMASK,
    FW_DMASK,
    FW_FMASK,
    FW_ALLOW_UTIME,
    FW_CODEPAGE,
    FW_IOCHARSET,
    FW_TIME_OFFSET,
    FW_TZ,
    FW_CHECK,
    FW_ERRORS,
    FW_NFS,
    FW_SHORTNAME,
    FW_NOTHING /* deprecated, and taken with a warning */
};

#define FT_VFAT  1 /* the option is vfat's */
#define FT_EXFAT 2 /* the option is exfat's */
#define FT_OLD   4 /* deprecated: taken with a warning */

/* One option of Linux's tables, fat_param_spec and vfat_param_spec
 * (fs/fat/inode.c) and exfat_parameters (fs/exfat/super.c). */
struct fat_word {
    const char *name;
    unsigned char kind;  /* enum fat_kind */
    unsigned char what;  /* enum fat_what */
    unsigned char types; /* FT_ */
    uint32_t bit;        /* FAT_OPT_ for FW_FLAG */
    const char *values;  /* FK_ENUM: the words, each ended by a space */
};

static const struct fat_word fat_words[] = {
    {"check", FK_ENUM, FW_CHECK, FT_VFAT, 0, "relaxed r strict s normal n "},
    {"uid", FK_U32, FW_UID, FT_VFAT | FT_EXFAT, 0, 0},
    {"gid", FK_U32, FW_GID, FT_VFAT | FT_EXFAT, 0, 0},
    {"umask", FK_OCT, FW_UMASK, FT_VFAT | FT_EXFAT, 0, 0},
    {"dmask", FK_OCT, FW_DMASK, FT_VFAT | FT_EXFAT, 0, 0},
    {"fmask", FK_OCT, FW_FMASK, FT_VFAT | FT_EXFAT, 0, 0},
    {"allow_utime", FK_OCT, FW_ALLOW_UTIME, FT_VFAT | FT_EXFAT, 0, 0},
    {"codepage", FK_U32, FW_CODEPAGE, FT_VFAT, 0, 0},
    {"usefree", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_USEFREE, 0},
    {"nocase", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_NOCASE, 0},
    {"quiet", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_QUIET, 0},
    {"showexec", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_SHOWEXEC, 0},
    {"debug", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_DEBUG, 0},
    {"sys_immutable", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_SYS_IMMUTABLE, 0},
    {"flush", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_FLUSH, 0},
    {"tz", FK_ENUM, FW_TZ, FT_VFAT, 0, "UTC "},
    {"time_offset", FK_S32, FW_TIME_OFFSET, FT_VFAT | FT_EXFAT, 0, 0},
    {"errors", FK_ENUM, FW_ERRORS, FT_VFAT | FT_EXFAT, 0, "continue panic remount-ro "},
    {"discard", FK_FLAG, FW_FLAG, FT_VFAT | FT_EXFAT, FAT_OPT_DISCARD, 0},
    {"nfs", FK_FLAG, FW_NFS, FT_VFAT, 0, 0},
    {"nfs", FK_ENUM, FW_NFS, FT_VFAT, 0, "stale_rw nostale_ro "},
    {"dos1xfloppy", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_DOS1XFLOPPY, 0},
    {"conv", FK_ENUM, FW_NOTHING, FT_VFAT | FT_OLD, 0, "binary text mtext auto b t m a "},
    {"fat", FK_U32, FW_NOTHING, FT_VFAT | FT_OLD, 0, 0},
    {"blocksize", FK_U32, FW_NOTHING, FT_VFAT | FT_OLD, 0, 0},
    {"cvf_format", FK_STRING, FW_NOTHING, FT_VFAT | FT_OLD, 0, 0},
    {"cvf_options", FK_STRING, FW_NOTHING, FT_VFAT | FT_OLD, 0, 0},
    {"posix", FK_FLAG, FW_NOTHING, FT_VFAT | FT_OLD, 0, 0},
    {"iocharset", FK_STRING, FW_IOCHARSET, FT_VFAT | FT_EXFAT, 0, 0},
    {"rodir", FK_FLAG, FW_FLAG, FT_VFAT, FAT_OPT_RODIR, 0},
    {"utf8", FK_BOOL, FW_FLAG, FT_VFAT, FAT_OPT_UTF8, 0},
    {"uni_xlate", FK_BOOL, FW_FLAG, FT_VFAT, FAT_OPT_UNI_XLATE, 0},
    {"nonumtail", FK_BOOL, FW_FLAG, FT_VFAT, FAT_OPT_NONUMTAIL, 0},
    {"shortname", FK_ENUM, FW_SHORTNAME, FT_VFAT, 0, "lower win95 winnt mixed "},
    {"keep_last_dots", FK_FLAG, FW_FLAG, FT_EXFAT, FAT_OPT_KEEP_LAST_DOTS, 0},
    {"sys_tz", FK_FLAG, FW_FLAG, FT_EXFAT, FAT_OPT_SYS_TZ, 0},
    {"zero_size_dir", FK_FLAG, FW_FLAG, FT_EXFAT, FAT_OPT_ZERO_SIZE_DIR, 0},
    {"utf8", FK_FLAG, FW_NOTHING, FT_EXFAT | FT_OLD, 0, 0},
    {"debug", FK_FLAG, FW_NOTHING, FT_EXFAT | FT_OLD, 0, 0},
    {"namecase", FK_U32, FW_NOTHING, FT_EXFAT | FT_OLD, 0, 0},
    {"codepage", FK_U32, FW_NOTHING, FT_EXFAT | FT_OLD, 0, 0},
};

/* Linux's bool_names, the true ones first. */
static const char fat_true[] = "1 true yes on ";
static const char fat_false[] = "0 false no off ";

/** The index, from 0, of the text from `v` to `end` among the words of
 * `list`, each ended by a space; -1 when it is none of them. */
static int word_index(const char *v, const char *end, const char *list)
{
    int i = 0;

    while (*list) {
        const char *w = list;

        while (*list != ' ') {
            list++;
        }
        if (list - w == end - v) {
            const char *a = w, *b = v;

            while (b < end && *a == *b) {
                a++;
                b++;
            }
            if (b == end) {
                return i;
            }
        }
        list++;
        i++;
    }
    return -1;
}

/**
 * Read the number from `v` to `end` as Linux's kstrtouint and kstrtoint
 * do: an optional sign, a minus only when `sign` is set, then digits in
 * `base`, or with `base` 0 in the base a leading `0x` or `0` names. 0 with
 * `*out` set, as the 32 bits of the value; -EINVAL for no digits, another
 * character, or a value past 32 bits, or past an int when `sign` is set.
 */
static int fat_number(const char *v, const char *end, int base, int sign, uint32_t *out)
{
    uint32_t value = 0, limit = sign ? 0x7FFFFFFFu : 0xFFFFFFFFu;
    int neg = 0;

    if (v < end && (*v == '+' || (sign && *v == '-'))) {
        neg = *v == '-';
        v++;
    }
    if (base == 0) {
        base = 10;
        if (end - v > 1 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
            base = 16;
            v += 2;
        } else if (v < end && v[0] == '0') {
            base = 8;
        }
    }
    if (v == end) {
        return -EINVAL;
    }
    if (neg) {
        limit++;
    }
    for (; v < end; v++) {
        uint32_t d = (uint32_t)(*v - '0');

        if (*v >= 'a' && *v <= 'f') {
            d = (uint32_t)(*v - 'a' + 10);
        } else if (*v >= 'A' && *v <= 'F') {
            d = (uint32_t)(*v - 'A' + 10);
        }
        if (*v < '0' || d >= (uint32_t)base || value > (limit - d) / (uint32_t)base) {
            return -EINVAL;
        }
        value = value * (uint32_t)base + d;
    }
    *out = neg ? 0u - value : value;
    return 0;
}

/** Set `*o` to Linux's defaults for a mount the caller makes
 * (fat_init_fs_context, exfat_init_fs_context). */
static void fat_defaults(struct fat_options *o)
{
    const process_t *p = current_proc;

    k_memset(o, 0, sizeof(*o));
    o->uid = p ? p->ruid : 0;
    o->gid = p ? p->rgid : 0;
    o->fmask = o->dmask = p && p->fs ? p->fs->umask : 022u;
    o->allow_utime = 0xFFFFFFFFu; /* set from dmask at the end */
    o->codepage = 437;
    o->errors = FAT_ERRORS_RO;
    o->check = 'n';
    k_strncpy(o->shortname, "mixed", sizeof(o->shortname));
}

/**
 * Read the value of the option `w`, from `v` to `end`, NULL when it has
 * none, as its kind says: a number into `*n`, and the index of an enum's
 * word, or a bool's truth, which a bare bool has, into `*i`. 0, or -EINVAL
 * for a value the kind refuses or a missing one.
 */
static int fat_value(const struct fat_word *w, const char *v, const char *end, uint32_t *n, int *i)
{
    if (w->kind == FK_FLAG ? v != NULL : (v == NULL && w->kind != FK_BOOL)) {
        return -EINVAL;
    }
    if (w->kind == FK_U32 || w->kind == FK_OCT || w->kind == FK_S32) {
        return fat_number(v, end, w->kind == FK_OCT ? 8 : 0, w->kind == FK_S32, n);
    }
    if (w->kind == FK_ENUM) {
        *i = word_index(v, end, w->values);
    } else if (w->kind == FK_BOOL && v) {
        *i = word_index(v, end, fat_true) >= 0 ? 1 : word_index(v, end, fat_false) >= 0 ? 0 : -1;
    } else if (w->kind == FK_BOOL) {
        *i = 1;
    }
    return *i < 0 ? -EINVAL : 0;
}

/**
 * Apply the option `w`, whose value runs from `v` to `end`, NULL when it
 * has none, to `*o`. 0, or -EINVAL for a value its kind refuses, an
 * unmapped id or a time offset past a day.
 */
static int fat_apply(const struct fat_word *w, const char *v, const char *end,
                     struct fat_options *o)
{
    uint32_t n = 0;
    int64_t k;
    int i = 0;

    if (fat_value(w, v, end, &n, &i) != 0) {
        return -EINVAL;
    }
    switch (w->what) {
    case FW_FLAG:
        o->flags = w->kind == FK_BOOL && !i ? o->flags & ~w->bit : o->flags | w->bit;
        break;
    case FW_UID:
    case FW_GID:
        k = w->what == FW_UID ? ns_uid_to_kernel(proc_userns(current_proc), n)
                              : ns_gid_to_kernel(proc_userns(current_proc), n);
        if (k < 0) {
            return -EINVAL;
        }
        *(w->what == FW_UID ? &o->uid : &o->gid) = (uint32_t)k;
        break;
    case FW_UMASK:
        o->fmask = o->dmask = n & 0xFFFFu;
        break;
    case FW_DMASK:
        o->dmask = n & 0xFFFFu;
        break;
    case FW_FMASK:
        o->fmask = n & 0xFFFFu;
        break;
    case FW_ALLOW_UTIME:
        o->allow_utime = n & 022u;
        break;
    case FW_CODEPAGE:
        o->codepage = n;
        break;
    case FW_IOCHARSET:
        return key_is(v, end, "utf8") ? 0 : -EINVAL;
    case FW_TIME_OFFSET:
    case FW_TZ:
        if (w->what == FW_TIME_OFFSET && ((int32_t)n < -24 * 60 || (int32_t)n > 24 * 60)) {
            return -EINVAL;
        }
        o->time_offset = w->what == FW_TZ ? 0 : (int32_t)n;
        o->flags |= FAT_OPT_TZ_SET;
        break;
    case FW_CHECK:
        o->check = "rrssnn"[i];
        break;
    case FW_ERRORS:
        o->errors = i + 1;
        break;
    case FW_NFS:
        o->nfs = i + 1;
        break;
    case FW_SHORTNAME:
        k_memcpy(o->shortname, v, (size_t)(end - v));
        o->shortname[end - v] = 0;
        break;
    default:
        break;
    }
    return 0;
}

/** The entry of `fat_words` named from `key` to `end` among the options of
 * the type `mine`, FT_VFAT or FT_EXFAT, as Linux's fs_lookup_key finds it:
 * of two, the one that takes a value when `valued` says one is given, and
 * none when none is. NULL for a name the type does not have. */
static const struct fat_word *fat_lookup(const char *key, const char *end, int valued,
                                         unsigned char mine)
{
    const struct fat_word *w = NULL;
    size_t i;

    for (i = 0; i < sizeof(fat_words) / sizeof(fat_words[0]); i++) {
        if ((fat_words[i].types & mine) && key_is(key, end, fat_words[i].name) &&
            (!w || (fat_words[i].kind == FK_FLAG) == !valued)) {
            w = &fat_words[i];
        }
    }
    return w;
}

/** Log the refusal of the option named from `key` to `end`: of its value
 * when `known`, else of the name. */
static void fat_refuse(const char *type, const char *key, const char *end, int known)
{
    char name[24];
    size_t len = (size_t)(end - key) < sizeof(name) - 1 ? (size_t)(end - key) : sizeof(name) - 1;

    k_memcpy(name, key, len);
    name[len] = 0;
    klog(KLOG_WARN, type, known ? "refused: bad value for '%s'" : "refused: unknown parameter '%s'",
         name);
}

/** Settle what the options of `o` leave to the end, as Linux's parsers and
 * fat_fill_super do: `allow_utime` from `dmask` when none was given,
 * `utf8` dropped under `uni_xlate`, and exfat's stamps in `time_offset`
 * unless `sys_tz`. 0, or -EINVAL for a vfat code page other than 437,
 * which load_nls would not find. */
static int fat_finish(struct fat_options *o, int exfat)
{
    if (o->allow_utime == 0xFFFFFFFFu) {
        o->allow_utime = ~o->dmask & 022u;
    }
    if (o->flags & FAT_OPT_UNI_XLATE) {
        o->flags &= ~FAT_OPT_UTF8;
    }
    if (exfat && !(o->flags & FAT_OPT_SYS_TZ)) {
        o->flags |= FAT_OPT_TZ_SET; /* exfat reads time_offset, 0 by default */
    }
    if (!exfat && o->codepage != 437) {
        klog(KLOG_WARN, "vfat", "refused: codepage cp%d not found", (int)o->codepage);
        return -EINVAL;
    }
    return 0;
}

int fat_parse_options(const char *options, int exfat, struct fat_options *o)
{
    const char *p = options ? options : "";
    const char *type = exfat ? "exfat" : "vfat";

    fat_defaults(o);
    while (*p) {
        const char *key = p, *keyend, *v = NULL, *end;
        const struct fat_word *w;
        int rc;

        while (*p && *p != ',' && *p != '=') {
            p++;
        }
        keyend = p;
        if (*p == '=') {
            v = ++p;
            while (*p && *p != ',') {
                p++;
            }
        }
        end = p;
        if (*p == ',') {
            p++;
        }
        if (key == keyend && !v) {
            continue; /* an empty item, as in a trailing comma */
        }
        if (key_is(key, keyend, "ro") && !v) {
            continue; /* the mount flag given as data */
        }
        w = fat_lookup(key, keyend, v != NULL, exfat ? FT_EXFAT : FT_VFAT);
        rc = w ? fat_apply(w, v, end, o) : -EINVAL;
        if (rc != 0) {
            fat_refuse(type, key, keyend, w != NULL);
            return rc;
        }
        if (w->types & FT_OLD) {
            klog(KLOG_WARN, type, "Deprecated parameter '%s'", w->name);
        }
    }
    return fat_finish(o, exfat);
}

/** Emit `name=value`, both short words, as one option. */
static void emit_pair(vfs_option_emit emit, void *out, const char *name, const char *value)
{
    char buf[40];
    size_t at = 0;

    while (*name && at < sizeof(buf) - 2) {
        buf[at++] = *name++;
    }
    buf[at++] = '=';
    while (*value && at < sizeof(buf) - 1) {
        buf[at++] = *value++;
    }
    buf[at] = 0;
    emit(out, buf, 0, VFS_OPT_WORD);
}

/** Emit `time_offset=N`, N in signed decimal. */
static void emit_offset(vfs_option_emit emit, void *out, int32_t minutes)
{
    char digits[12];
    char *d = digits + sizeof(digits) - 1;
    uint32_t m = minutes < 0 ? 0u - (uint32_t)minutes : (uint32_t)minutes;

    *d = 0;
    do {
        *--d = (char)('0' + m % 10);
        m /= 10;
    } while (m);
    if (minutes < 0) {
        *--d = '-';
    }
    emit_pair(emit, out, "time_offset", d);
}

/** Emit the options vfat and exfat both show first: the owner, the masks,
 * `allow_utime` and the I/O character set, with vfat's code page between. */
static void show_head(const struct fat_options *o, int exfat, vfs_option_emit emit, void *out)
{
    if (o->uid != 0) {
        emit(out, "uid", o->uid, VFS_OPT_DEC);
    }
    if (o->gid != 0) {
        emit(out, "gid", o->gid, VFS_OPT_DEC);
    }
    emit(out, "fmask", o->fmask, VFS_OPT_OCT4);
    emit(out, "dmask", o->dmask, VFS_OPT_OCT4);
    if (o->allow_utime) {
        emit(out, "allow_utime", o->allow_utime, VFS_OPT_OCT4);
    }
    if (!exfat) {
        emit(out, "codepage", o->codepage, VFS_OPT_DEC);
    }
    emit(out, "iocharset=utf8", 0, VFS_OPT_WORD);
}

/** Emit the flag of `o` named `name` when `bit` is set. */
static void show_flag(const struct fat_options *o, uint32_t bit, const char *name,
                      vfs_option_emit emit, void *out)
{
    if (o->flags & bit) {
        emit(out, name, 0, VFS_OPT_WORD);
    }
}

void fat_show_options(const struct fat_options *o, int exfat, vfs_option_emit emit, void *out)
{
    static const char *const errors[] = {"errors=remount-ro", "errors=continue", "errors=panic"};
    const char *policy = errors[o->errors == FAT_ERRORS_CONT    ? 1
                                : o->errors == FAT_ERRORS_PANIC ? 2
                                                                : 0];
    char check[2];

    show_head(o, exfat, emit, out);
    if (exfat) {
        emit(out, policy, 0, VFS_OPT_WORD);
        show_flag(o, FAT_OPT_DISCARD, "discard", emit, out);
        show_flag(o, FAT_OPT_KEEP_LAST_DOTS, "keep_last_dots", emit, out);
        if (o->flags & FAT_OPT_SYS_TZ) {
            emit(out, "sys_tz", 0, VFS_OPT_WORD);
        } else if (o->time_offset) {
            emit_offset(emit, out, o->time_offset);
        }
        show_flag(o, FAT_OPT_ZERO_SIZE_DIR, "zero_size_dir", emit, out);
        return;
    }
    emit_pair(emit, out, "shortname", o->shortname);
    if (o->check != 'n') {
        check[0] = o->check;
        check[1] = 0;
        emit_pair(emit, out, "check", check);
    }
    show_flag(o, FAT_OPT_USEFREE, "usefree", emit, out);
    show_flag(o, FAT_OPT_QUIET, "quiet", emit, out);
    show_flag(o, FAT_OPT_SHOWEXEC, "showexec", emit, out);
    show_flag(o, FAT_OPT_SYS_IMMUTABLE, "sys_immutable", emit, out);
    show_flag(o, FAT_OPT_UTF8, "utf8", emit, out);
    show_flag(o, FAT_OPT_UNI_XLATE, "uni_xlate", emit, out);
    show_flag(o, FAT_OPT_NONUMTAIL, "nonumtail", emit, out);
    show_flag(o, FAT_OPT_RODIR, "rodir", emit, out);
    show_flag(o, FAT_OPT_FLUSH, "flush", emit, out);
    if ((o->flags & FAT_OPT_TZ_SET) && o->time_offset) {
        emit_offset(emit, out, o->time_offset);
    } else if (o->flags & FAT_OPT_TZ_SET) {
        emit(out, "tz=UTC", 0, VFS_OPT_WORD);
    }
    emit(out, policy, 0, VFS_OPT_WORD);
    if (o->nfs) {
        emit(out, o->nfs == FAT_NFS_NOSTALE_RO ? "nfs=nostale_ro" : "nfs=stale_rw", 0,
             VFS_OPT_WORD);
    }
    show_flag(o, FAT_OPT_DISCARD, "discard", emit, out);
    show_flag(o, FAT_OPT_DOS1XFLOPPY, "dos1xfloppy", emit, out);
}

int32_t fat_minutes_east(const struct fat_options *o)
{
    return o->flags & FAT_OPT_TZ_SET ? o->time_offset : -rtc_minuteswest();
}
