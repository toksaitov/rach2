/* Private platform readings; unavailable fields have no value. */
#ifndef ETUDE_OBSERVE_SYSTEM_H
#define ETUDE_OBSERVE_SYSTEM_H

#include "etude/observe.h"

enum load_field {
    LOAD_CPU,
    LOAD_MEMORY,
    LOAD_SWAP_IN,
    LOAD_SWAP_OUT,
    LOAD_DISK_READ,
    LOAD_DISK_WRITE,
    LOAD_PRESSURE,
    LOAD_FIELDS
};

struct load_process {
    struct etude_pid id;
    long parent;
    uint64_t cpu_us;
    unsigned scope; /* 0 unknown, 1 owned, 2 outside, 3 shared virtualizer */
};

struct load_snapshot {
    uint64_t monotonic_us, utc_us;
    uint64_t value[LOAD_FIELDS];
    unsigned available;
    size_t count, denied;
    int overflow;
};

/* Read exclusive per-process CPU, never a parent's reaped-child accounting. */
int etude_load_snapshot(struct load_snapshot *s, struct load_process *p, size_t capacity);
/* Read a sample's clocks. UTC comes from the clock that etude_history_now
 * reads, so a trace's bounds compare with its caller's readings. */
void etude_load_clock(uint64_t *monotonic_us, uint64_t *utc_us);
const char *etude_load_platform(void);
const char *etude_load_field_name(unsigned field);

#endif
