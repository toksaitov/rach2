/*
 * i915_model_reset.h: the state of the host model's reset block
 * (i915_model_reset.c), GEN6_GDRST, which a test reads, and sets to make a
 * domain's reset never finish.
 */
#ifndef _I915_MODEL_RESET_H_
#define _I915_MODEL_RESET_H_

#include "i915_model.h"

/* GEN6_GDRST's domains the model knows: full, render, media, blitter and
 * the video enhancement engine's, bits 0 to 4. */
#define I915_MODEL_RESET_DOMAINS 5

/*
 * The reset. A write of GEN6_GDRST resets each domain whose bit it sets:
 * the rings of the render domain, the blitter's, the media domain's, the
 * video enhancement engine's, or of the whole GPU go back to their
 * power-on state, which ends a hang, and a reset of the
 * whole GPU clears every fence register as well; the bit then reads clear.
 * A domain whose bit is in `stuck`, the test's, resets nothing and its bit
 * stays set. `writes` counts the writes and `resets` each domain's resets
 * done, by bit.
 */
struct i915_model_reset {
    uint32_t stuck;
    uint32_t writes;
    uint32_t resets[I915_MODEL_RESET_DOMAINS];
};

/** The reset's state of `m`. */
struct i915_model_reset *i915_model_reset_state(struct i915_model *m);

#endif /* _I915_MODEL_RESET_H_ */
