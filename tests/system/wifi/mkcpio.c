/**
 * mkcpio: write a cpio archive of the "newc" format, the format of a Linux
 * initramfs, from a list that gives each entry's type, mode and contents.
 *
 * Usage: mkcpio OUTPUT LIST
 *
 * Each line of LIST is four fields separated by tabs:
 *
 *   d  MODE  NAME
 *   f  MODE  NAME  SOURCE
 *   l  MODE  NAME  TARGET
 *
 * for a directory, a regular file whose bytes are read from SOURCE, and a
 * symbolic link to TARGET. MODE is octal permission bits; NAME is the path in
 * the archive, without a leading slash. Every entry is owned by root, has
 * one link and the time SOURCE_DATE_EPOCH gives, 0 without it.
 *
 * The list, not the files, gives the modes, since a host that has no
 * executable bit for an ELF file, as MSYS2 on Windows, cannot say which
 * files are programs; and no file system call beyond fopen is made, so the
 * program builds with any host C compiler. It exits 0 when the archive is
 * written, and 1 with `mkcpio: detail` on standard error otherwise, removing
 * OUTPUT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_DIR 0040000UL
#define S_REG 0100000UL
#define S_LNK 0120000UL

/** The archive being written and what has been written to it. */
struct archive {
    FILE *out;          /* the archive, opened in binary mode */
    unsigned long ino;  /* the inode number of the next entry, from 1 */
    unsigned long time; /* every entry's modification time */
    unsigned long size; /* bytes written, for the padding to four */
};

/**
 * Write N bytes of BUF to the archive. Returns 0, or -1 when the write
 * failed.
 */
static int put(struct archive *a, const void *buf, unsigned long n)
{
    if (n > 0 && fwrite(buf, 1, n, a->out) != n)
        return -1;
    a->size += n;
    return 0;
}

/** Pad the archive with zero bytes to a multiple of four. */
static int pad(struct archive *a)
{
    static const char zero[4] = {0, 0, 0, 0};

    return put(a, zero, (4 - a->size % 4) % 4);
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
 * Write a regular file named NAME of MODE whose bytes are SOURCE's. Returns
 * 0, or -1 with a message printed.
 */
static int file_entry(struct archive *a, const char *name, unsigned long mode, const char *source)
{
    char buf[65536];
    FILE *in;
    long len;
    size_t n;

    in = fopen(source, "rb");
    if (in == NULL || fseek(in, 0, SEEK_END) != 0 || (len = ftell(in)) < 0 ||
        fseek(in, 0, SEEK_SET) != 0) {
        fprintf(stderr, "mkcpio: %s: cannot read\n", source);
        if (in != NULL)
            fclose(in);
        return -1;
    }
    if (header(a, name, S_REG | mode, (unsigned long)len) < 0) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (put(a, buf, n) < 0 || (len -= (long)n) < 0)
            break;
    }
    if (ferror(in) || len != 0) {
        fprintf(stderr, "mkcpio: %s: changed or unreadable while read\n", source);
        fclose(in);
        return -1;
    }
    fclose(in);
    return pad(a);
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
    int n = 0;
    char *p = line;

    line[strcspn(line, "\r\n")] = '\0';
    while (n < 4) {
        field[n++] = p;
        p = strchr(p, '\t');
        if (p == NULL)
            break;
        *p++ = '\0';
    }
    mode = n >= 3 ? strtoul(field[1], &mode_end, 8) : 0;
    if (n < 3 || *field[2] == '\0' || *field[2] == '/' || mode > 07777 || *mode_end != '\0' ||
        strlen(field[0]) != 1 || (field[0][0] == 'd') != (n == 3)) {
        fprintf(stderr, "mkcpio: line %lu: not TYPE MODE NAME [SOURCE]\n", lineno);
        return -1;
    }
    switch (field[0][0]) {
    case 'd':
        return header(a, field[2], S_DIR | mode, 0);
    case 'f':
        return file_entry(a, field[2], mode, field[3]);
    case 'l':
        if (header(a, field[2], S_LNK | mode, (unsigned long)strlen(field[3])) < 0 ||
            put(a, field[3], (unsigned long)strlen(field[3])) < 0)
            return -1;
        return pad(a);
    default:
        fprintf(stderr, "mkcpio: line %lu: type %s is not d, f or l\n", lineno, field[0]);
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
    if (a.out == NULL) {
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
