/*
 * Sound Blaster 16 driver interface.
 *
 * The Sound Blaster 16 is a classic ISA sound card. It plays 8-bit and
 * 16-bit PCM audio, mono or stereo, at sample rates from 5000 to 44100 Hz.
 * The card sits at base I/O port 0x220 on IRQ 5, with 8-bit transfers on
 * DMA channel 1 and 16-bit transfers on DMA channel 5.
 */

#ifndef _SB16_H_
#define _SB16_H_

#include "audio.h"

/*
 * SB16 I/O port offsets (relative to base address)
 */
#define SB16_MIXER_ADDR    0x04 /* Mixer address port */
#define SB16_MIXER_DATA    0x05 /* Mixer data port */
#define SB16_RESET         0x06 /* DSP reset */
#define SB16_READ_DATA     0x0A /* DSP read data */
#define SB16_WRITE_DATA    0x0C /* DSP write data/command */
#define SB16_WRITE_STATUS  0x0C /* DSP write buffer status (bit 7) */
#define SB16_READ_STATUS   0x0E /* DSP data available status (bit 7) */
#define SB16_INT_ACK_8BIT  0x0E /* 8-bit interrupt acknowledge */
#define SB16_INT_ACK_16BIT 0x0F /* 16-bit interrupt acknowledge */

/*
 * DSP commands
 */
#define DSP_SET_TIME_CONST  0x40 /* Set time constant (for older SB) */
#define DSP_SET_SAMPLE_RATE 0x41 /* Set output sample rate */
#define DSP_SPEAKER_ON      0xD1 /* Turn speaker on */
#define DSP_SPEAKER_OFF     0xD3 /* Turn speaker off */
#define DSP_STOP_8BIT       0xD0 /* Pause 8-bit DMA */
#define DSP_CONTINUE_8BIT   0xD4 /* Continue 8-bit DMA */
#define DSP_STOP_16BIT      0xD5 /* Pause 16-bit DMA */
#define DSP_CONTINUE_16BIT  0xD6 /* Continue 16-bit DMA */
#define DSP_EXIT_8BIT       0xDA /* Exit 8-bit auto-init DMA */
#define DSP_EXIT_16BIT      0xD9 /* Exit 16-bit auto-init DMA */
#define DSP_GET_VERSION     0xE1 /* Get DSP version */

/* DMA mode commands (8-bit, DSP 4.xx) */
#define DSP_8BIT_OUT_AI 0xC6 /* 8-bit output with auto-init */
#define DSP_8BIT_OUT_SC 0xC0 /* 8-bit output single cycle */

/* DMA mode commands (16-bit, DSP 4.xx) */
#define DSP_16BIT_OUT_AI 0xB6 /* 16-bit output with auto-init */
#define DSP_16BIT_OUT_SC 0xB0 /* 16-bit output single cycle */

/* Legacy DMA commands (more compatible) */
#define DSP_DMA_8BIT_OUT   0x14 /* 8-bit single-cycle DMA output */
#define DSP_DMA_8BIT_AI    0x1C /* 8-bit auto-init DMA output */
#define DSP_SET_BLOCK_SIZE 0x48 /* Set DMA block size */

/*
 * Mixer registers
 */
#define MIXER_RESET      0x00 /* Reset mixer */
#define MIXER_MASTER_VOL 0x22 /* Master volume (L/R) */
#define MIXER_VOICE_VOL  0x04 /* Voice/DAC volume */
#define MIXER_IRQ_SELECT 0x80 /* IRQ select */
#define MIXER_DMA_SELECT 0x81 /* DMA select */
#define MIXER_IRQ_STATUS 0x82 /* IRQ status */

/*
 * DMA transfer mode flags
 */
#define SB16_MODE_SIGNED 0x10 /* Signed samples */
#define SB16_MODE_STEREO 0x20 /* Stereo */

/*
 * Buffer size for double-buffering
 * Must be under 64KB and aligned for DMA
 * 160 samples @ 11025Hz = ~14.5ms per buffer
 */
#define SB16_BUFFER_SIZE 160


/** Probe for an SB16 at base port `base`; true if detected. */
bool sb16_probe(uint16_t base);

/** Return the DSP version as (major << 8) | minor. */
uint16_t sb16_get_version(void);

#endif /* _SB16_H_ */
