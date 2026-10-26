/* Native and remote preparation operands under the caller's ownership. */
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
#include "preparation_import.h"
#include "posix/store.h"
#include "win/winfmt.h"

static int absolute(const char *path)
{
#ifdef _WIN32
    return path && etude_win_absolute(path);
#else
    return path && *path == '/';
#endif
}

static int simple(const char *value, const char *allowed)
{
    return value && isalnum((unsigned char)*value) && strspn(value, allowed) == strlen(value);
}

static int remote_prefix(const struct json *producer, struct hp_words *words)
{
    if (pi_same(json_text(producer, "transport"), "ssh"))
        return 0;
    if (!pi_same(json_text(producer, "transport"), "wsl-ssh"))
        return -1;
    const char *distribution = json_text(producer, "distribution"),
               *user = json_text(producer, "user");
    const char *alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-";
    if (!simple(distribution, alphabet) || !simple(user, alphabet))
        return -1;
    hp_split(words, "env MSYS_NO_PATHCONV=1 wsl.exe --distribution");
    hp_add(words, distribution);
    hp_add(words, "--user");
    hp_add(words, user);
    hp_add(words, "--exec");
    return 0;
}

/* SSH takes one command string. Quote fixed vector operands literally. */
static char *remote_words(const struct hp_words *words)
{
    struct jbuf command = {0};
    for (size_t i = 0; i < words->n; i++) {
        if (i)
            jbuf_puts(&command, " ");
        jbuf_puts(&command, "'");
        for (const char *p = words->v[i]; *p; p++)
            if (*p == '\'')
                jbuf_puts(&command, "'\\''");
            else
                jbuf_put(&command, p, 1);
        jbuf_puts(&command, "'");
    }
    return command.data ? command.data : strdup("");
}

static int remote_command(const struct json *producer, const struct hp_words *operands,
                          struct hp_words *argv)
{
    struct hp_words words = {0};
    int bad = remote_prefix(producer, &words);
    for (size_t i = 0; !bad && i < operands->n; i++)
        hp_add(&words, operands->v[i]);
    char *command = !bad ? remote_words(&words) : NULL;
    if (command) {
        hp_split(argv, "ssh -o BatchMode=yes");
        hp_add(argv, json_text(producer, "host"));
        hp_add(argv, command);
    }
    hp_words_free(&words);
    free(command);
    return bad;
}

int pi_producer(const struct json *producer)
{
    const char *mode = json_text(producer, "transport");
    int local = pi_same(mode, "local"), wsl = pi_same(mode, "wsl-ssh");
    if (!producer || producer->type != JSON_OBJECT || (!local && !wsl && !pi_same(mode, "ssh")) ||
        !pi_hex(json_text(producer, "sha256"), 64))
        return -1;
    const char *required[] = {"tree", "metadata"};
    for (size_t i = 0; i < 2; i++) {
        const char *path = json_text(producer, required[i]);
        if (!path || strpbrk(path, "\n\r") || (local ? !absolute(path) : *path != '/'))
            return -1;
    }
    const char *fields = local ? "transport tree metadata sha256"
                         : wsl ? "transport tree metadata sha256 host etude distribution user"
                               : "transport tree metadata sha256 host etude";
    struct hp_words allowed = {0};
    hp_split(&allowed, fields);
    int bad = producer->count != allowed.n;
    for (size_t i = 0; !bad && i < allowed.n; i++)
        bad = !json_member(producer, allowed.v[i]);
    hp_words_free(&allowed);
    if (local || bad)
        return bad ? -1 : 0;
#ifdef _WIN32
    return -1;
#else
    const char *host = json_text(producer, "host"), *program = json_text(producer, "etude");
    const char *alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.@-";
    if (!host || *host == '-' || strspn(host, alphabet) != strlen(host) || !*host || !program ||
        *program != '/' || strpbrk(program, "\n\r"))
        return -1;
    struct hp_words words = {0};
    bad = remote_prefix(producer, &words);
    hp_words_free(&words);
    return bad;
#endif
}

static char *idle_local(struct pi_context *c, const struct json *producer)
{
    char *root = realpath(json_text(producer, "tree"), NULL);
    char *lock = root ? pi_path(root, "build/.run.lock") : NULL;
    struct stat source, destination, st;
    int bad = !root || stat(root, &source) || stat(c->tree, &destination) ||
              (source.st_dev == destination.st_dev && source.st_ino == destination.st_ino) ||
              !lstat(lock, &st) || errno != ENOENT;
    free(lock);
    if (bad) {
        fprintf(stderr,
                "etude prepare import: local producer is active, uncertain or the destination\n");
        free(root);
        return NULL;
    }
    return root;
}

char *pi_metadata(struct pi_context *c, const struct json *producer)
{
    if (pi_same(json_text(producer, "transport"), "local")) {
        char *root = idle_local(c, producer);
        if (!root)
            return NULL;
        free(root);
        return realpath(json_text(producer, "metadata"), NULL);
    }
    char *output = pi_path(c->work, "producer.json"), *error = pi_path(c->work, "producer.err");
    struct stat st;
    struct hp_words operands = {0}, command = {0};
    hp_add(&operands, json_text(producer, "etude"));
    hp_split(&operands, "lock tree");
    hp_add(&operands, json_text(producer, "tree"));
    hp_split(&operands, "prepare-import /bin/cat");
    hp_add(&operands, json_text(producer, "metadata"));
    int bad = !lstat(output, &st) || errno != ENOENT ||
              remote_command(producer, &operands, &command) ||
              hp_run(&c->host, command.v, output, error);
    free(error);
    hp_words_free(&operands);
    hp_words_free(&command);
    if (bad) {
        free(output);
        return NULL;
    }
    return output;
}

static int local_file(struct pi_context *c, const char *root, const struct json *file)
{
    const char *name = file->name;
    char *source = pi_path(root, name), *destination = pi_path(c->tree, name);
    struct json *current = pi_describe(destination);
    if (current && json_equal(current, file)) {
        int bad = pi_verify(source, file);
        json_free(current);
        free(source);
        free(destination);
        return bad;
    }
    json_free(current);
    struct stat source_status = {0};
    char *temporary = calloc(strlen(destination) + 40, 1), *parent = strdup(destination);
    int bad = !temporary || !parent;
    if (!bad) {
        sprintf(temporary, "%s.import.%ld", destination, (long)getpid());
        char *slash = strrchr(parent, '/');
        if (slash)
            *slash = 0;
        bad = pi_verify(source, file) || stat(source, &source_status) || hp_mkdir(parent);
    }
    const struct json *size = json_member(file, "size_bytes");
    if (!bad)
        bad = !size || size->type != JSON_INTEGER || size->integer < 0 ||
              store_copy_range_stop(source, 0, (uint64_t)size->integer, temporary, c->host.stop) ||
              chmod(temporary, source_status.st_mode & 0777) || pi_verify(temporary, file) ||
              pi_verify(source, file) || rename(temporary, destination);
    if (bad && temporary && *temporary)
        unlink(temporary);
    free(temporary);
    free(parent);
    free(source);
    free(destination);
    return bad ? -1 : 0;
}

static int remote_transfer(struct pi_context *c, const struct json *producer,
                           const struct json *imported)
{
    struct jbuf list = {0}, operand = {0}, route = {0};
    struct hp_words locker = {0}, command = {0};
    char *listing = pi_path(c->work, "import-files.list");
    char *output = pi_path(c->work, "transfer.out"), *error = pi_path(c->work, "transfer.err");
    for (size_t i = 0; i < imported->count; i++) {
        jbuf_puts(&list, imported->item[i]->name);
        jbuf_puts(&list, "\n");
    }
    int bad = hp_write(listing, list.data, list.len) || remote_prefix(producer, &locker);
    hp_add(&locker, json_text(producer, "etude"));
    hp_split(&locker, "lock tree");
    hp_add(&locker, json_text(producer, "tree"));
    hp_split(&locker, "prepare-import /usr/bin/rsync");
    char *remote = remote_words(&locker);
    hp_split(&command, "rsync -a --sparse --protect-args --compress");
    jbuf_puts(&route, "--files-from=");
    jbuf_puts(&route, listing);
    hp_add(&command, route.data);
    route.len = 0;
    route.data[0] = 0;
    jbuf_puts(&route, "--rsync-path=");
    jbuf_puts(&route, remote);
    hp_add(&command, route.data);
    hp_split(&command, "-e 'ssh -o BatchMode=yes' --");
    jbuf_puts(&operand, json_text(producer, "host"));
    jbuf_puts(&operand, ":");
    jbuf_puts(&operand, json_text(producer, "tree"));
    if (operand.data[operand.len - 1] != '/')
        jbuf_puts(&operand, "/");
    hp_add(&command, operand.data);
    char *destination = pi_path(c->tree, "");
    hp_add(&command, destination);
    bad = bad || hp_run(&c->host, command.v, output, error);
    free(destination);
    free(remote);
    free(output);
    free(error);
    free(listing);
    hp_words_free(&locker);
    hp_words_free(&command);
    jbuf_free(&list);
    jbuf_free(&operand);
    jbuf_free(&route);
    return bad ? -1 : 0;
}

int pi_transfer(struct pi_context *c, const struct json *producer, const struct json *imported)
{
    if (!pi_same(json_text(producer, "transport"), "local"))
        return remote_transfer(c, producer, imported);
    char *root = idle_local(c, producer);
    int bad = !root;
    for (size_t i = 0; !bad && i < imported->count; i++) {
        if (c->host.stop && *c->host.stop)
            bad = 1;
        else
            bad = local_file(c, root, imported->item[i]);
    }
    char *after = !bad ? idle_local(c, producer) : NULL;
    bad = bad || !after || strcmp(root, after);
    free(after);
    free(root);
    return bad ? -1 : 0;
}

int pi_validator(struct pi_context *c, const struct json *producer, const struct json *metadata)
{
    if (pi_same(json_text(producer, "transport"), "local"))
        return 0;
    struct hp_words operands = {0}, command = {0};
    hp_split(&operands, "/usr/bin/sha256sum");
    hp_add(&operands, json_text(producer, "etude"));
    char *output = pi_path(c->work, "validator.out"), *error = pi_path(c->work, "validator.err");
    int bad =
        remote_command(producer, &operands, &command) || hp_run(&c->host, command.v, output, error);
    char *text = !bad ? hp_read(output) : NULL;
    const struct json *validator = json_member(metadata, "validator");
    bad = bad || !text || strlen(text) < 65 || text[64] != ' ' ||
          !pi_same(json_text(producer, "etude"), json_text(validator, "path"));
    if (!bad) {
        text[64] = 0;
        bad = !pi_same(text, json_text(validator, "sha256"));
    }
    free(output);
    free(error);
    free(text);
    hp_words_free(&operands);
    hp_words_free(&command);
    return bad ? -1 : 0;
}

static int argument_space(const struct hp_words *argv)
{
    char *line = NULL;
    int bad =
        etude_win_cmdline(argv->v[0], (const char *const *)argv->v, &line) || strlen(line) >= 28000;
    free(line);
    return bad ? -1 : 0;
}

static int build_batch(struct pi_context *c, struct hp_words *argv, struct json *commands)
{
    char *output = pi_path(c->work, "native-build.out"),
         *error = pi_path(c->work, "native-build.err");
    int bad = hp_run(&c->host, argv->v, output, error);
    char *out = hp_read(output), *err = hp_read(error);
    if (out)
        fputs(out, stdout);
    if (err)
        fputs(err, stderr);
    if (!bad)
        hp_item(commands, hp_array(argv));
    free(out);
    free(err);
    free(output);
    free(error);
    return bad;
}

static int make_kind(struct pi_context *c, const struct json *roles, const struct json *imported,
                     const char *kind, const char *arch, struct json *commands)
{
    struct hp_words base = {0}, batch = {0};
    hp_split(&base, "make --no-print-directory -k");
    struct jbuf option = {0};
    jbuf_puts(&option, "ARCH=");
    jbuf_puts(&option, arch);
    hp_add(&base, option.data);
    hp_add(&base, pi_same(kind, "native") ? "-j2" : "-j1");
#ifdef _WIN32
    hp_split(&base, "HOSTCC=/ucrt64/bin/gcc CC=/ucrt64/bin/gcc ETUDECC=/ucrt64/bin/gcc "
                    "HOST_TEST_PLATFORM=windows OS=Windows_NT");
#endif
    for (size_t i = 0; i < imported->count; i++) {
        option.len = 0;
        option.data[0] = 0;
        jbuf_puts(&option, "-o");
        jbuf_puts(&option, imported->item[i]->name);
        hp_add(&base, option.data);
    }
    int bad = argument_space(&base);
    for (size_t i = 0; i < base.n; i++)
        hp_add(&batch, base.v[i]);
    for (size_t i = 0; !bad && i < roles->count; i++) {
        const struct json *role = roles->item[i];
        const char *goal_arch = !strncmp(role->name, "build/x86_64/", 13) ? "x86_64"
                                : pi_same(c->arches, "x86_64")            ? "x86_64"
                                                                          : "i686";
        if (!pi_same(role->string, kind) || !pi_same(goal_arch, arch))
            continue;
        hp_add(&batch, role->name);
        if (!argument_space(&batch))
            continue;
        free(batch.v[--batch.n]);
        batch.v[batch.n] = NULL;
        bad = batch.n == base.n || build_batch(c, &batch, commands);
        hp_words_free(&batch);
        for (size_t j = 0; j < base.n; j++)
            hp_add(&batch, base.v[j]);
        hp_add(&batch, role->name);
        bad = bad || argument_space(&batch);
    }
    if (!bad && batch.n > base.n)
        bad = build_batch(c, &batch, commands);
    hp_words_free(&base);
    hp_words_free(&batch);
    jbuf_free(&option);
    return bad ? -1 : 0;
}

struct json *pi_build(struct pi_context *c, const struct json *roles, const struct json *imported)
{
    for (size_t i = 0; i < roles->count; i++) {
        const struct json *role = roles->item[i];
        if (!pi_same(role->string, "native") || pi_native_data(role->name))
            continue;
        char *path = pi_path(c->tree, role->name);
        struct stat st;
        int present = !lstat(path, &st);
        int bad = present && (!S_ISREG(st.st_mode) || (pi_native_binary(path) && unlink(path)));
        free(path);
        if (bad)
            return NULL;
    }
    struct json *commands = hp_value(JSON_ARRAY);
    const char *kinds[] = {"native", "inventory"}, *arches[] = {"i686", "x86_64"};
    int bad = 0;
    for (size_t i = 0; !bad && i < 2; i++)
        for (size_t j = 0; !bad && j < 2; j++)
            bad = make_kind(c, roles, imported, kinds[i], arches[j], commands);
    if (bad) {
        json_free(commands);
        return NULL;
    }
    return commands;
}
