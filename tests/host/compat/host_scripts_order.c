/* Each partition preserves its selected host.list order and setup steps. */
#include "../../../tests/cases/host/host_script_inventory.h"
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

static int scripts_step(const struct json *steps, size_t row, const char *id, const char *stage,
                        const char *machine)
{
    const struct json *step = steps && row < steps->count ? steps->item[row] : NULL;
    const char *actual_id = json_text(step, "step_id");
    const char *actual_stage = json_text(step, "stage");
    const char *actual_machine = json_text(step, "machine");
    return actual_id && !strcmp(actual_id, id) && actual_stage && !strcmp(actual_stage, stage) &&
           (!machine || (actual_machine && !strcmp(actual_machine, machine)));
}

static const struct json *script_steps(const struct json *doc, const char *id)
{
    const struct json *cases = json_member(doc, "cases");
    for (size_t i = 0; cases && i < cases->count; i++) {
        const char *actual = json_text(cases->item[i], "case_id");
        if (actual && !strcmp(actual, id))
            return json_member(cases->item[i], "steps");
    }
    return NULL;
}

static void script_part_order(const struct json *doc, const struct host_script_row *rows,
                              size_t count, unsigned part)
{
    const struct json *steps = script_steps(doc, host_script_case(part));
    const char *prefix[] = {"listed", "products", "prepare"};
    const char *stages[] = {"execute", "boot", "execute"};
    size_t row = part == SCRIPT_NATIVE_PART ? 1 : 3;
    CHECK(steps && steps->count >= row, "script partition declares its setup steps");
    for (size_t i = 0; i < row; i++)
        CHECK(scripts_step(steps, i, prefix[i], stages[i], i == 1 ? "products" : NULL),
              "script partition setup prefix remains first");
    for (size_t i = 0; i < count; i++) {
        if ((unsigned)rows[i].part != part)
            continue;
        if (!strcmp(rows[i].id, "doom_libc")) {
            CHECK(scripts_step(steps, row, "doom-i686", "boot", "doom-i686"),
                  "Doom's matching-ABI helper boots immediately before its execution step");
            row++;
        }
        CHECK(scripts_step(steps, row, rows[i].id, "execute", NULL),
              "each selected host.list row retains its execution step in order");
        row++;
    }
    CHECK(steps && row == steps->count, "no partition execution step is omitted or extra");
}

static void host_scripts_order(void)
{
    struct json *doc = scripts_manifest();
    struct host_script_row rows[HOST_SCRIPT_ROWS];
    size_t count = 0;
    CHECK(host_script_inventory("tests/cases/host/host.list", "tests/cases/host.host.json", rows,
                                &count),
          "all five partitions declare each host.list assertion exactly once");
    for (unsigned part = 1; part < HOST_SCRIPT_PARTS; part++)
        script_part_order(doc, rows, count, part);
    json_free(doc);
}
