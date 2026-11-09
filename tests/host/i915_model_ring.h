/*
 * i915_model_ring.h: the state of the host model's rings and interrupt
 * blocks (i915_model_ring.c), which a test reads, sets to make a ring
 * slow, stuck or dead, and through which it raises and takes interrupts.
 */
#ifndef _I915_MODEL_RING_H_
#define _I915_MODEL_RING_H_

#include "i915_model.h"

/*
 * One ring of the model. While it is enabled (RING_CTL's RING_VALID) and
 * not stopped (MI_MODE's STOP_RING) and not `stuck`, each wait of the
 * driver runs its commands from its head to its tail through the global
 * GTT. `executed` counts the commands run. A command the model does not
 * know, or an address it cannot reach, stops the ring there: `fault` is
 * set to that command's first dword and the ring runs nothing more.
 *
 * The test's switches: `stuck` runs nothing, `idle_stuck` never reports
 * MODE_IDLE, `start_fails` drops RING_VALID from each write of RING_CTL,
 * as a dead ring does, and `flush_stuck` never clears INSTPM's sync flush.
 */
struct i915_model_ring {
    uint32_t executed;
    uint32_t fault;
    int faulted;
    int stuck;
    int idle_stuck;
    int start_fails;
    int flush_stuck;
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
