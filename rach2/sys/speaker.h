/*
 * speaker.h: the machine's speaker, a square wave at a frequency, which a
 * program sounds through the console's KIOCSOUND request. A platform's
 * driver defines it: arch/x86/speaker.c on a PC, through the PIT's channel 2.
 */
#ifndef _SPEAKER_H_
#define _SPEAKER_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/** Drive the speaker at `frequency` Hz, until `speaker_off`; 0 silences it.
 * A frequency below the lowest the channel can make sounds the lowest (on a
 * PC, about 18 Hz). */
void speaker_on(uint32_t frequency);

/** Silence the speaker. */
void speaker_off(void);

#endif /* _SPEAKER_H_ */
