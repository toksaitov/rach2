/*
 * i915_model_base.h: the state of the host model's forcewake, GT FIFO and
 * display power well blocks (i915_model_base.c), which a test reads and
 * sets to make the chip slow or stuck.
 */
#ifndef _I915_MODEL_BASE_H_
#define _I915_MODEL_BASE_H_

#include "i915_model.h"

/*
 * Forcewake. `req` is FORCEWAKE_MT's request bits as the masked writes
 * left them. Each write makes the acknowledgement, FORCEWAKE_ACK_HSW in
 * the register file, follow the request after `ack_delay_us`, 0 at once;
 * while `ack_stuck` is set it does not follow at all.
 */
struct i915_model_fw {
    uint32_t req;
    uint32_t ack_delay_us;
    int ack_stuck;
    int pending;
    uint32_t pending_us;
};

/*
 * The GT FIFO: GTFIFOCTL reads `free` free entries, which grow by
 * `refill_per_us` each microsecond the clock moves, up to `max`.
 */
struct i915_model_fifo {
    uint32_t free;
    uint32_t max;
    uint32_t refill_per_us;
};

/*
 * The display power well. `req` holds the request bit of each of its four
 * registers, BIOS, driver, KVMR and debug; `on` is its state, which each
 * register's state bit shows. A change of the requests turns the well on
 * when any is set, off when none is, after `delay_us`, 0 at once; while
 * `stuck` is set the state does not change.
 */
struct i915_model_pw {
    uint32_t req[4];
    int on;
    uint32_t delay_us;
    int stuck;
    int pending;
    uint32_t pending_us;
};

/** The forcewake block's state of `m`. */
struct i915_model_fw *i915_model_forcewake(struct i915_model *m);

/** The GT FIFO block's state of `m`. */
struct i915_model_fifo *i915_model_gt_fifo(struct i915_model *m);

/** The power well block's state of `m`. */
struct i915_model_pw *i915_model_power(struct i915_model *m);

#endif /* _I915_MODEL_BASE_H_ */
