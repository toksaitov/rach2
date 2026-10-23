/* Independent repeated runs share policy keys and retain their actual IDs. */
#include "collection_repeat.h"
#include "compare.h"

static void repetition_identity(const struct eh_json *data, const char *run)
{
    struct eh_json *reproduce = measurement_blob(eh_json_get(data, "reproduce"));
    const struct eh_json *record = eh_json_get(reproduce, "repetition");
    const struct eh_json *identity = eh_json_get(record, "run_id");
    CHECK(identity && identity->type == 's' && !strcmp(identity->string, run),
          "repeat key: reproduction keeps the actual run identity");
    const struct eh_json *items = eh_json_get(record, "items");
    size_t count = 0;
    for (const struct eh_json *item = items ? items->child : NULL; item; item = item->next) {
        const char *id = cmp_text(item, "item_id");
        CHECK(id && !strncmp(id, run, strlen(run)) && id[strlen(run)] == ':',
              "repeat key: reproduction keeps each qualified item identity");
        count++;
    }
    CHECK(count > 0, "repeat key: reproduction retains allocated items");
    eh_json_free(reproduce);
}

static struct eh_json *repetition_key_run(int variant)
{
    static const struct etude_legacy_member members[] = {
        {"plain", 0, 0, ONE("plain"), NULL}, {"side-x", 0, 0, ONE("side-x"), NULL}};
    char id[80], record[PATH_LEN], site[PATH_LEN];
    struct run r = {0};
    struct etude_legacy_set_opts o;
    struct etude_collection_repeat repeat = {0};
    fix_format(id, sizeof id, "repeat-comparison-%d", variant);
    measurement_paths(&r, id);
    r.members = members;
    r.n = variant == 3 ? 2 : 1;
    r.width = 1;
    opts_for(&o, &r);
    CHECK(!etude_collection_repeat_option(&repeat, 'S', variant == 2 ? "43" : "42"),
          "repeat key: seed is declared");
    CHECK(!etude_collection_repeat_option(&repeat, 'R', variant == 4 ? "2" : "1"),
          "repeat key: repetitions are declared");
    if (variant == 5)
        CHECK(!etude_collection_repeat_option(&repeat, 'L', "cpu:1"),
              "repeat key: reserved load is declared");
    o.repeat = &repeat;
    fix_format(record, sizeof record, "%s/repeat-comparison.record", fix_dir());
    measurement_record_file(record);
    o.prepared = 1;
    o.record = record;
    o.native_locks = locks;
    o.run_id = id;
    o.commit = measurement_commit;
    o.stop = &stop_signal;
    o.site = in(site, sizeof site, "site.permits");
    o.out = fopen(r.out_file, "wb");
    CHECK(o.out != NULL, "repeat key: output opens");
    r.status = o.out ? etude_legacy_set(&o) : 2;
    if (o.out)
        fclose(o.out);
    history_read(&r);
    CHECK(!r.status && !r.read && count_kind(&r.h, "attempt") == (variant >= 3 && variant <= 4 ? 2 : 1),
          "repeat key: real prepared collection and all allocated attempts pass");
    struct eh_json *plan = measurement_record(&r, "plan");
    repetition_identity(eh_json_get(plan, "data"), id);
    history_free(&r.h);
    return plan;
}

static const struct eh_json *repetition_key_item(const struct eh_json *data)
{
    const struct eh_json *items = eh_json_get(data, "items");
    for (const struct eh_json *item = items ? items->child : NULL; item; item = item->next)
        if (!strcmp(cmp_text(item, "case_id"), "fixture.plain.one") &&
            measurement_number(item, "repetition") == 0)
            return item;
    return NULL;
}

static void test_repetition_keys(void)
{
    unsetenv("RACH2_PORT_OFFSET");
    catch_stops();
    stop_signal = 0;
    struct eh_json *baseline = repetition_key_run(0);
    const struct eh_json *base = eh_json_get(baseline, "data");
    const struct eh_json *item = repetition_key_item(base);
    CHECK(item != NULL, "repeat key: baseline item exists");
    struct cmp_key a, b;
    cmp_item_key(base, item, &a);
    for (int variant = 1; variant < 6; variant++) {
        struct eh_json *plan = repetition_key_run(variant);
        const struct eh_json *data = eh_json_get(plan, "data");
        item = repetition_key_item(data);
        CHECK(item != NULL, "repeat key: matching case item exists");
        cmp_item_key(data, item, &b);
        CHECK((cmp_key_differs(&a, &b, 0, CMP_PLAN, NULL, 0) != 0) == (variant != 1),
              "repeat key: only seed, selection, repetitions or load changes the key");
        CHECK(!strcmp(a.value[CMP_PLAN - 1], b.value[CMP_PLAN - 1]) == (variant == 1),
              "repeat key: concurrency hashes stable policy and complete order");
        eh_json_free(plan);
    }
    eh_json_free(baseline);
    setenv("RACH2_PORT_OFFSET", "7000", 1);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
}
