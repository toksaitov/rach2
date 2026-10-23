/* Native history regressions use the production writer, reader and learner. */
#include "../../native/history.h"
static int nh_write(const char *path, const char *bytes)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    int ok = fputs(bytes, f) >= 0;
    return fclose(f) == 0 && ok;
}
static int nh_copy(const char *from, const char *to)
{
    char bytes[8192];
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    int ok = in && out;
    size_t n;
    while (ok && (n = fread(bytes, 1, sizeof(bytes), in)))
        ok = fwrite(bytes, 1, n, out) == n;
    if (in) { ok = !ferror(in) && ok; fclose(in); }
    if (out) ok = !fclose(out) && ok;
    return ok;
}
static void nh_plan_record(const struct eh_json *doc, const struct eh_json *data)
{
    const struct eh_json *exemption = eh_json_get(data, "exemption");
    const struct eh_json *runs = eh_json_get(exemption, "run_ids");
    const struct eh_json *attempts = eh_json_get(exemption, "attempt_ids");
    const struct eh_json *item = eh_json_get(data, "items")->child;
    const struct eh_json *allocated = eh_json_get(item, "attempt_ids");
    CHECK(!strcmp(cmp_text(data, "purpose"), "phase_first_run") && runs && runs->child &&
          !runs->child->next && !strcmp(runs->child->string, cmp_text(doc, "run_id")),
          "native first-run exemption binds only the actual run");
    CHECK(attempts && attempts->child && !attempts->child->next &&
          !strcmp(attempts->child->string, allocated->child->string),
          "native first-run exemption binds every allocated attempt");
    CHECK(!strcmp(cmp_text(eh_json_get(data, "selection"), "sha256"),
                  cmp_text(eh_json_get(exemption, "manifest"), "sha256")),
          "native first-run exemption binds the exact selection blob");
}
static void nh_run_record(const struct eh_json *data)
{
    const struct eh_json *setup = eh_json_get(eh_json_get(eh_json_get(data, "budget"), "one_time_setup_us"), "value");
    CHECK(setup && setup->type == 'z', "external native payload preparation remains explicitly unmeasured");
    const struct eh_json *result = eh_json_get(data, "result");
    const struct eh_json *end = eh_json_get(eh_json_get(data, "time"), "end_us");
    const struct eh_json *charged = eh_json_get(eh_json_get(eh_json_get(data, "budget"), "charged_us"), "value");
    if (!strcmp(cmp_text(result, "cleanup"), "unconfirmed")) {
        CHECK(end->type == 'z' && charged->type == 'z', "unconfirmed native cleanup has no completed run interval or charge");
    } else CHECK(end->type == 'n' && charged->type == 'n', "confirmed native cleanup has measured run interval and charge");
    const struct eh_json *cpu = eh_json_get(eh_json_get(data, "resources"), "tree_cpu_us");
    CHECK(!strcmp(cmp_text(cpu, "quality"), "lower_bound"), "reported case CPU remains a lower bound on the whole collector tree");
}
static int nh_visit(const char *line, void *context)
{
    unsigned *records = context;
    struct eh_json *doc = eh_json_parse(line);
    const struct eh_json *data = eh_json_get(doc, "data");
    if (!strcmp(cmp_text(doc, "kind"), "attempt")) {
        const struct eh_json *r = eh_json_get(data, "resources");
        const struct eh_json *cpu = eh_json_get(eh_json_get(r, "tree_cpu_us"), "value");
        const struct eh_json *memory = eh_json_get(eh_json_get(r, "combined_peak_bytes"), "value");
        CHECK(cpu && cpu->type == 'n' && cpu->number == 41, "native attempt keeps reported CPU");
        CHECK(memory && memory->type == 'n' && memory->number == 8192, "native attempt keeps reported peak");
        CHECK(eh_json_get(data, "time") && eh_json_get(data, "queue_us"), "native attempt records clocks and queue");
    }
    if (!strcmp(cmp_text(doc, "kind"), "plan")) nh_plan_record(doc, data);
    if (!strcmp(cmp_text(doc, "kind"), "run")) nh_run_record(data);
    (*records)++;
    eh_json_free(doc);
    return 0;
}
static void nh_run(struct nc_config *conf, struct nc_inventory *inventory, unsigned n, int status)
{
    char result[1024], run[80];
    struct nc_history h;
    fix_format(result, sizeof(result), "%s/native-history/run%u", fix_dir(), n);
    fix_format(run, sizeof(run), "native-history-%u", n);
    conf->results = result; conf->run = run;
    CHECK(nc_mkdir(result), "native result fixture directory");
    int opened = nc_history_open(&h, conf, inventory, 0, NULL, 0);
    CHECK(opened, "native history plan opens and validates");
    if (!opened) { nc_history_close(&h); return; }
    CHECK(h.count == 1, "native plan allocates every selected item");
    CHECK(h.items[0].whole.samples == (n < 5 ? 0U : n > 6 ? 6U : 5U), "five complete successes teach native bounds");
    CHECK(h.items[0].whole.us == (n < 5 ? UINT64_C(1800000000) : UINT64_C(1000000)), "native bound uses initial value then floor");
    struct nc_measure *m = &h.items[0];
    if (n == 6) {
        char machine[128]; strcpy(machine, h.machine);
        strcpy(h.machine, "other-machine");
        CHECK(nc_bounds(&h) && m->whole.samples == 0, "native bounds reject another machine");
        strcpy(h.machine, machine);
        const char *arch = conf->arch;
        conf->arch = "i686";
        CHECK(nc_bounds(&h) && m->whole.samples == 0, "native bounds reject another architecture");
        conf->arch = arch;
        char digest[65]; strcpy(digest, m->workload);
        memset(m->workload, '0', 64); m->workload[64] = 0;
        CHECK(nc_bounds(&h) && m->whole.samples == 0, "native bounds reject changed workload");
        strcpy(m->workload, digest);
        CHECK(nc_bounds(&h) && m->whole.samples == 5, "failed whole run never teaches native bounds");
    }

    CHECK(nc_measure_start(&h, m), "native measurement starts");
    m->observer.stage(m->observer.context, "execute");
    static const char noise[] = "heartbeat\n@etude 1 progress invented 1\n";
    m->observer.output(m->observer.context, noise, sizeof(noise) - 1);
    const struct etude_progress *progress = etude_watchdog_progress(m->watchdog);
    CHECK(progress->advances == 0, "undeclared output is never native progress");
    m->observer.stage(m->observer.context, "capture");
    m->observer.stage(m->observer.context, "cleanup");
    nc_measure_end(m);
    struct etude_native_result r = {0};
    r.attempt.complete = 1; r.removed = 1;
    r.attempt.end = ETUDE_END_EXITED;
    r.attempt.cleanup = n == 7 ? ETUDE_CLEANUP_UNCONFIRMED : ETUDE_CLEANUP_CONFIRMED;
    r.attempt.process.pid = (long)getpid();
    r.attempt.cpu_us.quality = r.attempt.peak_bytes.quality = ETUDE_QUALITY_EXACT;
    r.attempt.cpu_us.value = 41; r.attempt.cpu_us.method = "fixture-cpu-v1";
    r.attempt.peak_bytes.value = 8192; r.attempt.peak_bytes.method = "fixture-peak-v1";
    CHECK(nc_history_attempt(&h, 0, &r, status, status ? "declared fixture failure" : NULL), "native attempt history validates");
    CHECK(nc_history_finish(&h), "native run history validates");
    CHECK(nc_history_close(&h), "closed native history is indexed after durable completion");
    char path[1200], schema[1200];
    fix_format(path, sizeof(path), "%s/history.jsonl", result);
    fix_format(schema, sizeof(schema), "%s/tools/etude/schemas/history.schema.json", conf->tree);
    uint64_t bad = 0;
    unsigned records = 0;
    CHECK(etude_history_read(path, schema, nh_visit, &records, &bad) == 0 && records == 7,
          "complete native plan, four stages, attempt and run are durable standard history");
}
static void nh_stalled_write(void);
static void test_native_history(void)
{
    char tree[1024], state[1024], path[1200], directory[1200];
    struct nc_config conf = {0};
    struct nc_inventory inventory = {0};
    fix_format(tree, sizeof(tree), "%s/native-history/tree", fix_dir());
    fix_format(state, sizeof(state), "%s/native-history/state", fix_dir());
    fix_format(directory, sizeof(directory), "%s/tests/native", tree);
    CHECK(nc_mkdir(directory), "native history fixture tree");
    fix_format(path, sizeof(path), "%s/tests/native/bounds.json", tree);
    CHECK(nh_copy("tests/native/bounds.json", path), "native declared bounds fixture");
    fix_format(directory, sizeof(directory), "%s/tools/etude/schemas", tree);
    CHECK(nc_mkdir(directory), "native history schema directory");
    fix_format(path, sizeof(path), "%s/tools/etude/schemas/history.schema.json", tree);
    CHECK(nh_copy("tools/etude/schemas/history.schema.json", path), "native history uses production schema");
    fix_format(path, sizeof(path), "%s/native-source.json", tree);
    CHECK(nh_write(path, "{\"commit\":\"0123456789abcdef0123456789abcdef01234567\"}\n"), "native source fixture");
    fix_format(path, sizeof(path), "%s/assets.sha256", tree);
    CHECK(nh_write(path, "fixture prepared bytes\n"), "native source inventory fixture");
    conf.tree = tree; conf.state = state; conf.arch = "x86_64"; conf.self = self;
    conf.purpose = "phase_first_run"; conf.machine = "fixture-native";
    conf.kernel_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    inventory.ncases = 1;
    inventory.cases[0].name = "fixture"; inventory.cases[0].argc = 1;
    inventory.cases[0].argv[0] = "fixture";
    nh_stalled_write();
    for (unsigned n = 0; n < 5; n++) nh_run(&conf, &inventory, n, 0);
    nh_run(&conf, &inventory, 5, 1);
    nh_run(&conf, &inventory, 6, 0);
    nh_run(&conf, &inventory, 7, 4);
    nh_run(&conf, &inventory, 8, 3);
    nh_run(&conf, &inventory, 9, 0);
    fix_format(directory, sizeof(directory), "%s/native-history/unclosed", fix_dir());
    conf.results = directory; conf.run = "native-unclosed";
    CHECK(nc_mkdir(directory), "unfinished history fixture directory");
    struct nc_history unfinished;
    int opened = nc_history_open(&unfinished, &conf, &inventory, 0, NULL, 0);
    CHECK(opened, "unfinished native history opens");
    CHECK(nc_history_close(&unfinished), "unfinished stream closes without publishing learning index");
    fix_format(path, sizeof(path), "%s/history/native-unclosed.path", state);
    CHECK(access(path, F_OK) != 0 && errno == ENOENT, "unfinished native run cannot teach bounds");
}

/* A real framed peer completes the handshake, then never drains its input. */
static int nh_stalled_peer(void)
{
    const char *boot = "0123456789abcdef";
    char line[2048], out[2048];
    const char *alive[] = {"1", "1", "1"};
    size_t n = wire_encode(out, sizeof(out), ETUDE_FRAME_NO_SESSION, boot, 0, 0, 0,
                           ETUDE_EV_ALIVE, 3, alive);
    if (!n || write_all(1, out, n) || !fgets(line, sizeof(line), stdin)) return 1;
    struct wire_frame hello;
    n = strlen(line);
    if (!n || wire_parse(line, n - 1, &hello) || hello.nfields != 3) return 1;
    const char *ready[] = {hello.field[0], "1", hello.field[2], "1", "1"};
    n = wire_encode(out, sizeof(out), hello.session, boot, 1, 1, hello.seq,
                    ETUDE_EV_READY, 5, ready);
    if (!n || write_all(1, out, n)) return 1;
    struct pollfd input = {0, 0, 0};
    while (poll(&input, 1, -1) < 0 && errno == EINTR) {}
    return input.revents & POLLHUP ? 0 : 1;
}
struct nh_limit { uint64_t end; unsigned calls; };
static uint64_t nh_deadline(void *context)
{
    struct nh_limit *limit = context;
    limit->calls++;
    return limit->end;
}
static void nh_stalled_write(void)
{
    char *argv[] = {self, "--stalled-peer", NULL};
    struct nh_limit limit = {0};
    struct etude_remote_observer observer = {0};
    observer.context = &limit; observer.deadline = nh_deadline;
    struct etude_remote_conf conf = {0};
    conf.argv = argv; conf.run_id = "stalled-reader"; conf.stage_us = 2000000;
    conf.observer = &observer;
    struct etude_remote *remote = NULL;
    char why[256];
    int opened = etude_remote_open(&conf, &remote, why, sizeof(why)) == ETUDE_REMOTE_OK;
    CHECK(opened, "stalled reader completes the actual framed handshake");
    if (!opened) return;
    struct etude_reading start, end;
    CHECK(etude_history_now(&start) == 0, "stalled-reader start clock");
    limit.end = start.monotonic_us + 50000;
    unsigned before = limit.calls;
    size_t size = 4 * 1024 * 1024;
    char *data = calloc(1, size);
    CHECK(data != NULL, "stalled-reader transfer bytes");
    if (data) {
        CHECK(etude_remote_put(remote, "large", data, size, "0600") != ETUDE_REMOTE_OK,
              "a stopped reader cannot leave a successful transfer");
        CHECK(etude_history_now(&end) == 0 && end.monotonic_us - start.monotonic_us < 1000000,
              "observed deadline bounds a full pipe before the two-second stage bound");
        CHECK(limit.calls > before + 1, "a blocked write continues sampling the observer");
    }
    free(data);
    etude_remote_close(remote);
    int child;
    CHECK(waitpid(-1, &child, WNOHANG) == -1 && errno == ECHILD,
          "stalled-reader transport is reaped before the operation returns");
}
