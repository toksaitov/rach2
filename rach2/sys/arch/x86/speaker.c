#include "speaker.h"
#include "../../kio.h"

static bool _speaker_on = false;

/**
 * Drive the PC speaker at `frequency` Hz with a square wave.
 *
 * The speaker is gated by PIT channel 2: programming the channel to mode 3
 * (square wave) and connecting its output to the speaker produces a tone
 * whose pitch is the base PIT frequency divided by the loaded count. A
 * frequency of 0 has no valid divisor, so it silences the speaker instead.
 * The count is a 16-bit PIT register, so a sub-19 Hz request (whose divisor
 * exceeds 0xFFFF) is clamped to the lowest representable tone rather than
 * silently truncated to a wrong one.
 */
void speaker_on(uint32_t frequency)
{
    uint32_t divisor;
    uint8_t tmp;

    if (frequency == 0) {
        speaker_off();
        return;
    }

    divisor = SPEAKER_PIT_BASE_FREQ / frequency;
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

    _speaker_on = true;
}

/**
 * Silence the speaker by clearing its gate and enable bits.
 */
void speaker_off(void)
{
    uint8_t tmp;

    /* Clear port 0x61 bits 0 (timer gate) and 1 (speaker enable). */
    tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);

    _speaker_on = false;
}
