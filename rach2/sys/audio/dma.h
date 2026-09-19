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

/** Program a single DMA transfer on a channel; 0 ok, -1 on error. */
int dma_start(uint8_t channel, uint32_t buffer, uint32_t length, uint8_t mode);

/** Stop transfers on a channel by masking it. */
void dma_stop(uint8_t channel);

/** Report whether a buffer is a legal ISA DMA region for the channel width. */
bool dma_address_valid(uint32_t address, uint32_t length, bool is_16bit);

#endif /* _DMA_H_ */
