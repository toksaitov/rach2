/*
 * i915_model_rc6.h: the state of the host model's RC6, pcode mailbox and
 * frequency blocks (i915_model_rc6.c), which a test reads, and sets to
 * make the GT busy or the pcode slow, stuck or failing.
 */
#ifndef _I915_MODEL_RC6_H_
#define _I915_MODEL_RC6_H_

#include "i915_model.h"

/* The Lenovo's frequencies under Linux (dev-log/76-graphics/research/work/
 * tonight/lenovo.md): RPn and RP1 200 MHz, RP0 1000 MHz, RPe 500 MHz, and
 * boost 1000 MHz, so no overclocking; in the chip's units of 50 MHz. */
#define LENOVO_RP_STATE_CAP 0x00040414u /* RPn 4 << 16 | RP1 4 << 8 | RP0 20 */
#define LENOVO_PCODE_DDCC   0x00000a00u /* RPe 10 in bits 15:8 */

/* The most writes of the ring's frequency table the model keeps. */
#define I915_MODEL_FREQ_TABLE 64

/*
 * RC6. The GT is awake while `busy`, the test's, is set or the kernel
 * holds forcewake; otherwise it is idle. An idle GT is in RC6 when
 * GEN6_RC_CONTROL enables RC6 and either the hardware's timers are on
 * (GEN6_RC_CTL_HW_ENABLE) and it has been idle for GEN6_RC6_THRESHOLD
 * units of 1.28 us, or they are off and GEN6_RC_STATE's software target
 * is RC6. `in_rc6` is whether it was at the last tick, `idle_us` how long
 * it has been idle, and `rc6_us` the microseconds spent in RC6. The
 * residency counter, GEN6_GT_GFX_RC6, counts `residency` units of 1.28 us,
 * `frac` hundredths of a microsecond toward the next; a test may set it
 * near its wrap.
 *
 * The pcode mailbox. A command written with GEN6_PCODE_READY is taken
 * after `delay_us`, at once for 0, and never while `stuck` is set; then
 * the ready bit clears and the low byte holds `error`, 0 for success, or
 * GEN6_PCODE_ILLEGAL_CMD for a command the model does not know. It
 * answers the duty cycle control with `ddcc`, the overclocking parameters
 * with `oc_params` and the RC6 voltages with `rc6vids`, keeps each write
 * of the ring's frequency table in `table`, `ntable` of them, and puts the
 * data of GEN6_PCODE_WRITE_D_COMP in D_COMP_HSW. `commands` counts the
 * commands taken, `last_cmd` is the last.
 */
struct i915_model_rc6 {
    int busy;
    int in_rc6;
    uint32_t idle_us;
    uint64_t rc6_us;
    uint32_t residency;
    uint32_t frac;

    uint32_t delay_us;
    int stuck;
    uint32_t error;
    uint32_t ddcc;
    uint32_t oc_params;
    uint32_t rc6vids;
    uint32_t table[I915_MODEL_FREQ_TABLE];
    uint32_t ntable;
    uint32_t commands;
    uint32_t last_cmd;
    int pending;
    uint32_t pending_us;
    uint32_t pending_cmd;
};

/** The RC6 and pcode block's state of `m`. */
struct i915_model_rc6 *i915_model_rc6(struct i915_model *m);

#endif /* _I915_MODEL_RC6_H_ */
