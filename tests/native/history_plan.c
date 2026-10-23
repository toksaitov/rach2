/* Build a fully identified native plan before learning or starting work. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include "json.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <unistd.h>
int nc_selected(int argc, char **argv, int first, const char *name)
{
    if (argc == first)
        return 1;
    for (int n = first; n < argc; n++)
        if (!strcmp(argv[n], name))
            return 1;
    return 0;
}
static int identity(struct nc_history *h)
{
    struct utsname u;
    char path[4096], *bytes;
    size_t length;
    if (uname(&u))
        return 0;
    const char *machine = h->conf->machine ? h->conf->machine : u.nodename;
    size_t release = strlen(u.release), version = strlen(u.version);
    if (strlen(machine) >= sizeof(h->machine) || release + version + 2 > sizeof(h->version))
        return 0;
    strcpy(h->machine, machine);
    memcpy(h->version, u.release, release);
    h->version[release] = ' ';
    memcpy(h->version + release + 1, u.version, version + 1);
#ifdef __APPLE__
    size_t boot_size = sizeof(h->boot);
    if (sysctlbyname("kern.bootsessionuuid", h->boot, &boot_size, NULL, 0))
        return 0;
#else
    FILE *boot = fopen("/dev/shm/etude-guest-boot", "r");
    if (!boot)
        boot = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!boot || !fgets(h->boot, sizeof(h->boot), boot)) {
        if (boot)
            fclose(boot);
        return 0;
    }
    fclose(boot);
    h->boot[strcspn(h->boot, "\r\n")] = 0;
#endif
    snprintf(path, sizeof(path), "%s/native-source.json", h->conf->tree);
    if (cmp_read_file(path, 65536, &bytes, &length))
        return 0;
    struct eh_json *doc = eh_json_parse(bytes);
    const char *commit = cmp_text(doc, "commit");
    int ok = commit && strlen(commit) == 40 && strspn(commit, "0123456789abcdef") == 40;
    if (ok)
        strcpy(h->commit, commit);
    eh_json_free(doc);
    free(bytes);
    return ok && eh_identity(h->machine) && eh_identity(h->boot);
}
static int prior(struct nc_history *h)
{
    char directory[4096], file[8192], *paths[4096];
    size_t count = 0;
    snprintf(directory, sizeof(directory), "%s/history", h->conf->state);
    if (!nc_mkdir(directory))
        return 0;
    DIR *d = opendir(directory);
    if (!d)
        return 0;
    struct dirent *e;
    int ok = 1;
    while ((e = readdir(d))) {
        size_t size = strlen(e->d_name);
        if (size < 6 || strcmp(e->d_name + size - 5, ".path"))
            continue;
        if (count == 4096) {
            ok = 0;
            break;
        }
        snprintf(file, sizeof(file), "%s/%s", directory, e->d_name);
        FILE *f = fopen(file, "r");
        char path[4096];
        if (!f || !fgets(path, sizeof(path), f)) {
            if (f)
                fclose(f);
            ok = 0;
            break;
        }
        fclose(f);
        path[strcspn(path, "\r\n")] = 0;
        if (path[0] != '/' || !(paths[count] = strdup(path))) {
            ok = 0;
            break;
        }
        count++;
    }
    closedir(d);
    if (ok && count)
        ok = cmp_load(&h->prior, h->schema, paths, (int)count) == 0;
    for (size_t n = 0; n < count; n++)
        free(paths[n]);
    return ok;
}
static int reference(struct nc_history *h, char out[2048], const char *path)
{
    struct cmp_text t = {0};
    int ok = nc_file_blob(h, &t, path) && t.length < 2048;
    if (ok)
        memcpy(out, t.data, t.length + 1);
    free(t.data);
    return ok;
}
static int document(struct nc_history *h, char out[2048], struct cmp_text *t)
{
    struct cmp_text ref = {0};
    int ok = !t->failed && nc_blob(h, &ref, t->data, t->length) && ref.length < 2048;
    if (ok)
        memcpy(out, ref.data, ref.length + 1);
    free(t->data);
    free(ref.data);
    memset(t, 0, sizeof(*t));
    return ok;
}
static void identity_ref(struct cmp_text *t, const char *reference)
{
    struct eh_json *doc = eh_json_parse(reference);
    const struct eh_json *size = eh_json_get(doc, "size_bytes");
    if (!doc || !size)
        t->failed = 1;
    else
        cmp_printf(t, "{\"sha256\":\"%s\",\"size_bytes\":%llu}", cmp_text(doc, "sha256"),
                   (unsigned long long)size->number);
    eh_json_free(doc);
}
static int item(struct nc_history *h, size_t n, const struct nc_case *c)
{
    struct nc_measure *m = &h->items[n];
    struct cmp_text work = {0}, ref = {0};
    int size = snprintf(m->case_id, sizeof(m->case_id), "native.%s", c->name);
    if (size < 0 || size >= (int)sizeof(m->case_id))
        return 0;
    for (char *p = m->case_id; *p; p++)
        if (*p == '/')
            *p = '.';
    size =
        snprintf(m->item, sizeof(m->item), "%s:%s:%s:r0", h->conf->run, m->case_id, h->conf->arch);
    if (size < 0 || size >= (int)sizeof(m->item))
        return 0;
    size = snprintf(m->attempt, sizeof(m->attempt), "%s:a0", m->item);
    if (size < 0 || size >= (int)sizeof(m->attempt))
        return 0;
    cmp_printf(&work, "{\"argv\":[");
    for (size_t i = 0; i < c->argc; i++) {
        cmp_printf(&work, "%s", i ? "," : "");
        nc_quote(&work, c->argv[i]);
    }
    cmp_printf(&work, "],\"fixtures\":[");
    for (size_t i = 0; i < c->nfixtures; i++) {
        cmp_printf(&work, "%s", i ? "," : "");
        nc_quote(&work, c->fixtures[i]);
    }
    cmp_printf(&work, "],\"prepared_inputs\":");
    identity_ref(&work, h->source);
    cmp_printf(&work, "}");
    int ok = nc_blob(h, &ref, work.data, work.length);
    if (ok && ref.length < sizeof(m->workload_ref))
        strcpy(m->workload_ref, ref.data);
    else
        ok = 0;
    struct eh_json *d = ok ? eh_json_parse(ref.data) : NULL;
    if (d)
        snprintf(m->workload, sizeof(m->workload), "%s", cmp_text(d, "sha256"));
    eh_json_free(d);
    free(work.data);
    free(ref.data);
    return ok;
}
static void item_json(struct nc_history *h, struct cmp_text *t, size_t n)
{
    struct nc_measure *m = &h->items[n];
    cmp_printf(t, "{\"item_id\":");
    nc_quote(t, m->item);
    cmp_printf(t, ",\"case_id\":");
    nc_quote(t, m->case_id);
    cmp_printf(t,
               ",\"family_id\":\"native\",\"architecture\":\"%s\",\"domain\":\"native\","
               "\"workload\":%s,\"assertions\":%s,\"profile\":%s,"
               "\"repetition\":0,\"attempt_ids\":[",
               h->conf->arch, m->workload_ref, m->assertions_ref, m->profile_ref);
    nc_quote(t, m->attempt);
    cmp_printf(t, "]}");
}
static int plan(struct nc_history *h)
{
    struct cmp_text t = {0};
    cmp_printf(&t, "{\"purpose\":");
    nc_quote(&t, h->conf->purpose);
    cmp_printf(&t, ",\"selection\":%s,\"items\":[", h->selection);
    for (size_t i = 0; i < h->count; i++) {
        cmp_printf(&t, "%s", i ? "," : "");
        item_json(h, &t, i);
    }
    cmp_printf(&t,
               "],\"commit\":\"%s\",\"source\":%s,\"runner\":%s,\"artifacts\":[%s],"
               "\"environment\":{\"machine_id\":",
               h->commit, h->source, h->runner, h->source);
    nc_quote(&t, h->machine);
    cmp_printf(&t, ",\"boot_id\":");
    nc_quote(&t, h->boot);
    cmp_printf(&t, ",\"os\":\"rach2\",\"os_version\":");
    nc_quote(&t, h->version);
    cmp_printf(&t,
               ",\"accelerator\":\"native\",\"site_profile\":%s,\"tools\":%s,\"capacity\":%s,"
               "\"cache_state\":%s,\"load\":%s},\"concurrency\":%s,\"policies\":%s,"
               "\"exemption\":",
               h->environment, h->runner, h->environment, h->environment, h->environment,
               h->environment, h->policy);
    if (!strcmp(h->conf->purpose, "phase_first_run")) {
        cmp_printf(&t,
                   "{\"reason\":\"milestone-62-first-runs\",\"phase_issue\":589,\"manifest\":%s,"
                   "\"run_ids\":[",
                   h->selection);
        nc_quote(&t, h->conf->run);
        cmp_printf(&t, "],\"attempt_ids\":[");
        for (size_t i = 0; i < h->count; i++) {
            cmp_printf(&t, "%s", i ? "," : "");
            nc_quote(&t, h->items[i].attempt);
        }
        cmp_printf(&t, "]}");
    } else
        cmp_printf(&t, "null");
    cmp_printf(&t, ",\"reproduce\":%s}", h->reproduce);
    return nc_record(h, "plan", &t, h->plan);
}
static int prepare_items(struct nc_history *h, const struct nc_inventory *inventory, int argc,
                         char **argv, int first)
{
    const struct nc_config *conf = h->conf;
    char path[4096];
    struct cmp_text selection = {0};
    h->items = calloc(inventory->ncases, sizeof(*h->items));
    h->cases = calloc(inventory->ncases, sizeof(*h->cases));
    if (!h->items || !h->cases)
        return 0;
    snprintf(path, sizeof(path), "%s/assets.sha256", conf->tree);
    if (!reference(h, h->source, path) || !reference(h, h->runner, conf->self))
        return 0;
    snprintf(path, sizeof(path), "%s/tests/native/bounds.json", conf->tree);
    if (!reference(h, h->policy, path))
        return 0;
    cmp_printf(&selection, "{\"architecture\":\"%s\",\"cases\":[", conf->arch);
    for (size_t i = 0; i < inventory->ncases; i++)
        if (nc_selected(argc, argv, first, inventory->cases[i].name)) {
            if (!item(h, h->count, &inventory->cases[i]))
                return 0;
            cmp_printf(&selection, "%s", h->count ? "," : "");
            nc_quote(&selection, inventory->cases[i].name);
            h->cases[h->count++] = &inventory->cases[i];
        }
    cmp_printf(&selection, "]}");
    if (!h->count || !document(h, h->selection, &selection))
        return 0;
    return 1;
}

static int profiles(struct nc_history *h)
{
    const struct nc_config *conf = h->conf;
    struct cmp_text profile = {0}, assertions = {0};
    cmp_printf(&profile, "{\"bounds\":");
    identity_ref(&profile, h->policy);
    cmp_printf(&profile, ",\"prepared\":");
    identity_ref(&profile, h->source);
    cmp_printf(&profile, ",\"machine\":");
    nc_quote(&profile, h->machine);
    const char *names[] = {"peer", "outside", "interface", "address",      "gateway",
                           "far",  "empty",   "silent",    "kernel_sha256"};
    const char *values[] = {conf->peer,    conf->outside, conf->interface,
                            conf->address, conf->gateway, conf->far,
                            conf->empty,   conf->silent,  conf->kernel_sha256};
    for (size_t n = 0; n < sizeof(names) / sizeof(*names); n++) {
        cmp_printf(&profile, ",");
        nc_quote(&profile, names[n]);
        cmp_printf(&profile, ":");
        if (values[n])
            nc_quote(&profile, values[n]);
        else
            cmp_printf(&profile, "null");
    }
    cmp_printf(&profile, "}");
    cmp_printf(&assertions, "[{\"assertion_id\":\"completed\",\"source\":\"executor\",\"meaning\":"
                            "\"declared termination and complete cleanup\"}]");
    char p[2048], a[2048];
    if (!document(h, p, &profile) || !document(h, a, &assertions))
        return 0;
    struct eh_json *pd = eh_json_parse(p), *ad = eh_json_parse(a);
    for (size_t n = 0; n < h->count; n++) {
        strcpy(h->items[n].profile_ref, p);
        strcpy(h->items[n].assertions_ref, a);
        snprintf(h->items[n].profile, 65, "%s", cmp_text(pd, "sha256"));
        snprintf(h->items[n].assertions, 65, "%s", cmp_text(ad, "sha256"));
    }
    eh_json_free(pd);
    eh_json_free(ad);
    return 1;
}

static int reproduction(struct nc_history *h)
{
    const struct nc_config *c = h->conf;
    struct cmp_text t = {0};
    const char *names[] = {"--tree",    "--inventory",    "--images",        "--guest",
                           "--state",   "--stage",        "--results",       "--run",
                           "--purpose", "--machine-id",   "--kernel-sha256", "--arch",
                           "--peer",    "--outside-peer", "--interface",     "--address",
                           "--gateway", "--far",          "--empty",         "--silent"};
    const char *values[] = {c->tree,          c->inventory, c->images, c->guest,   c->state,
                            c->stage,         c->results,   c->run,    c->purpose, c->machine,
                            c->kernel_sha256, c->arch,      c->peer,   c->outside, c->interface,
                            c->address,       c->gateway,   c->far,    c->empty,   c->silent};
    cmp_printf(&t, "{\"argv\":[");
    nc_quote(&t, c->self);
    cmp_printf(&t, ",\"run\",\"native\"");
    for (size_t n = 0; n < sizeof(names) / sizeof(*names); n++)
        if (values[n]) {
            cmp_printf(&t, ",");
            nc_quote(&t, names[n]);
            cmp_printf(&t, ",");
            nc_quote(&t, values[n]);
        }
    cmp_printf(&t, ",\"--\"");
    for (size_t n = 0; n < h->count; n++) {
        cmp_printf(&t, ",");
        nc_quote(&t, h->cases[n]->name);
    }
    cmp_printf(&t, "],\"requires_new_run_and_results\":true}");
    return document(h, h->reproduce, &t);
}
static int metadata(struct nc_history *h)
{
    struct cmp_text env = {0};
    cmp_printf(&env,
               "{\"serial\":true,\"capacity\":{\"value\":null,\"reason\":\"native capacity was not "
               "sampled\"},"
               "\"cache\":{\"value\":null,\"reason\":\"OS cache remains natural\"},"
               "\"comparison_available\":false,\"reason\":\"outside activity was not observed\","
               "\"preparation\":{\"value\":null,\"reason\":\"payload production and initial asset "
               "verification preceded the grant and were not measured\"}}");
    if (!document(h, h->environment, &env))
        return 0;
    struct cmp_text fixed = {0};
    cmp_printf(&fixed, "{\"policy\":%s,\"items\":[", h->policy);
    for (size_t n = 0; n < h->count; n++) {
        struct nc_measure *m = &h->items[n];
        cmp_printf(&fixed, "%s{\"item\":", n ? "," : "");
        nc_quote(&fixed, m->item);
        cmp_printf(&fixed,
                   ",\"case_us\":%llu,\"case_samples\":%lu,\"gap_us\":%llu,\"gap_samples\":%lu,"
                   "\"transfer_us\":%llu,\"capture_us\":%llu,\"cleanup_us\":%llu}",
                   (unsigned long long)m->whole.us, (unsigned long)m->whole.samples,
                   (unsigned long long)m->gap.us, (unsigned long)m->gap.samples,
                   (unsigned long long)m->transfer.us, (unsigned long long)m->capture.us,
                   (unsigned long long)m->cleanup.us);
    }
    cmp_printf(&fixed, "]}");
    if (!document(h, h->policy, &fixed))
        return 0;

    return 1;
}

int nc_history_open(struct nc_history *h, const struct nc_config *conf,
                    const struct nc_inventory *inventory, int argc, char **argv, int first)
{
    char history[4096], stream[256];
    memset(h, 0, sizeof(*h));
    h->conf = conf;
    if (!eh_identity(conf->run) ||
        strspn(conf->run, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
            strlen(conf->run) ||
        !identity(h))
        return 0;
    if (conf->granted)
        h->start = *conf->granted;
    else if (etude_history_now(&h->start))
        return 0;
    snprintf(h->store, sizeof(h->store), "%s/store", conf->results);
    snprintf(h->schema, sizeof(h->schema), "%s/tools/etude/schemas/history.schema.json",
             conf->tree);
    if (!nc_mkdir(h->store) || !prior(h))
        return 0;
    if (!prepare_items(h, inventory, argc, argv, first))
        return 0;
    if (!profiles(h))
        return 0;
    if (!nc_bounds(h))
        return 0;
    if (!metadata(h) || !reproduction(h))
        return 0;
    snprintf(history, sizeof(history), "%s/history.jsonl", conf->results);
    snprintf(stream, sizeof(stream), "%s:native", conf->run);
    h->writer = etude_history_open(history, h->schema, stream, conf->run, 64 * 1024 * 1024);
    if (!h->writer || !plan(h))
        return 0;
    return 1;
}
static int index_history(struct nc_history *h)
{
    char index[4096], temporary[4096], history[4096];
    int n = snprintf(index, sizeof(index), "%s/history/%s.path", h->conf->state, h->conf->run);
    if (n < 0 || n >= (int)sizeof(index))
        return 0;
    n = snprintf(temporary, sizeof(temporary), "%s.%ld.partial", index, (long)getpid());
    if (n < 0 || n >= (int)sizeof(temporary))
        return 0;
    n = snprintf(history, sizeof(history), "%s/history.jsonl", h->conf->results);
    if (n < 0 || n >= (int)sizeof(history))
        return 0;
    FILE *f = fopen(temporary, "wx");
    if (!f)
        return 0;
    int ok = fprintf(f, "%s\n", history) > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f))
        ok = 0;
    if (ok)
        ok = link(temporary, index) == 0;
    if (unlink(temporary))
        ok = 0;
    return ok;
}
int nc_history_close(struct nc_history *h)
{
    int ok = !h->writer || etude_history_close(h->writer) == 0;
    if (ok && h->terminal)
        ok = index_history(h);
    for (size_t i = 0; i < h->count; i++)
        etude_watchdog_free(h->items[i].watchdog);
    cmp_free(&h->prior);
    free(h->items);
    free(h->cases);
    memset(h, 0, sizeof(*h));
    return ok;
}
