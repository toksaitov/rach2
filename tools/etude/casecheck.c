/*
 * casecheck: the validator of etude's case manifests, report records,
 * results and runs, tools/etude/Readme.md's contracts.
 *
 * casecheck [-s SCHEMAS] [-r ROOT] manifest FILE...
 *     checks each case manifest: its shape by schemas/case.schema.json, and
 *     the rules of cases.c that relate its values; with -r, each `where`
 *     names a file and line of the tree at ROOT.
 * casecheck [-s SCHEMAS] [-r ROOT] timing FILE...
 *     checks the manifests as `manifest` does and prints each line a timing
 *     assumption declares, as `FILE:LINE KIND CASE`; suitecheck.sh accepts a
 *     sleep or a bound on a duration only at such a line.
 * casecheck [-s SCHEMAS] [-r ROOT] families FILE...
 *     checks the manifests as `manifest` does and, when nothing is found,
 *     prints each one's family as `FAMILY_ID FILE`, in the order given.
 * casecheck [-s SCHEMAS] report [-f FEATURE=present|absent]... MANIFEST CASE
 *         ARCH PLACEMENT [OUTPUT]
 *     judges a program's output, standard input without OUTPUT, as the
 *     report of CASE run on ARCH (i686, x86_64 or none) and placed host,
 *     qemu or native, with the processor's features that a branch group is
 *     decided by; prints each assertion's outcome and the outcome they give.
 * casecheck [-s SCHEMAS] attempt [-f FEATURE=present|absent]... [-k CONSOLE]...
 *         MANIFEST CASE ARCH PLACEMENT OUTPUT RUN ATTEMPT ITEM END [VALUE]
 *     judges OUTPUT, a file or `-`, as `report` does, and prints the results
 *     document of the attempt ATTEMPT of the item ITEM of the run RUN, whose
 *     program ended as END: `exited` or `signaled` with VALUE, or `stopped`
 *     or `lost`. Each CONSOLE is a console of a machine of an attempt placed
 *     qemu, and the consoles decide the executor's assertions. The status is
 *     0 for a pass, 1 for a failure and 3 for an incomplete attempt; with 2,
 *     no document is printed.
 * casecheck [-s SCHEMAS] results [-o OUTCOME] RESULTS MANIFEST...
 *     checks each results document of the JSON Lines file RESULTS against
 *     its schema and its case, and prints each attempt's outcome; with -o,
 *     the outcome a history records for them, refuses one that differs.
 * casecheck [-s SCHEMAS] schema NAME FILE...
 *     checks each document of FILE, or each line of a FILE whose name ends
 *     in .jsonl, against schemas/NAME.schema.json.
 * casecheck [-s SCHEMAS] verdict [-b STORE] [-c COMMIT] [-m CASES] ARCH SET
 *         SUITES HISTORY...
 *     reads the history of one run, its streams or directories of them, as
 *     ci does: prints each item that did not pass and a last line,
 *     `casecheck: result: passed` or `casecheck: result: failed: NAME...`,
 *     each NAME a suite of SUITES, SET, or `runner` when the records cannot
 *     show what ran; verdict.c says how.
 * casecheck processor NAME
 *     prints the QEMU `-cpu` value of the processor profile NAME, which a
 *     script that boots QEMU itself passes, as the QEMU launcher does.
 * casecheck --selftest DIR
 *     runs the commands DIR/expect lists over the fixtures of DIR, with the
 *     schemas of DIR/../schemas, and compares each one's exit status and
 *     output with what the line expects.
 *
 * A finding goes to stderr as `casecheck: FILE:LINE: WHAT`. The exit status
 * is 0 when nothing is found (for `report`, when the report gives a pass;
 * for `verdict`, when the run passed), 1 after a finding, and 2 for a wrong
 * usage or a file that cannot be read, which for `verdict` is only the
 * results schema; `attempt` gives its own, above.
 * SCHEMAS is tools/etude/schemas by default.
 */
#include "casecheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/processor.h"
#include "schema.h"

/* Where messages go: stdout and stderr, or one buffer in a self-test. */
static struct jbuf *capture;
static int found;

/* The schemas when -s does not name them; a self-test uses its own tree's. */
static const char *default_schemas = "tools/etude/schemas";

/** Writes a message to `stream`, or to the self-test's buffer. */
static void emit(FILE *stream, const char *fmt, va_list ap)
{
    char text[1024];

    vsnprintf(text, sizeof text, fmt, ap);
    if (capture != NULL) {
        jbuf_puts(capture, text);
    } else {
        fputs(text, stream);
    }
}

/**
 * Reports a finding at `line` of `file`; a line of 0 or no file is left out.
 */
void finding(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    char where[600] = "";

    if (file != NULL && line > 0) {
        snprintf(where, sizeof where, "%s:%d: ", file, line);
    } else if (file != NULL) {
        snprintf(where, sizeof where, "%s: ", file);
    }
    va_start(ap, fmt);
    if (capture != NULL) {
        jbuf_puts(capture, "casecheck: ");
        jbuf_puts(capture, where);
    } else {
        fprintf(stderr, "casecheck: %s", where);
    }
    emit(stderr, fmt, ap);
    va_end(ap);
    if (capture != NULL) {
        jbuf_puts(capture, "\n");
    } else {
        fputc('\n', stderr);
    }
    found++;
}

/** Prints a line of a command's output. */
void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    emit(stdout, fmt, ap);
    va_end(ap);
}

/** Prints `text` whole as a command's output, where say() cuts a line short. */
void say_text(const char *text)
{
    if (capture != NULL) {
        jbuf_puts(capture, text);
    } else {
        fputs(text, stdout);
    }
}

/** Returns the number of findings so far. */
int findings(void)
{
    return found;
}

/**
 * Reads the whole of `path`, or standard input for `-`, into a terminated
 * buffer the caller frees, with its length in `*len`. Returns NULL, after
 * a message, when it cannot be read.
 */
char *read_file(const char *path, size_t *len)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    struct jbuf b = {0};
    char chunk[8192];
    size_t n;

    if (f == NULL) {
        finding(path, 0, "cannot read");
        return NULL;
    }
    jbuf_put(&b, "", 0);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        jbuf_put(&b, chunk, n);
    }
    if (ferror(f)) {
        finding(path, 0, "cannot read");
        jbuf_free(&b);
        b.data = NULL;
    }
    if (f != stdin) {
        fclose(f);
    }
    *len = b.len;
    return b.data;
}

/**
 * Reads the JSON document of `path`; on failure sets `*status` to 1 or 2 and
 * returns NULL.
 */
struct json *read_json(const char *path, int *status)
{
    size_t len;
    char *text = read_file(path, &len);
    struct json *doc;
    char err[200];

    if (text == NULL) {
        *status = 2;
        return NULL;
    }
    if (json_parse(text, len, 1, &doc, err, sizeof err) != 0) {
        finding(path, 0, "%s", err);
        *status = 1;
        doc = NULL;
    }
    free(text);
    return doc;
}

/**
 * Reads the schema `NAME.schema.json` of the directory `dir`, or returns NULL.
 */
struct json *read_schema(const char *dir, const char *name)
{
    char path[1024];
    int status = 0;

    snprintf(path, sizeof path, "%s/%s.schema.json", dir, name);
    return read_json(path, &status);
}

/* The file whose faults schema_findings reports. */
struct place {
    const char *file;
};

/** Reports one fault of a schema check as a finding. */
static void schema_fault(void *ctx, int line, const char *path, const char *what)
{
    const struct place *p = ctx;

    finding(p->file, line, "%s: %s", path, what);
}

/**
 * Checks `doc` of `file` against `schema`; returns the number of faults found.
 */
int schema_findings(const struct json *schema, const struct json *doc, const char *file)
{
    struct place p;

    p.file = file;
    return schema_check(schema, doc, schema_fault, &p);
}

/**
 * Checks each document of `path` against `schema`, line by line for JSON Lines.
 */
static int check_documents(const struct json *schema, const char *path)
{
    size_t len = strlen(path), size;
    int lines = len > 6 && strcmp(path + len - 6, ".jsonl") == 0;
    char *text = read_file(path, &size);
    char *start, *next;
    int number = 1, bad = 0;

    if (text == NULL) {
        return 2;
    }
    for (start = text; *start != '\0' || number == 1; start = next, number++) {
        struct json *doc;
        char err[200];

        next = lines ? strchr(start, '\n') : NULL;
        next = next != NULL ? next + 1 : start + strlen(start);
        if (lines && strspn(start, " \t\r\n") >= (size_t)(next - start)) {
            continue;
        }
        if (json_parse(start, (size_t)(next - start), number, &doc, err, sizeof err) != 0) {
            finding(path, 0, "%s", err);
            bad = 1;
        } else {
            bad |= schema_findings(schema, doc, path) != 0;
            json_free(doc);
        }
        if (!lines) {
            break;
        }
    }
    free(text);
    return bad;
}

/** The options before a command. */
struct options {
    const char *schemas;
    const char *root;
    int next; /* the index of the command */
};

/** Reads the options before the command; returns 0 or -1. */
static int read_options(int argc, char **argv, struct options *o)
{
    int i;

    o->schemas = default_schemas;
    o->root = NULL;
    for (i = 1; i + 1 < argc && argv[i][0] == '-' && argv[i][1] != '-'; i += 2) {
        if (strcmp(argv[i], "-s") == 0) {
            o->schemas = argv[i + 1];
        } else if (strcmp(argv[i], "-r") == 0) {
            o->root = argv[i + 1];
        } else {
            return -1;
        }
    }
    o->next = i;
    return i < argc ? 0 : -1;
}

/** Runs the `schema` command over the files `args`. */
static int command_schema(const struct options *o, char **args, int n)
{
    struct json *schema;
    int status = 0, i;

    if (n < 2) {
        return 2;
    }
    schema = read_schema(o->schemas, args[0]);
    if (schema == NULL) {
        return 2;
    }
    for (i = 1; i < n; i++) {
        int s = check_documents(schema, args[i]);

        status = s > status ? s : status;
    }
    json_free(schema);
    return status;
}

/** Runs the `results` command: `[-o OUTCOME] RESULTS MANIFEST...`. */
static int command_results(struct cases *set, char **args, int n)
{
    const char *claim = NULL;
    int status, judged;

    if (n >= 2 && strcmp(args[0], "-o") == 0) {
        claim = args[1];
        args += 2;
        n -= 2;
    }
    if (n < 2 || (claim != NULL && strcmp(claim, "pass") != 0 && strcmp(claim, "failure") != 0 &&
                  strcmp(claim, "incomplete") != 0)) {
        return 2;
    }
    status = load_manifests(set, args + 1, n - 1);
    if (status == 2) {
        return 2;
    }
    judged = judge_results(set, args[0], claim);
    return judged > status ? judged : status;
}

/** Runs the `processor` command: `NAME`. */
static int command_processor(char **args, int n)
{
    const struct etude_processor *p = n == 1 ? etude_processor_find(args[0]) : NULL;
    char cpu[256];

    if (n != 1) {
        return 2;
    }
    if (p == NULL || etude_processor_cpu(p, cpu, sizeof cpu) != 0) {
        finding(NULL, 0, "%s is no processor profile", args[0]);
        return 1;
    }
    say("%s\n", cpu);
    return 0;
}

/**
 * Runs one command line of casecheck, as main does; returns its exit status.
 */
static int run(int argc, char **argv)
{
    struct options o;
    struct cases set;
    const char *command;
    char **args;
    int n, status;

    if (read_options(argc, argv, &o) != 0) {
        return 2;
    }
    command = argv[o.next];
    args = argv + o.next + 1;
    n = argc - o.next - 1;
    memset(&set, 0, sizeof set);
    set.schemas = o.schemas;
    set.root = o.root;
    if (strcmp(command, "manifest") == 0 || strcmp(command, "timing") == 0 ||
        strcmp(command, "families") == 0) {
        if (n < 1) {
            return 2;
        }
        status = load_manifests(&set, args, n);
        if (status == 0 && strcmp(command, "timing") == 0) {
            print_timing(&set);
        } else if (status == 0 && strcmp(command, "families") == 0) {
            print_families(&set);
        }
    } else if (strcmp(command, "report") == 0) {
        status = judge_report(&set, args, n);
    } else if (strcmp(command, "attempt") == 0) {
        status = judge_attempt(&set, args, n);
    } else if (strcmp(command, "results") == 0) {
        status = command_results(&set, args, n);
    } else if (strcmp(command, "schema") == 0) {
        status = command_schema(&o, args, n);
    } else if (strcmp(command, "verdict") == 0) {
        status = judge_verdict(&set, args, n);
    } else if (strcmp(command, "processor") == 0) {
        status = command_processor(args, n);
    } else {
        status = 2;
    }
    free_manifests(&set);
    return status;
}

/* ---- the self-test ---- */

/** Prints the usage. */
static void usage(void)
{
    fprintf(stderr, "usage: casecheck [-s SCHEMAS] [-r ROOT] manifest|timing|families FILE...\n"
                    "       casecheck [-s SCHEMAS] report [-f FEATURE=present|absent]... "
                    "MANIFEST CASE ARCH PLACEMENT [OUTPUT]\n"
                    "       casecheck [-s SCHEMAS] attempt [-f FEATURE=present|absent]... "
                    "[-k CONSOLE]... MANIFEST CASE ARCH PLACEMENT OUTPUT RUN ATTEMPT ITEM END "
                    "[VALUE]\n"
                    "       casecheck [-s SCHEMAS] results [-o OUTCOME] RESULTS MANIFEST...\n"
                    "       casecheck [-s SCHEMAS] schema NAME FILE...\n"
                    "       casecheck [-s SCHEMAS] verdict [-b STORE] [-c COMMIT] [-m CASES] "
                    "ARCH SET SUITES HISTORY...\n"
                    "       casecheck processor NAME\n"
                    "       casecheck --selftest DIR\n");
}

/**
 * Splits the line `line` of the expect file into its expected status, the
 * words of its command line, with `./` and `../` paths taken from `dir` into
 * strings of their own that `owned` marks, and the text after ` | ` that its
 * output must contain, or with a leading `!` must not. Returns the number of
 * words, or -1 for a line that has no status.
 */
static int split_expect(char *line, const char *dir, char **argv, int *owned, char **text,
                        int *expect)
{
    char *bar = strstr(line, " | ");
    char *word;
    int argc = 1;

    *text = NULL;
    if (bar != NULL) {
        *bar = '\0';
        *text = bar + 3;
    }
    word = strtok(line, " ");
    if (word == NULL || word[1] != '\0' || word[0] < '0' || word[0] > '3') {
        return -1;
    }
    *expect = word[0] - '0';
    argv[0] = "casecheck";
    while ((word = strtok(NULL, " ")) != NULL && argc < 64) {
        owned[argc] = 0;
        if (strncmp(word, "./", 2) == 0 || strncmp(word, "../", 3) == 0) {
            size_t len = strlen(dir) + strlen(word) + 2;
            char *path = json_alloc(len);

            snprintf(path, len, "%s/%s", dir, word);
            word = path;
            owned[argc] = 1;
        }
        argv[argc++] = word;
    }
    return argc;
}

/**
 * Runs one line of the expect file; returns 0 when it gives what the line
 * expects.
 */
static int selftest_line(char *line, int number, const char *dir)
{
    char *argv[64], *text, *copy = json_alloc(strlen(line) + 1);
    int owned[64], argc, expect, status, i, bad, absent;
    struct jbuf out = {0};

    strcpy(copy, line);
    argc = split_expect(line, dir, argv, owned, &text, &expect);
    if (argc < 0) {
        fprintf(stderr, "casecheck selftest: line %d: no status\n", number);
        free(copy);
        return 1;
    }
    jbuf_put(&out, "", 0);
    capture = &out;
    status = run(argc, argv);
    capture = NULL;
    absent = text != NULL && text[0] == '!';
    text = absent ? text + 1 : text;
    bad = status != expect || (text != NULL && (strstr(out.data, text) == NULL) != absent);
    if (bad) {
        fprintf(stderr, "casecheck selftest: line %d: %s\n  exit %d, expected %d%s%s\n%s", number,
                copy, status, expect,
                text == NULL ? ""
                : absent     ? ", without the text: "
                             : ", with the text: ",
                text != NULL ? text : "", out.data);
    }
    for (i = 1; i < argc; i++) {
        if (owned[i]) {
            free(argv[i]);
        }
    }
    jbuf_free(&out);
    free(copy);
    return bad;
}

/**
 * Runs every command of DIR/expect; returns 0 when each gives what it expects.
 */
static int selftest(const char *dir)
{
    char path[1024], schemas[1024], *text, *line, *next;
    size_t len;
    int number = 1, bad = 0, cases = 0;

    snprintf(schemas, sizeof schemas, "%s/../schemas", dir);
    default_schemas = schemas;
    snprintf(path, sizeof path, "%s/expect", dir);
    text = read_file(path, &len);
    if (text == NULL) {
        return 2;
    }
    for (line = text; *line != '\0'; line = next, number++) {
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        } else {
            next = line + strlen(line);
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }
        cases++;
        bad |= selftest_line(line, number, dir);
    }
    free(text);
    if (bad) {
        fprintf(stderr, "casecheck selftest: FAIL\n");
        return 1;
    }
    printf("casecheck selftest: %d cases OK\n", cases);
    return 0;
}

int main(int argc, char **argv)
{
    int status;

    if (argc == 3 && strcmp(argv[1], "--selftest") == 0) {
        return selftest(argv[2]);
    }
    status = run(argc, argv);
    if (status == 2 && found == 0) {
        usage();
    }
    return status;
}
