/*
 * volume.c: the tooling machine's seed volume, a FAT16 file system, and its
 * job volume, a tar archive at the start of a raw disk, made and read with no
 * other program; etude-volume's commands run it.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#define _FILE_OFFSET_BITS 64

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#endif

#include "volume.h"

#define PATH_LEN 4096

/* The deepest tree a job volume takes from the host. */
#define DEPTH_MAX 64

/* The size of a tar block, and the largest size a ustar header's field
 * holds, 8 GiB less one byte. */
#define BLOCK     512
#define USTAR_MAX 077777777777ull

/* The largest pax or GNU long-name record read. */
#define LONG_MAX_BYTES (1u << 20)

#ifdef _WIN32
#define is_sep(c) ((c) == '/' || (c) == '\\')
#else
#define is_sep(c) ((c) == '/')
#endif

/** Set `why` from a format, and give -1. */
static int failure(char *why, size_t size, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vsnprintf(why, size, format, ap);
    va_end(ap);
    return -1;
}

/** Format into `buf`, and say whether it fitted. */
static int fmt(char *buf, size_t size, const char *format, ...)
{
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(buf, size, format, ap);
    va_end(ap);
    return n >= 0 && (size_t)n < size ? 0 : -1;
}

/** Make directory `path`; 0 when it was made, -1 with errno otherwise. */
static int make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/** Say whether `path` is a directory. */
static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * Make `path` and the directories above it that are missing; with `parents`
 * only those above it.
 */
static int make_dirs(const char *path, int parents)
{
    char buf[PATH_LEN], *p;

    if (fmt(buf, sizeof(buf), "%s", path) != 0)
        return -1;
    /* A drive's root, as in C:\, is there and is not made. */
    p = buf[0] != '\0' && buf[1] == ':' ? buf + 2 : buf;
    for (p++; *p; p++) {
        char sep = *p;

        if (!is_sep(sep))
            continue;
        *p = '\0';
        if (make_dir(buf) != 0 && errno != EEXIST && !is_dir(buf))
            return -1;
        *p = sep;
    }
    if (parents)
        return 0;
    return make_dir(buf) != 0 && errno != EEXIST && !is_dir(buf) ? -1 : 0;
}

/** Store `v` little-endian in the `n` bytes at `p`. */
static void put_le(unsigned char *p, unsigned long v, int n)
{
    int i;

    for (i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

/** The checksum of a short name that its long name's entries repeat. */
static unsigned char short_sum(const unsigned char *name)
{
    unsigned char sum = 0;
    int i;

    for (i = 0; i < 11; i++)
        sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

/** Say whether `name` can be a FAT long name of printable ASCII. */
static int long_name_ok(const char *name)
{
    size_t n = strlen(name), i;

    if (n == 0 || n > 255 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (name[i] < 0x20 || name[i] > 0x7e || strchr("\\/:*?\"<>|", name[i]) != NULL)
            return 0;
    return 1;
}

/**
 * Write the directory entries of file `name` at `e`: its long name's entries,
 * last part first, then the short entry `F` and seven digits of `index`.
 *
 * @return the count of entries written.
 */
static size_t seed_entry(unsigned char *e, const char *name, unsigned index, unsigned cluster,
                         unsigned long size)
{
    /* The offsets in a long name's entry of its 13 UCS-2 characters. */
    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    /* 2000-01-01, as FAT stores a date. */
    const unsigned long date = (20ul << 9) | (1ul << 5) | 1ul;
    unsigned char short_name[12], sum;
    size_t len = strlen(name), parts = (len + 12) / 13, i;
    int k;

    snprintf((char *)short_name, sizeof(short_name), "F%07u   ", index);
    sum = short_sum(short_name);
    for (i = 0; i < parts; i++) {
        unsigned char *l = e + 32 * (parts - 1 - i);

        l[0] = (unsigned char)((i + 1) | (i + 1 == parts ? 0x40 : 0));
        l[11] = 0x0f;
        l[13] = sum;
        for (k = 0; k < 13; k++) {
            size_t c = 13 * i + (size_t)k;

            put_le(l + at[k], c < len ? (unsigned char)name[c] : c == len ? 0 : 0xffff, 2);
        }
    }
    e += 32 * parts;
    memcpy(e, short_name, 11);
    e[11] = 0x20;
    put_le(e + 16, date, 2);
    put_le(e + 18, date, 2);
    put_le(e + 24, date, 2);
    put_le(e + 26, size > 0 ? cluster : 0, 2);
    put_le(e + 28, size, 4);
    return parts + 1;
}

/**
 * Read file `path` into the `room` bytes at `buf`, and its size into
 * `*size`.
 *
 * @return 0; 1 when the file is larger than `room`; or -1 when it cannot be
 *         read.
 */
static int load(const char *path, unsigned char *buf, size_t room, unsigned long *size)
{
    FILE *f = fopen(path, "rb");
    int rc;

    if (f == NULL)
        return -1;
    *size = (unsigned long)fread(buf, 1, room, f);
    rc = ferror(f) ? -1 : !feof(f) && fgetc(f) != EOF ? 1 : 0;
    fclose(f);
    return rc;
}

int etude_volume_seed(const char *img, const char *label, const char *dir, const char *const *names,
                      char *why, size_t whylen)
{
    /* One sector a cluster, 512 root entries, two FATs: 16223 clusters, which
     * makes the volume FAT16. */
    const unsigned total = ETUDE_VOLUME_SEED_SECTORS, reserved = 1, roots = 512;
    const unsigned root_sectors = roots * 32 / BLOCK;
    const unsigned fat = (total - reserved - root_sectors + 257) / 258;
    const unsigned root = reserved + 2 * fat, data = root + root_sectors;
    const unsigned clusters = total - data;
    unsigned char *v, *boot, *fat1;
    unsigned next = 2, used = 1, i;
    char path[PATH_LEN];
    FILE *f = NULL;
    int rc = 0;

    if ((v = calloc(total, BLOCK)) == NULL)
        return failure(why, whylen, "out of memory");
    boot = v;
    fat1 = v + (size_t)reserved * BLOCK;
    memcpy(boot, "\xeb\x3c\x90", 3);
    memcpy(boot + 3, "ETUDE   ", 8);
    put_le(boot + 11, BLOCK, 2);
    boot[13] = 1;
    put_le(boot + 14, reserved, 2);
    boot[16] = 2;
    put_le(boot + 17, roots, 2);
    put_le(boot + 19, total, 2);
    boot[21] = 0xf8;
    put_le(boot + 22, fat, 2);
    put_le(boot + 24, 32, 2);
    put_le(boot + 26, 2, 2);
    boot[36] = 0x80;
    boot[38] = 0x29;
    put_le(boot + 39, 0x45545544ul, 4);
    memset(boot + 43, ' ', 11);
    memcpy(boot + 43, label, strlen(label) < 11 ? strlen(label) : 11);
    memcpy(boot + 54, "FAT16   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xaa;
    put_le(fat1, 0xfff8, 2);
    put_le(fat1 + 2, 0xffff, 2);
    memcpy(v + (size_t)root * BLOCK, boot + 43, 11);
    v[(size_t)root * BLOCK + 11] = 0x08;
    for (i = 0; names[i] != NULL && rc == 0; i++) {
        unsigned long size = 0, n, c;
        size_t parts = (strlen(names[i]) + 12) / 13;
        int got;

        if (!long_name_ok(names[i]))
            rc = failure(why, whylen, "%s cannot be a name on the seed volume", names[i]);
        else if (used + parts + 1 > roots)
            rc = failure(why, whylen, "the seed volume's root directory is full");
        else if (fmt(path, sizeof(path), "%s/%s", dir, names[i]) != 0)
            rc = failure(why, whylen, "%s/%s cannot be read", dir, names[i]);
        else if ((got = load(path, v + ((size_t)data + next - 2) * BLOCK,
                             (size_t)(clusters - (next - 2)) * BLOCK, &size)) != 0)
            rc =
                failure(why, whylen,
                        got < 0 ? "%s cannot be read" : "%s does not fit on the seed volume", path);
        if (rc != 0)
            break;
        n = (size + BLOCK - 1) / BLOCK;
        for (c = next; c < next + n; c++)
            put_le(fat1 + 2 * c, c + 1 < next + n ? c + 1 : 0xffff, 2);
        used +=
            (unsigned)seed_entry(v + (size_t)root * BLOCK + 32 * used, names[i], i + 1, next, size);
        next += (unsigned)n;
    }
    memcpy(fat1 + (size_t)fat * BLOCK, fat1, (size_t)fat * BLOCK);
    if (rc == 0) {
        if ((f = fopen(img, "wb")) == NULL)
            rc = failure(why, whylen, "%s cannot be made: %s", img, strerror(errno));
        else if (fwrite(v, BLOCK, total, f) != total)
            rc = failure(why, whylen, "%s cannot be written", img), fclose(f);
        else if (fclose(f) != 0)
            rc = failure(why, whylen, "%s cannot be written", img);
    }
    free(v);
    return rc;
}

struct etude_volume {
    FILE *f;
    uint64_t size, used; /* the disk's bytes, and the archive's so far */
    char *why;
    size_t whylen;
    int failed;
};

/** Note that writing `v` failed, with the reason from a format; give -1. */
static int vfail(struct etude_volume *v, const char *format, ...)
{
    va_list ap;

    if (v->failed)
        return -1;
    va_start(ap, format);
    vsnprintf(v->why, v->whylen, format, ap);
    va_end(ap);
    v->failed = 1;
    return -1;
}

/** Write `n` bytes of the archive, leaving room for its end. */
static int put(struct etude_volume *v, const void *data, size_t n)
{
    if (v->failed)
        return -1;
    if (v->size && v->used + n + 2 * BLOCK > v->size)
        return vfail(v, "the job's files do not fit in its volume of %llu MiB",
                     (unsigned long long)(v->size >> 20));
    if (fwrite(data, 1, n, v->f) != n)
        return vfail(v, "the job volume cannot be written");
    v->used += n;
    return 0;
}

/** Write zeros to the end of the block the archive is in. */
static int pad(struct etude_volume *v)
{
    static const unsigned char zero[BLOCK];

    return v->used % BLOCK == 0 ? 0 : put(v, zero, BLOCK - (size_t)(v->used % BLOCK));
}

/** Write octal `value` in a header field of `n` bytes, with its NUL. */
static void octal(unsigned char *field, size_t n, uint64_t value)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "%0*llo", (int)(n - 1), (unsigned long long)value);
    memcpy(field, buf, n);
}

/** Append a pax record `key=value` to `buf`, of `*len` bytes so far. */
static int pax_record(char *buf, size_t size, size_t *len, const char *key, const char *value)
{
    size_t base = strlen(key) + strlen(value) + 3, n = base + 1;
    char digits[24];

    for (;;) {
        snprintf(digits, sizeof(digits), "%lu", (unsigned long)n);
        if (base + strlen(digits) == n)
            break;
        n = base + strlen(digits);
    }
    if (fmt(buf + *len, size - *len, "%lu %s=%s\n", (unsigned long)n, key, value) != 0)
        return -1;
    *len += n;
    return 0;
}

/** Write one ustar header block. */
static int header_block(struct etude_volume *v, const char *name, char type, unsigned mode,
                        uint64_t size, uint64_t mtime)
{
    unsigned char h[BLOCK];
    unsigned long sum = 0;
    size_t i;

    memset(h, 0, sizeof(h));
    memcpy(h, name, strlen(name) < 100 ? strlen(name) : 100);
    octal(h + 100, 8, mode);
    octal(h + 108, 8, 0);
    octal(h + 116, 8, 0);
    octal(h + 124, 12, size);
    octal(h + 136, 12, mtime);
    memset(h + 148, ' ', 8);
    h[156] = (unsigned char)type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, "root", 4);
    memcpy(h + 297, "root", 4);
    for (i = 0; i < BLOCK; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06lo", sum);
    return put(v, h, BLOCK);
}

/**
 * Write the header of entry `name`, a directory when `type` is '5', with a
 * pax header before it when the name or the size does not fit ustar's.
 */
static int header(struct etude_volume *v, const char *name, char type, unsigned mode, uint64_t size,
                  uint64_t mtime)
{
    char full[PATH_LEN + 2], pax[PATH_LEN + 128], num[24];
    size_t len = 0;

    if (fmt(full, sizeof(full), "%s%s", name, type == '5' ? "/" : "") != 0 ||
        (strlen(full) > 100 && pax_record(pax, sizeof(pax), &len, "path", full) != 0))
        return vfail(v, "%s is too long a name", name);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)size);
    if (size > USTAR_MAX && pax_record(pax, sizeof(pax), &len, "size", num) != 0)
        return vfail(v, "%s is too long a name", name);
    if (len > 0 && (header_block(v, "PaxHeader", 'x', 0644, len, mtime) != 0 ||
                    put(v, pax, len) != 0 || pad(v) != 0))
        return -1;
    return header_block(v, full, type, mode, size > USTAR_MAX ? 0 : size, mtime);
}

struct etude_volume *etude_volume_create(const char *img, uint64_t size, char *why, size_t whylen)
{
    struct etude_volume *v = calloc(1, sizeof(*v));

    if (v == NULL) {
        failure(why, whylen, "out of memory");
        return NULL;
    }
    if ((v->f = fopen(img, "wb")) == NULL) {
        failure(why, whylen, "%s cannot be made: %s", img, strerror(errno));
        free(v);
        return NULL;
    }
    v->size = size;
    v->why = why;
    v->whylen = whylen;
    return v;
}

int etude_volume_dir(struct etude_volume *v, const char *name)
{
    return header(v, name, '5', 0755, 0, 0);
}

int etude_volume_text(struct etude_volume *v, const char *name, const char *data, size_t n)
{
    if (header(v, name, '0', 0644, n, 0) != 0 || put(v, data, n) != 0)
        return -1;
    return pad(v);
}

/** Copy a host file of `size` bytes into the archive. */
static int add_file(struct etude_volume *v, const char *path, uint64_t size)
{
    char buf[65536];
    uint64_t left = size;
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL)
        return vfail(v, "%s cannot be read: %s", path, strerror(errno));
    while (left > 0 && !v->failed) {
        n = fread(buf, 1, left < sizeof(buf) ? (size_t)left : sizeof(buf), f);
        if (n == 0)
            break;
        put(v, buf, n);
        left -= n;
    }
    if (left > 0 || ferror(f) || fgetc(f) != EOF)
        vfail(v, "%s changed while it was copied", path);
    fclose(f);
    return v->failed ? -1 : pad(v);
}

/**
 * Add host file or directory `path` as entry `name`, both in buffers of
 * PATH_LEN bytes that a directory's children are appended to and taken from.
 */
static int add_tree(struct etude_volume *v, char *name, char *path, int depth)
{
    struct stat st;
    struct dirent *d;
    DIR *dp;
    size_t nlen = strlen(name), plen = strlen(path);

    if (stat(path, &st) != 0)
        return vfail(v, "%s cannot be read: %s", path, strerror(errno));
    if (S_ISREG(st.st_mode))
        return header(v, name, '0', (st.st_mode & S_IXUSR) ? 0755 : 0644, (uint64_t)st.st_size,
                      st.st_mtime > 0 ? (uint64_t)st.st_mtime : 0) != 0
                   ? -1
                   : add_file(v, path, (uint64_t)st.st_size);
    if (!S_ISDIR(st.st_mode))
        return vfail(v, "%s is neither a file nor a directory", path);
    if (depth == DEPTH_MAX)
        return vfail(v, "%s is more than %d directories deep", path, DEPTH_MAX);
    if (header(v, name, '5', 0755, 0, st.st_mtime > 0 ? (uint64_t)st.st_mtime : 0) != 0)
        return -1;
    if ((dp = opendir(path)) == NULL)
        return vfail(v, "%s cannot be read: %s", path, strerror(errno));
    while (!v->failed && (d = readdir(dp)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;
        if (fmt(name + nlen, PATH_LEN - nlen, "/%s", d->d_name) != 0 ||
            fmt(path + plen, PATH_LEN - plen, "/%s", d->d_name) != 0) {
            vfail(v, "%s/%s is too long a name", path, d->d_name);
            break;
        }
        add_tree(v, name, path, depth + 1);
        name[nlen] = '\0';
        path[plen] = '\0';
    }
    closedir(dp);
    return v->failed ? -1 : 0;
}

int etude_volume_add(struct etude_volume *v, const char *name, const char *path)
{
    char n[PATH_LEN], p[PATH_LEN];

    if (v->failed)
        return -1;
    if (fmt(n, sizeof(n), "%s", name) != 0 || fmt(p, sizeof(p), "%s", path) != 0)
        return vfail(v, "%s is too long a name", path);
    return add_tree(v, n, p, 0);
}

int etude_volume_close(struct etude_volume *v)
{
    static const unsigned char zero[2 * BLOCK];
    int rc;

    if (!v->failed && fwrite(zero, 1, sizeof(zero), v->f) != sizeof(zero))
        vfail(v, "the job volume cannot be written");
#ifdef _WIN32
    if (!v->failed && v->size &&
        (_fseeki64(v->f, (long long)v->size - 1, SEEK_SET) != 0 || fputc(0, v->f) == EOF))
#else
    if (!v->failed && v->size &&
        (fseeko(v->f, (off_t)v->size - 1, SEEK_SET) != 0 || fputc(0, v->f) == EOF))
#endif
        vfail(v, "the job volume cannot be extended to %llu bytes", (unsigned long long)v->size);
    if (fclose(v->f) != 0)
        vfail(v, "the job volume cannot be written");
    rc = v->failed ? -1 : 0;
    free(v);
    return rc;
}

/* A volume being read. */
struct reader {
    FILE *f;
    uint64_t at; /* the bytes read */
    char *why;
    size_t whylen;
};

/** Read one block; -1 with `why` set when the volume ends first. */
static int block(struct reader *r, unsigned char *b)
{
    if (fread(b, 1, BLOCK, r->f) != BLOCK)
        return failure(r->why, r->whylen,
                       "the volume ends at byte %llu, before its archive does: it is truncated",
                       (unsigned long long)r->at);
    r->at += BLOCK;
    return 0;
}

/**
 * Read a header's number field of `n` bytes: octal digits after any spaces,
 * ended by a space or a NUL, or GNU's base-256 form.
 */
static int number(const unsigned char *field, size_t n, uint64_t *value)
{
    size_t i = 0;

    *value = 0;
    if (field[0] & 0x80) {
        if (field[0] != 0x80)
            return -1;
        for (i = 1; i < n; i++) {
            if (*value >> 56)
                return -1;
            *value = *value << 8 | field[i];
        }
        return 0;
    }
    while (i < n && field[i] == ' ')
        i++;
    if (i == n || field[i] < '0' || field[i] > '7')
        return -1;
    for (; i < n && field[i] >= '0' && field[i] <= '7'; i++) {
        if (*value >> 60)
            return -1;
        *value = *value << 3 | (uint64_t)(field[i] - '0');
    }
    return i == n || field[i] == ' ' || field[i] == '\0' ? 0 : -1;
}

/** Say whether header `h` has the checksum it gives. */
static int checksum_ok(const unsigned char *h)
{
    unsigned long sum = 0;
    long ssum = 0;
    uint64_t stored;
    size_t i;

    if (number(h + 148, 8, &stored) != 0)
        return 0;
    for (i = 0; i < BLOCK; i++) {
        unsigned char c = i >= 148 && i < 156 ? ' ' : h[i];

        sum += c;
        ssum += (signed char)c;
    }
    return stored == sum || (long)stored == ssum;
}

/** Read the `size` bytes of an entry's data and its padding into a string. */
static char *data_string(struct reader *r, uint64_t size)
{
    unsigned char b[BLOCK];
    char *s;
    uint64_t got;

    if (size > LONG_MAX_BYTES) {
        failure(r->why, r->whylen, "the header at byte %llu has a long name of %llu bytes",
                (unsigned long long)(r->at - BLOCK), (unsigned long long)size);
        return NULL;
    }
    if ((s = malloc((size_t)size + 1)) == NULL) {
        failure(r->why, r->whylen, "out of memory");
        return NULL;
    }
    for (got = 0; got < size; got += BLOCK) {
        if (block(r, b) != 0) {
            free(s);
            return NULL;
        }
        memcpy(s + got, b, size - got < BLOCK ? (size_t)(size - got) : BLOCK);
    }
    s[size] = '\0';
    return s;
}

/** Skip the `size` bytes of an entry's data and its padding. */
static int skip(struct reader *r, uint64_t size)
{
    unsigned char b[BLOCK];
    uint64_t got;

    for (got = 0; got < size; got += BLOCK)
        if (block(r, b) != 0)
            return -1;
    return 0;
}

/**
 * Take the records of a pax header: `path` into `*path`, a new string, and
 * `size` into `*size`.
 */
static int pax_records(struct reader *r, const char *p, size_t n, char **path, uint64_t *size,
                       int *sized)
{
    const char *end = p + n;

    while (p < end) {
        char *rest;
        unsigned long len = strtoul(p, &rest, 10);
        const char *key = rest + 1, *eq;

        if (rest == p || *rest != ' ' || len < 5 || len > (unsigned long)(end - p) ||
            p[len - 1] != '\n' || (eq = memchr(key, '=', (size_t)(p + len - key))) == NULL)
            return failure(r->why, r->whylen, "a pax header before byte %llu is damaged",
                           (unsigned long long)r->at);
        if (eq - key == 4 && strncmp(key, "path", 4) == 0) {
            free(*path);
            if ((*path = malloc((size_t)(p + len - 1 - eq))) == NULL)
                return failure(r->why, r->whylen, "out of memory");
            memcpy(*path, eq + 1, (size_t)(p + len - 2 - eq));
            (*path)[p + len - 2 - eq] = '\0';
        } else if (eq - key == 4 && strncmp(key, "size", 4) == 0) {
            *size = strtoull(eq + 1, NULL, 10);
            *sized = 1;
        }
        p += len;
    }
    return 0;
}

/**
 * Make an entry's name from header `h` or a long name, without its leading
 * `./` and trailing slashes, in `out`; refuse a name that leaves the archive.
 */
static int entry_name(struct reader *r, const unsigned char *h, const char *long_name, char *out,
                      size_t size)
{
    char *p, *c;
    int n;

    if (long_name != NULL)
        n = fmt(out, size, "%s", long_name);
    else if (memcmp(h + 257, "ustar\0", 6) == 0 && h[345] != '\0')
        n = fmt(out, size, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
    else
        n = fmt(out, size, "%.100s", (const char *)h);
    if (n != 0)
        return failure(r->why, r->whylen, "the entry before byte %llu has too long a name",
                       (unsigned long long)r->at);
    for (p = out; p[0] == '.' && p[1] == '/';)
        for (p += 2; *p == '/'; p++)
            ;
    if (strcmp(p, ".") == 0)
        p++;
    memmove(out, p, strlen(p) + 1);
    for (n = (int)strlen(out); n > 0 && out[n - 1] == '/'; n--)
        out[n - 1] = '\0';
    if (out[0] == '/')
        return failure(r->why, r->whylen, "the volume's entry %s is not inside it", out);
    for (c = out; *c != '\0'; c = *p != '\0' ? p + 1 : p) {
        p = c + strcspn(c, "/");
        if (p == c || (p - c == 1 && c[0] == '.') || (p - c == 2 && c[0] == '.' && c[1] == '.'))
            return failure(r->why, r->whylen, "the volume's entry %s is not inside it", out);
    }
#ifdef _WIN32
    if (strpbrk(out, "\\:") != NULL)
        return failure(r->why, r->whylen, "the volume's entry %s cannot be a name on Windows", out);
#endif
    return 0;
}

/** Copy an entry's `size` bytes of data to host file `path`. */
static int extract(struct reader *r, const char *name, const char *path, uint64_t size)
{
    unsigned char b[BLOCK];
    uint64_t got;
    FILE *f;

    if (make_dirs(path, 1) != 0 || (f = fopen(path, "wb")) == NULL)
        return failure(r->why, r->whylen, "%s cannot be written: %s", path, strerror(errno));
    for (got = 0; got < size; got += BLOCK) {
        size_t n = size - got < BLOCK ? (size_t)(size - got) : BLOCK;

        if (fread(b, 1, BLOCK, r->f) != BLOCK) {
            fclose(f);
            return failure(r->why, r->whylen,
                           "the volume ends at byte %llu, inside %s: it is truncated",
                           (unsigned long long)r->at, name);
        }
        r->at += BLOCK;
        if (fwrite(b, 1, n, f) != n) {
            fclose(f);
            return failure(r->why, r->whylen, "%s cannot be written", path);
        }
    }
    if (fclose(f) != 0)
        return failure(r->why, r->whylen, "%s cannot be written", path);
    return 0;
}

/**
 * Find the pair of `map` that takes entry `name`, mark it as used in `used`,
 * and write the entry's host path to `path`.
 */
static int place(const char *const *map, unsigned char *used, const char *name, char *path,
                 size_t size)
{
    size_t i, n;

    for (i = 0; map[i] != NULL && map[i + 1] != NULL; i += 2) {
        n = strlen(map[i]);
        if (strncmp(name, map[i], n) == 0 && (name[n] == '\0' || name[n] == '/')) {
            used[i / 2] = 1;
            return fmt(path, size, "%s%s", map[i + 1], name + n);
        }
    }
    return -1;
}

/**
 * Write entry `name` of `size` bytes, a directory when `dir` is nonzero,
 * where the pairs of `map` place it, or skip a directory of no name.
 */
static int take(struct reader *r, const char *const *map, unsigned char *used, const char *name,
                int dir, uint64_t size)
{
    char path[PATH_LEN];

    if (dir && name[0] == '\0')
        return skip(r, size);
    if (name[0] == '\0' || place(map, used, name, path, sizeof(path)) != 0)
        return failure(r->why, r->whylen, "the volume has an entry it should not: %s", name);
    if (!dir)
        return extract(r, name, path, size);
    if (make_dirs(path, 0) != 0)
        return failure(r->why, r->whylen, "%s cannot be made: %s", path, strerror(errno));
    return skip(r, size);
}

int etude_volume_read(const char *img, const char *const *map, char *why, size_t whylen)
{
    static const unsigned char zero[BLOCK];
    struct reader r;
    unsigned char h[BLOCK], *used;
    char name[PATH_LEN], *long_name = NULL, *data;
    uint64_t size, pax_size = 0;
    size_t pairs, i;
    int sized = 0, rc = 0;

    for (pairs = 0; map[2 * pairs] != NULL && map[2 * pairs + 1] != NULL; pairs++)
        ;
    if ((used = calloc(pairs + 1, 1)) == NULL)
        return failure(why, whylen, "out of memory");
    r.at = 0;
    r.why = why;
    r.whylen = whylen;
    if ((r.f = fopen(img, "rb")) == NULL) {
        free(used);
        return failure(why, whylen, "%s cannot be read: %s", img, strerror(errno));
    }
    while (rc == 0) {
        if ((rc = block(&r, h)) != 0)
            break;
        if (memcmp(h, zero, BLOCK) == 0) {
            if ((rc = block(&r, h)) == 0 && memcmp(h, zero, BLOCK) != 0)
                rc = failure(why, whylen, "the volume has a lone zero block at byte %llu",
                             (unsigned long long)(r.at - 2 * BLOCK));
            break;
        }
        if (!checksum_ok(h) || memcmp(h + 257, "ustar", 5) != 0 || number(h + 124, 12, &size)) {
            rc = failure(why, whylen, "the header at byte %llu fails its checksum: it is damaged",
                         (unsigned long long)(r.at - BLOCK));
            break;
        }
        switch (h[156]) {
        case 'x':
        case 'L':
            if ((data = data_string(&r, size)) == NULL)
                rc = -1;
            else if (h[156] == 'x')
                rc = pax_records(&r, data, (size_t)size, &long_name, &pax_size, &sized);
            else {
                free(long_name);
                long_name = data;
                data = NULL;
            }
            free(data);
            continue;
        case 'g':
        case 'K':
            rc = skip(&r, size);
            continue;
        case '0':
        case '\0':
        case '7':
        case '5':
            break;
        default:
            if (entry_name(&r, h, long_name, name, sizeof(name)) == 0)
                rc = failure(why, whylen, "the volume's entry %s is %s", name,
                             h[156] == '1' || h[156] == '2' ? "a link"
                                                            : "neither a file nor a directory");
            else
                rc = -1;
            continue;
        }
        if (sized)
            size = pax_size;
        if ((rc = entry_name(&r, h, long_name, name, sizeof(name))) != 0)
            break;
        rc = take(&r, map, used, name, h[156] == '5', size);
        free(long_name);
        long_name = NULL;
        sized = 0;
    }
    for (i = 0; rc == 0 && i < pairs; i++)
        if (!used[i])
            rc = failure(why, whylen, "the volume has no %s", map[2 * i]);
    free(long_name);
    free(used);
    fclose(r.f);
    return rc;
}
