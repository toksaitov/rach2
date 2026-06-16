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

/** Drive the PC speaker at `frequency` Hz; 0 silences it. */
void speaker_on(uint32_t frequency);

/** Silence the speaker. */
void speaker_off(void);

/** True while the speaker is gated on. */
bool speaker_is_on(void);

#endif /* _SPEAKER_H_ */
