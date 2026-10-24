/* The compiled scripts scenario follows host.list, so its steps must too. */
static struct json *scripts_manifest(void)
{
    FILE *f = fopen("tests/cases/host.host.json", "rb");
    struct json *doc = NULL;
    struct jbuf data = {0};
    char bytes[8192], error[256];
    size_t n;
    CHECK(f != NULL, "real host manifest opens");
    if (!f)
        return NULL;
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0)
        jbuf_put(&data, bytes, n);
    CHECK(!ferror(f), "real host manifest reads whole");
    fclose(f);
    CHECK(!json_parse(data.data, data.len, 1, &doc, error, sizeof(error)),
          "real host manifest parses");
    jbuf_free(&data);
    return doc;
}

static int scripts_step(const struct json *steps, size_t row, const char *id,
                        const char *stage, const char *machine)
{
    const struct json *step = steps && row < steps->count ? steps->item[row] : NULL;
    const char *actual_id = json_text(step, "step_id");
    const char *actual_stage = json_text(step, "stage");
    const char *actual_machine = json_text(step, "machine");
    return actual_id && !strcmp(actual_id, id) && actual_stage &&
           !strcmp(actual_stage, stage) &&
           (!machine || (actual_machine && !strcmp(actual_machine, machine)));
}

static void host_scripts_order(void)
{
    struct json *doc = scripts_manifest();
    const struct json *cases = json_member(doc, "cases"), *steps = NULL;
    for (size_t i = 0; cases && i < cases->count; i++) {
        const char *id = json_text(cases->item[i], "case_id");
        if (id && !strcmp(id, "host.host.scripts"))
            steps = json_member(cases->item[i], "steps");
    }
    const char *prefix[] = {"listed", "products", "prepare"};
    const char *stages[] = {"execute", "boot", "execute"};
    CHECK(steps && steps->count >= 3, "scripts scenario declares its steps");
    for (size_t i = 0; steps && i < 3 && i < steps->count; i++) {
        CHECK(scripts_step(steps, i, prefix[i], stages[i], i == 1 ? "products" : NULL),
              "scripts setup prefix remains declared first");
    }
    FILE *f = fopen("tests/cases/host/host.list", "rb");
    CHECK(f != NULL, "real host list opens");
    char line[4096];
    size_t row = 3;
    while (f && fgets(line, sizeof(line), f)) {
        char *start = line + strspn(line, " \t\r\n");
        if (!*start || *start == '#')
            continue;
        char *bar = strchr(start, '|');
        if (bar)
            *bar = 0;
        if (bar && !strcmp(start, "doom_libc")) {
            CHECK(scripts_step(steps, row, "doom-i686", "boot", "doom-i686"),
                  "Doom's matching-ABI helper boots immediately before its execution step");
            row++;
        }
        CHECK(bar && scripts_step(steps, row, start, "execute", NULL),
              "every host.list row has the same declared execution step in order");
        row++;
    }
    if (f) {
        CHECK(!ferror(f), "real host list reads whole");
        fclose(f);
    }
    CHECK(steps && row == steps->count, "no script execution step is omitted or extra");
    json_free(doc);
}
