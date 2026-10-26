/* Run the real scenario operations against a recorded executor result. */
#include "etude/qemu.h"
static const struct etude_machine_result *cleanup_result(const struct etude_machine *);
#define etude_machine_result(machine) cleanup_result(machine)
#include "../../../tools/etude/posix/scenario.c"
#undef etude_machine_result

#define fix_dir    cleanup_fix_dir
#define fix_path   cleanup_fix_path
#define fix_passed cleanup_fix_passed
#define fix_format cleanup_fix_format
#include "fixdir_host.c"

static const struct etude_machine_result *cleanup_injected;
static int cleanup_failures;

/* Ordinary scenario rows retain the real launcher's result accessor. */
static const struct etude_machine_result *cleanup_result(const struct etude_machine *m)
{
    return cleanup_injected != NULL && m == NULL ? cleanup_injected : etude_machine_result(m);
}

#define CLEANUP_CHECK(test)                                                      \
    do {                                                                         \
        if (!(test)) {                                                           \
            fprintf(stderr, "scenario cleanup: line %d: %s\n", __LINE__, #test); \
            cleanup_failures++;                                                  \
        }                                                                        \
    } while (0)

static void cleanup_refused(struct etude_case *c, struct etude_case_machine *m,
                            struct etude_machine_result *r, const char *file)
{
    struct etude_case_machine *next = NULL, other = {.c = c, .decl = &c->decls[1]};
    struct disk_binding held = {.machine = other.decl, .name = "disk", .path = (char *)file};
    struct etude_termination end;

    c->unconfirmed = 0;
    CLEANUP_CHECK(etude_wait_end(m, &end) == ETUDE_LOST);
    CLEANUP_CHECK(end.end == ETUDE_END_SHUTDOWN && c->unconfirmed);
    CLEANUP_CHECK(disk_active(c, file));
    CLEANUP_CHECK(etude_bind_disk(c, "owner", "disk", file) == ETUDE_REFUSED);
    CLEANUP_CHECK(etude_bind_disk(c, "other", "disk", file) == ETUDE_REFUSED);
    CLEANUP_CHECK(etude_boot(c, "owner", NULL, &next, &end) == ETUDE_REFUSED);
    CLEANUP_CHECK(next == NULL && r->end == ETUDE_END_SHUTDOWN);
    c->bindings = &held;
    CLEANUP_CHECK(make_profile(&other, NULL) == ETUDE_REFUSED);
    c->bindings = NULL;
}

int etude_test_scenario_cleanup(void)
{
    struct etude_exec_result process = {.cleanup = ETUDE_CLEANUP_UNCONFIRMED};
    struct etude_machine_result result = {
        .state = ETUDE_MACHINE_ENDED, .end = ETUDE_END_SHUTDOWN, .qemu = &process};
    struct machine_decl decls[2] = {{.id = "owner", .helper = 1}, {.id = "other", .helper = 1}};
    struct etude_case c = {.cur = 0, .decls = decls, .decl_count = 2};
    struct etude_case_machine m = {.c = &c, .decl = &decls[0], .ended = 1};
    struct etude_termination end;
    const char *file = cleanup_fix_path("disk");
    char resolved[4096];
    FILE *out = fopen(file, "wb");
    struct eh_json *profile = eh_json_parse("{\"disks\":[{\"name\":\"disk\",\"size_bytes\":1}]}");

    CLEANUP_CHECK(out != NULL && profile != NULL);
    if (out != NULL)
        CLEANUP_CHECK(fclose(out) == 0);
    if (profile == NULL)
        return 1;
    if (realpath(file, resolved) == NULL) {
        eh_json_free(profile);
        return 1;
    }
    file = resolved;
    c.scratch = cleanup_fix_dir();
    c.machines = &m;
    m.disks[0].path = file;
    decls[0].now = &m;
    decls[0].qemu = decls[1].qemu = profile;
    cleanup_injected = &result;
    cleanup_refused(&c, &m, &result, file);
    result.qemu = NULL;
    cleanup_refused(&c, &m, &result, file);
    result.qemu = &process;
    process.cleanup = ETUDE_CLEANUP_CONFIRMED;
    CLEANUP_CHECK(etude_wait_end(&m, &end) == ETUDE_OK && end.end == ETUDE_END_SHUTDOWN);
    CLEANUP_CHECK(!disk_active(&c, file));
    CLEANUP_CHECK(etude_bind_disk(&c, "owner", "disk", file) == ETUDE_OK);
    CLEANUP_CHECK(etude_bind_disk(&c, "other", "disk", file) == ETUDE_OK);
    while (c.bindings != NULL) {
        struct disk_binding *b = c.bindings;
        c.bindings = b->next;
        free(b->name);
        free(b->path);
        free(b);
    }
    cleanup_injected = NULL;
    eh_json_free(profile);
    if (!cleanup_failures)
        cleanup_fix_passed();
    return cleanup_failures != 0;
}
