/* Route source production and endpoint handover through native owners. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "routine_route.h"
#include "routine_entry.h"
#include "routine_remote.h"
#include "routine_proof.h"
#include "routine_evidence.h"
#include "preparation_import.h"
#include "measurement_context.h"
#include "sha256.h"

struct route {
    struct routine_input in;
    struct json *config, *dynamic, *preparation;
    const char *action, *mode, *nonce;
    char descriptor[4096], work[4096];
    volatile sig_atomic_t *stop;
};

static const char *field(const struct json *object, const char *name)
{
    return json_text(object, name);
}

static int path(char out[4096], const char *root, const char *name)
{
    return routine_path(out, root, name);
}

static int create(const char *name, const struct json *value)
{
    return pi_create(name, value);
}

static struct json *reference(const char *path)
{
    struct jbuf bytes = {0};
    struct json *value = NULL;
    char why[256];
    if (!routine_ref(&bytes, path))
        json_parse(bytes.data, bytes.len, 1, &value, why, sizeof why);
    jbuf_free(&bytes);
    return value;
}

static int member_ref(struct json *object, const char *key, const char *directory, const char *name)
{
    char file[4096];
    if (path(file, directory, name))
        return -1;
    struct json *value = reference(file);
    if (!value)
        return -1;
    hp_member(object, key, value);
    return 0;
}

static int local(struct route *r, const char *action, const char *extra)
{
    struct hp_words command = {0};
    hp_add(&command, r->in.program);
    hp_split(&command, "authority");
    hp_add(&command, action);
    hp_add(&command, r->in.run);
    hp_add(&command, r->in.commit);
    if (extra)
        hp_add(&command, extra);
    int bad = routine_command(&command, r->stop);
    hp_words_free(&command);
    return bad;
}

static int empty_ledger(const char *directory)
{
    char name[4096];
    if (path(name, directory, "ledger-empty"))
        return -1;
    char *bytes = hp_read(name);
    int bad = !bytes || strcmp(bytes, "etude-ledger 1\n");
    free(bytes);
    return bad ? -1 : 0;
}

static int facts(struct route *r, const char *name, char directory[4096])
{
    return path(directory, r->work, name) || mkdir(directory, 0700) || local(r, "facts", directory);
}

static struct json *clock_read(struct route *r, const char *name)
{
    char output[4096];
    if (path(output, r->work, name))
        return NULL;
    struct hp_words command = {0};
    hp_add(&command, r->in.program);
    hp_split(&command, "authority clock");
    hp_add(&command, output);
    int bad = routine_command(&command, r->stop);
    hp_words_free(&command);
    return bad ? NULL : hp_document(output);
}

static int put_json(struct etude_routine_remote *remote, const char *name, const struct json *value)
{
    struct jbuf bytes = {0};
    json_canonical(value, &bytes);
    int bad = etude_routine_remote_put(remote, name, bytes.data, bytes.len, "0600");
    jbuf_free(&bytes);
    return bad;
}

static int put_file(struct etude_routine_remote *remote, const char *name, const char *source,
                    const char *mode, const struct json *expected)
{
    struct stat st;
    if (stat(source, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > SIZE_MAX)
        return -1;
    FILE *file = fopen(source, "rb");
    size_t size = (size_t)st.st_size;
    char *bytes = malloc(size ? size : 1);
    int bad = !file || !bytes;
    if (!bad)
        bad = fread(bytes, 1, size, file) != size || fgetc(file) != EOF || ferror(file);
    if (file && fclose(file))
        bad = 1;
    if (!bad && expected) {
        struct sha256 hash;
        unsigned char digest[32];
        char hex[65];
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)bytes, size);
        sha256_final(&hash, digest);
        sha256_hex(digest, hex);
        const struct json *length = json_member(expected, "size_bytes");
        bad = !length || length->type != JSON_INTEGER || length->integer != (long long)size ||
              !pi_same(field(expected, "sha256"), hex) || pi_verify(source, expected);
    }
    if (!bad)
        bad = etude_routine_remote_put(remote, name, bytes, size, mode);
    free(bytes);
    return bad;
}

static int put_actor(struct route *r, struct etude_routine_remote *remote)
{
    const struct json *actor = json_member(json_member(r->config, "producer"), "actor");
    const char *source = field(actor, "path");
    if (!source || actor->type != JSON_OBJECT || actor->count != 3)
        return -1;
    /* The protected route binds the retained bytes of the installed tool. */
    struct json *actual = pi_describe(source);
    const struct json *size = json_member(actor, "size_bytes");
    int bad = !actual || !size || size->type != JSON_INTEGER ||
              !pi_same(field(actor, "sha256"), field(actual, "sha256")) ||
              !json_equal(size, json_member(actual, "size_bytes")) ||
              put_file(remote, "producer-etude", source, "0755", actual) ||
              pi_verify(source, actual);
    json_free(actual);
    return bad;
}

static struct json *remote_operation(struct route *r, const char *action, int admitted,
                                     const char *transport_result)
{
    struct json *value = hp_value(JSON_OBJECT), *args = hp_value(JSON_ARRAY);
    const struct json *producer = json_member(r->config, "producer");
    const char *fixed[] = {action,     "--config",   "${WORKSPACE}/config.json",
                           "--commit", r->in.commit, "--run",
                           r->in.run,  "--purpose",  r->in.purpose,
                           "--arches", r->in.arches, "--"};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_item(args, hp_string(fixed[i]));
    for (int i = 0; i < r->in.count; i++)
        hp_item(args, hp_string(r->in.selection[i]));
    hp_member(value, "action", hp_string(action));
    hp_member(value, "args", args);
    hp_member(value, "producer", hp_clone(producer));
    hp_member(value, "mode", hp_string(r->mode));
    hp_member(value, "run", hp_string(r->in.run));
    hp_member(value, "commit", hp_string(r->in.commit));
    hp_member(value, "purpose", hp_string(r->in.purpose));
    hp_member(value, "clean", hp_clone(json_member(r->preparation, "clean")));
    struct json *flag = hp_value(JSON_BOOLEAN);
    flag->integer = admitted;
    hp_member(value, "transport_admitted", flag);
    hp_member(value, "transport_result", hp_string(transport_result));
    return value;
}

static int fetch_file(struct etude_routine_remote *remote, const char *token, const char *directory,
                      const char *name)
{
    char output[4096], *bytes = NULL;
    size_t size = 0;
    if (path(output, directory, name) ||
        etude_routine_remote_get(remote, token, name, 16u << 20, &bytes, &size))
        return -1;
    struct jbuf buffer = {0};
    jbuf_put(&buffer, bytes, size);
    int bad = routine_create(output, &buffer);
    jbuf_free(&buffer);
    free(bytes);
    return bad;
}

static int fetch_results(struct route *r, struct etude_routine_remote *remote, const char *token,
                         const char *action, int admitted, const char *directory)
{
    struct hp_words names = {0};
    if (!admitted)
        hp_add(&names, "ledger-empty");
    int windows = pi_same(r->mode, "windows-import"), freeze = pi_same(action, "freeze");
    if (freeze || pi_same(action, "facts") || (windows && pi_same(action, "produce")))
        hp_split(&names, "binding state policy-before policy-disabled facts.json");
    if (freeze || pi_same(action, "facts"))
        hp_add(&names, "jobs-before.json");
    if (freeze)
        hp_add(&names, "jobs-after.json");
    if (pi_same(action, "transport-cleanup"))
        hp_split(&names, "producer.admit.result transport.status");
    if (pi_same(action, "produce")) {
        hp_split(&names, "producer.json prepare.record preparation.receipt.json produced.json");
        if (!admitted)
            hp_add(&names, "producer.admit.result");
        if (windows)
            hp_add(&names, "producer.owner.json");
    }
    int bad = 0;
    for (size_t i = 0; !bad && i < names.n; i++)
        bad = fetch_file(remote, token, directory, names.v[i]);
    hp_words_free(&names);
    return bad;
}

static unsigned producer_bound(struct route *r)
{
    const char *seconds =
        field(json_member(json_member(r->config, "producer"), "policy"), "seconds");
    if (!seconds || !*seconds || strspn(seconds, "0123456789") != strlen(seconds))
        return 0;
    unsigned long n = strtoul(seconds, NULL, 10);
    unsigned bound = pi_same(r->in.purpose, "push")      ? 270
                     : pi_same(r->in.purpose, "ci")      ? 540
                     : pi_same(r->in.purpose, "release") ? 13800
                                                         : 86400;
    return !n || n > 86400 ? 0 : n < bound ? (unsigned)n : bound;
}

static int transport_paths(struct route *r, char grant[4096], char result[4096])
{
    const char *root = field(json_member(r->config, "producer"), "state_root");
    if (!root)
        return -1;
    char name[512];
    int n = snprintf(name, sizeof name, "%s:produce.grant.json", r->in.run);
    if (n < 0 || n >= (int)sizeof name || path(grant, root, name))
        return -1;
    n = snprintf(name, sizeof name, "%s:produce.admit.result", r->in.run);
    return n < 0 || n >= (int)sizeof name || path(result, root, name) ? -1 : 0;
}

static struct json *remote_transport(struct route *r, int admitted, const char *grant,
                                     const char *result)
{
    struct json *p = hp_clone(json_member(r->config, "producer"));
    if (!admitted)
        return p;
    unsigned seconds = producer_bound(r);
    if (!seconds) {
        json_free(p);
        return NULL;
    }
    struct json *admission = hp_value(JSON_OBJECT);
    hp_member(admission, "grant", hp_string(grant));
    hp_member(admission, "result", hp_string(result));
    struct jbuf claim = {0};
    jbuf_puts(&claim, r->in.run);
    jbuf_puts(&claim, ":producer:prepare");
    hp_member(admission, "claim", hp_string(claim.data));
    jbuf_free(&claim);
    hp_member(admission, "seconds", hp_number(seconds));
    hp_member(p, "_transport_admission", admission);
    return p;
}

static struct json *remote_environment(struct route *r, int admitted, const char *grant)
{
    const struct json *p = json_member(r->config, "producer");
    const struct json *original = json_member(p, "environment");
    if (!original || original->type != JSON_OBJECT)
        return NULL;
    for (size_t i = 0; i < original->count; i++)
        if (original->item[i]->type != JSON_STRING)
            return NULL;
    struct json *env = hp_clone(original);
    hp_member(env, "WORKSPACE", hp_string("${WORKSPACE}"));
    if (admitted) {
        const char *site = field(p, "site");
        if (!site) {
            json_free(env);
            return NULL;
        }
        hp_member(env, "ETUDE_ADMITTED", hp_string(site));
        hp_member(env, "ETUDE_SITE", hp_string(site));
        hp_member(env, "ETUDE_PREPARATION_GRANT", hp_string(grant));
    }
    return env;
}

struct remote_step {
    const char *action, *bundle, *output, *grant, *result;
    int admitted;
};

static int remote_execute(struct route *r, struct etude_routine_remote *remote,
                          const struct remote_step *step, const char **token)
{
    struct json *operation = remote_operation(r, step->action, step->admitted, step->result);
    struct json *env = remote_environment(r, step->admitted, step->grant),
                *args = hp_value(JSON_ARRAY);
    hp_item(args, hp_string("prepare"));
    hp_item(args, hp_string("producer"));
    hp_item(args, hp_string("${WORKSPACE}/operation.json"));
    struct jbuf nonce = {0};
    jbuf_puts(&nonce, r->nonce);
    jbuf_puts(&nonce, "\n");
    int bad = !env || put_json(remote, "config.json", r->dynamic) ||
              etude_routine_remote_put(remote, "nonce", nonce.data, nonce.len, "0600") ||
              put_actor(r, remote) ||
              (step->bundle && put_file(remote, "candidate.bundle", step->bundle, "0600", NULL)) ||
              put_json(remote, "operation.json", operation);
    if (!bad) {
        *token = etude_routine_remote_execute(remote, step->action, "producer-etude", args, env,
                                              step->output);
        bad = *token == NULL;
    }
    json_free(operation);
    json_free(env);
    json_free(args);
    jbuf_free(&nonce);
    return bad;
}

static int remote(struct route *r, const char *action, const char *bundle, const char *label,
                  char result_directory[4096])
{
    const char *name = label ? label : action;
    char grant[4096], result[4096], wire[4096], output[4096], run[512], file[512];
    int n = snprintf(run, sizeof run, "%s:%s", r->in.run, name);
    int m = snprintf(file, sizeof file, "%s.wire", name);
    if (n < 0 || n >= (int)sizeof run || m < 0 || m >= (int)sizeof file ||
        path(wire, r->work, file) || transport_paths(r, grant, result))
        return -1;
    m = snprintf(file, sizeof file, "%s.log", name);
    if (m < 0 || m >= (int)sizeof file || path(output, r->work, file) ||
        path(result_directory, r->work, name))
        return -1;
    int admitted = pi_same(action, "produce") && pi_same(r->mode, "macos-import");
    struct json *p = remote_transport(r, admitted, grant, result);
    const char *wsl = field(json_member(r->config, "physical"), "wsl");
    struct etude_routine_remote *connection =
        p ? etude_routine_remote_open(p, run, wire, r->mode, wsl, r->stop) : NULL;
    json_free(p);
    if (!connection)
        return -1;
    const char *token = NULL;
    struct remote_step step = {action, bundle, output, grant, result, admitted};
    int bad = remote_execute(r, connection, &step, &token) || mkdir(result_directory, 0700) ||
              fetch_results(r, connection, token, action, admitted, result_directory);
    if (!bad)
        bad = etude_routine_remote_close(&connection);
    if (connection)
        etude_routine_remote_abandon(&connection);
    if (!bad && admitted) {
        char cleanup[4096];
        bad = remote(r, "transport-cleanup", NULL, "produce-cleanup", cleanup);
        const char *files[] = {"producer.admit.result", "ledger-empty", "transport.status"};
        for (size_t i = 0; !bad && i < 3; i++) {
            char from[4096], to[4096];
            bad = path(from, cleanup, files[i]) || path(to, result_directory, files[i]) ||
                  routine_copy(from, to);
        }
    }
    return bad;
}

static int lease_endpoint(struct json *endpoints, const char *name, const char *directory)
{
    if (empty_ledger(directory))
        return -1;
    struct json *endpoint = hp_value(JSON_OBJECT), *flag = hp_value(JSON_BOOLEAN);
    flag->integer = 1;
    hp_member(endpoint, "name", hp_string(name));
    hp_member(endpoint, "normal_disabled", flag);
    const char *keys[] = {"policy_before", "policy_disabled", "ledger_empty"};
    const char *files[] = {"policy-before", "policy-disabled", "ledger-empty"};
    int bad = 0;
    for (size_t i = 0; !bad && i < 3; i++)
        bad = member_ref(endpoint, keys[i], directory, files[i]);
    if (!bad)
        hp_item(endpoints, endpoint);
    else
        json_free(endpoint);
    return bad;
}

static int lease_inputs(struct route *r, const char *linux, const char *windows,
                        const char *checked)
{
    char file[4096];
    if (path(file, windows, "facts.json"))
        return -1;
    struct json *facts = hp_document(file), *authority = hp_value(JSON_OBJECT);
    struct json *jobs = hp_value(JSON_ARRAY), *endpoints = hp_value(JSON_ARRAY);
    int bad = !facts || member_ref(authority, "binding", windows, "binding") ||
              member_ref(authority, "state", windows, "state") || !field(facts, "snapshot") ||
              !field(facts, "gate");
    hp_member(authority, "snapshot", hp_clone(json_member(facts, "snapshot")));
    hp_member(authority, "gate", hp_clone(json_member(facts, "gate")));
    const char *roots[] = {linux, linux, checked};
    const char *names[] = {"jobs-before.json", "jobs-after.json", "jobs-before.json"};
    for (size_t i = 0; !bad && i < 3; i++) {
        struct json *ref = path(file, roots[i], names[i]) ? NULL : reference(file);
        if (ref)
            hp_item(jobs, ref);
        else
            bad = 1;
    }
    hp_member(authority, "accepted_jobs", jobs);
    bad = bad || lease_endpoint(endpoints, "linux", linux) ||
          lease_endpoint(endpoints, "windows", windows);
    struct json *value = hp_value(JSON_OBJECT);
    hp_member(value, "authority", authority);
    hp_member(value, "endpoints", endpoints);
    if (!bad)
        bad = path(file, r->in.out, "lease.inputs.json") || create(file, value);
    json_free(value);
    json_free(facts);
    return bad;
}

static int quiesce(struct route *r)
{
    if (!pi_same(r->mode, "windows-import") && !pi_same(r->mode, "wsl-native"))
        return -1;
    char linux[4096], windows[4096], checked[4096], grant[4096];
    return remote(r, "freeze", NULL, NULL, linux) || local(r, "freeze", NULL) ||
           facts(r, "windows-grant", windows) || remote(r, "facts", NULL, "facts-grant", checked) ||
           path(grant, r->in.out, "physical-grant.json") || local(r, "physical", grant) ||
           lease_inputs(r, linux, windows, checked);
}

static int candidate_bundle(struct route *r, char bundle[4096])
{
    if (path(bundle, r->work, "candidate.bundle"))
        return -1;
    struct hp_words command = {0};
    hp_split(&command, "git -C");
    hp_add(&command, r->in.tree);
    hp_split(&command, "bundle create");
    hp_add(&command, bundle);
    hp_add(&command, "HEAD");
    int bad = routine_command(&command, r->stop);
    hp_words_free(&command);
    hp_split(&command, "git bundle list-heads");
    hp_add(&command, bundle);
    hp_add(&command, "HEAD");
    bad = bad || routine_query(&r->in, &command, "candidate-heads", r->stop);
    hp_words_free(&command);
    char output[4096];
    char *bytes =
        !bad && !path(output, r->in.out, "candidate-heads.stdout") ? hp_read(output) : NULL;
    struct hp_words heads = {0};
    bad = bad || !bytes || hp_split(&heads, bytes) || heads.n != 2 ||
          !pi_same(heads.v[0], r->in.commit) || !pi_same(heads.v[1], "HEAD");
    hp_words_free(&heads);
    free(bytes);
    return bad;
}

static int producer_metadata(struct route *r, const char *directory, struct json **metadata,
                             struct json **actual)
{
    char name[4096];
    if (path(name, directory, "producer.json"))
        return -1;
    *metadata = pi_document(name);
    if (path(name, directory, "produced.json"))
        return -1;
    *actual = hp_document(name);
    const struct json *receipt = json_member(*metadata, "receipt");
    struct jbuf run = {0};
    jbuf_puts(&run, r->in.run);
    jbuf_puts(&run, ":producer");
    int bad = !*metadata || !*actual || !pi_same(field(*metadata, "commit"), r->in.commit) ||
              !pi_same(field(receipt, "run_id"), run.data) || !field(*actual, "tree") ||
              !field(*actual, "metadata");
    const struct json *clean = json_member(r->preparation, "clean");
    if (!bad && clean && clean->integer) {
        const struct json *condition = json_member(receipt, "conditioning");
        const struct json *provenance = json_member(condition, "provenance");
        bad = !pi_same(field(condition, "inhouse"), "clean") ||
              !pi_same(field(condition, "backend"), "ordinary") || !provenance ||
              provenance->type != JSON_NULL;
    }
    jbuf_free(&run);
    return bad;
}

static int result_zero(const struct json *result)
{
    const char *keys[] = {"end", "value", "exit_end", "exit_value", "cleanup", "output_error"};
    if (!result || result->type != JSON_OBJECT || result->count != 6)
        return -1;
    for (size_t i = 0; i < 6; i++) {
        const struct json *v = json_member(result, keys[i]);
        if (!v || v->type != JSON_INTEGER || v->integer != (i == 4 ? 1 : 0))
            return -1;
    }
    return 0;
}

static struct json *phase_result(void)
{
    struct json *value = hp_value(JSON_OBJECT);
    hp_member(value, "end", hp_string("exited"));
    hp_member(value, "value", hp_number(0));
    hp_member(value, "exit_end", hp_string("exited"));
    hp_member(value, "exit_value", hp_number(0));
    hp_member(value, "cleanup", hp_string("confirmed"));
    hp_member(value, "output_error", hp_number(0));
    hp_member(value, "active", hp_number(0));
    return value;
}

static void common(struct route *r, struct json *value, const char *format)
{
    hp_member(value, "format", hp_string(format));
    hp_member(value, "schema_version", hp_number(1));
    hp_member(value, "run_id", hp_string(r->in.run));
    hp_member(value, "commit", hp_string(r->in.commit));
}

static int producer_phase(struct route *r, const char *produced)
{
    char name[4096];
    if (path(name, produced, "producer.owner.json"))
        return -1;
    struct json *raw = hp_document(name), *grant = NULL;
    int bad = !raw || result_zero(json_member(raw, "result")) ||
              path(name, r->in.out, "routine.grant.json") || routine_json(name, &grant);
    struct json *phase = hp_value(JSON_OBJECT);
    common(r, phase, "etude-routine-phase");
    const char *grant_fields[] = {"route", "lease", "owner"};
    for (size_t i = 0; !bad && i < 3; i++) {
        const struct json *v = json_member(grant, grant_fields[i]);
        if (!v)
            bad = 1;
        else
            hp_member(phase, grant_fields[i], hp_clone(v));
    }
    const char *source[] = {"process", "machine_id", "boot_id", "start", "end"};
    const char *target[] = {"child", "machine_id", "boot_id", "start", "end"};
    for (size_t i = 0; !bad && i < 5; i++) {
        const struct json *v = json_member(raw, source[i]);
        if (!v)
            bad = 1;
        else
            hp_member(phase, target[i], hp_clone(v));
    }
    hp_member(phase, "phase", hp_string("producer"));
    hp_member(phase, "result", phase_result());
    bad = bad || member_ref(phase, "admission", produced, "producer.admit.result") ||
          member_ref(phase, "receipt", produced, "producer.owner.json") ||
          path(name, r->in.out, "routine.producer.json") || create(name, phase);
    json_free(raw);
    json_free(grant);
    json_free(phase);
    return bad;
}

static int upstream(struct route *r, const char *produced, const struct json *before,
                    const struct json *after)
{
    struct json *value = hp_value(JSON_OBJECT), *producer = hp_value(JSON_OBJECT);
    common(r, value, "etude-routine-upstream");
    struct json *route = reference(r->descriptor);
    int bad = !route;
    hp_member(value, "route", route ? route : hp_value(JSON_NULL));
    bad = bad || member_ref(value, "routine_grant", r->in.out, "routine.grant.json");
    hp_member(value, "mode", hp_string("produced"));
    const char *keys[] = {"receipt", "record", "metadata", "admission", "cleanup"};
    const char *files[] = {"preparation.receipt.json", "prepare.record", "producer.json",
                           "producer.admit.result", "producer.owner.json"};
    for (size_t i = 0; !bad && i < 5; i++)
        bad = member_ref(producer, keys[i], produced, files[i]);
    hp_member(value, "producer", producer);
    struct json *interval = hp_value(JSON_OBJECT);
    hp_member(interval, "machine_id", hp_clone(json_member(before, "machine_id")));
    hp_member(interval, "boot_id", hp_clone(json_member(before, "boot_id")));
    hp_member(interval, "start", hp_clone(json_member(before, "reading")));
    hp_member(interval, "end", hp_clone(json_member(after, "reading")));
    hp_member(value, "owned_interval", interval);
    struct json *result = hp_value(JSON_OBJECT);
    hp_member(result, "outcome", hp_string("pass"));
    hp_member(result, "cleanup", hp_string("confirmed"));
    hp_member(value, "result", result);
    char name[4096];
    bad = bad || path(name, r->in.out, "routine.upstream.json") || create(name, value);
    json_free(value);
    return bad;
}

static char *suffix_path(const char *root, const char *middle, const char *leaf)
{
    if (!root)
        return NULL;
    struct jbuf value = {0};
    size_t n = strlen(root);
    while (n && root[n - 1] == '/')
        n--;
    jbuf_put(&value, root, n);
    jbuf_puts(&value, "/");
    jbuf_puts(&value, middle);
    if (leaf) {
        jbuf_puts(&value, "/");
        jbuf_puts(&value, leaf);
    }
    return value.data;
}

static struct json *import_configuration(struct route *r, const char *produced,
                                         const struct json *actual)
{
    const struct json *p = json_member(r->config, "producer");
    struct json *importer = hp_value(JSON_OBJECT);
    int windows = pi_same(r->mode, "windows-import"), bad = 0;
    if (windows) {
        char *tree = suffix_path(field(p, "unc_tree_root"), r->in.commit, NULL);
        char *metadata = suffix_path(field(p, "unc_output_root"), r->in.run, "producer.json");
        bad = !tree || !metadata;
        hp_member(importer, "transport", hp_string("local"));
        hp_member(importer, "tree", hp_string(tree ? tree : ""));
        hp_member(importer, "metadata", hp_string(metadata ? metadata : ""));
        free(tree);
        free(metadata);
    } else {
        const char *keys[] = {"transport", "host", "etude", "distribution", "user"};
        size_t count = pi_same(field(p, "transport"), "wsl-ssh") ? 5 : 3;
        for (size_t i = 0; !bad && i < count; i++) {
            const char *v = field(p, keys[i]);
            if (!v)
                bad = 1;
            else
                hp_member(importer, keys[i], hp_string(v));
        }
        hp_member(importer, "tree", hp_clone(json_member(actual, "tree")));
        hp_member(importer, "metadata", hp_clone(json_member(actual, "metadata")));
    }
    char name[4096];
    struct json *descriptor = !path(name, produced, "producer.json") ? pi_describe(name) : NULL;
    if (!descriptor)
        bad = 1;
    else
        hp_member(importer, "sha256", hp_clone(json_member(descriptor, "sha256")));
    json_free(descriptor);
    if (bad) {
        json_free(importer);
        importer = NULL;
    }
    return importer;
}

static int produce(struct route *r, char importer_path[4096])
{
    struct json *before = clock_read(r, "producer-start.json"), *after = NULL;
    struct json *metadata = NULL, *actual = NULL;
    char bundle[4096], produced[4096];
    int bad =
        !before || candidate_bundle(r, bundle) || remote(r, "produce", bundle, NULL, produced);
    if (!bad)
        after = clock_read(r, "producer-end.json");
    bad = bad || !after || producer_metadata(r, produced, &metadata, &actual) ||
          !pi_same(field(before, "machine_id"), field(after, "machine_id")) ||
          !pi_same(field(before, "boot_id"), field(after, "boot_id"));
    if (!bad && pi_same(r->mode, "windows-import"))
        bad = empty_ledger(produced) || upstream(r, produced, before, after) ||
              producer_phase(r, produced) || etude_routine_pack(r->in.out, NULL, r->stop) ||
              local(r, "permit", "consumer");
    struct json *importer = !bad ? import_configuration(r, produced, actual) : NULL;
    struct json *value = hp_value(JSON_OBJECT);
    hp_member(value, "format", hp_string("etude-preparation-import"));
    hp_member(value, "schema_version", hp_number(1));
    hp_member(value, "commit", hp_string(r->in.commit));
    hp_member(value, "producer", importer ? importer : hp_value(JSON_NULL));
    bad = bad || !importer || path(importer_path, r->in.out, "preparation-import.json") ||
          create(importer_path, value);
    json_free(value);
    json_free(metadata);
    json_free(actual);
    json_free(before);
    json_free(after);
    return bad;
}

static int project_site(const char *source, const char *destination, const char *endpoint)
{
    char *bytes = hp_read(source), *line = bytes;
    if (!bytes || strncmp(bytes, "etude-site 2\n", 13)) {
        free(bytes);
        return -1;
    }
    struct jbuf output = {0};
    int count = 0;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!strncmp(line, "endpoint ", 9)) {
            jbuf_puts(&output, "endpoint ");
            jbuf_puts(&output, endpoint);
            count++;
        } else
            jbuf_puts(&output, line);
        jbuf_puts(&output, "\n");
        line = next;
    }
    int bad = count != 1 || routine_create(destination, &output);
    jbuf_free(&output);
    free(bytes);
    return bad;
}

static int prepare_selection(struct route *r)
{
    const char *expected = field(json_member(r->preparation, "selection_export"), "path");
    if (!expected)
        return 0;
    char actual[4096];
    if (path(actual, r->in.out, "routine.selection.export.json"))
        return -1;
    struct hp_words command = {0};
    const char *fixed[] = {r->in.program, "run", "-N",         "-J", actual,     "-t",
                           r->in.tree,    "-a",  r->in.arches, "-A", r->in.accel};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&command, fixed[i]);
    for (int i = 0; i < r->in.count; i++)
        hp_add(&command, r->in.selection[i]);
    int bad = routine_command(&command, r->stop) || routine_selection(expected, actual);
    hp_words_free(&command);
    return bad;
}

static int consumer_environment(struct route *r, const char *importer, const char *site)
{
    const struct json *clean = json_member(r->preparation, "clean");
    int bad = setenv("ETUDE_PREPARATION_IMPORT", importer, 1) ||
              (clean && clean->integer ? setenv("ETUDE_PREPARATION_CLEAN", "1", 1)
                                       : unsetenv("ETUDE_PREPARATION_CLEAN")) ||
              setenv("ETUDE_SITE", site, 1);
    char grant[4096];
    bad = bad || path(grant, r->in.out, "preparation.grant.json") ||
          setenv("ETUDE_PREPARATION_GRANT", grant, 1);
    if (!bad && pi_same(r->mode, "windows-import")) {
        const char *vars[] = {"ETUDE_ROUTINE_GRANT", "ETUDE_ROUTINE_UPSTREAM",
                              "ETUDE_ROUTINE_PROOFS"};
        const char *names[] = {"routine.grant.json", "routine.upstream.json", "routine-proofs"};
        for (size_t i = 0; !bad && i < 3; i++)
            bad = path(grant, r->in.out, names[i]) || setenv(vars[i], grant, 1);
        bad = bad || setenv("ETUDE_AUTHORITY_PHASE", "consumer", 1);
    }
    return bad;
}

static unsigned consumer_bound(struct route *r, const struct routine_site *site)
{
    unsigned bound = pi_same(r->in.purpose, "push")      ? 270
                     : pi_same(r->in.purpose, "ci")      ? 540
                     : pi_same(r->in.purpose, "release") ? 13800
                                                         : 86400;
    return site->seconds < bound ? site->seconds : bound;
}

static int consumer_admission(struct route *r, const struct routine_site *policy, const char *site)
{
    const struct json *consumer = json_member(r->config, "consumer");
    const char *env = field(consumer, "env"), *account = field(consumer, "account");
    const char *resources = field(consumer, "preparation_resources");
    char result[4096], claim[512], seconds[32];
    int n = snprintf(claim, sizeof claim, "%s:prepare", r->in.run);
    if (!env || !account || !resources || n < 0 || n >= (int)sizeof claim ||
        path(result, r->in.out, "preparation.admit.result"))
        return -1;
    snprintf(seconds, sizeof seconds, "%u", consumer_bound(r, policy));
    struct hp_words command = {0};
    const char *fixed[] = {r->in.program, "admit",  "-d",           policy->locks,
                           "-r",          result,   policy->ledger, claim,
                           account,       "shared", resources,      policy->capacity_text,
                           seconds,       env};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        hp_add(&command, fixed[i]);
    struct jbuf admitted = {0};
    jbuf_puts(&admitted, "ETUDE_ADMITTED=");
    jbuf_puts(&admitted, site);
    hp_add(&command, admitted.data);
    jbuf_free(&admitted);
    const char *prepare[] = {r->in.program, "prepare",     "routine",    r->in.purpose,
                             r->in.tree,    r->in.out,     r->in.arches, r->in.run,
                             site,          r->in.program, r->in.accel};
    for (size_t i = 0; i < sizeof prepare / sizeof *prepare; i++)
        hp_add(&command, prepare[i]);
    for (int i = 0; i < r->in.count; i++)
        hp_add(&command, r->in.selection[i]);
    int bad = routine_command(&command, r->stop);
    hp_words_free(&command);
    return bad;
}

static int prepare(struct route *r)
{
    char importer[4096], projection[4096];
    if (prepare_selection(r) || produce(r, importer))
        return -1;
    struct routine_site policy;
    if (routine_site_read(r->in.site, r->descriptor, &policy))
        return -1;
    const char *site = r->in.site;
    if (pi_same(r->mode, "windows-import")) {
        if (path(projection, r->in.out, "consumer.site") ||
            project_site(r->in.site, projection, "windows"))
            return -1;
        site = projection;
    }
    struct json *clock = NULL;
    int bad = consumer_environment(r, importer, site);
    if (!bad)
        clock = clock_read(r, "consumer-start.json");
    bad = bad || !clock || consumer_admission(r, &policy, site);
    json_free(clock);
    return bad;
}

static int direct_request(struct route *r, const struct json *request)
{
    const char *keys[] = {"purpose", "commit", "run", "arches", "accel"};
    const char *values[] = {r->in.purpose, r->in.commit, r->in.run, r->in.arches, r->in.accel};
    for (size_t i = 0; i < 5; i++)
        if (!pi_same(field(request, keys[i]), values[i]))
            return -1;
    struct json *selection = hp_value(JSON_ARRAY);
    for (int i = 0; i < r->in.count; i++)
        hp_item(selection, hp_string(r->in.selection[i]));
    int bad = !json_equal(selection, json_member(request, "selection")) ||
              !json_equal(r->preparation, json_member(request, "preparation"));
    json_free(selection);
    const char *root = field(json_member(r->config, "physical"), "linux_output_root");
    const char *out = field(request, "out");
    if (!root || !*root || !out)
        return -1;
    size_t n = strlen(root);
    while (n && root[n - 1] == '/')
        n--;
    return bad || strncmp(out, root, n) || out[n] != '/' ? -1 : 0;
}

static int direct_setup(struct route *r)
{
    if (!pi_same(r->mode, "wsl-native"))
        return -1;
    char directory[4096], original[4096], projection[4096], request_path[4096];
    if (path(directory, r->work, "freeze") || path(original, directory, "policy-before") ||
        path(projection, r->in.out, "consumer.linux.site") ||
        project_site(original, projection, "linux") ||
        path(request_path, r->in.out, "routine.request.json"))
        return -1;
    struct json *request = hp_document(request_path);
    int bad =
        !request || direct_request(r, request) || etude_routine_pack(r->in.out, NULL, r->stop);
    json_free(request);
    return bad;
}

static int restore(struct route *r)
{
    char linux[4096], windows[4096];
    if (remote(r, "facts", NULL, "facts-final", linux) || local(r, "block", NULL) ||
        facts(r, "windows-final", windows))
        return -1;
    const char *names[] = {"linux", "windows"}, *roots[] = {linux, windows};
    for (size_t i = 0; i < 2; i++) {
        char input[4096], output[4096], name[64];
        snprintf(name, sizeof name, "%s-final.claims", names[i]);
        if (empty_ledger(roots[i]) || path(input, roots[i], "ledger-empty") ||
            path(output, r->in.out, name) || routine_copy(input, output))
            return -1;
    }
    char directory[4096];
    return remote(r, "restore", NULL, NULL, directory) || local(r, "restore", NULL);
}

static int route_options(struct route *r, int argc, char **argv)
{
    const char *names[] = {"config",  "tree",   "out",  "run",   "commit",
                           "purpose", "arches", "site", "etude", "accel"};
    const char *values[10] = {0};
    int i;
    if (argc < 2)
        return -1;
    r->action = argv[1];
    for (i = 2; i < argc && strcmp(argv[i], "--"); i += 2) {
        if (strncmp(argv[i], "--", 2) || i + 1 == argc)
            return -1;
        size_t j;
        for (j = 0; j < 10 && strcmp(argv[i] + 2, names[j]); j++)
            ;
        if (j == 10 || values[j])
            return -1;
        values[j] = argv[i + 1];
    }
    if (i < argc && !strcmp(argv[i], "--"))
        i++;
    if (i == argc)
        return -1;
    for (size_t j = 0; j < 10; j++)
        if (!values[j])
            return -1;
    struct hp_words fixed = {0};
    const char *operands[] = {"routine", values[5], values[1], values[2], values[6],
                              values[3], values[7], values[8], values[9]};
    for (size_t j = 0; j < 9; j++)
        hp_add(&fixed, operands[j]);
    for (; i < argc; i++)
        hp_add(&fixed, argv[i]);
    int bad =
        routine_input_read((int)fixed.n, fixed.v, &r->in) || !pi_same(values[4], r->in.commit);
    if (!bad) {
        /* routine_input retains selection strings; keep caller-owned argv. */
        r->in.selection = argv + argc - r->in.count;
        r->in.purpose = values[5];
        r->in.arches = values[6];
        r->in.run = values[3];
        r->in.accel = values[9];
        char *descriptor = realpath(values[0], NULL);
        bad = !descriptor || strlen(descriptor) >= sizeof r->descriptor;
        if (!bad)
            strcpy(r->descriptor, descriptor);
        free(descriptor);
    }
    hp_words_free(&fixed);
    return bad;
}

static int route_config(struct route *r)
{
    if (!pi_same(r->in.purpose, "push") && !pi_same(r->in.purpose, "ci") &&
        !pi_same(r->in.purpose, "release") && !pi_same(r->in.purpose, "phase_first_run"))
        return -1;
    r->config = hp_document(r->descriptor);
    const struct json *version = json_member(r->config, "schema_version");
    r->mode = field(r->config, "mode");
    if (!pi_same(field(r->config, "format"), "etude-routine-route") || !version ||
        version->type != JSON_INTEGER || version->integer != 1 || !r->mode ||
        (!pi_same(r->mode, "windows-import") && !pi_same(r->mode, "wsl-native") &&
         !pi_same(r->mode, "macos-import") && !pi_same(r->mode, "native")))
        return -1;
    if (!pi_same(r->mode, "native")) {
        char mode[24];
        if (routine_route_mode(r->descriptor, mode) || !pi_same(mode, r->mode))
            return -1;
    }
    return 0;
}

static int route_open(struct route *r)
{
    if (route_config(r))
        return -1;
    if (routine_phase_inputs(r->in.purpose, &r->preparation) ||
        path(r->work, r->in.out, "routine-proofs-input"))
        return -1;
    if (mkdir(r->work, 0700) && errno != EEXIST)
        return -1;
    struct stat st;
    if (lstat(r->work, &st) || !S_ISDIR(st.st_mode))
        return -1;
    struct json *source = pi_source(r->in.tree, r->in.commit);
    if (!source)
        return -1;
    json_free(source);
    const char *nonce = getenv("ETUDE_AUTHORITY_NONCE");
    r->nonce = strdup(nonce ? nonce : "");
    if (!r->nonce)
        return -1;
    r->dynamic = hp_clone(r->config);
    const char *env[] = {"GITHUB_RUN_ID", "RUNNER_NAME"};
    const char *keys[] = {"github_run", "github_runner"};
    for (size_t i = 0; i < 2; i++) {
        const char *value = getenv(env[i]);
        hp_member(r->dynamic, keys[i], value && *value ? hp_string(value) : hp_value(JSON_NULL));
    }
    return 0;
}

int etude_prepare_route_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    struct route r = {.stop = stop};
    int bad = route_options(&r, argc, argv) || route_open(&r);
    if (!bad) {
        if (pi_same(r.action, "quiesce"))
            bad = quiesce(&r);
        else if (pi_same(r.action, "prepare"))
            bad = prepare(&r);
        else if (pi_same(r.action, "direct-setup"))
            bad = direct_setup(&r);
        else if (pi_same(r.action, "restore"))
            bad = restore(&r);
        else if (pi_same(r.action, "ensure")) {
            char importer[4096];
            bad = produce(&r, importer);
            if (!bad)
                printf("%s\n", importer);
        } else
            bad = -1;
    }
    json_free(r.config);
    json_free(r.dynamic);
    json_free(r.preparation);
    free((char *)r.nonce);
    if (bad)
        fputs("etude prepare route: fixed phase failed; all prior evidence retained\n", stderr);
    return bad ? 1 : 0;
}
