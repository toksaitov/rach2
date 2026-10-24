/* One physical grant is charged once, independently of endpoint clocks. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "authority_internal.h"
#include "routine_owner.h"
#include "routine_proof.h"
#include "routine_charge.h"
#ifdef _WIN32
#include "win/files.h"
#endif

static int path(const struct etude_routine_owner *r, const char *name, char out[4096])
{
    int n = snprintf(out, 4096, "%s/%s", r->directory, name);
    return n < 0 || n >= 4096 ? -1 : 0;
}

static int reference(struct jbuf *b, const struct etude_routine_owner *r, const char *name)
{
    char file[4096];
    return path(r, name, file) || routine_ref(b, file);
}

static void owner_identity(struct jbuf *b, const struct etude_pid *id)
{
    char value[128];
    snprintf(value, sizeof value, "{\"pid\":\"%ld\",\"creation\":\"%llu\"}", id->pid,
              (unsigned long long)id->start);
    jbuf_puts(b, value);
}

static void common(struct jbuf *b, const struct etude_routine_owner *r, const char *format)
{
    jbuf_puts(b, "{\"format\":");
    routine_quote(b, format);
    jbuf_puts(b, ",\"schema_version\":1,\"run_id\":");
    routine_quote(b, r->run);
    jbuf_puts(b, ",\"commit\":");
    routine_quote(b, r->commit);
    jbuf_puts(b, ",\"owner\":");
    owner_identity(b, &r->owner);
    jbuf_puts(b, ",\"machine_id\":");
    routine_quote(b, r->clock.machine_id);
    jbuf_puts(b, ",\"boot_id\":");
    routine_quote(b, r->clock.boot_id);
    jbuf_puts(b, ",\"route\":");
}

static int settings(struct etude_routine_owner *r)
{
    struct authority_binding binding;
    struct authority_file_id configured, requested;
    if (authority_binding_read(&binding) || !authority_operator(binding.operator_id) ||
        authority_file_id(binding.route, &configured) || authority_file_id(r->route, &requested) ||
        configured.device != requested.device || configured.inode != requested.inode)
        return -1;
    struct json *doc = NULL;
    if (routine_json(r->route, &doc))
        return -1;
    const char *mode = json_text(doc, "mode");
    const char *format = json_text(doc, "format");
    if (!mode || !format || strcmp(format, "etude-routine-route") ||
        (strcmp(mode, "windows-import") && strcmp(mode, "wsl-native"))) {
        json_free(doc);
        return -1;
    }
    strcpy(r->mode, mode);
    const char *policy = json_text(doc, "budget_policy");
    int rc = !policy || etude_routine_work_us(policy, r->purpose, &r->work_us);
    json_free(doc);
    return rc ? -1 : 0;
}

int routine_owner_open(struct etude_routine_owner *r, const char *route, const char *directory,
                        const char *run, const char *purpose)
{
    memset(r, 0, sizeof *r);
    r->route = route;
    r->directory = directory;
    r->run = run;
    r->purpose = purpose;
    r->commit = getenv("ETUDE_ROUTINE_COMMIT");
#ifdef _WIN32
    r->owner.pid = GetCurrentProcessId();
#else
    r->owner.pid = getpid();
#endif
    if (!authority_absolute(route) || !authority_absolute(directory) || !authority_token(run) ||
        !r->commit || strlen(r->commit) != 40 || strspn(r->commit, "0123456789abcdef") != 40 ||
        etude_process_state(r->owner.pid, &r->owner) != 1 || !r->owner.start_known ||
        etude_context_clock(&r->clock) || settings(r))
        return -1;
    char nonce[65];
    if (authority_nonce(nonce) || setenv("ETUDE_AUTHORITY_NONCE", nonce, 1) ||
        setenv("ETUDE_AUTHORITY_RUN", run, 1))
        return -1;
    char id[96];
    snprintf(id, sizeof id, "%ld:%llu", r->owner.pid, (unsigned long long)r->owner.start);
    return setenv("ETUDE_AUTHORITY_OWNER", id, 1);
}

static int physical_reading(const struct etude_routine_owner *r, const char *name,
                            struct etude_reading *start);

static int write_lease(struct etude_routine_owner *r)
{
    char inputs[4096], output[4096];
    struct json *doc = NULL;
    if (path(r, "lease.inputs.json", inputs) || path(r, "routine.lease.json", output) ||
        routine_json(inputs, &doc))
        return -1;
    const struct json *authority = json_member(doc, "authority"), *endpoints = json_member(doc, "endpoints");
    if (!authority || authority->type != JSON_OBJECT || !endpoints || endpoints->type != JSON_ARRAY ||
        endpoints->count != 2) {
        json_free(doc);
        return -1;
    }
    struct jbuf b = {0};
    common(&b, r, "etude-routine-lease");
    int error = routine_ref(&b, r->route);
    jbuf_puts(&b, ",\"grant\":");
    routine_reading(&b, &r->start);
    jbuf_puts(&b, ",\"authority\":");
    json_canonical(authority, &b);
    jbuf_puts(&b, ",\"endpoints\":");
    json_canonical(endpoints, &b);
    jbuf_puts(&b, ",\"result\":{\"exclusive\":true,\"cleanup_before_grant\":\"confirmed\"}}\n");
    if (!error)
        error = routine_create(output, &b);
    jbuf_free(&b);
    json_free(doc);
    return error;
}

int routine_owner_grant(struct etude_routine_owner *r)
{
    struct etude_authority_phase p = {.run_id = r->run, .commit = r->commit, .owner = r->owner,
                                      .nonce = getenv("ETUDE_AUTHORITY_NONCE")};
    struct etude_authority *authority = NULL;
    if (etude_authority_reopen(&p, &authority))
        return -1;
    int error = strcmp(authority->state.phase, "blocked") ||
                authority->state.owner.pid != r->owner.pid ||
                authority->state.owner.start != r->owner.start ||
                physical_reading(r, "physical-grant.json", &r->start);
    etude_authority_close(authority);
    if (error || write_lease(r))
        return -1;
    char grant[4096], lease[4096], completion[4096], pid[32], creation[32];
    if (path(r, "routine.grant.json", grant) || path(r, "routine.lease.json", lease) ||
        path(r, "routine.completion.json", completion))
        return -1;
    snprintf(pid, sizeof pid, "%ld", r->owner.pid);
    snprintf(creation, sizeof creation, "%llu", (unsigned long long)r->owner.start);
    struct etude_routine_grant_inputs inputs = {.run_id = r->run, .commit = r->commit,
        .route = r->route, .lease = lease, .completion_path = completion,
        .owner_pid = pid, .owner_creation = creation, .clock = r->clock, .start = r->start};
    if (etude_context_routine_grant_write(grant, &inputs) ||
        setenv("ETUDE_ROUTINE_GRANT", grant, 1))
        return -1;
    char locator[4096];
    if (path(r, "routine-completion.path", locator))
        return -1;
    struct jbuf text = {0};
    const char *locator_path = getenv("ETUDE_ROUTINE_COMPLETION_LOCATOR");
    jbuf_puts(&text, locator_path ? locator_path : completion);
    jbuf_puts(&text, "\n");
    int failed = routine_create(locator, &text);
    jbuf_free(&text);
    if (failed)
        return -1;
    r->granted = 1;
    return 0;
}

int routine_owner_expired(const struct etude_routine_owner *r)
{
    struct etude_reading now;
    return r->granted && (etude_history_now(&now) || now.monotonic_us < r->start.monotonic_us ||
           now.monotonic_us - r->start.monotonic_us >= r->work_us);
}

int routine_owner_prepared(struct etude_routine_owner *r)
{
    return etude_history_now(&r->preparation_end);
}

int routine_owner_direct_prepared(struct etude_routine_owner *r)
{
    if (strcmp(r->mode, "wsl-native") || r->direct_prepared)
        return 0;
    char request[4096], phase[4096], ack[4096];
    if (path(r, "routine.prepared.request.json", request) ||
        path(r, "routine.preparation.json", phase) || path(r, "routine.prepared.ack", ack))
        return -1;
    if (access(request, F_OK))
        return errno == ENOENT ? 0 : -1;
    struct json *ref = NULL, *actual = NULL, *document = NULL;
    struct jbuf b = {0};
    int error = routine_json(request, &ref) || routine_ref(&b, phase);
    char reason[128];
    if (!error)
        error = json_parse(b.data, b.len, 1, &actual, reason, sizeof reason);
    if (!error)
        error = !json_equal(json_member(ref, "sha256"), json_member(actual, "sha256")) ||
                !json_equal(json_member(ref, "size_bytes"), json_member(actual, "size_bytes")) ||
                routine_json(phase, &document);
    if (!error) {
        const char *run = json_text(document, "run_id"), *commit = json_text(document, "commit");
        const struct json *result = json_member(document, "result");
        const char *end = json_text(result, "end"), *cleanup = json_text(result, "cleanup");
        const struct json *value = json_member(result, "value");
        error = !run || strcmp(run, r->run) || !commit || strcmp(commit, r->commit) ||
                !end || strcmp(end, "exited") || !cleanup || strcmp(cleanup, "confirmed") ||
                !value || value->type != JSON_INTEGER || value->integer;
    }
    if (!error)
        error = etude_history_now(&r->preparation_end);
    if (!error) {
        jbuf_free(&b);
        jbuf_puts(&b, r->run);
        jbuf_puts(&b, "\n");
        error = routine_create(ack, &b);
        r->direct_prepared = !error;
    }
    json_free(ref);
    json_free(actual);
    json_free(document);
    jbuf_free(&b);
    return error ? -1 : 0;
}

int routine_owner_observed(struct etude_routine_owner *r)
{
    return etude_history_now(&r->observation_end);
}

static int phase_result(struct jbuf *b, const struct etude_exec_result *result)
{
    char numbers[384];
    snprintf(numbers, sizeof numbers,
              "{\"end\":\"%s\",\"value\":%d,\"exit_end\":\"%s\",\"exit_value\":%d,"
              "\"cleanup\":\"%s\",\"output_error\":%d,\"active\":0}",
              result->end == ETUDE_END_EXITED ? "exited" : "cancelled", result->value,
              result->exit_end == ETUDE_END_EXITED ? "exited" : "lost", result->exit_value,
              result->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed",
              result->output_error);
    jbuf_puts(b, numbers);
    return result->cleanup != ETUDE_CLEANUP_CONFIRMED;
}

static int physical_reading(const struct etude_routine_owner *r, const char *name,
                            struct etude_reading *start)
{
    char file[4096];
    struct json *doc = NULL;
    if (path(r, name, file) || routine_json(file, &doc))
        return -1;
    const struct json *reading = json_member(doc, "reading");
    const char *machine = json_text(doc, "machine_id"), *boot = json_text(doc, "boot_id");
    const char *utc = json_text(reading, "utc");
    const struct json *mono = json_member(reading, "monotonic_us"),
                      *resolution = json_member(reading, "resolution_us");
    int error = !machine || strcmp(machine, r->clock.machine_id) || !boot ||
        strcmp(boot, r->clock.boot_id) || !utc || strlen(utc) >= sizeof start->utc ||
        !mono || mono->type != JSON_INTEGER || mono->integer < 0 ||
        !resolution || resolution->type != JSON_INTEGER || resolution->integer < 1;
    if (!error) {
        strcpy(start->utc, utc);
        start->monotonic_us = (uint64_t)mono->integer;
        start->resolution_us = (uint64_t)resolution->integer;
    }
    json_free(doc);
    return error ? -1 : 0;
}

int routine_owner_phase(struct etude_routine_owner *r, const char *name,
                         const struct etude_exec_result *result, const struct etude_reading *start)
{
    if (strcmp(name, "preparation") && strcmp(name, "collection"))
        return 0;
    struct etude_reading end, actual_start = *start;
    if (etude_history_now(&end) || (!strcmp(name, "preparation") &&
        physical_reading(r, "routine-proofs-input/consumer-start.json", &actual_start)))
        return -1;
    start = &actual_start;
    if (!strcmp(name, "collection")) {
        struct authority_binding binding;
        char claims[4096], retained[4096];
        if (authority_binding_read(&binding) || path(r, "windows-collection.claims", retained))
            return -1;
        int n = snprintf(claims, sizeof claims, "%s/claims", binding.ledger);
        if (n < 0 || (size_t)n >= sizeof claims || routine_copy(claims, retained))
            return -1;
    }
    char basename[96], raw[4096], document[4096];
    snprintf(basename, sizeof basename, "routine.%s.result.json", name);
    if (path(r, basename, raw))
        return -1;
    snprintf(basename, sizeof basename, "routine.%s.json", name);
    if (path(r, basename, document))
        return -1;
    struct jbuf b = {0};
    jbuf_puts(&b, "{\"format\":\"etude-routine-child\",\"schema_version\":1,\"process\":");
    owner_identity(&b, &result->process);
    jbuf_puts(&b, ",\"owner\":");
    owner_identity(&b, &r->owner);
    jbuf_puts(&b, ",\"machine_id\":");
    routine_quote(&b, r->clock.machine_id);
    jbuf_puts(&b, ",\"boot_id\":");
    routine_quote(&b, r->clock.boot_id);
    jbuf_puts(&b, ",\"start\":");
    routine_reading(&b, start);
    jbuf_puts(&b, ",\"end\":");
    routine_reading(&b, &end);
    jbuf_puts(&b, ",\"result\":");
    int error = phase_result(&b, result);
    jbuf_puts(&b, "}\n");
    if (!error)
        error = routine_create(raw, &b);
    jbuf_free(&b);
    common(&b, r, "etude-routine-phase");
    error |= routine_ref(&b, r->route);
    jbuf_puts(&b, ",\"lease\":");
    error |= reference(&b, r, "routine.lease.json");
    jbuf_puts(&b, ",\"phase\":");
    routine_quote(&b, name);
    jbuf_puts(&b, ",\"child\":");
    owner_identity(&b, &result->process);
    jbuf_puts(&b, ",\"start\":");
    routine_reading(&b, start);
    jbuf_puts(&b, ",\"end\":");
    routine_reading(&b, &end);
    jbuf_puts(&b, ",\"admission\":");
    error |= reference(&b, r, "preparation.admit.result");
    jbuf_puts(&b, ",\"receipt\":");
    error |= routine_ref(&b, raw);
    jbuf_puts(&b, ",\"result\":");
    error |= phase_result(&b, result);
    jbuf_puts(&b, "}\n");
    if (!error)
        error = routine_create(document, &b);
    jbuf_free(&b);
    if (!strcmp(name, "collection"))
        r->collector = result->process;
    return error;
}

int routine_owner_complete(struct etude_routine_owner *r, int status, int confirmed)
{
    if (!r->granted || !confirmed)
        return -1;
    struct etude_reading end;
    if (etude_history_now(&end))
        return -1;
    if (!strcmp(r->mode, "wsl-native")) {
        char file[4096];
        struct json *phase = NULL;
        if (!r->direct_prepared || path(r, "routine.collection.json", file) || routine_json(file, &phase))
            return -1;
        const struct json *child = json_member(phase, "child");
        const char *pid = json_text(child, "pid"), *creation = json_text(child, "creation");
        char *last;
        if (!pid || !creation) {
            json_free(phase);
            return -1;
        }
        errno = 0;
        r->collector.pid = strtol(pid, &last, 10);
        int error = errno || *last || r->collector.pid <= 1;
        r->collector.start = strtoull(creation, &last, 10);
        error |= errno || *last || !r->collector.start;
        r->collector.start_known = 1;
        json_free(phase);
        if (error)
            return -1;
    }
    struct jbuf b = {0};
    common(&b, r, "etude-routine-completion");
    int error = routine_ref(&b, r->route);
    jbuf_puts(&b, ",\"routine_grant\":");
    error |= reference(&b, r, "routine.grant.json");
    jbuf_puts(&b, ",\"lease\":");
    error |= reference(&b, r, "routine.lease.json");
    char completion[4096];
    if (path(r, "routine.completion.json", completion))
        error = 1;
    jbuf_puts(&b, ",\"completion_path\":");
    routine_quote(&b, completion);
    jbuf_puts(&b, ",\"collector\":");
    owner_identity(&b, &r->collector);
    jbuf_puts(&b, ",\"preparation_end\":");
    routine_reading(&b, &r->preparation_end);
    jbuf_puts(&b, ",\"observation_end\":");
    routine_reading(&b, &r->observation_end);
    jbuf_puts(&b, ",\"end\":");
    routine_reading(&b, &end);
    jbuf_puts(&b, ",\"history\":");
    char history[4096];
    int hn = snprintf(history, sizeof history, "%s/history/%s.jsonl", r->directory, r->run);
    error |= hn < 0 || (size_t)hn >= sizeof history || routine_ref(&b, history);
    jbuf_puts(&b, ",\"phase_cleanup\":[");
    if (!strcmp(r->mode, "windows-import")) {
        error |= reference(&b, r, "routine.producer.json");
        jbuf_puts(&b, ",");
    }
    error |= reference(&b, r, "routine.preparation.json");
    jbuf_puts(&b, ",");
    error |= reference(&b, r, "routine.collection.json");
    jbuf_puts(&b, "],\"observers\":[");
    error |= routine_ref(&b, r->trace);
    jbuf_puts(&b, "],\"ledgers\":[{\"endpoint\":\"linux\",\"claims\":");
    error |= reference(&b, r, "linux-final.claims");
    jbuf_puts(&b, "},{\"endpoint\":\"windows\",\"claims\":");
    error |= reference(&b, r, "windows-final.claims");
    jbuf_puts(&b, status ? "}],\"result\":{\"outcome\":\"fail\",\"cleanup\":\"confirmed\"}}\n" :
                         "}],\"result\":{\"outcome\":\"pass\",\"cleanup\":\"confirmed\"}}\n");
    if (!error)
        error = routine_create(completion, &b);
    jbuf_free(&b);
    return error;
}
