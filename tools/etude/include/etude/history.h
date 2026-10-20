/* history.h: durable history streams and measured resource accounting. */
#ifndef ETUDE_HISTORY_H
#define ETUDE_HISTORY_H
#include <stdint.h>
#include <stddef.h>

#define ETUDE_HISTORY_MAX UINT64_C(9007199254740991)

struct etude_history;

/* A complete metric, or an unavailable value with a reason. */
struct etude_measurement {
    uint64_t value;
    const char *method;
    const char *quality;  /* exact, sampled or lower_bound */
    uint64_t interval_us; /* zero means that the method does not sample */
    uint64_t error_bound;
    int error_known;
    const char *reason; /* non-NULL means unavailable; value is then ignored */
};

struct etude_reading {
    char utc[28];
    uint64_t monotonic_us;
    uint64_t resolution_us; /* rounded upward, at least one microsecond */
};

/* One scope, excluding nested guests and repeated PIDs. */
struct etude_accounting {
    struct etude_measurement cpu;
    struct etude_measurement memory;
};

/**
 * Create a new private stream, never reopening or replacing a file.
 *
 * The caller allocates globally unique stream and run identities. schema_path
 * is the trusted version-one history schema. limit_bytes bounds this stream;
 * a site's admission policy must also bound the number of streams. A stream
 * contains at most 65,536 records; a new stream continues a longer run. Failure
 * returns NULL with errno set. The stream is owned by one thread, in one
 * process; a forked child must open a stream of its own.
 */
struct etude_history *etude_history_open(const char *path, const char *schema_path,
                                         const char *stream_id, const char *run_id,
                                         uint64_t limit_bytes);

/**
 * Validate and durably append one record; return 0, or -1 with errno set.
 *
 * record_id must equal stream_id/sequence. The allocator can obtain it with
 * etude_history_record_id before building references. data is one JSON object
 * of the schema's kind. Invalid input writes nothing. An I/O error poisons
 * the stream, which must then be closed. A newline commits a record only
 * after its object bytes have been flushed to storage.
 */
int etude_history_append(struct etude_history *history, const char *record_id, const char *utc,
                         const char *kind, const char *data);

/** Copy the next globally unique record identity, or return -1 on overflow. */
int etude_history_record_id(const struct etude_history *history, char *out, size_t size);

/** Close and free a stream; return 0, or -1 on a close error. */
int etude_history_close(struct etude_history *history);

/**
 * Read a private stream with the trusted schema, in sequence order.
 *
 * Return 0 for a complete valid stream, 1 for a lost final record, or -1 for
 * invalid data or I/O. A nonempty complete prefix does not establish a run's
 * success. visit receives each validated line without its newline; it must
 * copy bytes it retains. Exact replays are delivered once. A nonzero callback
 * result stops reading with -1.
 * References, hashes and coverage are checked by consumers after reading all
 * streams; no individual summary proves success. bad_line is one-based and
 * zero on success. Input must be closed by its writer or a stable snapshot.
 */
int etude_history_read(const char *path, const char *schema_path,
                       int (*visit)(const char *, void *), void *context, uint64_t *bad_line);

/** Read UTC and monotonic time; return -1 if either clock is unavailable. */
int etude_history_now(struct etude_reading *reading);

/**
 * Measure an interval in one machine, boot and clock domain.
 * The caller establishes that identity before supplying the readings. A
 * missing end, backward reading or overflow produces an unavailable metric.
 */
struct etude_measurement etude_history_elapsed(const struct etude_reading *start,
                                               const struct etude_reading *end);

/** Write a measurement as JSON; return -1 on invalid input or overflow. */
int etude_measurement_json(const struct etude_measurement *metric, char *out, size_t size);

/**
 * Sum processor accounting of disjoint scopes, with checked arithmetic.
 * The executor supplies inclusive final accounting once per owned tree, never
 * both a VM and its guest. Missing scopes make the sum unavailable. A lower
 * bound or unknown error stays such in the sum. Empty input is unavailable.
 */
struct etude_measurement etude_history_cpu_sum(const struct etude_measurement *metrics,
                                               size_t count);

/**
 * Observe a combined resident-memory sample of all disjoint owned scopes.
 * Values are simultaneous resident sizes, never individual process peaks.
 * Missing or overflowing samples permanently make the peak unavailable.
 * interval_us is the maximum observed sampling gap, including the initial
 * and final gaps. The result is sampled with unbounded error.
 */
void etude_history_memory_sample(struct etude_measurement *peak,
                                 const struct etude_measurement *resident, size_t count,
                                 uint64_t interval_us);

/** Initialize an accumulator with no observations and an unavailable reason. */
void etude_history_accounting_init(struct etude_accounting *accounting);

#ifndef _WIN32
#include <sys/resource.h>
/**
 * Convert wait4's final usage for one reaped child, including reaped children
 * charged to it by the OS. CPU is a lower bound: an unreaped or escaped
 * descendant may be absent. ru_maxrss is not combined resident memory, so
 * memory is unavailable. The executor retains PID and creation identity in
 * resources.scope and preserves any cleanup uncertainty separately.
 */
struct etude_accounting etude_history_wait_usage(const struct rusage *usage);
#else
/**
 * Collect cumulative CPU from a Job Object, including terminated members.
 * The executor owns the job, forbids breakaway and excludes nested accounting.
 * HANDLE is passed as void * to avoid Windows headers in common code. A job
 * memory limit or peak commit charge is not combined resident memory.
 */
struct etude_accounting etude_history_job_usage(void *job);
#endif
#endif
