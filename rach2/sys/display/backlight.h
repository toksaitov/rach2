/*
 * backlight.h: the backlight class, /sys/class/backlight/NAME, through
 * which user space reads and sets the brightness of a panel, as on Linux.
 */
#ifndef _BACKLIGHT_H_
#define _BACKLIGHT_H_

#include "../lib/kint.h"

/*
 * A driver of a panel's backlight registers it by name, such as
 * "intel_backlight", at boot; a backlight is never removed, but another
 * driver may take it over (`backlight_rebind`). Each is a
 * directory of /sys/class/backlight with five attribute files, which
 * procfs.c reads and writes through the calls below, the backlight known
 * by its index from 0 in the order of registration.
 *
 * The class keeps the level root set and whether root turned the backlight
 * off, and gives the driver the level set and whether the backlight is on:
 * off while bl_power is off or the console is blank (blank.h). A driver with
 * a switch turns the backlight off by it and keeps the level; one without
 * is given the level 0. bl_power shows what root wrote, whatever the
 * console does.
 */

/* The most backlights. */
#define BACKLIGHT_MAX 4

/* The attributes of a backlight's directory, in the order it lists them. */
#define BACKLIGHT_ACTUAL     0 /* actual_brightness: the level the hardware has */
#define BACKLIGHT_POWER      1 /* bl_power: BACKLIGHT_ON or BACKLIGHT_OFF; root writes it */
#define BACKLIGHT_BRIGHTNESS 2 /* brightness: the level set; root writes it */
#define BACKLIGHT_LEVELS     3 /* max_brightness: the highest level */
#define BACKLIGHT_TYPE       4 /* type: "raw", "platform" or "firmware" */
#define BACKLIGHT_NATTR      5

/* The values of bl_power, Linux's FB_BLANK_UNBLANK and FB_BLANK_POWERDOWN. */
#define BACKLIGHT_ON  0
#define BACKLIGHT_OFF 4

/*
 * What a driver gives the class. Each is called with interrupts off, from
 * the tick among other places, and must not sleep.
 */
struct backlight_ops {
    /** The level the hardware has now, 0 to the backlight's highest. */
    int (*get)(void *ctx);
    /** Give the panel `level`, 0 to the backlight's highest; 0 is as dark
     * as the hardware goes. */
    void (*set)(void *ctx, int level);
    /** Turn the backlight on or off and leave its level as it is, or NULL
     * for a driver with no switch. */
    void (*power)(void *ctx, int on);
};

/**
 * Register a backlight `name`, at most 31 bytes, whose levels run from 0 to
 * `max`, driven through `ops` with `ctx`, which stay valid for good. Its
 * level is taken from the hardware's. Returns its index, or -1 when
 * BACKLIGHT_MAX are registered, `max` is not positive or the name is too
 * long. At boot, before any reader.
 */
int backlight_register(const char *name, int max, const struct backlight_ops *ops, void *ctx);

/**
 * Drive the backlight `name` through `ops` and `ctx` from now on, with
 * `max` its highest level, as a driver does that takes the panel over from
 * the one that registered it: i915 from the boot's intelbl, and intelbl
 * again when i915's device goes. The level is read from the new driver,
 * whose units it is in, clamped to `max`, when the driver reads one above
 * 0. A driver that reads 0, which has no lit level to read, as i915 after
 * its device's reset or a stand-in, is given the level the class kept,
 * taken to `max`, through its `set` alone, so that a level root set
 * survives the change of driver; its switch is left as it is. A kept 0
 * that root never wrote, the firmware's dark, is not given, so that i915's
 * enable raises it as Linux's does. bl_power as
 * root set it is kept. Returns the backlight's index, or -1 for no
 * backlight of that name or a `max` that is not positive. Any context; it
 * does not sleep.
 */
int backlight_rebind(const char *name, int max, const struct backlight_ops *ops, void *ctx);

/**
 * Take `level`, 0 to the backlight's highest, as backlight `i`'s level
 * without giving it to the driver, which set it itself: i915 raises a
 * level at or below the panel's least to the most when it turns the
 * backlight on, as Linux's __intel_backlight_enable does, and the class
 * then shows that level, as Linux's props.brightness does. Logged with
 * `why`. Process context, with no lock of the driver's held.
 */
void backlight_driver_level(int i, int level, const char *why);

/** Name backlight `i`'s type, as its `type` file shows it: "raw" (the
 * default), "platform" or "firmware", as Linux's. At boot, before any
 * reader. */
void backlight_set_type(int i, const char *type);

/** Note that backlight `i`'s level 0 still lights the panel, as an ACPI
 * _BCL whose least level is above 0 does, so that a blank cannot darken
 * the panel by it. At boot, before any reader. */
void backlight_set_lit_at_zero(int i);

/** Whether a blank turns some panel's backlight dark: a backlight with a
 * switch, or one whose level 0 is dark. Any context. */
int backlight_can_darken(void);

/** Probe the drivers of the panels' backlights, which register what they
 * find: `intelbl_init` on a PC. Once, at boot, after PCI and paging. */
void backlight_init(void);

/** The PC's driver of the Intel PCH's PWM (intelbl.c). */
void intelbl_init(void);

/**
 * Turn every backlight off while `blank` is set, and on at its own level
 * again when it is clear; the console's blanking calls it. Any context; it
 * does not sleep.
 */
void backlight_console_blank(int blank);

/** Turn every backlight on at its own level again, as
 * `backlight_console_blank(0)` does but without the class's lock, which the
 * code a panic interrupted may hold; for `console_panic_unblank` alone. */
void backlight_panic_unblank(void);

/** The number of backlights registered. */
int backlight_count(void);

/** The name of backlight `i`, or NULL past the last. */
const char *backlight_name(int i);

/** The backlight named by the `len` bytes at `name`, or -1. */
int backlight_find(const char *name, int len);

/**
 * Write the text of attribute `attr` of backlight `i`, a number or a word
 * and a newline, into `out` of `cap` bytes, NUL-terminated. Returns its
 * length, or -ENODEV for no such backlight or attribute.
 */
int backlight_show(int i, int attr, char *out, int cap);

/**
 * Set attribute `attr` of backlight `i`, brightness or bl_power, from the
 * `len` bytes at `text`, a number as Linux's kstrtoul reads it with base 0
 * (`k_strtoul`); the caller has checked that the writer is privileged.
 * brightness takes 0 to max_brightness, and bl_power BACKLIGHT_ON or
 * BACKLIGHT_OFF. Returns 0, -EINVAL for text that is no such value,
 * -ERANGE for a number past an unsigned long, or -ENODEV for no such
 * backlight or a read-only attribute.
 */
int backlight_store(int i, int attr, const char *text, int len);

#endif /* _BACKLIGHT_H_ */
