/* Both scheduling partitions validate one complete immutable inventory. */
enum partition_fault {
    PARTITION_OK,
    PARTITION_OVERLAP,
    PARTITION_OMITTED,
    PARTITION_EXTRA,
    PARTITION_SHARED_PLATFORM
};

static void partition_manifest(enum partition_fault fault)
{
    FILE *f = fopen(fix_path("tests/cases/host.host.json"), "wb");
    CHECK(f != NULL, "partition manifest opens");
    if (!f)
        return;
    fputs("{\"cases\":[{\"case_id\":\"host.host.etude\",\"assertions\":["
          "{\"assertion_id\":\"listed\"},{\"assertion_id\":\"etude_a\"}",
          f);
    if (fault == PARTITION_SHARED_PLATFORM)
        fputs(",{\"assertion_id\":\"etude_platform\"}", f);
    fputs("]},{\"case_id\":\"host.host.etude_quiet\",\"assertions\":["
          "{\"assertion_id\":\"listed\"}",
          f);
    if (fault != PARTITION_OMITTED && fault != PARTITION_SHARED_PLATFORM)
        fputs(",{\"assertion_id\":\"etude_platform\"}", f);
    if (fault == PARTITION_OVERLAP)
        fputs(",{\"assertion_id\":\"etude_a\"}", f);
    if (fault == PARTITION_EXTRA)
        fputs(",{\"assertion_id\":\"etude_missing\"}", f);
    fputs("]}]}\n", f);
    CHECK(!fclose(f), "partition manifest closes");
}

static void partition_inventory(int missing, int wrong_mode)
{
    char manifest[65], program[65], source[65];
    hash("tests/cases/host.host.json", manifest);
    hash("build/tests/probe", program);
    FILE *f = fopen(fix_path("build/tests/host-suite/etude.inventory.json"), "wb");
    CHECK(f != NULL, "partition inventory opens");
    if (!f)
        return;
    fprintf(f,
            "{\"schema_version\":1,\"group\":\"etude\",\"architecture\":\"none\","
            "\"manifest_sha256\":\"%s\",\"checks\":[",
            manifest);
    for (int i = 0; i < (missing ? 1 : 2); i++) {
        char path[128];
        const char *id = i ? "etude_platform" : "etude_a";
        snprintf(path, sizeof(path), "tests/host/%s_hosttest.c", id);
        hash(path, source);
        fprintf(f,
                "%s{\"assertion_id\":\"%s\",\"label\":\"partition probe\","
                "\"mode\":\"%s\",\"program\":\"build/tests/probe\",\"sha256\":\"%s\","
                "\"arguments\":[\"--probe\"],\"inputs\":[{\"path\":\"%s\",\"sha256\":\"%s\"}]}",
                i ? "," : "", id, i && !wrong_mode ? "alone" : "shared", program, path, source);
    }
    fputs("]}\n", f);
    CHECK(!fclose(f), "partition inventory closes");
}

static void partition_refusal(enum partition_fault fault, int missing, int wrong_mode,
                              const char *why)
{
    char output[32768];
    partition_manifest(fault);
    partition_inventory(missing, wrong_mode);
    CHECK(run_group(output, sizeof(output), "etude", NULL) != 0 &&
              strstr(output, "assert listed fail") && !strstr(output, "probe complete"),
          why);
}

static void partitions(void)
{
    char output[32768];
    copy_probe();
    put("tests/host/etude_a_hosttest.c", "/* shared prepared source */\n");
    put("tests/host/etude_platform_hosttest.c",
        "/* host-alone: native platform fixture isolation */\n");
    partition_manifest(PARTITION_OK);
    partition_inventory(0, 0);
    CHECK(run_group(output, sizeof(output), "etude", NULL) == 0 &&
              strstr(output, "assert etude_a pass") && !strstr(output, "assert etude_platform"),
          "shared partition executes only its declared fixture");
    CHECK(run_group(output, sizeof(output), "etude_quiet", NULL) == 0 &&
              strstr(output, "assert etude_platform pass") && !strstr(output, "assert etude_a"),
          "quiet partition executes only its declared fixture from the same inventory");
    CHECK(run_group(output, sizeof(output), "etude", "etude_platform") == 0 &&
              strstr(output, "begin host.host.etude_quiet") &&
              strstr(output, "assert etude_platform pass") && !strstr(output, "assert etude_a"),
          "an existing one-fixture request resolves its owning partition");
    partition_refusal(PARTITION_OVERLAP, 0, 0, "overlapping declarations refuse before launch");
    partition_refusal(PARTITION_OMITTED, 0, 0,
                      "an omitted quiet declaration refuses before launch");
    partition_refusal(PARTITION_EXTRA, 0, 0, "a missing companion fixture refuses before launch");
    partition_refusal(PARTITION_OK, 1, 0, "an incomplete inventory refuses before shared launch");
    partition_refusal(PARTITION_OK, 0, 1, "a quiet mode mismatch refuses before launch");
    partition_refusal(PARTITION_SHARED_PLATFORM, 0, 0,
                      "an alone platform entry in the shared partition refuses before launch");
}

static void script_driver_manifest(int duplicate)
{
    const char *suffixes[] = {"", "_native", "_images", "_build", "_tools"};
    FILE *f = fopen(fix_path("tests/cases/host.host.json"), "wb");
    CHECK(f != NULL, "script driver manifest opens");
    if (!f)
        return;
    fputs("{\"cases\":[", f);
    for (unsigned i = 0; i < 5; i++) {
        fprintf(f,
                "%s{\"case_id\":\"host.host.scripts%s\",\"assertions\":["
                "{\"assertion_id\":\"listed\"}",
                i ? "," : "", suffixes[i]);
        if (i < 2 || (duplicate && i == 2))
            fprintf(f, ",{\"assertion_id\":\"%c\"}", i == 1 ? 'b' : 'a');
        fputs("]}", f);
    }
    fputs("]}\n", f);
    CHECK(!fclose(f), "script driver manifest closes");
}

static void script_driver_partitions(void)
{
    char output[32768];
    copy_probe();
    CHECK(!mkdir(fix_path("tests/cases/host"), 0700), "script list directory created");
    put("tests/cases/host/host.list", "a|probe a\nb|probe b\n");
    script_driver_manifest(0);
    inventory("scripts", 0, NULL);
    CHECK(!run_group(output, sizeof(output), "scripts", NULL) && strstr(output, "assert a pass") &&
              strstr(output, "assert b pass"),
          "standalone scripts driver executes the complete declaration union");
    CHECK(!run_group(output, sizeof(output), "scripts", "b") && strstr(output, "assert b pass") &&
              !strstr(output, "assert a"),
          "standalone script selection preserves its one-check interface");
    script_driver_manifest(1);
    inventory("scripts", 0, NULL);
    CHECK(run_group(output, sizeof(output), "scripts", NULL) != 0 &&
              strstr(output, "assert listed fail") && !strstr(output, "probe complete"),
          "overlapping script declarations refuse before any standalone child starts");
}
