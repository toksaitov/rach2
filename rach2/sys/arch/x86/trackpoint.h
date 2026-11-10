#ifndef _TRACKPOINT_H_
#define _TRACKPOINT_H_

/*
 * trackpoint.h: a TrackPoint, the pointing stick of IBM's design that
 * ThinkPads put between the keys, on a touchpad's pass-through port, found
 * and set up as Linux's trackpoint.c does: its kind, IBM, ALPS, Elan, NXP,
 * JYT Synaptics or Synaptics, by the read-ID command; its buttons; its
 * power-on reset or Linux's settings; and the settings as files of its
 * serio port in /sys, as Linux has them.
 */

#include "../../lib/kint.h"

struct ps2io;

/* The kinds, the first byte of the answer to the read-ID command, Linux's
 * TP_VARIANT_*. */
#define TP_VARIANT_IBM           0x01
#define TP_VARIANT_ALPS          0x02
#define TP_VARIANT_ELAN          0x03
#define TP_VARIANT_NXP           0x04
#define TP_VARIANT_JYT_SYNAPTICS 0x05
#define TP_VARIANT_SYNAPTICS     0x06

/**
 * Ask the device on `io` for a TrackPoint's ID and, when it is one, set it
 * up as Linux's `trackpoint_detect` does with `set_properties`: the
 * settings at their power-on values; an IBM stick's buttons by its
 * extended button query, three for the others; an IBM stick's power-on
 * reset, and Linux's settings written to any stick that was not reset.
 * Returns 0 for a TrackPoint, which `io` then names until the next
 * detection, or -1 when the device is none. With interrupts off during the
 * bring-up, or inside a run of `ps2mouse_cmd_begin`.
 */
int trackpoint_detect(const struct ps2io *io);

/** Ask the device on `io` for a TrackPoint's ID alone, as Linux's
 * `trackpoint_detect` does without `set_properties`: 0 for a TrackPoint,
 * -1 when the device is none. */
int trackpoint_identify(const struct ps2io *io);

/** The vendor of the stick found, as Linux's `trackpoint_variants` names
 * it ("IBM", "ALPS" and so on), and its buttons, the low nibble of the
 * extended button query: three when the stick has a middle button. */
const char *trackpoint_vendor(void);
int trackpoint_buttons(void);

/**
 * Set the stick up again after its touchpad's reconnect, as Linux's
 * `trackpoint_reconnect` does: its ID read, an IBM stick's power-on reset,
 * and the settings written again, all of them when the stick was not
 * reset, only those not at their power-on values when it was. Returns 0,
 * or -1 when the device no longer answers as a TrackPoint.
 */
int trackpoint_reconnect(const struct ps2io *io);

/**
 * Add the stick's settings below `dir`, its serio port's directory in /sys,
 * as Linux's `trackpoint_attr_group` shows them: all fourteen for an IBM
 * stick, `sensitivity` and `press_to_select` for the others. Each reads as
 * its value and a newline; a write by root changes it on the stick, with
 * the touchpad and the stick disabled meanwhile. Returns 0, or -errno.
 * Process context.
 */
int trackpoint_sysfs_add(int dir);

#endif /* _TRACKPOINT_H_ */
