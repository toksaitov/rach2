/*
 * stdio.h - Minimal standard I/O for Doom on Rach2
 * Functions implemented in crt0.c
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
    int eof;
    int error;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fflush(FILE *f);

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int puts(const char *s);
int putchar(int c);
int sscanf(const char *str, const char *fmt, ...);

/* Functions not fully implemented but needed by Doom */
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

int vfprintf(FILE *f, const char *fmt, va_list ap);  /* Implemented in crt0.c */

int sprintf(char *str, const char *fmt, ...);   /* Implemented in crt0.c */
int snprintf(char *str, size_t size, const char *fmt, ...);  /* Implemented in crt0.c */
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);  /* Implemented in crt0.c */

static inline int remove(const char *path) {
    (void)path;
    return -1;
}

static inline int rename(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -1;
}

#endif /* _STDIO_H */
