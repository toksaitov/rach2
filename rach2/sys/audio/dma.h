/*
 * ISA DMA controller (8237A) interface.
 *
 * The 8237A transfers data between I/O devices and memory without CPU
 * intervention; the Sound Blaster 16 needs it for audio. Two cascaded
 * controllers cover 8 channels: controller 1 has channels 0-3 (8-bit)
 * and controller 2 has channels 4-7 (16-bit, channel 4 is the cascade).
 * The Sound Blaster 16 uses channel 1 for 8-bit audio and channel 5 for
 * 16-bit audio.
 */

#ifndef _DMA_H_
#define _DMA_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* DMA transfer modes. */
#define DMA_MODE_DEMAND  0x00 /* Demand transfer mode */
#define DMA_MODE_SINGLE  0x40 /* Single transfer mode (most common) */
#define DMA_MODE_BLOCK   0x80 /* Block transfer mode */
#define DMA_MODE_CASCADE 0xC0 /* Cascade mode */

#define DMA_MODE_READ  0x08 /* Memory to device (read from memory) */
#define DMA_MODE_WRITE 0x04 /* Device to memory (write to memory) */

#define DMA_MODE_AUTOINIT 0x10 /* Auto-initialize after transfer */

/**
 * Program the 8237A for a transfer of `length` bytes at physical address
 * `buffer` on `channel`, and unmask the channel. `mode` is the transfer-type
 * and direction bits of the mode register (DMA_MODE_*); the channel-select
 * bits are added here. Channels 0-3 are the 8-bit controller and 5-7 the
 * 16-bit one; channel 4 is the cascade. Returns 0, or -1 for channel 4, a
 * channel above 7, or a buffer that fails dma_address_valid (logged).
 */
int dma_start(uint8_t channel, uint32_t buffer, uint32_t length, uint8_t mode);

/** Stop transfers on `channel` by setting its mask bit; channel 4 and channels
 * above 7 are ignored. */
void dma_stop(uint8_t channel);

/**
 * Whether `length` bytes at physical `address` are a buffer ISA DMA can move
 * on a channel of the given width: not empty, below 16 MiB, inside one page
 * (64 KiB for an 8-bit channel, 128 KiB for a 16-bit one), and for a 16-bit
 * channel at an even address with an even length.
 */
bool dma_address_valid(uint32_t address, uint32_t length, bool is_16bit);

#endif /* _DMA_H_ */
