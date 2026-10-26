/*
 * extract.c: copy definitions out of kernel sources unchanged, for a fixture
 * of tests/host/fixtures that compiles a function no host test can compile
 * whole.
 *
 * Usage: extract OUT -f FILE ITEM... [-f FILE ITEM...]...
 *
 * Each ITEM is read from the FILE given before it and written to OUT in the
 * order given, separated by blank lines:
 *
 *     NAME              the whole function NAME, from its first line to the
 *                       brace that closes its body;
 *     struct NAME       the definition of the structure NAME;
 *     #define PREFIX    every #define line whose name begins with PREFIX, or
 *                       with one of the PREFIXes that `|` separates, in the
 *                       file's order.
 *
 * A function or a structure that is not defined exactly once in its file, and
 * a #define that matches no line, stop the program with status 1 and write
 * nothing.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The text written so far, and the file being read. */
static char *out_text, *file_text;
static size_t out_len, out_cap;
static const char *file_path;

static void fail(const char *format, ...)
{
    va_list ap;
    fputs("extract: ", stderr);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void put(const char *text, size_t n)
{
    if (out_len + n + 1 > out_cap) {
        char *grown = realloc(out_text, out_cap = (out_len + n + 1) * 2);
        if (grown == NULL)
            fail("out of memory");
        out_text = grown;
    }
    memcpy(out_text + out_len, text, n);
    out_len += n;
    out_text[out_len] = 0;
}

/* Read `path` whole, with each CR LF and lone CR read as LF. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *text = NULL;
    size_t n = 0, cap = 0;
    int c, cr = 0;

    if (f == NULL)
        fail("%s: cannot read", path);
    while ((c = getc(f)) != EOF) {
        if (c == '\n' && cr) {
            cr = 0;
            continue;
        }
        cr = c == '\r';
        if (n + 2 > cap) {
            char *grown = realloc(text, cap = cap * 2 + 65536);
            if (grown == NULL)
                fail("out of memory");
            text = grown;
        }
        text[n++] = cr ? '\n' : (char)c;
    }
    if (ferror(f))
        fail("%s: cannot read", path);
    fclose(f);
    if (text == NULL && (text = malloc(1)) == NULL)
        fail("out of memory");
    text[n] = 0;
    return text;
}

static int word_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/*
 * The end of the head of function `name` that begins at the line start `p`:
 * a type and storage words of letters, digits, `_`, blanks and `*`, the name
 * after a blank or `*`, its parameters, and the `{` that opens a line after
 * them with no `;` before it. Returns a pointer to that `{`, or NULL.
 */
static const char *head(const char *p, const char *name)
{
    size_t n = strlen(name);
    if (!word_char(*p) || (*p >= '0' && *p <= '9'))
        return NULL;
    for (const char *s = p + 1; word_char(*s) || *s == ' ' || *s == '*'; s++) {
        if ((s[-1] != ' ' && s[-1] != '*') || strncmp(s, name, n) || s[n] != '(')
            continue;
        for (const char *q = s + n + 1; *q && *q != ';'; q++)
            if (q[0] == '\n' && q[1] == '{')
                return q + 1;
    }
    return NULL;
}

/* The end of the comment, string, character constant at `p`, or NULL when
 * none begins there or it does not end. */
static const char *skip(const char *p)
{
    if (p[0] == '/' && p[1] == '*') {
        const char *end = strstr(p + 2, "*/");
        return end ? end + 2 : NULL;
    }
    if (p[0] == '/' && p[1] == '/')
        return p + 2 + strcspn(p + 2, "\n");
    if (*p == '"' || *p == '\'') {
        for (const char *q = p + 1; *q; q++) {
            if (*q == '\\' && q[1])
                q++;
            else if (*q == *p)
                return q + 1;
        }
    }
    return NULL;
}

/* Copy the function `name`, which the file must define once. */
static void function(const char *name)
{
    const char *start = NULL, *brace = NULL, *t = file_text;
    int count = 0;

    while (*t) {
        const char *b = head(t, name);
        if (b) {
            if (count++ == 0) {
                start = t;
                brace = b;
            }
            t = b + 1;
        } else
            t += strcspn(t, "\n") + (t[strcspn(t, "\n")] != 0);
        while (*t && t != file_text && t[-1] != '\n')
            t++;
    }
    if (count != 1)
        fail("%s: function %s is defined %d times", file_path, name, count);
    int depth = 0;
    for (const char *p = brace; *p;) {
        const char *end = skip(p);
        if (end) {
            p = end;
            continue;
        }
        if (*p == '{')
            depth++;
        else if (*p == '}' && --depth == 0) {
            put(start, (size_t)(p + 1 - start));
            return;
        }
        p++;
    }
    fail("%s: function %s does not end", file_path, name);
}

/* Copy the definition of structure `name`, which the file must have once:
 * from a line `struct NAME {` to the first line after it that is `};`. */
static void structure(const char *name)
{
    size_t n = strlen(name);
    const char *start = NULL, *stop = NULL, *t = file_text;
    int count = 0;

    while (*t) {
        const char *end = NULL;
        if (!strncmp(t, "struct ", 7) && !strncmp(t + 7, name, n) && !strncmp(t + 7 + n, " {", 2))
            end = strstr(t + 9 + n, "\n};");
        if (end) {
            if (count++ == 0) {
                start = t;
                stop = end + 3;
            }
            t = end + 3;
        } else
            t += strcspn(t, "\n") + (t[strcspn(t, "\n")] != 0);
        while (*t && t != file_text && t[-1] != '\n')
            t++;
    }
    if (count != 1)
        fail("%s: struct %s is defined %d times", file_path, name, count);
    put(start, (size_t)(stop - start));
}

/* Copy every #define line whose name begins with one of the prefixes of
 * `pattern`, which `|` separates. */
static void defines(const char *pattern)
{
    int found = 0;
    if (strpbrk(pattern, "\\.^$*+?()[]{}"))
        fail("%s: #define %s: a prefix is plain text", file_path, pattern);
    for (const char *t = file_text; *t; t += strcspn(t, "\n") + (t[strcspn(t, "\n")] != 0)) {
        if (strncmp(t, "#define ", 8))
            continue;
        int match = 0;
        for (const char *alt = pattern; !match;) {
            size_t n = strcspn(alt, "|");
            match = !strncmp(t + 8, alt, n);
            if (!alt[n])
                break;
            alt += n + 1;
        }
        if (match) {
            if (found++)
                put("\n", 1);
            put(t, strcspn(t, "\n"));
        }
    }
    if (!found)
        fail("%s: no #define matches %s", file_path, pattern);
}

int main(int argc, char **argv)
{
    int parts = 0;

    if (argc < 5 || strcmp(argv[2], "-f"))
        fail("usage: extract OUT -f FILE ITEM... [-f FILE ITEM...]...");
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) {
            if (++i == argc)
                fail("-f needs a file");
            free(file_text);
            file_path = argv[i];
            file_text = read_file(file_path);
            continue;
        }
        if (parts++)
            put("\n\n", 2);
        if (!strncmp(argv[i], "struct ", 7))
            structure(argv[i] + 7);
        else if (!strncmp(argv[i], "#define ", 8))
            defines(argv[i] + 8);
        else
            function(argv[i]);
    }
    put("\n", 1);
    FILE *f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(out_text, 1, out_len, f) != out_len || fclose(f))
        fail("%s: cannot write", argv[1]);
    free(out_text);
    free(file_text);
    return 0;
}
