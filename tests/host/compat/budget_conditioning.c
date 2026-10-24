/* Synthetic proof variants check conditioning, not measured performance. */
static void conditioning_receipt(char *out, size_t size, const char *state, const char *proof)
{
    if (!state) {
        fix_format(out, size,
                   "{\"format\":\"etude-preparation-receipt\",\"outcome\":\"pass\","
                   "\"cleanup\":\"confirmed\"}");
        return;
    }
    fix_format(out, size,
               "{\"format\":\"etude-preparation-receipt\",\"schema_version\":1,"
               "\"outcome\":\"pass\",\"cleanup\":\"confirmed\","
               "\"commit\":\"1111111111111111111111111111111111111111\",\"record_sha256\":"
               "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
               "\"conditioning\":{\"inhouse\":\"%s\",\"backend\":\"%s\",\"provenance\":%s}}",
               state, proof ? "import" : "ordinary", proof ? proof : "null");
}

static void conditioning_declaration(char out[2048], const char *receipt, int extra)
{
    char text[8192], hex[65];
    fix_format(text, sizeof(text),
               "{\"schema_version\":1,\"format\":\"etude-preparation-nodes\","
               "\"architectures\":[\"i686\",\"x86_64\"],\"kernel\":null,\"nodes\":[\"%s\"%s],"
               "\"preparation\":%s}",
               NODE, extra ? ",\"fixture.extra\"" : "", receipt);
    const char *path = put("conditioning.nodes", text);
    CHECK(path && !file_digest(path, hex), "conditioning declaration hashes");
    fix_format(out, 2048, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"%s\"]}", hex,
               (unsigned long)strlen(text), path);
}

static void conditioning_import(char out[2048], const char *state, const char *role)
{
    char receipt[2048], text[8192], hex[65];
    conditioning_receipt(receipt, sizeof(receipt), state, NULL);
    fix_format(text, sizeof(text),
               "{\"schema_version\":1,\"format\":\"etude-preparation-import-provenance\","
               "\"commit\":\"1111111111111111111111111111111111111111\","
               "\"producer_sha256\":\"retained-producer-identity\",\"ordinary_production\":%s,"
               "\"roles\":{\"%s\":\"%s\"%s},\"imported\":{\"%s\":{\"sha256\":"
               "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}},"
               "\"artifacts\":{\"%s\":{\"sha256\":"
               "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}}}",
               receipt, NODE, role ? role : "native",
               role ? "" : ",\"implicit.kernel\":\"portable\"", NODE, NODE);
    const char *path = put("conditioning.import", text);
    CHECK(path && !file_digest(path, hex), "conditioning import hashes");
    fix_format(out, 2048, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"%s\"]}", hex,
               (unsigned long)strlen(text), path);
}

static void conditioning_node_forecast(const char *state)
{
    struct run r = CI_RUN("conditioning-node", "2026-10-24T01:00:00.000000Z");
    const struct run *runs[] = {&r};
    struct cmp_history h;
    struct etude_budget_forecast f;
    char receipt[4096], proof[2048], name[64];
    fix_format(name, sizeof(name), "conditioning-%s", state);
    r.id = name;
    conditioning_receipt(receipt, sizeof(receipt), state, NULL);
    /* The extra node prevents aggregate matching, exercising per-node fallback. */
    conditioning_declaration(proof, receipt, 1);
    preparation_artifacts = proof;
    r.purpose = "phase_first_run";
    r.exemption = 1;
    r.verdict = "exempt";
    int loaded = load(runs, 1, &h, name);
    preparation_artifacts = NULL;
    if (loaded)
        return;
    CHECK(!forecast(&policy, ETUDE_BUDGET_RELEASE, &h, &f), "conditioning node forecast");
    CHECK(f.prep_us == (!strcmp(state, "clean") ? 4 * (uint64_t)SECOND : 0),
          "per-node release cost requires actual clean conditioning");
    CHECK(!strcmp(state, "clean") || f.report.unknown,
          "retained per-node preparation is unknown, not free");
    etude_budget_report_free(&f.report);
    cmp_free(&h);
}

static void conditioning_aggregate(struct cmp_record *plan, int expected)
{
    const char *nodes[] = {NODE};
    struct etude_preparation_selection selected = {nodes, 1, 3, ETUDE_BUDGET_RELEASE};
    struct cmp_record run = {0}, step = {0};
    struct cmp_record *records[] = {plan, &run, &step}, *runs[] = {&run};
    struct cmp_history history = {0};
    uint64_t us;
    size_t count;
    plan->id = "plan";
    plan->run = run.run = step.run = "conditioning";
    run.id = "run";
    run.kind = "run";
    run.load_eligible = 1;
    run.data = run.json = eh_json_parse(
        "{\"plan_id\":\"plan\",\"preparation_steps\":[\"step\"],"
        "\"result\":{\"outcome\":\"pass\",\"cleanup\":\"confirmed\",\"disruptions\":[]}}");
    step.id = "step";
    step.kind = "step";
    step.data = step.json =
        eh_json_parse("{\"plan_id\":\"plan\",\"item_id\":null,\"stage\":\"prepare\",\"time\":{"
                      "\"end_utc\":\"2026-10-24T00:00:00.000000Z\",\"elapsed_us\":{\"value\":12345,"
                      "\"quality\":\"exact\",\"error_bound\":1}},\"result\":{\"outcome\":\"pass\","
                      "\"cleanup\":\"confirmed\",\"disruptions\":[]}}");
    history.by_id = records;
    history.unique = 3;
    history.by_run = runs;
    history.runs = 1;
    CHECK(!etude_budget_preparation(&history, &policy, &selected, &us, &count) &&
              count == (size_t)expected && us == (expected ? 12345 : 0),
          "aggregate release conditioning preserves the original local charge");
    selected.cls = ETUDE_BUDGET_FIRST_RUN;
    CHECK(!etude_budget_preparation(&history, &policy, &selected, &us, &count) && count == 1,
          "warm first-run aggregate remains available for its own class");
    eh_json_free(run.json);
    eh_json_free(step.json);
}

static void test_conditioned_preparation(void)
{
    static const struct {
        const char *local, *producer, *role;
        int eligible;
    } rows[] = {{"clean", NULL, NULL, 1},
                {NULL, NULL, NULL, 0},
                {"retained", NULL, NULL, 0},
                {"unknown", NULL, NULL, 0},
                {"clean", "clean", "portable", 1},
                {"clean", "retained", "portable", 0},
                {"clean", "unknown", "portable", 0},
                {"retained", "clean", "portable", 0},
                {"clean", "retained", "native", 1},
                {"clean", "retained", "inventory", 1}};
    struct cmp_record plan = {0};
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        char receipt[4096], imported[2048], proof[2048], text[4096];
        if (rows[i].producer)
            conditioning_import(imported, rows[i].producer, rows[i].role);
        conditioning_receipt(receipt, sizeof(receipt), rows[i].local,
                             rows[i].producer ? imported : NULL);
        conditioning_declaration(proof, receipt, 0);
        fix_format(text, sizeof(text),
                   "{\"purpose\":\"phase_first_run\",\"environment\":{\"machine_id\":\"m1\","
                   "\"os\":\"linux\",\"accelerator\":\"kvm\"},\"artifacts\":[%s]}",
                   proof);
        plan.data = plan.json = eh_json_parse(text);
        CHECK(etude_budget_preparation_eligible(&plan, ETUDE_BUDGET_RELEASE, NODE) ==
                  rows[i].eligible,
              "release production matches local and portable producer conditioning");
        CHECK(etude_budget_preparation_eligible(&plan, ETUDE_BUDGET_FIRST_RUN, NODE),
              "warm first measurements remain available under their own class");
        conditioning_aggregate(&plan, rows[i].eligible);
        if (rows[i].producer) {
            CHECK(put("conditioning.import", "changed") != NULL, "import proof is changed");
            CHECK(!etude_budget_preparation_eligible(&plan, ETUDE_BUDGET_RELEASE, NODE),
                  "changed import proof cannot teach release cost");
        }
        eh_json_free(plan.json);
    }
    conditioning_node_forecast("retained");
    conditioning_node_forecast("clean");
}

static void test_implicit_preparation(void)
{
    struct cmp_record plan = {0};
    char receipt[4096], imported[2048], proof[2048], text[4096];
    conditioning_import(imported, "retained", NULL);
    conditioning_receipt(receipt, sizeof(receipt), "clean", imported);
    conditioning_declaration(proof, receipt, 0);
    fix_format(text, sizeof(text),
               "{\"purpose\":\"phase_first_run\",\"environment\":{\"machine_id\":\"m1\","
               "\"os\":\"linux\",\"accelerator\":\"kvm\"},\"artifacts\":[%s]}",
               proof);
    plan.data = plan.json = eh_json_parse(text);
    CHECK(etude_budget_preparation_eligible(&plan, ETUDE_BUDGET_RELEASE, NODE),
          "native node retains its actual clean local preparation");
    conditioning_aggregate(&plan, 0);
    eh_json_free(plan.json);
}
