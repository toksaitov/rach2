/*
 * dma.h - ISA DMA controller (8237A) interface
 *
 * The 8237A DMA controller handles data transfers between I/O devices
 * and memory without CPU intervention. Required for Sound Blaster 16.
 *
 * Two cascaded controllers handle 8 channels:
 *   - Controller 1: Channels 0-3 (8-bit transfers)
 *   - Controller 2: Channels 4-7 (16-bit transfers, channel 4 is cascade)
 *
 * Sound Blaster 16 typically uses:
 *   - Channel 1 for 8-bit audio
 *   - Channel 5 for 16-bit audio
 */

#ifndef _DMA_H_
#define _DMA_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * DMA transfer modes
 */
#define DMA_MODE_DEMAND     0x00    /* Demand transfer mode */
#define DMA_MODE_SINGLE     0x40    /* Single transfer mode (most common) */
#define DMA_MODE_BLOCK      0x80    /* Block transfer mode */
#define DMA_MODE_CASCADE    0xC0    /* Cascade mode */

#define DMA_MODE_READ       0x08    /* Memory to device (read from memory) */
#define DMA_MODE_WRITE      0x04    /* Device to memory (write to memory) */

#define DMA_MODE_AUTOINIT   0x10    /* Auto-initialize after transfer */

/*
 * Start a DMA transfer
 *
 * channel: DMA channel (0-3 for 8-bit, 5-7 for 16-bit)
 * buffer:  Physical address of the DMA buffer
 *          IMPORTANT: Must be below 16MB and must not cross 64KB/128KB boundary
 * length:  Transfer length in bytes (for 16-bit channels, in words)
 * mode:    Transfer mode (use DMA_MODE_* constants)
 *
 * Returns 0 on success, -1 on error
 */
int dma_start(uint8_t channel, uint32_t buffer, uint32_t length, uint8_t mode);

/*
 * Stop a DMA transfer
 * Masks the channel to prevent further transfers
 */
void dma_stop(uint8_t channel);

/*
 * Get the remaining transfer count for a channel
 * Returns the number of bytes/words remaining
 */
uint32_t dma_get_count(uint8_t channel);

/*
 * Check if a physical address is valid for DMA
 * Must be below 16MB and not cross a 64KB boundary (8-bit) or 128KB (16-bit)
 */
bool dma_address_valid(uint32_t address, uint32_t length, bool is_16bit);

#endif /* _DMA_H_ */
