/* Real private authority files exercise policy races without installed writes.
 */
#include "../../../tools/etude/authority_internal.h"

static void authority_put(const char *name, const char *bytes)
{
    FILE *file = fopen(name, "wx");
    CHECK(file != NULL, "authority create");
    if (file) {
        CHECK(fwrite(bytes, 1, strlen(bytes), file) == strlen(bytes), "authority write");
        CHECK(fclose(file) == 0, "authority close");
        CHECK(chmod(name, 0600) == 0, "authority protect");
    }
}

static void authority_config(char binding[4096], char ledger[4096], char marker[4096])
{
    const char *directory = fix_path("authority");
    CHECK(mkdir(directory, 0700) == 0, "authority directory");
    char site[4096], gate[4096], archive[4096], state[4096], route[4096], claims[4096];
    char program[4096];
    snprintf(binding, 4096, "%s/binding", directory);
    snprintf(ledger, 4096, "%s/ledger", directory);
    snprintf(marker, 4096, "%s/physical", directory);
    snprintf(site, sizeof site, "%s/site", directory);
    snprintf(gate, sizeof gate, "%s/gate", directory);
    snprintf(archive, sizeof archive, "%s/archive", directory);
    snprintf(state, sizeof state, "%s/state", directory);
    snprintf(route, sizeof route, "%s/route", directory);
    snprintf(program, sizeof program, "%s/program", directory);
    CHECK(mkdir(ledger, 0700) == 0 && mkdir(archive, 0700) == 0, "authority roots");
    int n = snprintf(claims, sizeof claims, "%s/claims", ledger);
    CHECK(n > 0 && (size_t)n < sizeof claims, "authority claims path");
    authority_put(claims, "etude-ledger 1\n");
    authority_put(gate, "");
    authority_put(route, "{}\n");
    authority_put(program, "authority fixture program\n");
    CHECK(chmod(program, 0700) == 0, "authority program protect");
    authority_put(site, "etude-site 2\nendpoint linux\ncapacity 1000,4096,8192\n");
    char bytes[32768];
    n = snprintf(bytes, sizeof bytes,
                 "etude-authority 2\nledger %s\nsite %s\ngate %s\nstate %s\narchive %s\n"
                 "operator %lu\nprogram %s\nphysical_block %s\nroute %s\n",
                 ledger, site, gate, state, archive, (unsigned long)geteuid(), program, marker,
                 route);
    CHECK(n > 0 && (size_t)n < sizeof bytes, "authority binding size");
    authority_put(binding, bytes);
    CHECK(setenv("ETUDE_TEST_AUTHORITY", binding, 1) == 0, "authority fixture binding");
}

/** Check that a binding of format 1 is read with its unused `launcher_body`,
 * and that neither format is read with the other's fields. */
static void authority_formats(const char *binding)
{
    static const char *const rows[][3] = {
        {"one", "etude-authority 1", "launcher_body /launch_body.sh\n"},
        {"one-short", "etude-authority 1", ""},
        {"two-long", "etude-authority 2", "launcher_body /launch_body.sh\n"}};
    char fields[32768], bytes[33000], path[4096];
    struct authority_binding b;
    FILE *file = fopen(binding, "r");
    size_t n = file ? fread(fields, 1, sizeof fields - 1, file) : 0;
    CHECK(file && fclose(file) == 0 && n, "authority binding read back");
    fields[n] = 0;
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++) {
        snprintf(path, sizeof path, "%s/binding-%s", fix_path("authority"), rows[i][0]);
        snprintf(bytes, sizeof bytes, "%s%s%s", rows[i][1], strchr(fields, '\n'), rows[i][2]);
        authority_put(path, bytes);
        CHECK(setenv("ETUDE_TEST_AUTHORITY", path, 1) == 0, "authority format binding");
        CHECK((authority_binding_read(&b) == 0) == (i == 0), rows[i][0]);
    }
    CHECK(setenv("ETUDE_TEST_AUTHORITY", binding, 1) == 0, "authority fixture binding restored");
}

static void authority_env(const struct etude_authority_phase *phase, const char *name)
{
    if (name) {
        CHECK(setenv("ETUDE_AUTHORITY_NONCE", phase->nonce, 1) == 0, "authority capability");
        CHECK(setenv("ETUDE_AUTHORITY_RUN", phase->run_id, 1) == 0, "authority run");
        CHECK(setenv("ETUDE_AUTHORITY_PHASE", name, 1) == 0, "authority phase");
    } else {
        unsetenv("ETUDE_AUTHORITY_NONCE");
        unsetenv("ETUDE_AUTHORITY_RUN");
        unsetenv("ETUDE_AUTHORITY_PHASE");
    }
}

static void test_authority(void)
{
    char binding[4096], ledger[4096], marker[4096], alias[4096];
    authority_config(binding, ledger, marker);
    char site_path[4096], route_path[4096], site_alias[4096], route_alias[4096];
    snprintf(site_path, sizeof site_path, "%s/site", fix_path("authority"));
    snprintf(route_path, sizeof route_path, "%s/route", fix_path("authority"));
    snprintf(site_alias, sizeof site_alias, "%s/site-alias", fix_path("authority"));
    snprintf(route_alias, sizeof route_alias, "%s/route-alias", fix_path("authority"));
    struct authority_binding route_binding;
    CHECK(symlink(site_path, site_alias) == 0 && symlink(route_path, route_alias) == 0,
          "route aliases");
    char program[4096];
    snprintf(program, sizeof program, "%s/program", fix_path("authority"));
    CHECK(chmod(program, 0720) == 0, "authority program group writable");
    CHECK(authority_route(site_alias, route_alias, &route_binding) != 0,
          "group writable authority program refused");
    CHECK(chmod(program, 0700) == 0, "authority program protection restored");
    CHECK(authority_route(site_alias, route_alias, &route_binding) == 0,
          "route and site aliases match protected identities");
    CHECK(authority_route(route_path, route_path, &route_binding) != 0, "different site refused");
    CHECK(authority_route(site_path, site_path, &route_binding) != 0, "different route refused");
    CHECK(authority_route(site_path, NULL, &route_binding) != 0, "absent route refused");
    authority_formats(binding);
    struct etude_authority *normal = NULL, *exclusive = NULL;
    struct etude_authority_phase phase = {
        .run_id = "fixture-authority",
        .phase = "producer",
        .commit = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .nonce = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};
    CHECK(etude_process_state(getpid(), &phase.owner) == 1, "authority actual owner");
    const struct etude_ledger_res capacity[] = {{"cpu_millis", 1000}};
    CHECK(etude_authority_open(ledger, &normal) == 0 && normal, "enrolled ledger");
    CHECK(etude_authority_claim(normal, "ordinary", capacity, 1) == 0, "ordinary claim");
    CHECK(etude_authority_freeze(&phase, &exclusive) != 0 && !exclusive,
          "gate retained before policy read");
    etude_authority_close(normal);
    normal = NULL;
    CHECK(etude_authority_freeze(&phase, &exclusive) == 0 && exclusive, "empty freeze");
    CHECK(etude_authority_allow(exclusive, &phase) == 0, "allow exact phase");
    etude_authority_close(exclusive);
    exclusive = NULL;
    int n = snprintf(alias, sizeof alias, "%s.alias", ledger);
    CHECK(n > 0 && (size_t)n < sizeof alias && symlink(ledger, alias) == 0, "ledger alias");
    CHECK(etude_authority_open(alias, &normal) == 0 && normal, "alias enrolled by identity");
    CHECK(etude_authority_claim(normal, "stale-enabled-policy", capacity, 1) != 0,
          "stale policy cannot bypass block");
    authority_env(&phase, "producer");
    CHECK(etude_authority_claim(normal, "owned-phase", capacity, 1) == 0, "owned phase claim");
    CHECK(setenv("ETUDE_AUTHORITY_NONCE", "bad", 1) == 0, "wrong capability");
    CHECK(etude_authority_claim(normal, "ordinary-account", capacity, 1) != 0,
          "ordinary caller cannot reuse projection");
    authority_env(&phase, NULL);
    etude_authority_close(normal);
    normal = NULL;
    CHECK(etude_authority_reopen(&phase, &exclusive) == 0, "owner reopens empty endpoint");
    CHECK(etude_authority_block(exclusive) == 0, "block phase after cleanup");
    etude_authority_close(exclusive);
    exclusive = NULL;
    struct etude_reading grant;
    CHECK(etude_authority_physical(&phase, 0, &grant) == 0, "actual physical grant");
    CHECK(grant.resolution_us > 0 && access(marker, F_OK) == 0, "grant reading and marker");
    CHECK(etude_authority_reopen(&phase, &exclusive) == 0, "restoration owns gate");
    CHECK(etude_authority_restore(exclusive) == 0, "exact policy restoration");
    etude_authority_close(exclusive);
    exclusive = NULL;
    CHECK(etude_authority_open(ledger, &normal) == 0 && normal, "restored ordinary ledger");
    CHECK(etude_authority_claim(normal, "premature", capacity, 1) != 0,
          "physical marker protects restored policy");
    etude_authority_close(normal);
    CHECK(etude_authority_physical(&phase, 1, NULL) == 0, "release after cleanup");
    CHECK(etude_authority_open(ledger, &normal) == 0 && normal, "normal reopening");
    CHECK(etude_authority_claim(normal, "ordinary-after", capacity, 1) == 0,
          "ordinary admission restored");
    etude_authority_close(normal);
    unsetenv("ETUDE_TEST_AUTHORITY");
}
