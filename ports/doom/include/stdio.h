/*
 * stdio.h: the streams and formatted output of Doom on Rach2. A FILE is a
 * descriptor with no buffer, so each call is a system call and fflush does
 * nothing. crt0.c defines the functions declared here, and the rest are
 * inline.
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define BUFSIZ 1024

typedef struct {
    int fd;
    int eof;   /* a read returned 0; fseek, rewind and clearerr clear it */
    int error; /* a read or a write failed; rewind and clearerr clear it */
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/**
 * Open `path` by the first letter of `mode`: "r" reads; "r+", exactly,
 * reads and writes; "w" writes a file created or emptied; and "a" writes at
 * the end of a file created when it is missing. A '+' after "w" or "a" is
 * ignored, and so is "b". A file created has mode 0666 less the umask.
 * Returns NULL when the open fails or no memory is left for the FILE, which
 * is never freed.
 */
FILE *fopen(const char *path, const char *mode);

/** Close the descriptor of `f`. Returns 0, or -1 for a NULL `f`; the FILE
 * stays allocated. */
int fclose(FILE *f);

/**
 * One read or write of `size` times `nmemb` bytes, with no retry of a short
 * transfer. Returns the whole items moved: 0 on an error, which sets the
 * error flag, and for fread at the end of the file, which sets the end flag.
 */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);

int fseek(FILE *f, long offset, int whence);

/** The offset of the descriptor, from lseek: a negative errno on an error. */
long ftell(FILE *f);

int feof(FILE *f);
int ferror(FILE *f);
int fflush(FILE *f);

/**
 * Format as the C library does and write the result to descriptor 1 in one
 * write. The flags, the width and the precision are read and ignored; ll
 * reads a long; %f, %e, %g and their capitals print `<float>`; and an
 * unknown conversion prints '?' and takes an int. At most 1023 bytes are
 * written, and the bytes formatted are returned.
 */
int printf(const char *fmt, ...);

/** Format as vsnprintf does into 1024 bytes and write them in one write. */
int fprintf(FILE *f, const char *fmt, ...);

/** Write `s` and a newline in two writes; returns 0. */
int puts(const char *s);
int putchar(int c);

/**
 * Read only %d, which skips spaces and takes a sign and digits; any other
 * conversion takes nothing and stores nothing. Any other character of the
 * format must equal the next of `str`, so a space matches only a space.
 * Returns the %d conversions made, each counted even when no digit follows.
 */
int sscanf(const char *str, const char *fmt, ...);

static inline int fgetc(FILE *f) {
    unsigned char c;
    extern int read(int fd, void *buf, size_t count);
    if (!f) return EOF;
    if (read(f->fd, &c, 1) <= 0) {
        f->eof = 1;
        return EOF;
    }
    return c;
}

static inline int fputc(int c, FILE *f) {
    unsigned char ch = c;
    extern int write(int fd, const void *buf, size_t count);
    if (!f) return EOF;
    write(f->fd, &ch, 1);
    return c;
}

static inline int fputs(const char *s, FILE *f) {
    extern size_t strlen(const char *s);
    extern int write(int fd, const void *buf, size_t count);
    if (!f || !s) return EOF;
    write(f->fd, s, strlen(s));
    return 0;
}

static inline char *fgets(char *s, int size, FILE *f) {
    int i = 0;
    int c;
    if (!f || !s || size <= 0) return NULL;
    while (i < size - 1) {
        c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

static inline void rewind(FILE *f) {
    if (f) {
        fseek(f, 0, SEEK_SET);
        f->eof = 0;
        f->error = 0;
    }
}

static inline void clearerr(FILE *f) {
    if (f) {
        f->eof = 0;
        f->error = 0;
    }
}

static inline int getc(FILE *f) { return fgetc(f); }
static inline int putc(int c, FILE *f) { return fputc(c, f); }
static inline int getchar(void) { return fgetc(stdin); }

int vfprintf(FILE *f, const char *fmt, va_list ap);

/** vsnprintf with a size of 4096; the caller's buffer must hold the result. */
int sprintf(char *str, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);

/**
 * Format into `str`, of `size` bytes, and terminate it. %d, %u and %x take
 * a width, a precision and the 0 flag, %o a precision and the 0 flag, and
 * %s a width, right-aligned; the - flag is ignored. ll reads a long,
 * %f, %e, %g and their capitals give `<float>`, and an unknown conversion
 * gives '?' and takes an int. Returns the characters stored, without the
 * terminator, rather than the length the whole result would need; a `size`
 * of 0 stores nothing and returns 0.
 */
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

/** A stub that always fails and returns -1. */
static inline int remove(const char *path) {
    (void)path;
    return -1;
}

/** A stub that always fails and returns -1. */
static inline int rename(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -1;
}

#endif /* _STDIO_H */
