/*
 * speaker.c: the PC speaker, as speaker.h describes it. The PIT's channel 2
 * makes the frequency, and port 0x61's low two bits gate the channel to the
 * speaker.
 */
#include "portio.h"
#include "../../speaker.h"

/* PIT base oscillator frequency (Hz) */
#define SPEAKER_PIT_BASE_FREQ 1193182

/* I/O ports */
#define SPEAKER_PORT 0x61
#define PIT_CHANNEL2 0x42
#define PIT_COMMAND  0x43

/**
 * Programming PIT channel 2 to mode 3, the square wave, and connecting its
 * output to the speaker produces a tone whose pitch is the PIT's base
 * frequency divided by the loaded count. A frequency of 0 has no divisor,
 * so it silences the speaker instead. The count is a 16-bit register, so
 * the divisor of a request below 19 Hz, which exceeds 0xFFFF, is clamped to
 * 0xFFFF, the lowest tone the count can make.
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
}

void speaker_off(void)
{
    uint8_t tmp;

    /* Clear port 0x61 bits 0 (timer gate) and 1 (speaker enable). */
    tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);
}
