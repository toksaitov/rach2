/* Native preparation of the tools declared by host checks. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "host_prepare.h"
#include "posix/legacy_cases.h"

static int same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

static int order(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int copy_file(const char *from, const char *to)
{
    struct stat st;
    unsigned char bytes[65536];
    size_t n;
    FILE *input = fopen(from, "rb"), *output = NULL;
    int bad = !input || stat(from, &st);
    if (!bad)
        output = fopen(to, "wb");
    bad = bad || !output;
    while (!bad && (n = fread(bytes, 1, sizeof(bytes), input)))
        bad = fwrite(bytes, 1, n, output) != n;
    if (input) {
        bad = bad || ferror(input);
        fclose(input);
    }
    if (output && fclose(output))
        bad = 1;
    if (!bad)
        bad = chmod(to, st.st_mode & 0777);
    return bad ? -1 : 0;
}

static int native_magic(const char *path)
{
    unsigned char bytes[4];
    FILE *file = fopen(path, "rb");
    size_t n = file ? fread(bytes, 1, sizeof(bytes), file) : 0;
    if (file)
        fclose(file);
    return n == 4 &&
           (!memcmp(bytes, "\177ELF", 4) || !memcmp(bytes, "\317\372\355\376", 4) ||
            !memcmp(bytes, "\376\355\372\317", 4) || !memcmp(bytes, "\316\372\355\376", 4) ||
            !memcmp(bytes, "\376\355\372\316", 4) || !memcmp(bytes, "\312\376\272\276", 4) ||
            !memcmp(bytes, "MZ", 2));
}

/**
 * Place the native tool `source` at `to`: a copy, or on macOS a link for a
 * program of the sealed system volume, which macOS kills when a copy of it
 * runs from any other path. That volume does not change while the system runs.
 */
static int place_tool(const char *source, const char *to)
{
#ifdef __APPLE__
    static const char *const sealed[] = {"/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/",
                                         "/usr/libexec/"};
    for (size_t i = 0; i < sizeof(sealed) / sizeof(sealed[0]); i++)
        if (!strncmp(source, sealed[i], strlen(sealed[i])))
            return symlink(source, to) ? -1 : 0;
#endif
    return copy_file(source, to);
}

static int copied_tool(struct hp_context *c, const char *root, const char *name, const char *source,
                       struct json *inputs)
{
    char output[4096], temporary[4096] = {0}, provenance[4096], before[65], after[65], copy[65];
    if (snprintf(output, sizeof(output), "%s/%s", root, name) >= (int)sizeof(output) ||
        snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", output, (long)getpid()) >=
            (int)sizeof(temporary) ||
        snprintf(provenance, sizeof(provenance), "%s.provenance.json", output) >=
            (int)sizeof(provenance) ||
        hp_mkdir(root) || hp_hash(source, before) || place_tool(source, temporary) ||
        !native_magic(temporary) || hp_hash(temporary, copy) || hp_hash(source, after) ||
        strcmp(before, after) || strcmp(before, copy)) {
        if (temporary[0])
            unlink(temporary);
        return -1;
    }
    if (rename(temporary, output)) {
        unlink(temporary);
        return -1;
    }
    struct json *record = hp_value(JSON_OBJECT);
    hp_member(record, "source", hp_string(source));
    hp_member(record, "sha256", hp_string(copy));
    int bad = hp_publish(provenance, record) || hp_input(c, output, inputs, 0) ||
              hp_input(c, provenance, inputs, 0);
    json_free(record);
    return bad ? -1 : 0;
}

static char *external_tool(const char *name)
{
#ifdef __APPLE__
    if (same(name, "atos"))
        return realpath("/usr/bin/atos", NULL);
#endif
    const char *prepared = getenv("HOST_TEST_PREPARED_TOOLS");
    char path[4096];
    if (!prepared || snprintf(path, sizeof(path), "%s/%s", prepared, name) >= (int)sizeof(path))
        return NULL;
    char *result = realpath(path, NULL);
    if (!result && strlen(path) < sizeof(path) - 4) {
        strcat(path, ".exe");
        result = realpath(path, NULL);
    }
    return result;
}

static int sanitizer(struct hp_context *c, const char *root, struct json *inputs)
{
    struct json *bound = hp_value(JSON_ARRAY), *record = hp_value(JSON_OBJECT);
    int bad = hp_mkdir(root);
#ifdef __APPLE__
    char *source = external_tool("atos");
    bad = bad || !source || copied_tool(c, root, "atos", source, bound);
    free(source);
    const char *platform = "darwin";
#elif defined(_WIN32)
    const char *platform = "win32";
#else
    const char *platform = "linux";
#endif
    hp_member(record, "schema_version", hp_number(1));
    hp_member(record, "platform", hp_string(platform));
    hp_member(record, "inputs", hp_clone(bound));
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/tools.json", root) >= (int)sizeof(path))
        bad = 1;
    if (!bad)
        bad = hp_publish(path, record);
    for (size_t i = 0; !bad && i < bound->count; i++)
        hp_item(inputs, hp_clone(bound->item[i]));
    if (!bad)
        bad = hp_input(c, path, inputs, 0);
    json_free(bound);
    json_free(record);
    return bad ? -1 : 0;
}

static struct json *files_record(const char *root)
{
    struct hp_words files = {0};
    struct json *record = hp_value(JSON_OBJECT);
    if (hp_files(&files, root))
        goto failed;
    if (files.n > 1)
        qsort(files.v, files.n, sizeof(*files.v), order);
    for (size_t i = 0; i < files.n; i++) {
        char hash[65];
        if (hp_hash(files.v[i], hash))
            goto failed;
        hp_member(record, files.v[i] + strlen(root) + 1, hp_string(hash));
    }
    hp_words_free(&files);
    return record;
failed:
    hp_words_free(&files);
    json_free(record);
    return NULL;
}

/* The independent fixture marker predates ASCII contract documents. Keep
 * its filename escapes and byte format, without relaxing the JSON reader. */
static int filename_code(const unsigned char **cursor, unsigned *result)
{
    const unsigned char *p = *cursor;
    unsigned code = *p++;
    if (code >= 128) {
        unsigned extra = code >= 0xf0 ? 3 : code >= 0xe0 ? 2 : code >= 0xc2 ? 1 : 0;
        if (!extra || code > 0xf4)
            return -1;
        code &= extra == 3 ? 7 : extra == 2 ? 15 : 31;
        for (unsigned i = 0; i < extra; i++) {
            if ((*p & 0xc0) != 0x80)
                return -1;
            code = (code << 6) | (*p++ & 63);
        }
        if ((extra == 1 && code < 128) || (extra == 2 && code < 2048) ||
            (extra == 3 && code < 65536) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return -1;
    }
    *cursor = p;
    *result = code;
    return 0;
}

static int filename_json(struct jbuf *out, const char *name)
{
    jbuf_puts(out, "\"");
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        unsigned code;
        if (filename_code(&p, &code))
            return -1;
        if (code < 127) {
            const char *escape = code == '"'    ? "\\\""
                                 : code == '\\' ? "\\\\"
                                 : code == '\b' ? "\\b"
                                 : code == '\f' ? "\\f"
                                 : code == '\n' ? "\\n"
                                 : code == '\r' ? "\\r"
                                 : code == '\t' ? "\\t"
                                                : NULL;
            if (escape) {
                jbuf_puts(out, escape);
                continue;
            }
            if (code >= 32) {
                char byte = (char)code;
                jbuf_put(out, &byte, 1);
                continue;
            }
        }
        char encoded[16];
        if (code <= 65535)
            snprintf(encoded, sizeof(encoded), "\\u%04x", code);
        else {
            code -= 65536;
            snprintf(encoded, sizeof(encoded), "\\u%04x\\u%04x", 0xd800 + (code >> 10),
                     0xdc00 + (code & 1023));
        }
        jbuf_puts(out, encoded);
    }
    jbuf_puts(out, "\"");
    return 0;
}

static char *exfat_marker(const char *hash, const struct json *files)
{
    struct jbuf bytes = {0};
    jbuf_puts(&bytes, "{\"files\": {");
    for (size_t i = 0; i < files->count; i++) {
        if (i)
            jbuf_puts(&bytes, ", ");
        if (filename_json(&bytes, files->item[i]->name)) {
            jbuf_free(&bytes);
            return NULL;
        }
        jbuf_puts(&bytes, ": ");
        filename_json(&bytes, files->item[i]->string);
    }
    jbuf_puts(&bytes, "}, \"producer_sha256\": ");
    filename_json(&bytes, hash);
    jbuf_puts(&bytes, "}\n");
    return bytes.data;
}

static int publish_exfat(struct hp_context *c, const char *root, const char *temporary,
                         const char *hash, const struct json *files)
{
    struct stat st;
    int bad = 0;
    /* Publication of an existing fixture is completed by the native
     * replacement operation, after the old marker has been invalidated. */
    char backup[4096];
    snprintf(backup, sizeof(backup), "%s.%ld.previous", root, (long)getpid());
    int had_old = lstat(root, &st) == 0;
    if (lstat(backup, &st) == 0 || (had_old && rename(root, backup)) || rename(temporary, root)) {
        if (had_old && lstat(root, &st) != 0)
            (void)rename(backup, root);
        bad = 1;
    } else if (had_old)
        bad = legacy_remove_tree(backup);
    char *record = exfat_marker(hash, files), marker[4096];
    if (!record || snprintf(marker, sizeof(marker), "%s.%ld.tmp", c->output, (long)getpid()) >=
                       (int)sizeof(marker))
        bad = 1;
    if (!bad)
        bad = hp_write(marker, record, strlen(record)) || rename(marker, c->output);
    free(record);
    return bad;
}

static int external_exfat(struct hp_context *c)
{
    const char *root = "build/tests/host-suite/tools/exfat_external";
    const char *source = "tests/host/mkexfat_external.sh";
    char hash[65], after[65];
    struct json *files = files_record(root);
    int bad = hp_hash(source, hash);
    char *old = hp_read(c->output);
    char *expected = !bad && files ? exfat_marker(hash, files) : NULL;
    int reuse = old && expected && files->count && !strcmp(old, expected);
    free(old);
    free(expected);
    json_free(files);
    if (reuse)
        return 0;
    /* The existing independent formatter owns its normal fixture directory.
     * A failed production is kept, never replaced by a fabricated marker. */
    char temporary[4096], writer[4096], output[4096], error[4096];
    struct stat st;
    if (hp_path(temporary, sizeof(temporary), "%s.%ld.tmp", root, (long)getpid()) ||
        hp_path(writer, sizeof(writer), "%s/writer", temporary) ||
        hp_work_files(c, "exfat", output, error) || !lstat(temporary, &st) || hp_mkdir(temporary))
        return -1;
    char *absolute = realpath(temporary, NULL);
    char *first[] = {"sh", (char *)source, "-p", writer, NULL};
    char *second[] = {"sh", (char *)source, absolute, NULL};
    bad = bad || !absolute || hp_run(c, first, output, error) || hp_run(c, second, output, error) ||
          hp_hash(source, after) || strcmp(hash, after);
    free(absolute);
    files = !bad ? files_record(temporary) : NULL;
    const struct json *image = json_member(files, "exfat_external.img");
    const struct json *manifest = json_member(files, "exfat_external.manifest");
    bad = bad || !files || !files->count || !image || !manifest;
    if (!bad) {
        bad = publish_exfat(c, root, temporary, hash, files);
    }
    if (bad)
        fprintf(stderr, "host preparation: independent exFAT evidence: %s\n", temporary);
    json_free(files);
    return bad ? -1 : 0;
}

static int merge_inputs(struct json *entry, const struct json *inputs)
{
    const struct json *old = json_member(entry, "inputs");
    struct json *all = hp_clone(old);
    if (all->type != JSON_ARRAY) {
        json_free(all);
        return -1;
    }
    for (size_t i = 0; i < inputs->count; i++) {
        const char *path = json_text(inputs->item[i], "path");
        int found = 0;
        for (size_t j = 0; path && j < all->count; j++)
            if (same(path, json_text(all->item[j], "path"))) {
                if (!json_equal(inputs->item[i], all->item[j])) {
                    json_free(all);
                    return -1;
                }
                found = 1;
            }
        if (!found)
            hp_item(all, hp_clone(inputs->item[i]));
    }
    hp_member(entry, "inputs", all);
    return 0;
}

static struct json *tool_inventory(const struct json *marker, const char *name)
{
    const struct json *version = json_member(marker, "schema_version");
    const struct json *recipe = json_member(marker, "recipe");
    if (!version || version->type != JSON_INTEGER || version->integer != 1 || !recipe ||
        !json_text(marker, "sha256") || recipe->type != JSON_OBJECT ||
        !json_member(recipe, "compiler") || json_member(recipe, "compiler")->type != JSON_OBJECT ||
        !json_member(recipe, "command") || json_member(recipe, "command")->type != JSON_ARRAY ||
        !json_member(recipe, "inputs") || json_member(recipe, "inputs")->type != JSON_ARRAY ||
        !json_member(recipe, "compiler_inputs") ||
        json_member(recipe, "compiler_inputs")->type != JSON_ARRAY)
        return NULL;
    struct json *inventory = hp_value(JSON_OBJECT), *checks = hp_value(JSON_ARRAY);
    const struct json *sealed = json_member(marker, "inventory_entry");
    struct json *entry = sealed ? hp_clone(sealed) : hp_value(JSON_OBJECT);
    if (!sealed) {
        hp_member(entry, "assertion_id", hp_string(name));
        hp_member(entry, "sha256", hp_clone(json_member(marker, "sha256")));
        hp_member(entry, "command", hp_clone(json_member(recipe, "command")));
        hp_member(entry, "inputs", hp_clone(json_member(recipe, "inputs")));
        hp_member(entry, "compiler_inputs", hp_clone(json_member(recipe, "compiler_inputs")));
    }
    hp_item(checks, entry);
    hp_member(inventory, "compiler", hp_clone(json_member(recipe, "compiler")));
    hp_member(inventory, "checks", checks);
    return inventory;
}

static int program_record(struct hp_context *c, struct hp_check *check, const char *program,
                          const char *marker_path)
{
    struct hp_words command = {0};
    for (size_t i = 0; i < c->compiler.n; i++)
        hp_add(&command, c->compiler.v[i]);
    for (size_t i = 0; i < check->flags.n; i++)
        hp_add(&command, check->flags.v[i]);
    hp_add(&command, "-o");
    hp_add(&command, program);
    for (size_t i = 0; i < check->sources.n; i++)
        hp_add(&command, check->sources.v[i]);
    for (size_t i = 0; i < check->libraries.n; i++)
        hp_add(&command, check->libraries.v[i]);
    struct json *record = hp_value(JSON_OBJECT), *recipe = hp_value(JSON_OBJECT);
    hp_member(record, "schema_version", hp_number(1));
    hp_member(recipe, "compiler", hp_clone(c->identity));
    hp_member(recipe, "command", hp_array(&command));
    hp_member(recipe, "inputs", hp_clone(json_member(check->entry, "compile_inputs")));
    hp_member(recipe, "compiler_inputs", hp_clone(json_member(check->entry, "compiler_inputs")));
    hp_member(record, "recipe", recipe);
    hp_member(record, "actual_command", hp_clone(json_member(check->entry, "command")));
    hp_member(record, "sha256", hp_clone(json_member(check->entry, "sha256")));
    hp_member(record, "inventory_entry", hp_clone(check->entry));
    int bad = hp_publish(marker_path, record);
    hp_words_free(&command);
    json_free(record);
    return bad;
}

static int tool_bytes(const struct json *marker, const char *origin, const char *program)
{
    if (marker) {
        char path[4096], hash[65];
        if (origin)
            snprintf(path, sizeof(path), "%s/%s", origin, program);
        else
            snprintf(path, sizeof(path), "%s", program);
        if (hp_hash(path, hash) || !same(hash, json_text(marker, "sha256")))
            return -1;
    }
    return 0;
}

/**
 * Reports a local tool record that does not describe the program beside it.
 *
 * The record is invalid when it exists but is not a complete version 1 record,
 * and the program has changed when its SHA-256 differs from the record's. With
 * `repair` set, the record is discarded and the program rebuilt; otherwise the
 * fault stops the preparation.
 *
 * @return 0 when the record is valid or absent, 1 when it was discarded, and -1
 * on a fault without `repair`.
 */
static int local_record(const char *program, const char *marker_path, const char *name, int repair,
                        struct json **marker)
{
    struct json *old = *marker ? tool_inventory(*marker, name) : NULL;
    struct stat metadata;
    const char *fault = NULL, *path = marker_path;
    if (*marker ? !old : lstat(marker_path, &metadata) == 0)
        fault = "invalid compiled tool record";
    else if (tool_bytes(*marker, NULL, program)) {
        fault = "prepared host tool bytes changed";
        path = program;
    }
    json_free(old);
    if (!fault)
        return 0;
    fprintf(stderr, "host preparation: %s%s: %s\n", fault, repair ? ", rebuilding" : "", path);
    if (!repair)
        return -1;
    json_free(*marker);
    *marker = NULL;
    return 1;
}

/**
 * Compiles `program`, or reuses it when its `.build.json` record matches.
 *
 * A record is reused only when the compiler identity, the command, the current
 * dependency scan and the program's SHA-256 all match it. A record from the
 * imported preparation in `c->origin` is used when there is no local one. A
 * local record that is invalid or does not match the program's bytes stops the
 * preparation, or with `repair` set is discarded so that the program is
 * rebuilt.
 */
static int prepared_program(struct hp_context *c, struct hp_check *check, const char *program,
                            const char *name, int repair, struct json *inputs)
{
    char marker_path[4096], previous[4096];
    if (snprintf(marker_path, sizeof(marker_path), "%s.build.json", program) >=
        (int)sizeof(marker_path))
        return -1;
    struct json *marker = c->print ? NULL : hp_document(marker_path);
    if (!c->print && local_record(program, marker_path, name, repair, &marker) < 0) {
        json_free(marker);
        return -1;
    }
    const char *origin = NULL;
    if (!marker && c->origin &&
        snprintf(previous, sizeof(previous), "%s/%s", c->origin, marker_path) <
            (int)sizeof(previous)) {
        marker = hp_document(previous);
        if (marker)
            origin = c->origin;
    }
    struct json *old = marker ? tool_inventory(marker, name) : NULL;
    if (marker && !old) {
        fprintf(stderr, "host preparation: invalid compiled tool record: %s\n", previous);
        json_free(marker);
        return -1;
    }
    if (origin && tool_bytes(marker, origin, program)) {
        fprintf(stderr, "host preparation: prepared host tool bytes changed: %s/%s\n", origin,
                program);
        json_free(old);
        json_free(marker);
        return -1;
    }
    struct json *saved = c->old;
    const char *saved_origin = c->origin;
    c->old = old;
    c->origin = origin;
    int bad = hp_build(c, check, program, 1);
    c->old = saved;
    c->origin = saved_origin;
    if (bad || c->print)
        goto done;
    if (check->reused && origin)
        bad = copy_file(previous, marker_path);
    if (!bad && !check->reused) {
        bad = program_record(c, check, program, marker_path);
    }
    if (!bad) {
        const struct json *values = json_member(check->entry, "inputs");
        for (size_t i = 0; values && i < values->count; i++)
            hp_item(inputs, hp_clone(values->item[i]));
        bad = hp_input(c, program, inputs, 0) || hp_input(c, marker_path, inputs, 0);
    }
done:
    json_free(old);
    json_free(marker);
    return bad ? -1 : 0;
}

static int compiled_tool(struct hp_context *c, const char *name, const struct hp_words *flags,
                         struct json *inputs)
{
    static const struct {
        const char *name, *sources;
    } recipes[] = {
        {"mkfatfixture", "tests/host/mkfatfixture.c"},
        {"etude-workload",
         "tests/host/fixtures/etude_workload.c tests/host/fixtures/guest_workload.c"},
        {"mkexfatfixture", "tests/host/mkexfatfixture.c"},
        {"setidcheck", "tools/inspect/setidcheck.c"},
        {"etude-volume", "tools/etude/volume/etude_volume.c tools/etude/volume/volume.c"}};
    const char *sources = NULL;
    for (size_t i = 0; i < sizeof(recipes) / sizeof(recipes[0]); i++)
        if (same(name, recipes[i].name))
            sources = recipes[i].sources;
    if (!sources)
        return -1;
    char program[4096];
    snprintf(program, sizeof(program), "build/tests/host-suite/tools/%s", name);
    struct hp_check check;
    memset(&check, 0, sizeof(check));
    check.entry = hp_value(JSON_OBJECT);
    hp_member(check.entry, "assertion_id", hp_string(name));
    hp_member(check.entry, "inputs", hp_value(JSON_ARRAY));
    hp_split(&check.sources, sources);
    for (size_t i = 0; i < flags->n; i++) {
        const char *word = flags->v[i];
        if (same(name, "etude-workload") &&
            (!strncmp(word, "-fsanitize=", 11) || !strncmp(word, "-fno-sanitize-recover=", 22) ||
             same(word, "-g")))
            continue;
        hp_add(&check.flags, word);
    }
    hp_split(&check.flags, "-Wall -Wextra -Werror");
    int bad = hp_mkdir("build/tests/host-suite/tools") ||
              prepared_program(c, &check, program, name, 0, inputs);
    hp_words_free(&check.sources);
    hp_words_free(&check.flags);
    hp_words_free(&check.libraries);
    json_free(check.entry);
    return bad ? -1 : 0;
}

static int companions(struct hp_context *c, struct hp_check *owner, struct json *inputs)
{
    const char *program = json_text(owner->entry, "program");
    char root[4096];
    if (!program || strlen(program) >= sizeof(root))
        return -1;
    strcpy(root, program);
    char *end = strrchr(root, '/');
    if (!end)
        return -1;
    *end = 0;
    static const char *const names[] = {"wintarget.exe", "winstand.exe", "etude.exe",
                                        "etude-volume.exe", "image-command.exe"};
    int bad = 0;
    for (size_t i = 0; !bad && i < sizeof(names) / sizeof(names[0]); i++) {
        struct hp_check check;
        memset(&check, 0, sizeof(check));
        check.entry = hp_value(JSON_OBJECT);
        hp_member(check.entry, "assertion_id", hp_string(names[i]));
        hp_member(check.entry, "inputs", hp_value(JSON_ARRAY));
        hp_windows_flags(&check.flags);
        if (i == 0) {
            hp_add(&check.sources, "tools/etude/win/wintarget.c");
            hp_add(&check.libraries, "-municode");
        } else if (i == 1) {
            hp_split(&check.sources, "tools/etude/win/winstand.c tools/etude/volume/volume.c");
            bad = hp_source_list(c, &check.sources, "ETUDE_WIN_FILE_SRC");
        } else if (i == 2)
            bad = hp_source_list(c, &check.sources, "ETUDE_WIN_SRC");
        else if (i == 3)
            bad = hp_source_list(c, &check.sources, "ETUDE_VOLUME_SRC") ||
                  hp_source_list(c, &check.sources, "ETUDE_WIN_FILE_SRC");
        else
            hp_split(&check.sources, "tools/etude/win/image_command.c tools/etude/win/hold.c "
                                     "tools/etude/win/winfmt.c");
        if (i != 4)
            hp_split(&check.libraries, "-lws2_32 -lbcrypt");
        char output[4096];
        if (snprintf(output, sizeof(output), "%s/%s", root, names[i]) >= (int)sizeof(output))
            bad = 1;
        if (!bad)
            bad = prepared_program(c, &check, output, names[i], 1, inputs);
        json_free(check.entry);
        hp_words_free(&check.sources);
        hp_words_free(&check.flags);
        hp_words_free(&check.libraries);
    }
    if (!bad && !c->print) {
        char destination[4096];
        const char *source = "tools/etude/schemas/history.schema.json";
        if (snprintf(destination, sizeof(destination), "%s/history.schema.json", root) >=
            (int)sizeof(destination))
            bad = 1;
        else
            bad = copy_file(source, destination) || hp_input(c, source, inputs, 0) ||
                  hp_input(c, destination, inputs, 0);
    }
    return bad ? -1 : 0;
}

static int external_exfat_tools(struct hp_context *c, struct json *inputs)
{
    struct hp_words paths = {0};
    hp_split(&paths, "build/tests/host/exfat_writer tests/host/mkexfat_external.sh");
    int bad = hp_files(&paths, "build/tests/host-suite/tools/exfat_external");
    for (size_t i = 0; !bad && i < paths.n; i++)
        bad = hp_input(c, paths.v[i], inputs, 0);
    hp_words_free(&paths);
    return bad ? -1 : 0;
}

static int native_tool_name(const char *name)
{
    static const char *const names[] = {"git",     "tar",      "mdir",   "mcopy",  "mke2fs",
                                        "debugfs", "dumpe2fs", "e2fsck", "python3"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (same(name, names[i]))
            return 1;
    return 0;
}

static int native_tool(struct hp_context *c, const char *name, struct json *inputs)
{
    char *source = external_tool(name);
    int bad = 0;
    if (!source) {
        const char *prepared = getenv("HOST_TEST_PREPARED_TOOLS");
        fprintf(stderr, "host preparation: %s: %s\n", name,
                prepared ? "not in HOST_TEST_PREPARED_TOOLS"
                         : "HOST_TEST_PREPARED_TOOLS is not set");
        bad = 1;
    } else if (copied_tool(c, "build/tests/host-suite/tools", name, source, inputs)) {
        fprintf(stderr, "host preparation: %s: cannot copy native tool %s\n", name, source);
        bad = 1;
    }
    free(source);
    return bad ? -1 : 0;
}

int hp_tools(struct hp_context *c, struct hp_check *check)
{
    struct json *inputs = hp_value(JSON_ARRAY);
    struct hp_words common = {0};
    hp_common_flags(&common);
    int bad = 0;
    if (hp_windows_host() && same(json_text(check->entry, "assertion_id"), "etude_platform"))
        bad = companions(c, check, inputs);
    for (size_t i = 0; !bad && i < check->tools.n; i++) {
        const char *name = check->tools.v[i];
        if (same(name, "atos")) {
            if (!c->print)
                bad = sanitizer(c, "build/tests/host-suite/sanitizer", inputs);
        } else if (same(name, "exfat_external")) {
            if (!c->print)
                bad = external_exfat_tools(c, inputs);
        } else if (native_tool_name(name)) {
            if (!c->print)
                bad = native_tool(c, name, inputs);
        } else
            bad = compiled_tool(c, name, &common, inputs);
    }
    if (!bad && !c->print)
        bad = merge_inputs(check->entry, inputs);
    json_free(inputs);
    hp_words_free(&common);
    return bad ? -1 : 0;
}

int hp_tool_main(struct hp_context *c)
{
    if (same(c->only, "external-exfat"))
        return external_exfat(c);
    if (!same(c->only, "sanitizer"))
        return -1;
    struct json *inputs = hp_value(JSON_ARRAY);
    int result = sanitizer(c, c->output, inputs);
    json_free(inputs);
    return result;
}
