/**
 * prosecheck: the checks on the prose this project writes.
 *
 * One lexer serves all of them, which is the point of putting them in one
 * tool: `columns` measures comment lines, `banned` reads comments and
 * string literals for terms that are wrong at any frequency, `count`
 * reports word and phrase totals for the report and baseline drivers, and
 * `devlog` checks the milestone index against the directories beside it.
 *
 * `columns [PATH ...]` holds comment lines to 80 columns. CLAUDE.md wraps
 * comments at 80 and .clang-format sets 100 for code, so only WHOLE
 * comment lines are measured: a comment trailing a code line is part of a
 * code line and answers to the second rule. A string literal holding a
 * comment delimiter is not a comment either, which is why the scan tracks
 * literals.
 *
 * `banned [PATH ...]` fails on the terms CLAUDE.md's register rule names.
 * It reads comments AND string literals, in every language here, because a
 * person reads a test failure message and it does not matter whether that
 * message came from C or from shell.
 *
 * `count {c|hash|md} FILE ...` prints `words N` and one `phrase N` line
 * per watched phrase, over the files named. The watch list is measured and
 * never gated: a high multiple is a question to ask, not a defect. Prose
 * is counted one comment at a time, each normalized first (its `*`, `//`
 * and `#` decoration removed and its whitespace collapsed), because
 * without that the numbers measure formatting instead of writing: a phrase
 * broken across a line break would not be counted at all, a lone `*` would
 * count as a word, and the two corpora wrap comments differently. Counting
 * per comment also stops a phrase being found across the boundary between
 * two unrelated comments. Strings are excluded here: a string literal is
 * not prose and counting it would move the word totals the rates are
 * computed against.
 *
 * `devlog [DIR]` checks dev-log/Readme.md against the milestone
 * directories. AGENTS.md tells a reader that the last entry is where the
 * tree stands, and nothing a milestone does touches the index, so nothing
 * but a check keeps that sentence true. The rule is equality, not
 * membership: the entries must be exactly the finished milestones, in the
 * directories' numeric order. A directory holding only a plan must have no
 * entry, because an entry states that the milestone happened.
 *
 * `--selftest` runs every rule over fixtures.
 *
 * This file states the register rule's terms, so it is exempt from
 * `banned`, as is that rule's own section in CLAUDE.md.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIMIT 80
#define MAX_FILE (4 * 1024 * 1024)

/* The root Makefile is named here because it is the build's entry point and
   sits in no walked directory. */
static const char *const ROOTS[] = { "rach2", "utils", "ish", "krffs",
                                     "tests", "tools", "mk", "ports/doom",
                                     "Makefile" };
static const char *const TOP_MD[] = { "CLAUDE.md", "AGENTS.md", "Readme.md" };
static const char *const SKIP_DIRS[] = { "vendor", "build", ".git" };
static const char *const SKIP_PATHS[] = { "ports/doom/src" };

/* Wrong at any frequency. Every one of these was either named in the
   register rule or named by a reader outside this project who had never
   seen it. A term belongs here only if it has no legitimate use in this
   domain: "trap" collides with the shell builtin, "sight" with the idiom
   "anywhere in sight", "bench" with a test bench, "gate" with the hardware
   gates at port 0x61, "surface" with an attack surface, and "era" with
   ordinary English. Of the seven shorthand nouns the rule lists, only
   "seam" and "timebox" have no legitimate use in this tree. */
static const char *const BANNED[] = {
    "load-bearing", "load bearing", "worth stating plainly", "worth noting",
    "blast radius", "tripwire", "timebox", "full stop", "seam"
};
#define N_BANNED (int) (sizeof(BANNED) / sizeof(BANNED[0]))

/* Ordinary phrases, measured and never gated. The last two are the forms
   milestone 26 recast `rather than` into, watched for the obvious reason:
   a cure that concentrates in one phrase is the same fault wearing
   different words. */
static const char *const WATCH[] = {
    "carries", "holds", "rather than", "which is", "not a", "honest",
    "the point", "in practice", "worth", "note that", ", not", "instead of"
};
#define N_WATCH (int) (sizeof(WATCH) / sizeof(WATCH[0]))

/* This file names what it bans; CLAUDE.md states the rule in one section
   and the rest of that file is checked normally. */
static const char *const EXEMPT_FILES[] = { "tools/verify/prosecheck.c" };
#define EXEMPT_MD "CLAUDE.md"
#define EXEMPT_HEADING "### Register rule"

static char repo[1024];

/* ---- a growable buffer of normalized spans ---- */

struct spans {
    char **item;
    int n, cap;
};

static void *xalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL) {
        fprintf(stderr, "prosecheck: out of memory\n");
        exit(2);
    }
    return p;
}

static void spans_push(struct spans *s, char *text)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->item = realloc(s->item, (size_t) s->cap * sizeof(char *));
        if (s->item == NULL)
            exit(2);
    }
    s->item[s->n++] = text;
}

static void spans_free(struct spans *s)
{
    int i;

    for (i = 0; i < s->n; i++)
        free(s->item[i]);
    free(s->item);
    s->item = NULL;
    s->n = s->cap = 0;
}

/* One comment, with its decoration gone and its whitespace collapsed:
   leading `*`, `//` or `#` runs at the start of each line, then every run
   of whitespace becomes one space. Returns a fresh string, or NULL when
   nothing is left. */
static char *normalize(const char *text, size_t len)
{
    char *out = xalloc(len + 1);
    size_t i = 0, n = 0;
    int at_line_start = 1;

    while (i < len) {
        if (at_line_start) {
            size_t j = i;

            while (j < len && (text[j] == ' ' || text[j] == '\t'))
                j++;
            if (j < len && (text[j] == '*' || text[j] == '#')) {
                char mark = text[j];

                while (j < len && text[j] == mark)
                    j++;
                if (j < len && (text[j] == ' ' || text[j] == '\t'))
                    j++;
                i = j;
            } else if (j + 1 < len && text[j] == '/' && text[j + 1] == '/') {
                while (j < len && text[j] == '/')
                    j++;
                if (j < len && (text[j] == ' ' || text[j] == '\t'))
                    j++;
                i = j;
            }
            at_line_start = 0;
            continue;
        }
        if (text[i] == '\n') {
            at_line_start = 1;
            if (n > 0 && out[n - 1] != ' ')
                out[n++] = ' ';
            i++;
            continue;
        }
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r') {
            if (n > 0 && out[n - 1] != ' ')
                out[n++] = ' ';
            i++;
            continue;
        }
        out[n++] = text[i++];
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
    if (n == 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* ---- the C lexer: comments and string literals, one state machine ---- */

enum span_kind { SPAN_COMMENT, SPAN_STRING };

typedef void (*span_fn)(void *ctx, enum span_kind kind, int line,
                        const char *body, size_t len);

/* Every comment and string in C source. One state machine, so a `/ *`
   inside a string and a quote inside a comment are both read correctly. */
static void c_spans(const char *text, size_t n, span_fn emit, void *ctx)
{
    size_t i = 0, start = 0;
    int line = 1, start_line = 1;
    enum { CODE, BLOCK, LINE_COMMENT, STRING, CHARLIT } state = CODE;

    while (i < n) {
        char ch = text[i];
        char nxt = i + 1 < n ? text[i + 1] : '\0';

        if (state == CODE) {
            if (ch == '\n') {
                line++;
                i++;
            } else if (ch == '/' && nxt == '*') {
                state = BLOCK;
                start = i + 2;
                start_line = line;
                i += 2;
            } else if (ch == '/' && nxt == '/') {
                state = LINE_COMMENT;
                start = i + 2;
                start_line = line;
                i += 2;
            } else if (ch == '"') {
                state = STRING;
                start = i + 1;
                start_line = line;
                i++;
            } else if (ch == '\'') {
                state = CHARLIT;
                start = i + 1;
                start_line = line;
                i++;
            } else {
                i++;
            }
            continue;
        }
        if (state == BLOCK) {
            if (ch == '*' && nxt == '/') {
                emit(ctx, SPAN_COMMENT, start_line, text + start, i - start);
                state = CODE;
                i += 2;
                continue;
            }
            if (ch == '\n')
                line++;
            i++;
            continue;
        }
        if (state == LINE_COMMENT) {
            if (ch == '\n') {
                emit(ctx, SPAN_COMMENT, start_line, text + start, i - start);
                state = CODE;
                line++;
                i++;
                continue;
            }
            i++;
            continue;
        }
        /* A literal. A backslash escapes the next character, so a trailing
           backslash cannot end the literal early. */
        if (ch == '\\') {
            if (i + 1 < n && text[i + 1] == '\n')
                line++;
            i += 2;
            continue;
        }
        if (ch == '\n') {
            line++;
            i++;
            continue;
        }
        if (state == STRING && ch == '"') {
            emit(ctx, SPAN_STRING, start_line, text + start, i - start);
            state = CODE;
            i++;
            continue;
        }
        if (state == CHARLIT && ch == '\'') {
            state = CODE;
            i++;
            continue;
        }
        i++;
    }
    if (state == BLOCK || state == LINE_COMMENT)
        emit(ctx, SPAN_COMMENT, start_line, text + start, n - start);
}

/* The lines carrying a `#` comment, outside quotes: shell, make and Python
   as this tree writes them. A triple-quoted Python string spans lines,
   which the per-line quote tracking cannot see, so those lines are found
   first and skipped; without that, a fixture holding a deliberately
   over-width `#` line reads as a comment. Fills `mark[]`, one byte per
   line, 1-based. */
static void hash_comment_lines(const char *text, size_t n, char *mark,
                               int max_lines)
{
    int line = 1, depth = 0;
    size_t i = 0;

    memset(mark, 0, (size_t) max_lines + 2);
    /* Pass one: the lines inside a triple-quoted string. */
    while (i < n && line <= max_lines) {
        size_t end = i;
        int marks = 0;
        size_t j;

        while (end < n && text[end] != '\n')
            end++;
        for (j = i; j + 2 < end; j++)
            if ((text[j] == '"' && text[j + 1] == '"' && text[j + 2] == '"') ||
                (text[j] == '\'' && text[j + 1] == '\'' &&
                 text[j + 2] == '\'')) {
                marks++;
                j += 2;
            }
        if (depth)
            mark[line] = 2;
        if (marks % 2) {
            depth = depth ? 0 : 1;
            mark[line] = 2;
        }
        i = end + 1;
        line++;
    }
    /* Pass two: a `#` outside quotes on a line pass one did not skip. */
    line = 1;
    i = 0;
    while (i < n && line <= max_lines) {
        size_t end = i, j;
        int in_single = 0, in_double = 0;

        while (end < n && text[end] != '\n')
            end++;
        if (mark[line] == 2) {
            mark[line] = 0;
            i = end + 1;
            line++;
            continue;
        }
        for (j = i; j < end; j++) {
            if (text[j] == '\\') {
                j++;
                continue;
            }
            if (text[j] == '\'' && !in_double)
                in_single = !in_single;
            else if (text[j] == '"' && !in_single)
                in_double = !in_double;
            else if (text[j] == '#' && !in_single && !in_double) {
                mark[line] = 1;
                break;
            }
        }
        i = end + 1;
        line++;
    }
}

/* ---- file reading and the walk ---- */

/* Counts the files the walk reached but could not read. A file skipped in
   silence is a file that passes every rule, so the count is reported and
   fails the run. */
static long unread;

static char *read_text(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (f == NULL) {
        fprintf(stderr, "prosecheck: %s: %s; it was not checked\n", path,
                strerror(errno));
        unread++;
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "prosecheck: %s: cannot be measured; it was not "
                "checked\n", path);
        unread++;
        fclose(f);
        return NULL;
    }
    if (n > MAX_FILE) {
        fprintf(stderr, "prosecheck: %s: %ld bytes, past the %d this tool "
                "reads; it was not checked\n", path, n, MAX_FILE);
        unread++;
        fclose(f);
        return NULL;
    }
    buf = xalloc((size_t) n + 1);
    if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
        fprintf(stderr, "prosecheck: %s: short read; it was not checked\n",
                path);
        unread++;
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = (size_t) n;
    return buf;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);

    return n >= m && strcmp(s + n - m, suffix) == 0;
}

enum lang { LANG_NONE, LANG_C, LANG_HASH, LANG_MD };

static enum lang lang_of(const char *path)
{
    if (ends_with(path, ".c") || ends_with(path, ".h"))
        return LANG_C;
    /* A makefile is hash-commented like a shell script, and the build's own
       files answer to the same 80-column rule as the code they build. Named
       explicitly because they carry no extension. */
    if (ends_with(path, ".sh") || ends_with(path, ".mk") ||
        ends_with(path, ".py") || ends_with(path, "/Makefile") ||
        strcmp(path, "Makefile") == 0)
        return LANG_HASH;
    if (ends_with(path, ".md"))
        return LANG_MD;
    return LANG_NONE;
}

/* The path relative to the repository root, for messages. */
static const char *relpath(const char *path)
{
    size_t n = strlen(repo);

    if (strncmp(path, repo, n) == 0 && path[n] == '/')
        return path + n + 1;
    return path;
}

typedef void (*file_fn)(void *ctx, const char *path);

/* `is_top` marks the path the caller named. The skipped paths are skipped
   as descendants, not as roots: `ports/doom/src` is upstream Doom and is
   left out of this project's prose, and it is also the corpus the report
   reads, which it asks for by name. */
static void walk_at(const char *root, file_fn visit, void *ctx, int md_too,
                    int is_top)
{
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    struct spans names = { NULL, 0, 0 };
    int i;

    if (stat(root, &st) != 0) {
        fprintf(stderr, "prosecheck: %s: %s; nothing under it was "
                "checked\n", root, strerror(errno));
        unread++;
        return;
    }
    if (S_ISREG(st.st_mode)) {
        visit(ctx, root);
        return;
    }
    if (!is_top)
        for (i = 0; i < (int) (sizeof(SKIP_PATHS) / sizeof(SKIP_PATHS[0]));
             i++)
            if (strstr(root, SKIP_PATHS[i]) != NULL)
                return;
    dir = opendir(root);
    if (dir == NULL) {
        fprintf(stderr, "prosecheck: %s: %s; nothing under it was "
                "checked\n", root, strerror(errno));
        unread++;
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char full[2048];
        int skip = 0;

        if (ent->d_name[0] == '.')
            continue;
        for (i = 0; i < (int) (sizeof(SKIP_DIRS) / sizeof(SKIP_DIRS[0])); i++)
            if (strcmp(ent->d_name, SKIP_DIRS[i]) == 0)
                skip = 1;
        if (skip)
            continue;
        if ((size_t) snprintf(full, sizeof(full), "%s/%s", root,
                              ent->d_name) >= sizeof(full)) {
            fprintf(stderr, "prosecheck: the path under %s named %s is "
                    "longer than %lu bytes; it was not checked\n", root,
                    ent->d_name, (unsigned long) sizeof(full));
            unread++;
            continue;
        }
        spans_push(&names, strdup(full));
    }
    closedir(dir);
    /* Sorted, so two runs of this tool report in one order. */
    for (i = 1; i < names.n; i++) {
        char *key = names.item[i];
        int j = i - 1;

        while (j >= 0 && strcmp(names.item[j], key) > 0) {
            names.item[j + 1] = names.item[j];
            j--;
        }
        names.item[j + 1] = key;
    }
    for (i = 0; i < names.n; i++) {
        /* lstat, not stat: a symbolic link to a directory above this one
           would walk the tree again and again. There are none in this
           tree, so one that appears is reported rather than followed. */
        if (lstat(names.item[i], &st) != 0) {
            fprintf(stderr, "prosecheck: %s: %s; it was not checked\n",
                    names.item[i], strerror(errno));
            unread++;
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            fprintf(stderr, "prosecheck: %s is a symbolic link; it was not "
                    "checked\n", names.item[i]);
            unread++;
        } else if (S_ISDIR(st.st_mode))
            walk_at(names.item[i], visit, ctx, md_too, 0);
        else if (S_ISREG(st.st_mode)) {
            enum lang l = lang_of(names.item[i]);

            if (l == LANG_C || l == LANG_HASH || (md_too && l == LANG_MD))
                visit(ctx, names.item[i]);
        }
    }
    spans_free(&names);
}

static void walk(const char *root, file_fn visit, void *ctx, int md_too)
{
    walk_at(root, visit, ctx, md_too, 1);
}

/* ---- collecting prose ---- */

struct collect {
    struct spans out;
    int want_strings;
};

static void collect_span(void *ctx, enum span_kind kind, int line,
                         const char *body, size_t len)
{
    struct collect *c = ctx;
    char *text;

    (void) line;
    if (kind == SPAN_STRING && !c->want_strings)
        return;
    text = normalize(body, len);
    if (text != NULL)
        spans_push(&c->out, text);
}

/* The `#` comments and quoted strings of a shell, make or Python file,
   line by line. A string spanning lines is not tracked, which costs
   nothing here: it is the same per-line scan the width check uses. */
static void hash_spans(const char *text, size_t n, span_fn emit, void *ctx)
{
    char *mark;
    int line = 1, max_lines = 1;
    size_t i;

    for (i = 0; i < n; i++)
        if (text[i] == '\n')
            max_lines++;
    mark = xalloc((size_t) max_lines + 2);
    hash_comment_lines(text, n, mark, max_lines);
    i = 0;
    while (i < n) {
        size_t end = i, j;
        char quote = '\0';
        size_t start = 0;

        while (end < n && text[end] != '\n')
            end++;
        for (j = i; j < end; j++) {
            if (text[j] == '\\') {
                j++;
                continue;
            }
            if (quote != '\0') {
                if (text[j] == quote) {
                    emit(ctx, SPAN_STRING, line, text + start, j - start);
                    quote = '\0';
                }
                continue;
            }
            if (text[j] == '"' || text[j] == '\'') {
                quote = text[j];
                start = j + 1;
                continue;
            }
            if (text[j] == '#' && line <= max_lines && mark[line]) {
                emit(ctx, SPAN_COMMENT, line, text + j + 1, end - j - 1);
                break;
            }
        }
        i = end + 1;
        line++;
    }
    free(mark);
}

/* Markdown paragraphs, with code and any exempt section removed. The
   exempt section ends at the next heading of the same level or higher;
   Markdown allows up to three spaces before a `#`, so a heading matched by
   its first column alone could be indented by one space and the exemption
   would then run to the end of the file. */
static void md_spans(const char *text, size_t n, const char *path,
                     span_fn emit, void *ctx)
{
    const char *skip_heading = NULL;
    int skip_level = 0, fenced = 0, exempt = 0;
    char para[65536];
    size_t plen = 0;
    size_t i = 0;

    if (path != NULL && strcmp(relpath(path), EXEMPT_MD) == 0) {
        skip_heading = EXEMPT_HEADING;
        skip_level = 3;
    } else if (path != NULL && ends_with(path, "/" EXEMPT_MD)) {
        skip_heading = EXEMPT_HEADING;
        skip_level = 3;
    }
    while (i <= n) {
        size_t end = i;
        char line[8192];
        const char *trimmed;
        size_t len;

        while (end < n && text[end] != '\n')
            end++;
        len = end - i;
        if (len >= sizeof(line))
            len = sizeof(line) - 1;
        memcpy(line, text + i, len);
        line[len] = '\0';
        trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t')
            trimmed++;

        if (strncmp(trimmed, "```", 3) == 0) {
            fenced = !fenced;
        } else if (fenced || strncmp(line, "    ", 4) == 0) {
            /* Code, not prose. */
        } else {
            int is_heading = 0, level = 0;
            const char *p = line;
            int lead = 0;

            while (*p == ' ' && lead < 3) {
                p++;
                lead++;
            }
            while (p[level] == '#')
                level++;
            if (level >= 1 && level <= 6 && (p[level] == ' ' ||
                                             p[level] == '\t'))
                is_heading = 1;

            if (skip_heading != NULL) {
                if (strncmp(trimmed, skip_heading,
                            strlen(skip_heading)) == 0) {
                    exempt = 1;
                    i = end + 1;
                    continue;
                }
                if (exempt && is_heading && level <= skip_level)
                    exempt = 0;
                if (exempt) {
                    i = end + 1;
                    continue;
                }
            }
            if (*trimmed != '\0') {
                if (plen + len + 2 < sizeof(para)) {
                    if (plen > 0)
                        para[plen++] = ' ';
                    memcpy(para + plen, line, len);
                    plen += len;
                    para[plen] = '\0';
                }
            } else if (plen > 0) {
                emit(ctx, SPAN_COMMENT, 0, para, plen);
                plen = 0;
                para[0] = '\0';
            }
        }
        if (end >= n)
            break;
        i = end + 1;
    }
    if (plen > 0)
        emit(ctx, SPAN_COMMENT, 0, para, plen);
}

/* The normalized prose of one file, by language. */
static void prose_of(const char *path, const char *text, size_t n,
                     int strings, struct spans *out)
{
    struct collect c;

    c.out.item = out->item;
    c.out.n = out->n;
    c.out.cap = out->cap;
    c.want_strings = strings;
    switch (lang_of(path)) {
    case LANG_C:
        c_spans(text, n, collect_span, &c);
        break;
    case LANG_HASH:
        hash_spans(text, n, collect_span, &c);
        break;
    case LANG_MD:
        md_spans(text, n, path, collect_span, &c);
        break;
    default:
        break;
    }
    *out = c.out;
}

/* ---- phrase counting ---- */

static int is_word(int c)
{
    return isalnum((unsigned char) c) || c == '_';
}

/* A word boundary at `at` in `text`, the way a regular expression's `\b`
   decides it: one side a word character and the other not, with the ends
   of the string counting as not. */
static int boundary(const char *text, size_t len, size_t at)
{
    int before = at > 0 && is_word(text[at - 1]);
    int after = at < len && is_word(text[at]);

    return before != after;
}

/* Occurrences of `phrase` in one normalized span, case-insensitive and
   bounded at both ends, the way the report has always counted them. */
static long count_in(const char *span, const char *phrase)
{
    size_t slen = strlen(span), plen = strlen(phrase);
    size_t i;
    long n = 0;

    if (plen == 0 || plen > slen)
        return 0;
    for (i = 0; i + plen <= slen; i++) {
        size_t j;

        for (j = 0; j < plen; j++)
            if (tolower((unsigned char) span[i + j]) !=
                tolower((unsigned char) phrase[j]))
                break;
        if (j < plen)
            continue;
        if (!boundary(span, slen, i) || !boundary(span, slen, i + plen))
            continue;
        n++;
    }
    return n;
}

static long count_spans(const struct spans *s, const char *phrase)
{
    long n = 0;
    int i;

    for (i = 0; i < s->n; i++)
        n += count_in(s->item[i], phrase);
    return n;
}

static long words_of(const struct spans *s)
{
    long n = 0;
    int i;

    for (i = 0; i < s->n; i++) {
        const char *p = s->item[i];

        while (*p != '\0') {
            while (*p == ' ')
                p++;
            if (*p == '\0')
                break;
            n++;
            while (*p != '\0' && *p != ' ')
                p++;
        }
    }
    return n;
}

/* ---- columns: comment lines wider than the limit ---- */

struct width_ctx {
    long over, files;
};

/* The comment lines of one file, and which of them are too wide. What
   opens a comment LINE differs by language: `#` is a comment in shell,
   make and Python, and a preprocessor directive in C, so
   a preprocessor directive carrying a trailing comment is a code line. */
struct marks {
    char *line;
    int max;
};

static void mark_comment_lines(void *ctx, enum span_kind kind, int line,
                               const char *body, size_t len)
{
    struct marks *m = ctx;
    size_t i;
    int last = line;

    if (kind != SPAN_COMMENT)
        return;
    for (i = 0; i < len; i++)
        if (body[i] == '\n')
            last++;
    for (; line <= last && line <= m->max; line++)
        m->line[line] = 1;
}

static void check_width(void *ctx, const char *path)
{
    struct width_ctx *w = ctx;
    size_t n;
    char *text = read_text(path, &n);
    struct marks m;
    enum lang l = lang_of(path);
    int line = 1, max_lines = 1, reported = 0;
    size_t i;

    if (text == NULL)
        return;
    if (l != LANG_C && l != LANG_HASH) {
        free(text);
        return;
    }
    for (i = 0; i < n; i++)
        if (text[i] == '\n')
            max_lines++;
    m.line = xalloc((size_t) max_lines + 2);
    m.max = max_lines;
    memset(m.line, 0, (size_t) max_lines + 2);
    if (l == LANG_C) {
        c_spans(text, n, mark_comment_lines, &m);
    } else {
        hash_comment_lines(text, n, m.line, max_lines);
    }
    i = 0;
    while (i < n) {
        size_t end = i, width;
        const char *p;

        while (end < n && text[end] != '\n')
            end++;
        width = end - i;
        if (line <= max_lines && m.line[line] && width > LIMIT) {
            p = text + i;
            while (p < text + end && (*p == ' ' || *p == '\t'))
                p++;
            if ((l == LANG_C &&
                 (p[0] == '*' || (p[0] == '/' && (p[1] == '*' ||
                                                  p[1] == '/')))) ||
                (l == LANG_HASH && p[0] == '#')) {
                fprintf(stderr, "%s:%d: comment line is %lu columns\n",
                        relpath(path), line, (unsigned long) width);
                w->over++;
                reported = 1;
            }
        }
        i = end + 1;
        line++;
    }
    if (reported)
        w->files++;
    free(m.line);
    free(text);
}

/* ---- banned: terms wrong at any frequency ---- */

struct banned_ctx {
    long findings;
};

static void check_banned(void *ctx, const char *path)
{
    struct banned_ctx *b = ctx;
    struct spans prose = { NULL, 0, 0 };
    size_t n;
    char *text;
    int i;

    for (i = 0; i < (int) (sizeof(EXEMPT_FILES) / sizeof(EXEMPT_FILES[0])); i++)
        if (strcmp(relpath(path), EXEMPT_FILES[i]) == 0)
            return;
    text = read_text(path, &n);
    if (text == NULL)
        return;
    prose_of(path, text, n, 1, &prose);
    for (i = 0; i < N_BANNED; i++) {
        long uses = count_spans(&prose, BANNED[i]);

        if (uses > 0) {
            printf("%s: %ld use(s) of \"%s\"\n", relpath(path), uses,
                   BANNED[i]);
            b->findings += uses;
        }
    }
    spans_free(&prose);
    free(text);
}

/* ---- count: the word and phrase totals the report drivers tabulate ---- */

struct count_ctx {
    struct spans prose;
    enum lang want;
};

static void collect_file(void *ctx, const char *path)
{
    struct count_ctx *c = ctx;
    size_t n;
    char *text;
    int i;

    if (c->want != LANG_NONE && lang_of(path) != c->want)
        return;
    /* The file stating the register rule is left out of the counts as
       well as of the ban: its word totals are the rule's own text, and
       counting them would move the rates every phrase is measured
       against. */
    for (i = 0; i < (int) (sizeof(EXEMPT_FILES) / sizeof(EXEMPT_FILES[0])); i++)
        if (strcmp(relpath(path), EXEMPT_FILES[i]) == 0)
            return;
    text = read_text(path, &n);
    if (text == NULL)
        return;
    prose_of(path, text, n, 0, &c->prose);
    free(text);
}

/* ---- devlog: the index against the milestone directories ---- */

struct milestone {
    char name[128];
    int number;
    int finished;
};

/* Writes `a/b` followed by `tail` to `dst`, or exits when the path does
   not fit: a shorter path would name another file. */
static void join_path(char *dst, size_t cap, const char *a, const char *b,
                      const char *tail)
{
    if (snprintf(dst, cap, "%s/%s%s", a, b, tail) >= (int) cap) {
        fprintf(stderr, "prosecheck: path too long: %s/%s%s\n", a, b, tail);
        exit(2);
    }
}

static int read_milestones(const char *devlog, struct milestone *out, int max)
{
    DIR *dir = opendir(devlog);
    struct dirent *ent;
    int n = 0, i, j;

    if (dir == NULL)
        return -1;
    while ((ent = readdir(dir)) != NULL) {
        char full[1024];
        struct stat st;
        const char *p = ent->d_name;
        int ok = 1;

        /* A milestone directory: two digits, a dash, a lowercase name. */
        if (!isdigit((unsigned char) p[0]) || !isdigit((unsigned char) p[1]) ||
            p[2] != '-' || p[3] == '\0')
            continue;
        for (i = 3; p[i] != '\0'; i++)
            if (!islower((unsigned char) p[i]) &&
                !isdigit((unsigned char) p[i]) && p[i] != '-')
                ok = 0;
        if (!ok)
            continue;
        join_path(full, sizeof(full), devlog, ent->d_name, "");
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (n == max) {
            fprintf(stderr, "prosecheck: more than %d milestone "
                    "directories; the rest would go unchecked\n", max);
            exit(2);
        }
        if (strlen(ent->d_name) >= sizeof(out[n].name)) {
            fprintf(stderr, "prosecheck: milestone directory name too long: "
                    "%s\n", ent->d_name);
            exit(2);
        }
        strcpy(out[n].name, ent->d_name);
        out[n].number = (p[0] - '0') * 10 + (p[1] - '0');
        out[n].finished = 0;
        join_path(full, sizeof(full), devlog, ent->d_name, "/Readme.md");
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode))
            out[n].finished = 1;
        join_path(full, sizeof(full), devlog, ent->d_name, "/results.md");
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode))
            out[n].finished = 1;
        n++;
    }
    closedir(dir);
    for (i = 1; i < n; i++) {
        struct milestone key = out[i];

        j = i - 1;
        while (j >= 0 && out[j].number > key.number) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

/* The milestone names the index lists, in order, ignoring fenced blocks.
   An entry is `* `NN-name`: ...` at the left margin: an indented bullet is
   a sub-item, and a name without the colon is prose that mentions a
   milestone. */
static int read_entries(const char *text, size_t n, char names[][128],
                        int max)
{
    size_t i = 0;
    int count = 0, fenced = 0;

    while (i < n) {
        size_t end = i;
        const char *p = text + i;
        const char *t;

        while (end < n && text[end] != '\n')
            end++;
        t = p;
        while (t < text + end && (*t == ' ' || *t == '\t'))
            t++;
        if ((t + 2 < text + end) &&
            ((t[0] == '`' && t[1] == '`' && t[2] == '`') ||
             (t[0] == '~' && t[1] == '~' && t[2] == '~'))) {
            fenced = !fenced;
            i = end + 1;
            continue;
        }
        if (fenced) {
            i = end + 1;
            continue;
        }
        if (p[0] == '*' && (p[1] == ' ' || p[1] == '\t')) {
            const char *q = p + 1;

            while (q < text + end && (*q == ' ' || *q == '\t'))
                q++;
            if (q < text + end && *q == '`') {
                const char *name = ++q;

                while (q < text + end && *q != '`')
                    q++;
                if (q < text + end && *q == '`') {
                    const char *after = q + 1;
                    size_t len = (size_t) (q - name);

                    while (after < text + end &&
                           (*after == ' ' || *after == '\t'))
                        after++;
                    if (after < text + end && *after == ':' && len > 3 &&
                        len < 128 && isdigit((unsigned char) name[0]) &&
                        isdigit((unsigned char) name[1]) && name[2] == '-') {
                        if (count == max) {
                            fprintf(stderr, "prosecheck: more than %d index "
                                    "entries; the rest would go unread\n",
                                    max);
                            exit(2);
                        }
                        memcpy(names[count], name, len);
                        names[count][len] = '\0';
                        count++;
                    }
                }
            }
        }
        i = end + 1;
    }
    return count;
}

/* The complaints about one index; returns how many were printed. */
static int judge_devlog(struct milestone *dirs, int n_dirs,
                        char entries[][128], int n_entries,
                        struct spans *problems)
{
    char expected[256][128];
    int n_expected = 0;
    int i, j;

    for (i = 0; i < n_dirs; i++)
        if (dirs[i].finished)
            snprintf(expected[n_expected++], 128, "%s", dirs[i].name);

    for (i = 0; i < n_entries; i++) {
        int known = -1;

        for (j = 0; j < n_dirs; j++)
            if (strcmp(dirs[j].name, entries[i]) == 0)
                known = j;
        if (known < 0) {
            char msg[512];

            snprintf(msg, sizeof(msg), "the index names %s, which is not a "
                     "directory", entries[i]);
            spans_push(problems, strdup(msg));
        } else if (!dirs[known].finished) {
            char msg[512];

            snprintf(msg, sizeof(msg), "the index names %s, which has a plan "
                     "and no record, so the work is not done and the entry "
                     "states that it is", entries[i]);
            spans_push(problems, strdup(msg));
        }
    }
    for (i = 0; i < n_entries; i++)
        for (j = 0; j < i; j++)
            if (strcmp(entries[i], entries[j]) == 0) {
                char msg[512];

                snprintf(msg, sizeof(msg), "the index names %s more than "
                         "once", entries[i]);
                spans_push(problems, strdup(msg));
                break;
            }
    for (i = 0; i < n_expected; i++) {
        int seen = 0;

        for (j = 0; j < n_entries; j++)
            if (strcmp(entries[j], expected[i]) == 0)
                seen = 1;
        if (!seen) {
            char msg[512];

            snprintf(msg, sizeof(msg), "dev-log/%.127s is finished and has "
                     "no entry in the index", expected[i]);
            spans_push(problems, strdup(msg));
        }
    }
    /* Order is the half a membership check misses, reported against the
       first position that differs: one displaced entry shifts everything
       after it. */
    if (problems->n == 0) {
        int same = n_entries == n_expected;

        for (i = 0; same && i < n_entries; i++)
            if (strcmp(entries[i], expected[i]) != 0)
                same = 0;
        if (!same) {
            int limit = n_entries < n_expected ? n_entries : n_expected;
            int found = 0;

            for (i = 0; i < limit; i++)
                if (strcmp(entries[i], expected[i]) != 0) {
                    char msg[512];

                    snprintf(msg, sizeof(msg), "the index's entry %d is "
                             "%.127s where the directories give %.127s; the "
                             "entries must "
                             "be in the milestones' numeric order, because a "
                             "reader takes the last one as the current state",
                             i + 1, entries[i], expected[i]);
                    spans_push(problems, strdup(msg));
                    found = 1;
                    break;
                }
            if (!found) {
                char msg[512];

                snprintf(msg, sizeof(msg), "the index lists %d entries and "
                         "there are %d finished milestones", n_entries,
                         n_expected);
                spans_push(problems, strdup(msg));
            }
        }
    }
    return problems->n;
}

/* ---- the selftest ---- */

static int st_bad;

static void st_check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "prosecheck: selftest: %s\n", what);
        st_bad++;
    }
}

static const char SELFTEST_C[] =
    "/* a short header */\n"
    "/* a block comment line made deliberately longer than eighty columns "
    "......... */\n"
    "int x = 1;  /* a trailing comment on a code line, over eighty, not a "
    "comment line */\n"
    "const char *s = \"/* not a comment: a delimiter inside a string, padded "
    "past eighty */\";\n"
    "const char *t = \"// also not a comment, padded well past eighty columns "
    "as well ....\";\n"
    "char q = '\"';  /* a quote inside a char literal must not open a "
    "string */\n";

static const char SELFTEST_SH[] =
    "#!/bin/sh\n"
    "# a comment line written deliberately to run well past the eighty column "
    "rule ....\n"
    "echo \"# not a comment: a hash inside double quotes, padded out past "
    "eighty ......\"\n"
    "echo 'a # inside single quotes is not a comment either, padded past "
    "eighty ......'\n";

static const char PROSE_C[] =
    "/* a comment naming a load-bearing thing */\n"
    "const char *s = \"the seam was reached\";\n"
    "int x = 1;  /* plain */\n"
    "/* a phrase broken by a line break: rather\n"
    " * than counting it, an unnormalized scan misses it\n"
    " */\n"
    "/* ends with rather */\n"
    "/* than begins the next one */\n";

static const char PROSE_SH[] =
    "#!/bin/sh\n"
    "# an ordinary comment\n"
    "echo \"the seam failed\"          # a message a person reads\n";

static const char PROSE_MD[] =
    "# Doc\n\n"
    "A sentence with worth noting in it.\n\n"
    "```\n"
    "a fenced block with load-bearing inside\n"
    "```\n";

static int any_span_has(const struct spans *s, const char *needle)
{
    int i;

    for (i = 0; i < s->n; i++)
        if (strstr(s->item[i], needle) != NULL)
            return 1;
    return 0;
}

static char st_dir[256];

/* The selftest's fixtures are flat files in one directory, so removing
   them and the directory is the whole of the cleanup. */
static void st_clean(void)
{
    DIR *dir = opendir(st_dir);
    struct dirent *ent;

    if (dir != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            char path[512];

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof(path), "%s/%s", st_dir, ent->d_name);
            unlink(path);
        }
        closedir(dir);
    }
    rmdir(st_dir);
}

static char *st_write(const char *name, const char *body)
{
    static char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", st_dir, name);
    f = fopen(path, "wb");
    if (f == NULL)
        exit(2);
    fputs(body, f);
    fclose(f);
    return path;
}

static void st_widths(const char *name, const char *body, int want_line)
{
    struct width_ctx w = { 0, 0 };
    char path[512];
    char msg[256];

    snprintf(path, sizeof(path), "%s", st_write(name, body));
    /* The findings go to stderr; what is asserted is the count, which is
       one over-width comment line in each fixture. */
    check_width(&w, path);
    snprintf(msg, sizeof(msg), "%s: expected 1 over-width comment line, got "
             "%ld", name, w.over);
    st_check(w.over == 1, msg);
    (void) want_line;
}

static int selftest(void)
{
    struct spans only = { NULL, 0, 0 }, both = { NULL, 0, 0 };
    struct spans sh = { NULL, 0, 0 }, md = { NULL, 0, 0 };
    char *path;
    size_t n;
    char *text;
    FILE *devnull;
    FILE *hold;

    st_bad = 0;
    snprintf(st_dir, sizeof(st_dir), "%s/prosecheck-selftest.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (mkdtemp(st_dir) == NULL) {
        fprintf(stderr, "prosecheck: cannot make a temporary directory\n");
        return 2;
    }

    /* The width rule. Its findings are the point of the fixtures, so they
       are counted with the report itself sent to /dev/null. */
    devnull = fopen("/dev/null", "w");
    hold = stderr;
    if (devnull != NULL)
        stderr = devnull;
    st_widths("t.c", SELFTEST_C, 2);
    st_widths("t.sh", SELFTEST_SH, 2);
    if (devnull != NULL) {
        stderr = hold;
        fclose(devnull);
    }

    /* The prose rules. */
    path = st_write("p.c", PROSE_C);
    text = read_text(path, &n);
    prose_of(path, text, n, 0, &only);
    prose_of(path, text, n, 1, &both);
    st_check(!any_span_has(&only, "seam"),
             "a string literal leaked into comment prose");
    st_check(any_span_has(&only, "load-bearing"),
             "missed a banned term in a comment");
    st_check(any_span_has(&both, "seam"),
             "a string literal was not scanned for the banned terms");
    st_check(count_spans(&only, "rather than") == 1,
             "a phrase broken across lines was not counted once");
    st_check(count_spans(&only, "than begins") == 1 &&
             count_spans(&only, "rather than begins") == 0,
             "a phrase was matched across two separate comments");
    {
        int i, bare = 0;

        for (i = 0; i < only.n; i++)
            if (strcmp(only.item[i], "*") == 0 ||
                strcmp(only.item[i], "//") == 0 ||
                strcmp(only.item[i], "#") == 0)
                bare = 1;
        st_check(!bare, "comment decoration was counted as a word");
    }
    free(text);

    path = st_write("p.sh", PROSE_SH);
    text = read_text(path, &n);
    prose_of(path, text, n, 1, &sh);
    st_check(any_span_has(&sh, "seam"),
             "a shell string a person reads was not scanned");
    free(text);

    path = st_write("p.md", PROSE_MD);
    text = read_text(path, &n);
    prose_of(path, text, n, 0, &md);
    st_check(!any_span_has(&md, "load-bearing"),
             "a fenced block counted as Markdown prose");
    st_check(any_span_has(&md, "worth noting"),
             "missed a banned term in Markdown");
    free(text);

    /* The register rule may name what it bans; nothing else may. */
    {
        char claude[1024];
        struct spans body = { NULL, 0, 0 };

        join_path(claude, sizeof(claude), repo, "CLAUDE.md", "");
        text = read_text(claude, &n);
        if (text != NULL) {
            prose_of(claude, text, n, 0, &body);
            st_check(!any_span_has(&body, "load-bearing"),
                     "the register rule's own text was not exempted");
            spans_free(&body);

            /* The exemption must end at the next heading even when that
               heading is indented, which Markdown allows: otherwise one
               space silences the file. */
            {
                char *at = strstr(text, "\n## Comments\n");

                if (at != NULL) {
                    FILE *f;
                    char doctored[1024];
                    size_t head = (size_t) (at - text) + 1;

                    snprintf(doctored, sizeof(doctored), "%s/CLAUDE.md",
                             st_dir);
                    f = fopen(doctored, "wb");
                    if (f != NULL) {
                        fwrite(text, 1, head, f);
                        fputs("  ## Comments\n\nA load-bearing line.\n", f);
                        fputs(at + strlen("\n## Comments\n"), f);
                        fclose(f);
                    }
                    {
                        size_t dn;
                        char *dtext = read_text(doctored, &dn);
                        struct spans dbody = { NULL, 0, 0 };

                        if (dtext != NULL) {
                            prose_of(doctored, dtext, dn, 0, &dbody);
                            st_check(any_span_has(&dbody, "load-bearing"),
                                     "an indented heading let the exemption "
                                     "swallow the file");
                            spans_free(&dbody);
                            free(dtext);
                        }
                    }
                }
            }
            free(text);
        }
    }

    /* The dev-log index rules, over a temporary tree. */
    {
        static const struct {
            const char *name;
            const char *index;
            int n_dirs;
            const char *want;
        } cases[] = {
            { "a current index",
              "* `24-metal`: a sentence.\n* `25-sealed`: a sentence.\n", 3,
              NULL },
            { "the newest finished milestone missing",
              "* `24-metal`: a sentence.\n", 3,
              "dev-log/25-sealed is finished and has no entry" },
            { "an in-flight milestone indexed as done",
              "* `24-metal`: a sentence.\n* `25-sealed`: a sentence.\n"
              "* `26-portable`: a sentence.\n", 3,
              "which has a plan and no record" },
            { "entries out of order",
              "* `25-sealed`: a sentence.\n* `24-metal`: a sentence.\n", 3,
              "the index's entry 1 is 25-sealed" },
            { "an entry for a directory that is gone",
              "* `24-metal`: a sentence.\n* `25-sealed`: a sentence.\n"
              "* `27-polish`: a sentence.\n", 3,
              "which is not a directory" },
            { "a milestone named twice",
              "* `24-metal`: a sentence.\n* `25-sealed`: a sentence.\n"
              "* `25-sealed`: a sentence.\n", 3,
              "more than once" },
            { "a decoy entry inside a fenced block",
              "* `24-metal`: a sentence.\n```\n* `25-sealed`: a sentence.\n"
              "```\n", 3,
              "dev-log/25-sealed is finished and has no entry" },
            { "a fenced example does not satisfy the order rule",
              "* `24-metal`: a sentence.\n* `25-sealed`: a sentence.\n"
              "```\n* `26-portable`: a sentence.\n```\n", 3, NULL }
        };
        struct milestone dirs[3] = {
            { "24-metal", 24, 1 }, { "25-sealed", 25, 1 }, { "26-portable", 26, 0 }
        };
        char entries[64][128];
        int i;

        for (i = 0; i < (int) (sizeof(cases) / sizeof(cases[0])); i++) {
            struct spans problems = { NULL, 0, 0 };
            int n_entries = read_entries(cases[i].index,
                                         strlen(cases[i].index), entries, 64);
            char msg[256];

            judge_devlog(dirs, cases[i].n_dirs, entries, n_entries,
                         &problems);
            snprintf(msg, sizeof(msg), "devlog: %s", cases[i].name);
            if (cases[i].want == NULL)
                st_check(problems.n == 0, msg);
            else
                st_check(problems.n > 0 &&
                         strstr(problems.item[0], cases[i].want) != NULL, msg);
            spans_free(&problems);
        }

        /* The entry pattern: an indented bullet is a sub-item, a name
           without backticks or without the colon is prose, and a tilde
           fence hides its contents too. */
        {
            static const char sample[] =
                "* `25-sealed`: an entry.\n"
                "  * `24-metal`: indented, so a sub-item.\n"
                "* not-a-milestone: no backticks.\n"
                "* `26-portable` no colon after the name.\n"
                "~~~\n* `07-init`: fenced with tildes.\n~~~\n";
            int n_entries = read_entries(sample, sizeof(sample) - 1, entries, 64);

            st_check(n_entries == 1 &&
                     strcmp(entries[0], "25-sealed") == 0,
                     "devlog: the entry pattern read more than the one entry");
        }
    }

    spans_free(&only);
    spans_free(&both);
    spans_free(&sh);
    spans_free(&md);
    if (st_bad) {
        fprintf(stderr, "prosecheck: selftest: %d case(s) failed\n", st_bad);
        st_clean();
        return 1;
    }
    st_clean();
    printf("prosecheck: selftest OK (24 cases)\n");
    return 0;
}

/* ---- entry ---- */

/* The tool is run from the repository root by every caller; the root is
   where CLAUDE.md is. Not finding one is fatal, because a checker that
   walks a tree it did not find reports the same OK as a clean tree. */
static void set_repo(void)
{
    struct stat st;
    char started_at[1100];

    if (getcwd(repo, sizeof(repo)) == NULL) {
        fprintf(stderr, "prosecheck: cannot read the working directory: "
                "%s\n", strerror(errno));
        exit(2);
    }
    snprintf(started_at, sizeof(started_at), "%s", repo);
    for (;;) {
        char probe[1100];
        char *slash;

        snprintf(probe, sizeof(probe), "%s/CLAUDE.md", repo);
        if (stat(probe, &st) == 0)
            return;
        slash = strrchr(repo, '/');
        if (slash == NULL || slash == repo)
            break;
        *slash = '\0';
    }
    fprintf(stderr, "prosecheck: no CLAUDE.md at or above %s, so there is "
            "no tree to check; run this from the repository\n", started_at);
    exit(2);
}

int main(int argc, char **argv)
{
    int i;

    set_repo();

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();

    if (argc >= 2 && strcmp(argv[1], "columns") == 0) {
        struct width_ctx w = { 0, 0 };

        if (argc > 2) {
            for (i = 2; i < argc; i++)
                walk(argv[i], check_width, &w, 0);
        } else {
            for (i = 0; i < (int) (sizeof(ROOTS) / sizeof(ROOTS[0])); i++) {
                char full[1100];

                snprintf(full, sizeof(full), "%s/%s", repo, ROOTS[i]);
                walk(full, check_width, &w, 0);
            }
        }
        if (w.over > 0) {
            fprintf(stderr, "prosecheck: %ld comment line(s) over %d columns "
                    "in %ld file(s)\n", w.over, LIMIT, w.files);
            return 1;
        }
        if (unread > 0) {
            fprintf(stderr, "prosecheck: %ld file(s) could not be read, so "
                    "this proved nothing about them\n", unread);
            return 1;
        }
        printf("prosecheck: OK (no comment line over %d columns)\n", LIMIT);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "banned") == 0) {
        struct banned_ctx b = { 0 };

        if (argc > 2) {
            for (i = 2; i < argc; i++) {
                struct stat st;

                if (stat(argv[i], &st) != 0) {
                    fprintf(stderr, "prosecheck: no such path: %s\n", argv[i]);
                    return 2;
                }
                walk(argv[i], check_banned, &b, 1);
            }
        } else {
            for (i = 0; i < (int) (sizeof(ROOTS) / sizeof(ROOTS[0])); i++) {
                char full[1100];

                snprintf(full, sizeof(full), "%s/%s", repo, ROOTS[i]);
                walk(full, check_banned, &b, 1);
            }
            for (i = 0; i < (int) (sizeof(TOP_MD) / sizeof(TOP_MD[0])); i++) {
                char full[1100];

                snprintf(full, sizeof(full), "%s/%s", repo, TOP_MD[i]);
                walk(full, check_banned, &b, 1);
            }
        }
        if (b.findings > 0) {
            fprintf(stderr, "prosecheck: %ld banned term use(s)\n",
                    b.findings);
            return 1;
        }
        if (unread > 0) {
            fprintf(stderr, "prosecheck: %ld file(s) could not be read, so "
                    "this proved nothing about them\n", unread);
            return 1;
        }
        printf("prosecheck: OK (no banned term in the in-house prose)\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "count") == 0) {
        struct count_ctx c;

        c.prose.item = NULL;
        c.prose.n = 0;
        c.prose.cap = 0;
        c.want = strcmp(argv[2], "c") == 0 ? LANG_C
            : strcmp(argv[2], "hash") == 0 ? LANG_HASH
            : strcmp(argv[2], "md") == 0 ? LANG_MD : LANG_NONE;
        if (c.want == LANG_NONE && strcmp(argv[2], "all") != 0) {
            fprintf(stderr, "prosecheck: count takes c, hash, md or all\n");
            return 2;
        }
        if (strcmp(argv[2], "all") == 0)
            c.want = LANG_NONE;
        for (i = 3; i < argc; i++)
            walk(argv[i], collect_file, &c, 1);
        printf("words %ld\n", words_of(&c.prose));
        for (i = 0; i < N_WATCH; i++)
            printf("phrase %ld\n", count_spans(&c.prose, WATCH[i]));
        spans_free(&c.prose);
        if (unread > 0) {
            fprintf(stderr, "prosecheck: %ld file(s) could not be read, so "
                    "these counts are over less than the tree\n", unread);
            return 1;
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "watchlist") == 0) {
        for (i = 0; i < N_WATCH; i++) {
            const char *p = WATCH[i];

            /* The report writes a phrase as one field, with its spaces as
               underscores. */
            while (*p != '\0') {
                putchar(*p == ' ' ? '_' : *p);
                p++;
            }
            putchar(i + 1 < N_WATCH ? ' ' : '\n');
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "devlog") == 0) {
        char devlog[1100];
        char index[1200];
        struct milestone dirs[256];
        char entries[256][128];
        struct spans problems = { NULL, 0, 0 };
        int n_dirs, n_entries, done = 0, planned = 0;
        const char *newest = "none";
        size_t n;
        char *text;
        struct stat st;

        if (argc > 2)
            snprintf(devlog, sizeof(devlog), "%s", argv[2]);
        else
            snprintf(devlog, sizeof(devlog), "%s/dev-log", repo);
        if (stat(devlog, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "prosecheck: %s is not a directory\n", devlog);
            return 1;
        }
        snprintf(index, sizeof(index), "%s/Readme.md", devlog);
        text = read_text(index, &n);
        if (text == NULL) {
            fprintf(stderr, "prosecheck: %s is missing\n", index);
            return 1;
        }
        n_dirs = read_milestones(devlog, dirs, 256);
        n_entries = read_entries(text, n, entries, 256);
        free(text);
        judge_devlog(dirs, n_dirs, entries, n_entries, &problems);
        if (problems.n > 0) {
            for (i = 0; i < problems.n; i++)
                fprintf(stderr, "prosecheck: %s\n", problems.item[i]);
            spans_free(&problems);
            return 1;
        }
        for (i = 0; i < n_dirs; i++) {
            if (dirs[i].finished) {
                done++;
                newest = dirs[i].name;
            } else {
                planned++;
            }
        }
        printf("prosecheck: OK (%d finished, newest is %s", done, newest);
        if (planned > 0)
            printf(", %d planned", planned);
        printf(")\n");
        return 0;
    }

    fprintf(stderr, "usage: prosecheck columns [PATH ...] | prosecheck "
            "banned [PATH ...] | prosecheck count {c|hash|md|all} FILE ... | "
            "prosecheck watchlist | prosecheck devlog [DIR] | "
            "prosecheck --selftest\n");
    return 2;
}
