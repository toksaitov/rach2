/*
 * intelbl.h: the PWM of an Intel PCH that drives a panel's backlight
 * (intelbl.c): the boot's driver of "intel_backlight", and the code i915
 * drives the same PWM with once it binds the graphics device.
 */
#ifndef _INTELBL_H_
#define _INTELBL_H_

#include "../lib/kint.h"

/*
 * A PWM, driven through `rd` and `wr` on `io`, which take a register's
 * offset in the graphics device's BAR 0: the pages intelbl_init mapped at
 * boot, i915's register access once it binds, or a host test's model.
 * `max` is the PWM's period, the highest level; `inverted` says the
 * polarity bit was set, so that the duty cycle counts the dark part;
 * `switched` says the panel power sequencer's backlight enable turns the
 * panel off.
 */
struct intelbl {
    uint32_t (*rd)(void *io, uint32_t reg);
    void (*wr)(void *io, uint32_t reg, uint32_t value);
    void *io;
    int max;
    int inverted;
    int switched;
};

/**
 * Take the PWM the firmware left: 0, with `bl` ready for `intelbl_get` and
 * `intelbl_set`, or -1 with the reason in `*why` when the PCH's PWM is off
 * or has no period, which leaves it untouched. A processor's PWM that still
 * drives the panel gives its level to the PCH's and is turned off, with the
 * PCH's override set, as i915's lpt_setup_backlight does.
 */
int intelbl_attach(struct intelbl *bl, const char **why);

/** Set `switched` from the panel power sequencer: its power and its
 * backlight enable both on. Nothing is written. */
void intelbl_attach_switch(struct intelbl *bl);

/** The level the PWM gives the panel now, of the `struct intelbl` at
 * `ctx`: a backlight class `get`. */
int intelbl_get(void *ctx);

/** Give the panel `level`, clamped to 0 and the period: a backlight class
 * `set`. */
void intelbl_set(void *ctx, int level);

/** Turn the backlight enable line of the panel power sequencer on or off,
 * leaving the duty cycle as it is: a backlight class `power`. */
void intelbl_power(void *ctx, int on);

/**
 * Give "intel_backlight" back to the PWM intelbl_init attached at boot,
 * through the pages it mapped, as i915 does when its device goes. 0, or -1
 * when the boot registered no backlight of its own.
 */
int intelbl_take_back(void);

#endif /* _INTELBL_H_ */
