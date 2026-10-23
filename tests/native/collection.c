/* collection.c: serialize the declared native cases and retain every result. */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700
#include "collection.h"
#include "history.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t interrupted;
static void request_stop(int sig)
{
    interrupted = sig;
}
int nc_mkdir(const char *path)
{
    char copy[4096], *at;
    if (strlen(path) >= sizeof(copy))
        return 0;
    strcpy(copy, path);
    for (at = copy + 1; *at; at++)
        if (*at == '/') {
            *at = 0;
            if (mkdir(copy, 0700) && errno != EEXIST)
                return 0;
            *at = '/';
        }
    return !mkdir(copy, 0700) || errno == EEXIST;
}
static int write_bytes(const char *path, const void *data, size_t size)
{
    FILE *fp = fopen(path, "wb");
    int ok;
    if (!fp)
        return 0;
    ok = !size || (data && fwrite(data, 1, size, fp) == size);
    return !fclose(fp) && ok;
}
static void json_string(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', fp);
        if ((unsigned char)*s < 32)
            fprintf(fp, "\\u%04x", (unsigned char)*s);
        else
            fputc(*s, fp);
    }
    fputc('"', fp);
}
static int save_result(const char *directory, struct nc_history *history, size_t index,
                       const struct etude_native_result *r, const char *reason, int status)
{
    const struct nc_case *c = history->cases[index];
    struct nc_measure *measure = &history->items[index];
    char path[8192];
    FILE *fp;
    int ok;
    snprintf(path, sizeof(path), "%s/out", directory);
    ok = write_bytes(path, r->attempt.out.data, r->attempt.out.kept);
    snprintf(path, sizeof(path), "%s/err", directory);
    ok &= write_bytes(path, r->attempt.err.data, r->attempt.err.kept);
    snprintf(path, sizeof(path), "%s/result.json", directory);
    fp = fopen(path, "w");
    if (!fp)
        return 0;
    fputs("{\"schema_version\":1,\"case\":", fp);
    json_string(fp, c->name);
    fprintf(fp,
            ",\"status\":%d,\"transport\":%d,\"complete\":%s,\"end\":%d,\"value\":%d,\"cleanup_"
            "confirmed\":%s,\"workspace_removed\":%s,\"reason\":",
            r->status, (int)r->transport, r->attempt.complete ? "true" : "false",
            (int)r->attempt.end, r->attempt.value,
            r->attempt.cleanup == ETUDE_CLEANUP_CONFIRMED ? "true" : "false",
            r->removed ? "true" : "false");
    json_string(fp, reason ? reason : "");
    fprintf(fp, ",\"collection_status\":%d,\"measurements\":", status);
    struct cmp_text metrics = {0};
    cmp_printf(&metrics, "{\"time\":");
    nc_time(history, &metrics, &measure->start, &measure->end);
    cmp_printf(&metrics, ",\"cpu_us\":");
    nc_metric(&metrics, &r->attempt.cpu_us);
    cmp_printf(&metrics, ",\"peak_bytes\":");
    nc_metric(&metrics, &r->attempt.peak_bytes);
    cmp_printf(&metrics, ",\"watchdog_expiry\":");
    nc_quote(&metrics,
             etude_expiry_name(etude_watchdog_check(measure->watchdog, measure->end.monotonic_us)));
    cmp_printf(
        &metrics,
        ",\"case_bound_us\":%llu,\"gap_bound_us\":%llu,\"case_samples\":%lu,\"gap_samples\":%lu}",
        (unsigned long long)measure->whole.us, (unsigned long long)measure->gap.us,
        (unsigned long)measure->whole.samples, (unsigned long)measure->gap.samples);
    if (metrics.failed)
        ok = 0;
    else
        fputs(metrics.data, fp);
    free(metrics.data);
    fputs("}\n", fp);
    return !fclose(fp) && ok;
}
static int execute(struct nc_attempt *a, const struct nc_config *conf, const char *directory,
                   const struct nc_case *c, struct etude_native_result *r)
{
    a->spec.journal = nc_text(a, "%s/journal", directory);
    a->spec.evidence = nc_text(a, "%s/evidence", directory);
    a->spec.cancel_requested = &interrupted;
    if (c->proof && !nc_proof_prepare(a, conf, directory, c->proof))
        return 2;
    int status = etude_native_attempt(&a->spec, r);
    if (c->proof)
        status = nc_proof_judge(a, r, c->proof);
    return status;
}

static int append_summary(const struct nc_config *conf, const struct nc_case *c,
                          const struct etude_native_result *r, int status, const char *why)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/summary", conf->results);
    {
        FILE *summary = fopen(path, "a");
        if (!summary)
            status = 4;
        else {
            fprintf(summary,
                    "%s %s: status %d, end %d value %d, complete %d, cleanup %s, removed %d%s%s\n",
                    !status                      ? "pass"
                    : status == 3 || status == 4 ? "incomplete"
                                                 : "fail",
                    c->name, status, (int)r->attempt.end, r->attempt.value, r->attempt.complete,
                    r->attempt.cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed",
                    r->removed, why && *why ? "; " : "", why ? why : "");
            if (fclose(summary))
                status = 4;
        }
    }
    return status;
}

int nc_run(const struct nc_config *conf, const struct nc_inventory *inventory,
           const struct nc_case *c, struct nc_history *history, size_t index)
{
    struct nc_measure *measure = &history->items[index];
    struct nc_attempt a;
    struct etude_native_result r;
    char directory[4096], name[256];
    int made, status;
    size_t n;
    snprintf(name, sizeof(name), "%s", c->name);
    for (n = 0; name[n]; n++)
        if (name[n] == '/')
            name[n] = '_';
    snprintf(directory, sizeof(directory), "%s/%s", conf->results, name);
    if (mkdir(directory, 0700))
        return 4;
    memset(&r, 0, sizeof(r));
    r.status = 2;
    r.transport = ETUDE_REMOTE_REFUSED;
    r.attempt.end = ETUDE_END_LOST;
    r.attempt.value = -1;
    r.attempt.cleanup = ETUDE_CLEANUP_UNCONFIRMED;
    made = nc_attempt_make(&a, conf, inventory, c);
    if (!nc_measure_start(history, measure)) {
        nc_attempt_free(&a);
        return 4;
    }
    a.spec.run = conf->run;
    a.spec.case_id = measure->case_id;
    a.spec.attempt = measure->attempt;
    a.spec.deadline_ms = (measure->whole.us + 999) / 1000;
    uint64_t stage = measure->transfer.us;
    if (measure->capture.us > stage)
        stage = measure->capture.us;
    if (measure->cleanup.us > stage)
        stage = measure->cleanup.us;
    a.spec.stage_ms = (stage + 999) / 1000;
    a.spec.observer = &measure->observer;

    if (interrupted || history->unconfirmed) {
        snprintf(a.why, sizeof(a.why),
                 history->unconfirmed ? "earlier native cleanup is unconfirmed"
                                      : "the collection was interrupted before this case");
        made = 0;
        r.status = 3;
    }
    if (made)
        status = execute(&a, conf, directory, c, &r);
    else {
        status = r.status;
        r.attempt.cleanup = ETUDE_CLEANUP_CONFIRMED;
    }
    nc_measure_end(measure);
    if (etude_watchdog_check(measure->watchdog, measure->end.monotonic_us))
        status = 4;
    if (!save_result(directory, history, index, &r, made ? r.detail : a.why, status))
        status = 4;
    if (!nc_history_attempt(history, index, &r, status, made ? r.detail : a.why))
        status = 4;
    status = append_summary(conf, c, &r, status, made ? NULL : a.why);
    history->failed |= status != 0;
    history->incomplete |= status == 3 || status == 4;
    printf("native: %s: %s; evidence %s\n", c->name,
           !status                      ? "pass"
           : status == 3 || status == 4 ? "incomplete"
                                        : "fail",
           directory);
    fflush(stdout);
    etude_native_result_free(&r);
    nc_attempt_free(&a);
    return status;
}
static int configure(int argc, char **argv, struct nc_config *c, int *first)
{
    int n;
    memset(c, 0, sizeof(*c));
    c->tree = ".";
    c->arch = "x86_64";
    c->purpose = "focused";
    c->state = "/var/native/state";
    c->stage = "/var/native/stage";
    for (n = 2; n < argc; n += 2) {
        const char **value = NULL;
        if (!strcmp(argv[n], "--")) {
            n++;
            break;
        }
        if (n + 1 == argc)
            return 0;
        struct option {
            const char *name;
            const char **value;
        } options[] = {{"--tree", &c->tree},
                       {"--inventory", &c->inventory},
                       {"--images", &c->images},
                       {"--guest", &c->guest},
                       {"--state", &c->state},
                       {"--stage", &c->stage},
                       {"--results", &c->results},
                       {"--run", &c->run},
                       {"--purpose", &c->purpose},
                       {"--machine-id", &c->machine},
                       {"--kernel-sha256", &c->kernel_sha256},
                       {"--arch", &c->arch},
                       {"--peer", &c->peer},
                       {"--outside-peer", &c->outside},
                       {"--interface", &c->interface},
                       {"--address", &c->address},
                       {"--gateway", &c->gateway},
                       {"--far", &c->far},
                       {"--empty", &c->empty},
                       {"--silent", &c->silent}};
        for (size_t i = 0; i < sizeof(options) / sizeof(*options); i++)
            if (!strcmp(argv[n], options[i].name)) {
                value = options[i].value;
                break;
            }
        if (!value || !*argv[n + 1] || strpbrk(argv[n + 1], "\r\n\t"))
            return 0;
        *value = argv[n + 1];
    }
    *first = n;
    return (!strcmp(c->arch, "i686") || !strcmp(c->arch, "x86_64")) &&
           (!strcmp(c->purpose, "focused") || !strcmp(c->purpose, "phase_first_run")) &&
           (!c->kernel_sha256 ||
            (strlen(c->kernel_sha256) == 64 && strspn(c->kernel_sha256, "0123456789abcdef") == 64));
}
static int defaults(struct nc_config *c, const char *self)
{
    static char tree[4096], inventory[8192], images[8192], guest[8192], run[160], executable[4096];
    if (!realpath(c->tree, tree) || !realpath(self, executable))
        return 0;
    c->tree = tree;
    c->self = executable;
    snprintf(inventory, sizeof(inventory), "%s/tests/native", tree);
    snprintf(images, sizeof(images), "%s/%s/tests/native/images", tree,
             !strcmp(c->arch, "x86_64") ? "build/x86_64" : "build");
    snprintf(guest, sizeof(guest), "%s/%s/tools/etude/guest/etude-guest", tree,
             !strcmp(c->arch, "x86_64") ? "build/x86_64" : "build");
    snprintf(run, sizeof(run), "native-%lld-%ld", (long long)time(NULL), (long)getpid());
    if (!c->inventory)
        c->inventory = inventory;
    if (!c->images)
        c->images = images;
    if (!c->guest)
        c->guest = guest;
    if (!c->run)
        c->run = run;
    return 1;
}
static int requested(const struct nc_inventory *i, int argc, char **argv, int first)
{
    int n;
    for (n = first; n < argc; n++) {
        size_t at;
        for (at = 0; at < i->ncases; at++)
            if (!strcmp(argv[n], i->cases[at].name))
                break;
        if (at == i->ncases) {
            fprintf(stderr, "native: unknown case: %s\n", argv[n]);
            return 0;
        }
        for (int old = first; old < n; old++)
            if (!strcmp(argv[old], argv[n]))
                return 0;
    }
    return 1;
}
int nc_collection(struct nc_config *conf, const struct nc_inventory *i, int argc, char **argv,
                  int first)
{
    char lock[4096], summary[4096];
    struct nc_history history;
    struct etude_reading granted;
    unsigned failures = 0, ran = 0;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    if (!conf->results || !nc_mkdir(conf->results) || !nc_mkdir(conf->state))
        return 2;
    snprintf(lock, sizeof(lock), "%s/collection.lock", conf->state);
    if (mkdir(lock, 0700)) {
        fprintf(stderr, "native: another collection owns %s\n", lock);
        return 2;
    }
    if (etude_history_now(&granted)) {
        rmdir(lock);
        return 2;
    }
    conf->granted = &granted;
    snprintf(summary, sizeof(summary), "%s/summary", conf->results);
    int summary_fd = open(summary, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (summary_fd < 0 || close(summary_fd)) {
        rmdir(lock);
        return 2;
    }
    if (!nc_history_open(&history, conf, i, argc, argv, first)) {
        nc_history_close(&history);
        rmdir(lock);
        fputs("native: cannot open identified native history\n", stderr);
        return 2;
    }
    for (size_t n = 0; n < history.count; n++) {
        int status;
        status = nc_run(conf, i, history.cases[n], &history, n);
        failures += status != 0;
        ran++;
    }
    if (history.unconfirmed || rmdir(lock)) {
        failures++;
        history.unconfirmed = 1;
        fprintf(stderr, "native: cleanup unconfirmed; collection claim retained: %s\n", lock);
    }
    if (!nc_history_finish(&history))
        failures++;
    if (!nc_history_close(&history))
        failures++;
    printf("native: result: %u cases, %u failed or incomplete%s\n", ran, failures,
           interrupted ? ", interrupted" : "");
    return interrupted ? 128 + interrupted : failures ? 1 : 0;
}
int etude_native_collection_main(int argc, char **argv)
{
    struct nc_config conf;
    struct nc_inventory *inventory;
    int first, status = 2;
    if (argc > 1 && !strcmp(argv[1], "--cut-transport"))
        return nc_cut_transport(argc, argv);
    if (argc < 2 ||
        (strcmp(argv[1], "list") && strcmp(argv[1], "run") && strcmp(argv[1], "proof")) ||
        !configure(argc, argv, &conf, &first) || !defaults(&conf, argv[0])) {
        fputs("usage: etude run native [list|proof] [--tree DIR] [--results DIR] "
              "[configuration] [-- CASE...]\n",
              stderr);
        return 2;
    }
    if (!strcmp(argv[1], "proof"))
        return nc_assets(&conf) ? nc_proof(&conf) : 2;
    inventory = calloc(1, sizeof(*inventory));
    if (!inventory)
        return 2;
    if ((!strcmp(argv[1], "list") || nc_assets(&conf)) &&
        nc_inventory_read(inventory, &conf, strcmp(argv[1], "list") != 0) &&
        requested(inventory, argc, argv, first)) {
        if (!strcmp(argv[1], "list")) {
            for (size_t n = 0; n < inventory->ncases; n++)
                if (nc_selected(argc, argv, first, inventory->cases[n].name))
                    puts(inventory->cases[n].name);
            status = 0;
        } else
            status = nc_collection(&conf, inventory, argc, argv, first);
    }
    nc_inventory_free(inventory);
    free(inventory);
    return status;
}
