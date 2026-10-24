/* Focused context/history rows of the set-driver fixture; no QEMU needed. */
#include "measurement_context.h"
#include "etude/observe.h"
#include "history_json.h"

static const char measurement_commit[] = "1111111111111111111111111111111111111111";

static void measurement_quote(struct jbuf *b, const char *value)
{
    struct json v;
    memset(&v, 0, sizeof v);
    v.type = JSON_STRING;
    v.string = (char *)value;
    json_canonical(&v, b);
}
static void measurement_hash(const char *text, char hash[65])
{
    struct sha256 sha;
    unsigned char digest[32];
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)text, strlen(text));
    sha256_final(&sha, digest);
    sha256_hex(digest, hash);
}
static void measurement_descriptor(struct jbuf *b, const char *path, const char *name)
{
    struct stat st;
    char hash[65], size[100];
    CHECK(!etude_digest_file(NULL, path, &st, hash), "context: input hashes");
    jbuf_puts(b, "{\"path\":");
    measurement_quote(b, name ? name : path);
    snprintf(size, sizeof size, ",\"sha256\":\"%s\",\"size_bytes\":%llu}", hash,
             (unsigned long long)st.st_size);
    jbuf_puts(b, size);
}
static void measurement_file(const char *path, struct jbuf *b)
{
    CHECK(!write_text(path, b->data, 0644), "context: snapshot writes");
    jbuf_free(b);
}
static struct eh_json *measurement_record(const struct run *r, const char *kind)
{
    for (size_t i = 0; i < r->h.n; i++) {
        struct eh_json *doc = eh_json_parse(r->h.line[i]);
        const struct eh_json *v = eh_json_get(doc, "kind");
        if (v && v->type == 's' && !strcmp(v->string, kind))
            return doc;
        eh_json_free(doc);
    }
    return NULL;
}
static struct eh_json *measurement_blob(const struct eh_json *blob)
{
    char text[32768];
    const struct eh_json *locations = eh_json_get(blob, "locations");
    const struct eh_json *path = locations ? locations->child : NULL;
    if (!path || path->type != 's' || read_text(path->string, text, sizeof text) < 0)
        return NULL;
    return eh_json_parse(text);
}
static uint64_t measurement_number(const struct eh_json *doc, const char *key)
{
    const struct eh_json *n = eh_json_get(doc, key);
    return n && n->type == 'n' && n->number >= 0 ? (uint64_t)n->number : UINT64_MAX;
}
static void measurement_bound(const struct eh_json *blob, const char *text)
{
    char hash[65];
    measurement_hash(text, hash);
    const struct eh_json *sum = eh_json_get(blob, "sha256");
    CHECK(sum && sum->type == 's' && !strcmp(sum->string, hash),
          "context: plan binds snapshot bytes");
}
static void measurement_record_file(const char *path)
{
    char program[PATH_LEN], hash[65];
    struct stat st;
    in(program, sizeof program, "build/cases/probe");
    CHECK(!etude_digest_file(NULL, program, &st, hash), "context: prepared program hashes");
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "context: producer record opens");
    if (f) {
        fprintf(f,
                "etude-prepare 1\ntree %s\narch i686\nkernel build/rach2/kernel\n"
                "file %s %llu %llu 0 build/cases/probe\n",
                tree, hash, (unsigned long long)st.st_size, (unsigned long long)st.st_ino);
        CHECK(!fclose(f), "context: producer record closes");
    }
}
static void measurement_paths(struct run *r, const char *name)
{
    r->name = name;
    fix_format(r->results, sizeof r->results, "%s/%s.results", fix_dir(), name);
    fix_format(r->evidence, sizeof r->evidence, "%s/%s.evidence", fix_dir(), name);
    fix_format(r->status_file, sizeof r->status_file, "%s/%s.status", fix_dir(), name);
    fix_format(r->out_file, sizeof r->out_file, "%s/%s.out", fix_dir(), name);
}
static void measurement_snapshots(void)
{
    struct jbuf b = {0};
    char path[PATH_LEN];
    jbuf_puts(&b, "{\"schema_version\":1,\"commit\":");
    measurement_quote(&b, measurement_commit);
    jbuf_puts(&b, ",\"inputs\":[");
    measurement_descriptor(&b, in(path, sizeof path, ONE("plain")), ONE("plain"));
    jbuf_puts(&b, "]}");
    measurement_file(fix_path("measurement-source.json"), &b);
    jbuf_puts(&b, "{\"schema_version\":1,\"inputs\":[");
    measurement_descriptor(&b, self, NULL);
    jbuf_puts(&b, "]}");
    measurement_file(fix_path("measurement-tools.json"), &b);
    jbuf_puts(&b, "{\"schema_version\":1,\"policy\":");
    measurement_descriptor(&b, in(path, sizeof path, "site.permits"), NULL);
    jbuf_puts(&b, "}");
    measurement_file(fix_path("measurement-site.json"), &b);
}
/* These synthetic cache inputs exercise binding, not actual cache residency. */
static void measurement_cache_states(struct jbuf *b, int known)
{
    static const char *const names[] = {"artifacts", "build", "tools", "os"};
    jbuf_puts(b, "{");
    for (size_t i = 0; i < 4; i++) {
        if (i)
            jbuf_puts(b, ",");
        measurement_quote(b, names[i]);
        jbuf_puts(b, ":{\"state\":");
        measurement_quote(b, known ? "cold" : "unknown");
        jbuf_puts(b, ",\"procedure\":\"synthetic context fixture\"}");
    }
    jbuf_puts(b, "}");
}
static void measurement_cache(struct jbuf *context, const struct etude_preparation_receipt *receipt,
                              int known)
{
    struct jbuf states = {0}, b = {0};
    measurement_cache_states(&states, known);
    jbuf_puts(&b, known ? "{\"schema_version\":1,\"controlled\":true"
                        : "{\"schema_version\":1,\"controlled\":false");
    jbuf_puts(&b, ",\"method\":\"synthetic context fixture\",\"states\":");
    jbuf_puts(&b, states.data);
    jbuf_puts(&b, "}");
    measurement_file(fix_path("measurement-cache.json"), &b);
    if (known) {
        /* The witness is actual fixture receipt bytes with their exact hash. */
        char path[PATH_LEN], hash[65], size[100];
        struct stat st;
        fix_format(path, sizeof path, "%s/%s.receipt", fix_dir(), receipt->run_id);
        CHECK(!etude_digest_file(NULL, path, &st, hash), "context: conditioning witness hashes");
        jbuf_puts(&b, "{\"format\":\"etude-cache-protocol\",\"schema_version\":1,\"run_id\":");
        measurement_quote(&b, receipt->run_id);
        jbuf_puts(&b, ",\"commit\":");
        measurement_quote(&b, receipt->commit);
        jbuf_puts(&b, ",\"machine_id\":");
        measurement_quote(&b, receipt->clock.machine_id);
        jbuf_puts(&b, ",\"boot_id\":");
        measurement_quote(&b, receipt->clock.boot_id);
        jbuf_puts(&b, ",\"states\":");
        jbuf_puts(&b, states.data);
        jbuf_puts(&b, ",\"evidence\":[{\"sha256\":");
        measurement_quote(&b, hash);
        fix_format(size, sizeof size, ",\"size_bytes\":%llu,\"locations\":[",
                   (unsigned long long)st.st_size);
        jbuf_puts(&b, size);
        measurement_quote(&b, path);
        jbuf_puts(&b, "]}]}");
        measurement_file(fix_path("measurement-cache-evidence.json"), &b);
        jbuf_puts(context, ",\"cache_evidence\":");
        measurement_descriptor(context, fix_path("measurement-cache-evidence.json"), NULL);
    }
    jbuf_free(&states);
}

static int plan_suite_architectures(const struct run *r, const char *name)
{
    struct eh_json *plan = measurement_record(r, "plan");
    const struct eh_json *data = eh_json_get(plan, "data");
    struct eh_json *selection = measurement_blob(eh_json_get(data, "selection"));
    const struct eh_json *suites = eh_json_get(selection, "suites");
    int count = 0, mask = 0;
    for (const struct eh_json *suite = suites ? suites->child : NULL; suite; suite = suite->next) {
        const struct eh_json *s = eh_json_get(suite, "suite"),
                             *a = eh_json_get(suite, "architecture");
        if (!s || s->type != 's' || strcmp(s->string, name))
            continue;
        count++;
        if (a && a->type == 's')
            mask |= !strcmp(a->string, "i686") ? 1 : !strcmp(a->string, "x86_64") ? 2 : 0;
    }
    eh_json_free(selection);
    eh_json_free(plan);
    return count == 2 && mask == 3;
}

static void measurement_exemption(struct jbuf *b, const struct etude_legacy_set_opts *o,
                                  const char *id, int bad)
{
    char path[PATH_LEN], text[16384], hash[65], meta[180];
    fix_format(path, sizeof path, "%s/%s.selection-export", fix_dir(), id);
    CHECK(!etude_legacy_selection_export(o, path), "context: actual selection exports");
    CHECK(read_text(path, text, sizeof text) > 0, "context: selection export reads");
    struct json *export = NULL;
    char error[256];
    CHECK(!json_parse(text, strlen(text), 1, &export, error, sizeof error),
          "context: export parses");
    const char *bytes = json_text(export, "selection_bytes");
    CHECK(bytes != NULL, "context: exact selection bytes exist");
    if (!bytes) {
        json_free(export);
        jbuf_puts(b, "null");
        return;
    }
    measurement_hash(bytes, hash);
    fix_format(path, sizeof path, "%s/%s.selection.json", fix_dir(), id);
    CHECK(!write_text(path, bytes, 0644), "context: exact selection bytes are retained");
    if (bad == 1)
        hash[0] = hash[0] == '0' ? '1' : '0';
    jbuf_puts(b, "{\"reason\":\"milestone-62-first-runs\",\"phase_issue\":589,\"manifest\":{");
    snprintf(meta, sizeof meta, "\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[", hash,
             (unsigned long)strlen(bytes));
    jbuf_puts(b, meta);
    measurement_quote(b, path);
    jbuf_puts(b, "]},\"run_ids\":[");
    measurement_quote(b, bad == 2 ? "unrelated-run" : id);
    if (bad == 5)
        jbuf_puts(b, ",\"unrelated-extra-run\"");
    jbuf_puts(b, "],\"attempt_ids\":[");
    if (bad != 3) {
        char attempt[256];
        fix_format(attempt, sizeof attempt, "%s:fixture.plain.one:i686:r0:a0", id);
        measurement_quote(b, attempt);
        if (bad == 4)
            jbuf_puts(b, ",\"unrelated-extra-attempt\"");
    } else
        jbuf_puts(b, "\"unrelated-attempt\"");
    jbuf_puts(b, "]}");
    json_free(export);
}
/* A closed fixture trace is valid evidence even without system CPU counters.
 * Its samples also end before this run, so it cannot be a timing baseline. */
static void measurement_unavailable(struct jbuf *context, const char *id,
                                    const struct etude_preparation_receipt *receipt)
{
    struct jbuf trace = {0};
    char path[PATH_LEN], stop[PATH_LEN], hash[65], fields[1024];
#ifdef _WIN32
    const char *platform = "windows-native-counters-v1";
#elif defined(__APPLE__)
    const char *platform = "macos-mach-libproc-v1";
#else
    const char *platform = "linux-proc-counters-v1";
#endif
    jbuf_puts(&trace, "{\"kind\":\"start\",\"schema_version\":1,"
                      "\"method\":\"native-load-v1\",\"platform\":");
    measurement_quote(&trace, platform);
    jbuf_puts(&trace, ",\"run_id\":");
    measurement_quote(&trace, id);
    jbuf_puts(&trace, ",\"machine_id\":");
    measurement_quote(&trace, receipt->clock.machine_id);
    jbuf_puts(&trace, ",\"boot_id\":");
    measurement_quote(&trace, receipt->clock.boot_id);
    jbuf_puts(&trace, ",\"root_pid\":1,\"root_creation\":\"1\","
                      "\"observer_pid\":2,\"observer_creation\":\"2\","
                      "\"requested_interval_us\":1000000,\"process_limit\":4096}\n");
    measurement_hash(trace.data, hash);
    uint64_t utc = etude_observe_utc(receipt->end.utc);
    for (int i = 0; i < 2; i++) {
        snprintf(fields, sizeof fields,
                 "{\"kind\":\"sample\",\"monotonic_us\":%llu,\"utc_us\":%llu,"
                 "\"system_busy_cpu_us\":null,\"outside_cpu_lower_us\":0,"
                 "\"shared_virtualizer_cpu_lower_us\":0,\"processes\":0,\"denied\":1,"
                 "\"lost\":0,\"unclassified\":0,\"overflow\":false,"
                 "\"identities_sha256\":\"%064d\"}\n",
                 (unsigned long long)receipt->end.monotonic_us, (unsigned long long)utc, 0);
        jbuf_puts(&trace, fields);
    }
    snprintf(fields, sizeof fields,
             "{\"kind\":\"end\",\"utc_us\":%llu,\"monotonic_us\":%llu,"
             "\"samples\":2,\"complete\":true,\"owner_lost\":false}\n",
             (unsigned long long)utc, (unsigned long long)receipt->end.monotonic_us);
    jbuf_puts(&trace, fields);
    fix_format(path, sizeof path, "%s/%s.outer-trace", fix_dir(), id);
    fix_format(stop, sizeof stop, "%s/%s.outer-stop", fix_dir(), id);
    measurement_file(path, &trace);
    jbuf_puts(context, ",\"outer_observation\":{\"trace\":");
    measurement_quote(context, path);
    jbuf_puts(context, ",\"stop_request\":");
    measurement_quote(context, stop);
    jbuf_puts(context, ",\"header_sha256\":");
    measurement_quote(context, hash);
    jbuf_puts(context, "}");
}
static struct etude_measurement_context *
measurement_context(struct etude_legacy_set_opts *o, const char *id, int first, int bad,
                    struct etude_preparation_receipt *receipt)
{
    char path[PATH_LEN], hash[65], error[256];
    struct stat st;
    struct jbuf b = {0};
    memset(receipt, 0, sizeof *receipt);
    strcpy(receipt->run_id, id);
    strcpy(receipt->commit, measurement_commit);
    CHECK(!etude_context_clock(&receipt->clock) && !etude_history_now(&receipt->start),
          "context: preparation starts in the shared clock");
    CHECK(!etude_digest_file(NULL, o->record, &st, hash), "context: producer record hashes");
    strcpy(receipt->record_sha256, hash);
    CHECK(!etude_history_now(&receipt->end), "context: preparation ends");
    receipt->passed = receipt->cleanup_confirmed = 1;
    fix_format(path, sizeof path, "%s/%s.receipt", fix_dir(), id);
    CHECK(!etude_context_receipt_write(path, receipt), "context: preparation receipt writes");
    jbuf_puts(&b, "{\"format\":\"etude-measurement-context\",\"schema_version\":1,\"run_id\":");
    measurement_quote(&b, id);
    jbuf_puts(&b, ",\"commit\":");
    measurement_quote(&b, measurement_commit);
    jbuf_puts(&b, first ? ",\"purpose\":\"phase_first_run\"" : ",\"purpose\":\"ci\"");
    measurement_cache(&b, receipt, bad == -1);
    const char *names[] = {"source", "tools", "site_profile", "cache_state"};
    const char *files[] = {"measurement-source.json", "measurement-tools.json",
                           "measurement-site.json", "measurement-cache.json"};
    for (size_t i = 0; i < 4; i++) {
        jbuf_puts(&b, ",");
        measurement_quote(&b, names[i]);
        jbuf_puts(&b, ":");
        measurement_descriptor(&b, fix_path(files[i]), NULL);
    }
    jbuf_puts(&b, ",\"artifacts\":");
    measurement_descriptor(&b, o->record, NULL);
    jbuf_puts(&b, ",\"preparation\":");
    measurement_descriptor(&b, path, NULL);
    jbuf_puts(&b, ",\"exemption\":");
    if (first)
        measurement_exemption(&b, o, id, bad);
    else
        jbuf_puts(&b, "null");
    if (bad < 0)
        measurement_unavailable(&b, id, receipt);
    jbuf_puts(&b, "}");
    fix_format(path, sizeof path, "%s/%s.context", fix_dir(), id);
    measurement_file(path, &b);
    struct etude_measurement_context *ctx = NULL;
    const struct etude_context_inputs inputs = {tree, o->record, o->site, measurement_commit, self};
    CHECK(!etude_context_open(path, &inputs, &ctx, error, sizeof error),
          "context: bound context opens");
    return ctx;
}
static void measurement_unavailable_history(const struct run *r, const struct eh_json *load,
                                            int known_cache)
{
    const struct eh_json *eligible = eh_json_get(load, "context_eligible");
    const struct eh_json *outer = eh_json_get(load, "outer");
    const struct eh_json *summary = eh_json_get(outer, "summary");
    const struct eh_json *cpu = eh_json_get(eh_json_get(summary, "first"), "system_busy_cpu_us");
    const struct eh_json *ok = eh_json_get(summary, "comparison_available");
    CHECK(eligible && eligible->type == (known_cache ? 't' : 'f'),
          "context: only a complete cache protocol permits context eligibility");
    CHECK(ok && ok->type == 'f' && cpu && cpu->type == 'z',
          "context: unavailable CPU measurements never permit comparison");
    char path[PATH_LEN], bytes[8192] = {0};
    fix_format(path, sizeof path, "%s/%s.outer-trace", fix_dir(), r->name);
    CHECK(read_text(path, bytes, sizeof bytes) > 0, "context: unavailable trace reads");
    measurement_bound(eh_json_get(outer, "trace"), bytes);
    const struct eh_json *locations = eh_json_get(eh_json_get(outer, "trace"), "locations");
    const struct eh_json *kept = locations ? locations->child : NULL;
    char retained[8192];
    CHECK(kept && kept->type == 's' && read_text(kept->string, retained, sizeof retained) > 0 &&
              !strcmp(retained, bytes),
          "context: raw unavailable trace is retained byte for byte");
    fix_format(path, sizeof path, "%s/%s.outer-stop", fix_dir(), r->name);
    CHECK(exists(path), "context: completed observation still receives its stop request");
}
static void measurement_history(struct run *r, const struct etude_measurement_context *ctx,
                                const struct etude_preparation_receipt *receipt, int first,
                                int unavailable, int known_cache)
{
    struct eh_json *plan = measurement_record(r, "plan"), *run = measurement_record(r, "run");
    const struct eh_json *p = eh_json_get(plan, "data"), *data = eh_json_get(run, "data");
    CHECK(plan && run, "context: plan and run records exist");
    const struct eh_json *purpose = eh_json_get(p, "purpose");
    CHECK(purpose && purpose->type == 's' &&
              !strcmp(purpose->string, first ? "phase_first_run"
                                       : ctx ? "ci"
                                             : "focused"),
          "context: requested purpose reaches the plan");
    if (ctx) {
        const char *keys[] = {"source", "tools", "site_profile", "cache_state"};
        for (size_t i = 0; i < 4; i++) {
            const struct eh_json *parent = i ? eh_json_get(p, "environment") : p;
            measurement_bound(eh_json_get(parent, keys[i]), etude_context_text(ctx, keys[i]));
        }
        const struct eh_json *a = eh_json_get(p, "artifacts");
        const struct eh_json *sha = a && a->child ? eh_json_get(a->child, "sha256") : NULL;
        CHECK(sha && sha->type == 's' && !strcmp(sha->string, receipt->record_sha256),
              "context: plan binds the actual artifact producer record");
    }
    const struct eh_json *time = eh_json_get(data, "time"), *budget = eh_json_get(data, "budget");
    const struct eh_json *charge = eh_json_get(budget, "charged_us");
    struct eh_json *load = measurement_blob(eh_json_get(data, "load"));
    const struct eh_json *available = eh_json_get(load, "comparison_available");
    CHECK(available && available->type == 'f',
          "context: unavailable outer observation is not a baseline");
    if (unavailable)
        measurement_unavailable_history(r, load, known_cache);
    if (ctx) {
        uint64_t start = measurement_number(time, "start_us"),
                 end = measurement_number(time, "end_us");
        CHECK(start == receipt->start.monotonic_us && end >= receipt->end.monotonic_us &&
                  measurement_number(charge, "value") == end - start,
              "context: charge is contiguous preparation start through final cleanup");
        struct eh_json *step = measurement_record(r, "step");
        const struct eh_json *step_data = eh_json_get(step, "data");
        CHECK(measurement_number(eh_json_get(step_data, "time"), "start_us") == start,
              "context: preparation step begins at the actual receipt start");
        eh_json_free(step);
        struct eh_json *attempt = measurement_record(r, "attempt");
        const struct eh_json *a = eh_json_get(attempt, "data");
        CHECK(end >= measurement_number(eh_json_get(a, "time"), "end_us"),
              "context: run end follows its owned attempt cleanup");
        eh_json_free(attempt);
        const struct eh_json *proof = eh_json_get(load, "preparation");
        const struct eh_json *proof_id = eh_json_get(proof, "run_id");
        CHECK(proof_id && proof_id->type == 's' && !strcmp(proof_id->string, receipt->run_id) &&
                  measurement_number(eh_json_get(proof, "start"), "monotonic_us") == start,
              "context: full preparation proof is retained with the run");
        const struct eh_json *verdict = eh_json_get(budget, "verdict");
        CHECK(verdict && verdict->type == 's' &&
                  !strcmp(verdict->string, first ? "exempt" : "pass"),
              "context: exact first-run exemption and routine CI budget differ");
    } else {
        const struct eh_json *value = eh_json_get(charge, "value");
        CHECK(value && value->type == 'z',
              "context: prepared run without receipt has unknown ordinary charge");
    }
    eh_json_free(load);
    eh_json_free(plan);
    eh_json_free(run);
}
static void measurement_export_bytes(const struct json *doc)
{
    char hash[65];
    const char *bytes = json_text(doc, "selection_bytes"),
               *digest = json_text(doc, "selection_sha256");
    const struct json *size = json_member(doc, "selection_size_bytes");
    if (bytes)
        measurement_hash(bytes, hash);
    else
        hash[0] = 0;
    CHECK(bytes && strlen(bytes) && bytes[strlen(bytes) - 1] == '\n' && digest &&
              !strcmp(hash, digest) && size && size->type == JSON_INTEGER &&
              size->integer == (long long)strlen(bytes),
          "context: exact exported bytes include the final newline and match their hash");
}
struct measurement_allocations {
    unsigned pass, tick, none, arch;
};
static void measurement_export_repeat(struct measurement_allocations *count, const char *arch,
                                      long long repetition)
{
    CHECK((!strcmp(arch, "i686") || !strcmp(arch, "x86_64")) && repetition >= 0 && repetition < 2,
          "context: repeated case has its resolved architecture");
    if (repetition >= 0 && repetition < 2) {
        unsigned bit = 1u << ((!strcmp(arch, "x86_64") ? 2 : 0) + (unsigned)repetition);
        CHECK(!(count->pass & bit), "context: no repeated allocation is duplicated");
        count->pass |= bit;
    }
}
static void measurement_export_item(struct measurement_allocations *count, const struct json *item)
{
    const struct json *rep = json_member(item, "repetition");
    const char *id = json_text(item, "case_id"), *a = json_text(item, "architecture");
    CHECK(id && a && rep && rep->type == JSON_INTEGER, "context: allocated item is explicit");
    if (!id || !a || !rep || rep->type != JSON_INTEGER)
        return;
    if (!strcmp(id, "fixture.good.pass"))
        measurement_export_repeat(count, a, rep->integer);
    else if (!strcmp(id, "fixture.good.tick"))
        count->tick |= !strcmp(a, "i686") ? 1u : !strcmp(a, "x86_64") ? 2u : 0u;
    else if (!strcmp(id, "fixture.good.none")) {
        CHECK(!strcmp(a, "none") && !rep->integer,
              "context: architecture-independent work retains none");
        count->none++;
    } else if (!strcmp(id, "fixture.good.arch")) {
        CHECK(!strcmp(a, "x86_64") && !rep->integer, "context: x86-64-only work stays x86-64");
        count->arch++;
    }
}
static void measurement_export_items(const struct json *doc)
{
    const struct json *items = json_member(doc, "items");
    struct measurement_allocations count = {0};
    CHECK(items && items->type == JSON_ARRAY && items->count == 8,
          "context: exact repeated items are allocated once per architecture");
    for (size_t i = 0; items && i < items->count; i++)
        measurement_export_item(&count, items->item[i]);
    CHECK(count.pass == 15 && count.tick == 3 && count.none == 1 && count.arch == 1,
          "context: full exported first-attempt scope matches the selected workloads");
}
/* The exported scope includes repetitions and architecture-independent work. */
static void measurement_export_cases(void)
{
    static const struct etude_legacy_member member = {"good", 0, 0, "cases/good/good.json", NULL};
    struct run r;
    struct etude_legacy_set_opts o;
    char path[PATH_LEN], text[32768], error[256];
    memset(&r, 0, sizeof r);
    r.name = "measurement-export";
    r.members = &member;
    r.n = 1;
    r.width = 1;
    opts_for(&o, &r);
    o.arch = "i686,x86_64";
    o.native_locks = locks;
    fix_format(path, sizeof path, "%s/measurement-both.export", fix_dir());
    CHECK(!etude_legacy_selection_export(&o, path), "context: both-architecture selection exports");
    CHECK(read_text(path, text, sizeof text) > 0, "context: allocation export reads");
    struct json *doc = NULL;
    CHECK(!json_parse(text, strlen(text), 1, &doc, error, sizeof error),
          "context: allocation export parses");
    measurement_export_bytes(doc);
    measurement_export_items(doc);
    json_free(doc);
}
static void test_contexts(void)
{
    static const struct etude_legacy_member member = {"plain", 0, 0, ONE("plain"), NULL};
    char record[PATH_LEN];
    fix_format(record, sizeof record, "%s/measurement.record", fix_dir());
    measurement_record_file(record);
    measurement_snapshots();
    measurement_export_cases();
    catch_stops();
    stop_signal = 0;
    for (int n = 0; n < 13; n++) {
        char id[80], site[PATH_LEN];
        struct run r;
        struct etude_legacy_set_opts o;
        struct etude_preparation_receipt receipt;
        struct etude_measurement_context *ctx = NULL;
        unsetenv("RACH2_PORT_OFFSET");
        memset(&r, 0, sizeof r);
        fix_format(id, sizeof id, "measurement-context-%d", n);
        measurement_paths(&r, id);
        r.members = &member;
        r.n = 1;
        r.width = 1;
        opts_for(&o, &r);
        o.prepared = 1;
        o.record = record;
        o.native_locks = locks;
        o.run_id = id;
        o.commit = measurement_commit;
        o.stop = &stop_signal;
        o.site = in(site, sizeof site, "site.permits");
        int bad = n >= 3 && n < 8 ? n - 2 : 0;
        if (n >= 11)
            bad = n == 11 ? -1 : -2;
        if (n)
            ctx = measurement_context(&o, id, n >= 2 && n < 8, bad, &receipt);
        o.context = ctx;
        if (n == 8)
            o.commit = "2222222222222222222222222222222222222222";
        if (n == 9)
            o.commit = NULL;
        if (n == 10)
            o.run_id = "unrelated-run";
        o.out = fopen(r.out_file, "wb");
        CHECK(o.out != NULL, "context: output opens");
        r.status = o.out ? etude_legacy_set(&o) : 2;
        if (o.out)
            fclose(o.out);
        history_read(&r);
        if (n < 3 || n >= 11) {
            CHECK(!r.status && !r.read, "context: valid prepared host run succeeds");
            measurement_history(&r, ctx, &receipt, n == 2, n >= 11, n == 11);
        } else {
            CHECK(r.status == 2 && !count_kind(&r.h, "attempt"),
                  "context: unrelated selection, run, commit or attempt refuses work");
            if (n >= 8) {
                char output[4096];
                CHECK(read_text(r.out_file, output, sizeof output) > 0 &&
                          strstr(output, n == 10 ? "allocated run identity differs"
                                                 : "differs from the verified source commit"),
                      "context: direct API refuses a conflicting or absent identity");
            }
        }
        CHECK(!exists(r.status_file), "context: status is removed before the run ends");
        struct etude_ledger *l = etude_ledger_open(ledger);
        long claims = 0;
        CHECK(l && !etude_ledger_list(l, count_claim, &claims) && !claims,
              "context: no admission claim remains");
        if (l)
            etude_ledger_close(l);
        etude_context_close(ctx);
        history_free(&r.h);
    }
    setenv("RACH2_PORT_OFFSET", "7000", 1);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
}
