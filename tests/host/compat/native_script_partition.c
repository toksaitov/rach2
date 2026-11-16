/* The host-domain native partition keeps all ten original identities. */
static void script_copy_input(const char *source, const char *destination)
{
    FILE *in = fopen(source, "rb"), *out = fopen(fix_path(destination), "wb");
    char bytes[8192];
    size_t n;
    CHECK(in && out, "native script input copy opens");
    if (in && out)
        while ((n = fread(bytes, 1, sizeof(bytes), in)) != 0)
            CHECK(fwrite(bytes, 1, n, out) == n, "native script input copies whole");
    if (in) {
        CHECK(!ferror(in), "native script source reads whole");
        fclose(in);
    }
    if (out)
        CHECK(!fclose(out), "native script input closes");
}
static void native_script_inventory_fixture(void)
{
    char source[65], program[65], manifest[65], line[4096];
    hash("tests/host/a_hosttest.c", source);
    hash("build/tests/probe", program);
    hash("tests/cases/host.host.json", manifest);
    FILE *list = fopen("tests/cases/host/host.list", "rb");
    FILE *out = fopen(fix_path("build/tests/host-suite/scripts.inventory.json"), "wb");
    CHECK(list && out, "complete native script fixture inventory opens");
    unsigned count = 0;
    if (list && out) {
        fprintf(out,
                "{\"schema_version\":1,\"group\":\"scripts\",\"architecture\":\"none\","
                "\"manifest_sha256\":\"%s\",\"checks\":[",
                manifest);
        while (fgets(line, sizeof(line), list)) {
            if (line[0] == '#' || line[0] == '\n')
                continue;
            char *bar = strchr(line, '|');
            CHECK(bar != NULL, "script fixture list row has its separator");
            if (!bar)
                break;
            *bar = 0;
            fprintf(out,
                    "%s{\"assertion_id\":\"%s\",\"label\":\"partition probe\","
                    "\"mode\":\"shared\",\"program\":\"build/tests/probe\",\"sha256\":\"%s\","
                    "\"arguments\":[\"--probe\"],\"inputs\":[{\"path\":\"tests/host/a_hosttest.c\","
                    "\"sha256\":\"%s\"}],\"unsupported\":\"\"}",
                    count++ ? "," : "", line, program, source);
        }
        fputs("]}\n", out);
        CHECK(count == 47 && !ferror(list), "native partition fixture retains all 47 entries");
    }
    if (list)
        fclose(list);
    if (out)
        CHECK(!fclose(out), "native script inventory closes");
}
static void native_script_partition(void)
{
    const char *ids[] = {"krffs_open", "krffs_root_lock", "krffs_stat", "kstack", "endothers",
                         "chown",      "pagecache",       "anthropic",  "gemini", "fakeapi"};
    char output[32768];
    copy_probe();
    script_copy_input("tests/cases/host.host.json", "tests/cases/host.host.json");
    script_copy_input("tests/cases/host/host.list", "tests/cases/host/host.list");
    native_script_inventory_fixture();
    CHECK(!run_group(output, sizeof(output), "scripts_native", NULL),
          "host-domain native partition runs through the actual prepared driver");
    const char *begin = strstr(output, "@etude 1 begin host.host.scripts_native\n");
    const char *first = strstr(output, "@etude 1 assert ");
    CHECK(begin && first && begin < first, "native script report begins before its assertions");
    const char *at = output;
    for (unsigned i = 0; i < 10; i++) {
        char expected[128];
        snprintf(expected, sizeof(expected), "@etude 1 assert %s pass", ids[i]);
        const char *next = strstr(at, expected);
        CHECK(next != NULL, "each native assertion occurs in its original serial order");
        if (next)
            at = next + strlen(expected);
    }
    CHECK(strstr(output, "@etude 1 end 11") && !strstr(output, "assert tooling_image") &&
              !strstr(output, "assert ci "),
          "the native partition executes only its ten rows");
    script_driver_manifest(0);
    inventory("scripts", 0, NULL);
    CHECK(run_group(output, sizeof(output), "scripts_native", NULL) != 0 &&
              !strstr(output, "probe complete"),
          "native partition rejects an incomplete classification before any child starts");
}
