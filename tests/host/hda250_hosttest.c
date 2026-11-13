/**
 * Run hda_hosttest's rows at the shipped 250 Hz tick: a KDMKTONE of 100 ms
 * sounds for 25 ticks and the jacks are polled every 63, as Linux's
 * msecs_to_jiffies gives 100 ms and 250 ms at HZ=250, and the pin a beep
 * opened goes off after a second of 250 ticks.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/audio/resample.c
 */
#include "hda_hosttest.c"

/* The driver's periods in ticks, which the rows count in, at 250 Hz. */
typedef char hda250_ticks[HDA_JACK_TICKS == 63 && HDA_BEEP_IDLE_TICKS == 250 ? 1 : -1];
