#include "speaker.h"
#include "kio.h"

static bool _speaker_on = false;

void speaker_on(uint32_t frequency)
{
    uint32_t divisor;
    uint8_t tmp;

    if (frequency == 0) {
        speaker_off();
        return;
    }

    /* Calculate divisor for PIT */
    divisor = SPEAKER_PIT_BASE_FREQ / frequency;

    /* Configure PIT channel 2 for square wave generation
     * Command: 0xB6 = channel 2, access mode lobyte/hibyte, mode 3 (square wave)
     */
    outb(PIT_COMMAND, 0xB6);

    /* Send frequency divisor (low byte first, then high byte) */
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    /* Enable speaker: set bits 0 (timer gate) and 1 (speaker enable) */
    tmp = inb(SPEAKER_PORT);
    if ((tmp & 0x03) != 0x03) {
        outb(SPEAKER_PORT, tmp | 0x03);
    }

    _speaker_on = true;
}

void speaker_off(void)
{
    uint8_t tmp;

    /* Disable speaker: clear bits 0 and 1 */
    tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);

    _speaker_on = false;
}

bool speaker_is_on(void)
{
    return _speaker_on;
}
