/*
 * speaker.h: the machine's speaker, a square wave at a frequency, which a
 * program sounds through the console's KIOCSOUND and KDMKTONE requests. A
 * platform's driver defines it: arch/x86/speaker.c on a PC, through the
 * PIT's channel 2, and through the audio driver's beep generator beside it
 * where the driver has one, such as an HD-audio codec's beep widget.
 */
#ifndef _SPEAKER_H_
#define _SPEAKER_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/* The PIT's input clock in Hz, which the console's requests divide, as
 * Linux's do, whatever the platform. */
#define SPEAKER_CLOCK 1193182u

/* What the speaker has been asked to do since boot, for /proc/console. */
struct speaker_counters {
    uint32_t tones;        /* KDMKTONE tones begun */
    uint32_t sounds;       /* KIOCSOUND sounds begun */
    uint32_t ends;         /* tones the tick ended at their length */
    uint32_t cuts;         /* tones and sounds a later request ended or replaced */
    uint32_t divisor;      /* the last tone's or sound's divisor */
    uint32_t ms;           /* the last tone's length in ms; 0 for a sound */
    uint32_t beeps;        /* tones and sounds the beep generator began */
    uint32_t beep_divider; /* the generator's divider as last sent; 0 silent */
    uint32_t beep_retries; /* stops the tick found the generator busy for and
                              sent again at a later tick */
};

/** Drive the speaker at `frequency` Hz, until `speaker_off`; 0 silences it.
 * A frequency below the lowest the channel can make sounds the lowest (on a
 * PC, about 18 Hz). */
void speaker_on(uint32_t frequency);

/** Drive the speaker at SPEAKER_CLOCK / `divisor` Hz, until `speaker_off`;
 * 0 silences it, and a divisor above 0xFFFF sounds 0xFFFF's tone. */
void speaker_on_divisor(uint32_t divisor);

/** Silence the speaker. */
void speaker_off(void);

/**
 * Sound SPEAKER_CLOCK / `divisor` Hz for `ms` milliseconds, then silence,
 * as Linux's KDMKTONE does; with `ms` 0 the speaker sounds until the next
 * request, as KIOCSOUND does. A divisor of 0, or one whose pitch is not
 * between 21 and 32766 Hz, silences the speaker, as Linux's console does.
 * Each call ends the tone or sound before it, and sounds or stops the beep
 * generator too, where there is one (speaker_set_beep), which may wait for
 * the codec's answers. Takes the speaker's lock with interrupts off; any
 * context but the tick's.
 */
void speaker_tone(uint32_t divisor, uint32_t ms);

/** End a tone whose length has passed, and stop the beep generator, which
 * the tick sends in a try and again at the next tick while the generator is
 * busy: the boot processor's tick calls it once a tick, with interrupts
 * off. A tick that finds a request holding the speaker's lock tries again
 * at the next. */
void speaker_tick(void);

/**
 * Give the speaker a beep generator, audio.h's `beep`, which every request
 * sounds beside the PIT's channel, as Linux sounds every input device that
 * has EV_SND; NULL takes it away. init_audio calls it once it has chosen a
 * driver whose device has one.
 */
void speaker_set_beep(int (*beep)(uint32_t hz, int tick));

/** Copy the speaker's counters into `out`. */
void speaker_get_counters(struct speaker_counters *out);

#endif /* _SPEAKER_H_ */
