/* Native file and process operations used by host inventory preparation. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#else
extern char **environ;
#endif
#include "etude/exec.h"
#include "host_prepare.h"
#include "sha256.h"

void hp_add(struct hp_words *words, const char *word)
{
    char **v = realloc(words->v, (words->n + 2) * sizeof(*v));
    if (v == NULL)
        exit(1);
    words->v = v;
    words->v[words->n++] = strdup(word);
    words->v[words->n] = NULL;
    if (words->v[words->n - 1] == NULL)
        exit(1);
}

/* Metadata and CC use shell words, without expansions or command execution. */
int hp_split(struct hp_words *words, const char *text)
{
    struct jbuf word = {0};
    int quote = 0, active = 0;
    for (; *text; text++) {
        char b = *text;
        if (b == '\\' && quote != '\'') {
            if (!text[1])
                goto failed;
            text++;
            jbuf_put(&word, text, 1);
            active = 1;
        } else if (b == quote) {
            quote = 0;
        } else if (!quote && (b == '\'' || b == '"')) {
            quote = b;
            active = 1;
        } else if (!quote && (b == ' ' || b == '\t' || b == '\n' || b == '\r')) {
            if (active) {
                hp_add(words, word.data ? word.data : "");
                word.len = 0;
                if (word.data)
                    word.data[0] = 0;
                active = 0;
            }
        } else {
            jbuf_put(&word, &b, 1);
            active = 1;
        }
    }
    if (quote)
        goto failed;
    if (active)
        hp_add(words, word.data ? word.data : "");
    jbuf_free(&word);
    return 0;
failed:
    jbuf_free(&word);
    return -1;
}

void hp_words_free(struct hp_words *words)
{
    for (size_t i = 0; i < words->n; i++)
        free(words->v[i]);
    free(words->v);
    memset(words, 0, sizeof(*words));
}

int hp_hash(const char *path, char hex[65])
{
    struct sha256 hash;
    unsigned char bytes[65536], result[32];
    size_t n;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return -1;
    sha256_init(&hash);
    while ((n = fread(bytes, 1, sizeof(bytes), file)))
        sha256_update(&hash, bytes, n);
    int bad = ferror(file);
    fclose(file);
    if (bad)
        return -1;
    sha256_final(&hash, result);
    sha256_hex(result, hex);
    return 0;
}

char *hp_read(const char *path)
{
    struct jbuf text = {0};
    char bytes[65536];
    size_t n;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return NULL;
    while ((n = fread(bytes, 1, sizeof(bytes), file)))
        jbuf_put(&text, bytes, n);
    int bad = ferror(file);
    fclose(file);
    if (bad) {
        jbuf_free(&text);
        return NULL;
    }
    return text.data ? text.data : strdup("");
}

/**
 * Format a path into `path`, a buffer of `size` bytes, as snprintf does.
 *
 * Return 0, or -1 when the path does not fit; the truncated text left in
 * `path` then names some other file and must not be used.
 */
int hp_path(char *path, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(path, size, format, ap);
    va_end(ap);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

/**
 * Name the files in the work directory that capture a command's output and
 * errors, `NAME.out` and `NAME.err`.
 *
 * Return 0, or -1 when either path does not fit in its 4096 bytes.
 */
int hp_work_files(const struct hp_context *c, const char *name, char output[4096], char error[4096])
{
    if (hp_path(output, 4096, "%s/%s.out", c->work, name) ||
        hp_path(error, 4096, "%s/%s.err", c->work, name))
        return -1;
    return 0;
}

int hp_mkdir(const char *path)
{
    char *copy = strdup(path);
    struct stat st;
    int result = 0;
    if (copy == NULL)
        return -1;
    for (char *p = copy + 1;; p++) {
        if (*p != '/' && *p != 0)
            continue;
        char b = *p;
        *p = 0;
        if (!(strlen(copy) == 2 && copy[1] == ':') &&
            ((stat(copy, &st) != 0 && mkdir(copy, 0755) != 0 && errno != EEXIST) ||
             stat(copy, &st) != 0 || !S_ISDIR(st.st_mode))) {
            result = -1;
            break;
        }
        *p = b;
        if (!b)
            break;
    }
    free(copy);
    return result;
}

int hp_write(const char *path, const void *bytes, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return -1;
    int bad = fwrite(bytes, 1, size, file) != size;
    if (fclose(file))
        bad = 1;
    return bad ? -1 : 0;
}

struct json *hp_document(const char *path)
{
    char *text = hp_read(path), why[256];
    struct json *value = NULL;
    if (text && json_parse(text, strlen(text), 1, &value, why, sizeof(why)))
        fprintf(stderr, "host preparation: %s: %s\n", path, why);
    free(text);
    return value;
}

struct json *hp_value(enum json_type type)
{
    struct json *value = json_alloc(sizeof(*value));
    memset(value, 0, sizeof(*value));
    value->type = type;
    return value;
}

struct json *hp_string(const char *text)
{
    struct json *value = hp_value(text ? JSON_STRING : JSON_NULL);
    if (text)
        value->string = strdup(text);
    return value;
}

struct json *hp_number(long long number)
{
    struct json *value = hp_value(JSON_INTEGER);
    value->integer = number;
    return value;
}

void hp_item(struct json *array, struct json *value)
{
    struct json **items = realloc(array->item, (array->count + 1) * sizeof(*items));
    if (!items)
        exit(1);
    array->item = items;
    array->item[array->count++] = value;
}

void hp_member(struct json *object, const char *name, struct json *value)
{
    for (size_t i = 0; i < object->count; i++) {
        if (!strcmp(object->item[i]->name, name)) {
            json_free(object->item[i]);
            value->name = strdup(name);
            object->item[i] = value;
            return;
        }
    }
    value->name = strdup(name);
    hp_item(object, value);
}

struct json *hp_clone(const struct json *value)
{
    if (!value)
        return hp_value(JSON_NULL);
    struct json *copy = hp_value(value->type);
    copy->integer = value->integer;
    copy->line = value->line;
    if (value->string)
        copy->string = strdup(value->string);
    for (size_t i = 0; i < value->count; i++) {
        struct json *child = hp_clone(value->item[i]);
        if (value->item[i]->name)
            child->name = strdup(value->item[i]->name);
        hp_item(copy, child);
    }
    return copy;
}

struct json *hp_array(const struct hp_words *words)
{
    struct json *array = hp_value(JSON_ARRAY);
    for (size_t i = 0; i < words->n; i++)
        hp_item(array, hp_string(words->v[i]));
    return array;
}

int hp_publish(const char *path, const struct json *value)
{
    char temporary[4096];
    struct jbuf bytes = {0};
    int n = snprintf(temporary, sizeof(temporary), "%s.%ld.tmp", path, (long)getpid());
    if (n < 0 || n >= (int)sizeof(temporary))
        return -1;
    json_canonical(value, &bytes);
    jbuf_puts(&bytes, "\n");
    int result = hp_write(temporary, bytes.data, bytes.len);
    if (!result)
        result = rename(temporary, path);
    if (result)
        unlink(temporary);
    jbuf_free(&bytes);
    return result;
}

const char *hp_input_path(const struct json *value, char path[4096])
{
    const char *name = json_text(value, "path");
    const char *hex = json_text(value, "path_hex");
    if (!hex)
        return name;
    size_t n = strlen(hex);
    if (name || n % 2 || n / 2 >= 4096)
        return NULL;
    for (size_t i = 0; i < n; i += 2) {
        const char *digits = "0123456789abcdef";
        const char *a = strchr(digits, hex[i]), *b = strchr(digits, hex[i + 1]);
        if (!a || !b || (a == digits && b == digits))
            return NULL;
        path[i / 2] = (char)(((a - digits) << 4) | (b - digits));
    }
    path[n / 2] = 0;
    return path;
}

int hp_run(struct hp_context *c, char *const argv[], const char *out, const char *err)
{
    struct etude_exec *executor = etude_exec_new();
    struct etude_exec_spec spec;
    struct etude_proc *process = NULL;
    int result = -1, cancelled = 0;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = "host-preparation";
    struct hp_words command = {0};
    char *program = hp_program(argv[0]);
    if (!program) {
        fprintf(stderr, "host preparation: unavailable program: %s\n", argv[0]);
        if (executor)
            etude_exec_free(executor);
        return -1;
    }
    hp_add(&command, program);
    free(program);
    for (size_t i = 1; argv[i]; i++)
        hp_add(&command, argv[i]);
    spec.argv = command.v;
    spec.envp = c->environment ? c->environment : environ;
    spec.cwd = c->tree;
    spec.stdout_path = out;
    spec.stderr_path = err;
    spec.grace_us = 5000000;
    spec.output_max = 1u << 20;
    if (executor && etude_exec_start(executor, &spec, &process) == 0) {
        /* interval: observe preparation stop requests every 100 ms */
        while (etude_exec_wait(executor, 100) != process) {
            if (c->stop && *c->stop && !cancelled) {
                etude_exec_cancel(process);
                cancelled = 1;
            }
        }
        const struct etude_exec_result *r = etude_proc_result(process);
        if (r && r->cleanup == ETUDE_CLEANUP_CONFIRMED && r->exit_end == ETUDE_END_EXITED)
            result = r->exit_value;
        etude_proc_free(process);
    }
    if (executor)
        etude_exec_free(executor);
    hp_words_free(&command);
    return result;
}
