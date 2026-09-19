/**
 * guestcheck: hold the guest makefiles equal to the host build they mirror.
 *
 * The guest builds (rach2/guest.mk, utils/guest.mk, ish/guest.mk) name
 * their sources by hand, because pdpmake reads none of the GNU makefiles
 * that are the host build's authority. A list that drifts builds a kernel
 * missing a file or a program missing its companion source, and the
 * failure would surface inside a booted guest, late and hard to read. This
 * guard runs on the host, inside the verify gate, and has five contracts:
 *
 *   1. rach2/guest.mk derives OBJS from the shared rach2/sources.mk lists
 *      the host build includes, object for object, loader.o first, no
 *      duplicates; the kernel.objs recipe writes $(OBJS) whole, and the
 *      link reads @kernel.objs plus the tcc runtime at the pinned text
 *      address.
 *   2. utils/guest.mk's per-program source map equals the host
 *      utils/Makefile map, including the companion sources (pwauth.c,
 *      resolv.c) and the two -D builds (init-login, adduser-crash).
 *   3. ish/guest.mk's object list equals the host ish/Makefile's OBJECTS.
 *   4. The build fits the machine: the deepest kernel include chain leaves
 *      room in the 16-descriptor table for the compiler's own files, and no
 *      guest recipe line approaches the 3072-byte exec string budget.
 *   5. The os-real manifest stages all three source trees at /src,
 *      root-owned and read-only, so the machine actually has what the
 *      makefiles build, and each staged tree has its guest.mk.
 *
 * Usage: guestcheck [REPO_ROOT]
 */
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FD_LIMIT 16
#define FD_RESERVED 4 /* stdin, stdout, stderr, and the output file */

/* The kernel's execve budget is 3072 bytes across the argv and environment
   strings together. A recipe reaches the kernel as `sh -c <expanded text>`
   plus the build environment, so each recipe is expanded the way pdpmake
   will and the result held under the budget minus this allowance for the
   environment a login shell exports. */
#define EXEC_BUDGET 3072
#define ENV_ALLOWANCE 500

#define MAX_TEXT (1024 * 1024)
#define MAX_LINE 8192

static char repo[1024];

static void fail(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "check-guest: fail: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/**
 * Appends a formatted string to a fixed buffer, advancing `*used`.
 *
 * `snprintf` returns the length it would have written, so adding that
 * return value to an index walks past the end of the buffer on the first
 * truncation and the next call writes outside it. Every buffer here holds
 * makefile text that is parsed afterwards, so a buffer that fills is a
 * fault to report and not a length to trim: a checker reading a shortened
 * makefile is reading one that is not on disk. `what` names the buffer in
 * the diagnostic.
 */
static void appendf(char *out, size_t out_len, size_t *used, const char *what,
                    const char *fmt, ...)
{
    va_list ap;
    int want;

    if (*used >= out_len)
        fail("%s does not fit in %lu bytes", what, (unsigned long) out_len);
    va_start(ap, fmt);
    want = vsnprintf(out + *used, out_len - *used, fmt, ap);
    va_end(ap);
    if (want < 0 || (size_t) want >= out_len - *used)
        fail("%s does not fit in %lu bytes", what, (unsigned long) out_len);
    *used += (size_t) want;
}

/* ---- a list of strings ---- */

struct list {
    char **item;
    int n, cap;
};

static void *xalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL)
        fail("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = xalloc(strlen(s) + 1);

    strcpy(p, s);
    return p;
}

static void push(struct list *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->item = realloc(l->item, (size_t) l->cap * sizeof(char *));
        if (l->item == NULL)
            fail("out of memory");
    }
    l->item[l->n++] = xstrdup(s);
}

static void list_free(struct list *l)
{
    int i;

    for (i = 0; i < l->n; i++)
        free(l->item[i]);
    free(l->item);
    l->item = NULL;
    l->n = l->cap = 0;
}

static int has(const struct list *l, const char *s)
{
    int i;

    for (i = 0; i < l->n; i++)
        if (strcmp(l->item[i], s) == 0)
            return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* Whitespace-split `text` onto `out`. */
static void split_into(struct list *out, const char *text)
{
    char buf[MAX_LINE];
    const char *p = text;

    while (*p != '\0') {
        size_t n = 0;

        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (*p == '\0')
            break;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' &&
               *p != '\r' && n + 1 < sizeof(buf))
            buf[n++] = *p++;
        buf[n] = '\0';
        push(out, buf);
    }
}

static void join_into(char *out, size_t out_len, const struct list *l)
{
    size_t used = 0;
    int i;

    out[0] = '\0';
    for (i = 0; i < l->n; i++)
        appendf(out, out_len, &used, "a joined list", "%s%s",
                i ? " " : "", l->item[i]);
}

/* ---- reading makefiles ---- */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0 || n > MAX_TEXT) {
        fclose(f);
        return NULL;
    }
    buf = xalloc((size_t) n + 1);
    if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* The makefile text with one level of `include` lines inlined, which is
   how the shared source manifests reach the guest makefiles. */
static char *guest_text(const char *name)
{
    char path[1200];
    char *text, *out;
    char dir[1200];
    char *slash;
    size_t used = 0;
    const char *p;

    snprintf(path, sizeof(path), "%s/%s", repo, name);
    text = read_file(path);
    if (text == NULL)
        fail("cannot read %s", name);
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash != NULL)
        *slash = '\0';
    out = xalloc(MAX_TEXT);
    out[0] = '\0';
    p = text;
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        char line[MAX_LINE];

        if (len >= sizeof(line))
            fail("a line of %lu bytes, past the %lu this reads; the rest of "
                 "it would be parsed as a shorter line",
                 (unsigned long) len, (unsigned long) sizeof(line) - 1);
        memcpy(line, p, len);
        line[len] = '\0';
        if (strncmp(line, "include ", 8) == 0) {
            const char *what = line + 8;
            char sub[1400];
            char *subtext;

            while (*what == ' ' || *what == '\t')
                what++;
            if (snprintf(sub, sizeof(sub), "%s/%s", dir, what) >=
                (int) sizeof(sub))
                fail("the path of %s, included by %s, is longer than this "
                     "reads", what, name);
            subtext = read_file(sub);
            if (subtext == NULL)
                fail("cannot read %s, included by %s", what, name);
            appendf(out, MAX_TEXT, &used, "the included makefile text",
                    "%s\n", subtext);
            free(subtext);
        } else {
            appendf(out, MAX_TEXT, &used, "the makefile text", "%s\n", line);
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
    free(text);
    return out;
}

/* The whitespace-split value of a backslash-continued macro. Returns 0
   when the macro is absent. */
static int read_macro(const char *text, const char *name, struct list *out)
{
    const char *p = text;
    size_t nlen = strlen(name);

    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);

        if (strncmp(p, name, nlen) == 0) {
            const char *q = p + nlen;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '=') {
                q++;
                /* The value, following its backslash continuations. */
                for (;;) {
                    char piece[MAX_LINE];
                    size_t plen = (size_t) ((p + len) - q);
                    int continued;

                    if (plen >= sizeof(piece))
                        plen = sizeof(piece) - 1;
                    memcpy(piece, q, plen);
                    piece[plen] = '\0';
                    while (plen > 0 && (piece[plen - 1] == ' ' ||
                                        piece[plen - 1] == '\t' ||
                                        piece[plen - 1] == '\r'))
                        piece[--plen] = '\0';
                    continued = plen > 0 && piece[plen - 1] == '\\';
                    if (continued)
                        piece[--plen] = '\0';
                    split_into(out, piece);
                    if (!continued || end == NULL)
                        return 1;
                    p = end + 1;
                    end = strchr(p, '\n');
                    len = end != NULL ? (size_t) (end - p) : strlen(p);
                    q = p;
                    while (*q == ' ' || *q == '\t')
                        q++;
                }
            }
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
    return 0;
}

static void need_macro(const char *text, const char *name, struct list *out)
{
    if (!read_macro(text, name, out))
        fail("no %s macro", name);
}

/* Every simple macro of the text, as parallel name and value lists. */
struct macros {
    struct list name;
    struct list value;
};

static void macros_of(const char *text, struct macros *m)
{
    const char *p = text;

    memset(m, 0, sizeof(*m));
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        size_t i = 0;
        char name[256];

        while (i < len && (isalnum((unsigned char) p[i]) || p[i] == '_'))
            i++;
        if (i > 0 && i < sizeof(name)) {
            size_t j = i;

            while (j < len && (p[j] == ' ' || p[j] == '\t'))
                j++;
            if (j < len && p[j] == '=') {
                struct list value = { NULL, 0, 0 };
                char joined[MAX_LINE];

                memcpy(name, p, i);
                name[i] = '\0';
                if (read_macro(text, name, &value)) {
                    join_into(joined, sizeof(joined), &value);
                    push(&m->name, name);
                    push(&m->value, joined);
                }
                list_free(&value);
            }
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
}

static const char *macro_value(const struct macros *m, const char *name)
{
    int i;

    for (i = 0; i < m->name.n; i++)
        if (strcmp(m->name.item[i], name) == 0)
            return m->value.item[i];
    return "";
}

static void macros_free(struct macros *m)
{
    list_free(&m->name);
    list_free(&m->value);
}

/* One `$(VAR:.a=.b)` reference: fills name, old and new. Returns the
   length of the reference, or 0. */
static size_t subst_ref_at(const char *p, char *name, char *old, char *new_)
{
    size_t i = 0, n = 0;

    if (p[0] != '$' || p[1] != '(')
        return 0;
    i = 2;
    while (isalnum((unsigned char) p[i]) || p[i] == '_') {
        if (n + 1 >= 64)
            return 0;
        name[n++] = p[i++];
    }
    name[n] = '\0';
    if (n == 0 || p[i] != ':' || p[i + 1] != '.')
        return 0;
    i++;
    n = 0;
    old[n++] = p[i++];
    while (isalnum((unsigned char) p[i]) || p[i] == '_') {
        if (n + 1 >= 16)
            return 0;
        old[n++] = p[i++];
    }
    old[n] = '\0';
    if (p[i] != '=' || p[i + 1] != '.')
        return 0;
    i++;
    n = 0;
    new_[n++] = p[i++];
    while (isalnum((unsigned char) p[i]) || p[i] == '_') {
        if (n + 1 >= 16)
            return 0;
        new_[n++] = p[i++];
    }
    new_[n] = '\0';
    if (p[i] != ')')
        return 0;
    return i + 1;
}

/* Replace one word's suffix, the way a substitution reference does. */
static void swap_suffix(char *out, size_t out_len, const char *word,
                        const char *old, const char *new_)
{
    size_t wl = strlen(word), ol = strlen(old);

    if (wl >= ol && strcmp(word + wl - ol, old) == 0)
        snprintf(out, out_len, "%.*s%s", (int) (wl - ol), word, new_);
    else
        snprintf(out, out_len, "%s", word);
}

/* Expand `$(VAR:.a=.b)` tokens against the text's macros: a whole token
   that is one reference becomes the macro's words with their suffixes
   swapped. */
static void expand_tokens(const char *text, const struct list *tokens,
                          struct list *out)
{
    int i;

    for (i = 0; i < tokens->n; i++) {
        char name[64], old[16], new_[16];
        size_t len = subst_ref_at(tokens->item[i], name, old, new_);
        struct list value = { NULL, 0, 0 };
        int j;

        if (len == 0 || len != strlen(tokens->item[i])) {
            push(out, tokens->item[i]);
            continue;
        }
        read_macro(text, name, &value);
        for (j = 0; j < value.n; j++) {
            char word[512];

            swap_suffix(word, sizeof(word), value.item[j], old, new_);
            push(out, word);
        }
        list_free(&value);
    }
}

/* Expand substitution references inside one line. */
static void subst_refs(char *line, size_t line_len, const struct macros *m)
{
    char out[MAX_LINE];
    size_t used = 0;
    const char *p = line;

    out[0] = '\0';
    while (*p != '\0') {
        char name[64], old[16], new_[16];
        size_t len = subst_ref_at(p, name, old, new_);

        if (len == 0) {
            if (used + 2 < sizeof(out)) {
                out[used++] = *p;
                out[used] = '\0';
            }
            p++;
            continue;
        }
        {
            struct list words = { NULL, 0, 0 };
            int j;

            split_into(&words, macro_value(m, name));
            for (j = 0; j < words.n; j++) {
                char word[512];

                swap_suffix(word, sizeof(word), words.item[j], old, new_);
                appendf(out, sizeof(out), &used, "an expanded word list",
                        "%s%s", j ? " " : "", word);
            }
            list_free(&words);
        }
        p += len;
    }
    snprintf(line, line_len, "%s", out);
}

/* Expand macros and substitution references in one makefile line. Three
   passes, the way a macro naming another macro is resolved. */
static void expand_line(char *line, size_t line_len, const struct macros *m)
{
    int pass, i;

    for (pass = 0; pass < 3; pass++) {
        subst_refs(line, line_len, m);
        for (i = 0; i < m->name.n; i++) {
            char want[128];
            char out[MAX_LINE];
            const char *at;
            size_t used = 0;
            const char *p = line;

            snprintf(want, sizeof(want), "$(%s)", m->name.item[i]);
            out[0] = '\0';
            while ((at = strstr(p, want)) != NULL) {
                appendf(out, sizeof(out), &used, "a substitution reference",
                        "%.*s%s", (int) (at - p), p, m->value.item[i]);
                p = at + strlen(want);
            }
            if (used > 0) {
                appendf(out, sizeof(out), &used, "a substitution reference",
                        "%s", p);
                snprintf(line, line_len, "%s", out);
            }
        }
    }
}

/* ---- explicit rules ---- */

struct rule {
    char target[256];
    struct list prereqs;
    struct list recipe;
};

struct rules {
    struct rule *item;
    int n, cap;
};

static struct rule *rule_find(struct rules *r, const char *target)
{
    int i;

    for (i = 0; i < r->n; i++)
        if (strcmp(r->item[i].target, target) == 0)
            return &r->item[i];
    return NULL;
}

/* Every explicit rule of the text: `target: prereqs` and its recipe
   lines. A line with a `=` before the colon is a macro, not a rule, and a
   target beginning with a dot is a special target. */
static void rules_of(const char *text, struct rules *out)
{
    const char *p = text;

    memset(out, 0, sizeof(*out));
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        char line[MAX_LINE];
        const char *colon;

        if (len >= sizeof(line))
            fail("a line of %lu bytes, past the %lu this reads; the rest of "
                 "it would be parsed as a shorter line",
                 (unsigned long) len, (unsigned long) sizeof(line) - 1);
        memcpy(line, p, len);
        line[len] = '\0';
        colon = strchr(line, ':');
        if (len > 0 && line[0] != '\t' && line[0] != '#' && colon != NULL &&
            memchr(line, '=', (size_t) (colon - line)) == NULL) {
            struct rule r;
            char target[256];
            size_t tlen = (size_t) (colon - line);
            const char *q;

            while (tlen > 0 && (line[tlen - 1] == ' ' ||
                                line[tlen - 1] == '\t'))
                tlen--;
            if (tlen >= sizeof(target))
                tlen = sizeof(target) - 1;
            memcpy(target, line, tlen);
            target[tlen] = '\0';
            memset(&r, 0, sizeof(r));
            snprintf(r.target, sizeof(r.target), "%s", target);
            split_into(&r.prereqs, colon + 1);
            /* The recipe: the tab-indented lines that follow, with blank
               lines between them allowed. */
            q = end != NULL ? end + 1 : NULL;
            while (q != NULL && *q != '\0') {
                const char *rend = strchr(q, '\n');
                size_t rlen = rend != NULL ? (size_t) (rend - q) : strlen(q);
                char rline[MAX_LINE];

                if (rlen > 0 && q[0] != '\t')
                    break;
                if (rlen > 0) {
                    if (rlen >= sizeof(rline))
                        rlen = sizeof(rline) - 1;
                    memcpy(rline, q + 1, rlen - 1);
                    rline[rlen - 1] = '\0';
                    push(&r.recipe, rline);
                }
                if (rend == NULL) {
                    q = NULL;
                    break;
                }
                q = rend + 1;
            }
            if (target[0] != '.') {
                if (out->n == out->cap) {
                    out->cap = out->cap ? out->cap * 2 : 32;
                    out->item = realloc(out->item,
                                        (size_t) out->cap *
                                        sizeof(struct rule));
                    if (out->item == NULL)
                        fail("out of memory");
                }
                out->item[out->n++] = r;
            } else {
                list_free(&r.prereqs);
                list_free(&r.recipe);
            }
            if (q == NULL)
                break;
            p = q;
            continue;
        }
        if (end == NULL)
            break;
        p = end + 1;
    }
}

static void rules_free(struct rules *r)
{
    int i;

    for (i = 0; i < r->n; i++) {
        list_free(&r->item[i].prereqs);
        list_free(&r->item[i].recipe);
    }
    free(r->item);
    r->item = NULL;
    r->n = r->cap = 0;
}

/* ---- the five contracts ---- */

/* One `make -s -C rach2 TARGET` list: the host build's own answer for the
   shared source manifest, asked of make so this guard cannot disagree with
   it by reading the file differently. */
static void make_list(const char *target, struct list *out)
{
    char cmd[1200];
    char buf[MAX_TEXT];
    FILE *pipe;
    size_t n;

    snprintf(cmd, sizeof(cmd), "make -s -C '%s/rach2' %s 2>/dev/null", repo,
             target);
    pipe = popen(cmd, "r");
    if (pipe == NULL)
        fail("cannot run make %s", target);
    n = fread(buf, 1, sizeof(buf) - 1, pipe);
    buf[n] = '\0';
    if (pclose(pipe) != 0)
        fail("make %s failed", target);
    split_into(out, buf);
}

static int file_has_line(const char *path, const char *want)
{
    char full[1200];
    char *text;
    const char *p;
    int found = 0;

    snprintf(full, sizeof(full), "%s/%s", repo, path);
    text = read_file(full);
    if (text == NULL)
        fail("cannot read %s", path);
    p = text;
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);

        if (len == strlen(want) && strncmp(p, want, len) == 0)
            found = 1;
        if (end == NULL)
            break;
        p = end + 1;
    }
    free(text);
    return found;
}

static int check_kernel_lists(void)
{
    struct list sources = { NULL, 0, 0 };
    struct list expected = { NULL, 0, 0 };
    struct list objs_raw = { NULL, 0, 0 };
    struct list objs = { NULL, 0, 0 };
    struct rules rules;
    struct rule *r;
    char *text;
    char link[MAX_LINE];
    int i, count;
    static const char *const link_needs[] = {
        "@kernel.objs", "-Wl,-Ttext=0x100074", "/lib/tcc/libtcc1.a",
        "-nostdlib", "-static", "-o kernel.new", "mv kernel.new kernel"
    };

    make_list("print-asm-src", &sources);
    make_list("print-c-src", &sources);
    for (i = 0; i < sources.n; i++) {
        char obj[512];
        size_t len = strlen(sources.item[i]);

        if (len > 2 && (strcmp(sources.item[i] + len - 2, ".c") == 0 ||
                        strcmp(sources.item[i] + len - 2, ".s") == 0))
            snprintf(obj, sizeof(obj), "%.*s.o", (int) (len - 2),
                     sources.item[i]);
        else
            snprintf(obj, sizeof(obj), "%s", sources.item[i]);
        push(&expected, obj);
    }

    if (!file_has_line("rach2/Makefile", "include sources.mk"))
        fail("rach2/Makefile does not include sources.mk, so the host build "
             "no longer reads the shared kernel source list");

    text = guest_text("rach2/guest.mk");
    need_macro(text, "OBJS", &objs_raw);
    expand_tokens(text, &objs_raw, &objs);
    if (objs.n != expected.n) {
        fail("rach2/guest.mk OBJS differs from the host list (the host has "
             "%d objects and the guest has %d)", expected.n, objs.n);
    }
    for (i = 0; i < objs.n; i++)
        if (strcmp(objs.item[i], expected.item[i]) != 0)
            fail("rach2/guest.mk OBJS differs from the host list (entry %d "
                 "is %s and the host has %s; or the order differs)", i + 1,
                 objs.item[i], expected.item[i]);

    rules_of(text, &rules);
    r = rule_find(&rules, "kernel.objs");
    if (r == NULL)
        fail("rach2/guest.mk has no kernel.objs rule");
    if (!has(&r->prereqs, "guest.mk") || !has(&r->prereqs, "sources.mk"))
        fail("kernel.objs does not depend on guest.mk and sources.mk, so an "
             "edited object list would link through the stale response file");
    if (r->recipe.n != 2 ||
        strcmp(r->recipe.item[0],
               "printf '%s\\n' $(OBJS) > kernel.objs.new") != 0 ||
        strcmp(r->recipe.item[1], "mv kernel.objs.new kernel.objs") != 0)
        fail("kernel.objs must write $(OBJS) whole through the temporary "
             "file and publish it by rename");
    if (objs.n == 0 || strcmp(objs.item[0], "loader.o") != 0)
        fail("OBJS does not start with loader.o");
    for (i = 0; i < objs.n; i++) {
        int j;

        for (j = 0; j < i; j++)
            if (strcmp(objs.item[i], objs.item[j]) == 0)
                fail("OBJS contains a duplicate object");
    }

    r = rule_find(&rules, "kernel");
    link[0] = '\0';
    if (r != NULL)
        join_into(link, sizeof(link), &r->recipe);
    for (i = 0; i < (int) (sizeof(link_needs) / sizeof(link_needs[0])); i++)
        if (strstr(link, link_needs[i]) == NULL)
            fail("the kernel link line lacks %s", link_needs[i]);

    count = sources.n;
    rules_free(&rules);
    list_free(&sources);
    list_free(&expected);
    list_free(&objs_raw);
    list_free(&objs);
    free(text);
    return count;
}

/* The host's per-program source map: for each program, the .c files its
   rule names, or the program's own source when the rule adds none. */
struct progmap {
    struct list program;
    struct list sources; /* one joined string per program */
};

static void host_utils_map(struct progmap *map)
{
    char *text;
    struct macros m;
    struct rules rules;
    struct list programs = { NULL, 0, 0 };
    int i, j;

    if (!file_has_line("utils/Makefile", "include programs.mk"))
        fail("utils/Makefile does not include programs.mk, so the host "
             "build no longer reads the shared program map");
    text = guest_text("utils/Makefile");
    macros_of(text, &m);
    need_macro(text, "UTILITIES", &programs);
    need_macro(text, "SBIN_PROGS", &programs);
    rules_of(text, &rules);
    memset(map, 0, sizeof(*map));

    for (i = 0; i < programs.n; i++) {
        struct list found = { NULL, 0, 0 };
        char joined[MAX_LINE];
        int matched = 0;

        for (j = 0; j < rules.n && !matched; j++) {
            char want_a[512], want_b[512];

            snprintf(want_a, sizeof(want_a), "$(BUILD)/%s",
                     programs.item[i]);
            snprintf(want_b, sizeof(want_b), "$(BUILD)/sbin/%s",
                     programs.item[i]);
            if (strcmp(rules.item[j].target, want_a) == 0 ||
                strcmp(rules.item[j].target, want_b) == 0) {
                char line[MAX_LINE];
                struct list words = { NULL, 0, 0 };
                int k;

                join_into(line, sizeof(line), &rules.item[j].prereqs);
                expand_line(line, sizeof(line), &m);
                split_into(&words, line);
                for (k = 0; k < words.n; k++) {
                    size_t len = strlen(words.item[k]);

                    if (len > 2 && strcmp(words.item[k] + len - 2, ".c") == 0)
                        push(&found, words.item[k]);
                }
                list_free(&words);
                matched = 1;
            }
        }
        if (found.n == 0) {
            char own[512];

            snprintf(own, sizeof(own), "%s.c", programs.item[i]);
            push(&found, own);
        }
        qsort(found.item, (size_t) found.n, sizeof(char *), cmp_str);
        join_into(joined, sizeof(joined), &found);
        push(&map->program, programs.item[i]);
        push(&map->sources, joined);
        list_free(&found);
    }
    rules_free(&rules);
    macros_free(&m);
    list_free(&programs);
    free(text);
}

/**
 * The rule for `name` in `file` links to `NAME.new` and renames it into
 * place: the compiler writes into its destination, so a link the machine
 * stops part way would otherwise leave a file newer than its source that
 * the next make calls built. The recipe is those two lines, the link not
 * prefixed with make's `-`, which would carry a failed link on to the
 * rename; a program with no rule is a fault of the makefile.
 */
static void check_links_through_temporary(const char *file, const struct rule *r,
                                          const char *name)
{
    char temp[256], mv[600];

    if (r == NULL) {
        fail("%s has no rule for %s", file, name);
        return;
    }
    snprintf(temp, sizeof(temp), "-o %s.new ", name);
    snprintf(mv, sizeof(mv), "mv %s.new %s", name, name);
    if (r->recipe.n != 2 || r->recipe.item[0][0] == '-' || strstr(r->recipe.item[0], temp) == NULL
        || strcmp(r->recipe.item[1], mv) != 0)
        fail("%s %s does not link to %s.new and rename it, so a link stopped "
             "part way would look built", file, name, name);
}

static int check_utils(void)
{
    struct progmap map;
    char *text;
    struct macros m;
    struct rules rules;
    struct list progs = { NULL, 0, 0 };
    int i, count;
    static const struct {
        const char *program;
        const char *define;
    } defines[] = {
        { "init-login", "-DINIT_CONSOLE_LOGIN" },
        { "adduser-crash", "-DADDUSER_CRASH_POINT" }
    };

    host_utils_map(&map);
    text = guest_text("utils/guest.mk");
    macros_of(text, &m);
    rules_of(text, &rules);
    need_macro(text, "UTILITIES", &progs);
    need_macro(text, "SBIN_PROGS", &progs);
    if (progs.n != map.program.n)
        fail("utils/guest.mk program list differs from the host list (host "
             "has %d, guest has %d)", map.program.n, progs.n);

    for (i = 0; i < map.program.n; i++) {
        struct rule *r = rule_find(&rules, map.program.item[i]);
        struct list words = { NULL, 0, 0 };
        struct list found = { NULL, 0, 0 };
        char line[MAX_LINE], joined[MAX_LINE];
        int k;

        if (r == NULL)
            fail("utils/guest.mk has no rule for %s", map.program.item[i]);
        join_into(line, sizeof(line), &r->prereqs);
        expand_line(line, sizeof(line), &m);
        split_into(&words, line);
        for (k = 0; k < words.n; k++) {
            size_t len = strlen(words.item[k]);

            if (len > 2 && strcmp(words.item[k] + len - 2, ".c") == 0)
                push(&found, words.item[k]);
        }
        qsort(found.item, (size_t) found.n, sizeof(char *), cmp_str);
        join_into(joined, sizeof(joined), &found);
        if (strcmp(joined, map.sources.item[i]) != 0)
            fail("utils/guest.mk %s builds from %s; the host builds from %s",
                 map.program.item[i], joined, map.sources.item[i]);
        list_free(&words);
        list_free(&found);

        /* And the recipe compiles exactly those sources: a rule that
           names a companion source and a recipe that does not compile it
           is a program missing half of itself. */
        join_into(line, sizeof(line), &r->recipe);
        expand_line(line, sizeof(line), &m);
        split_into(&words, line);
        for (k = 0; k < words.n; k++) {
            size_t len = strlen(words.item[k]);

            if (len > 2 && strcmp(words.item[k] + len - 2, ".c") == 0)
                push(&found, words.item[k]);
        }
        qsort(found.item, (size_t) found.n, sizeof(char *), cmp_str);
        join_into(joined, sizeof(joined), &found);
        if (strcmp(joined, map.sources.item[i]) != 0)
            fail("utils/guest.mk %s compiles %s in its recipe; the host "
                 "compiles %s", map.program.item[i],
                 joined[0] ? joined : "nothing", map.sources.item[i]);
        list_free(&words);
        list_free(&found);
    }

    for (i = 0; i < 2; i++) {
        struct rule *r = rule_find(&rules, defines[i].program);
        char line[MAX_LINE];

        if (r == NULL)
            fail("utils/guest.mk has no rule for %s", defines[i].program);
        join_into(line, sizeof(line), &r->recipe);
        expand_line(line, sizeof(line), &m);
        if (strstr(line, defines[i].define) == NULL)
            fail("utils/guest.mk %s lacks %s", defines[i].program,
                 defines[i].define);
    }

    for (i = 0; i < map.program.n; i++)
        check_links_through_temporary("utils/guest.mk",
                                      rule_find(&rules, map.program.item[i]),
                                      map.program.item[i]);

    count = map.program.n;
    rules_free(&rules);
    macros_free(&m);
    list_free(&progs);
    list_free(&map.program);
    list_free(&map.sources);
    free(text);
    return count;
}

static int check_ish(void)
{
    char *host, hostpath[1200];
    char *text;
    struct list src = { NULL, 0, 0 };
    struct list expected = { NULL, 0, 0 };
    struct list objs_raw = { NULL, 0, 0 };
    struct list objs = { NULL, 0, 0 };
    int i, count;

    if (!file_has_line("ish/Makefile", "include sources.mk"))
        fail("ish/Makefile does not include sources.mk, so the host build "
             "no longer reads the shared source list");
    snprintf(hostpath, sizeof(hostpath), "%s/ish/Makefile", repo);
    host = read_file(hostpath);
    if (host == NULL)
        fail("cannot read ish/Makefile");
    if (strstr(host, "OBJECTS = $(addprefix $(BUILD)/, $(ISH_SRC:.c=.o))")
        == NULL)
        fail("ish/Makefile does not derive OBJECTS from ISH_SRC");
    free(host);

    text = guest_text("ish/guest.mk");
    need_macro(text, "ISH_SRC", &src);
    for (i = 0; i < src.n; i++) {
        char obj[512];

        swap_suffix(obj, sizeof(obj), src.item[i], ".c", ".o");
        push(&expected, obj);
    }
    need_macro(text, "OBJS", &objs_raw);
    expand_tokens(text, &objs_raw, &objs);
    {
        struct rules rules = { NULL, 0, 0 };

        rules_of(text, &rules);
        check_links_through_temporary("ish/guest.mk", rule_find(&rules, "ish"), "ish");
        rules_free(&rules);
    }
    if (objs.n != expected.n)
        fail("ish/guest.mk OBJS does not derive from the shared ISH_SRC");
    for (i = 0; i < objs.n; i++)
        if (strcmp(objs.item[i], expected.item[i]) != 0)
            fail("ish/guest.mk OBJS does not derive from the shared "
                 "ISH_SRC");
    count = expected.n;
    list_free(&src);
    list_free(&expected);
    list_free(&objs_raw);
    list_free(&objs);
    free(text);
    return count;
}

/* ---- the include graph ---- */

struct node {
    char path[1024];
    struct list includes;
    int depth;   /* 0 until computed */
    int visiting;
    int is_c;
};

struct graph {
    struct node *item;
    int n, cap;
};

static struct node *node_find(struct graph *g, const char *path)
{
    int i;

    for (i = 0; i < g->n; i++)
        if (strcmp(g->item[i].path, path) == 0)
            return &g->item[i];
    return NULL;
}

/* `a/b/../c` as `a/c`, so two spellings of one file are one node. */
static void normalize_path(char *path)
{
    char out[1024];
    char *parts[128];
    int n = 0, i;
    char *p = path, *slash;
    size_t used = 0;
    int absolute = path[0] == '/';

    while (p != NULL && *p != '\0') {
        size_t plen;

        slash = strchr(p, '/');
        plen = slash != NULL ? (size_t) (slash - p) : strlen(p);
        if (slash != NULL)
            *slash = '\0';
        if (plen == 0 || (plen == 1 && p[0] == '.')) {
            /* nothing */
        } else if (plen == 2 && p[0] == '.' && p[1] == '.') {
            if (n > 0)
                n--;
        } else if (n < 128) {
            parts[n++] = p;
        }
        p = slash != NULL ? slash + 1 : NULL;
    }
    out[0] = '\0';
    for (i = 0; i < n; i++)
        appendf(out, sizeof(out), &used, "a normalised path", "%s%s",
                (i || absolute) ? "/" : "", parts[i]);
    snprintf(path, 1024, "%s", out);
}

/* The first byte of `p` that is not a space or a tab, or `e`. */
static const char *skip_blanks(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

static void scan_includes(struct graph *g, const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *ent;

    /* A directory or file the walk cannot read is a fault, not a gap to
       pass over: a source left out of the graph can only shorten the
       deepest include chain, so an omission reads as a build that fits. */
    if (d == NULL)
        fail("cannot read the directory %s; the include graph would be "
             "missing whatever is under it", dir);
    while ((ent = readdir(d)) != NULL) {
        char full[1024];
        struct stat st;
        size_t len;

        if (ent->d_name[0] == '.' || strcmp(ent->d_name, "build") == 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (stat(full, &st) != 0)
            fail("cannot read %s; the include graph would be missing it",
                 full);
        if (S_ISDIR(st.st_mode)) {
            scan_includes(g, full);
            continue;
        }
        len = strlen(full);
        if (len < 3 || (strcmp(full + len - 2, ".c") != 0 &&
                        strcmp(full + len - 2, ".h") != 0))
            continue;
        {
            char *text = read_file(full);
            struct node node;
            const char *p;

            if (text == NULL)
                fail("cannot read %s; the include graph would be missing "
                     "it", full);
            memset(&node, 0, sizeof(node));
            snprintf(node.path, sizeof(node.path), "%s", full);
            normalize_path(node.path);
            node.is_c = strcmp(full + len - 2, ".c") == 0;
            p = text;
            while (*p != '\0') {
                const char *end = strchr(p, '\n');
                size_t l = end != NULL ? (size_t) (end - p) : strlen(p);
                const char *e = p + l;
                const char *q = skip_blanks(p, e);

                if (q < e && *q == '#') {
                    q = skip_blanks(q + 1, e);
                    if ((size_t) (e - q) > 8 &&
                        strncmp(q, "include", 7) == 0) {
                        q = skip_blanks(q + 7, e);
                        if (q < e && *q == '"') {
                            const char *close = ++q;
                            char rel[1024], resolved[1024];

                            while (close < e && *close != '"')
                                close++;
                            if (close < e) {
                                snprintf(rel, sizeof(rel), "%.*s",
                                         (int) (close - q), q);
                                if (snprintf(resolved, sizeof(resolved),
                                             "%s/%s", dir, rel) >=
                                    (int) sizeof(resolved))
                                    fail("the path of %s under %s is longer "
                                         "than this reads", rel, dir);
                                normalize_path(resolved);
                                push(&node.includes, resolved);
                            }
                        }
                    }
                }
                if (end == NULL)
                    break;
                p = end + 1;
            }
            free(text);
            if (g->n == g->cap) {
                g->cap = g->cap ? g->cap * 2 : 128;
                g->item = realloc(g->item,
                                  (size_t) g->cap * sizeof(struct node));
                if (g->item == NULL)
                    fail("out of memory");
            }
            g->item[g->n++] = node;
        }
    }
    closedir(d);
}

/* The deepest quoted-include chain reachable from `n`. A cycle counts as
   zero, the way the compiler's own guards end one. */
static int depth_of(struct graph *g, struct node *n)
{
    int best = 0, i;

    if (n->visiting)
        return 0;
    if (n->depth > 0)
        return n->depth;
    n->visiting = 1;
    for (i = 0; i < n->includes.n; i++) {
        struct node *child = node_find(g, n->includes.item[i]);

        if (child != NULL) {
            int d = depth_of(g, child);

            if (d > best)
                best = d;
        }
    }
    n->visiting = 0;
    n->depth = best + 1;
    return n->depth;
}

static int include_depth(void)
{
    struct graph g;
    char root[1200];
    int best = 0, i;

    memset(&g, 0, sizeof(g));
    snprintf(root, sizeof(root), "%s/rach2", repo);
    scan_includes(&g, root);
    for (i = 0; i < g.n; i++)
        if (g.item[i].is_c) {
            int d = depth_of(&g, &g.item[i]);

            if (d > best)
                best = d;
        }
    for (i = 0; i < g.n; i++)
        list_free(&g.item[i].includes);
    free(g.item);
    return best;
}

static int check_limits(void)
{
    int depth = include_depth();
    int f;
    static const char *const names[] = { "rach2/guest.mk", "utils/guest.mk",
                                         "ish/guest.mk" };

    if (depth + FD_RESERVED > FD_LIMIT)
        fail("the deepest kernel include chain is %d files; with the %d "
             "descriptors a compile has besides its includes that exceeds "
             "the %d-descriptor table", depth, FD_RESERVED, FD_LIMIT);

    for (f = 0; f < 3; f++) {
        char *text = guest_text(names[f]);
        struct macros m;
        struct rules rules;
        int i, j;

        macros_of(text, &m);
        rules_of(text, &rules);
        for (i = 0; i < rules.n; i++) {
            for (j = 0; j < rules.item[i].recipe.n; j++) {
                char line[MAX_LINE];
                char out[MAX_LINE];
                const char *at;
                size_t used = 0, len;
                const char *p;

                snprintf(line, sizeof(line), "%s",
                         rules.item[i].recipe.item[j]);
                expand_line(line, sizeof(line), &m);
                /* `$@` and `$<`, the two automatic variables these
                   recipes use. */
                out[0] = '\0';
                p = line;
                while ((at = strstr(p, "$@")) != NULL) {
                    appendf(out, sizeof(out), &used, "an expanded recipe",
                            "%.*s%s", (int) (at - p), p,
                            rules.item[i].target);
                    p = at + 2;
                }
                appendf(out, sizeof(out), &used, "an expanded recipe",
                        "%s", p);
                snprintf(line, sizeof(line), "%s", out);
                out[0] = '\0';
                used = 0;
                p = line;
                while ((at = strstr(p, "$<")) != NULL) {
                    appendf(out, sizeof(out), &used, "an expanded recipe",
                            "%.*s%s", (int) (at - p), p,
                            rules.item[i].prereqs.n > 0
                                ? rules.item[i].prereqs.item[0] : "");
                    p = at + 2;
                }
                appendf(out, sizeof(out), &used, "an expanded recipe",
                        "%s", p);
                len = strlen(out);
                if (len + ENV_ALLOWANCE > EXEC_BUDGET)
                    fail("%s: the %s recipe expands to %lu bytes, which with "
                         "the %d-byte environment allowance exceeds the "
                         "%d-byte exec budget", names[f],
                         rules.item[i].target, (unsigned long) len,
                         ENV_ALLOWANCE, EXEC_BUDGET);
            }
        }
        rules_free(&rules);
        macros_free(&m);
        free(text);
    }
    return depth;
}

static void check_staging(void)
{
    char path[1200];
    char *text;
    const char *p;
    static const char *const want[] = {
        "tree 0644 /src/rach2 rach2",
        "tree 0644 /src/utils utils",
        "tree 0644 /src/ish ish"
    };
    static const char *const dirs_ok[] = {
        "dir 0755 /src", "dir 0755 /src/utils", "dir 0755 /src/rach2",
        "dir 0755 /src/rach2/sys", "dir 0755 /src/ish"
    };
    int seen[3] = { 0, 0, 0 };
    int i;

    snprintf(path, sizeof(path), "%s/krffs/manifests/os-real.manifest", repo);
    text = read_file(path);
    if (text == NULL)
        fail("cannot read krffs/manifests/os-real.manifest");
    p = text;
    while (*p != '\0') {
        const char *end = strchr(p, '\n');
        size_t len = end != NULL ? (size_t) (end - p) : strlen(p);
        char line[MAX_LINE];
        char *hash;
        struct list fields = { NULL, 0, 0 };

        if (len >= sizeof(line))
            fail("a line of %lu bytes, past the %lu this reads; the rest of "
                 "it would be parsed as a shorter line",
                 (unsigned long) len, (unsigned long) sizeof(line) - 1);
        memcpy(line, p, len);
        line[len] = '\0';
        hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        {
            size_t l = strlen(line);

            while (l > 0 && (line[l - 1] == ' ' || line[l - 1] == '\t' ||
                             line[l - 1] == '\r'))
                line[--l] = '\0';
        }
        split_into(&fields, line);
        if (fields.n >= 3 &&
            (strcmp(fields.item[2], "/src") == 0 ||
             strncmp(fields.item[2], "/src/", 5) == 0)) {
            int known = 0;

            for (i = 0; i < 3; i++)
                if (strcmp(line, want[i]) == 0) {
                    seen[i] = 1;
                    known = 1;
                }
            for (i = 0; i < 5; i++)
                if (strcmp(line, dirs_ok[i]) == 0)
                    known = 1;
            if (!known)
                fail("os-real.manifest directive '%s' touches /src; only "
                     "the three root-owned read-only source trees may live "
                     "there, or a later line could hand the shared source "
                     "to another owner", line);
        }
        list_free(&fields);
        if (end == NULL)
            break;
        p = end + 1;
    }
    free(text);
    for (i = 0; i < 3; i++)
        if (!seen[i])
            fail("os-real.manifest does not stage '%s' (root-owned, 0644)",
                 want[i]);
    for (i = 0; i < 3; i++) {
        static const char *const trees[] = { "rach2", "utils", "ish" };
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s/guest.mk", repo, trees[i]);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            fail("%s/guest.mk is missing or not a regular file, so the "
                 "staged tree could not be built in the guest", trees[i]);
    }
}

int main(int argc, char **argv)
{
    int sources, programs, ish_objects, depth;

    if (argc > 1) {
        snprintf(repo, sizeof(repo), "%s", argv[1]);
    } else if (getcwd(repo, sizeof(repo)) == NULL) {
        fail("cannot read the working directory");
    }
    sources = check_kernel_lists();
    programs = check_utils();
    ish_objects = check_ish();
    depth = check_limits();
    check_staging();
    printf("check-guest: ok: %d kernel sources, %d programs, %d ish "
           "objects, include depth %d, three /src trees staged\n",
           sources, programs, ish_objects, depth);
    return 0;
}
