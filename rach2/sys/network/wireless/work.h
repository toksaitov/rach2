/* The wireless worker's private calls, the scan and the BSS table. */
#ifndef _WLAN_WORK_H_
#define _WLAN_WORK_H_

#include "wireless.h"

/* How long a scan stays on a channel, in milliseconds, as Linux's mac80211
 * does: long enough for probe responses on an active channel, and for one
 * beacon at the usual interval of 102.4 ms on a passive one. */
#define WLAN_SCAN_ACTIVE_DWELL_MS  30
#define WLAN_SCAN_PASSIVE_DWELL_MS 110

/* A BSS not heard from for this long leaves the table unless it is held,
 * cfg80211's IEEE80211_SCAN_RESULT_EXPIRE. */
#define WLAN_BSS_EXPIRE_MS 30000

/* Set in the cookie of every probe request a software scan sends, so that
 * whoever routes transmit status can tell it from the data path's and drop
 * it; the data path's cookies leave this bit clear. */
#define WLAN_SCAN_COOKIE ((uint64_t)1 << 62)

/** Start the wireless worker's kernel thread, `kwlan`, if it is not running.
 * Process context under the kernel lock; the registry calls it before its
 * first radio starts. Before the first program has its pid the thread is
 * only marked wanted, so that init takes pid 1, and wlan_work_threads_start
 * makes it. Returns 0 or -ENOMEM. Work queued before the thread exists
 * waits for it. */
int wlan_work_start(void);

/** Make the thread a radio asked for before the first program started,
 * and every later one at once. platform_threads_start calls it once init
 * has its pid. */
void wlan_work_threads_start(void);

/**
 * Run, in the order of their due ticks, every queued work due at `now`,
 * including work those callbacks queue for `now` or earlier. Work with
 * equal due ticks runs in the order it was queued. The worker thread calls
 * it with the kernel lock; the host test calls it to stand in for the
 * thread. Returns the number of callbacks run.
 */
unsigned wlan_work_run_due(uint64_t now);

/** The due tick of the earliest queued work, 1 for immediate work, or 0
 * when nothing is queued. IRQ-safe. */
uint64_t wlan_work_next_due(void);

/* The table of BSSs heard, owned by the worker. Entries are allocated one
 * at a time and keep their slot until they leave; a dump walks the slots. */
struct wlan_bss_table {
    struct wlan_bss *slots[WLAN_MAX_BSS];
    unsigned count;
    uint32_t generation; /* changes whenever an entry comes or goes */
};

/** Make `table` empty. */
void wlan_bss_init(struct wlan_bss_table *table);

/** Free every entry, held or not, at the radio's unregistration, when no
 * holder remains. */
void wlan_bss_destroy(struct wlan_bss_table *table);

/**
 * Take a received beacon or probe response into the table: a new entry, or
 * the one with its BSSID and channel updated. The channel is the DS
 * Parameter Set's when the frame has one, else the receive frequency; a
 * channel not in `band`, or disabled there, drops the frame. A beacon whose
 * SSID is hidden (empty or all zero bytes) keeps the SSID and elements an
 * earlier probe response gave the entry. When the table is full the oldest
 * entry nobody holds makes room.
 *
 * Worker only, kernel lock held. The frame and status are borrowed until
 * return. Returns 0, -EINVAL for a frame that is not a well-formed beacon or
 * probe response or whose elements exceed WLAN_MAX_BSS_IES, -ENOSPC when
 * every entry is held, or -ENOMEM.
 */
int wlan_bss_rx(struct wlan_bss_table *table, const struct wlan_band *band, const uint8_t *frame,
                unsigned len, const struct wlan_rx_status *status, uint64_t now);

/** The entry for `bssid` on `frequency` MHz, or on any channel when it is
 * zero, or NULL. The pointer is valid until the worker next changes the
 * table, unless the caller holds the entry. */
struct wlan_bss *wlan_bss_find(struct wlan_bss_table *table, const uint8_t bssid[6],
                               unsigned frequency);

/** Hold `bss`, so that neither aging nor eviction removes it: the MLME
 * holds the current BSS from authentication until it leaves. Worker only. */
void wlan_bss_hold(struct wlan_bss *bss);

/** Drop a hold; an entry older than the expiry with no holder left goes at
 * the next expiry. Worker only. */
void wlan_bss_put(struct wlan_bss *bss);

/** Remove every entry nobody holds that was last heard more than `max_age`
 * ticks before `now`. Worker or process context under the kernel lock,
 * never in an interrupt; nl80211 calls it before a scan dump. */
void wlan_bss_expire(struct wlan_bss_table *table, uint64_t now, uint64_t max_age);

/** Remove every entry nobody holds, as a scan with the flush flag asks. */
void wlan_bss_flush(struct wlan_bss_table *table);

/** The next entry at or after slot `*cursor`, advancing `*cursor` past it,
 * or NULL when none is left: a dump keeps the cursor between reads and
 * compares `generation` to tell that the table changed under it. */
struct wlan_bss *wlan_bss_next(struct wlan_bss_table *table, unsigned *cursor);

enum wlan_scan_state { WLAN_SCAN_IDLE, WLAN_SCAN_SOFTWARE, WLAN_SCAN_HARDWARE };

/* One of the scan's work records, which finds its scan from the work. */
struct wlan_scan_work {
    struct wlan_work work;
    struct wlan_scan *scan;
};

/* How the scan reports its events: the registry passes `wlan_event`. */
typedef void (*wlan_event_fn)(struct wlan *wlan, const struct wlan_event *event);

/* A radio's scan. The registry embeds it in its station record and attaches
 * it once; every field is the worker's. */
struct wlan_scan {
    struct wlan *wlan;
    wlan_event_fn event;
    struct wlan_radio *radio;
    const struct wlan_band *band; /* channels after the regulatory rules */
    struct wlan_bss_table *table;
    struct wlan_scan_request request; /* a copy of the running request */
    uint16_t frequencies[WLAN_RADIO_MAX_CHANNELS];
    unsigned n_frequencies; /* the channels this scan visits */
    unsigned next;          /* the index of the next channel to visit */
    enum wlan_scan_state state;
    int start_pending;               /* the scan is accepted, not yet begun */
    int abort;                       /* an abort was asked for */
    volatile int hw_done;            /* the radio reported its scan done */
    volatile int hw_aborted;         /* and that it was cancelled */
    struct wlan_scan_work steps[2];  /* a step queues the other one */
    unsigned step;                   /* which of `steps` comes next */
    struct wlan_scan_work done;      /* the completion of a hardware scan */
    const struct wlan_channel *home; /* the channel to return to, or NULL */
    unsigned home_filters;
    uint64_t started;        /* the tick the scan began */
    uint64_t probe_cookie;   /* the last probe request's cookie */
    uint16_t sequence;       /* the last probe request's sequence number */
    uint32_t probes_sent;    /* probe requests the radio accepted */
    uint32_t channels_tuned; /* channels the software scan visited */
};

/** Attach `scan` to its radio, its band after the regulatory rules and its
 * BSS table, idle, reporting its events through `event`. The registry calls
 * it once at registration; the pointers stay valid until `wlan_scan_detach`.
 * Taking the event call as an argument keeps scan.c free of a link-time
 * dependence on nl80211. */
void wlan_scan_attach(struct wlan_scan *scan, struct wlan *wlan, wlan_event_fn event,
                      struct wlan_radio *radio, const struct wlan_band *band,
                      struct wlan_bss_table *table);

/** Cancel the scan's work and forget a running scan, emitting nothing, at
 * unregistration once the radio is stopped. Process context, kernel lock,
 * no net_lock; may sleep. Not from the worker. */
void wlan_scan_detach(struct wlan_scan *scan);

/**
 * Accept a scan and queue its start on the worker, which emits
 * WLAN_EVENT_SCAN_STARTED and later WLAN_EVENT_SCAN_DONE, or
 * WLAN_EVENT_SCAN_ABORTED, with the request's cookie. The scan visits the
 * requested channels, or every channel `band` allows; it probes for the
 * request's SSIDs only on a channel the regulatory rules leave active, and
 * listens on the rest. No SSIDs make a passive scan. The radio's own
 * `scan_start` is tried first; -EOPNOTSUPP from it selects the software scan.
 *
 * The request is copied. Worker or process context under the kernel lock,
 * no net_lock, never in an interrupt. Returns 0, -EBUSY while a scan runs,
 * or -EINVAL for an SSID longer than 32 bytes, too many SSIDs or frequencies,
 * a frequency outside the band, no channel left to visit, or elements longer
 * than WLAN_MAX_SCAN_IE.
 */
int wlan_scan_start(struct wlan_scan *scan, const struct wlan_scan_request *request);

/** Abort the running scan: the radio's scan is cancelled, or the software
 * scan goes back to the home channel, and the worker emits
 * WLAN_EVENT_SCAN_ABORTED. Context as `wlan_scan_start`; it may sleep when
 * called outside the worker. Returns 0, or -ENOENT when no scan runs. */
int wlan_scan_abort(struct wlan_scan *scan);

/** Whether a scan is accepted and not yet finished. */
int wlan_scan_running(const struct wlan_scan *scan);

/** Set the channel and filters a scan returns the radio to, or NULL for
 * none: the MLME sets the operating channel while it is associated. The
 * channel is borrowed until it is set again. Worker only. */
void wlan_scan_set_home(struct wlan_scan *scan, const struct wlan_channel *channel,
                        unsigned filters);

/** The registry's `wlan_radio_scan_done` calls it for the radio's scan: it
 * records the outcome and queues the completion. IRQ-safe under the kernel
 * lock; a completion with no hardware scan running is ignored. */
void wlan_scan_hw_done(struct wlan_scan *scan, int aborted);

#endif
