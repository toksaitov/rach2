/* Both scheduling partitions validate one complete immutable inventory. */
enum partition_fault { PARTITION_OK, PARTITION_OVERLAP, PARTITION_OMITTED, PARTITION_EXTRA };

static void partition_manifest(enum partition_fault fault)
{
    FILE *f = fopen(fix_path("tests/cases/host.host.json"), "wb");
    CHECK(f != NULL, "partition manifest opens");
    if (!f)
        return;
    fputs("{\"cases\":[{\"case_id\":\"host.host.etude\",\"assertions\":["
          "{\"assertion_id\":\"listed\"},{\"assertion_id\":\"etude_a\"}]},"
          "{\"case_id\":\"host.host.etude_quiet\",\"assertions\":["
          "{\"assertion_id\":\"listed\"}",
          f);
    if (fault != PARTITION_OMITTED)
        fputs(",{\"assertion_id\":\"etude_b\"}", f);
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
        snprintf(path, sizeof(path), "tests/host/etude_%c_hosttest.c", 'a' + i);
        hash(path, source);
        fprintf(f,
                "%s{\"assertion_id\":\"etude_%c\",\"label\":\"partition probe\","
                "\"mode\":\"%s\",\"program\":\"build/tests/probe\",\"sha256\":\"%s\","
                "\"arguments\":[\"--probe\"],\"inputs\":[{\"path\":\"%s\",\"sha256\":\"%s\"}]}",
                i ? "," : "", 'a' + i, i && !wrong_mode ? "alone" : "shared", program, path,
                source);
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
    put("tests/host/etude_b_hosttest.c", "/* host-alone: private response check */\n");
    partition_manifest(PARTITION_OK);
    partition_inventory(0, 0);
    CHECK(run_group(output, sizeof(output), "etude", NULL) == 0 &&
              strstr(output, "assert etude_a pass") && !strstr(output, "assert etude_b"),
          "shared partition executes only its declared fixture");
    CHECK(run_group(output, sizeof(output), "etude_quiet", NULL) == 0 &&
              strstr(output, "assert etude_b pass") && !strstr(output, "assert etude_a"),
          "quiet partition executes only its declared fixture from the same inventory");
    CHECK(run_group(output, sizeof(output), "etude", "etude_b") == 0 &&
              strstr(output, "begin host.host.etude_quiet") &&
              strstr(output, "assert etude_b pass") && !strstr(output, "assert etude_a"),
          "an existing one-fixture request resolves its owning partition");
    partition_refusal(PARTITION_OVERLAP, 0, 0, "overlapping declarations refuse before launch");
    partition_refusal(PARTITION_OMITTED, 0, 0,
                      "an omitted quiet declaration refuses before launch");
    partition_refusal(PARTITION_EXTRA, 0, 0, "a missing companion fixture refuses before launch");
    partition_refusal(PARTITION_OK, 1, 0, "an incomplete inventory refuses before shared launch");
    partition_refusal(PARTITION_OK, 0, 1, "a quiet mode mismatch refuses before launch");
}
