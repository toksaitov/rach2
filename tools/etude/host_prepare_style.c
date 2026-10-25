/* Source-check evidence is produced and checked by native Etude code. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "check.h"
#include "host_prepare.h"
#include "sha256.h"

static int same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

static int add_hash(struct json *value, const char *key, const char *path)
{
    char hash[65];
    if (hp_hash(path, hash))
        return -1;
    hp_member(value, key, hp_string(hash));
    return 0;
}

static void ordered(struct hp_words *words)
{
    for (size_t i = 1; i < words->n; i++) {
        char *word = words->v[i];
        size_t j = i;
        while (j && strcmp(words->v[j - 1], word) > 0) {
            words->v[j] = words->v[j - 1];
            j--;
        }
        words->v[j] = word;
    }
}

static int recipe_hash(const struct hp_words *files, char hex[65])
{
    struct sha256 recipe;
    unsigned char raw[32];
    sha256_init(&recipe);
    int bad = 0;
    for (size_t i = 0; !bad && i < files->n; i++) {
        size_t size = 0;
        char *bytes = read_file(files->v[i], &size);
        bad = !bytes;
        if (!bad) {
            sha256_update(&recipe, (const unsigned char *)files->v[i], strlen(files->v[i]) + 1);
            sha256_update(&recipe, (const unsigned char *)bytes, size);
            sha256_update(&recipe, (const unsigned char *)"", 1);
        }
        free(bytes);
    }
    sha256_final(&recipe, raw);
    sha256_hex(raw, hex);
    return bad ? -1 : 0;
}

static int pinned_sources(const char *text, const struct hp_words *pins, struct json *sources)
{
    int bad = !text;
    char hex[65];
    for (size_t i = 0; !bad && i < pins->n; i++) {
        char key[64], name[64];
        snprintf(key, sizeof(key), "%s_SHA256=", pins->v[i]);
        const char *pin = strstr(text, key);
        bad = !pin || (pin != text && pin[-1] != '\n');
        if (bad)
            break;
        pin += strlen(key);
        bad = strspn(pin, "0123456789abcdef") != 64 || (pin[64] && pin[64] != '\n');
        if (bad)
            break;
        memcpy(hex, pin, 64);
        hex[64] = 0;
        snprintf(name, sizeof(name), "%s", pins->v[i]);
        for (char *p = name; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        hp_member(sources, name, hp_string(hex));
    }
    return bad;
}

/* The builder's existing pinned-tool record remains the source of identity. */
static struct json *pinned_judge(struct hp_context *c, const char *tool, const char *prefix)
{
    struct hp_words files = {0}, licenses = {0}, pins = {0};
    struct json *expected = hp_value(JSON_OBJECT), *sources = hp_value(JSON_OBJECT);
    struct json *license_hashes = hp_value(JSON_OBJECT), *found = NULL;
    char path[4096], executable[64], hex[65];
    const char *script = "tools/setup/build_host_tool.sh";
    char *text = hp_read(script);
    int bad = !text;
    hp_split(&files, "tools/setup/build_host_tool.sh tools/setup/source_tool_record.py "
                     "mk/source-tools.mk tests/system/source_tools_test.py "
                     "tests/system/source_tools_test.ps1");
    hp_add(&pins, same(tool, "pmccabe") ? "PMCCABE" : "MANDOC");
    hp_add(&licenses, same(tool, "pmccabe") ? "pmccabe.txt" : "mandoc.txt");
    if (same(tool, "mandoc")) {
        struct hp_words supplemental = {0};
        hp_add(&pins, "ZLIB");
        hp_add(&licenses, "zlib.txt");
#ifdef _WIN32
        hp_add(&pins, "STRPTIME");
        hp_add(&licenses, "mandoc-strptime.c");
#endif
        hp_add(&files, "tools/setup/mandoc-batch.mk");
        bad = bad || hp_files(&supplemental, "tools/setup/mandoc-win");
        ordered(&supplemental);
        for (size_t i = 0; i < supplemental.n; i++) {
            const char *name = supplemental.v[i];
            size_t n = strlen(name);
            if (n > 2 && name[n - 2] == '.' && (name[n - 1] == 'c' || name[n - 1] == 'h'))
                hp_add(&files, name);
        }
        hp_words_free(&supplemental);
    }
    bad = bad || pinned_sources(text, &pins, sources);
    if (recipe_hash(&files, hex))
        bad = 1;
    hp_member(expected, "recipe_sha256", hp_string(hex));
#ifdef _WIN32
    snprintf(executable, sizeof(executable), "%s.exe", tool);
    struct json *native = hp_value(JSON_BOOLEAN);
    native->integer = 1;
#else
    snprintf(executable, sizeof(executable), "%s", tool);
    struct json *native = hp_value(JSON_BOOLEAN);
#endif
    hp_member(expected, "native_windows", native);
    hp_member(expected, "tool", hp_string(tool));
    hp_member(expected, "version", hp_string(same(tool, "pmccabe") ? "2.8" : "1.14.6"));
    hp_member(expected, "executable", hp_string(executable));
    snprintf(path, sizeof(path), "%s/bin/%s", prefix, executable);
    bad = bad || add_hash(expected, "binary_sha256", path);
    snprintf(path, sizeof(path), "%s/%s.check.json", prefix, tool);
    bad = bad || add_hash(expected, "checks_sha256", path);
    for (size_t i = 0; !bad && i < licenses.n; i++) {
        snprintf(path, sizeof(path), "%s/share/licenses/%s", prefix, licenses.v[i]);
        bad = add_hash(license_hashes, licenses.v[i], path);
    }
    hp_member(expected, "sources", sources);
    hp_member(expected, "licenses", license_hashes);
    snprintf(path, sizeof(path), "%s/%s.json", prefix, tool);
    found = hp_document(path);
    if (!bad && found && json_equal(expected, found)) {
        snprintf(path, sizeof(path), "%s/bin/%s", prefix, executable);
        char *resolved = realpath(path, NULL);
        if (resolved)
            hp_member(found, "path", hp_string(resolved));
        else
            bad = 1;
        free(resolved);
    } else
        bad = 1;
    json_free(expected);
    hp_words_free(&files);
    hp_words_free(&licenses);
    hp_words_free(&pins);
    free(text);
    if (bad) {
        json_free(found);
        fprintf(stderr, "host preparation: %s identity changed; make host-%s\n", tool, tool);
        return NULL;
    }
    (void)c;
    return found;
}

static struct json *judges(struct hp_context *c, const char *prefix)
{
    const char *name = c->only;
    if (same(name, "sizes") || same(name, "manual")) {
        struct json *found = pinned_judge(c, same(name, "sizes") ? "pmccabe" : "mandoc", prefix);
        if (found && same(name, "manual")) {
            struct json *inputs = hp_value(JSON_OBJECT);
            char path[4096];
            snprintf(path, sizeof(path), "%s/vendor/busybox/applet_tables-main.h",
                     same(c->arch, "x86_64") ? "build/x86_64" : "build");
            if (add_hash(inputs, path, path)) {
                json_free(inputs);
                json_free(found);
                return NULL;
            }
            hp_member(found, "prepared_inputs", inputs);
        }
        return found;
    }
    if (!same(name, "sparse"))
        return hp_value(JSON_NULL);
    const char *command = getenv("USERCHECK_CLANG");
    char *program = hp_program(command ? command : "clang");
    char output[4096], error[4096];
    snprintf(output, sizeof(output), "%s/clang.out", c->work);
    snprintf(error, sizeof(error), "%s/clang.err", c->work);
    char *argv[] = {program, "--version", NULL};
    char *version = NULL;
    if (program && !hp_run(c, argv, output, error))
        version = hp_read(output);
    struct json *found = NULL;
    if (version && *version) {
        version[strcspn(version, "\r\n")] = 0;
        found = hp_value(JSON_OBJECT);
        hp_member(found, "tool", hp_string("clang"));
        hp_member(found, "version", hp_string(version));
        hp_member(found, "path", hp_string(program));
        if (add_hash(found, "binary_sha256", program)) {
            json_free(found);
            found = NULL;
        }
    }
    free(program);
    free(version);
    return found;
}

static int fingerprint(struct hp_context *c, char hash[65])
{
    char output[4096], error[4096];
    snprintf(output, sizeof(output), "%s/fingerprint.out", c->work);
    snprintf(error, sizeof(error), "%s/fingerprint.err", c->work);
    char *argv[] = {"build/tests/cases/host/style", "--fingerprint", c->tree, NULL};
    if (hp_run(c, argv, output, error))
        return -1;
    char *text = hp_read(output);
    int bad = !text || strspn(text, "0123456789abcdef") != 64 || strcmp(text + 64, "\n");
    if (!bad) {
        memcpy(hash, text, 64);
        hash[64] = 0;
    }
    free(text);
    return bad ? -1 : 0;
}

/* The ordinary report judge first establishes complete assertion coverage. */
static int collect(struct hp_context *c, const char *id, const char *arch, const char *log,
                   struct json *record)
{
    struct cases cases = {0};
    cases.schemas = "tools/etude/schemas";
    cases.root = c->tree;
    char *argv[] = {"tests/cases/host.style.json", (char *)id, (char *)arch, "host", (char *)log};
    int bad = judge_report(&cases, argv, 5);
    free_manifests(&cases);
    if (bad)
        return -1;
    char *text = hp_read(log), *line = text;
    struct json *assertions = hp_value(JSON_ARRAY), *branches = hp_value(JSON_ARRAY);
    while (line) {
        char *end = strchr(line, '\n');
        if (end)
            *end++ = 0;
        struct hp_words words = {0};
        if (!strncmp(line, "@etude 1 ", 9) && !hp_split(&words, line + 9)) {
            if (words.n == 3 && same(words.v[0], "assert")) {
                bad = bad || !same(words.v[2], "pass");
                hp_item(assertions, hp_string(words.v[1]));
            } else if (words.n == 3 && same(words.v[0], "branch")) {
                bad = bad || !(same(words.v[2], "recorded") || same(words.v[2], "present") ||
                               same(words.v[2], "judged"));
                struct json *branch = hp_value(JSON_OBJECT);
                hp_member(branch, "group", hp_string(words.v[1]));
                hp_member(branch, "alternative", hp_string(words.v[2]));
                hp_item(branches, branch);
            }
        }
        hp_words_free(&words);
        line = end;
    }
    free(text);
    hp_member(record, "assertions", assertions);
    hp_member(record, "branches", branches);
    return bad ? -1 : 0;
}

static int write_style(struct hp_context *c, struct json *record, const char *id, const char *arch,
                       const char *source, const struct json *actual_judges)
{
    char timestamp[64] = {0};
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    int bad = !utc || !strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", utc);
    hp_member(record, "version", hp_number(1));
    hp_member(record, "case_id", hp_string(id));
    hp_member(record, "architecture", hp_string(arch));
    hp_member(record, "source_sha256", hp_string(source));
    hp_member(record, "prepared_at", hp_string(timestamp));
    struct hp_words command = {0};
    hp_split(&command, "sh tests/cases/host/style.sh");
    hp_add(&command, c->only);
    hp_member(record, "command", hp_array(&command));
    hp_words_free(&command);
    hp_member(record, "actual_judges", hp_clone(actual_judges));
    char *versions = hp_read("tools/setup/judges");
    if (versions)
        hp_member(record, "judges", hp_string(versions));
    else
        bad = 1;
    free(versions);
    if (!bad)
        bad = hp_publish(c->output, record);
    return bad;
}

static int run_style(struct hp_context *c, const char *prefix, const char *id, const char *arch,
                     const char *source, const struct json *actual_judges)
{
    char log[4096], after[65];
    struct json *record = hp_value(JSON_OBJECT);
    struct jbuf path = {0};
    jbuf_puts(&path, prefix);
    jbuf_puts(&path, "/bin");
#ifdef _WIN32
    jbuf_puts(&path, ";");
#else
    jbuf_puts(&path, ":");
#endif
    jbuf_puts(&path, getenv("PATH") ? getenv("PATH") : "");
    int bad = setenv("PATH", path.data, 1) || setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    jbuf_free(&path);
    snprintf(log, sizeof(log), "%s.log", c->output);
    char *argv[] = {"sh", "tests/cases/host/style.sh", (char *)c->only, NULL};
    bad = bad || hp_run(c, argv, log, log) || collect(c, id, arch, log, record) ||
          fingerprint(c, after) || strcmp(source, after);
    struct json *final_judges = judges(c, prefix);
    bad = bad || !final_judges || !json_equal(actual_judges, final_judges);
    json_free(final_judges);
    if (!bad) {
        bad = write_style(c, record, id, arch, source, actual_judges);
    }
    json_free(record);
    return bad;
}

static char *source_tool_prefix(struct hp_context *c)
{
    const char *configured = getenv("SOURCE_TOOL_PREFIX");
    char *prefix = realpath(configured ? configured : "build/host-tools/native", NULL);
    if (!prefix) {
        struct jbuf path = {0};
        if (!configured || configured[0] != '/') {
            jbuf_puts(&path, c->tree);
            jbuf_puts(&path, "/");
        }
        jbuf_puts(&path, configured ? configured : "build/host-tools/native");
        prefix = path.data;
    }
    return prefix;
}

static int style_name(const char *name)
{
    return name && strlen(name) <= 80 &&
           strspn(name, "abcdefghijklmnopqrstuvwxyz-") == strlen(name);
}

int hp_style(struct hp_context *c)
{
    if (!style_name(c->only))
        return -1;
    char *prefix = source_tool_prefix(c);
    char id[128], source[65];
    snprintf(id, sizeof(id), "host.style.%s", c->only);
    struct json *manifest = hp_document("tests/cases/host.style.json");
    const struct json *cases = json_member(manifest, "cases"), *definition = NULL;
    for (size_t i = 0; cases && i < cases->count; i++)
        if (same(json_text(cases->item[i], "case_id"), id))
            definition = cases->item[i];
    struct json *actual_judges = prefix ? judges(c, prefix) : NULL;
    int bad = !definition || !actual_judges || fingerprint(c, source);
    const struct json *architecture = json_member(definition, "architectures");
    const char *arch = architecture && architecture->type == JSON_STRING ? "none" : c->arch;
    struct json *old = hp_document(c->output);
    const struct json *old_judges = json_member(old, "actual_judges");
    char *reader[] = {"build/tests/cases/host/style", (char *)c->only, NULL};
    char scratch[4096], error[4096];
    snprintf(scratch, sizeof(scratch), "%s/reader.out", c->work);
    snprintf(error, sizeof(error), "%s/reader.err", c->work);
    if (setenv("ARCH", c->arch, 1) || setenv("ETUDE_TREE", c->tree, 1) ||
        setenv("ETUDE_CASE", id, 1))
        bad = 1;
    int reused = !bad && old_judges && json_equal(old_judges, actual_judges) &&
                 same(json_text(old, "source_sha256"), source) &&
                 same(json_text(old, "case_id"), id) &&
                 same(json_text(old, "architecture"), arch) && !hp_run(c, reader, scratch, error);
    if (!bad && !reused) {
        bad = run_style(c, prefix, id, arch, source, actual_judges);
    }
    if (!bad)
        printf("style preparation: %s: %s\n", c->only, reused ? "unchanged evidence" : "passed");
    json_free(actual_judges);
    json_free(old);
    json_free(manifest);
    free(prefix);
    return bad ? -1 : 0;
}
