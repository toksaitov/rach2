/* The native composer retains actual inputs and refuses unrelated proofs. */
#include "../../../tools/etude/json.h"
#include "../../../tools/etude/routine_proof.h"

int etude_prepare_context_main(int argc, char **argv);

static char compose_source_hash[65];

static int compose_fixture(const char *output, char *const extra[])
{
    char paths[7][4096];
    const char *names[] = {"compose-record", "compose-site", "compose-tool", "compose-source",
                           output,           "receipt",      "trace"};
    for (size_t i = 0; i < sizeof names / sizeof(*names); i++)
        snprintf(paths[i], sizeof paths[i], "%s/%s", fix_dir(), names[i]);
    char *argv[80] = {"context",
                      "--tree",
                      (char *)fix_dir(),
                      "--record",
                      paths[0],
                      "--site",
                      paths[1],
                      "--etude",
                      paths[2],
                      "--source-snapshot",
                      paths[3],
                      "--output",
                      paths[4],
                      "--commit",
                      (char *)commit,
                      "--run-id",
                      "context-test",
                      "--source-sha256",
                      compose_source_hash,
                      "--cache-method",
                      "fixture",
                      "--ordinary-preparation",
                      "native fixture",
                      "--one-time-production",
                      "none",
                      "--purpose",
                      "focused",
                      "--tool",
                      paths[2]};
    size_t count = 29;
    for (size_t i = 0; extra && extra[i]; i++) {
        if (!strcmp(extra[i], "--purpose") && extra[i + 1]) {
            argv[26] = extra[++i];
            continue;
        }
        if (count + 1 >= sizeof argv / sizeof(*argv))
            return -1;
        argv[count++] = extra[i];
    }
    argv[count] = NULL;
    return etude_prepare_context_main((int)count, argv);
}
static void compose_seed(void)
{
    char input[2048], hash[65], text[4096];
    put("compose-input", "deployed source bytes\n");
    put("compose-tool", "native tool bytes\n");
    put("compose-record", "ordinary artifact record\n");
    put("compose-site", "site policy\n");
    descriptor("compose-input", input, hash);
    snprintf(text, sizeof text,
             "{\"schema_version\":1,\"commit\":\"%s\",\"inputs\":[{\"path\":\"compose-input\","
             "\"sha256\":\"%s\",\"size_bytes\":22}]}",
             commit, hash);
    put("compose-source", text);
    descriptor("compose-source", input, compose_source_hash);
}
static int compose_open(const char *name, struct etude_measurement_context **context)
{
    char path[4096], record[4096], policy[4096], program[4096], error[160];
    char *tree = realpath(fix_dir(), NULL);
    if (!tree)
        return -1;
    snprintf(path, sizeof path, "%s/%s/context.json", tree, name);
    snprintf(record, sizeof record, "%s/compose-record", tree);
    snprintf(policy, sizeof policy, "%s/compose-site", tree);
    snprintf(program, sizeof program, "%s/compose-tool", tree);
    struct etude_context_inputs inputs = {tree, record, policy, commit, program};
    int rc = etude_context_open(path, &inputs, context, error, sizeof error);
    free(tree);
    return rc;
}

static void compose_positive(void)
{
    struct etude_measurement_context *context = NULL;
    char error[160], path[4096];
    CHECK(!compose_fixture("compose-good", NULL), "native composer writes complete snapshots");
    CHECK(!compose_open("compose-good", &context) && context && !etude_context_eligible(context),
          "unknown cache and absent observer remain correctness only");
    put("compose-tool", "changed executable\n");
    CHECK(context && etude_context_recheck(context, error, sizeof error),
          "composed context rejects a changed deployed tool");
    put("compose-tool", "native tool bytes\n");
    etude_context_close(context);
    CHECK(compose_fixture("compose-good", NULL), "composer never replaces an existing output");
    snprintf(path, sizeof path, "%s/compose-good/tools.json", fix_dir());
    struct json *doc = NULL;
    CHECK(!routine_json(path, &doc), "composed native tool snapshot parses");
    const struct json *inputs = json_member(doc, "inputs");
    CHECK(inputs && inputs->type == JSON_ARRAY && inputs->count == 1,
          "main program and repeated tool input are captured once");
    json_free(doc);
}
static void compose_refusals(void)
{
    char original[65], output[4096];
    strcpy(original, compose_source_hash);
    memset(compose_source_hash, '0', 64);
    compose_source_hash[64] = 0;
    CHECK(compose_fixture("compose-bad-hash", NULL), "composer rejects a wrong source hash");
    strcpy(compose_source_hash, original);
    snprintf(output, sizeof output, "%s/compose-bad-hash", fix_dir());
    struct stat st;
    CHECK(lstat(output, &st) != 0 && errno == ENOENT,
          "source verification fails before output creation");
    put("compose-input", "changed source\n");
    CHECK(compose_fixture("compose-bad-source", NULL), "composer rejects changed source bytes");
    put("compose-input", "deployed source bytes\n");
    char *upstream[] = {"--upstream-preparation", (char *)fix_path("receipt"), NULL};
    CHECK(compose_fixture("compose-no-grant", upstream),
          "upstream preparation cannot be supplied without a physical grant");
    char stop[4096], trace[4096];
    snprintf(stop, sizeof stop, "%s/compose-stop", fix_dir());
    snprintf(trace, sizeof trace, "%s/trace", fix_dir());
    put("compose-stop", "existing stop\n");
    char *observer[] = {"--outer-trace", trace, "--outer-stop-request", stop, NULL};
    CHECK(compose_fixture("compose-stopped", observer),
          "a sampler stop already present cannot describe a new observation");
    CHECK(!unlink(stop), "observer stop fixture is removed");
}
static void compose_prepared(void)
{
    struct etude_preparation_receipt prepared = receipt;
    char ref[2048], prep[4096], protocol[4096], trace[4096], stop[4096];
    descriptor("compose-record", ref, prepared.record_sha256);
    snprintf(prep, sizeof prep, "%s/compose-receipt", fix_dir());
    snprintf(protocol, sizeof protocol, "%s/cache-evidence", fix_dir());
    snprintf(trace, sizeof trace, "%s/trace", fix_dir());
    snprintf(stop, sizeof stop, "%s/compose-uncreated/observer-stop", fix_dir());
    CHECK(!etude_context_receipt_write(prep, &prepared), "native composer receipt writes once");
    char *args[] = {"--preparation",
                    prep,
                    "--cache-protocol",
                    protocol,
                    "--outer-trace",
                    trace,
                    "--outer-stop-request",
                    stop,
                    NULL};
    CHECK(!compose_fixture("compose-prepared", args),
          "composer retains current native receipt, cache evidence and observer");
    struct etude_measurement_context *context = NULL;
    CHECK(!compose_open("compose-prepared", &context) && etude_context_eligible(context),
          "native reader accepts composed controlled same-boot context");
    const struct etude_preparation_receipt *got = etude_context_preparation(context);
    CHECK(got && got->same_run && !strcmp(got->record_sha256, prepared.record_sha256),
          "composer preserves the actual preparation event and record identity");
    etude_context_close(context);
    char *missing[] = {"--cache-protocol", protocol, NULL};
    CHECK(compose_fixture("compose-cache-without-prep", missing),
          "cache evidence cannot be used without native preparation");
}

static void compose_selection_seed(int duplicate)
{
    const char raw[] = "{\"selection\":1}\n";
    struct sha256 sha;
    unsigned char sum[32];
    char hash[65], numbers[256];
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)raw, strlen(raw));
    sha256_final(&sha, sum);
    sha256_hex(sum, hash);
    struct jbuf b = {0};
    jbuf_puts(&b, "{\"selection_bytes\":");
    routine_quote(&b, raw);
    snprintf(numbers, sizeof numbers,
             ",\"selection_size_bytes\":%lu,"
             "\"selection_sha256\":\"%s\",\"items\":[",
             (unsigned long)strlen(raw), hash);
    jbuf_puts(&b, numbers);
    const char item[] =
        "{\"case_id\":\"guest.fixture\",\"architecture\":\"i686\",\"repetition\":0}";
    jbuf_puts(&b, item);
    if (duplicate) {
        jbuf_puts(&b, ",");
        jbuf_puts(&b, item);
    }
    jbuf_puts(&b, "]}");
    put("compose-selection", b.data);
    jbuf_free(&b);
}
static void compose_selections(void)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/compose-selection", fix_dir());
    char *args[] = {"--purpose", "phase_first_run", "--selection-export", path, NULL};
    compose_selection_seed(0);
    CHECK(!compose_fixture("compose-first-run", args),
          "native composer binds actual selection bytes and allocated attempts");
    struct etude_measurement_context *context = NULL;
    CHECK(!compose_open("compose-first-run", &context) && context,
          "native reader opens the composed phase-first selection");
    etude_context_close(context);
    compose_selection_seed(1);
    CHECK(compose_fixture("compose-repeated-attempt", args),
          "repeated allocated attempt identities are refused");
    char *wrong_purpose[] = {"--selection-export", path, NULL};
    CHECK(compose_fixture("compose-focused-exemption", wrong_purpose),
          "a focused context cannot receive a phase-first exemption");
}

static void test_context_compose(void)
{
    compose_seed();
    compose_positive();
    compose_refusals();
    compose_prepared();
    compose_selections();
}
