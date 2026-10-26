/* Fixed source staging, production and authority steps in one workspace. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "routine_producer.h"
#include "preparation_import.h"
#include "routine_proof.h"
#include "history_json.h"
#ifdef _WIN32
#include "win/files.h"
#endif

struct producer {
    struct hp_context host;
    struct json *operation, *config;
    const struct json *p;
    const char *action, *mode, *commit, *run, *purpose, *arches, *file;
    struct hp_words selection;
    unsigned serial;
    int clean, transport;
};

static const char *field(const struct json *o, const char *key)
{
    return json_text(o, key);
}

static int execute(struct producer *c, struct hp_words *command, char **capture)
{
    char output[4096], error[4096];
    unsigned serial = c->serial++;
    if (hp_path(output, sizeof output, "%s/producer.command.%ld.%u.out", c->host.tree,
                (long)getpid(), serial) ||
        hp_path(error, sizeof error, "%s/producer.command.%ld.%u.err", c->host.tree, (long)getpid(),
                serial)) {
        hp_words_free(command);
        return -1;
    }
    fputs("etude prepare producer:", stdout);
    for (size_t i = 0; i < command->n; i++)
        printf(" %s", command->v[i]);
    putchar('\n');
    fflush(stdout);
    int result = hp_run(&c->host, command->v, output, error);
    char *out = hp_read(output), *err = hp_read(error);
    if (out)
        fputs(out, stdout);
    if (err)
        fputs(err, stderr);
    free(err);
    if (capture)
        *capture = out;
    else
        free(out);
    hp_words_free(command);
    return result;
}

static int environment(const char *key, const char *value)
{
    return value ? setenv(key, value, 1) : unsetenv(key);
}

static int setup_environment(struct producer *c)
{
    char *nonce = hp_read("nonce");
    if (!nonce)
        return -1;
    nonce[strcspn(nonce, "\r\n")] = 0;
    int physical = pi_same(c->mode, "windows-import") || pi_same(c->mode, "wsl-native");
    int bad =
        (physical && !pi_hex(nonce, 64)) || (*nonce && environment("ETUDE_AUTHORITY_NONCE", nonce));
    free(nonce);
    return bad || environment("ETUDE_PHASE_SELECTION_EXPORT", NULL) ||
           environment("ETUDE_PREPARATION_CLEAN", c->clean ? "1" : NULL);
}

static int arguments(struct producer *c)
{
    const struct json *args = json_member(c->operation, "args");
    const char *values[] = {c->action,  "--config",  "${WORKSPACE}/config.json",
                            "--commit", c->commit,   "--run",
                            c->run,     "--purpose", c->purpose,
                            "--arches"};
    if (!args || args->type != JSON_ARRAY || args->count < 12)
        return -1;
    char *config = pi_path(c->host.tree, "config.json");
    int bad = 0;
    for (size_t i = 0; !bad && i < 10; i++) {
        const struct json *v = args->item[i];
        bad = v->type != JSON_STRING ||
              (!pi_same(v->string, values[i]) && !(i == 2 && pi_same(v->string, config)));
    }
    c->arches = args->item[10]->type == JSON_STRING ? args->item[10]->string : NULL;
    bad = bad ||
          (!pi_same(c->arches, "both") && !pi_same(c->arches, "i686") &&
           !pi_same(c->arches, "x86_64")) ||
          args->item[11]->type != JSON_STRING || !pi_same(args->item[11]->string, "--");
    for (size_t i = 12; !bad && i < args->count; i++) {
        const struct json *v = args->item[i];
        bad = v->type != JSON_STRING || !*v->string || strpbrk(v->string, "\r\n");
        if (!bad)
            hp_add(&c->selection, v->string);
    }
    free(config);
    return bad ? -1 : 0;
}

static int installed_actor(const struct json *producer)
{
    const struct json *actor = json_member(producer, "actor");
    if (!actor)
        return 0;
    const char *program = field(producer, "etude");
    const struct json *size = json_member(actor, "size_bytes");
    struct json *actual = program ? pi_describe(program) : NULL;
    int bad = actor->type != JSON_OBJECT || actor->count != 3 || !field(actor, "path") || !actual ||
              !pi_hex(field(actor, "sha256"), 64) || !size || size->type != JSON_INTEGER ||
              !json_equal(size, json_member(actual, "size_bytes")) ||
              !pi_same(field(actor, "sha256"), field(actual, "sha256"));
    json_free(actual);
    return bad ? -1 : 0;
}

static int producer_fields(const struct json *p)
{
    const char *paths[] = {"etude", "site", "repository", "tree_root", "output_root"};
    const char *tokens[] = {"account", "accelerator", "preparation_resources"};
    for (size_t i = 0; i < 5; i++) {
        const char *value = field(p, paths[i]);
        if (!value || *value != '/' || strpbrk(value, "\r\n"))
            return -1;
    }
    for (size_t i = 0; i < 3; i++) {
        const char *value = field(p, tokens[i]);
        if (!value || !*value || strpbrk(value, "\r\n"))
            return -1;
    }
    const struct json *policy = json_member(p, "policy");
    return !field(policy, "ledger") || !field(policy, "capacity") || installed_actor(p) ? -1 : 0;
}

static int condition(struct producer *c, const struct json *clean, const struct json *transport)
{
    const struct json *version = json_member(c->config, "schema_version");
    if (!version || version->type != JSON_INTEGER || version->integer != 1 ||
        (!pi_same(c->mode, "windows-import") && !pi_same(c->mode, "wsl-native") &&
         !pi_same(c->mode, "macos-import") && !pi_same(c->mode, "native")))
        return -1;
    c->clean = clean->integer;
    c->transport = transport->integer;
    if (c->transport && !pi_same(c->mode, "macos-import"))
        return -1;
    if (!pi_same(c->purpose, "phase_first_run") &&
        ((!pi_same(c->purpose, "push") && !pi_same(c->purpose, "ci") &&
          !pi_same(c->purpose, "release")) ||
         c->clean != pi_same(c->purpose, "release")))
        return -1;
    return 0;
}

static int fixed_operation(struct producer *c)
{
    const char *keys[] = {"action", "mode", "commit", "run", "purpose"};
    const char **values[] = {&c->action, &c->mode, &c->commit, &c->run, &c->purpose};
    for (size_t i = 0; i < 5; i++) {
        *values[i] = field(c->operation, keys[i]);
        if (!*values[i])
            return -1;
    }
    const struct json *clean = json_member(c->operation, "clean");
    const struct json *transport = json_member(c->operation, "transport_admitted");
    c->p = json_member(c->operation, "producer");
    if (!pi_same(field(c->config, "format"), "etude-routine-route") ||
        !pi_same(field(c->config, "mode"), c->mode) ||
        !json_equal(c->p, json_member(c->config, "producer")) || producer_fields(c->p) ||
        !pi_hex(c->commit, 40) || !eh_identity(c->run) || !*c->run ||
        strspn(c->run, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") !=
            strlen(c->run) ||
        c->run[0] == '.' || !clean || clean->type != JSON_BOOLEAN || !transport ||
        transport->type != JSON_BOOLEAN)
        return -1;
    if (condition(c, clean, transport))
        return -1;
    return arguments(c);
}

static int copy_file(const char *from, const char *to)
{
    return routine_copy(from, to);
}

static int copy_ledger(struct producer *c)
{
    const struct json *policy = json_member(c->p, "policy");
    const char *ledger = field(policy, "ledger");
    if (!ledger)
        return -1;
    char *claims = pi_path(ledger, "claims");
    int bad = copy_file(claims, "ledger-empty");
    free(claims);
    return bad;
}

static int projection(struct producer *c)
{
    char *bytes = hp_read(field(c->p, "site"));
    if (!bytes || strncmp(bytes, "etude-site 2\n", 13)) {
        free(bytes);
        return -1;
    }
    struct jbuf output = {0};
    char *line = bytes;
    int count = 0;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!strncmp(line, "endpoint ", 9)) {
            jbuf_puts(&output, "endpoint linux\n");
            count++;
        } else {
            jbuf_puts(&output, line);
            jbuf_puts(&output, "\n");
        }
        line = next;
    }
    int bad = count != 1 || routine_create("producer.site", &output);
    jbuf_free(&output);
    free(bytes);
    return bad;
}

static struct json *policy_read(const char *path)
{
    char *raw = hp_read(path), *line = raw;
    struct json *value = hp_value(JSON_OBJECT);
    int bad = !raw;
    while (!bad && line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (line == raw) {
            if (strcmp(line, "etude-site 2"))
                bad = 1;
        } else {
            char *space = strchr(line, ' ');
            if (!space || !space[1])
                bad = 1;
            else {
                *space++ = 0;
                bad = json_member(value, line) != NULL;
                if (!bad)
                    hp_member(value, line, hp_string(space));
            }
        }
        line = next;
    }
    free(raw);
    if (bad) {
        json_free(value);
        value = NULL;
    }
    return value;
}

static int self_command(struct producer *c, struct hp_words *command)
{
    const char *program = field(c->p, "etude");
    if (!program || !*program)
        return -1;
    hp_add(command, program);
    hp_split(command, "prepare producer");
    hp_add(command, c->file);
    return 0;
}

static int source_lock(struct producer *c, int query)
{
    const char *repository = field(c->p, "repository");
    if (!repository)
        return -1;
    struct hp_words command = {0};
    hp_add(&command, field(c->p, "etude"));
    hp_split(&command, query ? "lock tree -q" : "lock tree");
    hp_add(&command, repository);
    if (!query) {
        hp_add(&command, "routine-source");
        if (self_command(c, &command)) {
            hp_words_free(&command);
            return -1;
        }
    }
    return execute(c, &command, NULL);
}

static int authority(struct producer *c, const char *action, int directory)
{
    struct hp_words command = {0};
    hp_add(&command, field(c->p, "etude"));
    hp_add(&command, "authority");
    hp_add(&command, action);
    hp_add(&command, c->run);
    hp_add(&command, c->commit);
    if (directory)
        hp_add(&command, c->host.tree);
    return execute(c, &command, NULL);
}

static int git(struct producer *c, const char *repository, const char *action, const char *bundle,
               const char *commit)
{
    struct hp_words command = {0};
    hp_split(&command, "git -C");
    hp_add(&command, repository);
    hp_add(&command, action);
    if (pi_same(action, "bundle"))
        hp_add(&command, "verify");
    hp_add(&command, bundle);
    if (commit)
        hp_add(&command, commit);
    return execute(c, &command, NULL);
}

static char *stage_source(struct producer *c)
{
    const char *root = field(c->p, "tree_root"), *repository = field(c->p, "repository");
    if (!root || !repository || hp_mkdir(root))
        return NULL;
    char *tree = pi_path(root, c->commit), *bundle = pi_path(c->host.tree, "candidate.bundle");
    int bad = git(c, repository, "bundle", bundle, NULL) ||
              git(c, repository, "fetch", bundle, c->commit);
    struct stat st;
    if (!bad && lstat(tree, &st)) {
        struct hp_words command = {0};
        hp_split(&command, "git -C");
        hp_add(&command, repository);
        hp_split(&command, "worktree add --detach");
        hp_add(&command, tree);
        hp_add(&command, c->commit);
        bad = errno != ENOENT || execute(c, &command, NULL);
        hp_words_free(&command);
    }
    struct json *source = !bad ? pi_source(tree, c->commit) : NULL;
    bad = bad || !source;
    json_free(source);
    free(bundle);
    if (bad) {
        free(tree);
        tree = NULL;
    }
    return tree;
}

static int collection(struct producer *c, const char *tree, const char *out, const char *site)
{
    char *run = pi_path(c->run, "producer"), *record = pi_path(out, "prepare.record");
    /* This is a run suffix, not a filesystem path. */
    run[strlen(c->run)] = ':';
    char *receipt = pi_path(out, "preparation.receipt.json");
    int bad = environment("ETUDE_SITE", site) || environment("ETUDE_ROUTINE_COMMIT", c->commit) ||
              environment("ETUDE_PREPARATION_RUN_ID", run) ||
              environment("ETUDE_PREPARATION_RECEIPT", receipt) ||
              environment("ETUDE_PREPARATION_COMMIT", c->commit) ||
              environment("ETUDE_PREPARATION_EXPORT", "1") ||
              environment("RACH2_QEMU_ACCEL", field(c->p, "accelerator"));
    struct hp_words command = {0};
    hp_add(&command, field(c->p, "etude"));
    hp_split(&command, "prepare collection -t");
    hp_add(&command, tree);
    hp_add(&command, "-a");
    hp_add(&command, c->arches);
    hp_add(&command, "-p");
    hp_add(&command, record);
    for (size_t i = 0; i < c->selection.n; i++)
        hp_add(&command, c->selection.v[i]);
    bad = bad || execute(c, &command, NULL);
    hp_words_free(&command);
    free(run);
    free(record);
    free(receipt);
    return bad;
}

static int export_producer(struct producer *c, const char *tree, const char *out)
{
    struct hp_words command = {0};
    const char *program = field(c->p, "etude");
    hp_add(&command, program);
    hp_split(&command, "prepare export --tree");
    hp_add(&command, tree);
    hp_add(&command, "--commit");
    hp_add(&command, c->commit);
    const char *keys[] = {"record", "receipt", "output"};
    const char *files[] = {"prepare.record", "preparation.receipt.json", "producer.json"};
    for (size_t i = 0; i < 3; i++) {
        char option[32];
        snprintf(option, sizeof option, "--%s", keys[i]);
        char *path = pi_path(out, files[i]);
        hp_add(&command, option);
        hp_add(&command, path);
        free(path);
    }
    hp_add(&command, "--etude");
    hp_add(&command, program);
    return execute(c, &command, NULL);
}

static int produce(struct producer *c, const char *site)
{
    const char *root = field(c->p, "output_root");
    char *tree = stage_source(c), *out = root ? pi_path(root, c->run) : NULL;
    int bad = !tree || !out || hp_mkdir(root) || mkdir(out, 0700) ||
              collection(c, tree, out, site) || export_producer(c, tree, out);
    const char *files[] = {"producer.json", "prepare.record", "preparation.receipt.json"};
    for (size_t i = 0; !bad && i < 3; i++) {
        char *from = pi_path(out, files[i]);
        bad = copy_file(from, files[i]);
        free(from);
    }
    if (!bad) {
        struct json *value = hp_value(JSON_OBJECT);
        hp_member(value, "tree", hp_string(tree));
        char *metadata = pi_path(out, "producer.json");
        hp_member(value, "metadata", hp_string(metadata));
        hp_member(value, "mode", hp_string("produced"));
        bad = pi_create("produced.json", value);
        free(metadata);
        json_free(value);
    }
    free(tree);
    free(out);
    return bad;
}

static unsigned seconds(struct producer *c, const struct json *policy)
{
    const char *raw = field(policy, "seconds");
    char *end;
    unsigned long n = raw ? strtoul(raw, &end, 10) : 0;
    if (!raw || !*raw || *end || !n || n > 604800)
        return 0;
    unsigned bound = pi_same(c->purpose, "push")      ? 270
                     : pi_same(c->purpose, "ci")      ? 540
                     : pi_same(c->purpose, "release") ? 13800
                                                      : 86400;
    return n < bound ? (unsigned)n : bound;
}

static int admission(struct producer *c, const char *site, const struct json *policy)
{
    const struct json *expected = json_member(c->p, "policy");
    if (!field(policy, "locks"))
        return -1;
    const char *keys[] = {"ledger", "capacity"};
    for (size_t i = 0; i < 2; i++)
        if (!pi_same(field(policy, keys[i]), field(expected, keys[i])))
            return -1;
    char *grant = pi_path(c->host.tree, "producer.grant.json");
    int bad = environment("ETUDE_PREPARATION_GRANT", grant);
    free(grant);
    unsigned bound = seconds(c, policy);
    struct hp_words command = {0};
    if (pi_same(c->mode, "windows-import")) {
        hp_add(&command, field(c->p, "etude"));
        hp_split(&command, "authority phase");
        hp_add(&command, c->run);
        hp_add(&command, c->commit);
        hp_add(&command, "producer");
        char *owner = pi_path(c->host.tree, "producer.owner.json");
        hp_add(&command, owner);
        free(owner);
    }
    hp_add(&command, field(c->p, "etude"));
    hp_split(&command, "admit -d");
    hp_add(&command, field(policy, "locks"));
    hp_add(&command, "-r");
    char *result = pi_path(c->host.tree, "producer.admit.result");
    hp_add(&command, result);
    free(result);
    hp_add(&command, field(policy, "ledger"));
    char *claim = pi_path(c->run, "producer:prepare");
    claim[strlen(c->run)] = ':';
    hp_add(&command, claim);
    hp_add(&command, field(c->p, "account"));
    hp_add(&command, "shared");
    hp_add(&command, field(c->p, "preparation_resources"));
    hp_add(&command, field(policy, "capacity"));
    char limit[32];
    snprintf(limit, sizeof limit, "%u", bound);
    hp_add(&command, limit);
    hp_add(&command, "/usr/bin/env");
    const char *vars[] = {"ETUDE_ADMITTED", "ETUDE_SITE"};
    for (size_t i = 0; i < 2; i++) {
        struct jbuf word = {0};
        jbuf_puts(&word, vars[i]);
        jbuf_puts(&word, "=");
        jbuf_puts(&word, site);
        hp_add(&command, word.data);
        jbuf_free(&word);
    }
    hp_add(&command, field(c->p, "etude"));
    hp_split(&command, "lock tree");
    hp_add(&command, field(c->p, "repository"));
    hp_add(&command, "routine-source");
    bad = bad || !bound || self_command(c, &command) || execute(c, &command, NULL);
    hp_words_free(&command);
    free(claim);
    if (!bad)
        bad = pi_same(c->mode, "windows-import") ? authority(c, "facts", 1) : copy_ledger(c);
    return bad;
}

static int production(struct producer *c)
{
    char *projected = pi_path(c->host.tree, "producer.site");
    const char *site = c->transport ? field(c->p, "site") : projected;
    int admitted = pi_same(getenv("ETUDE_ADMITTED"), site), bad;
    if (c->transport && !admitted)
        bad = -1;
    else if (admitted) {
        int locked = source_lock(c, 1);
        bad = locked == 0 ? produce(c, site) : locked == 1 ? source_lock(c, 0) : -1;
    } else {
        bad = projection(c);
        struct json *policy = !bad ? policy_read(projected) : NULL;
        bad = bad || !policy || admission(c, projected, policy);
        json_free(policy);
    }
    free(projected);
    return bad;
}

static int jobs(struct producer *c, const char *tag)
{
    struct json *snapshot = etude_routine_jobs_snapshot(
        &c->host, json_member(c->config, "accepted_jobs"), field(c->config, "github_run"),
        field(c->config, "github_runner"), tag);
    char *path = pi_path(tag, "json");
    path[strlen(tag)] = '.';
    int bad = !snapshot || pi_create(path, snapshot);
    free(path);
    json_free(snapshot);
    return bad;
}

static int transport_cleanup(struct producer *c)
{
    const char *result = field(c->operation, "transport_result");
    char *bytes = result ? hp_read(result) : NULL, *status = NULL;
    if (!bytes || strcmp(bytes, "exited 0\n")) {
        free(bytes);
        return -1;
    }
    struct hp_words command = {0};
    hp_add(&command, field(c->p, "etude"));
    hp_add(&command, "status");
    hp_add(&command, field(json_member(c->p, "policy"), "ledger"));
    int bad = execute(c, &command, &status) || !status;
    struct jbuf status_bytes = {0};
    if (status)
        jbuf_puts(&status_bytes, status);
    bad = bad || routine_create("transport.status", &status_bytes);
    jbuf_free(&status_bytes);
    char *claim = pi_path(c->run, "producer:prepare");
    claim[strlen(c->run)] = ':';
    char *line = status;
    while (!bad && line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        const char *id = !strncmp(line, "claim ", 6)     ? line + 6
                         : !strncmp(line, "request ", 8) ? line + 8
                                                         : "";
        size_t n = strlen(claim);
        bad = !strncmp(id, claim, n) && (id[n] == 0 || id[n] == ':');
        line = next;
    }
    if (!bad)
        bad = copy_file(result, "producer.admit.result") || copy_ledger(c);
    free(bytes);
    free(status);
    free(claim);
    return bad;
}

static int dispatch(struct producer *c)
{
    if (pi_same(c->action, "transport-cleanup"))
        return transport_cleanup(c);
    if (pi_same(c->action, "projection"))
        return projection(c);
    if (pi_same(c->action, "produce"))
        return production(c);
    int freeze = pi_same(c->action, "freeze"), facts = pi_same(c->action, "facts");
    if (!freeze && !facts && !pi_same(c->action, "restore"))
        return -1;
    int bad = (freeze || facts) && jobs(c, "jobs-before");
    bad = bad || authority(c, c->action, facts);
    if (!bad && freeze)
        bad = jobs(c, "jobs-after") || authority(c, "facts", 1);
    if (!bad && pi_same(c->action, "restore"))
        bad = copy_ledger(c);
    return bad;
}

int etude_routine_producer_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    struct producer c;
    memset(&c, 0, sizeof c);
    c.host.stop = stop;
    if (argc != 2 || !getcwd(c.host.tree, sizeof c.host.tree))
        return 2;
    char *path = realpath(argv[1], NULL), *expected = pi_path(c.host.tree, "operation.json");
    int bad = !path || strcmp(path, expected);
    c.file = path;
    c.operation = !bad ? pi_document(path) : NULL;
    c.config = !bad ? pi_document("config.json") : NULL;
    bad = bad || !c.operation || !c.config || fixed_operation(&c) || setup_environment(&c);
    if (!bad)
        bad = dispatch(&c);
    if (bad)
        fputs("etude prepare producer: fixed operation refused or failed; evidence retained\n",
              stderr);
    hp_words_free(&c.selection);
    json_free(c.operation);
    json_free(c.config);
    free(path);
    free(expected);
    return bad ? 1 : 0;
}
