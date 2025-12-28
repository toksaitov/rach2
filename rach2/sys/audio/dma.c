/*
 * dma.c - ISA DMA controller (8237A) implementation
 *
 * Programs the 8237A DMA controller for Sound Blaster 16 audio transfers.
 */

#include "dma.h"
#include "../kio.h"
#include "../serial.h"

/*
 * DMA controller 1 ports (channels 0-3, 8-bit)
 */
#define DMA1_ADDR_CH0       0x00    /* Channel 0 address */
#define DMA1_COUNT_CH0      0x01    /* Channel 0 count */
#define DMA1_ADDR_CH1       0x02    /* Channel 1 address */
#define DMA1_COUNT_CH1      0x03    /* Channel 1 count */
#define DMA1_ADDR_CH2       0x04    /* Channel 2 address */
#define DMA1_COUNT_CH2      0x05    /* Channel 2 count */
#define DMA1_ADDR_CH3       0x06    /* Channel 3 address */
#define DMA1_COUNT_CH3      0x07    /* Channel 3 count */
#define DMA1_STATUS         0x08    /* Status register (read) */
#define DMA1_COMMAND        0x08    /* Command register (write) */
#define DMA1_REQUEST        0x09    /* Request register */
#define DMA1_SINGLEMASK     0x0A    /* Single mask register */
#define DMA1_MODE           0x0B    /* Mode register */
#define DMA1_FLIPFLOP       0x0C    /* Flip-flop reset */
#define DMA1_MASTER_RESET   0x0D    /* Master reset (write) */
#define DMA1_TEMP           0x0D    /* Temporary register (read) */
#define DMA1_MASK_RESET     0x0E    /* Clear mask register */
#define DMA1_MULTIMASK      0x0F    /* All mask register bits */

/*
 * DMA controller 2 ports (channels 4-7, 16-bit)
 */
#define DMA2_ADDR_CH4       0xC0    /* Channel 4 address */
#define DMA2_COUNT_CH4      0xC2    /* Channel 4 count */
#define DMA2_ADDR_CH5       0xC4    /* Channel 5 address */
#define DMA2_COUNT_CH5      0xC6    /* Channel 5 count */
#define DMA2_ADDR_CH6       0xC8    /* Channel 6 address */
#define DMA2_COUNT_CH6      0xCA    /* Channel 6 count */
#define DMA2_ADDR_CH7       0xCC    /* Channel 7 address */
#define DMA2_COUNT_CH7      0xCE    /* Channel 7 count */
#define DMA2_STATUS         0xD0    /* Status register (read) */
#define DMA2_COMMAND        0xD0    /* Command register (write) */
#define DMA2_REQUEST        0xD2    /* Request register */
#define DMA2_SINGLEMASK     0xD4    /* Single mask register */
#define DMA2_MODE           0xD6    /* Mode register */
#define DMA2_FLIPFLOP       0xD8    /* Flip-flop reset */
#define DMA2_MASTER_RESET   0xDA    /* Master reset (write) */
#define DMA2_TEMP           0xDA    /* Temporary register (read) */
#define DMA2_MASK_RESET     0xDC    /* Clear mask register */
#define DMA2_MULTIMASK      0xDE    /* All mask register bits */

/*
 * Page registers for each DMA channel (bits 16-23 of physical address)
 */
#define DMA_PAGE_CH0        0x87
#define DMA_PAGE_CH1        0x83
#define DMA_PAGE_CH2        0x81
#define DMA_PAGE_CH3        0x82
#define DMA_PAGE_CH4        0x8F    /* Unused - cascade */
#define DMA_PAGE_CH5        0x8B
#define DMA_PAGE_CH6        0x89
#define DMA_PAGE_CH7        0x8A

static const uint8_t dma_page_ports[] = {
    DMA_PAGE_CH0, DMA_PAGE_CH1, DMA_PAGE_CH2, DMA_PAGE_CH3,
    DMA_PAGE_CH4, DMA_PAGE_CH5, DMA_PAGE_CH6, DMA_PAGE_CH7
};

/*
 * Get address port for a channel
 */
static uint16_t dma_get_addr_port(uint8_t channel)
{
    switch (channel) {
        case 0: return DMA1_ADDR_CH0;
        case 1: return DMA1_ADDR_CH1;
        case 2: return DMA1_ADDR_CH2;
        case 3: return DMA1_ADDR_CH3;
        case 4: return DMA2_ADDR_CH4;
        case 5: return DMA2_ADDR_CH5;
        case 6: return DMA2_ADDR_CH6;
        case 7: return DMA2_ADDR_CH7;
        default: return 0;
    }
}

/*
 * Get count port for a channel
 */
static uint16_t dma_get_count_port(uint8_t channel)
{
    switch (channel) {
        case 0: return DMA1_COUNT_CH0;
        case 1: return DMA1_COUNT_CH1;
        case 2: return DMA1_COUNT_CH2;
        case 3: return DMA1_COUNT_CH3;
        case 4: return DMA2_COUNT_CH4;
        case 5: return DMA2_COUNT_CH5;
        case 6: return DMA2_COUNT_CH6;
        case 7: return DMA2_COUNT_CH7;
        default: return 0;
    }
}

/*
 * Start a DMA transfer
 */
int dma_start(uint8_t channel, uint32_t buffer, uint32_t length, uint8_t mode)
{
    uint16_t addr_port, count_port;
    uint16_t mask_port, mode_port, flipflop_port;
    uint32_t addr, count;
    uint8_t page;

    if (channel > 7 || channel == 4) {
        return -1;  /* Invalid channel */
    }

    /* Validate address for DMA */
    if (!dma_address_valid(buffer, length, channel >= 4)) {
        serial_printf("DMA: Invalid address 0x%x length %d\n", buffer, length);
        return -1;
    }

    /* Determine which controller */
    if (channel < 4) {
        /* Controller 1 (8-bit) */
        mask_port = DMA1_SINGLEMASK;
        mode_port = DMA1_MODE;
        flipflop_port = DMA1_FLIPFLOP;
        addr = buffer;
        count = length - 1;  /* Count is length - 1 */
    } else {
        /* Controller 2 (16-bit) */
        mask_port = DMA2_SINGLEMASK;
        mode_port = DMA2_MODE;
        flipflop_port = DMA2_FLIPFLOP;
        /* 16-bit channels use word addresses (divide by 2) */
        addr = buffer >> 1;
        count = (length >> 1) - 1;  /* Count in words - 1 */
    }

    addr_port = dma_get_addr_port(channel);
    count_port = dma_get_count_port(channel);
    page = (buffer >> 16) & 0xFF;

    /* Mask the channel (disable it) */
    outb(mask_port, (channel & 3) | 0x04);

    /* Clear flip-flop */
    outb(flipflop_port, 0);

    /* Set the mode (channel bits + mode flags) */
    outb(mode_port, (channel & 3) | mode);

    /* Set address (low byte, then high byte) */
    outb(addr_port, addr & 0xFF);
    outb(addr_port, (addr >> 8) & 0xFF);

    /* Set page register (bits 16-23 of physical address) */
    outb(dma_page_ports[channel], page);

    /* Clear flip-flop again for count */
    outb(flipflop_port, 0);

    /* Set count (low byte, then high byte) */
    outb(count_port, count & 0xFF);
    outb(count_port, (count >> 8) & 0xFF);

    /* Unmask the channel (enable it) */
    outb(mask_port, channel & 3);

    return 0;
}

/*
 * Stop a DMA transfer
 */
void dma_stop(uint8_t channel)
{
    uint16_t mask_port;

    if (channel > 7) return;

    mask_port = (channel < 4) ? DMA1_SINGLEMASK : DMA2_SINGLEMASK;

    /* Mask the channel (disable it) */
    outb(mask_port, (channel & 3) | 0x04);
}

/*
 * Get the remaining transfer count
 */
uint32_t dma_get_count(uint8_t channel)
{
    uint16_t count_port, flipflop_port;
    uint32_t count;

    if (channel > 7) return 0;

    count_port = dma_get_count_port(channel);
    flipflop_port = (channel < 4) ? DMA1_FLIPFLOP : DMA2_FLIPFLOP;

    /* Clear flip-flop */
    outb(flipflop_port, 0);

    /* Read count (low byte, then high byte) */
    count = inb(count_port);
    count |= (uint32_t)inb(count_port) << 8;

    /* Count is length - 1, so add 1 */
    count++;

    /* For 16-bit channels, count is in words */
    if (channel >= 4) {
        count <<= 1;
    }

    return count;
}

/*
 * Check if a physical address is valid for DMA
 */
bool dma_address_valid(uint32_t address, uint32_t length, bool is_16bit)
{
    uint32_t end, boundary;

    /* Must be below 16MB (ISA DMA limit) */
    if (address >= 0x1000000 || address + length > 0x1000000) {
        return false;
    }

    /* Check for boundary crossing */
    end = address + length - 1;
    if (is_16bit) {
        /* 16-bit transfers must not cross 128KB boundary */
        boundary = 0x20000;
    } else {
        /* 8-bit transfers must not cross 64KB boundary */
        boundary = 0x10000;
    }

    /* Check if start and end are in the same boundary block */
    if ((address / boundary) != (end / boundary)) {
        return false;
    }

    return true;
}
