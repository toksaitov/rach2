/*
 * backlight.h: the backlight class, /sys/class/backlight/NAME, through
 * which user space reads and sets the brightness of a panel, as on Linux.
 */
#ifndef _BACKLIGHT_H_
#define _BACKLIGHT_H_

#include "../lib/kint.h"

/*
 * A driver of a panel's backlight registers it by name, such as
 * "intel_backlight", at boot; a backlight is never removed. Each is a
 * directory of /sys/class/backlight with five attribute files, which
 * procfs.c reads and writes through the calls below, the backlight known
 * by its index from 0 in the order of registration.
 */

/* The most backlights. */
#define BACKLIGHT_MAX 4

/* The attributes of a backlight's directory, in the order it lists them. */
#define BACKLIGHT_ACTUAL     0 /* actual_brightness: the level the hardware has */
#define BACKLIGHT_POWER      1 /* bl_power: BACKLIGHT_ON or BACKLIGHT_OFF; root writes it */
#define BACKLIGHT_BRIGHTNESS 2 /* brightness: the level set; root writes it */
#define BACKLIGHT_LEVELS     3 /* max_brightness: the highest level */
#define BACKLIGHT_TYPE       4 /* type: "raw", a level of the hardware's own */
#define BACKLIGHT_NATTR      5

/* The values of bl_power, Linux's FB_BLANK_UNBLANK and FB_BLANK_POWERDOWN. */
#define BACKLIGHT_ON  0
#define BACKLIGHT_OFF 4

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
 * `len` bytes at `text`, a decimal number with an optional newline after
 * it; the caller has checked that the writer is privileged. brightness
 * takes 0 to max_brightness, and bl_power BACKLIGHT_ON or BACKLIGHT_OFF.
 * Returns 0, -EINVAL for text that is no such value, or -ENODEV for no such
 * backlight or a read-only attribute.
 */
int backlight_store(int i, int attr, const char *text, int len);

#endif /* _BACKLIGHT_H_ */
