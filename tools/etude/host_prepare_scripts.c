/* The declared host.list partitions share the native inventory validator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "host_prepare.h"
#include "../../tests/cases/host/host_script_inventory.h"

static int same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

static int add_option(struct hp_words *words, const char *key, const char *value)
{
    struct jbuf text = {0};
    jbuf_puts(&text, key);
    jbuf_puts(&text, "=");
    jbuf_puts(&text, value);
    hp_add(words, text.data);
    jbuf_free(&text);
    return 0;
}

static int kernel_line(struct hp_context *c, struct hp_check *check, const char *line,
                       const char *extractor, struct hp_words *generated)
{
    struct hp_words words = {0}, flags = {0};
    int bad = hp_split(&words, line);
    int matches = !bad && words.n >= c->compiler.n;
    for (size_t i = 0; matches && i < c->compiler.n; i++)
        matches = same(words.v[i], c->compiler.v[i]);
    if (matches) {
        const char *source = NULL;
        for (size_t i = c->compiler.n; !bad && i < words.n; i++) {
            const char *word = words.v[i];
            if (same(word, "-MF") || same(word, "-MT") || same(word, "-o"))
                bad = ++i == words.n;
            else if (same(word, "-c")) {
                bad = ++i == words.n;
                if (!bad)
                    source = words.v[i];
            } else if (!same(word, "-MMD") && !same(word, "-MP"))
                hp_add(&flags, word);
        }
        if (source) {
            hp_add(&check->sources, source);
            if (!check->flags.n) {
                check->flags = flags;
                memset(&flags, 0, sizeof(flags));
            }
        }
    } else if (words.n > 2 && same(words.v[0], extractor) && same(words.v[2], "-f")) {
        hp_add(generated, words.v[1]);
    }
    hp_words_free(&words);
    hp_words_free(&flags);
    return bad;
}

/* Read Make's extraction and compiler recipes without executing them. */
static int kernel_recipe(struct hp_context *c, struct hp_check *check, const char *base,
                         const char *program)
{
    struct hp_words argv = {0}, generated = {0};
    char output[4096], error[4096], extractor[4096];
    if (hp_path(extractor, sizeof(extractor), "%s/extract", base))
        return -1;
    char *compiler = hp_make_words(&c->compiler);
    hp_add(&argv, getenv("MAKE") ? getenv("MAKE") : "make");
    hp_add(&argv, "--no-print-directory");
    hp_add(&argv, "-Bn");
    hp_add(&argv, "-f");
    hp_add(&argv, "mk/host-fixtures.mk");
    add_option(&argv, "HOSTCC", compiler);
    add_option(&argv, "HOST_FIXTURE_PREFIX", base);
    hp_add(&argv, program);
    int bad = hp_work_files(c, "kernel-plan", output, error) || hp_run(c, argv.v, output, error);
    char *text = !bad ? hp_read(output) : NULL;
    for (char *line = text; line && !bad;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        bad = kernel_line(c, check, line, extractor, &generated);
        line = next;
    }
    if (!check->sources.n)
        bad = 1;
    if (!bad && !c->print && generated.n) {
        /* Extract existing fragments before the compiler's include scan. */
        free(argv.v[argv.n - 1]);
        argv.v[argv.n - 1] = NULL;
        argv.n--;
        free(argv.v[2]);
        argv.v[2] = strdup("-s");
        for (size_t i = 0; i < generated.n; i++)
            hp_add(&argv, generated.v[i]);
        bad = hp_run(c, argv.v, output, error);
    }
    hp_add(&check->data, "mk/host-fixtures.mk");
    hp_add(&check->data, "tests/host/fixtures/extract.c");
    free(compiler);
    free(text);
    hp_words_free(&generated);
    hp_words_free(&argv);
    return bad ? -1 : 0;
}

static struct json *entry(const char *id, const char *label, const char *program,
                          const char *backend)
{
    struct json *record = hp_value(JSON_OBJECT);
    hp_member(record, "assertion_id", hp_string(id));
    hp_member(record, "label", hp_string(label));
    hp_member(record, "mode", hp_string("shared"));
    hp_member(record, "program", hp_string(program));
    hp_member(record, "arguments", hp_value(JSON_ARRAY));
    hp_member(record, "sha256", hp_value(JSON_NULL));
    hp_member(record, "inputs", hp_value(JSON_ARRAY));
    hp_member(record, "unsupported", hp_value(JSON_NULL));
    hp_member(record, "backend", hp_string(backend));
    hp_member(record, "instrumentation", hp_value(program ? JSON_ARRAY : JSON_NULL));
    hp_member(record, "command", hp_value(JSON_NULL));
    return record;
}

static int script_program(struct hp_context *c, struct hp_check *check, const char *base,
                          const char *program, int kernel, const struct json *adapter)
{
    int bad = 0;
    if (kernel) {
        check->objects = 1;
        bad = kernel_recipe(c, check, base, program) || hp_build(c, check, program, 0);
        if (!bad) {
            struct json *instrumentation = hp_value(JSON_ARRAY);
#ifndef _WIN32
            hp_item(instrumentation, hp_string("address"));
            hp_item(instrumentation, hp_string("undefined"));
#ifdef __APPLE__
            hp_add(&check->tools, "atos");
            hp_member(check->entry, "sanitizer_symbolizer",
                      hp_string("build/tests/host-suite/sanitizer/atos"));
#endif
#endif
            hp_member(check->entry, "instrumentation", instrumentation);
            bad = hp_tools(c, check);
        }
    } else if (adapter && !c->print) {
        char hash[65];
        bad = hp_hash(program, hash);
        hp_member(check->entry, "sha256", hp_string(hash));
        if (json_member(adapter, "arguments"))
            hp_member(check->entry, "arguments", hp_clone(json_member(adapter, "arguments")));
    }
    return bad;
}

static int append_script(struct hp_context *c, struct json *checks, const char *base,
                         const struct host_script_row *row, const struct json *adapters,
                         char *const fields[2])
{
    int bad = 0;
    const char *route = row->part == SCRIPT_NATIVE_PART ? "native" : "gnu";
    if (c->gnu)
        route = "gnu";
    if (c->only && strcmp(c->only, fields[0]))
        return 0;
    struct hp_check check;
    memset(&check, 0, sizeof(check));
    char program[4096];
    int kernel = row->kind == SCRIPT_KERNEL;
    const struct json *adapter = json_member(adapters, fields[0]);
    if (kernel ? hp_path(program, sizeof(program), "%s/%s", base, fields[0])
               : hp_path(program, sizeof(program), "%s",
                         json_text(adapter, "program") ? json_text(adapter, "program") : ""))
        return 1;
    int active = c->gnu ? same(route, "gnu") : same(route, "native");
    check.entry = entry(fields[0], fields[1], active && *program ? program : NULL, route);
    if (active)
        bad = script_program(c, &check, base, program, kernel, adapter);
    if (!adapter && !kernel) {
        struct jbuf detail = {0};
        jbuf_puts(&detail, "no native adapter for the behavioral check: ");
        jbuf_puts(&detail, fields[0]);
        hp_member(check.entry, "unsupported", hp_string(detail.data));
        jbuf_free(&detail);
    }
    const struct json *previous = json_member(check.entry, "inputs");
    struct json *inputs = hp_clone(previous);
    bad = bad || hp_input(c, "tests/cases/host/host.list", inputs, 0) ||
          hp_input(c, "tests/cases/host/adapters.json", inputs, 0);
    hp_member(check.entry, "inputs", inputs);
    if (!bad) {
        hp_item(checks, check.entry);
        check.entry = NULL;
    }
    json_free(check.entry);
    hp_words_free(&check.sources);
    hp_words_free(&check.flags);
    hp_words_free(&check.libraries);
    hp_words_free(&check.data);
    hp_words_free(&check.tools);
    return bad;
}

int hp_scripts(struct hp_context *c, struct json *checks, const char *base)
{
    struct host_script_row rows[HOST_SCRIPT_ROWS];
    size_t count = 0;
    if (!host_script_inventory("tests/cases/host/host.list", "tests/cases/host.host.json", rows,
                               &count))
        return -1;
    FILE *list = fopen("tests/cases/host/host.list", "rb");
    struct json *adapters = hp_document("tests/cases/host/adapters.json");
    char line[4096];
    size_t row = 0;
    int bad = !list || !adapters;
    while (!bad && fgets(line, sizeof(line), list)) {
        if (!*line || *line == '#' || *line == '\n')
            continue;
        char *label = strchr(line, '|');
        if (!label || row == count) {
            bad = 1;
            break;
        }
        *label++ = 0;
        label[strcspn(label, "\r\n")] = 0;
        if (strchr(label, '|') || strcmp(rows[row].id, line)) {
            bad = 1;
            break;
        }
        char *fields[] = {line, label};
        bad = append_script(c, checks, base, &rows[row++], adapters, fields);
    }
    if (list) {
        bad = bad || ferror(list) || row != count;
        fclose(list);
    }
    json_free(adapters);
    return bad ? -1 : 0;
}
