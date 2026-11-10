/* Exercise the real partition dispatcher with recorded helper operations. */
#include "../../../tests/cases/host/host_scripts_scenario.c"

struct etude_case {
    const char *id, *manifest;
    unsigned boots, commands, assertions, dispatched, next, listed;
};
struct etude_case_machine {
    struct etude_case *owner;
};
static struct host_script_row script_rows[HOST_SCRIPT_ROWS];
static struct etude_case_machine script_machine;

const char *etude_case_id(const struct etude_case *c)
{
    return c->id;
}
const char *etude_artifact(struct etude_case *c, const char *name)
{
    if (!strcmp(name, "host-list"))
        return "tests/cases/host/host.list";
    CHECK(!strcmp(name, "host-cases"), "dispatcher requests its declared inventory");
    return c->manifest;
}
enum etude_status etude_step(struct etude_case *c, const char *step)
{
    (void)c;
    (void)step;
    return ETUDE_OK;
}
void etude_assert(struct etude_case *c, const char *id, int held, const char *detail)
{
    (void)detail;
    if (!strcmp(id, "listed")) {
        c->listed = held;
        return;
    }
    CHECK(held && c->assertions < c->dispatched && c->next &&
              !strcmp(id, script_rows[c->next - 1].id),
          "every dispatched row reports its result");
    c->assertions++;
}
void etude_notrun(struct etude_case *c, const char *id, const char *reason)
{
    (void)c;
    (void)id;
    (void)reason;
    CHECK(0, "a valid partition runs every selected check");
}
void etude_progress(struct etude_case *c, const char *witness, uint64_t units)
{
    CHECK(!strcmp(witness, "checks") && units == c->assertions,
          "partition progress counts completed selected checks");
}
enum etude_status etude_boot(struct etude_case *c, const char *name, const char *const disks[],
                             struct etude_case_machine **m, struct etude_termination *end)
{
    CHECK(!strcmp(name, "products") && !disks, "partition boots its own products helper");
    c->boots++;
    script_machine.owner = c;
    *m = &script_machine;
    memset(end, 0, sizeof(*end));
    return ETUDE_OK;
}
enum etude_status etude_guest_command(struct etude_case_machine *m, const char *const argv[],
                                      const char *const env[], const char *input,
                                      const char *output, struct etude_guest_result *r)
{
    (void)argv;
    (void)output;
    CHECK(!env && !input, "workspace commands retain their explicit environment contract");
    m->owner->commands++;
    memset(r, 0, sizeof(*r));
    r->complete = 1;
    r->termination.end = ETUDE_END_EXITED;
    r->cleanup = ETUDE_CLEANUP_CONFIRMED;
    return ETUDE_OK;
}
int host_assets_stage(struct etude_case *c, struct etude_case_machine *m)
{
    return m->owner == c;
}
static int script_dispatch(struct etude_case *c, const char *id, enum host_script_kind kind)
{
    int part = host_script_case_part(c->id);
    while (c->next < HOST_SCRIPT_ROWS && (int)script_rows[c->next].part != part)
        c->next++;
    int valid = c->next < HOST_SCRIPT_ROWS && !strcmp(script_rows[c->next].id, id) &&
                script_rows[c->next].kind == kind;
    CHECK(valid, "actual dispatch preserves the selected inventory order and backend");
    c->next++;
    c->dispatched++;
    return valid;
}
enum etude_status etude_host(struct etude_case *c, char *const argv[], const char *output,
                             struct etude_termination *end)
{
    enum host_script_kind kind = !strcmp(argv[0], "fakeapi") ? SCRIPT_PROTOCOL : SCRIPT_KERNEL;
    CHECK(!argv[1] && !strcmp(argv[0], kind == SCRIPT_PROTOCOL ? "fakeapi" : output),
          "native adapter keeps its original executable and assertion label");
    script_dispatch(c, output, kind);
    memset(end, 0, sizeof(*end));
    end->end = ETUDE_END_EXITED;
    return ETUDE_OK;
}
int host_image_check(struct etude_case *c, struct etude_case_machine *m, const char *id)
{
    CHECK(m->owner == c, "image checks use their own products helper");
    return script_dispatch(c, id, SCRIPT_IMAGE);
}
int host_build_check(struct etude_case *c, struct etude_case_machine *m, const char *id)
{
    CHECK(m->owner == c, "build checks use their own products helper");
    return script_dispatch(c, id, SCRIPT_BUILD);
}
int host_doom_check(struct etude_case *c, struct etude_case_machine *m)
{
    return host_build_check(c, m, "doom_libc");
}
int host_guest_check(struct etude_case *c, struct etude_case_machine *m, const char *program,
                     const char *id)
{
    CHECK(m->owner == c && !strcmp(program, "/host-products/host-flow-check"),
          "flow checks retain their prepared controller");
    return script_dispatch(c, id, SCRIPT_FLOW);
}

/* Negative manifests change declarations without changing the live list. */
static void script_bad_manifest(int duplicate)
{
    FILE *f = fopen(fix_path("script-parts.json"), "wb");
    CHECK(f != NULL, "negative partition manifest opens");
    if (!f)
        return;
    fputs("{\"cases\":[", f);
    for (unsigned p = 0; p < HOST_SCRIPT_PARTS; p++) {
        fprintf(f, "%s{\"case_id\":\"%s\",\"assertions\":[{\"assertion_id\":\"listed\"}",
                p ? "," : "", host_script_case(p));
        for (unsigned i = 0; i < HOST_SCRIPT_ROWS; i++) {
            if ((unsigned)script_rows[i].part != p || (!duplicate && i == 0))
                continue;
            fprintf(f, ",{\"assertion_id\":\"%s\"}", script_rows[i].id);
            if (duplicate && i == 0)
                fprintf(f, ",{\"assertion_id\":\"%s\"}", script_rows[i].id);
        }
        fputs("]}", f);
    }
    fputs("]}\n", f);
    CHECK(!fclose(f), "negative partition manifest closes");
}
static void host_scripts_dispatch(void)
{
    size_t n = 0;
    unsigned total = 0;
    CHECK(host_script_inventory("tests/cases/host/host.list", "tests/cases/host.host.json",
                                script_rows, &n),
          "five real partitions cover the complete live script inventory");
    for (unsigned p = 1; p < HOST_SCRIPT_PARTS; p++) {
        struct etude_case c = {.id = host_script_case(p), .manifest = "tests/cases/host.host.json"};
        unsigned expected = 0;
        for (unsigned i = 0; i < n; i++)
            expected += (unsigned)script_rows[i].part == p;
        CHECK(!etude_scenario(&c) && c.listed && c.dispatched == expected &&
                  c.assertions == expected && c.boots == (p != SCRIPT_NATIVE_PART) &&
                  c.commands == (p == SCRIPT_NATIVE_PART ? 0u : 4u),
              "each real partition dispatches every owned row with independent setup and cleanup");
        total += c.dispatched;
    }
    CHECK(total == HOST_SCRIPT_ROWS - 10, "all GNU script assertions run exactly once");
    for (int duplicate = 0; duplicate < 2; duplicate++) {
        script_bad_manifest(duplicate);
        struct etude_case c = {.id = host_script_case(SCRIPT_FLOW_PART),
                               .manifest = fix_path("script-parts.json")};
        CHECK(etude_scenario(&c) == 1 && !c.listed && !c.boots && !c.dispatched && !c.commands,
              "omitted or duplicate declarations refuse before helper or assertion dispatch");
    }
}
