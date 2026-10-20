/*
 * macho.c: the digest by which a proof compares a 64-bit Mach-O file, with
 * its debug map's object paths and times, its UUID and its code signature
 * left out.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macho.h"
#include "sha256.h"

#define MH_MAGIC_64       0xfeedfacfu
#define LC_SYMTAB         0x2u
#define LC_UUID           0x1bu
#define LC_CODE_SIGNATURE 0x1du
#define N_OSO             0x66u

/* The sizes of a mach_header_64 and of an nlist_64. */
#define HEADER_SIZE 32u
#define NLIST_SIZE  16u

/** Give the little-endian 32-bit value at `p`. */
static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Zero the `n` bytes of `buf`, of `len`, from `off`; 0, or -1 past its end. */
static int blank(unsigned char *buf, size_t len, uint32_t off, uint32_t n)
{
    if (off > len || n > len - off)
        return -1;
    memset(buf + off, 0, n);
    return 0;
}

/**
 * Zero the time and the object's path of each N_OSO entry of the symbol
 * table that the LC_SYMTAB command `cmd` describes; 0, or -1 for a table or
 * a path that is not within the file.
 */
static int blank_oso(unsigned char *buf, size_t len, const unsigned char *cmd)
{
    uint32_t symoff = le32(cmd + 8), nsyms = le32(cmd + 12);
    uint32_t stroff = le32(cmd + 16), strsize = le32(cmd + 20), i, strx;
    unsigned char *sym, *s, *end;

    if (symoff > len || nsyms > (len - symoff) / NLIST_SIZE || stroff > len ||
        strsize > len - stroff)
        return -1;
    for (i = 0; i < nsyms; i++) {
        sym = buf + symoff + (size_t)i * NLIST_SIZE;
        if (sym[4] != N_OSO)
            continue;
        strx = le32(sym);
        if (strx >= strsize)
            return -1;
        s = buf + stroff + strx;
        end = memchr(s, '\0', strsize - strx);
        if (end == NULL)
            return -1;
        memset(sym + 8, 0, 8);
        memset(s, 0, (size_t)(end - s));
    }
    return 0;
}

/** Zero the parts of the Mach-O file `buf`, of `len`, that the digest leaves
 * out; 0, or -1 when a load command does not fit the file. */
static int blank_all(unsigned char *buf, size_t len)
{
    uint32_t ncmds = le32(buf + 16), sizeofcmds = le32(buf + 20), size, i;
    size_t at = HEADER_SIZE, end;
    unsigned char *cmd;

    if (sizeofcmds > len - HEADER_SIZE)
        return -1;
    end = HEADER_SIZE + sizeofcmds;
    for (i = 0; i < ncmds; i++, at += size) {
        if (end - at < 8)
            return -1;
        cmd = buf + at;
        size = le32(cmd + 4);
        if (size < 8 || size > end - at)
            return -1;
        if (le32(cmd) == LC_UUID && (size < 24 || blank(buf, len, (uint32_t)at + 8, 16) != 0))
            return -1;
        if (le32(cmd) == LC_CODE_SIGNATURE &&
            (size < 16 || blank(buf, len, le32(cmd + 8), le32(cmd + 12)) != 0))
            return -1;
        if (le32(cmd) == LC_SYMTAB && (size < 24 || blank_oso(buf, len, cmd) != 0))
            return -1;
    }
    return 0;
}

/** Read all of `f` after the `got` bytes already in `*buf`, of `*cap`. */
static int read_rest(FILE *f, unsigned char **buf, size_t *cap, size_t *got)
{
    unsigned char *more;
    size_t n;

    for (;;) {
        if (*got == *cap) {
            if (*cap > ((size_t)-1) / 2) {
                errno = EFBIG;
                return -1;
            }
            more = realloc(*buf, *cap * 2);
            if (more == NULL)
                return -1;
            *buf = more;
            *cap *= 2;
        }
        n = fread(*buf + *got, 1, *cap - *got, f);
        *got += n;
        if (n == 0)
            return ferror(f) ? (errno = EIO, -1) : 0;
    }
}

/**
 * Give in `hex` the SHA-256 of the 64-bit little-endian Mach-O file `path`
 * with these parts zeroed: the path and the time of each object in its debug
 * map (the N_OSO entries of its symbol table), its UUID and its code
 * signature.
 *
 * Apple's linker puts in the debug map the path and the time of each object
 * it read, and the compiler driver names the objects of a one-step build at
 * random in `TMPDIR`; the UUID is a hash of the whole output, and the ad hoc
 * signature hashes its pages. These parts differ between two builds of the
 * same sources while every other byte is the same, provided the objects'
 * paths have the same length, so that the string table keeps its layout.
 *
 * @return 0; 1 for a file that is not a 64-bit Mach-O, or whose load
 *         commands do not fit it, which the caller compares whole; or -1
 *         with errno set.
 */
int macho_digest(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    size_t cap = 65536, got = 0;
    unsigned char *buf, sum[32];
    struct sha256 h;
    int status, e;

    if (f == NULL)
        return -1;
    buf = malloc(cap);
    if (buf == NULL) {
        fclose(f);
        return -1;
    }
    got = fread(buf, 1, HEADER_SIZE, f);
    if (got < HEADER_SIZE || le32(buf) != MH_MAGIC_64)
        status = ferror(f) ? (errno = EIO, -1) : 1;
    else if (read_rest(f, &buf, &cap, &got) != 0)
        status = -1;
    else
        status = blank_all(buf, got) == 0 ? 0 : 1;
    e = errno;
    fclose(f);
    if (status == 0) {
        sha256_init(&h);
        sha256_update(&h, buf, got);
        sha256_final(&h, sum);
        sha256_hex(sum, hex);
    }
    free(buf);
    errno = e;
    return status;
}
