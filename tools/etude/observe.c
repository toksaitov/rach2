/* A separate native thread observes load even while the driver hashes inputs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#include "win/files.h"
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif
#include "observe_scope.h"
#include "json.h"
#include "history_json.h"
#include "sha256.h"
#include "measurement_context.h"

struct etude_observer {
    struct load_account account;
    struct load_snapshot first, last;
    struct load_process *processes;
    struct etude_context_clock clock;
    struct etude_pid identity;
    char run_id[256];
    FILE *file;
    uint64_t samples, max_gap, denied;
    unsigned resets, available;
    int stop, ready, done, joined, failed, owner_lost, overflow, cancelled;
#ifdef _WIN32
    HANDLE thread, event, ready_event;
#else
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
#endif
};

static int wait_stop(struct etude_observer *o)
{
#ifdef _WIN32
    return WaitForSingleObject(o->event, 1000) == WAIT_OBJECT_0;
#else
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec++;
    pthread_mutex_lock(&o->mutex);
    if (!o->stop) pthread_cond_timedwait(&o->condition, &o->mutex, &until);
    int stop = o->stop;
    pthread_mutex_unlock(&o->mutex);
    return stop;
#endif
}

int etude_observe_done(struct etude_observer *o)
{
    if (o->joined) return 1;
#ifdef _WIN32
    return WaitForSingleObject(o->thread, 0) == WAIT_OBJECT_0;
#else
    pthread_mutex_lock(&o->mutex);
    int done = o->done;
    pthread_mutex_unlock(&o->mutex);
    return done;
#endif
}

/* Retain a digest of all observed creation identities without an unbounded
 * process list. CPU deltas use the same identities, not PID alone. */
static void identities(const struct etude_observer *o, const struct load_snapshot *s,
                       char hex[65])
{
    struct sha256 hash;
    unsigned char digest[32];
    char line[192];
    sha256_init(&hash);
    for (size_t i = 0; i < s->count; i++) {
        const struct load_process *p = &o->processes[i];
        int n = snprintf(line, sizeof line, "%ld %llu %ld %u\n", p->id.pid,
                         (unsigned long long)p->id.start, p->parent, p->scope);
        sha256_update(&hash, (const unsigned char *)line, (size_t)n);
    }
    sha256_final(&hash, digest);
    sha256_hex(digest, hex);
}

static void trace_sample(struct etude_observer *o, const struct load_snapshot *s)
{
    char hex[65];
    identities(o, s, hex);
    long size = ftell(o->file);
    if (size < 0 || (unsigned long)size > ETUDE_OBSERVE_BYTES - 4096) {
        o->failed = 1;
        return;
    }
    fprintf(o->file, "{\"kind\":\"sample\",\"monotonic_us\":%llu,\"utc_us\":%llu",
        (unsigned long long)s->monotonic_us, (unsigned long long)s->utc_us);
    for (unsigned i = 0; i < LOAD_FIELDS; i++) {
        fprintf(o->file, ",\"%s\":", etude_load_field_name(i));
        if ((s->available & (1u << i)) && s->value[i] <= (uint64_t)JSON_INTEGER_MAX)
            fprintf(o->file, "%llu", (unsigned long long)s->value[i]);
        else fputs("null", o->file);
    }
    fprintf(o->file, ",\"outside_cpu_lower_us\":%llu,\"shared_virtualizer_cpu_lower_us\":%llu,"
        "\"processes\":%lu,\"denied\":%lu,\"lost\":%llu,\"unclassified\":%llu,"
        "\"identities_sha256\":\"%s\",\"overflow\":%s}\n",
        (unsigned long long)o->account.outside_us, (unsigned long long)o->account.virtualizer_us,
        (unsigned long)s->count, (unsigned long)s->denied, (unsigned long long)o->account.lost,
        (unsigned long long)o->account.unclassified, hex, s->overflow ? "true" : "false");
    if (fflush(o->file) || ferror(o->file)) o->failed = 1;
}

static void sample(struct etude_observer *o)
{
    struct load_snapshot s;
    int rc = etude_load_snapshot(&s, o->processes, ETUDE_OBSERVE_PROCESSES);
    if (rc != 0) o->failed = 1;
    etude_load_account(&o->account, &s, o->processes);
    if (!o->samples) o->first = s;
    else {
        uint64_t gap = s.monotonic_us - o->last.monotonic_us;
        if (gap > o->max_gap) o->max_gap = gap;
        for (unsigned i = 0; i < LOAD_FIELDS; i++)
            if ((s.available & o->last.available & (1u << i)) &&
                i != LOAD_MEMORY && s.value[i] < o->last.value[i]) o->resets |= 1u << i;
    }
    o->available &= s.available;
    o->overflow |= s.overflow;
    o->denied += s.denied;
    o->last = s;
    o->samples++;
    trace_sample(o, &s);
}

static void observe(struct etude_observer *o)
{
    fprintf(o->file, "{\"kind\":\"start\",\"schema_version\":1,\"method\":\"native-load-v1\","
        "\"platform\":\"%s\",\"machine_id\":\"%s\",\"boot_id\":\"%s\","
        "\"run_id\":\"%s\",\"root_pid\":%ld,\"root_creation\":\"%llu\","
        "\"observer_pid\":%ld,\"observer_creation\":\"%llu\","
        "\"requested_interval_us\":%u,\"process_limit\":%u}\n", etude_load_platform(),
        o->clock.machine_id, o->clock.boot_id, o->run_id,
        o->account.root.pid, (unsigned long long)o->account.root.start,
        o->identity.pid, (unsigned long long)o->identity.start,
        ETUDE_OBSERVE_INTERVAL_US, ETUDE_OBSERVE_PROCESSES);
    sample(o);
#ifdef _WIN32
    SetEvent(o->ready_event);
#else
    pthread_mutex_lock(&o->mutex); o->ready = 1;
    pthread_cond_signal(&o->condition); pthread_mutex_unlock(&o->mutex);
#endif
    while (!wait_stop(o)) {
        struct etude_pid now;
        if (etude_process_state(o->account.root.pid, &now) != 1 || !now.start_known ||
            now.start != o->account.root.start) { o->owner_lost = 1; break; }
        sample(o);
    }
    if (o->cancelled) o->failed = 1;
    sample(o);
    fprintf(o->file, "{\"kind\":\"end\",\"utc_us\":%llu,\"monotonic_us\":%llu,"
        "\"samples\":%llu,\"complete\":%s,\"owner_lost\":%s}\n",
        (unsigned long long)o->last.utc_us, (unsigned long long)o->last.monotonic_us,
        (unsigned long long)o->samples, !o->failed && !o->owner_lost ? "true" : "false",
        o->owner_lost ? "true" : "false");
    if (fflush(o->file) || ferror(o->file)) o->failed = 1;
}

#ifdef _WIN32
static DWORD WINAPI worker(void *p) { observe(p); return 0; }
#else
static void *worker(void *p)
{
    struct etude_observer *o = p;
    observe(o);
    pthread_mutex_lock(&o->mutex);
    o->done = 1;
    pthread_mutex_unlock(&o->mutex);
    return NULL;
}
#endif

struct etude_observer *etude_observe_start(const char *path, const struct etude_pid *root,
                                         const char *run_id)
{
    struct etude_observer *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    struct etude_pid owner;
#ifdef _WIN32
    long self = (long)GetCurrentProcessId();
#else
    long self = (long)getpid();
#endif
    if (!run_id || strlen(run_id) >= sizeof o->run_id || !eh_identity(run_id) ||
        etude_context_clock(&o->clock) != 0 || etude_process_state(self, &o->identity) != 1 ||
        !o->identity.start_known || etude_process_state(root->pid, &owner) != 1 ||
        !owner.start_known || !root->start_known || owner.start != root->start) {
        free(o); return NULL;
    }
    strcpy(o->run_id, run_id);
    o->account.root = *root;
    o->available = (1u << LOAD_FIELDS) - 1;
    o->account.previous = calloc(ETUDE_OBSERVE_PROCESSES, sizeof *o->processes);
    o->processes = calloc(ETUDE_OBSERVE_PROCESSES, sizeof *o->processes);
    o->file = fopen(path, "wbx");
    if (!o->account.previous || !o->processes || !o->file) goto fail;
#ifdef _WIN32
    o->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    o->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!o->event || !o->ready_event) {
        if (o->event) CloseHandle(o->event);
        if (o->ready_event) CloseHandle(o->ready_event);
        goto fail;
    }
    o->thread = CreateThread(NULL, 0, worker, o, 0, NULL);
    if (!o->thread) { CloseHandle(o->event); CloseHandle(o->ready_event); goto fail; }
    WaitForSingleObject(o->ready_event, INFINITE);
#else
    if (pthread_mutex_init(&o->mutex, NULL)) goto fail;
    if (pthread_cond_init(&o->condition, NULL)) { pthread_mutex_destroy(&o->mutex); goto fail; }
    if (pthread_create(&o->thread, NULL, worker, o)) {
        pthread_cond_destroy(&o->condition); pthread_mutex_destroy(&o->mutex); goto fail;
    }
    pthread_mutex_lock(&o->mutex);
    while (!o->ready) pthread_cond_wait(&o->condition, &o->mutex);
    pthread_mutex_unlock(&o->mutex);
#endif
    if (o->failed) { etude_observe_free(o); return NULL; }
    return o;
fail:
    if (o->file) fclose(o->file);
    free(o->processes); free(o->account.previous); free(o);
    return NULL;
}

int etude_observe_stop(struct etude_observer *o)
{
    if (!o) return -1;
    if (!o->joined) {
#ifdef _WIN32
        SetEvent(o->event);
        WaitForSingleObject(o->thread, INFINITE);
        CloseHandle(o->thread); CloseHandle(o->event); CloseHandle(o->ready_event);
#else
        pthread_mutex_lock(&o->mutex); o->stop = 1;
        pthread_cond_signal(&o->condition); pthread_mutex_unlock(&o->mutex);
        pthread_join(o->thread, NULL);
        pthread_cond_destroy(&o->condition); pthread_mutex_destroy(&o->mutex);
#endif
        if (fclose(o->file)) o->failed = 1;
        o->file = NULL; o->joined = 1;
    }
    return o->failed || o->owner_lost ? -1 : 0;
}

int etude_observe_cancel(struct etude_observer *o)
{
    if (!o || o->joined) return -1;
#ifdef _WIN32
    o->cancelled = 1;
#else
    pthread_mutex_lock(&o->mutex);
    o->cancelled = 1;
    pthread_mutex_unlock(&o->mutex);
#endif
    return etude_observe_stop(o);
}

void etude_observe_free(struct etude_observer *o)
{
    if (!o) return;
    etude_observe_stop(o);
    free(o->account.previous); free(o->processes); free(o);
}

void etude_observe_bounds(const struct etude_observer *o, uint64_t *start, uint64_t *end)
{
    *start = o->first.utc_us;
    *end = o->last.utc_us;
}

static void append(struct jbuf *b, const char *format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (n >= 0 && n < (int)sizeof text) jbuf_put(b, text, (size_t)n);
}

static void metric(struct jbuf *b, const char *name, uint64_t value, const char *quality,
                   uint64_t gap, int available)
{
    append(b, ",\"%s\":", name);
    if (available && value <= (uint64_t)JSON_INTEGER_MAX)
        append(b, "{\"value\":%llu,\"method\":\"native-load-v1\",\"quality\":\"%s\","
            "\"interval_us\":%llu,\"error_bound\":null}", (unsigned long long)value,
            quality, (unsigned long long)gap);
    else jbuf_puts(b, "{\"value\":null,\"reason\":\"native counter unavailable, reset or overflowed\"}");
}

char *etude_observe_summary(const struct etude_observer *o, const struct etude_measurement *owned)
{
    struct jbuf b = {0};
    int cpu = o && o->joined && o->samples >= 2 && o->max_gap <= 5 * ETUDE_OBSERVE_INTERVAL_US && !o->failed && !o->owner_lost &&
        (o->available & 1) && !(o->resets & 1);
    if (!o) return NULL;
    uint64_t busy = cpu ? o->last.value[LOAD_CPU] - o->first.value[LOAD_CPU] : 0;
    append(&b, "{\"schema_version\":1,\"method\":\"native-load-v1\",\"platform\":\"%s\","
        "\"comparison_available\":%s,\"coverage\":{\"start_utc_us\":%llu,\"end_utc_us\":%llu,"
        "\"start_us\":%llu,\"end_us\":%llu,\"samples\":%llu,\"max_gap_us\":%llu},"
        "\"process_losses\":%llu,\"process_counter_resets\":%llu,\"denied_reads\":%llu,"
        "\"process_limit_reached\":%s", etude_load_platform(), cpu ? "true" : "false",
        (unsigned long long)o->first.utc_us, (unsigned long long)o->last.utc_us,
        (unsigned long long)o->first.monotonic_us, (unsigned long long)o->last.monotonic_us,
        (unsigned long long)o->samples, (unsigned long long)o->max_gap,
        (unsigned long long)o->account.lost, (unsigned long long)o->account.resets,
        (unsigned long long)o->denied, o->overflow ? "true" : "false");
    metric(&b, "system_busy_cpu_us", busy, "sampled", o->max_gap, cpu);
    metric(&b, "outside_cpu_us", o->account.outside_us, "lower_bound", o->max_gap, cpu);
    metric(&b, "shared_virtualizer_cpu_us", o->account.virtualizer_us, "lower_bound", o->max_gap, cpu);
    metric(&b, "non_owned_cpu_upper_bound_us", cpu && owned && busy >= owned->value ?
        busy - owned->value : 0, "sampled", o->max_gap,
        cpu && owned && !owned->reason && busy >= owned->value);
    for (unsigned i = 1; i < LOAD_FIELDS; i++) {
        int available = (o->available & (1u << i)) && !(o->resets & (1u << i));
        uint64_t v = i == LOAD_MEMORY ? o->last.value[i] : o->last.value[i] - o->first.value[i];
        metric(&b, etude_load_field_name(i), v, "sampled", o->max_gap, available);
    }
    jbuf_puts(&b, ",\"limitations\":\"process deltas omit short-lived and inaccessible work; residual includes unaccounted owned overhead; block counters include device stacking\"}\n");
    return b.data;
}
