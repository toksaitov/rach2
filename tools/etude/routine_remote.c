/* Ordinary route operations through the existing native serve client. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "include/etude/remote.h"
#include "retention.h"
#include "routine_remote.h"
#include "../../utils/sha256.h"

struct etude_routine_remote {
    struct etude_remote *connection;
    struct etude_remote_observer observer;
    char *run;
    char token[17];
    char **argv, **envp;
    struct json *files;
    FILE *output;
    int output_error;
    volatile sig_atomic_t *stop;
};

static void routine_digest(const void *bytes, size_t size, char hex[65])
{
    struct sha256 hash;
    unsigned char raw[32];
    sha256_init(&hash);
    sha256_update(&hash, bytes, size);
    sha256_final(&hash, raw);
    sha256_hex(raw, hex);
}
static int routine_name(const char *name)
{
    return name && *name && strcmp(name, ".") && strcmp(name, "..") &&
           strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") ==
               strlen(name);
}
static int routine_identity(const char *text)
{
    return routine_name(text) && strlen(text) <= 128 &&
           strspn(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") > 0;
}
static int routine_strings(const struct json *value, enum json_type type)
{
    if (!value || value->type != type)
        return 0;
    for (size_t i = 0; i < value->count; i++)
        if (value->item[i]->type != JSON_STRING)
            return 0;
    return 1;
}
static int routine_word(struct json *words, const char *word)
{
    if (!word || !*word || strpbrk(word, "\r\n"))
        return ret_fail("missing or invalid remote command operand");
    return ret_push(words, ret_string(word));
}
static int routine_extend(struct json *words, const struct json *tail)
{
    for (size_t i = 0; i < tail->count; i++)
        if (routine_word(words, tail->item[i]->string))
            return -1;
    return 0;
}

static struct json *routine_admitted(const struct json *producer, struct json *command)
{
    const struct json *admission = json_member(producer, "_transport_admission");
    if (!admission)
        return command;
    const struct json *policy = json_member(producer, "policy");
    int64_t seconds = ret_number(admission, "seconds");
    if (seconds <= 0 || seconds > 86400)
        goto invalid;
    const char *etude = json_text(producer, "etude"), *site = json_text(producer, "site");
    const char *ledger = json_text(policy, "ledger"), *locks = json_text(policy, "locks");
    const char *capacity = json_text(policy, "capacity"), *grant = json_text(admission, "grant");
    char *duration = ret_format("%lld", (long long)seconds);
    char *grant_env = grant ? ret_format("ETUDE_PREPARATION_GRANT=%s", grant) : NULL;
    char *admitted_env = site ? ret_format("ETUDE_ADMITTED=%s", site) : NULL;
    char *site_env = site ? ret_format("ETUDE_SITE=%s", site) : NULL;
    const char *prefix[] = {etude,
                            "authority",
                            "transport",
                            site,
                            ledger,
                            locks,
                            capacity,
                            duration,
                            "/usr/bin/env",
                            grant_env,
                            etude,
                            "admit",
                            "-i",
                            "-d",
                            locks,
                            "-r",
                            json_text(admission, "result"),
                            ledger,
                            json_text(admission, "claim"),
                            json_text(producer, "account"),
                            "shared",
                            json_text(producer, "preparation_resources"),
                            capacity,
                            duration,
                            "/usr/bin/env",
                            admitted_env,
                            site_env};
    struct json *words = ret_new(JSON_ARRAY);
    int rc = 0;
    for (size_t i = 0; !rc && i < sizeof prefix / sizeof *prefix; i++)
        rc = routine_word(words, prefix[i]);
    if (!rc)
        rc = routine_extend(words, command);
    free(duration);
    free(grant_env);
    free(admitted_env);
    free(site_env);
    json_free(command);
    if (!rc)
        return words;
    json_free(words);
    return NULL;
invalid:
    json_free(command);
    ret_fail("invalid remote transport admission");
    return NULL;
}

static void routine_quote(struct jbuf *buffer, const char *text)
{
    if (*text && strspn(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                              "0123456789_./=:,+-@%") == strlen(text)) {
        jbuf_puts(buffer, text);
        return;
    }
    jbuf_puts(buffer, "'");
    for (const char *p = text; *p; p++)
        if (*p == '\'')
            jbuf_puts(buffer, "'\\''");
        else
            jbuf_put(buffer, p, 1);
    jbuf_puts(buffer, "'");
}

static struct json *routine_ssh(const struct json *producer, struct json *words)
{
    const char *host = json_text(producer, "host");
    int rc = !host || !*host || *host == '-' ||
             strspn(host, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@._-") !=
                 strlen(host);
    struct jbuf shell = {0};
    for (size_t i = 0; !rc && i < words->count; i++) {
        if (i)
            jbuf_puts(&shell, " ");
        routine_quote(&shell, words->item[i]->string);
    }
    json_free(words);
    words = ret_new(JSON_ARRAY);
    const char *prefix[] = {"ssh", "-oBatchMode=yes", "--", host, shell.data};
    for (size_t i = 0; !rc && i < sizeof prefix / sizeof *prefix; i++)
        rc = routine_word(words, prefix[i]);
    jbuf_free(&shell);
    if (rc) {
        json_free(words);
        return NULL;
    }
    return words;
}

static struct json *routine_transport(const struct json *producer, struct json *command,
                                      const char *mode, const char *wsl)
{
    const char *transport = json_text(producer, "transport");
    struct json *words = ret_new(JSON_ARRAY);
    int direct = transport && !strcmp(transport, "wsl"), rc = !transport;
    if (direct) {
#ifdef _WIN32
        rc = !mode || (strcmp(mode, "windows-import") && strcmp(mode, "wsl-native")) || !wsl ||
             !etude_win_absolute(wsl) || !strchr(wsl, ':') ||
             json_member(producer, "_transport_admission") != NULL;
#else
        (void)mode;
        (void)wsl;
        rc = 1;
#endif
    } else if (!transport || (strcmp(transport, "ssh") && strcmp(transport, "wsl-ssh")))
        rc = 1;
    if (!rc && (direct || !strcmp(transport, "wsl-ssh"))) {
        const char *distribution = json_text(producer, "distribution");
        const char *user = json_text(producer, "user");
        rc = !routine_identity(distribution) || !routine_identity(user);
        if (!rc && !direct)
            rc = routine_word(words, "MSYS_NO_PATHCONV=1");
        const char *prefix[] = {
            direct ? wsl : "wsl.exe", "--distribution", distribution, "--user", user, "--exec"};
        for (size_t i = 0; !rc && i < sizeof prefix / sizeof *prefix; i++)
            rc = routine_word(words, prefix[i]);
    }
    if (!rc)
        rc = routine_extend(words, command);
    json_free(command);
    if (!rc && !direct) {
        words = routine_ssh(producer, words);
        rc = !words;
    }
    if (rc) {
        json_free(words);
        ret_fail("invalid or unavailable native responder transport");
        return NULL;
    }
    return words;
}

static void routine_vector_free(char **argv)
{
    for (size_t i = 0; argv && argv[i]; i++)
        free(argv[i]);
    free(argv);
}
static char **routine_vector(const struct json *words)
{
    char **argv = json_alloc((words->count + 1) * sizeof *argv);
    for (size_t i = 0; i < words->count; i++)
        argv[i] = strdup(words->item[i]->string);
    return argv;
}
#ifdef _WIN32
static char **routine_environment(void)
{
    size_t count = 0, used = 0;
    while (environ[count])
        count++;
    char **envp = json_alloc((count + 2) * sizeof *envp);
    for (size_t i = 0; i < count; i++)
        if (_strnicmp(environ[i], "MSYS2_ARG_CONV_EXCL=", 20))
            envp[used++] = strdup(environ[i]);
    envp[used] = strdup("MSYS2_ARG_CONV_EXCL=*");
    return envp;
}
#endif

static int routine_console(int fd, const char *bytes, size_t size)
{
    FILE *console = fd == 1 ? stdout : stderr;
    if (fflush(console))
        return -1;
#ifdef _WIN32
    DWORD written = 0;
    HANDLE handle = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    return size > UINT32_MAX || !WriteFile(handle, bytes, (DWORD)size, &written, NULL) ||
                   written != size
               ? -1
               : 0;
#else
    return fwrite(bytes, 1, size, console) != size || fflush(console) ? -1 : 0;
#endif
}

static void routine_output(void *context, int fd, const char *bytes, size_t size)
{
    struct etude_routine_remote *r = context;
    if (!r->output || fwrite(bytes, 1, size, r->output) != size || fflush(r->output) ||
        routine_console(fd, bytes, size))
        r->output_error = 1;
}
static int routine_expired(void *context)
{
    struct etude_routine_remote *r = context;
    return r->output_error || (r->stop && *r->stop);
}
static void routine_release(struct etude_routine_remote *r)
{
    routine_vector_free(r->argv);
    routine_vector_free(r->envp);
    json_free(r->files);
    free(r->run);
    free(r);
}

struct etude_routine_remote *etude_routine_remote_open(const struct json *producer, const char *run,
                                                       const char *evidence, const char *mode,
                                                       const char *wsl, volatile sig_atomic_t *stop)
{
    const char *program = json_text(producer, "etude"), *state = json_text(producer, "state_root");
    if (!program || !state || !evidence || !*evidence || !run || !*run || strpbrk(run, "/\\\r\n") ||
        (stop && *stop)) {
        ret_fail("invalid remote responder or run identity");
        return NULL;
    }
    struct json *command = ret_new(JSON_ARRAY);
    char *directory = ret_format("%s/%s", state, run);
    const char *prefix[] = {program, "serve", "--stdio", "-d", directory};
    int rc = 0;
    for (size_t i = 0; !rc && i < 5; i++)
        rc = routine_word(command, prefix[i]);
    free(directory);
    command = !rc ? routine_admitted(producer, command) : (json_free(command), NULL);
    command = command ? routine_transport(producer, command, mode, wsl) : NULL;
    if (!command)
        return NULL;
    struct etude_routine_remote *r = json_alloc(sizeof *r);
    r->run = strdup(run);
    r->stop = stop;
    r->argv = routine_vector(command);
    r->files = ret_new(JSON_ARRAY);
    json_free(command);
    r->observer.context = r;
    r->observer.stream = routine_output;
    r->observer.expired = routine_expired;
    struct etude_remote_conf conf = {0};
    conf.argv = r->argv;
    conf.run_id = r->run;
    conf.transcript = evidence;
    conf.max_frame = ETUDE_FRAME_STDIO_MAX;
    conf.stage_us = 86400000000ull;
    conf.require_transport_cleanup = 1;
    conf.cancel_requested = stop;
    conf.observer = &r->observer;
#ifdef _WIN32
    const char *transport = json_text(producer, "transport");
    if (!strcmp(transport, "wsl")) {
        r->envp = routine_environment();
        conf.envp = r->envp;
    }
#endif
    char why[512];
    enum etude_remote_status status = etude_remote_open(&conf, &r->connection, why, sizeof why);
    if (status != ETUDE_REMOTE_OK) {
        ret_fail("responder handshake failed: %s", why);
        routine_release(r);
        return NULL;
    }
    return r;
}

int etude_routine_remote_put(struct etude_routine_remote *r, const char *name, const void *bytes,
                             size_t size, const char *mode)
{
    if (!routine_name(name) || !mode || (r->stop && *r->stop))
        return ret_fail("invalid staged filename or cancelled remote put");
    enum etude_remote_status status = etude_remote_put(r->connection, name, bytes, size, mode);
    if (status != ETUDE_REMOTE_OK)
        return ret_fail("remote put failed: %s", etude_remote_why(r->connection));
    char sha[65];
    routine_digest(bytes, size, sha);
    struct json *ref = ret_new(JSON_OBJECT);
    ret_set(ref, "name", ret_string(name));
    ret_set(ref, "sha256", ret_string(sha));
    ret_set(ref, "size_bytes", ret_integer((int64_t)size));
    ret_set(ref, "mode", ret_string(mode));
    for (size_t i = 0; i < r->files->count; i++)
        if (!strcmp(json_text(r->files->item[i], "name"), name)) {
            json_free(r->files->item[i]);
            r->files->item[i] = ref;
            return 0;
        }
    return ret_push(r->files, ref);
}

static struct json *routine_invocation(struct etude_routine_remote *r, const char *name,
                                       const char *program, const struct json *arguments,
                                       const struct json *environment)
{
    char *attempt = ret_format("%s:%s", r->run, name), sha[65];
    routine_digest(attempt, strlen(attempt), sha);
    memcpy(r->token, sha, 16);
    r->token[16] = 0;
    struct json *inv = ret_new(JSON_OBJECT), *fixtures = ret_new(JSON_ARRAY);
    struct json *workspace = ret_new(JSON_OBJECT);
    ret_set(inv, "schema_version", ret_integer(1));
    ret_set(inv, "run_id", ret_string(r->run));
    ret_set(inv, "attempt_id", ret_string(attempt));
    ret_set(inv, "attempt_token", ret_string(r->token));
    char *case_id = ret_format("routine.%s", name);
    ret_set(inv, "case_id", ret_string(case_id));
    ret_set(inv, "program", ret_string(program));
    ret_set(inv, "arguments", ret_clone(arguments));
    ret_set(inv, "environment", ret_clone(environment));
    ret_set(inv, "stdin", ret_new(JSON_NULL));
    ret_set(inv, "role", ret_string("ordinary"));
    ret_set(inv, "files", ret_clone(r->files));
    ret_set(workspace, "fixture", ret_string("workspace"));
    ret_set(workspace, "given", ret_new(JSON_ARRAY));
    ret_push(fixtures, workspace);
    ret_set(inv, "fixtures", fixtures);
    ret_set(inv, "deadline_us", ret_integer(86400000000ll));
    ret_set(inv, "grace_us", ret_integer(2000000));
    ret_set(inv, "output_max_bytes", ret_integer(536870912));
    free(attempt);
    free(case_id);
    return inv;
}

static int routine_result(struct etude_routine_remote *r, const char *name, const char *output,
                          const struct etude_remote_result *result)
{
    struct json *doc = ret_new(JSON_OBJECT);
    char *attempt = ret_format("%s:%s", r->run, name);
    ret_set(doc, "attempt_id", ret_string(attempt));
    ret_set(doc, "attempt_token", ret_string(result->attempt));
    ret_set(doc, "responder_boot", ret_string(result->boot));
    if (result->started) {
        struct json *child = ret_new(JSON_OBJECT);
        char *pid = ret_format("%ld", result->process.pid);
        char *creation = ret_format("%llu", (unsigned long long)result->process.start);
        ret_set(child, "pid", ret_string(pid));
        ret_set(child, "creation", ret_string(creation));
        ret_set(doc, "child", child);
        free(pid);
        free(creation);
    }
    const char *end = result->end == ETUDE_END_EXITED     ? "exited"
                      : result->end == ETUDE_END_SIGNALED ? "signaled"
                      : result->end == ETUDE_END_STOPPED  ? "stopped"
                                                          : "lost";
    char *value = ret_format("%d", result->value), *path = ret_format("%s.result.json", output);
    ret_set(doc, "end", ret_string(end));
    ret_set(doc, "value", ret_string(value));
    ret_set(doc, "cleanup",
            ret_string(result->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed"));
    int rc = ret_write(path, doc, 0);
    free(attempt);
    free(value);
    free(path);
    json_free(doc);
    return rc;
}

const char *etude_routine_remote_execute(struct etude_routine_remote *r, const char *name,
                                         const char *program, const struct json *arguments,
                                         const struct json *environment, const char *output)
{
    if (!routine_name(name) || !program || !output || !routine_strings(arguments, JSON_ARRAY) ||
        !routine_strings(environment, JSON_OBJECT) || (r->stop && *r->stop)) {
        ret_fail("invalid or cancelled remote invocation");
        return NULL;
    }
    r->output = fopen(output, "wbx");
    if (!r->output) {
        ret_fail("remote log already exists or cannot be created: %s", output);
        return NULL;
    }
    r->output_error = 0;
    struct json *invocation = routine_invocation(r, name, program, arguments, environment);
    struct jbuf raw = {0};
    json_canonical(invocation, &raw);
    struct etude_remote_result result;
    enum etude_remote_status status =
        etude_remote_run(r->connection, raw.data, raw.len, 0, &result);
    int rc = fclose(r->output) || r->output_error;
    r->output = NULL;
    rc |= routine_result(r, name, output, &result);
    int pass = !rc && status == ETUDE_REMOTE_OK && result.complete && result.started &&
               result.end == ETUDE_END_EXITED && result.value == 0 &&
               result.cleanup == ETUDE_CLEANUP_CONFIRMED;
    etude_remote_result_free(&result);
    json_free(invocation);
    jbuf_free(&raw);
    if (!pass)
        ret_fail("remote preparation or cleanup failed: %s", etude_remote_why(r->connection));
    return pass ? r->token : NULL;
}

int etude_routine_remote_get(struct etude_routine_remote *r, const char *token, const char *name,
                             size_t maximum, char **bytes, size_t *size)
{
    if (!routine_name(name) || !token || strlen(token) != 16 ||
        strspn(token, "0123456789abcdef") != 16 || (r->stop && *r->stop))
        return ret_fail("invalid or cancelled remote proof read");
    enum etude_remote_status status =
        etude_remote_get(r->connection, token, name, maximum, bytes, size);
    return status == ETUDE_REMOTE_OK
               ? 0
               : ret_fail("remote proof read failed: %s", etude_remote_why(r->connection));
}

static int routine_end(struct etude_routine_remote **remote, int abandon)
{
    struct etude_routine_remote *r = *remote;
    *remote = NULL;
    if (!r)
        return 0;
    enum etude_remote_status status =
        abandon ? etude_remote_abandon(r->connection) : etude_remote_close(r->connection);
    routine_release(r);
    return status == ETUDE_REMOTE_OK ? 0 : ret_fail("responder transport cleanup failed");
}
int etude_routine_remote_close(struct etude_routine_remote **remote)
{
    return routine_end(remote, 0);
}
int etude_routine_remote_abandon(struct etude_routine_remote **remote)
{
    return routine_end(remote, 1);
}
