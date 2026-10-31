/*
 * font.c: the packed Unifont that term draws with, at 1x or 2x, and the
 * scale kept in term's configuration file.
 */

/* POSIX's calls, which -std=c99 hides. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "term.h"

#define CONF_KEY "font-scale="

/**
 * Read the whole of `fd` into a buffer of malloc(3), storing its length in
 * `*size`; NULL with errno set on failure. A file that grows while it is
 * read is cut at the size fstat(2) gave.
 */
static unsigned char *read_all(int fd, size_t *size)
{
    struct stat st;
    unsigned char *buf;
    size_t done = 0;
    ssize_t n;

    if (fstat(fd, &st) < 0)
        return NULL;
    if (st.st_size <= 0) {
        errno = EINVAL;
        return NULL;
    }
    buf = malloc((size_t)st.st_size);
    if (!buf)
        return NULL;
    while (done < (size_t)st.st_size) {
        n = read(fd, buf + done, (size_t)st.st_size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EINVAL;
            free(buf);
            return NULL;
        }
        done += (size_t)n;
    }
    *size = done;
    return buf;
}

int term_font_load(struct term_font *f, const char *path, unsigned scale)
{
    int fd, e;

    f->data = NULL;
    f->size = 0;
    if (term_font_scale(f, scale) < 0)
        return -EINVAL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    f->data = read_all(fd, &f->size);
    e = errno;
    close(fd);
    if (!f->data)
        return -e;
    if (unifont_open(&f->index, f->data, (unsigned long)f->size) < 0) {
        term_font_free(f);
        return -EINVAL;
    }
    return 0;
}

const unsigned char *term_font_glyph(const struct term_font *f, uint32_t ucs4, unsigned *width)
{
    const unsigned char *rows;
    int w;

    if (!f->data)
        return NULL;
    rows = unifont_glyph(&f->index, ucs4, &w);
    if (rows)
        *width = (unsigned)w;
    return rows;
}

int term_font_scale(struct term_font *f, unsigned scale)
{
    if (scale != 1 && scale != 2)
        return -EINVAL;
    f->scale = scale;
    f->cell_width = 8 * scale;
    f->cell_height = UNIFONT_HEIGHT * scale;
    return 0;
}

void term_font_free(struct term_font *f)
{
    free(f->data);
    f->data = NULL;
    f->size = 0;
}

/**
 * Read the configuration file `conf` whole, NUL-terminated; NULL with errno
 * set when it cannot be read. `*len` gets its length without the NUL.
 */
static char *read_conf(const char *conf, size_t *len)
{
    unsigned char *data, *text;
    size_t size;
    int fd, e;

    fd = open(conf, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    data = read_all(fd, &size);
    e = errno;
    close(fd);
    if (!data) {
        /* An empty file reads as an empty text. */
        if (e != EINVAL)
            return NULL;
        size = 0;
    }
    text = realloc(data, size + 1);
    if (!text) {
        free(data);
        return NULL;
    }
    text[size] = '\0';
    *len = size;
    return (char *)text;
}

/**
 * The `font-scale=` line of `text`: its start, and in `*end` the byte past
 * its newline or the text's end; NULL when there is none. Blanks before the
 * name are skipped, and a line that begins with `#` is a comment.
 */
static const char *conf_line(const char *text, const char **end)
{
    const char *p = text, *q, *nl;

    while (*p) {
        nl = strchr(p, '\n');
        nl = nl ? nl + 1 : p + strlen(p);
        q = p;
        while (*q == ' ' || *q == '\t')
            q++;
        if (strncmp(q, CONF_KEY, sizeof CONF_KEY - 1) == 0) {
            *end = nl;
            return p;
        }
        p = nl;
    }
    return NULL;
}

unsigned term_font_conf_scale(const char *conf)
{
    const char *line, *end, *v;
    unsigned scale = 0;
    size_t len;
    char *text;

    text = read_conf(conf, &len);
    if (!text)
        return 0;
    line = conf_line(text, &end);
    if (line) {
        v = strchr(line, '=') + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        if ((*v == '1' || *v == '2') && strchr(" \t\r\n", v[1]))
            scale = (unsigned)(*v - '0');
    }
    free(text);
    return scale;
}

int term_font_conf_save(const char *conf, unsigned scale)
{
    const char *line = NULL, *end = NULL;
    char *text, *tmp;
    size_t len = 0;
    FILE *out;
    int r = 0;

    if (scale != 1 && scale != 2)
        return -EINVAL;
    text = read_conf(conf, &len);
    if (!text && errno != ENOENT)
        return -errno;
    if (text)
        line = conf_line(text, &end);
    tmp = malloc(strlen(conf) + sizeof ".new");
    if (!tmp) {
        free(text);
        return -ENOMEM;
    }
    strcpy(tmp, conf);
    strcat(tmp, ".new");
    out = fopen(tmp, "w");
    if (!out) {
        r = -errno;
        goto done;
    }
    if (line) {
        fwrite(text, 1, (size_t)(line - text), out);
        fprintf(out, "%s%u\n", CONF_KEY, scale);
        fputs(end, out);
    } else {
        if (text) {
            fputs(text, out);
            if (len > 0 && text[len - 1] != '\n')
                fputc('\n', out);
        }
        fprintf(out, "%s%u\n", CONF_KEY, scale);
    }
    if (ferror(out))
        r = -EIO;
    if (fclose(out) != 0 && r == 0)
        r = -errno;
    if (r == 0 && rename(tmp, conf) < 0)
        r = -errno;
    if (r < 0)
        unlink(tmp);
done:
    free(tmp);
    free(text);
    return r;
}
