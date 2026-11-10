/*
 * i915_model_ring.h: the state of the host model's rings and interrupt
 * blocks (i915_model_ring.c), which a test reads, sets to make a ring
 * slow, stuck or dead, and through which it raises and takes interrupts.
 */
#ifndef _I915_MODEL_RING_H_
#define _I915_MODEL_RING_H_

#include "i915_model.h"

/*
 * The commands of contexts and flushes a ring runs (i915_model_ctx.c).
 * MI_SET_CONTEXT, the render ring's alone, to an image other than the one
 * CCID names saves the context registers, INSTPM, CACHE_MODE_0_GEN7 and
 * CACHE_MODE_1, into the old image behind a marker, and restores them
 * from the new one unless MI_RESTORE_INHIBIT is set; a restore of an image
 * that holds no saved state faults the ring, as does a switch while
 * arbitration is on. `switches`, `saves`, `restores` and `inhibited` count
 * them, and `pipe_controls` and `flush_dws` the flushes run.
 */
struct i915_model_ctx {
    uint32_t switches;
    uint32_t saves;
    uint32_t restores;
    uint32_t inhibited;
    uint32_t pipe_controls;
    uint32_t flush_dws;
    int arb_off;
};

/* The marker a saved image begins with, and the dwords after it that hold
 * INSTPM, CACHE_MODE_0_GEN7 and CACHE_MODE_1. */
#define I915_MODEL_CTX_SAVED 0x1915c0deu

/*
 * One ring of the model. While it is enabled (RING_CTL's RING_VALID) and
 * not stopped (MI_MODE's STOP_RING) and not `stuck`, each wait of the
 * driver runs its commands from its head to its tail through the global
 * GTT. `executed` counts the commands run. MI_BATCH_BUFFER_START runs a
 * batch of stores, through the aliasing PPGTT when its bit 8 is set, which
 * the ring walks from PP_DIR_BASE while RING_MODE_GEN7's PPGTT bit is set
 * and PP_DIR_DCLV marks the address valid; `batches` counts them. A reset
 * of the ring clears those registers. A command the model does not know,
 * or an address it cannot reach, stops the ring there: `fault` is set to
 * that command's first dword and the ring runs nothing more.
 *
 * `ctx` is the state of its contexts and flushes.
 *
 * The test's switches: `stuck` runs nothing, `idle_stuck` never reports
 * MODE_IDLE, `start_fails` drops RING_VALID from each write of RING_CTL,
 * as a dead ring does, and `flush_stuck` never clears INSTPM's sync flush.
 */
struct i915_model_ring {
    uint32_t executed;
    uint32_t batches;
    uint32_t fault;
    int faulted;
    int stuck;
    int idle_stuck;
    int start_fails;
    int flush_stuck;
    struct i915_model_ctx ctx;
};

/*
 * The interrupts. The identity registers latch an event whose mask bit is
 * clear and are cleared by writing ones; the device raises a message on
 * each rise of its line, which is up while the display engine's master
 * enable is set and any bank has an identity bit that its enable passes,
 * the south display's through the display engine's PCH event. `messages`
 * counts the messages raised and `pending` those the test has not taken
 * yet (`i915_model_take_message`).
 *
 * `dma` is the test's: it gives the host address of the page at the
 * physical address `phys`, or NULL, so that the rings read their commands
 * and write their memory.
 */
struct i915_model_irq {
    int line;
    uint32_t messages;
    uint32_t pending;
    void *(*dma)(struct i915_model *m, uint64_t phys);
};

/** The render ring's state of `m` (0) or the blitter's (1). */
struct i915_model_ring *i915_model_ring_state(struct i915_model *m, int ring);

/** The interrupt state of `m`. */
struct i915_model_irq *i915_model_irq_state(struct i915_model *m);

/** Raise the events `bits` in the bank `bank` (I915_IRQ_ of i915.h), as
 * the chip does when they happen. */
void i915_model_irq_event(struct i915_model *m, int bank, uint32_t bits);

/** Take one pending message: 1, or 0 when none is pending. */
int i915_model_take_message(struct i915_model *m);

#endif /* _I915_MODEL_RING_H_ */
