/* Make compiles host checks; Etude binds its recipes and inputs. */
#include <ctype.h>
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

static int path_join(char out[4096], const char *a, const char *b)
{
    int n = snprintf(out, 4096, "%s/%s", a, b);
    return n < 0 || n >= 4096 ? -1 : 0;
}

/**
 * Resolve a program path, using native executable lookup on Windows.
 *
 * Return an allocated absolute path, or NULL when the program is unavailable.
 */
char *hp_program(const char *name)
{
    if (strchr(name, '/') || strchr(name, '\\')) {
        char *result = realpath(name, NULL);
#ifdef _WIN32
        char path[4096];
        if (!result && snprintf(path, sizeof(path), "%s.exe", name) < (int)sizeof(path))
            result = realpath(path, NULL);
#endif
        return result;
    }
#ifdef _WIN32
    char path[32768];
    return etude_win_program(name, path, sizeof path) ? NULL : realpath(path, NULL);
#else
    const char *env = getenv("PATH");
    char *paths = strdup(env ? env : ""), *cursor = paths;
    char *result = NULL;
    const char *separator = ":";
    while (cursor && !result) {
        char *next = strpbrk(cursor, separator), path[4096];
        if (next)
            *next++ = 0;
        if (!path_join(path, *cursor ? cursor : ".", name) && access(path, X_OK) == 0)
            result = realpath(path, NULL);
        cursor = next;
    }
    free(paths);
    return result;
#endif
}

#ifdef _WIN32
static int compiler_path(struct hp_context *c)
{
    if (c->compiler.v[0][0] == '/' && c->compiler.v[0][1] != '/') {
        struct jbuf option = {0};
        jbuf_puts(&option, "HOST_CHECK_COMPILER_NAME=");
        jbuf_puts(&option, c->compiler.v[0]);
        char output[4096], error[4096];
        char *argv[] = {getenv("MAKE") ? getenv("MAKE") : "make",
                        "--no-print-directory",
                        "-f",
                        "mk/host-check.mk",
                        option.data,
                        "print-compiler",
                        NULL};
        int bad =
            hp_work_files(c, "compiler-path", output, error) || hp_run(c, argv, output, error);
        char *text = !bad ? hp_read(output) : NULL;
        if (text)
            text[strcspn(text, "\r\n")] = 0;
        char *native = text && *text ? hp_program(text) : NULL;
        if (native) {
            c->compiler_requested = strdup(c->compiler.v[0]);
            free(c->compiler.v[0]);
            c->compiler.v[0] = native;
        }
        jbuf_free(&option);
        free(text);
        return native ? 0 : -1;
    }
    return 0;
}
#endif

int hp_compiler(struct hp_context *c)
{
    char output[4096], error[4096], hash[65];
    struct hp_words argv = {0};
    char *program = NULL, *version = NULL;
#ifdef _WIN32
    if (compiler_path(c))
        goto failed;
#endif
    program = hp_program(c->compiler.v[0]);
    if (!program || hp_hash(program, hash) || path_join(output, c->work, "compiler.out") ||
        path_join(error, c->work, "compiler.err"))
        goto failed;
    for (size_t i = 0; i < c->compiler.n; i++)
        hp_add(&argv, c->compiler.v[i]);
    hp_add(&argv, "--version");
    if (hp_run(c, argv.v, output, error) || !(version = hp_read(output)))
        goto failed;
    size_t n = strlen(version);
    while (n && isspace((unsigned char)version[n - 1]))
        version[--n] = 0;
    if (!*version)
        goto failed;
    c->identity = hp_value(JSON_OBJECT);
    hp_member(c->identity, "command", hp_array(&c->compiler));
    hp_member(c->identity, "executable", hp_string(program));
    hp_member(c->identity, "sha256", hp_string(hash));
    hp_member(c->identity, "version", hp_string(version));
    free(program);
    free(version);
    hp_words_free(&argv);
    return 0;
failed:
    free(program);
    free(version);
    hp_words_free(&argv);
    return -1;
}

/* Normalize separators and dot components, retaining store-link spellings. */
static char *input_name(struct hp_context *c, const char *path, int external)
{
    struct hp_words parts = {0};
    char *copy = strdup(path), *p = copy, *result = NULL;
    struct jbuf out = {0};
    if (!copy)
        return NULL;
#ifdef _WIN32
    for (char *at = copy; *at; at++)
        if (*at == '\\')
            *at = '/';
    if (!strncmp(copy, "//?/", 4))
        p += 4;
#endif
    int absolute = *p == '/' || (strlen(p) > 1 && isalpha((unsigned char)p[0]) && p[1] == ':');
    if (absolute) {
        size_t n = strlen(c->tree);
#ifdef _WIN32
        int inside = strlen(p) > n && !strncasecmp(p, c->tree, n) && p[n] == '/';
#else
        int inside = strlen(p) > n && !strncmp(p, c->tree, n) && p[n] == '/';
#endif
        if (inside)
            p += n + 1;
        else if (external) {
            result = strdup(p);
            goto done;
        } else
            goto done;
    }
    while (*p) {
        char *end = strchr(p, '/');
        if (end)
            *end = 0;
        if (!strcmp(p, "..")) {
            if (!parts.n)
                goto done;
            free(parts.v[--parts.n]);
            parts.v[parts.n] = NULL;
        } else if (end != p && strcmp(p, ".")) {
            if (strchr(p, ':') || strchr(p, '\\'))
                goto done;
            hp_add(&parts, p);
        }
        if (!end)
            break;
        p = end + 1;
    }
    for (size_t i = 0; i < parts.n; i++) {
        if (i)
            jbuf_puts(&out, "/");
        jbuf_puts(&out, parts.v[i]);
    }
    if (out.len) {
        result = out.data;
        memset(&out, 0, sizeof(out));
    }
done:
    jbuf_free(&out);
    hp_words_free(&parts);
    free(copy);
    return result;
}

int hp_input(struct hp_context *c, const char *path, struct json *inputs, int external)
{
    char hash[65], *name = input_name(c, path, external);
    if (external && name) {
        char *resolved = realpath(path, NULL);
        if (resolved) {
            free(name);
            name = resolved;
        }
    }
    if (!name || hp_hash(path, hash)) {
        free(name);
        return -1;
    }
    for (size_t i = 0; i < inputs->count; i++) {
        char decoded[4096];
        const char *old = hp_input_path(inputs->item[i], decoded);
        if (old && !strcmp(old, name)) {
            int result = strcmp(json_text(inputs->item[i], "sha256"), hash) != 0;
            free(name);
            return result ? -1 : 0;
        }
    }
    struct json *value = hp_value(JSON_OBJECT);
    struct jbuf encoded = {0};
    int ascii = 1;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p > 127)
            ascii = 0;
    if (ascii)
        hp_member(value, "path", hp_string(name));
    else {
        static const char digits[] = "0123456789abcdef";
        for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
            const char pair[2] = {digits[*p >> 4], digits[*p & 15]};
            jbuf_put(&encoded, pair, 2);
        }
        hp_member(value, "path_hex", hp_string(encoded.data));
        jbuf_free(&encoded);
    }
    hp_member(value, "sha256", hp_string(hash));
    hp_item(inputs, value);
    free(name);
    return 0;
}

int hp_inputs_valid(struct hp_context *c, const struct json *inputs, const char *origin)
{
    (void)c;
    if (!inputs || inputs->type != JSON_ARRAY)
        return 0;
    for (size_t i = 0; i < inputs->count; i++) {
        char decoded[4096];
        const char *name = hp_input_path(inputs->item[i], decoded);
        const char *wanted = json_text(inputs->item[i], "sha256");
        char path[4096], hash[65];
        if (!name || !wanted ||
            (origin && name[0] != '/' && !(isalpha((unsigned char)name[0]) && name[1] == ':')
                 ? path_join(path, origin, name)
                 : snprintf(path, sizeof(path), "%s", name) >= (int)sizeof(path)) ||
            hp_hash(path, hash) || strcmp(hash, wanted))
            return 0;
    }
    return 1;
}

char *hp_make_words(const struct hp_words *words)
{
    struct jbuf out = {0};
    for (size_t i = 0; i < words->n; i++) {
        if (i)
            jbuf_puts(&out, " ");
        if (*words->v[i] &&
            strspn(words->v[i], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                "0123456789_./:=,+@%-") == strlen(words->v[i])) {
            jbuf_puts(&out, words->v[i]);
            continue;
        }
        jbuf_puts(&out, "'");
        for (const char *p = words->v[i]; *p; p++)
            if (*p == '\'')
                jbuf_puts(&out, "'\\''");
            else if (*p == '$')
                jbuf_puts(&out, "$$");
            else
                jbuf_put(&out, p, 1);
        jbuf_puts(&out, "'");
    }
    return out.data ? out.data : strdup("");
}

static void assignment(struct hp_words *argv, const char *name, const char *value)
{
    struct jbuf word = {0};
    jbuf_puts(&word, name);
    jbuf_puts(&word, "=");
    jbuf_puts(&word, value);
    hp_add(argv, word.data);
    jbuf_free(&word);
}

static int actual_commands(struct hp_context *c, struct hp_check *check, char *text)
{
    struct json *commands = hp_value(JSON_ARRAY);
    for (char *line = text; line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        struct hp_words words = {0};
        int matches = !hp_split(&words, line) && words.n >= c->compiler.n;
        for (size_t i = 0; matches && i < c->compiler.n; i++)
            matches = !strcmp(words.v[i], c->compiler.v[i]);
        if (matches)
            hp_item(commands, hp_array(&words));
        hp_words_free(&words);
        line = next;
    }
    if (!commands->count) {
        json_free(commands);
        return -1;
    }
    hp_member(check->entry, "command", commands->item[--commands->count]);
    if (commands->count)
        hp_member(check->entry, "compile_commands", commands);
    else
        json_free(commands);
    return 0;
}

static int build_make(struct hp_context *c, struct hp_check *check, const char *program,
                      const char *goal, int force, int system)
{
    struct hp_words argv = {0};
    char output[4096], error[4096];
    char *compiler = hp_make_words(&c->compiler), *flags = hp_make_words(&check->flags);
    char *libraries = hp_make_words(&check->libraries);
    struct jbuf sources = {0};
    int result = -1;
    hp_add(&argv, getenv("MAKE") ? getenv("MAKE") : "make");
    hp_add(&argv, "--no-print-directory");
    hp_add(&argv, "-f");
    hp_add(&argv, "mk/host-check.mk");
    if (force)
        assignment(&argv, "HOST_CHECK_FORCE", "1");
    if (c->print)
        hp_add(&argv, "-n");
    assignment(&argv, "HOSTCC", compiler);
    assignment(&argv, "HOST_CHECK_OUTPUT", program);
    char scans[4096];
    if (snprintf(scans, sizeof(scans), "%s-sources/scan.%ld", program, (long)getpid()) >=
        (int)sizeof(scans))
        goto finished;
    assignment(&argv, "HOST_CHECK_SCAN_WORK", scans);
    assignment(&argv, "HOST_CHECK_FLAGS", flags);
    assignment(&argv, "HOST_CHECK_LIBRARIES", libraries);
    if (check->objects)
        assignment(&argv, "HOST_CHECK_OBJECT_MODE", "1");
    if (!strcmp(goal, "all") && !c->print) {
        char temporary[4096];
        if (snprintf(temporary, sizeof(temporary), "%s-sources/linked.%ld", program,
                     (long)getpid()) >= (int)sizeof(temporary))
            goto finished;
        assignment(&argv, "HOST_CHECK_LINK_OUTPUT", temporary);
    }
    assignment(&argv, "HOST_CHECK_SCAN_FLAG", system ? "-M" : "-MM");
    for (size_t i = 0; i < check->sources.n; i++) {
        const char *source = check->sources.v[i];
        size_t n = strlen(source);
        if (n > 2 && !strcmp(source + n - 2, ".c")) {
            if (sources.len)
                jbuf_puts(&sources, " ");
            jbuf_puts(&sources, source);
        }
    }
    assignment(&argv, "HOST_CHECK_SOURCES", sources.data ? sources.data : "");
    hp_add(&argv, goal);
    if (!path_join(output, c->work, "make.out") && !path_join(error, c->work, "make.err"))
        result = hp_run(c, argv.v, output, error);
    char *out = hp_read(output), *err = hp_read(error);
    if (out)
        fputs(out, stdout);
    if (err)
        fputs(err, stderr);
    if (result)
        fprintf(stderr, "host preparation: Make failed: %s\n", program);
    if (!result && !c->print && !strcmp(goal, "all")) {
        result = !out || actual_commands(c, check, out);
        if (!result)
            hp_member(check->entry, "make_command", hp_array(&argv));
    }
    free(out);
    free(err);
finished:
    hp_words_free(&argv);
    free(compiler);
    free(flags);
    free(libraries);
    jbuf_free(&sources);
    return result;
}

static int scan_inputs(struct hp_context *c, struct hp_check *check, const char *program,
                       struct json *local, struct json *external, int system)
{
    if (build_make(c, check, program, "scan", 0, system))
        return -1;
    struct jbuf make_dependencies = {0};
    int result = 0;
    for (size_t i = 0; i < check->sources.n; i++) {
        char path[4096];
        const char *source = check->sources.v[i];
        size_t n = strlen(source);
        if (hp_input(c, source, local, 0))
            goto failed;
        if (n < 2 || strcmp(source + n - 2, ".c"))
            continue;
        int count = snprintf(path, sizeof(path), "%s-sources/scan.%ld/%s.scan", program,
                             (long)getpid(), source);
        char *text = count > 0 && count < (int)sizeof(path) ? hp_read(path) : NULL;
        struct hp_words words = {0};
        if (!text)
            goto failed;
        /* A Windows compiler's text-mode output ends each line with CR LF. */
        char *kept = text;
        for (char *p = text; *p; p++)
            if (p[0] != '\r' || p[1] != '\n')
                *kept++ = *p;
        *kept = 0;
        for (char *p = text; *p; p++)
            if (p[0] == '\\' && p[1] == '\n')
                p[0] = p[1] = ' ';
        char *colon = strchr(text, ':');
        int bad = !colon || hp_split(&words, colon + 1);
        if (colon) {
            jbuf_puts(&make_dependencies, "$(HOST_CHECK_OUTPUT):");
            jbuf_puts(&make_dependencies, colon + 1);
            jbuf_puts(&make_dependencies, "\n");
        }
        for (size_t j = 0; !bad && j < words.n; j++) {
            char *relative = input_name(c, words.v[j], 0);
            bad = hp_input(c, words.v[j], relative ? local : external, relative == NULL);
            free(relative);
        }
        free(text);
        hp_words_free(&words);
        if (bad)
            goto failed;
    }
    char path[4096], temporary[4096];
    if (snprintf(path, sizeof(path), "%s-sources/dependencies.mk", program) >= (int)sizeof(path) ||
        snprintf(temporary, sizeof(temporary), "%s.%ld", path, (long)getpid()) >=
            (int)sizeof(temporary) ||
        hp_write(temporary, make_dependencies.data, make_dependencies.len) ||
        rename(temporary, path))
        goto failed;
    goto done;
failed:
    result = -1;
done:
    jbuf_free(&make_dependencies);
    return result;
}

int hp_preprocess(struct hp_context *c, struct hp_check *check, const char *program)
{
    return build_make(c, check, program, "preprocess", 0, 0);
}

/* JSON input arrays have a stable pathname order, independent of TU order. */
static int input_order(const void *a, const void *b)
{
    const struct json *const *x = a, *const *y = b;
    char a_path[4096], b_path[4096];
    return strcmp(hp_input_path(*x, a_path), hp_input_path(*y, b_path));
}

void hp_sort_inputs(struct json *inputs)
{
    if (inputs->count > 1)
        qsort(inputs->item, inputs->count, sizeof(*inputs->item), input_order);
}

static int absolute_path(const char *path)
{
    return path &&
           (*path == '/' || (strlen(path) > 1 && isalpha((unsigned char)*path) && path[1] == ':'));
}

/* Only the compiler and its recognized path operands have store aliases. */
static struct json *compiler_words(struct hp_context *c, const struct json *command)
{
    struct json *copy = hp_clone(command);
    if (copy->type != JSON_ARRAY)
        return copy;
    for (size_t i = 0; i < copy->count; i++) {
        struct json *word = copy->item[i];
        if (word->type != JSON_STRING)
            continue;
        const char *text = word->string, *path = text;
        size_t prefix = 0;
        if (!strncmp(text, "--sysroot=", 10))
            prefix = 10;
        else if (!strncmp(text, "-B", 2) || !strncmp(text, "-L", 2))
            prefix = 2;
        path += prefix;
        const char *previous =
            i && copy->item[i - 1]->type == JSON_STRING ? copy->item[i - 1]->string : "";
        int operand = !strcmp(previous, "-isystem") || !strcmp(previous, "-iquote") ||
                      !strcmp(previous, "-I") || !strcmp(previous, "-L") ||
                      !strcmp(previous, "-B") || !strcmp(previous, "--sysroot");
        if ((i == 0 || prefix || operand) && absolute_path(path)) {
            char *resolved =
                i == 0 && !prefix && c->compiler_requested && !strcmp(path, c->compiler_requested)
                    ? strdup(c->compiler.v[0])
                    : realpath(path, NULL);
            if (resolved) {
                struct jbuf value = {0};
                jbuf_put(&value, text, prefix);
                jbuf_puts(&value, resolved);
                free(word->string);
                word->string = value.data;
                free(resolved);
            }
        }
    }
    return copy;
}

int hp_identity_equal(struct hp_context *c, const struct json *a, const struct json *b)
{
    if (!a || !b)
        return 0;
    struct json *old = hp_clone(a), *current = hp_clone(b);
    hp_member(old, "command", compiler_words(c, json_member(a, "command")));
    hp_member(current, "command", compiler_words(c, json_member(b, "command")));
    const char *executable = json_text(old, "executable");
    if (c->compiler_requested && executable && !strcmp(executable, c->compiler_requested))
        hp_member(old, "executable", hp_string(json_text(current, "executable")));
    int equal = json_equal(old, current);
    json_free(old);
    json_free(current);
    return equal;
}

static int sdk_equal(const struct json *a, const struct json *b)
{
    if (!a || !b)
        return a == b;
    struct json *old = hp_clone(a), *current = hp_clone(b);
    struct json *both[] = {old, current};
    int valid = old->type == JSON_ARRAY && current->type == JSON_ARRAY;
    for (size_t n = 0; valid && n < 2; n++) {
        for (size_t i = 0; i < both[n]->count; i++) {
            const char *path = json_text(both[n]->item[i], "path");
            if (!path) {
                valid = 0;
                break;
            }
            char *resolved = realpath(path, NULL);
            if (resolved) {
                hp_member(both[n]->item[i], "path", hp_string(resolved));
                free(resolved);
            }
        }
        if (valid)
            hp_sort_inputs(both[n]);
    }
    int equal = valid && json_equal(old, current);
    json_free(old);
    json_free(current);
    return equal;
}

static const struct json *previous_check(struct hp_context *c, const char *id)
{
    const struct json *checks = json_member(c->old, "checks");
    const struct json *identity = json_member(c->old, "compiler");
    if (!checks || !identity || !hp_identity_equal(c, identity, c->identity))
        return NULL;
    for (size_t i = 0; i < checks->count; i++)
        if (json_text(checks->item[i], "assertion_id") &&
            !strcmp(json_text(checks->item[i], "assertion_id"), id))
            return checks->item[i];
    return NULL;
}

/* A changed old compiler form is rebuilt once, not relabeled. */
static int legacy_recipe(struct hp_context *c, struct hp_check *check, const struct json *old,
                         const char *program)
{
    const struct json *recorded = json_member(old, "command");
    if (!recorded || recorded->type != JSON_ARRAY || json_member(old, "compile_commands"))
        return 0;
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
    struct json *expected = hp_array(&command);
    struct json *a = compiler_words(c, recorded), *b = compiler_words(c, expected);
    int match = json_equal(a, b);
    json_free(a);
    json_free(b);
    json_free(expected);
    hp_words_free(&command);
    return match;
}

static int old_has_inputs(const struct json *old, const struct json *current)
{
    if (!old || old->type != JSON_ARRAY)
        return 0;
    for (size_t i = 0; i < current->count; i++) {
        int found = 0;
        for (size_t j = 0; j < old->count; j++)
            if (json_equal(old->item[j], current->item[i]))
                found = 1;
        if (!found)
            return 0;
    }
    return 1;
}

/* The recipe has the local and compiler inputs that the scan found. */
static int old_matches(struct hp_context *c, struct hp_check *check, const char *program,
                       const struct json *old, const struct json *recipe)
{
    const struct json *local = json_member(recipe, "inputs");
    const struct json *external = json_member(recipe, "compiler_inputs");
    const struct json *made = json_member(old, "make_recipe");
    const struct json *recorded = json_member(old, "compile_inputs");
    const struct json *sdk = json_member(old, "compiler_inputs");
    if (made) {
        struct json *canonical = hp_clone(made);
        hp_member(canonical, "compiler_inputs", hp_clone(external));
        int equal = sdk_equal(json_member(made, "compiler_inputs"), external) &&
                    json_equal(canonical, recipe);
        json_free(canonical);
        return equal;
    }
    if (!legacy_recipe(c, check, old, program) || !sdk || !sdk_equal(sdk, external))
        return 0;
    if (recorded)
        return json_equal(recorded, local);
    return old_has_inputs(json_member(old, "inputs"), local) &&
           hp_inputs_valid(c, json_member(old, "inputs"), c->origin) &&
           hp_inputs_valid(c, sdk, NULL);
}

static int copy_program(const char *from, const char *to)
{
    unsigned char bytes[65536];
    struct stat st;
    FILE *in = fopen(from, "rb"), *out = NULL;
    size_t n;
    int bad = !in || stat(from, &st);
    if (!bad)
        out = fopen(to, "wb");
    if (!out)
        bad = 1;
    while (!bad && (n = fread(bytes, 1, sizeof(bytes), in)))
        bad = fwrite(bytes, 1, n, out) != n;
    if (in) {
        bad = bad || ferror(in);
        fclose(in);
    }
    if (out && fclose(out))
        bad = 1;
    return bad || chmod(to, (st.st_mode & 0777) | 0200) ? -1 : 0;
}

static int publish_compilation(struct hp_context *c, struct hp_check *check, const char *program,
                               const struct json *local, const struct json *external, int system)
{
    struct json *after = hp_value(JSON_ARRAY), *after_external = hp_value(JSON_ARRAY);
    int bad = scan_inputs(c, check, program, after, after_external, system);
    hp_sort_inputs(after);
    hp_sort_inputs(after_external);
    bad = bad || !json_equal(local, after) || !json_equal(external, after_external);
    json_free(after);
    json_free(after_external);
    char temporary[4096];
    int n =
        snprintf(temporary, sizeof(temporary), "%s-sources/linked.%ld", program, (long)getpid());
    if (bad || n < 0 || n >= (int)sizeof(temporary) || rename(temporary, program))
        return -1;
    return 0;
}

int hp_build(struct hp_context *c, struct hp_check *check, const char *program, int system)
{
    if (c->print)
        return build_make(c, check, program, "all", 1, system);
    struct json *local = hp_value(JSON_ARRAY), *external = hp_value(JSON_ARRAY);
    struct json *recipe = hp_value(JSON_OBJECT);
    const char *id = json_text(check->entry, "assertion_id");
    const struct json *old = previous_check(c, id);
    char hash[65], old_program[4096];
    int result = -1;
    if (scan_inputs(c, check, program, local, external, system))
        goto done;
    hp_sort_inputs(local);
    hp_sort_inputs(external);
    hp_member(recipe, "sources", hp_array(&check->sources));
    hp_member(recipe, "flags", hp_array(&check->flags));
    hp_member(recipe, "libraries", hp_array(&check->libraries));
    hp_member(recipe, "objects", hp_number(check->objects));
    hp_member(recipe, "inputs", hp_clone(local));
    hp_member(recipe, "compiler_inputs", hp_clone(external));
    char make_hash[65];
    if (hp_hash("mk/host-check.mk", make_hash))
        goto done;
    hp_member(recipe, "makefile_sha256", hp_string(make_hash));
    int path_ok = c->origin ? !path_join(old_program, c->origin, program)
                            : snprintf(old_program, sizeof(old_program), "%s", program) <
                                  (int)sizeof(old_program);
    int reuse = old && path_ok && old_matches(c, check, program, old, recipe) &&
                !hp_hash(old_program, hash) && json_text(old, "sha256") &&
                !strcmp(hash, json_text(old, "sha256"));
    if (reuse && c->origin &&
        (copy_program(old_program, program) || hp_hash(program, hash) ||
         strcmp(hash, json_text(old, "sha256"))))
        goto done;
    if (!reuse && build_make(c, check, program, "all", 1, system))
        goto done;
    check->reused = reuse;
    if (!reuse) {
        if (publish_compilation(c, check, program, local, external, system) ||
            hp_hash(program, hash))
            goto done;
    }
    if (reuse) {
        hp_member(check->entry, "command", hp_clone(json_member(old, "command")));
        if (json_member(old, "make_command"))
            hp_member(check->entry, "make_command", hp_clone(json_member(old, "make_command")));
        if (json_member(old, "compile_commands"))
            hp_member(check->entry, "compile_commands",
                      hp_clone(json_member(old, "compile_commands")));
    }
    hp_member(check->entry, "compile_inputs", hp_clone(local));
    for (size_t i = 0; i < check->data.n; i++)
        if (hp_input(c, check->data.v[i], local, 0))
            goto done;
    hp_sort_inputs(local);
    hp_member(check->entry, "sha256", hp_string(hash));
    hp_member(check->entry, "inputs", local);
    local = NULL;
    hp_member(check->entry, "compiler_inputs", external);
    external = NULL;
    hp_member(check->entry, "make_recipe", recipe);
    recipe = NULL;
    result = 0;
done:
    json_free(local);
    json_free(external);
    json_free(recipe);
    return result;
}
