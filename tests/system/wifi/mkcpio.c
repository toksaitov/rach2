/**
 * mkcpio: write a cpio archive of the "newc" format, the format of a Linux
 * initramfs, from a list of files and of tar archives whose members it takes.
 *
 * Usage: mkcpio OUTPUT LIST
 *
 * Each line of LIST is fields separated by tabs:
 *
 *   d  MODE  NAME
 *   f  MODE  NAME  SOURCE
 *   l  MODE  NAME  TARGET
 *   t  ARCHIVE
 *
 * for a directory, a regular file whose bytes are read from SOURCE, a
 * symbolic link to TARGET, and every member of the uncompressed tar ARCHIVE
 * but those at its top whose names begin with a dot, an Alpine package's
 * metadata. MODE is octal permission bits; NAME is the path in the archive,
 * without a leading slash. A member keeps its tar header's mode; it may be a
 * file, a directory or a symbolic link, and its name may come from a pax or a
 * GNU long-name header. Every entry is owned by root, has one link and the
 * time SOURCE_DATE_EPOCH gives, 0 without it. A directory already written is
 * not written again; a later file or link of a name written before takes
 * its place when the kernel unpacks the archive.
 *
 * The members go from the tar stream to the archive without touching the host's
 * file system, whose names may ignore case, as macOS's and Windows' do, and
 * which on MSYS2 has neither a program's executable bit nor a link to an
 * absolute target; and the list, not a file, gives a file's mode. No file
 * system call beyond fopen is made, so the program builds with any host C
 * compiler. It exits 0 when the archive is written, and 1 with
 * `mkcpio: detail` on standard error otherwise, removing OUTPUT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_DIR        0040000UL
#define S_REG        0100000UL
#define S_LNK        0120000UL
#define NAME_MAX_LEN 4096

/** The archive being written and what has been written to it. */
struct archive {
    FILE *out;           /* the archive, opened in binary mode */
    unsigned long ino;   /* the inode number of the next entry, from 1 */
    unsigned long time;  /* every entry's modification time */
    unsigned long size;  /* bytes written, for the padding to four */
    char **dirs;         /* the directories written, an open-addressed table */
    unsigned long ndirs; /* entries used in dirs */
    unsigned long cap;   /* entries in dirs, a power of two */
};

/**
 * Write N bytes of BUF to the archive. Returns 0, or -1 when the write
 * failed.
 */
static int put(struct archive *a, const void *buf, unsigned long n)
{
    if (n > 0 && fwrite(buf, 1, n, a->out) != n) {
        fprintf(stderr, "mkcpio: write failed\n");
        return -1;
    }
    a->size += n;
    return 0;
}

/** Pad the archive with zero bytes to a multiple of four. */
static int pad(struct archive *a)
{
    static const char zero[4] = {0, 0, 0, 0};

    return put(a, zero, (4 - a->size % 4) % 4);
}

/** The FNV-1a hash of NAME. */
static unsigned long hash(const char *name)
{
    unsigned long h = 2166136261UL;

    while (*name != '\0')
        h = ((h ^ (unsigned char)*name++) * 16777619UL) & 0xffffffffUL;
    return h;
}

/**
 * Record the directory NAME as written. Returns 1 when it was written
 * before, 0 when it is new, and -1 when memory ran out.
 */
static int dir_seen(struct archive *a, const char *name)
{
    unsigned long i, j;
    char **old, **grown;

    if (2 * (a->ndirs + 1) > a->cap) {
        grown = calloc(a->cap * 2, sizeof(*grown));
        if (grown == NULL)
            return -1;
        old = a->dirs;
        for (i = 0; i < a->cap; i++) {
            if (old[i] == NULL)
                continue;
            for (j = hash(old[i]) & (2 * a->cap - 1); grown[j] != NULL;
                 j = (j + 1) & (2 * a->cap - 1))
                ;
            grown[j] = old[i];
        }
        free(old);
        a->dirs = grown;
        a->cap *= 2;
    }
    for (i = hash(name) & (a->cap - 1); a->dirs[i] != NULL; i = (i + 1) & (a->cap - 1)) {
        if (strcmp(a->dirs[i], name) == 0)
            return 1;
    }
    a->dirs[i] = malloc(strlen(name) + 1);
    if (a->dirs[i] == NULL)
        return -1;
    strcpy(a->dirs[i], name);
    a->ndirs++;
    return 0;
}

/**
 * Write one entry's header and NAME, padded, for an entry of MODE (type and
 * permissions) whose data is SIZE bytes long. Returns 0 or -1.
 */
static int header(struct archive *a, const char *name, unsigned long mode, unsigned long size)
{
    static const char digits[] = "0123456789ABCDEF";
    unsigned long field[13];
    char head[110];
    unsigned long namesize = (unsigned long)strlen(name) + 1;
    int i, j;

    if (size > 0xffffffffUL || a->time > 0xffffffffUL || a->ino > 0xffffffffUL) {
        fprintf(stderr, "mkcpio: %s: a field does not fit in 32 bits\n", name);
        return -1;
    }
    field[0] = a->ino++;
    field[1] = mode;
    field[2] = 0;                                       /* uid */
    field[3] = 0;                                       /* gid */
    field[4] = (mode & 0170000UL) == S_DIR ? 2UL : 1UL; /* nlink */
    field[5] = a->time;                                 /* mtime */
    field[6] = size;                                    /* filesize */
    field[7] = field[8] = field[9] = field[10] = 0;     /* devices */
    field[11] = namesize;                               /* namesize */
    field[12] = 0;                                      /* check */
    memcpy(head, "070701", 6);
    for (i = 0; i < 13; i++) {
        for (j = 0; j < 8; j++)
            head[6 + 8 * i + j] = digits[(field[i] >> (28 - 4 * j)) & 0xf];
    }
    if (put(a, head, sizeof(head)) < 0 || put(a, name, namesize) < 0)
        return -1;
    return pad(a);
}

/**
 * Write a directory entry for NAME unless one was written. Returns 0 or -1.
 */
static int dir_entry(struct archive *a, const char *name, unsigned long mode)
{
    int seen = dir_seen(a, name);

    if (seen < 0) {
        fprintf(stderr, "mkcpio: out of memory\n");
        return -1;
    }
    return seen ? 0 : header(a, name, S_DIR | mode, 0);
}

/** Write a symbolic link NAME of MODE to TARGET. Returns 0 or -1. */
static int link_entry(struct archive *a, const char *name, unsigned long mode, const char *target)
{
    unsigned long len = (unsigned long)strlen(target);

    if (header(a, name, S_LNK | mode, len) < 0 || put(a, target, len) < 0)
        return -1;
    return pad(a);
}

/**
 * Copy LEN bytes of IN, which FROM names for messages, into the archive.
 * Returns 0, or -1 with a message printed when IN ended early.
 */
static int copy(struct archive *a, FILE *in, unsigned long len, const char *from)
{
    char buf[65536];
    size_t n;

    while (len > 0) {
        n = fread(buf, 1, len < sizeof(buf) ? len : sizeof(buf), in);
        if (n == 0) {
            fprintf(stderr, "mkcpio: %s: ended or failed while read\n", from);
            return -1;
        }
        if (put(a, buf, n) < 0)
            return -1;
        len -= n;
    }
    return 0;
}

/**
 * Write a regular file named NAME of MODE whose bytes are SOURCE's. Returns
 * 0, or -1 with a message printed.
 */
static int file_entry(struct archive *a, const char *name, unsigned long mode, const char *source)
{
    FILE *in;
    long len;
    int st;

    in = fopen(source, "rb");
    if (in == NULL || fseek(in, 0, SEEK_END) != 0 || (len = ftell(in)) < 0 ||
        fseek(in, 0, SEEK_SET) != 0) {
        fprintf(stderr, "mkcpio: %s: cannot read\n", source);
        if (in != NULL)
            fclose(in);
        return -1;
    }
    st = header(a, name, S_REG | mode, (unsigned long)len) < 0 ||
                 copy(a, in, (unsigned long)len, source) < 0
             ? -1
             : pad(a);
    fclose(in);
    return st;
}

/**
 * The value of the octal or base-256 number in the LEN bytes of FIELD of a
 * tar header. Sets *BAD when the field holds neither.
 */
static unsigned long tar_number(const unsigned char *field, int len, int *bad)
{
    unsigned long v = 0;
    int i = 0;

    if (field[0] & 0x80) {
        for (i = 1; i < len; i++) {
            if (v > (0xffffffffUL >> 8))
                *bad = 1;
            v = (v << 8) | field[i];
        }
        return v;
    }
    while (i < len && field[i] == ' ')
        i++;
    for (; i < len && field[i] >= '0' && field[i] <= '7'; i++)
        v = v * 8 + (unsigned long)(field[i] - '0');
    if (i < len && field[i] != '\0' && field[i] != ' ')
        *bad = 1;
    return v;
}

/**
 * Copy into OUT, of OUTLEN bytes, the value of KEY in the LEN bytes of pax
 * records at REC, if a record gives one. Returns 1 when one did, 0 when none
 * did, -1 when the records are malformed or the value is too long.
 */
static int pax_value(const char *rec, unsigned long len, const char *key, char *out, size_t outlen)
{
    unsigned long at = 0, n;
    size_t klen = strlen(key);
    const char *p, *eq;
    int found = 0;

    while (at < len) {
        n = strtoul(rec + at, NULL, 10);
        if (n == 0 || n > len - at || rec[at + n - 1] != '\n')
            return -1;
        p = memchr(rec + at, ' ', n);
        eq = p != NULL ? memchr(p, '=', n - (unsigned long)(p - (rec + at))) : NULL;
        if (eq == NULL)
            return -1;
        if ((size_t)(eq - p - 1) == klen && memcmp(p + 1, key, klen) == 0) {
            if ((size_t)(rec + at + n - 1 - (eq + 1)) >= outlen)
                return -1;
            memcpy(out, eq + 1, (size_t)(rec + at + n - 1 - (eq + 1)));
            out[rec + at + n - 1 - (eq + 1)] = '\0';
            found = 1;
        }
        at += n;
    }
    return found;
}

/**
 * Read a member's data of LEN bytes, a pax or long-name record, from IN into
 * a new buffer, and skip its padding. Returns the buffer, NUL-terminated, or
 * NULL with a message printed.
 */
static char *tar_record(FILE *in, unsigned long len, const char *archive)
{
    char *buf;
    unsigned long padded = (len + 511) / 512 * 512;

    if (len > (1UL << 20)) {
        fprintf(stderr, "mkcpio: %s: a record of %lu bytes\n", archive, len);
        return NULL;
    }
    buf = malloc(padded + 1);
    if (buf == NULL || fread(buf, 1, padded, in) != padded) {
        fprintf(stderr, "mkcpio: %s: a record ended early\n", archive);
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

/** The length of the string in a tar header's field of LEN bytes at F. */
static size_t field_len(const unsigned char *f, size_t len)
{
    const unsigned char *nul = memchr(f, '\0', len);

    return nul != NULL ? (size_t)(nul - f) : len;
}

/** Skip LEN bytes of IN. Returns 0, or -1 when it ended first. */
static int skip(FILE *in, unsigned long len)
{
    char buf[512];
    size_t n;

    while (len > 0) {
        n = fread(buf, 1, len < sizeof(buf) ? len : sizeof(buf), in);
        if (n == 0)
            return -1;
        len -= n;
    }
    return 0;
}

/**
 * Write every member of the tar ARCHIVE but its dot files at the top.
 * Returns 0, or -1 with a message printed.
 */
static int tar_entries(struct archive *a, const char *archive)
{
    unsigned char h[512];
    char name[NAME_MAX_LEN], target[NAME_MAX_LEN];
    char *rec, *p;
    unsigned long size, mode;
    int have_name = 0, have_target = 0, bad, st = 0, r;
    size_t len;
    FILE *in;

    in = fopen(archive, "rb");
    if (in == NULL) {
        fprintf(stderr, "mkcpio: %s: cannot open\n", archive);
        return -1;
    }
    while (st == 0 && (len = fread(h, 1, sizeof(h), in)) == sizeof(h)) {
        bad = 0;
        size = tar_number(h + 124, 12, &bad);
        mode = tar_number(h + 100, 8, &bad) & 07777;
        if (h[0] == '\0') /* a zero block: an end, or the end of a segment */
            continue;
        if (bad) {
            fprintf(stderr, "mkcpio: %s: a header that is not tar's\n", archive);
            st = -1;
            break;
        }
        if (h[156] == 'x' || h[156] == 'L' || h[156] == 'K') {
            rec = tar_record(in, size, archive);
            if (rec == NULL) {
                st = -1;
                break;
            }
            if (h[156] == 'L' || h[156] == 'K') {
                p = h[156] == 'L' ? name : target;
                if (strlen(rec) >= NAME_MAX_LEN) {
                    fprintf(stderr, "mkcpio: %s: a long name too long\n", archive);
                    st = -1;
                } else {
                    strcpy(p, rec);
                    *(h[156] == 'L' ? &have_name : &have_target) = 1;
                }
            } else {
                r = pax_value(rec, size, "path", name, sizeof(name));
                have_name |= r == 1;
                st = r < 0 ? -1 : st;
                r = pax_value(rec, size, "linkpath", target, sizeof(target));
                have_target |= r == 1;
                st = r < 0 ? -1 : st;
                if (st < 0)
                    fprintf(stderr, "mkcpio: %s: a pax header that does not parse\n", archive);
            }
            free(rec);
            continue;
        }
        if (h[156] == 'g') {
            st = skip(in, (size + 511) / 512 * 512);
            continue;
        }
        if (!have_name) {
            len = 0;
            if (memcmp(h + 257, "ustar", 5) == 0 && h[345] != '\0') {
                len = field_len(h + 345, 155);
                memcpy(name, h + 345, len);
                name[len++] = '/';
            }
            memcpy(name + len, h, field_len(h, 100));
            name[len + field_len(h, 100)] = '\0';
        }
        if (!have_target) {
            memcpy(target, h + 157, 100);
            target[100] = '\0';
        }
        have_name = have_target = 0;
        p = name;
        while (p[0] == '.' && p[1] == '/')
            p += 2;
        len = strlen(p);
        while (len > 0 && p[len - 1] == '/')
            p[--len] = '\0';
        if (len == 0 || (p[0] == '.' && strchr(p, '/') == NULL)) {
            st = skip(in, (size + 511) / 512 * 512);
            continue;
        }
        switch (h[156]) {
        case '0':
        case '\0':
        case '7':
            st = header(a, p, S_REG | mode, size) < 0 || copy(a, in, size, archive) < 0 ||
                         pad(a) < 0 || skip(in, (512 - size % 512) % 512) < 0
                     ? -1
                     : 0;
            break;
        case '5':
            st = dir_entry(a, p, mode);
            break;
        case '2':
            st = link_entry(a, p, 0777, target);
            break;
        default:
            fprintf(stderr, "mkcpio: %s: %s: tar type %c is not a file, directory or link\n",
                    archive, p, h[156]);
            st = -1;
        }
    }
    if (st == 0 && (len != 0 || ferror(in))) {
        fprintf(stderr, "mkcpio: %s: ends inside a header\n", archive);
        st = -1;
    }
    fclose(in);
    return st;
}

/**
 * Write the entry that one LINE of the list describes; LINE is changed.
 * Returns 0, or -1 with a message printed.
 */
static int entry(struct archive *a, char *line, unsigned long lineno)
{
    char *field[4];
    char *mode_end;
    unsigned long mode;
    int n = 0, want;
    char *p = line;

    line[strcspn(line, "\r\n")] = '\0';
    while (n < 4) {
        field[n++] = p;
        p = strchr(p, '\t');
        if (p == NULL)
            break;
        *p++ = '\0';
    }
    want = field[0][0] == 't' ? 2 : field[0][0] == 'd' ? 3 : 4;
    if (strlen(field[0]) != 1 || n != want) {
        fprintf(stderr, "mkcpio: line %lu: not TYPE MODE NAME [SOURCE] or t ARCHIVE\n", lineno);
        return -1;
    }
    if (field[0][0] == 't')
        return tar_entries(a, field[1]);
    mode = strtoul(field[1], &mode_end, 8);
    if (*field[2] == '\0' || *field[2] == '/' || mode > 07777 || *mode_end != '\0') {
        fprintf(stderr, "mkcpio: line %lu: a bad mode or name\n", lineno);
        return -1;
    }
    switch (field[0][0]) {
    case 'd':
        return dir_entry(a, field[2], mode);
    case 'f':
        return file_entry(a, field[2], mode, field[3]);
    case 'l':
        return link_entry(a, field[2], mode, field[3]);
    default:
        fprintf(stderr, "mkcpio: line %lu: type %s is not d, f, l or t\n", lineno, field[0]);
        return -1;
    }
}

int main(int argc, char **argv)
{
    struct archive a;
    char line[8192];
    unsigned long lineno = 0;
    const char *epoch = getenv("SOURCE_DATE_EPOCH");
    FILE *list;
    int bad = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: mkcpio OUTPUT LIST\n");
        return 2;
    }
    list = fopen(argv[2], "r");
    if (list == NULL) {
        fprintf(stderr, "mkcpio: %s: cannot open\n", argv[2]);
        return 1;
    }
    a.out = fopen(argv[1], "wb");
    a.cap = 1024;
    a.ndirs = 0;
    a.dirs = calloc(a.cap, sizeof(*a.dirs));
    if (a.out == NULL || a.dirs == NULL) {
        fprintf(stderr, "mkcpio: %s: cannot create\n", argv[1]);
        fclose(list);
        return 1;
    }
    a.ino = 1;
    a.time = epoch != NULL ? strtoul(epoch, NULL, 10) : 0;
    a.size = 0;
    while (!bad && fgets(line, sizeof(line), list) != NULL) {
        lineno++;
        if (strchr(line, '\n') == NULL && !feof(list)) {
            fprintf(stderr, "mkcpio: line %lu: too long\n", lineno);
            bad = 1;
        } else if (line[0] != '\n' && line[0] != '\0') {
            bad = entry(&a, line, lineno) < 0;
        }
    }
    fclose(list);
    if (!bad) {
        a.ino = 0;
        bad = header(&a, "TRAILER!!!", 0, 0) < 0;
    }
    if (fclose(a.out) != 0)
        bad = 1;
    if (bad) {
        fprintf(stderr, "mkcpio: %s: not written\n", argv[1]);
        remove(argv[1]);
        return 1;
    }
    return 0;
}
