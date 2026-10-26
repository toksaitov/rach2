/* Reports and diagnostic streams remain separate in files and history. */
static int stream_evidence(const struct run *r)
{
    const char *line = case_attempt(r, "fixture.streams.fail", "i686", 0);
    struct json *doc = NULL;
    char error[256], path[PATH_LEN * 2];
    static char original[OUT_LEN], kept_bytes[OUT_LEN];
    if (!line || json_parse(line, strlen(line), 1, &doc, error, sizeof(error)))
        return 0;
    const struct json *data = json_member(doc, "data");
    const struct json *evidence = json_member(json_member(data, "result"), "evidence");
    int ok = evidence && evidence->type == JSON_ARRAY && evidence->count == 2;
    for (size_t i = 0; ok && i < 2; i++) {
        const struct json *locations = json_member(evidence->item[i], "locations");
        const char *source = locations && locations->type == JSON_ARRAY && locations->count == 1 &&
                                     locations->item[0]->type == JSON_STRING
                                 ? locations->item[0]->string
                                 : NULL;
        fix_format(path, sizeof(path), "%s/output/fixture.streams.fail.r0.%s", r->results,
                   i ? "err" : "log");
        ok = source && read_text(path, original, sizeof(original)) > 0 &&
             read_text(source, kept_bytes, sizeof(kept_bytes)) > 0 && !strcmp(original, kept_bytes);
    }
    json_free(doc);
    return ok;
}

static void test_streams(void)
{
    static const struct etude_legacy_member members[] = {
        {"streams", 0, 0, "cases/streams/streams.json", NULL}};
    static struct run r;
    char path[PATH_LEN * 2], diagnostics[1024];
    int before = fails;
    r.name = "streams";
    r.members = members;
    r.n = 1;
    set_run(&r);
    CHECK(r.status == 1 && r.read == 0, "streams: whole history of the failed set");
    CHECK(case_is(&r, "fixture.streams.pass", "i686", 0, "pass", NULL),
          "streams: stderr cannot fail a passing report");
    CHECK(case_is(&r, "fixture.streams.fail", "i686", 0, "failure", "ran failed"),
          "streams: stderr cannot pass a failed report");
    CHECK(output_has(&r, "fixture.streams.pass.r0", "@etude 1 assert ran pass") &&
              !output_has(&r, "fixture.streams.pass.r0", "stream diagnostic"),
          "streams: stdout keeps only the report");
    fix_format(path, sizeof(path), "%s/output/fixture.streams.pass.r0.err", r.results);
    CHECK(read_text(path, diagnostics, sizeof(diagnostics)) > 0 &&
              has_line(diagnostics, "@etude 1 assert ran fail diagnostic only"),
          "streams: complete stderr remains beside stdout");
    CHECK(stream_evidence(&r), "streams: failed history keeps both streams separately");
    CHECK(has_line(r.out, "stream diagnostic"), "streams: diagnostics remain visible");
    CHECK(r.claims == 0 && !r.leaks, "streams: claims and process memory released");
    show(&r, before);
}

static int stream_cases_only(void)
{
    test_streams();
    test_prepare();
    if (fails)
        return 1;
    completed = 1;
    fix_passed();
    puts("etude_legacyset_hosttest: streams pass");
    return 0;
}
