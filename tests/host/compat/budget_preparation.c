/* Aggregate costs retain node-set, platform and availability boundaries. */
static void test_aggregate_preparation(void)
{
    const char *nodes[] = {"build/tests/host/sha256_hosttest"};
    struct etude_preparation_selection selected = {nodes, 1, 3, ETUDE_BUDGET_RELEASE};
    static const char declaration[] =
        "{\"schema_version\":1,\"format\":\"etude-preparation-nodes\","
        "\"architectures\":[\"i686\",\"x86_64\"],\"kernel\":null,"
        "\"nodes\":[\"build/tests/host/sha256_hosttest\"]}\n";
    const char *path = put("aggregate.nodes", declaration);
    char hex[65], text[8192];
    struct cmp_record plan = {0}, run = {0}, step = {0};
    struct cmp_record *records[] = {&plan, &run, &step}, *runs[] = {&run};
    struct cmp_history history = {0};
    uint64_t us;
    size_t count;
    CHECK(path && file_digest(path, hex) == 0, "aggregate declaration");
    if (!path)
        return;
    fix_format(text, sizeof(text),
               "{\"purpose\":\"phase_first_run\",\"environment\":{\"machine_id\":\"m1\","
               "\"os\":\"linux\",\"accelerator\":\"kvm\"},\"artifacts\":[{\"sha256\":\"%s\","
               "\"size_bytes\":%lu,\"locations\":[\"%s\"]}]}",
               hex, (unsigned long)strlen(declaration), path);
    plan.id = "plan";
    plan.kind = "plan";
    plan.json = eh_json_parse(text);
    plan.data = plan.json;
    run.id = "run";
    run.kind = "run";
    run.load_eligible = 1;
    run.json = eh_json_parse("{\"plan_id\":\"plan\",\"preparation_steps\":[\"step\"],"
                             "\"result\":{\"outcome\":\"pass\",\"cleanup\":\"confirmed\",\"disruptions\":[]}}");
    run.data = run.json;
    step.id = "step";
    step.kind = "step";
    step.json = eh_json_parse("{\"plan_id\":\"plan\",\"item_id\":null,\"stage\":\"build\",\"time\":{"
                              "\"end_utc\":\"2026-10-24T00:00:00.000000Z\",\"elapsed_us\":{\"value\":12345,"
                              "\"quality\":\"exact\",\"error_bound\":1}},\"result\":{\"outcome\":\"pass\","
                              "\"cleanup\":\"confirmed\",\"disruptions\":[]}}");
    step.data = step.json;
    plan.run = run.run = step.run = "one";
    history.by_id = records;
    history.unique = 3;
    history.by_run = runs;
    history.runs = 1;
    CHECK(etude_budget_preparation(&history, &policy, &selected, &us, &count) == 0 &&
              count == 1 && us == 12345, "aggregate charged once");
    selected.architectures = 1;
    CHECK(etude_budget_preparation(&history, &policy, &selected, &us, &count) == 0 && count == 0,
          "aggregate refuses architecture change");
    selected.architectures = 3;
    nodes[0] = "build/tests/host/other";
    CHECK(etude_budget_preparation(&history, &policy, &selected, &us, &count) == 0 && count == 0,
          "aggregate refuses node change");
    nodes[0] = "build/tests/host/sha256_hosttest";
    run.load_eligible = 0;
    CHECK(etude_budget_preparation(&history, &policy, &selected, &us, &count) == 0 && count == 0,
          "aggregate refuses unavailable load");
    run.load_eligible = 1;
    CHECK(put("aggregate.nodes", "changed") != NULL, "aggregate corrupt fixture");
    CHECK(etude_budget_preparation(&history, &policy, &selected, &us, &count) == 0 && count == 0,
          "aggregate refuses declaration mutation");
    eh_json_free(plan.json);
    eh_json_free(run.json);
    eh_json_free(step.json);
}
