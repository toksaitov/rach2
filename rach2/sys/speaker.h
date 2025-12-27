#ifndef _SPEAKER_H_
#define _SPEAKER_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/*
 * PC Speaker Driver
 *
 * Uses PIT Channel 2 for frequency generation and port 0x61 for speaker gate.
 * The speaker produces a square wave at the specified frequency.
 */

/* PIT base oscillator frequency (Hz) */
#define SPEAKER_PIT_BASE_FREQ 1193182

/* I/O ports */
#define SPEAKER_PORT    0x61
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43

/*
 * Turn on the PC speaker at the specified frequency (Hz).
 * frequency: Tone frequency in Hz (e.g., 440 for A4)
 */
void speaker_on(uint32_t frequency);

/*
 * Turn off the PC speaker.
 */
void speaker_off(void);

/*
 * Check if the speaker is currently on.
 */
bool speaker_is_on(void);

#endif /* _SPEAKER_H_ */
