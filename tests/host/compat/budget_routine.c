/* Synthetic ownership documents test the reader, not machine performance. */
#include <stdarg.h>

struct routine_fixture {
    char refs[40][2048], paths[40][1024], hashes[40][65];
    char completion[1024], history[1024];
    unsigned count, serial;
};
static unsigned routine_serial;
static const char routine_commit[] = "1111111111111111111111111111111111111111";
static const char routine_owner[] = "{\"pid\":\"10\",\"creation\":\"100\"}";
static const char routine_result[] =
    "{\"end\":\"exited\",\"value\":0,\"exit_end\":\"exited\",\"exit_value\":0,"
    "\"active\":0,\"cleanup\":\"confirmed\",\"output_error\":0}";

static const char *routine_file(struct routine_fixture *f, const char *format, ...)
{
    char name[64], text[32768];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    CHECK(n >= 0 && n < (int)sizeof(text) && f->count < 40, "routine fixture fits");
    if (n < 0 || n >= (int)sizeof(text) || f->count == 40)
        exit(2);
    unsigned at = f->count++;
    fix_format(name, sizeof(name), "routine-%u-%u", f->serial, at);
    fix_format(f->paths[at], sizeof(f->paths[at]), "%s/%s", fix_dir(), name);
    const char *path = f->paths[at];
    FILE *file = fopen(path, "wb");
    CHECK(file && fputs(text, file) >= 0 && !fclose(file), "routine file writes");
    CHECK(!file_digest(path, f->hashes[at]), "routine fixture digest");
    fix_format(f->refs[at], sizeof(f->refs[at]),
               "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"%s\"]}", f->hashes[at],
               (unsigned long)strlen(text), path);
    return f->refs[at];
}
static void routine_reading(char out[160], unsigned microseconds)
{
    fix_format(out, 160,
               "{\"utc\":\"2026-10-24T00:00:%02u.%06uZ\",\"monotonic_us\":%u,"
               "\"resolution_us\":1}",
               (microseconds - 1000000) / 1000000, microseconds % 1000000, microseconds);
}
static const char *routine_trace_samples(struct routine_fixture *f, int physical, int imported,
                                         unsigned samples)
{
    struct cmp_text text = {0};
    cmp_printf(&text,
               "{\"kind\":\"start\",\"schema_version\":1,\"method\":\"native-load-v1\","
               "\"platform\":\"%s\",\"machine_id\":\"%s\",\"boot_id\":\"%s\","
               "\"run_id\":\"routine-fixture\",\"root_pid\":%d,\"root_creation\":\"%d\","
               "\"observer_pid\":30,\"observer_creation\":\"300\","
               "\"requested_interval_us\":1000000,\"process_limit\":4096}\n",
               physical || imported ? "windows-native-counters-v1" : "linux-proc-counters-v1",
               physical || imported ? "pc" : "guest", physical || imported ? "boot" : "guestboot",
               physical ? 10 : 20, physical ? 100 : 200);
    for (unsigned i = 0; i < samples; i++)
        cmp_printf(&text,
                   "{\"kind\":\"sample\",\"monotonic_us\":%u,\"utc_us\":%llu,"
                   "\"system_busy_cpu_us\":%u,\"outside_cpu_lower_us\":0,"
                   "\"shared_virtualizer_cpu_lower_us\":0,\"processes\":2,\"denied\":0,"
                   "\"lost\":0,\"unclassified\":0,\"overflow\":false,\"identities_sha256\":"
                   "\"0000000000000000000000000000000000000000000000000000000000000000\"}\n",
                   1000000 + i * 1000000,
                   (unsigned long long)etude_observe_utc("2026-10-24T00:00:00.000000Z") +
                       i * UINT64_C(1000000),
                   i * 1000);
    cmp_printf(&text,
               "{\"kind\":\"end\",\"monotonic_us\":%u,\"utc_us\":%llu,"
               "\"samples\":%u,\"complete\":true,\"owner_lost\":false}\n",
               samples * 1000000,
               (unsigned long long)etude_observe_utc("2026-10-24T00:00:00.000000Z") +
                   (samples - 1) * UINT64_C(1000000),
               samples);
    const char *ref = routine_file(f, "%s", text.data);
    free(text.data);
    return ref;
}
static const char *routine_trace(struct routine_fixture *f, int physical, int imported)
{
    return routine_trace_samples(f, physical, imported, 5);
}

static const char *routine_phase(struct routine_fixture *f, const char *name, const char *route,
                                 const char *lease, const char *admit, int imported)
{
    int collection = !strcmp(name, "collection"), producer = !strcmp(name, "producer");
    unsigned start = producer ? 1000000 : collection ? 4000000 : 2000000;
    unsigned end = producer ? 2000000 : collection ? 5500000 : 3000000;
    char first[160], last[160];
    routine_reading(first, start);
    routine_reading(last, end);
    if (!imported) {
        start = collection ? 103000000 : 99000000;
        end = collection ? 105000000 : 102500000;
        fix_format(first, sizeof(first),
                   "{\"utc\":\"2026-10-24T00:00:01.000000Z\","
                   "\"monotonic_us\":%u,\"resolution_us\":1}",
                   start);
        fix_format(last, sizeof(last),
                   "{\"utc\":\"2026-10-24T00:00:04.000000Z\","
                   "\"monotonic_us\":%u,\"resolution_us\":1}",
                   end);
    }
    const char *raw = routine_file(
        f,
        "{\"format\":\"etude-routine-child\",\"schema_version\":1,"
        "\"process\":{\"pid\":\"%u\",\"creation\":\"%u\"},\"owner\":%s,\"result\":%s,"
        "\"machine_id\":\"%s\",\"boot_id\":\"%s\",\"start\":%s,\"end\":%s}",
        collection ? 20U : 15U, collection ? 200U : 150U, routine_owner, routine_result,
        imported ? "pc" : "guest", imported ? "boot" : "guestboot", first, last);
    return routine_file(
        f,
        "{\"format\":\"etude-routine-phase\",\"schema_version\":1,\"run_id\":\"routine-fixture\","
        "\"commit\":\"%s\",\"route\":%s,\"lease\":%s,\"phase\":\"%s\",\"owner\":%s,"
        "\"child\":{\"pid\":\"%u\",\"creation\":\"%u\"},\"machine_id\":\"%s\","
        "\"boot_id\":\"%s\",\"start\":%s,\"end\":%s,\"admission\":%s,\"receipt\":%s,\"result\":%s}",
        routine_commit, route, lease, name, routine_owner, collection ? 20U : 15U,
        collection ? 200U : 150U, imported ? "pc" : "guest", imported ? "boot" : "guestboot", first,
        last, admit, raw, routine_result);
}
static void routine_lease(struct routine_fixture *f, const char *route, const char *empty,
                          const char **lease, const char **grant)
{
    const char *before = routine_file(f, "etude-site 2\nendpoint linux\ncapacity 24000,1,1\n");
    const char *disabled = routine_file(f, "etude-site 2\nendpoint disabled\ncapacity 24000,1,1\n");
    const char *snapshot_path = f->paths[f->count - 1];
    const char *binding = routine_file(
        f,
        "etude-authority 1\nledger /authority/ledger\nsite /authority/site\ngate /authority/gate\n"
        "state /authority/state\narchive /authority/archive\noperator S-1-5-21\n"
        "program /authority/program\nlauncher_body /authority/launcher\n"
        "physical_block /authority/block\nroute %s\n",
        f->paths[0]);
    const char *state =
        routine_file(f,
                     "etude-authority-state 1\nrun routine-fixture\ncommit %s\nphase blocked\n"
                     "owner_pid 10\nowner_creation 100\n"
                     "nonce aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                     "snapshot %s\n",
                     routine_commit, snapshot_path);
    char first[160];
    routine_reading(first, 1000000);
    *lease = routine_file(
        f,
        "{\"format\":\"etude-routine-lease\",\"schema_version\":1,\"run_id\":\"routine-fixture\","
        "\"commit\":\"%s\",\"route\":%s,\"owner\":%s,\"machine_id\":\"pc\",\"boot_id\":\"boot\","
        "\"grant\":%s,\"authority\":{\"binding\":%s,\"state\":%s,\"snapshot\":%s,"
        "\"gate\":{\"path\":\"/authority/gate\",\"identity\":{\"device\":\"1\",\"inode\":\"2\"}}},"
        "\"endpoints\":[{\"name\":\"linux\",\"policy_before\":%s,\"policy_disabled\":%s,"
        "\"ledger_empty\":%s,\"normal_disabled\":true},{\"name\":\"windows\",\"policy_before\":%s,"
        "\"policy_disabled\":%s,\"ledger_empty\":%s,\"normal_disabled\":true}],"
        "\"result\":{\"exclusive\":true,\"cleanup_before_grant\":\"confirmed\"}}",
        routine_commit, route, routine_owner, first, binding, state, disabled, before, disabled,
        empty, disabled, disabled, empty);
    *grant = routine_file(
        f,
        "{\"format\":\"etude-routine-grant\",\"schema_version\":1,\"run_id\":\"routine-fixture\","
        "\"commit\":\"%s\",\"route\":%s,\"lease\":%s,\"owner\":%s,\"machine_id\":\"pc\","
        "\"boot_id\":\"boot\",\"start\":%s,\"completion_path\":\"%s\","
        "\"phase\":\"physical-capacity-granted\"}",
        routine_commit, route, *lease, routine_owner, first, f->completion);
}
static const char *routine_producer(struct routine_fixture *f, const char *route, const char *grant,
                                    const char *admit, char proof[2048])
{
    const char *record = routine_file(f, "etude-prepare 1\ntree /producer\n");
    unsigned record_at = f->count - 1;
    const char *receipt = routine_file(
        f,
        "{\"format\":\"etude-preparation-receipt\",\"schema_version\":1,"
        "\"run_id\":\"routine-fixture:producer\",\"commit\":\"%s\",\"record_sha256\":\"%s\","
        "\"machine_id\":\"producer\",\"boot_id\":\"producerboot\",\"outcome\":\"pass\","
        "\"cleanup\":\"confirmed\",\"start\":{\"utc\":\"2026-10-24T00:00:00Z\","
        "\"monotonic_us\":10,\"resolution_us\":1},\"end\":{\"utc\":\"2026-10-24T00:00:01Z\","
        "\"monotonic_us\":20,\"resolution_us\":1}}",
        routine_commit, f->hashes[record_at]);
    size_t size;
    char *bytes = NULL;
    CHECK(!cmp_read_file(f->paths[f->count - 1], 8192, &bytes, &size), "producer receipt reads");
    const char *metadata = routine_file(
        f,
        "{\"format\":\"etude-preparation-producer\",\"schema_version\":1,\"commit\":\"%s\","
        "\"record\":\"etude-prepare 1\\ntree /producer\\n\",\"receipt\":%s}",
        routine_commit, bytes);
    unsigned metadata_at = f->count - 1;
    const char *imported =
        routine_file(f,
                     "{\"format\":\"etude-preparation-import-provenance\",\"schema_version\":1,"
                     "\"commit\":\"%s\",\"producer_sha256\":\"%s\",\"ordinary_production\":%s}",
                     routine_commit, f->hashes[metadata_at], bytes);
    fix_format(proof, 2048, "%s", imported);
    free(bytes);
    const char *cleanup = routine_file(
        f, "{\"format\":\"etude-authority-child\",\"schema_version\":1,"
           "\"machine_id\":\"producer\",\"boot_id\":\"producerboot\","
           "\"start\":{\"utc\":\"2026-10-24T00:00:00Z\",\"monotonic_us\":9,\"resolution_us\":1},"
           "\"end\":{\"utc\":\"2026-10-24T00:00:01Z\",\"monotonic_us\":21,\"resolution_us\":1},"
           "\"owner\":{\"pid\":\"50\",\"creation\":\"500\"},"
           "\"process\":{\"pid\":\"40\",\"creation\":\"400\"},"
           "\"keeper\":{\"pid\":\"41\",\"creation\":\"410\"},"
           "\"result\":{\"end\":0,\"value\":0,\"exit_end\":0,\"exit_value\":0,"
           "\"cleanup\":1,\"output_error\":0}}");
    return routine_file(
        f,
        "{\"format\":\"etude-routine-upstream\",\"schema_version\":1,\"run_id\":\"routine-"
        "fixture\","
        "\"commit\":\"%s\",\"route\":%s,\"routine_grant\":%s,\"mode\":\"produced\","
        "\"producer\":{\"receipt\":%s,\"record\":%s,\"metadata\":%s,\"admission\":%s,\"cleanup\":%"
        "s},"
        "\"owned_interval\":{\"machine_id\":\"pc\",\"boot_id\":\"boot\","
        "\"start\":{\"utc\":\"2026-10-24T00:00:00Z\",\"monotonic_us\":1000000,\"resolution_us\":1},"
        "\"end\":{\"utc\":\"2026-10-24T00:00:01Z\",\"monotonic_us\":2000000,\"resolution_us\":1}},"
        "\"result\":{\"outcome\":\"pass\",\"cleanup\":\"confirmed\"}}",
        routine_commit, route, grant, receipt, record, metadata, admit, cleanup);
}
static char *routine_read(const char *path)
{
    char *bytes = NULL;
    size_t size;
    CHECK(!cmp_read_file(path, 65536, &bytes, &size), "routine bytes read");
    return bytes;
}
static struct json *routine_json(const char *bytes)
{
    struct json *doc = NULL;
    char error[160];
    CHECK(!json_parse(bytes, strlen(bytes), 1, &doc, error, sizeof(error)), "routine JSON parses");
    return doc;
}
static const char *routine_receipt(struct routine_fixture *f, int imported, const char *proof)
{
    return routine_file(
        f,
        "{\"format\":\"etude-preparation-receipt\",\"schema_version\":1,"
        "\"run_id\":\"routine-fixture\",\"commit\":\"%s\",\"record_sha256\":"
        "\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"machine_id\":\"%s\",\"boot_id\":\"%s\",\"outcome\":\"pass\",\"cleanup\":\"confirmed\","
        "\"start\":{\"utc\":\"2026-10-24T00:00:01.200000Z\",\"monotonic_us\":%u,"
        "\"resolution_us\":1},\"end\":{\"utc\":\"2026-10-24T00:00:01.800000Z\","
        "\"monotonic_us\":%u,\"resolution_us\":1},\"conditioning\":{\"inhouse\":\"clean\","
        "\"backend\":\"%s\",\"provenance\":%s}}",
        routine_commit, imported ? "pc" : "guest", imported ? "boot" : "guestboot",
        imported ? 2200000U : 100000000U, imported ? 2800000U : 102000000U,
        imported ? "import" : "ordinary", imported ? proof : "null");
}
static const char *routine_history(struct routine_fixture *f, int imported)
{
    const char *trace = routine_trace(f, 0, imported);
    const char *load = routine_file(f, "{\"runtime\":{\"trace\":%s}}", trace);
    const char *ref = routine_file(
        f,
        "{\"run_id\":\"routine-fixture\",\"kind\":\"plan\",\"data\":{\"commit\":\"%s\"}}\n"
        "{\"run_id\":\"routine-fixture\",\"kind\":\"run\",\"data\":{\"time\":{\"clock\":{"
        "\"machine_id\":\"%s\",\"boot_id\":\"%s\"},\"end_us\":%u},\"load\":%s}}\n",
        routine_commit, imported ? "pc" : "guest", imported ? "boot" : "guestboot",
        imported ? 5000000U : 104000000U, load);
    fix_format(f->history, sizeof(f->history), "%s", f->paths[f->count - 1]);
    return ref;
}
struct routine_parts {
    const char *route, *lease, *grant, *upstream, *empty, *admit, *receipt, *history;
    int imported;
};
static char *routine_fragment(const struct json *value)
{
    struct jbuf out = {0};
    json_canonical(value, &out);
    return out.data;
}
static const char *routine_producer_phase(struct routine_fixture *f, const struct routine_parts *p)
{
    struct json *upref = routine_json(p->upstream);
    size_t size;
    char *bytes = etude_routine_bytes(upref, &size);
    struct json *upstream = routine_json(bytes);
    const struct json *ref = json_member(json_member(upstream, "producer"), "cleanup");
    free(bytes);
    bytes = etude_routine_bytes(ref, &size);
    struct json *raw = routine_json(bytes);
    char *child = routine_fragment(json_member(raw, "process"));
    char *start = routine_fragment(json_member(raw, "start"));
    char *end = routine_fragment(json_member(raw, "end"));
    char *cleanup = routine_fragment(ref);
    const char *result = routine_file(
        f,
        "{\"format\":\"etude-routine-phase\",\"schema_version\":1,\"run_id\":\"routine-fixture\","
        "\"commit\":\"%s\",\"route\":%s,\"lease\":%s,\"phase\":\"producer\",\"owner\":%s,"
        "\"child\":%s,\"machine_id\":\"producer\",\"boot_id\":\"producerboot\","
        "\"start\":%s,\"end\":%s,\"admission\":%s,\"receipt\":%s,\"result\":%s}",
        routine_commit, p->route, p->lease, routine_owner, child, start, end, p->admit, cleanup,
        routine_result);
    free(child);
    free(start);
    free(end);
    free(cleanup);
    free(bytes);
    json_free(raw);
    json_free(upstream);
    json_free(upref);
    return result;
}
static char *routine_terminal(struct routine_fixture *f, const struct routine_parts *p)
{
    const char *prep = routine_phase(f, "preparation", p->route, p->lease, p->admit, p->imported);
    const char *collection =
        routine_phase(f, "collection", p->route, p->lease, p->admit, p->imported);
    const char *producer = p->imported ? routine_producer_phase(f, p) : NULL;
    const char *trace = routine_trace(f, 1, p->imported);
    char first[160], last[160];
    routine_reading(first, 3500000);
    routine_reading(last, 6000000);
    struct cmp_text text = {0};
    cmp_printf(&text,
               "{\"format\":\"etude-routine-completion\",\"schema_version\":1,\"run_id\":\"routine-"
               "fixture\","
               "\"commit\":\"%s\",\"route\":%s,\"routine_grant\":%s,\"lease\":%s,\"owner\":%s,"
               "\"collector\":{\"pid\":\"20\",\"creation\":\"200\"},\"machine_id\":\"pc\",\"boot_"
               "id\":\"boot\","
               "\"preparation_end\":%s,\"end\":%s,\"completion_path\":\"%s\",\"history\":%s,"
               "\"phase_cleanup\":[%s,%s%s%s],\"ledgers\":[{\"endpoint\":\"linux\",\"claims\":%s},"
               "{\"endpoint\":\"windows\",\"claims\":%s}],\"observers\":[%s],"
               "\"result\":{\"outcome\":\"pass\",\"cleanup\":\"confirmed\"}}",
               routine_commit, p->route, p->grant, p->lease, routine_owner, first, last,
               f->completion, p->history, prep, collection, producer ? "," : "",
               producer ? producer : "", p->empty, p->empty, trace);
    return text.data;
}
static struct json *routine_nodes(struct routine_fixture *f, struct routine_parts *p)
{
    p->history = routine_history(f, p->imported);
    char *receipt;
    struct json *r = routine_json(p->receipt);
    char path[4096];
    CHECK(!etude_routine_location(r, path, sizeof(path)), "receipt location resolves");
    json_free(r);
    receipt = routine_read(path);
    const char *ref = routine_file(
        f,
        "{\"format\":\"etude-preparation-nodes\",\"schema_version\":1,\"nodes\":[\"%s\"],"
        "\"preparation\":%s,\"routine_grant\":%s,\"upstream_preparation\":%s}",
        NODE, receipt, p->grant, p->upstream ? p->upstream : "null");
    free(receipt);
    char *bytes = routine_read(f->paths[f->count - 1]);
    (void)ref;
    struct json *nodes = routine_json(bytes);
    free(bytes);
    return nodes;
}
static void routine_changed(struct routine_fixture *f, const struct etude_routine_input *in,
                            const char *original, const char *from, const char *to)
{
    char *at = strstr(original, from);
    CHECK(at != NULL, "negative routine field exists");
    if (!at)
        return;
    struct cmp_text text = {0};
    cmp_printf(&text, "%.*s%s%s", (int)(at - original), original, to, at + strlen(from));
    FILE *file = fopen(f->completion, "wb");
    CHECK(file && fputs(text.data, file) >= 0 && !fclose(file), "negative completion writes");
    struct etude_routine_cost cost;
    CHECK(etude_routine_charge(in, &cost) == -1, "changed completion refused");
    free(text.data);
}
static void routine_copy(const char *from, const char *to)
{
    char *bytes = routine_read(from);
    FILE *file = fopen(to, "wb");
    CHECK(file && fputs(bytes, file) >= 0 && !fclose(file), "transported bytes copy");
    free(bytes);
}
static void routine_transport(struct routine_fixture *f, struct etude_routine_input *in)
{
    char name[80], directory[1024], path[1200];
    fix_format(name, sizeof(name), "routine-proofs-%u", f->serial);
    fix_format(directory, sizeof(directory), "%s/%s", fix_dir(), name);
    CHECK(!mkdir(directory, 0700), "transport proof directory creates");
    for (unsigned i = 0; i < f->count; i++) {
        fix_format(path, sizeof(path), "%s/%s", directory, f->hashes[i]);
        routine_copy(f->paths[i], path);
    }
    CHECK(!etude_routine_proof_directory(directory), "transport resolver accepts directory");
    char terminal[1200];
    fix_format(terminal, sizeof(terminal), "%s/completion.json", directory);
    routine_copy(f->completion, terminal);
    const char *original = in->completion_path;
    in->completion_path = terminal;
    struct etude_routine_cost cost;
    CHECK(etude_routine_charge(in, &cost) == 1, "transported exact proofs pass");
    char *history = routine_read(f->history);
    FILE *changed = fopen(f->history, "ab");
    CHECK(changed && fputs("{}\n", changed) >= 0 && !fclose(changed), "actual history changes");
    CHECK(etude_routine_charge(in, &cost) == -1,
          "copied proof cannot replace changed final history");
    changed = fopen(f->history, "wb");
    CHECK(changed && fputs(history, changed) >= 0 && !fclose(changed), "actual history restores");
    free(history);
    fix_format(path, sizeof(path), "%s/%s", directory, f->hashes[0]);
    CHECK(!unlink(path), "transport proof removes");
    CHECK(etude_routine_charge(in, &cost) == -1, "missing proof refuses original fallback");
    FILE *file = fopen(path, "wb");
    CHECK(file && fputs("wrong", file) >= 0 && !fclose(file), "wrong proof writes");
    CHECK(etude_routine_charge(in, &cost) == -1, "wrong proof bytes refused");
    CHECK(!unlink(path) && !symlink(f->paths[0], path), "proof symlink creates");
    CHECK(etude_routine_charge(in, &cost) == -1, "proof symlink refused");
    CHECK(!etude_routine_proof_directory(NULL), "transport resolver resets");
    in->completion_path = original;
    struct json *ref = routine_json(f->refs[0]);
    struct json *locations = (struct json *)json_member(ref, "locations");
    locations->item = realloc(locations->item, 2 * sizeof(*locations->item));
    locations->item[1] = routine_json("\"/another/location\"");
    locations->count = 2;
    size_t size;
    char *bytes = etude_routine_bytes(ref, &size);
    CHECK(!bytes, "ambiguous proof locations refused");
    free(bytes);
    json_free(ref);
}
static char *routine_full_observation(struct routine_fixture *f,
                                      const struct etude_routine_input *in, const char *terminal,
                                      int imported)
{
    struct json *document = routine_json(terminal);
    struct json *observers = (struct json *)json_member(document, "observers");
    const char *ref = routine_trace_samples(f, 1, imported, 6);
    json_free(observers->item[0]);
    observers->item[0] = routine_json(ref);
    struct jbuf text = {0};
    json_canonical(document, &text);
    FILE *file = fopen(f->completion, "wb");
    CHECK(file && fputs(text.data, file) >= 0 && !fclose(file), "full owner trace writes");
    struct etude_routine_cost cost;
    CHECK(etude_routine_charge(in, &cost) == 1 && cost.load_available &&
              cost.preparation_load_available && cost.charged_us == 5000000,
          "actual terminal observation covers unchanged physical charge");
    json_free(document);
    return text.data;
}

static void routine_case(int imported)
{
    struct routine_fixture *f = calloc(1, sizeof(*f));
    CHECK(f != NULL, "routine fixture allocates");
    if (!f)
        return;
    f->serial = ++routine_serial;
    char name[80];
    fix_format(name, sizeof(name), "routine-completion-%u", f->serial);
    fix_format(f->completion, sizeof(f->completion), "%s/%s", fix_dir(), name);
    struct routine_parts p = {0};
    p.imported = imported;
    p.route =
        routine_file(f, "{\"format\":\"etude-routine-route\",\"schema_version\":1,\"mode\":\"%s\"}",
                     imported ? "windows-import" : "wsl-native");
    p.empty = routine_file(f, "etude-ledger 1\n");
    p.admit = routine_file(f, "exited 0\n");
    routine_lease(f, p.route, p.empty, &p.lease, &p.grant);
    char proof[2048] = "null";
    if (imported)
        p.upstream = routine_producer(f, p.route, p.grant, p.admit, proof);
    p.receipt = routine_receipt(f, imported, proof);
    struct json *nodes = routine_nodes(f, &p);
    const struct json *grant = json_member(nodes, "routine_grant");
    const struct json *upstream = json_member(nodes, "upstream_preparation");
    CHECK(!etude_routine_context(grant, upstream, "routine-fixture", routine_commit),
          "immutable preflight accepts absent terminal");
    struct etude_routine_input in = {
        nodes, "routine-fixture", routine_commit, f->completion, f->history, NULL, NULL};
    struct etude_routine_cost cost;
    CHECK(etude_routine_charge(&in, &cost) == -1, "missing completion refused");
    char *terminal = routine_terminal(f, &p);
    FILE *file = fopen(f->completion, "wb");
    CHECK(file && fputs(terminal, file) >= 0 && !fclose(file), "terminal writes");
    CHECK(etude_routine_charge(&in, &cost) == 1, "physical charge validates");
    CHECK(cost.charged_us == 5000000 && cost.preparation_us == 2500000 && cost.error_us == 2,
          "one physical interval charged without producer clock addition");
    CHECK(!cost.load_available && cost.preparation_load_available,
          "unobserved cleanup tail excludes ranking but covered preparation remains eligible");
    char *full = routine_full_observation(f, &in, terminal, imported);
    free(terminal);
    terminal = full;
    CHECK(etude_routine_context(grant, upstream, "routine-fixture", routine_commit) == -1,
          "preexisting terminal refused at preflight");
    routine_changed(f, &in, terminal, "\"cleanup\":\"confirmed\"", "\"cleanup\":\"unknown\"");
    routine_changed(f, &in, terminal, "\"creation\":\"200\"", "\"creation\":\"201\"");
    routine_changed(f, &in, terminal, "\"run_id\":\"routine-fixture\"", "\"run_id\":\"other\"");
    file = fopen(f->completion, "wb");
    CHECK(file && fputs(terminal, file) >= 0 && !fclose(file), "terminal restores");
    routine_transport(f, &in);
    free(terminal);
    json_free(nodes);
    free(f);
}
static void routine_local_load(void)
{
    const char *time = "{\"start_utc\":\"2026-10-24T00:00:00Z\","
                       "\"end_utc\":\"2026-10-24T00:00:04Z\"}";
    char ref[2048];
    CHECK(!load_fixture(ref, sizeof(ref), fix_dir(), "local-prefix", time), "local trace writes");
    struct eh_json *reference = eh_json_parse(ref);
    size_t size;
    char *bytes = cmp_blob_bytes(reference, &size);
    char *mark = strstr(bytes, "\"comparison_available\":true");
    CHECK(mark != NULL, "whole-run availability exists");
    struct cmp_text changed = {0}, data = {0};
    cmp_printf(&changed, "%.*s\"comparison_available\":false%s", (int)(mark - bytes), bytes,
               mark + strlen("\"comparison_available\":true"));
    CHECK(!load_fixture_store(ref, sizeof(ref), fix_dir(), changed.data), "local-only load writes");
    cmp_printf(&data, "{\"time\":%s,\"load\":%s}", time, ref);
    struct cmp_record run = {0};
    run.run = "local-prefix";
    struct eh_json *parsed = eh_json_parse(data.data);
    run.data = parsed;
    cmp_check_load(&run, NULL);
    CHECK(run.local_load_eligible && !run.load_eligible,
          "whole-run exclusion does not erase independently verified local observation");
    eh_json_free(parsed);
    eh_json_free(reference);
    free(bytes);
    free(changed.data);
    free(data.data);
}
static void test_routine_charge(void)
{
    routine_case(0);
    routine_case(1);
    routine_local_load();
    uint64_t work = 0;
    const char *path = write_policy("routine-policy.json", &usual);
    CHECK(!etude_routine_work_us(path, "ci", &work) && work == UINT64_C(540000000),
          "owner work deadline uses the actual purpose ceiling and cleanup reserve");
    CHECK(etude_routine_work_us(path, "phase_first_run", &work) == -1,
          "experimental exemption is not a routine owner deadline");
}
