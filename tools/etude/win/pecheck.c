/**
 * pecheck: refuse Windows programs that import an MSYS2 or Cygwin runtime.
 *
 * Etude's Windows programs are built with MinGW-w64 and must run on a
 * Windows machine that has neither MSYS2 nor Cygwin. `pecheck FILE...` reads
 * the DLL names each program imports, from its import directory and its
 * delay-load import directory, and refuses every name that is not a library
 * of Windows itself. It parses the PE format by hand, with explicit
 * little-endian reads and every offset checked against the file, so that it
 * builds with any hosted C99 compiler and a damaged file cannot make it read
 * outside its buffer.
 *
 * The exit status is 0 when every file passed, 1 when an import was refused,
 * and 2 when a file could not be read or is not a well-formed PE image, or
 * for a wrong usage; 2 wins over 1. `pecheck --selftest` checks the parser
 * and the judgement on PE images it builds in memory.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DOS header and the PE signature that follows it. */
#define DOS_HEADER_SIZE   64
#define DOS_PE_OFFSET     0x3c
#define PE_SIGNATURE_SIZE 4

/* The COFF file header, after the PE signature. */
#define COFF_SIZE      20
#define COFF_NSECTIONS 2
#define COFF_OPT_SIZE  16

/* The optional header, after the COFF header, in its two forms. */
#define OPT_MAGIC_PE32     0x10b
#define OPT_MAGIC_PE32PLUS 0x20b
#define OPT_MAGIC_ROM      0x107
#define OPT32_IMAGE_BASE   28
#define OPT32_NDIRS        92
#define OPT32_DIRS         96
#define OPT64_IMAGE_BASE   24
#define OPT64_NDIRS        108
#define OPT64_DIRS         112

/* The data directories: an RVA and a size each. */
#define DIR_IMPORT     1
#define DIR_DELAY      13
#define DIR_MAX        16
#define DIR_ENTRY_SIZE 8

/* A section header of the section table. */
#define SECTION_SIZE 40
#define SEC_VSIZE    8
#define SEC_VA       12
#define SEC_RAW_SIZE 16
#define SEC_RAW_PTR  20

/* An import descriptor and a delay-load descriptor. */
#define IMPORT_DESC_SIZE 20
#define IMPORT_DESC_NAME 12
#define DELAY_DESC_SIZE  32
#define DELAY_DESC_ATTRS 0
#define DELAY_DESC_NAME  4
#define DELAY_ATTR_RVA   1

/* The limits pecheck sets on what it reads. */
#define DLL_NAME_MAX    255
#define MAX_DESCRIPTORS 1024
#define FILE_LIMIT      ((size_t)1 << 30)
#define DETAIL_SIZE     200

/** One imported DLL: its name inside the file's buffer, and how it loads. */
struct import {
    const char *name; /* NUL-terminated, printable ASCII, 1 to 255 bytes */
    int delay;        /* 1 when it comes from the delay-load directory */
};

/** The DLLs one image imports, in the order of its directories. */
struct imports {
    struct import item[2 * MAX_DESCRIPTORS];
    int n;
};

/** What pecheck needs of a parsed image. */
struct pe {
    const unsigned char *buf;
    size_t len;
    uint64_t image_base;
    uint32_t dir[DIR_MAX]; /* the RVA of each data directory, 0 when absent */
    size_t sections;       /* the file offset of the section table */
    unsigned nsections;
    char *detail; /* the reason the image is malformed, DETAIL_SIZE bytes */
};

/** The descriptor table of one data directory that names DLLs. */
struct directory {
    const char *what;  /* its name in messages */
    int index;         /* its index among the data directories */
    size_t desc_size;  /* the size of one descriptor */
    size_t name_field; /* the offset of the DLL name's RVA in a descriptor */
    int delay;
};

static const struct directory directories[] = {
    {"import", DIR_IMPORT, IMPORT_DESC_SIZE, IMPORT_DESC_NAME, 0},
    {"delay-load import", DIR_DELAY, DELAY_DESC_SIZE, DELAY_DESC_NAME, 1},
};

/* The libraries of Windows itself that a program may import, in lower case.
   A `*` matches any run of characters. */
static const char *const windows_libraries[] = {
    "kernel32.dll", "advapi32.dll", "ws2_32.dll",   "ntdll.dll",  "bcrypt.dll",   "shell32.dll",
    "userenv.dll",  "psapi.dll",    "ucrtbase.dll", "msvcrt.dll", "api-ms-win-*",
};

static const char reason_msys[] = "an MSYS2 runtime";
static const char reason_cygwin[] = "a Cygwin runtime";
static const char reason_other[] = "not a library of Windows itself";

/** A runtime library that pecheck refuses with a reason of its own. */
struct runtime {
    const char *pattern;
    const char *reason;
};

static const struct runtime runtimes[] = {
    {"msys-*.dll", reason_msys},
    {"cygwin1.dll", reason_cygwin},
    {"cyg*.dll", reason_cygwin},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/** Return the little-endian 16-bit value at `p`. */
static uint32_t get16(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

/** Return the little-endian 32-bit value at `p`. */
static uint32_t get32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Return the little-endian 64-bit value at `p`. */
static uint64_t get64(const unsigned char *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

/** Tell whether `n` bytes at offset `off` lie in a buffer of `len` bytes. */
static int inside(size_t len, uint64_t off, uint64_t n)
{
    return off <= len && n <= len - off;
}

/** Write a message of at most `n` bytes to `buf`, and return -1. */
static int fail(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return -1;
}

/**
 * Write the reason an image is malformed to `pe->detail`, and return -1.
 *
 * The parser's functions return through this, so that every error leaves a
 * message for the caller to print.
 */
static int malformed(const struct pe *pe, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(pe->detail, DETAIL_SIZE, fmt, ap);
    va_end(ap);
    return -1;
}

/**
 * Read the headers of the PE image in `pe->buf`.
 *
 * Fills in the image base, the RVAs of the data directories and the place of
 * the section table, having checked that the headers and the section table
 * lie inside the file. The optional header may be PE32 or PE32+, and must be
 * long enough for the data directories it counts, at most sixteen.
 *
 * @return 0, or -1 with the reason in `pe->detail`.
 */
static int parse_headers(struct pe *pe)
{
    const unsigned char *buf = pe->buf;
    uint64_t pe_off, opt, dirs;
    uint32_t opt_size, magic, ndirs, i;

    if (pe->len < DOS_HEADER_SIZE)
        return malformed(pe, "the file is shorter than a DOS header");
    if (buf[0] != 'M' || buf[1] != 'Z')
        return malformed(pe, "no MZ signature");
    pe_off = get32(buf + DOS_PE_OFFSET);
    if (!inside(pe->len, pe_off, PE_SIGNATURE_SIZE + COFF_SIZE))
        return malformed(pe, "the PE header at offset 0x%lx is past the end of the file",
                         (unsigned long)pe_off);
    if (memcmp(buf + pe_off, "PE\0\0", PE_SIGNATURE_SIZE) != 0)
        return malformed(pe, "no PE signature at offset 0x%lx", (unsigned long)pe_off);

    pe->nsections = get16(buf + pe_off + PE_SIGNATURE_SIZE + COFF_NSECTIONS);
    opt_size = get16(buf + pe_off + PE_SIGNATURE_SIZE + COFF_OPT_SIZE);
    opt = pe_off + PE_SIGNATURE_SIZE + COFF_SIZE;
    if (!inside(pe->len, opt, opt_size))
        return malformed(pe, "the optional header runs past the end of the file");
    if (opt_size < 2)
        return malformed(pe, "the optional header is too short");

    magic = get16(buf + opt);
    if (magic == OPT_MAGIC_PE32) {
        if (opt_size < OPT32_DIRS)
            return malformed(pe, "the PE32 optional header is too short");
        pe->image_base = get32(buf + opt + OPT32_IMAGE_BASE);
        ndirs = get32(buf + opt + OPT32_NDIRS);
        dirs = opt + OPT32_DIRS;
    } else if (magic == OPT_MAGIC_PE32PLUS) {
        if (opt_size < OPT64_DIRS)
            return malformed(pe, "the PE32+ optional header is too short");
        pe->image_base = get64(buf + opt + OPT64_IMAGE_BASE);
        ndirs = get32(buf + opt + OPT64_NDIRS);
        dirs = opt + OPT64_DIRS;
    } else {
        return malformed(pe, "unknown optional header magic 0x%lx", (unsigned long)magic);
    }

    if (ndirs > DIR_MAX)
        ndirs = DIR_MAX;
    if (dirs + (uint64_t)ndirs * DIR_ENTRY_SIZE > opt + opt_size)
        return malformed(pe, "the optional header is too short for its %lu data directories",
                         (unsigned long)ndirs);
    for (i = 0; i < DIR_MAX; i++)
        pe->dir[i] = i < ndirs ? get32(buf + dirs + (uint64_t)i * DIR_ENTRY_SIZE) : 0;

    pe->sections = (size_t)(opt + opt_size);
    if (!inside(pe->len, pe->sections, (uint64_t)pe->nsections * SECTION_SIZE))
        return malformed(pe, "the section table runs past the end of the file");
    return 0;
}

/**
 * Find the bytes of the file at a relative virtual address (RVA).
 *
 * A section covers the RVAs from its VirtualAddress for the larger of its
 * VirtualSize and its SizeOfRawData. Only the first SizeOfRawData bytes come
 * from the file, at PointerToRawData; the loader fills the rest with zeros.
 * On success `*off` is the RVA's offset in the file, and `*avail` counts the
 * bytes from there to the end of the section's data or of the file,
 * whichever comes first. An RVA in no section, or in a part of one that the
 * file does not hold, is an error, since pecheck judges only what the file
 * holds.
 *
 * @param what  what the RVA locates, for the message.
 * @return 0, or -1 with the reason in `pe->detail`.
 */
static int map_rva(const struct pe *pe, uint32_t rva, const char *what, size_t *off, size_t *avail)
{
    unsigned i;

    for (i = 0; i < pe->nsections; i++) {
        const unsigned char *s = pe->buf + pe->sections + (size_t)i * SECTION_SIZE;
        uint32_t va = get32(s + SEC_VA);
        uint32_t vsize = get32(s + SEC_VSIZE);
        uint32_t raw = get32(s + SEC_RAW_SIZE);
        uint64_t extent = vsize > raw ? vsize : raw;
        uint64_t start, end;

        if (rva < va || rva - va >= extent)
            continue;
        start = (uint64_t)get32(s + SEC_RAW_PTR) + (rva - va);
        end = (uint64_t)get32(s + SEC_RAW_PTR) + raw;
        if (end > pe->len)
            end = pe->len;
        if (rva - va >= raw || start >= end)
            return malformed(pe, "%s at RVA 0x%lx is not in the file", what, (unsigned long)rva);
        *off = (size_t)start;
        *avail = (size_t)(end - start);
        return 0;
    }
    return malformed(pe, "%s at RVA 0x%lx is outside every section", what, (unsigned long)rva);
}

/**
 * Read the DLL name at an RVA.
 *
 * A name is 1 to 255 bytes of printable ASCII other than the space, ended
 * by a NUL inside its section's data in the file. On success `*name` points
 * into the image's buffer.
 *
 * @return 0, or -1 with the reason in `pe->detail`.
 */
static int read_name(const struct pe *pe, uint32_t rva, const char **name)
{
    size_t off, avail, limit, i;

    if (map_rva(pe, rva, "a DLL name", &off, &avail) != 0)
        return -1;
    limit = avail < DLL_NAME_MAX + 1 ? avail : DLL_NAME_MAX + 1;
    for (i = 0; i < limit && pe->buf[off + i] != '\0'; i++) {
        unsigned char c = pe->buf[off + i];

        if (c <= ' ' || c > '~')
            return malformed(pe, "the DLL name at RVA 0x%lx has the byte 0x%02x",
                             (unsigned long)rva, (unsigned)c);
    }
    if (i == limit && limit > DLL_NAME_MAX)
        return malformed(pe, "the DLL name at RVA 0x%lx is longer than %d bytes",
                         (unsigned long)rva, DLL_NAME_MAX);
    if (i == limit)
        return malformed(pe, "the DLL name at RVA 0x%lx has no terminating NUL",
                         (unsigned long)rva);
    if (i == 0)
        return malformed(pe, "the DLL name at RVA 0x%lx is empty", (unsigned long)rva);
    *name = (const char *)pe->buf + off;
    return 0;
}

/** Tell whether the `n` bytes at `p` are all zero. */
static int all_zero(const unsigned char *p, size_t n)
{
    while (n > 0 && *p == 0) {
        p++;
        n--;
    }
    return n == 0;
}

/**
 * Append the DLL names of one directory's descriptor table to `im`.
 *
 * The table ends with an all-zero descriptor, which must come within the
 * section that holds the table and within MAX_DESCRIPTORS entries. Reading to
 * the all-zero descriptor reads every descriptor the loader could use, since
 * the loader stops there or sooner. A descriptor that is not all zero must
 * name a DLL.
 *
 * A delay-load descriptor whose attributes have bit 0 clear is in the old
 * form, which Visual C++ 6 wrote: its addresses are virtual addresses, not
 * RVAs, and the name's RVA is found by subtracting the image base.
 *
 * @return 0, or -1 with the reason in `pe->detail`.
 */
static int read_directory(const struct pe *pe, const struct directory *d, struct imports *im)
{
    uint32_t rva = pe->dir[d->index];
    char label[64];
    size_t off, avail, i;

    if (rva == 0)
        return 0;
    snprintf(label, sizeof label, "the %s directory", d->what);
    if (map_rva(pe, rva, label, &off, &avail) != 0)
        return -1;
    for (i = 0;; i++) {
        const unsigned char *p = pe->buf + off + i * d->desc_size;
        uint64_t name_rva;

        if (i == MAX_DESCRIPTORS)
            return malformed(pe, "the %s directory has more than %d descriptors", d->what,
                             MAX_DESCRIPTORS);
        if (avail < (i + 1) * d->desc_size)
            return malformed(pe, "the %s directory does not end within its section", d->what);
        if (all_zero(p, d->desc_size))
            return 0;

        name_rva = get32(p + d->name_field);
        if (d->delay && !(get32(p + DELAY_DESC_ATTRS) & DELAY_ATTR_RVA)) {
            if (name_rva < pe->image_base || name_rva - pe->image_base > UINT32_MAX)
                return malformed(
                    pe, "%s descriptor %lu gives the name address 0x%lx, outside the image",
                    d->what, (unsigned long)i, (unsigned long)name_rva);
            name_rva -= pe->image_base;
        }
        if (name_rva == 0)
            return malformed(pe, "%s descriptor %lu has no DLL name", d->what, (unsigned long)i);
        if (read_name(pe, (uint32_t)name_rva, &im->item[im->n].name) != 0)
            return -1;
        im->item[im->n++].delay = d->delay;
    }
}

/**
 * Collect the DLLs that the PE image in `buf` imports.
 *
 * The names of the import directory come first, then those of the
 * delay-load import directory, each in table order. The names in `im` point
 * into `buf`, which must outlive them.
 *
 * @param detail  a buffer of DETAIL_SIZE bytes for the reason the image is
 *                malformed.
 * @return 0, or -1 when the image is malformed.
 */
static int parse_image(const unsigned char *buf, size_t len, struct imports *im, char *detail)
{
    struct pe pe;
    size_t i;

    memset(&pe, 0, sizeof pe);
    pe.buf = buf;
    pe.len = len;
    pe.detail = detail;
    im->n = 0;
    if (parse_headers(&pe) != 0)
        return -1;
    for (i = 0; i < COUNT(directories); i++)
        if (read_directory(&pe, &directories[i], im) != 0)
            return -1;
    return 0;
}

/** Tell whether the first `n` bytes of `s`, in lower case, equal `lower`. */
static int same_folded(const char *lower, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (tolower((unsigned char)s[i]) != (unsigned char)lower[i])
            return 0;
    return 1;
}

/**
 * Tell whether `name` matches `pattern`, ignoring case.
 *
 * The pattern is in lower case, and may have one `*`, which matches any run
 * of characters.
 */
static int match(const char *pattern, const char *name)
{
    const char *star = strchr(pattern, '*');
    size_t len = strlen(name), head, tail;

    if (star == NULL)
        return len == strlen(pattern) && same_folded(pattern, name, len);
    head = (size_t)(star - pattern);
    tail = strlen(star + 1);
    return len >= head + tail && same_folded(pattern, name, head) &&
           same_folded(star + 1, name + len - tail, tail);
}

/**
 * Judge one imported DLL name.
 *
 * @return NULL when the name is a library of Windows itself, or the reason
 *         it is refused.
 */
static const char *judge(const char *name)
{
    size_t i;

    for (i = 0; i < COUNT(windows_libraries); i++)
        if (match(windows_libraries[i], name))
            return NULL;
    for (i = 0; i < COUNT(runtimes); i++)
        if (match(runtimes[i].pattern, name))
            return runtimes[i].reason;
    return reason_other;
}

/**
 * Read the whole of a file into memory.
 *
 * @return a buffer of `*lenp` bytes that the caller frees, or NULL with
 *         `errno` set; a file of FILE_LIMIT bytes or more gives EFBIG.
 */
static unsigned char *read_file(const char *path, size_t *lenp)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL, *grown;
    size_t len = 0, cap = 0, got;
    int saved;

    if (f == NULL)
        return NULL;
    for (;;) {
        if (len == cap) {
            if (cap >= FILE_LIMIT) {
                saved = EFBIG;
                goto failed;
            }
            cap = cap ? cap * 2 : 65536;
            grown = realloc(buf, cap);
            if (grown == NULL) {
                saved = ENOMEM;
                goto failed;
            }
            buf = grown;
        }
        errno = 0;
        got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0)
            break;
    }
    if (ferror(f)) {
        saved = errno ? errno : EIO;
        goto failed;
    }
    fclose(f);
    *lenp = len;
    return buf;

failed:
    free(buf);
    fclose(f);
    errno = saved;
    return NULL;
}

/**
 * Check one file and report on it.
 *
 * Prints the file's imports to stdout, then each refusal to stderr.
 *
 * @return 0 when every import is allowed, 1 when one is refused, 2 when the
 *         file cannot be read or is not a well-formed PE image.
 */
static int check_file(const char *path, struct imports *im)
{
    char detail[DETAIL_SIZE];
    unsigned char *buf;
    size_t len;
    const char *reason;
    int status = 0, i;

    buf = read_file(path, &len);
    if (buf == NULL) {
        fprintf(stderr, "pecheck: %s: %s\n", path, strerror(errno));
        return 2;
    }
    if (parse_image(buf, len, im, detail) != 0) {
        fprintf(stderr, "pecheck: %s: not a PE image: %s\n", path, detail);
        free(buf);
        return 2;
    }

    printf("pecheck: %s: imports:", path);
    if (im->n == 0)
        printf(" (none)");
    for (i = 0; i < im->n; i++)
        printf(" %s%s", im->item[i].name, im->item[i].delay ? "(delay)" : "");
    printf("\n");
    fflush(stdout);
    for (i = 0; i < im->n; i++) {
        reason = judge(im->item[i].name);
        if (reason != NULL) {
            fprintf(stderr, "pecheck: %s: refused: %s: %s\n", path, im->item[i].name, reason);
            status = 1;
        }
    }
    free(buf);
    return status;
}

/* ---- the self-test ---- */

/* The layout of the images the self-test builds: one section of import
   tables and names, whose data follows the headers in the file. */
#define T_PE_OFFSET   0x40
#define T_SECTION_VA  0x1000
#define T_RAW_PTR     0x200
#define T_FILE_ALIGN  0x200
#define T_LARGE_RAW   0x6000
#define T_IMAGE_MAX   (T_RAW_PTR + T_LARGE_RAW)
#define T_NAMES_MAX   8
#define T_MANY        (MAX_DESCRIPTORS + 76)
#define T_OUTSIDE_RVA 0x8000
#define T_BASE32      0x400000
#define T_BASE64      0x140000000

/** The damage a self-test case does to the image it builds. */
enum fault {
    FAULT_NONE,
    FAULT_SHORT_HEADERS,      /* the file ends inside the optional header */
    FAULT_SHORT_TABLE,        /* the file ends inside the import table */
    FAULT_DOS_SIGNATURE,      /* no MZ */
    FAULT_PE_SIGNATURE,       /* no PE\0\0 */
    FAULT_MAGIC,              /* a ROM image's optional header */
    FAULT_NAME_OUTSIDE,       /* an import name's RVA in no section */
    FAULT_DELAY_NAME_OUTSIDE, /* a delay-load name's RVA in no section */
    FAULT_NO_NUL,             /* the file ends before the last name's NUL */
    FAULT_NO_END,             /* the import table runs to its section's end */
    FAULT_MANY,               /* more import descriptors than the limit */
    FAULT_NAME_255,           /* the first name is 255 bytes long */
    FAULT_NAME_256            /* the first name is 256 bytes long */
};

/** One image of the self-test and the verdict it must get. */
struct test_case {
    const char *name;
    int pe32plus;
    const char *const *imports; /* NULL-terminated; at most T_NAMES_MAX */
    const char *const *delays;  /* likewise */
    int old_delay;              /* delay-load descriptors in the old form */
    enum fault fault;
    int expect;      /* the exit status pecheck must give: 0, 1 or 2 */
    const char *why; /* for 1, the first refusal's reason; for 2, a phrase of the error */
};

static const char *const ucrt[] = {"KERNEL32.dll", "api-ms-win-crt-runtime-l1-1-0.dll", NULL};
static const char *const ucrt_upper[] = {"KERNEL32.DLL", "API-MS-WIN-CRT-RUNTIME-L1-1-0.DLL", NULL};
static const char *const msvcrt[] = {"KERNEL32.dll", "msvcrt.dll", NULL};
static const char *const sockets[] = {"WS2_32.dll", "bcrypt.dll", NULL};
static const char *const msys[] = {"KERNEL32.dll", "msys-2.0.dll", NULL};
static const char *const msys_upper[] = {"KERNEL32.dll", "MSYS-2.0.DLL", NULL};
static const char *const msys_alone[] = {"msys-2.0.dll", NULL};
static const char *const cygwin[] = {"cygwin1.dll", NULL};
static const char *const cygz[] = {"KERNEL32.dll", "cygz.dll", NULL};
static const char *const pthread[] = {"KERNEL32.dll", "libwinpthread-1.dll", NULL};
static const char *const kernel[] = {"KERNEL32.dll", NULL};
static const char *const none[] = {NULL};

static const struct test_case cases[] = {
    {"pe32+ ucrt program", 1, ucrt, none, 0, FAULT_NONE, 0, NULL},
    {"names in upper case", 1, ucrt_upper, none, 0, FAULT_NONE, 0, NULL},
    {"pe32 msvcrt program", 0, msvcrt, none, 0, FAULT_NONE, 0, NULL},
    {"allowed delay-load imports", 1, ucrt, sockets, 0, FAULT_NONE, 0, NULL},
    {"msys2 runtime", 1, msys, none, 0, FAULT_NONE, 1, reason_msys},
    {"msys2 runtime in upper case", 1, msys_upper, none, 0, FAULT_NONE, 1, reason_msys},
    {"cygwin1.dll", 1, cygwin, none, 0, FAULT_NONE, 1, reason_cygwin},
    {"cygz.dll", 1, cygz, none, 0, FAULT_NONE, 1, reason_cygwin},
    {"libwinpthread-1.dll", 1, pthread, none, 0, FAULT_NONE, 1, reason_other},
    {"msys2 runtime delay-loaded alone", 1, kernel, msys_alone, 0, FAULT_NONE, 1, reason_msys},
    {"msys2 runtime delay-loaded, old form", 0, kernel, msys_alone, 1, FAULT_NONE, 1, reason_msys},
    {"255-byte name", 1, kernel, none, 0, FAULT_NAME_255, 1, reason_other},
    {"256-byte name", 1, kernel, none, 0, FAULT_NAME_256, 2, "longer than 255"},
    {"truncated headers", 1, ucrt, none, 0, FAULT_SHORT_HEADERS, 2, "past the end of the file"},
    {"truncated import table", 1, ucrt, none, 0, FAULT_SHORT_TABLE, 2, "does not end"},
    {"bad dos signature", 1, ucrt, none, 0, FAULT_DOS_SIGNATURE, 2, "no MZ signature"},
    {"bad pe signature", 1, ucrt, none, 0, FAULT_PE_SIGNATURE, 2, "no PE signature"},
    {"rom image magic", 1, ucrt, none, 0, FAULT_MAGIC, 2, "unknown optional header magic"},
    {"import name outside every section", 1, ucrt, none, 0, FAULT_NAME_OUTSIDE, 2,
     "outside every section"},
    {"delay-load name outside every section", 1, kernel, msys_alone, 0, FAULT_DELAY_NAME_OUTSIDE, 2,
     "outside every section"},
    {"name without its nul", 1, msys, none, 0, FAULT_NO_NUL, 2, "no terminating NUL"},
    {"import table without end", 1, msys, none, 0, FAULT_NO_END, 2, "does not end"},
    {"too many import descriptors", 1, kernel, none, 0, FAULT_MANY, 2, "more than"},
};

/** Store `v` at `p` as 16 little-endian bits. */
static void put16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

/** Store `v` at `p` as 32 little-endian bits. */
static void put32(unsigned char *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

/** Store `v` at `p` as 64 little-endian bits. */
static void put64(unsigned char *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

/** Return the number of names in a NULL-terminated list. */
static size_t count_names(const char *const *list)
{
    size_t n = 0;

    while (list[n] != NULL)
        n++;
    return n;
}

/**
 * Where the parts of a self-test image go.
 *
 * The image has one section, which holds the DLL names, the delay-load
 * descriptors and the import descriptors. The offsets ending in `_at` are
 * from the start of the section's data.
 */
struct layout {
    const char *names[2 * T_NAMES_MAX]; /* the imports, then the delay-loaded */
    size_t nimp, ndel;
    size_t names_at, names_end, delay_at, delay_size, import_at, import_size;
    size_t used;     /* the section's VirtualSize */
    size_t raw;      /* the section's SizeOfRawData */
    size_t opt;      /* the file offset of the optional header */
    size_t opt_size; /* its size, with sixteen data directories */
    uint64_t base;
};

/**
 * Place the parts of the image that a self-test case describes.
 *
 * The names come first, except for FAULT_NO_NUL, which needs the last name
 * at the end of the file. The long-name faults replace the first name.
 */
static void plan_layout(const struct test_case *tc, struct layout *l)
{
    static char long_name[DLL_NAME_MAX + 2];
    size_t i, names_size = 0;

    l->nimp = count_names(tc->imports);
    l->ndel = count_names(tc->delays);
    for (i = 0; i < l->nimp; i++)
        l->names[i] = tc->imports[i];
    for (i = 0; i < l->ndel; i++)
        l->names[l->nimp + i] = tc->delays[i];
    if (tc->fault == FAULT_NAME_255 || tc->fault == FAULT_NAME_256) {
        size_t n = tc->fault == FAULT_NAME_255 ? DLL_NAME_MAX : DLL_NAME_MAX + 1;

        memset(long_name, 'a', n - 4);
        memcpy(long_name + n - 4, ".dll", 5);
        l->names[0] = long_name;
    }
    for (i = 0; i < l->nimp + l->ndel; i++)
        names_size += strlen(l->names[i]) + 1;

    l->delay_size = l->ndel ? (l->ndel + 1) * DELAY_DESC_SIZE : 0;
    l->import_size = l->nimp ? (l->nimp + 1) * IMPORT_DESC_SIZE : 0;
    if (tc->fault == FAULT_NO_NUL) {
        l->delay_at = 0;
        l->import_at = l->delay_size;
        l->names_at = l->import_at + l->import_size;
        l->used = l->names_at + names_size;
    } else {
        l->names_at = 0;
        l->delay_at = (names_size + 3) & ~(size_t)3;
        l->import_at = l->delay_at + l->delay_size;
        l->used = l->import_at + l->import_size;
    }
    l->raw = tc->fault == FAULT_MANY ? T_LARGE_RAW
                                     : (l->used + T_FILE_ALIGN - 1) / T_FILE_ALIGN * T_FILE_ALIGN;
    l->opt = T_PE_OFFSET + PE_SIGNATURE_SIZE + COFF_SIZE;
    l->opt_size = (tc->pe32plus ? OPT64_DIRS : OPT32_DIRS) + DIR_MAX * DIR_ENTRY_SIZE;
    l->base = tc->pe32plus ? T_BASE64 : T_BASE32;
}

/** Write the headers and the section table of a self-test image. */
static void write_headers(unsigned char *img, const struct test_case *tc, const struct layout *l)
{
    unsigned char *coff = img + T_PE_OFFSET + PE_SIGNATURE_SIZE, *opt = img + l->opt;
    unsigned char *dirs = opt + (tc->pe32plus ? OPT64_DIRS : OPT32_DIRS);
    unsigned char *sect = opt + l->opt_size;

    img[0] = 'M';
    img[1] = 'Z';
    put32(img + DOS_PE_OFFSET, T_PE_OFFSET);
    memcpy(img + T_PE_OFFSET, "PE\0\0", PE_SIGNATURE_SIZE);
    put16(coff + COFF_NSECTIONS, 1);
    put16(coff + COFF_OPT_SIZE, (uint32_t)l->opt_size);
    if (tc->pe32plus) {
        put16(opt, OPT_MAGIC_PE32PLUS);
        put64(opt + OPT64_IMAGE_BASE, l->base);
        put32(opt + OPT64_NDIRS, DIR_MAX);
    } else {
        put16(opt, OPT_MAGIC_PE32);
        put32(opt + OPT32_IMAGE_BASE, (uint32_t)l->base);
        put32(opt + OPT32_NDIRS, DIR_MAX);
    }
    if (l->nimp) {
        put32(dirs + DIR_IMPORT * DIR_ENTRY_SIZE, (uint32_t)(T_SECTION_VA + l->import_at));
        put32(dirs + DIR_IMPORT * DIR_ENTRY_SIZE + 4, (uint32_t)l->import_size);
    }
    if (l->ndel) {
        put32(dirs + DIR_DELAY * DIR_ENTRY_SIZE, (uint32_t)(T_SECTION_VA + l->delay_at));
        put32(dirs + DIR_DELAY * DIR_ENTRY_SIZE + 4, (uint32_t)l->delay_size);
    }
    memcpy(sect, ".idata", 6);
    put32(sect + SEC_VSIZE, (uint32_t)l->used);
    put32(sect + SEC_VA, T_SECTION_VA);
    put32(sect + SEC_RAW_SIZE, (uint32_t)l->raw);
    put32(sect + SEC_RAW_PTR, T_RAW_PTR);
}

/**
 * Write the DLL names and both descriptor tables of a self-test image.
 *
 * Sets `l->names_end` to the offset just past the last name's NUL.
 */
static void write_tables(unsigned char *data, const struct test_case *tc, struct layout *l)
{
    uint32_t name_rva[2 * T_NAMES_MAX];
    size_t i;

    l->names_end = l->names_at;
    for (i = 0; i < l->nimp + l->ndel; i++) {
        size_t n = strlen(l->names[i]) + 1;

        name_rva[i] = (uint32_t)(T_SECTION_VA + l->names_end);
        memcpy(data + l->names_end, l->names[i], n);
        l->names_end += n;
    }
    for (i = 0; i < l->ndel; i++) {
        unsigned char *d = data + l->delay_at + i * DELAY_DESC_SIZE;
        uint32_t rva = name_rva[l->nimp + i];

        put32(d + DELAY_DESC_ATTRS, tc->old_delay ? 0 : DELAY_ATTR_RVA);
        put32(d + DELAY_DESC_NAME, tc->old_delay ? (uint32_t)(l->base + rva) : rva);
    }
    for (i = 0; i < l->nimp; i++)
        put32(data + l->import_at + i * IMPORT_DESC_SIZE + IMPORT_DESC_NAME, name_rva[i]);
}

/**
 * Damage a self-test image as its case says.
 *
 * @return the length of the file, which a truncation shortens.
 */
static size_t damage(unsigned char *img, const struct test_case *tc, const struct layout *l)
{
    unsigned char *data = img + T_RAW_PTR, *imports = data + l->import_at;
    size_t i;

    switch (tc->fault) {
    case FAULT_NONE:
    case FAULT_NAME_255:
    case FAULT_NAME_256:
        break;
    case FAULT_SHORT_HEADERS:
        return l->opt + l->opt_size / 2;
    case FAULT_SHORT_TABLE:
        return T_RAW_PTR + l->import_at + IMPORT_DESC_SIZE / 2;
    case FAULT_DOS_SIGNATURE:
        img[1] = 'X';
        break;
    case FAULT_PE_SIGNATURE:
        img[T_PE_OFFSET + 1] = 'X';
        break;
    case FAULT_MAGIC:
        put16(img + l->opt, OPT_MAGIC_ROM);
        break;
    case FAULT_NAME_OUTSIDE:
        put32(imports + IMPORT_DESC_NAME, T_OUTSIDE_RVA);
        break;
    case FAULT_DELAY_NAME_OUTSIDE:
        put32(data + l->delay_at + DELAY_DESC_NAME, T_OUTSIDE_RVA);
        break;
    case FAULT_NO_NUL:
        return T_RAW_PTR + l->names_end - 1;
    case FAULT_NO_END:
        for (i = l->nimp * IMPORT_DESC_SIZE; l->import_at + i < l->raw; i++)
            imports[i] = imports[i % IMPORT_DESC_SIZE];
        break;
    case FAULT_MANY:
        for (i = 1; i < T_MANY; i++)
            memcpy(imports + i * IMPORT_DESC_SIZE, imports, IMPORT_DESC_SIZE);
        break;
    }
    return T_RAW_PTR + l->raw;
}

/**
 * Build the PE image that a self-test case describes, damaged as it says.
 *
 * @return a buffer of exactly `*lenp` bytes, which the caller frees, so that
 *         the sanitizers see a read past its end; NULL when memory runs out.
 */
static unsigned char *build_image(const struct test_case *tc, size_t *lenp)
{
    static unsigned char img[T_IMAGE_MAX];
    struct layout l;
    unsigned char *copy;
    size_t len;

    plan_layout(tc, &l);
    memset(img, 0, sizeof img);
    write_headers(img, tc, &l);
    write_tables(img + T_RAW_PTR, tc, &l);
    len = damage(img, tc, &l);
    copy = malloc(len ? len : 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, img, len);
    *lenp = len;
    return copy;
}

/**
 * Check that the parser found exactly the names a case built, in order.
 *
 * @return 0, or -1 with the difference in `why`.
 */
static int compare_names(const struct test_case *tc, const struct imports *im, char *why, size_t n)
{
    size_t nimp = count_names(tc->imports), ndel = count_names(tc->delays), i;

    if ((size_t)im->n != nimp + ndel)
        return fail(why, n, "found %d names, built %lu", im->n, (unsigned long)(nimp + ndel));
    for (i = 0; i < nimp + ndel; i++) {
        const char *want = i < nimp ? tc->imports[i] : tc->delays[i - nimp];

        if (strcmp(im->item[i].name, want) != 0 || im->item[i].delay != (i >= nimp))
            return fail(why, n, "name %lu is %s%s, built %s%s", (unsigned long)i, im->item[i].name,
                        im->item[i].delay ? "(delay)" : "", want, i >= nimp ? "(delay)" : "");
    }
    return 0;
}

/**
 * Run one self-test case: build its image, parse and judge it.
 *
 * @return 0 when the verdict is the one the case expects, or -1 with the
 *         difference in `why`.
 */
static int run_case(const struct test_case *tc, struct imports *im, char *why, size_t n)
{
    char detail[DETAIL_SIZE];
    unsigned char *img;
    const char *first = NULL, *reason;
    size_t len;
    int status = 0, result = 0, i;

    img = build_image(tc, &len);
    if (img == NULL)
        return fail(why, n, "out of memory");
    if (parse_image(img, len, im, detail) != 0) {
        status = 2;
    } else {
        for (i = 0; i < im->n; i++) {
            reason = judge(im->item[i].name);
            if (reason != NULL && first == NULL)
                first = reason;
        }
        status = first != NULL;
    }

    if (status != tc->expect)
        result = fail(why, n, "status %d, expected %d%s%s", status, tc->expect,
                      status == 2 ? ": " : "", status == 2 ? detail : "");
    else if (status == 2 && strstr(detail, tc->why) == NULL)
        result = fail(why, n, "malformed for another reason: %s", detail);
    else if (status == 1 && first != tc->why)
        result = fail(why, n, "refused as %s, expected %s", first, tc->why);
    else if (status != 2 && tc->fault == FAULT_NONE)
        result = compare_names(tc, im, why, n);
    free(img);
    return result;
}

/**
 * Run every self-test case and report the result.
 *
 * @return 0 when every case passed, 1 when one failed.
 */
static int selftest(void)
{
    char why[DETAIL_SIZE + 64];
    struct imports *im = malloc(sizeof *im);
    size_t i;
    int failed = 0;

    if (im == NULL) {
        fprintf(stderr, "pecheck: selftest: out of memory\n");
        return 1;
    }
    for (i = 0; i < COUNT(cases); i++) {
        if (run_case(&cases[i], im, why, sizeof why) != 0) {
            fprintf(stderr, "pecheck: selftest: result: fail: %s: %s\n", cases[i].name, why);
            failed = 1;
        }
    }
    free(im);
    if (!failed)
        printf("pecheck: selftest: result: pass: %lu cases\n", (unsigned long)COUNT(cases));
    return failed;
}

/** Check the files the arguments name, or run the self-test. */
int main(int argc, char **argv)
{
    struct imports *im;
    int i = 1, worst = 0, status;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if (i < argc && strcmp(argv[i], "--") == 0)
        i++;
    else if (i < argc && argv[i][0] == '-' && argv[i][1] != '\0')
        i = argc;
    if (i >= argc) {
        fprintf(stderr, "pecheck: usage: pecheck FILE... | pecheck --selftest\n");
        return 2;
    }

    im = malloc(sizeof *im);
    if (im == NULL) {
        fprintf(stderr, "pecheck: out of memory\n");
        return 2;
    }
    for (; i < argc; i++) {
        status = check_file(argv[i], im);
        if (status > worst)
            worst = status;
    }
    free(im);
    return worst;
}
