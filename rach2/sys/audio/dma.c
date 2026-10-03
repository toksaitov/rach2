/*
 * ISA DMA controller (8237A) driver for Sound Blaster 16 audio transfers.
 */

#include "../arch/x86/portio.h"
#include "../klog.h"
#include "dma.h"
#include "../kio.h"
#include "../serial.h"

/*
 * DMA controller 1 ports (channels 0-3, 8-bit)
 */
#define DMA1_ADDR_CH0     0x00 /* Channel 0 address */
#define DMA1_COUNT_CH0    0x01 /* Channel 0 count */
#define DMA1_ADDR_CH1     0x02 /* Channel 1 address */
#define DMA1_COUNT_CH1    0x03 /* Channel 1 count */
#define DMA1_ADDR_CH2     0x04 /* Channel 2 address */
#define DMA1_COUNT_CH2    0x05 /* Channel 2 count */
#define DMA1_ADDR_CH3     0x06 /* Channel 3 address */
#define DMA1_COUNT_CH3    0x07 /* Channel 3 count */
#define DMA1_STATUS       0x08 /* Status register (read) */
#define DMA1_COMMAND      0x08 /* Command register (write) */
#define DMA1_REQUEST      0x09 /* Request register */
#define DMA1_SINGLEMASK   0x0A /* Single mask register */
#define DMA1_MODE         0x0B /* Mode register */
#define DMA1_FLIPFLOP     0x0C /* Flip-flop reset */
#define DMA1_MASTER_RESET 0x0D /* Master reset (write) */
#define DMA1_TEMP         0x0D /* Temporary register (read) */
#define DMA1_MASK_RESET   0x0E /* Clear mask register */
#define DMA1_MULTIMASK    0x0F /* All mask register bits */

/*
 * DMA controller 2 ports (channels 4-7, 16-bit)
 */
#define DMA2_ADDR_CH4     0xC0 /* Channel 4 address */
#define DMA2_COUNT_CH4    0xC2 /* Channel 4 count */
#define DMA2_ADDR_CH5     0xC4 /* Channel 5 address */
#define DMA2_COUNT_CH5    0xC6 /* Channel 5 count */
#define DMA2_ADDR_CH6     0xC8 /* Channel 6 address */
#define DMA2_COUNT_CH6    0xCA /* Channel 6 count */
#define DMA2_ADDR_CH7     0xCC /* Channel 7 address */
#define DMA2_COUNT_CH7    0xCE /* Channel 7 count */
#define DMA2_STATUS       0xD0 /* Status register (read) */
#define DMA2_COMMAND      0xD0 /* Command register (write) */
#define DMA2_REQUEST      0xD2 /* Request register */
#define DMA2_SINGLEMASK   0xD4 /* Single mask register */
#define DMA2_MODE         0xD6 /* Mode register */
#define DMA2_FLIPFLOP     0xD8 /* Flip-flop reset */
#define DMA2_MASTER_RESET 0xDA /* Master reset (write) */
#define DMA2_TEMP         0xDA /* Temporary register (read) */
#define DMA2_MASK_RESET   0xDC /* Clear mask register */
#define DMA2_MULTIMASK    0xDE /* All mask register bits */

/*
 * Page registers for each DMA channel (bits 16-23 of physical address)
 */
#define DMA_PAGE_CH0 0x87
#define DMA_PAGE_CH1 0x83
#define DMA_PAGE_CH2 0x81
#define DMA_PAGE_CH3 0x82
#define DMA_PAGE_CH4 0x8F /* Unused - cascade */
#define DMA_PAGE_CH5 0x8B
#define DMA_PAGE_CH6 0x89
#define DMA_PAGE_CH7 0x8A

static const uint8_t dma_page_ports[] = {DMA_PAGE_CH0, DMA_PAGE_CH1, DMA_PAGE_CH2, DMA_PAGE_CH3,
                                         DMA_PAGE_CH4, DMA_PAGE_CH5, DMA_PAGE_CH6, DMA_PAGE_CH7};

/**
 * Return the address-register port for a DMA channel, or 0 if out of range.
 */
static uint16_t dma_get_addr_port(uint8_t channel)
{
    switch (channel) {
    case 0:
        return DMA1_ADDR_CH0;
    case 1:
        return DMA1_ADDR_CH1;
    case 2:
        return DMA1_ADDR_CH2;
    case 3:
        return DMA1_ADDR_CH3;
    case 4:
        return DMA2_ADDR_CH4;
    case 5:
        return DMA2_ADDR_CH5;
    case 6:
        return DMA2_ADDR_CH6;
    case 7:
        return DMA2_ADDR_CH7;
    default:
        return 0;
    }
}

/**
 * Return the count-register port for a DMA channel, or 0 if out of range.
 */
static uint16_t dma_get_count_port(uint8_t channel)
{
    switch (channel) {
    case 0:
        return DMA1_COUNT_CH0;
    case 1:
        return DMA1_COUNT_CH1;
    case 2:
        return DMA1_COUNT_CH2;
    case 3:
        return DMA1_COUNT_CH3;
    case 4:
        return DMA2_COUNT_CH4;
    case 5:
        return DMA2_COUNT_CH5;
    case 6:
        return DMA2_COUNT_CH6;
    case 7:
        return DMA2_COUNT_CH7;
    default:
        return 0;
    }
}

/**
 * Program the 8237A for a single transfer on `channel` and unmask it.
 *
 * `buffer` is a physical address and `length` a byte count; both must satisfy
 * dma_address_valid for the channel width. Channels 0-3 are the 8-bit
 * controller; channels 5-7 are the 16-bit controller, where the address and
 * count are expressed in words. Channel 4 is the cascade and is rejected.
 *
 * The 8237A loads its 16-bit address and count registers one byte at a time,
 * sequenced by an internal flip-flop that selects low then high byte. The
 * flip-flop is reset before each pair, and the channel is masked across the
 * reprogramming so the controller does not act on a half-written register.
 *
 * `mode` supplies the transfer-type and direction bits of the mode register;
 * this routine OR's in the low two channel-select bits.
 *
 * @return 0 on success, -1 for an invalid channel or address.
 */
int dma_start(uint8_t channel, uint32_t buffer, uint32_t length, uint8_t mode)
{
    uint16_t addr_port, count_port;
    uint16_t mask_port, mode_port, flipflop_port;
    uint32_t addr, count;
    uint8_t page;

    if (channel > 7 || channel == 4) {
        return -1;
    }

    if (!dma_address_valid(buffer, length, channel >= 4)) {
        klog(KLOG_ERR, "dma", "refused: address 0x%x, %d bytes", buffer, length);
        return -1;
    }

    if (channel < 4) {
        mask_port = DMA1_SINGLEMASK;
        mode_port = DMA1_MODE;
        flipflop_port = DMA1_FLIPFLOP;
        addr = buffer;
        count = length - 1; /* count register has N-1 */
    } else {
        mask_port = DMA2_SINGLEMASK;
        mode_port = DMA2_MODE;
        flipflop_port = DMA2_FLIPFLOP;
        /* 16-bit channels address and count in words */
        addr = buffer >> 1;
        count = (length >> 1) - 1;
    }

    addr_port = dma_get_addr_port(channel);
    count_port = dma_get_count_port(channel);
    page = (buffer >> 16) & 0xFF;

    /* single-mask register: bit 2 sets the mask, low two bits select channel */
    outb(mask_port, (channel & 3) | 0x04);

    outb(flipflop_port, 0);

    /* mode register: low two bits select channel, rest is caller's mode */
    outb(mode_port, (channel & 3) | mode);

    /* address: low byte then high byte, sequenced by the flip-flop */
    outb(addr_port, addr & 0xFF);
    outb(addr_port, (addr >> 8) & 0xFF);

    /* page register has bits 16-23 of the physical address */
    outb(dma_page_ports[channel], page);

    outb(flipflop_port, 0);

    /* count: low byte then high byte */
    outb(count_port, count & 0xFF);
    outb(count_port, (count >> 8) & 0xFF);

    /* clear the mask bit to enable the channel */
    outb(mask_port, channel & 3);

    return 0;
}

/**
 * Stop transfers on `channel` by setting its single-mask bit.
 */
void dma_stop(uint8_t channel)
{
    uint16_t mask_port;

    if (channel > 7)
        return;

    mask_port = (channel < 4) ? DMA1_SINGLEMASK : DMA2_SINGLEMASK;

    /* single-mask register: bit 2 sets the mask, low two bits select channel */
    outb(mask_port, (channel & 3) | 0x04);
}

/**
 * Report whether [address, address+length) is a legal ISA DMA buffer.
 *
 * ISA DMA reaches only the low 16MB. The controller increments the address
 * register but not the page register, so a transfer may not cross a page
 * boundary: 64KB for 8-bit channels, 128KB for 16-bit channels.
 *
 * An empty transfer is not legal: the count register holds one less than the
 * byte or word count, so a length of zero would program a full sweep. A
 * 16-bit channel counts words, so its address and its length are even.
 */
bool dma_address_valid(uint32_t address, uint32_t length, bool is_16bit)
{
    uint32_t end, boundary;

    if (length == 0) {
        return false;
    }
    if (address >= 0x1000000 || address + length > 0x1000000) {
        return false;
    }
    if (is_16bit && ((address & 1) != 0 || (length & 1) != 0)) {
        return false;
    }

    end = address + length - 1;
    if (is_16bit) {
        boundary = 0x20000;
    } else {
        boundary = 0x10000;
    }

    if ((address / boundary) != (end / boundary)) {
        return false;
    }

    return true;
}
