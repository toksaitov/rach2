#ifndef _SPEAKER_H_
#define _SPEAKER_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/*
 * The machine's speaker: a square wave at a frequency, which the console's
 * bell and a program's tone ioctl sound. A platform's driver defines it:
 * arch/x86/speaker.c on a PC, through the PIT's channel 2.
 */

/** Drive the speaker at `frequency` Hz; 0 silences it. */
void speaker_on(uint32_t frequency);

/** Silence the speaker. */
void speaker_off(void);

#endif /* _SPEAKER_H_ */
