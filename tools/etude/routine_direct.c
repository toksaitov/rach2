/* The Linux clock and identities remain separate from the physical clock. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "routine_direct.h"
#include "routine_proof.h"
#include "measurement_context.h"
#ifdef _WIN32
#include "win/files.h"
#endif

static int direct_path(char out[4096], const char *name)
{
    const char *directory = getenv("ETUDE_ROUTINE_DIRECTORY");
    if (!directory)
        return -1;
    int n = snprintf(out, 4096, "%s/%s", directory, name);
    return n < 0 || n >= 4096 ? -1 : 0;
}

static void identity(struct jbuf *b, const struct etude_pid *id)
{
    char text[160];
    snprintf(text, sizeof text, "{\"pid\":\"%ld\",\"creation\":\"%llu\"}", id->pid,
             (unsigned long long)id->start);
    jbuf_puts(b, text);
}

static void result_json(struct jbuf *b, const struct etude_exec_result *r)
{
    char text[384];
    snprintf(text, sizeof text,
             "{\"end\":\"%s\",\"value\":%d,\"exit_end\":\"%s\",\"exit_value\":%d,"
             "\"cleanup\":\"%s\",\"output_error\":%d,\"active\":0}",
             r->end == ETUDE_END_EXITED ? "exited" : "cancelled", r->value,
             r->exit_end == ETUDE_END_EXITED ? "exited" : "lost", r->exit_value,
             r->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed", r->output_error);
    jbuf_puts(b, text);
}

static int phase_document(const char *phase, const struct etude_pid *owner,
                          const struct etude_exec_result *r, const struct etude_reading *start,
                          const struct json *grant)
{
    char basename[128], raw[4096], output[4096], receipt[4096];
    snprintf(basename, sizeof basename, "routine.%s.result.json", phase);
    if (direct_path(raw, basename))
        return -1;
    snprintf(basename, sizeof basename, "routine.%s.json", phase);
    if (direct_path(output, basename) || direct_path(receipt, "preparation.admit.result"))
        return -1;
    struct etude_context_clock clock;
    struct etude_reading end;
    if (etude_context_clock(&clock) || etude_history_now(&end))
        return -1;
    struct jbuf b = {0};
    jbuf_puts(&b, "{\"format\":\"etude-routine-child\",\"schema_version\":1,\"owner\":");
    identity(&b, owner);
    jbuf_puts(&b, ",\"process\":");
    identity(&b, &r->process);
    jbuf_puts(&b, ",\"machine_id\":");
    routine_quote(&b, clock.machine_id);
    jbuf_puts(&b, ",\"boot_id\":");
    routine_quote(&b, clock.boot_id);
    jbuf_puts(&b, ",\"start\":");
    routine_reading(&b, start);
    jbuf_puts(&b, ",\"end\":");
    routine_reading(&b, &end);
    jbuf_puts(&b, ",\"result\":");
    result_json(&b, r);
    jbuf_puts(&b, "}\n");
    int error = routine_create(raw, &b);
    jbuf_free(&b);
    jbuf_puts(&b, "{\"format\":\"etude-routine-phase\",\"schema_version\":1");
    const char *fields[] = {"run_id", "commit", "route", "lease", "owner"};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        jbuf_puts(&b, ",");
        routine_quote(&b, fields[i]);
        jbuf_puts(&b, ":");
        const struct json *value = json_member(grant, fields[i]);
        if (!value) {
            jbuf_free(&b);
            return -1;
        }
        json_canonical(value, &b);
    }
    jbuf_puts(&b, ",\"phase\":");
    routine_quote(&b, phase);
    jbuf_puts(&b, ",\"machine_id\":");
    routine_quote(&b, clock.machine_id);
    jbuf_puts(&b, ",\"boot_id\":");
    routine_quote(&b, clock.boot_id);
    jbuf_puts(&b, ",\"child\":");
    identity(&b, &r->process);
    jbuf_puts(&b, ",\"start\":");
    routine_reading(&b, start);
    jbuf_puts(&b, ",\"end\":");
    routine_reading(&b, &end);
    jbuf_puts(&b, ",\"admission\":");
    /* The normal Linux preparation receipt proves its actual granted work;
     * the collection proves its own per-case claims in finalized history. */
    error |= routine_ref(&b, receipt);
    jbuf_puts(&b, ",\"receipt\":");
    error |= routine_ref(&b, raw);
    jbuf_puts(&b, ",\"result\":");
    result_json(&b, r);
    jbuf_puts(&b, "}\n");
    if (!error)
        error = routine_create(output, &b);
    jbuf_free(&b);
    return error;
}

int routine_direct_phase(const char *phase, const struct etude_pid *owner,
                         const struct etude_exec_result *r, const struct etude_reading *start)
{
    const char *grant_path = getenv("ETUDE_ROUTINE_GRANT");
    if (!getenv("ETUDE_ROUTINE_DIRECT"))
        return 0;
    struct json *grant = NULL;
    if (!grant_path || routine_json(grant_path, &grant))
        return -1;
    int error = phase_document(phase, owner, r, start, grant);
    json_free(grant);
    if (!error && !strcmp(phase, "preparation")) {
        char request[4096], proof[4096];
        struct jbuf b = {0};
        error = direct_path(request, "routine.prepared.request.json") ||
                direct_path(proof, "routine.preparation.json");
        if (!error)
            error = routine_ref(&b, proof) || routine_create(request, &b);
        jbuf_free(&b);
    }
    return error;
}

int routine_direct_ack(void)
{
    if (!getenv("ETUDE_ROUTINE_DIRECT"))
        return 1;
    const char *run = getenv("ETUDE_ROUTINE_RUN");
    char name[4096], text[257];
    if (!run || direct_path(name, "routine.prepared.ack"))
        return -1;
    FILE *file = fopen(name, "rb");
    if (!file)
        return errno == ENOENT ? 0 : -1;
    size_t n = fread(text, 1, sizeof text, file);
    int ok =
        !ferror(file) && n == strlen(run) + 1 && !memcmp(text, run, n - 1) && text[n - 1] == '\n';
    if (fclose(file))
        ok = 0;
    return ok ? 1 : -1;
}
