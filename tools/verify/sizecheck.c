/**
 * sizecheck: the size limits on functions, read from the source.
 *
 * Every function in the files given is measured four ways: the lines of
 * its body, the parameters it takes, the deepest block nesting inside it,
 * and, when pmccabe's table is given, its cyclomatic complexity. A function
 * over a limit is a finding unless the reasons file has an entry for it:
 * the file, the function, the measure, the value the entry allows and why
 * a split would make the function worse. The entry is a ratchet: a
 * function may not grow past the value it records, and an entry for a
 * function that is within the limit again is stale and reported, so the
 * file shrinks as the code does and never grows without a reason written.
 *
 * The parse relies on the house format: a definition's closing parenthesis
 * ends a line at column zero's statement and the next line opens the body
 * with a brace at column zero, and the body ends with a brace at column
 * zero. clang-format keeps every in-house file in that shape.
 *
 * Without a pmccabe table the complexity is not judged, and the reasons
 * file's complexity entries are not called stale, so the check runs on a
 * machine without pmccabe and the testing host judges the rest.
 *
 * A reasons entry is one line, `FILE:FUNCTION MEASURE VALUE REASON`, where
 * the measure is `lines`, `params`, `nesting` or `complexity`; `#` begins a
 * comment. `--report` prints every function's four measures, for a reading
 * of the distribution.
 *
 * Usage: sizecheck [--pmccabe TABLE] [--lines N] [--params N] [--nesting N]
 *                  [--complexity N] [--report] REASONS FILE...
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAME_MAX_LEN 128
#define PATH_MAX_LEN 512
#define REASONS_MAX  1024
#define LINE_MAX_LEN 4096
#define SIG_LINES    4
#define SIG_MAX_LEN  (SIG_LINES * LINE_MAX_LEN + SIG_LINES)

struct reason {
    char file[PATH_MAX_LEN];
    char name[NAME_MAX_LEN];
    char measure[16];
    long value;
    int used;
};

static struct reason reasons[REASONS_MAX];
static int nreasons;
static int findings;
static int functions;
static int excused;
static int report;

static long lim_lines = 100, lim_params = 6, lim_nesting = 4, lim_complexity = 25;

/* pmccabe's table: one line per function, the modified count first. */
struct complexity {
    char file[PATH_MAX_LEN];
    char name[NAME_MAX_LEN];
    long value;
};
static struct complexity *complexities;
static int ncomplexities;

static void read_reasons(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[LINE_MAX_LEN];
    int lineno = 0;

    if (!f) {
        fprintf(stderr, "sizecheck: %s: cannot read\n", path);
        exit(2);
    }
    while (fgets(line, sizeof(line), f)) {
        struct reason *r;
        char *colon, *sp;

        lineno++;
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        if (nreasons == REASONS_MAX) {
            fprintf(stderr, "sizecheck: %s: more than %d entries\n", path, REASONS_MAX);
            exit(2);
        }
        r = &reasons[nreasons];
        colon = strchr(line, ':');
        sp = colon ? strchr(colon, ' ') : NULL;
        if (!colon || !sp || (size_t)(colon - line) >= sizeof(r->file) ||
            (size_t)(sp - colon - 1) >= sizeof(r->name)) {
            fprintf(stderr, "sizecheck: %s:%d: not FILE:FUNCTION MEASURE VALUE REASON\n", path,
                    lineno);
            exit(2);
        }
        memcpy(r->file, line, (size_t)(colon - line));
        r->file[colon - line] = '\0';
        memcpy(r->name, colon + 1, (size_t)(sp - colon - 1));
        r->name[sp - colon - 1] = '\0';
        if (sscanf(sp + 1, "%15s %ld", r->measure, &r->value) != 2 ||
            strlen(sp + 1) < strlen(r->measure) + 4) {
            fprintf(stderr, "sizecheck: %s:%d: %s:%s: the measure, its value and a reason\n", path,
                    lineno, r->file, r->name);
            exit(2);
        }
        if (strcmp(r->measure, "lines") != 0 && strcmp(r->measure, "params") != 0 &&
            strcmp(r->measure, "nesting") != 0 && strcmp(r->measure, "complexity") != 0) {
            fprintf(stderr, "sizecheck: %s:%d: %s is not a measure\n", path, lineno, r->measure);
            exit(2);
        }
        nreasons++;
    }
    fclose(f);
}

static void read_pmccabe(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[LINE_MAX_LEN];
    int cap = 0;

    if (!f) {
        fprintf(stderr, "sizecheck: %s: cannot read\n", path);
        exit(2);
    }
    while (fgets(line, sizeof(line), f)) {
        long modified, a, b, c, d;
        char where[1024];
        char *paren, *colon;
        struct complexity *cx;

        if (sscanf(line, "%ld %ld %ld %ld %ld %1000s", &modified, &a, &b, &c, &d, where) != 6) {
            continue;
        }
        paren = strchr(where, '(');
        colon = paren ? strchr(paren, ':') : NULL;
        if (!paren || !colon) {
            continue;
        }
        if (ncomplexities == cap) {
            cap = cap ? cap * 2 : 1024;
            complexities = realloc(complexities, sizeof(*complexities) * (size_t)cap);
            if (!complexities) {
                fprintf(stderr, "sizecheck: out of memory\n");
                exit(2);
            }
        }
        cx = &complexities[ncomplexities];
        if ((size_t)(paren - where) >= sizeof(cx->file)) {
            continue;
        }
        memcpy(cx->file, where, (size_t)(paren - where));
        cx->file[paren - where] = '\0';
        /* the name follows "(LINE): " in the line, after `where` */
        {
            const char *p = strstr(line, "): ");
            size_t n;

            if (!p) {
                continue;
            }
            p += 3;
            n = strcspn(p, " \t\n");
            if (n == 0 || n >= sizeof(cx->name)) {
                continue;
            }
            memcpy(cx->name, p, n);
            cx->name[n] = '\0';
        }
        cx->value = modified;
        ncomplexities++;
    }
    fclose(f);
}

static long complexity_of(const char *file, const char *name)
{
    int i;

    for (i = 0; i < ncomplexities; i++) {
        if (strcmp(complexities[i].name, name) == 0 && strstr(file, complexities[i].file) != NULL) {
            return complexities[i].value;
        }
    }
    return -1;
}

/* A measured excess against the reasons: counted as a finding, or as an
   excess the file allows. */
static void judge(const char *file, const char *name, const char *measure, long value, long limit)
{
    int i;

    if (value <= limit) {
        return;
    }
    for (i = 0; i < nreasons; i++) {
        struct reason *r = &reasons[i];

        if (strcmp(r->name, name) == 0 && strcmp(r->measure, measure) == 0 &&
            strcmp(r->file, file) == 0) {
            r->used = 1;
            if (value > r->value) {
                printf("sizecheck: %s:%s: %s %ld, past the %ld the reasons file allows\n", file,
                       name, measure, value, r->value);
                findings++;
            } else {
                excused++;
            }
            return;
        }
    }
    printf("sizecheck: %s:%s: %s %ld, over the limit of %ld and not in the reasons file\n", file,
           name, measure, value, limit);
    findings++;
}

/* The code of one line with its comments and string contents removed, so a
   brace or a comma inside them is not counted; `inc` carries a comment
   across lines. */
static void bare(const char *in, char *out, size_t cap, int *inc)
{
    size_t i = 0, o = 0;

    while (in[i] != '\0' && o + 1 < cap) {
        if (*inc) {
            if (in[i] == '*' && in[i + 1] == '/') {
                *inc = 0;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (in[i] == '/' && in[i + 1] == '*') {
            *inc = 1;
            i += 2;
            continue;
        }
        if (in[i] == '"' || in[i] == '\'') {
            char q = in[i];

            out[o++] = q;
            i++;
            while (in[i] != '\0' && in[i] != q) {
                if (in[i] == '\\' && in[i + 1] != '\0') {
                    i++;
                }
                i++;
            }
            if (in[i] == q) {
                out[o++] = q;
                i++;
            }
            continue;
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
}

static int is_definition_end(const char *code)
{
    size_t n = strlen(code);

    while (n > 0 && (code[n - 1] == '\n' || code[n - 1] == ' ')) {
        n--;
    }
    if (n == 0) {
        return 0;
    }
    if (code[n - 1] == ')') {
        return 1;
    }
    /* an attribute macro after the closing parenthesis */
    return strstr(code, ") K_NORETURN") != NULL;
}

/* The index of the buffered line a signature starts at: the nearest one
   that begins at column zero, since clang-format indents a continuation.
   -1 when the buffered lines do not end a definition. */
static int signature_start(char lines[][LINE_MAX_LEN], int nprev)
{
    int i;

    if (nprev == 0 || !is_definition_end(lines[nprev - 1])) {
        return -1;
    }
    for (i = nprev - 1; i > 0; i--) {
        if (lines[i][0] != ' ' && lines[i][0] != '\t') {
            break;
        }
    }
    return isalpha((unsigned char)lines[i][0]) ? i : -1;
}

/* The parameters a list opened at `open` declares: the commas at its own
   level plus one, or none for `()` and `(void)`. */
static long count_params(const char *open)
{
    const char *p;
    int commas = 0, level = 0;

    for (p = open + 1; *p != '\0' && (*p != ')' || level > 0); p++) {
        if (*p == '(') {
            level++;
        } else if (*p == ')') {
            level--;
        } else if (*p == ',' && level == 0) {
            commas++;
        }
    }
    for (p = open + 1; *p == ' '; p++) {
    }
    return (*p == ')' || strncmp(p, "void", 4) == 0) ? 0 : commas + 1;
}

/* The name and the parameter count of the definition whose signature the
   buffered lines end, or 0 when they do not end one. */
static int parse_signature(char lines[][LINE_MAX_LEN], int nprev, char *name, long *params)
{
    char sig[SIG_MAX_LEN] = "";
    char *open, *p;
    int i, start = signature_start(lines, nprev);

    if (start < 0) {
        return 0;
    }
    for (i = start; i < nprev; i++) {
        strcat(sig, lines[i]);
        strcat(sig, " ");
    }
    open = strchr(sig, '(');
    if (!open) {
        return 0;
    }
    p = open;
    while (p > sig && (p[-1] == ' ' || p[-1] == '\t')) {
        p--;
    }
    while (p > sig && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
        p--;
    }
    for (i = 0;
         p + i < open && i < NAME_MAX_LEN - 1 && (isalnum((unsigned char)p[i]) || p[i] == '_');
         i++) {
        name[i] = p[i];
    }
    name[i] = '\0';
    *params = count_params(open);
    return 1;
}

/* The braces of one body line: `depth` is the block depth after it and
   `nesting` the deepest block the body has opened. */
static void scan_body_line(const char *code, int *depth, int *nesting)
{
    int i;

    for (i = 0; code[i] != '\0'; i++) {
        if (code[i] == '{') {
            (*depth)++;
            if (*depth - 1 > *nesting) {
                *nesting = *depth - 1;
            }
        } else if (code[i] == '}') {
            (*depth)--;
        }
    }
}

static void finish_function(const char *path, const char *name, long params, int body_lines,
                            int nesting)
{
    long cx = complexity_of(path, name);

    functions++;
    if (report) {
        printf("%s:%s lines %d params %ld nesting %d complexity %ld\n", path, name, body_lines,
               params, nesting, cx);
    }
    judge(path, name, "lines", body_lines, lim_lines);
    judge(path, name, "params", params, lim_params);
    judge(path, name, "nesting", nesting, lim_nesting);
    if (cx >= 0) {
        judge(path, name, "complexity", cx, lim_complexity);
    }
}

static void check_file(const char *path)
{
    FILE *f = fopen(path, "r");
    static char lines[SIG_LINES][LINE_MAX_LEN]; /* the signature may span a few lines */
    char line[LINE_MAX_LEN], code[LINE_MAX_LEN];
    int inc = 0, nprev = 0, depth = 0, in_body = 0, body_lines = 0, nesting = 0;
    char name[NAME_MAX_LEN] = "";
    long params = 0;

    if (!f) {
        fprintf(stderr, "sizecheck: %s: cannot read\n", path);
        exit(2);
    }
    while (fgets(line, sizeof(line), f)) {
        bare(line, code, sizeof(code), &inc);
        if (in_body) {
            scan_body_line(code, &depth, &nesting);
            if (depth == 0) {
                finish_function(path, name, params, body_lines, nesting);
                in_body = 0;
                nprev = 0;
            } else {
                body_lines++;
            }
        } else if (code[0] == '{' && depth == 0 && parse_signature(lines, nprev, name, &params)) {
            if (code[1] == '}') {
                finish_function(path, name, params, 0, 0);
            } else {
                in_body = 1;
                body_lines = 0;
                nesting = 0;
                depth = 1;
            }
            nprev = 0;
        } else if (code[0] == '\n' || code[0] == '\0') {
            nprev = 0;
        } else {
            if (nprev == SIG_LINES) {
                memmove(lines[0], lines[1], sizeof(lines[0]) * (SIG_LINES - 1));
                nprev = SIG_LINES - 1;
            }
            strcpy(lines[nprev++], code);
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *pmccabe = NULL;
    int i, first;

    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--pmccabe") == 0 && i + 1 < argc) {
            pmccabe = argv[++i];
        } else if (strcmp(argv[i], "--lines") == 0 && i + 1 < argc) {
            lim_lines = atol(argv[++i]);
        } else if (strcmp(argv[i], "--params") == 0 && i + 1 < argc) {
            lim_params = atol(argv[++i]);
        } else if (strcmp(argv[i], "--nesting") == 0 && i + 1 < argc) {
            lim_nesting = atol(argv[++i]);
        } else if (strcmp(argv[i], "--complexity") == 0 && i + 1 < argc) {
            lim_complexity = atol(argv[++i]);
        } else if (strcmp(argv[i], "--report") == 0) {
            report = 1;
        } else {
            fprintf(stderr, "sizecheck: %s: not an option\n", argv[i]);
            return 2;
        }
    }
    first = i;
    if (argc - first < 2) {
        fprintf(stderr, "usage: sizecheck [--pmccabe TABLE] [--lines N] [--params N] [--nesting N] "
                        "[--complexity N] REASONS FILE...\n");
        return 2;
    }
    read_reasons(argv[first]);
    if (pmccabe) {
        read_pmccabe(pmccabe);
    }
    for (i = first + 1; i < argc; i++) {
        check_file(argv[i]);
    }
    for (i = 0; i < nreasons; i++) {
        if (!reasons[i].used && (pmccabe || strcmp(reasons[i].measure, "complexity") != 0)) {
            printf("sizecheck: %s:%s: the reasons file's %s entry is stale, the function is within "
                   "the limit or gone\n",
                   reasons[i].file, reasons[i].name, reasons[i].measure);
            findings++;
        }
    }
    if (findings) {
        printf("sizecheck: %d finding(s)\n", findings);
        return 1;
    }
    printf("sizecheck: ok: %d functions in %d files, %d excesses under reasons\n", functions,
           argc - first - 1, excused);
    return 0;
}
