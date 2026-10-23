/* Reserved-load startup, bad readiness and early exit on the native executor. */
#include "collection_load.h"
#include "collection_repeat.h"
static struct etude_ledger *repetition_ledger;
static struct etude_collection_load *repetition_owned;
static int repetition_claim;
static void repetition_cleanup(void)
{
    enum etude_cleanup cleanup = etude_collection_load_stop(repetition_owned);
    if (repetition_claim && repetition_ledger)
        etude_ledger_finish(repetition_ledger, "reserved-load", cleanup);
    repetition_claim = 0;
    etude_collection_load_free(repetition_owned);
    repetition_owned = NULL;
    if (repetition_ledger)
        etude_ledger_close(repetition_ledger);
    repetition_ledger = NULL;
}

static int repetition_load_role(const char *kind)
{
    if (!strcmp(kind, "memory")) {
        puts("not a readiness record");
        return 1;
    }
    fputs(ETUDE_LOAD_READY, stdout);
    fflush(stdout);
    if (!strcmp(kind, "disk"))
        return 1;
    host_wait_forever();
    return 0;
}

static void repetition_load_case(struct etude_ledger *ledger, enum etude_load_kind kind)
{
    struct etude_load load = {"fixture-load", kind, 1};
    struct etude_ledger_res cap[] = {
        {"cpu_millis", 10000}, {"memory_bytes", 1073741824}, {"disk_bytes", 1048576}};
    uint64_t amount[3];
    CHECK(!etude_collection_load_resources(&load, amount), "repeat: load resources are explicit");
    struct etude_ledger_res request[3];
    for (size_t i = 0; i < 3; i++) {
        request[i].name = cap[i].name;
        request[i].amount = amount[i];
    }
    int admitted = etude_ledger_admit(ledger, "reserved-load", "fixture", request, 3, cap, 3, 0);
    CHECK(!admitted, "repeat: load admitted before its processes start");
    if (admitted)
        return;
    repetition_claim = 1;
    struct etude_collection_load_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.program = self;
    spec.claim = "reserved-load";
    spec.workspace = fix_dir();
    spec.output = fix_path("repeat-load");
    spec.load = &load;
    spec.ledger = ledger;
    struct etude_collection_load *owned = etude_collection_load_start(&spec);
    repetition_owned = owned;
    CHECK(owned != NULL, "repeat: native load starts with registered processes");
    int ready = 0;
    while (owned && !ready)
        ready = etude_collection_load_wait(owned, 1000);
    while (owned && kind == ETUDE_LOAD_DISK && ready >= 0)
        ready = etude_collection_load_wait(owned, 1000);
    CHECK(kind == ETUDE_LOAD_CPU ? ready == 1 : ready == -1,
          "repeat: readiness is required and a failed load cannot pass");
    enum etude_cleanup cleanup = etude_collection_load_stop(owned);
    CHECK(cleanup == ETUDE_CLEANUP_CONFIRMED, "repeat: every owned load process is reaped");
    const char *record = etude_collection_load_record(owned);
    CHECK(strstr(record, "\"cleanup\":\"confirmed\"") && strstr(record, "\"keeper\":{") &&
              strstr(record, "\"process\":{") && strstr(record, "\"amount\":1"),
          "repeat: actual load identity and cleanup retained");
    CHECK(!etude_ledger_finish(ledger, "reserved-load", cleanup),
          "repeat: reservation ends only after confirmed load cleanup");
    repetition_claim = 0;
    etude_collection_load_free(owned);
    repetition_owned = NULL;
}

static void test_repetition_loads(void)
{
    struct etude_collection_repeat r = {0};
    CHECK(!etude_collection_repeat_option(&r, 'R', "3") && !etude_collection_repeat_valid(&r),
          "repeat: count requires explicit seed");
    CHECK(!etude_collection_repeat_option(&r, 'S', "0") &&
              !etude_collection_repeat_option(&r, 'L', "cpu:1") &&
              etude_collection_repeat_valid(&r),
          "repeat: zero seed and native CPU load are valid");
    CHECK(etude_collection_repeat_option(&r, 'L', "cpu:0") < 0 &&
              etude_collection_repeat_option(&r, 'L', "cpu:1025") < 0 &&
              etude_collection_repeat_option(&r, 'S', "1") < 0,
          "repeat: invalid or repeated configuration is refused");
    static int registered;
    if (!registered) {
        atexit(repetition_cleanup);
        registered = 1;
    }
    struct etude_ledger *ledger = etude_ledger_open(fix_path("repeat-ledger"));
    repetition_ledger = ledger;
    CHECK(ledger != NULL, "repeat: private ledger opens");
    if (ledger) {
        repetition_load_case(ledger, ETUDE_LOAD_CPU);
        repetition_load_case(ledger, ETUDE_LOAD_MEMORY);
        repetition_load_case(ledger, ETUDE_LOAD_DISK);
        repetition_cleanup();
    }
}

static int repetition_cases_only(void)
{
    test_repetition_loads();
    if (fails)
        return 1;
    completed = 1;
    fix_passed();
    puts("etude_legacyset_hosttest: repetitions pass");
    return 0;
}
