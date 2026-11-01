/*
 * Sound Blaster 16 driver interface.
 *
 * The Sound Blaster 16 is a classic ISA sound card. It plays 8-bit and
 * 16-bit PCM audio, mono or stereo, at sample rates from 5000 to 44100 Hz.
 * The driver looks for it at the I/O bases 0x220, 0x240, 0x260 and 0x280,
 * and uses IRQ 5, DMA channel 1 for 8-bit and channel 5 for 16-bit
 * transfers.
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
 * For console playback a DMA period, each half of the double buffer, is
 * SB16_PERIOD_FRAMES frames for each 11025 Hz of the rate or part of it:
 * 11.6 ms at 11025, 22050 and
 * 44100 Hz, and at most 25.6 ms, at 5000 Hz. Its bytes are at most
 * SB16_PERIOD_MAX, 512 frames of 16-bit stereo at 44100 Hz, so the whole
 * buffer is one page and lies inside one ISA DMA page (dma_address_valid).
 * A stream asks for its own period, clamped to SB16_PERIOD_MAX bytes.
 */
#define SB16_PERIOD_FRAMES 128
#define SB16_PERIOD_MAX    2048

/**
 * Probe for an SB16 at I/O base `base`: reset its DSP and read its version.
 * True when a DSP of version 4 or later (an SB16) responded; for an older card
 * it logs the version and returns false. The driver's base port is `base`
 * afterwards either way.
 */
bool sb16_probe(uint16_t base);

/**
 * The frames a sample written now waits behind: the ring's and the double
 * buffer's from the DMA controller's position to the end of the half the
 * interrupt refilled last, which is the other half's end when the second half
 * plays and the buffer's end when the first does. 0 when nothing plays. Safe
 * from any context.
 */
uint32_t sb16_delay_frames(void);

#endif /* _SB16_H_ */
