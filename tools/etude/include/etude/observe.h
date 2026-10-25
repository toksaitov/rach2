/* Native observations of load throughout a collection, not progress witnesses.
 */
#ifndef ETUDE_OBSERVE_H
#define ETUDE_OBSERVE_H

#include "etude/exec.h"
#include "etude/history.h"

#define ETUDE_OBSERVE_INTERVAL_US 1000000u
#define ETUDE_OBSERVE_BYTES       (16u << 20)
#define ETUDE_OBSERVE_PROCESSES   4096u

struct etude_observer;

/* Start a bounded trace, observing the descendants of root as owned. */
struct etude_observer *etude_observe_start(const char *path, const struct etude_pid *root,
                                           const char *run_id);
/* Keep this owner trace open until finish, after its worker has joined. */
struct etude_observer *etude_observe_start_terminal(const char *path, const struct etude_pid *root,
                                                    const char *run_id);
/* Join, take the actual terminal reading, then sample counters and close. */
int etude_observe_finish(struct etude_observer *observer, struct etude_reading *end);
/* Stop and join the sampling thread; its final sample ends the trace. */
int etude_observe_stop(struct etude_observer *observer);
/* Cancellation closes the trace with an incomplete footer. */
int etude_observe_cancel(struct etude_observer *observer);
int etude_observe_done(struct etude_observer *observer);
/* The summary is JSON from malloc. Missing accounting remains unavailable. */
char *etude_observe_summary(const struct etude_observer *observer,
                            const struct etude_measurement *owned);
void etude_observe_free(struct etude_observer *observer);
void etude_observe_bounds(const struct etude_observer *observer, uint64_t *start_utc_us,
                          uint64_t *end_utc_us);
int etude_observe_main(int argc, char **argv);
/* Read a complete trace; unavailable counters/coverage yield a summary with
 * comparison_available false. NULL denotes malformed or incomplete capture. */
char *etude_observe_read(const char *bytes, size_t length, const char *run_id,
                         uint64_t start_utc_us, uint64_t end_utc_us);
/* Verify the complete native trace and its coverage, returning JSON or NULL. */
char *etude_observe_import(const char *bytes, size_t length, const char *run_id,
                           uint64_t start_utc_us, uint64_t end_utc_us);
uint64_t etude_observe_utc(const char *utc);

#endif
