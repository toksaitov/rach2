/*
 * speaker.c: the PC speaker, as speaker.h describes it. The PIT's channel 2
 * makes the frequency, port 0x61's low two bits gate the channel to the
 * speaker, the audio driver's beep generator sounds each tone too, and the
 * tick ends a tone of a given length.
 */
#include "portio.h"
#include "../../speaker.h"
#include "../../timer.h"
#include "../../lib/kerrno.h"
#include "../../lib/spinlock.h"

/* I/O ports */
#define SPEAKER_PORT 0x61
#define PIT_CHANNEL2 0x42
#define PIT_COMMAND  0x43

/* The pitches Linux's console sounds; it silences any other. */
#define SPEAKER_LOW_HZ  21u
#define SPEAKER_HIGH_HZ 32766u

/* The tone a request began, under speaker_lock. */
static struct spinlock speaker_lock = SPINLOCK_INIT;
static volatile int tone_timed;            /* a tone sounds until tone_end */
static uint32_t tone_end;                  /* the low half of kernel_ticks it ends at */
static int sounding;                       /* a tone or a sound has not ended */
static int (*beep)(uint32_t hz, int tick); /* the beep generator, or NULL */
static volatile int beep_owed;             /* the tick has yet to stop the generator */
static struct speaker_counters counters;

/** Drive the speaker at `frequency` Hz, through `speaker_on_divisor`. */
void speaker_on(uint32_t frequency)
{
    if (frequency == 0) {
        speaker_off();
        return;
    }
    speaker_on_divisor(SPEAKER_CLOCK / frequency);
}

/**
 * Programming PIT channel 2 to mode 3, the square wave, and connecting its
 * output to the speaker produces a tone whose pitch is SPEAKER_CLOCK divided
 * by the loaded count. The count is a 16-bit register, so a larger divisor,
 * which a request below 19 Hz makes, is clamped to 0xFFFF, the lowest tone
 * the count can make.
 */
void speaker_on_divisor(uint32_t divisor)
{
    uint8_t tmp;

    if (divisor == 0) {
        speaker_off();
        return;
    }
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }

    /* 0xB6: channel 2, access lobyte/hibyte, mode 3 (square wave). */
    outb(PIT_COMMAND, 0xB6);

    /* Channel 2 count, low byte then high byte. */
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    /* Port 0x61 bit 0 = timer gate, bit 1 = speaker enable. */
    tmp = inb(SPEAKER_PORT);
    if ((tmp & 0x03) != 0x03) {
        outb(SPEAKER_PORT, tmp | 0x03);
    }
}

void speaker_off(void)
{
    uint8_t tmp;

    /* Clear port 0x61 bits 0 (timer gate) and 1 (speaker enable). */
    tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);
}

void speaker_tone(uint32_t divisor, uint32_t ms)
{
    uint32_t flags;
    uint32_t hz = divisor ? SPEAKER_CLOCK / divisor : 0;

    flags = spin_lock_irqsave(&speaker_lock);
    if (sounding) {
        counters.cuts++;
    }
    tone_timed = 0;
    sounding = 0;
    beep_owed = 0;
    if (hz < SPEAKER_LOW_HZ || hz > SPEAKER_HIGH_HZ) {
        speaker_off();
        hz = 0;
    } else {
        speaker_on_divisor(divisor);
        sounding = 1;
        counters.divisor = divisor;
        counters.ms = ms;
        if (ms) {
            counters.tones++;
            tone_end = (uint32_t)kernel_ticks + MS_TO_TICKS(ms);
            tone_timed = 1;
        } else {
            counters.sounds++;
        }
    }
    if (beep) {
        int div = beep(hz, 0);

        if (div > 0)
            counters.beeps++;
        counters.beep_divider = div > 0 ? (uint32_t)div : 0;
    }
    spin_unlock_irqrestore(&speaker_lock, flags);
}

void speaker_tick(void)
{
    if (!tone_timed && !beep_owed) {
        return;
    }
    if (!spin_trylock(&speaker_lock)) {
        return;
    }
    if (tone_timed && (int32_t)((uint32_t)kernel_ticks - tone_end) >= 0) {
        speaker_off();
        tone_timed = 0;
        sounding = 0;
        counters.ends++;
        beep_owed = beep != NULL;
    }
    if (beep_owed) {
        if (beep(0, 1) == -EBUSY) {
            counters.beep_retries++;
        } else {
            beep_owed = 0;
            counters.beep_divider = 0;
        }
    }
    spin_unlock(&speaker_lock);
}

void speaker_set_beep(int (*fn)(uint32_t hz, int tick))
{
    uint32_t flags = spin_lock_irqsave(&speaker_lock);

    beep = fn;
    beep_owed = 0;
    spin_unlock_irqrestore(&speaker_lock, flags);
}

void speaker_get_counters(struct speaker_counters *out)
{
    uint32_t flags = spin_lock_irqsave(&speaker_lock);

    *out = counters;
    spin_unlock_irqrestore(&speaker_lock, flags);
}
