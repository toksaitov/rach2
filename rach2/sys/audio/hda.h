/*
 * Intel High Definition Audio (HDA) driver interface.
 *
 * HDA is the modern PC audio controller, a PCI device of class 0x0403.
 * Its registers are reached through memory-mapped I/O. The driver sets up
 * the CORB and RIRB ring buffers, but sends codec verbs through the
 * Immediate Command Interface (ICI); sample data moves by DMA streaming.
 */

#ifndef _HDA_H_
#define _HDA_H_

#include "audio.h"
#include "../pci.h"

/*
 * HDA controller register offsets (from MMIO base)
 */
#define HDA_GCAP      0x00 /* Global Capabilities (16-bit) */
#define HDA_VMIN      0x02 /* Minor Version (8-bit) */
#define HDA_VMAJ      0x03 /* Major Version (8-bit) */
#define HDA_OUTPAY    0x04 /* Output Payload Capability (16-bit) */
#define HDA_INPAY     0x06 /* Input Payload Capability (16-bit) */
#define HDA_GCTL      0x08 /* Global Control (32-bit) */
#define HDA_WAKEEN    0x0C /* Wake Enable (16-bit) */
#define HDA_STATESTS  0x0E /* State Change Status (16-bit) */
#define HDA_GSTS      0x10 /* Global Status (16-bit) */
#define HDA_INTCTL    0x20 /* Interrupt Control (32-bit) */
#define HDA_INTSTS    0x24 /* Interrupt Status (32-bit) */
#define HDA_WALCLK    0x30 /* Wall Clock Counter (32-bit) */
#define HDA_SSYNC     0x38 /* Stream Synchronization (32-bit) */
#define HDA_CORBLBASE 0x40 /* CORB Lower Base Address (32-bit) */
#define HDA_CORBUBASE 0x44 /* CORB Upper Base Address (32-bit) */
#define HDA_CORBWP    0x48 /* CORB Write Pointer (16-bit) */
#define HDA_CORBRP    0x4A /* CORB Read Pointer (16-bit) */
#define HDA_CORBCTL   0x4C /* CORB Control (8-bit) */
#define HDA_CORBSTS   0x4D /* CORB Status (8-bit) */
#define HDA_CORBSIZE  0x4E /* CORB Size (8-bit) */
#define HDA_RIRBLBASE 0x50 /* RIRB Lower Base Address (32-bit) */
#define HDA_RIRBUBASE 0x54 /* RIRB Upper Base Address (32-bit) */
#define HDA_RIRBWP    0x58 /* RIRB Write Pointer (16-bit) */
#define HDA_RINTCNT   0x5A /* Response Interrupt Count (16-bit) */
#define HDA_RIRBCTL   0x5C /* RIRB Control (8-bit) */
#define HDA_RIRBSTS   0x5D /* RIRB Status (8-bit) */
#define HDA_RIRBSIZE  0x5E /* RIRB Size (8-bit) */
#define HDA_ICW       0x60 /* Immediate Command Write (32-bit) */
#define HDA_IRR       0x64 /* Immediate Response Read (32-bit) */
#define HDA_ICS       0x68 /* Immediate Command Status (16-bit) */
#define HDA_DPLBASE   0x70 /* DMA Position Lower Base (32-bit) */
#define HDA_DPUBASE   0x74 /* DMA Position Upper Base (32-bit) */

/* Immediate Command Status bits */
#define HDA_ICS_ICB 0x01 /* Immediate Command Busy */
#define HDA_ICS_IRV 0x02 /* Immediate Response Valid */

/* Stream descriptor registers (offset 0x80 + 0x20 * stream_index) */
#define HDA_SD_CTL      0x00 /* Stream Descriptor Control (24-bit) */
#define HDA_SD_STS      0x03 /* Stream Descriptor Status (8-bit) */
#define HDA_SD_LPIB     0x04 /* Link Position in Buffer (32-bit) */
#define HDA_SD_CBL      0x08 /* Cyclic Buffer Length (32-bit) */
#define HDA_SD_LVI      0x0C /* Last Valid Index (16-bit) */
#define HDA_SD_FIFOW    0x0E /* FIFO Watermark (16-bit) */
#define HDA_SD_FIFOSIZE 0x10 /* FIFO Size (16-bit) */
#define HDA_SD_FORMAT   0x12 /* Stream Format (16-bit) */
#define HDA_SD_BDPL     0x18 /* BDL Pointer Lower (32-bit) */
#define HDA_SD_BDPU     0x1C /* BDL Pointer Upper (32-bit) */

/*
 * Global Control Register bits
 */
#define HDA_GCTL_CRST   0x01  /* Controller Reset */
#define HDA_GCTL_FCNTRL 0x02  /* Flush Control */
#define HDA_GCTL_UNSOL  0x100 /* Accept Unsolicited Response Enable */

/*
 * Stream Descriptor Control bits
 */
#define HDA_SD_CTL_SRST 0x01 /* Stream Reset */
#define HDA_SD_CTL_RUN  0x02 /* Stream Run */
#define HDA_SD_CTL_IOCE 0x04 /* Interrupt on Completion Enable */
#define HDA_SD_CTL_FEIE 0x08 /* FIFO Error Interrupt Enable */
#define HDA_SD_CTL_DEIE 0x10 /* Descriptor Error Interrupt Enable */

/*
 * Stream Format bits
 */
#define HDA_FMT_CHAN_MASK 0x000F /* Number of channels - 1 */
#define HDA_FMT_BITS_8    0x0000 /* 8-bit samples */
#define HDA_FMT_BITS_16   0x0010 /* 16-bit samples */
#define HDA_FMT_BITS_20   0x0020 /* 20-bit samples */
#define HDA_FMT_BITS_24   0x0030 /* 24-bit samples */
#define HDA_FMT_BITS_32   0x0040 /* 32-bit samples */
#define HDA_FMT_DIV_MASK  0x0700 /* Base rate divisor */
#define HDA_FMT_MULT_MASK 0x3800 /* Base rate multiplier */
#define HDA_FMT_BASE_48K  0x0000 /* 48 kHz base rate */
#define HDA_FMT_BASE_44K  0x4000 /* 44.1 kHz base rate */

/*
 * Codec verbs (commands)
 */
#define HDA_VERB_GET_PARAM      0xF0000 /* Get Parameter */
#define HDA_VERB_GET_CONN_LIST  0xF0200 /* Get Connection List Entry */
#define HDA_VERB_GET_CONN_SEL   0xF0100 /* Get Connection Select */
#define HDA_VERB_GET_CONFIG_DEF 0xF1C00 /* Get Configuration Default */
#define HDA_VERB_GET_PIN_SENSE  0xF0900 /* Get Pin Sense */
#define HDA_VERB_SET_CONN_SEL   0x70100 /* Set Connection Select */
#define HDA_VERB_SET_CHANNEL    0x70600 /* Set Converter Stream/Channel (706h) */
#define HDA_VERB_SET_AMP_GAIN   0x30000 /* Set Amplifier Gain/Mute */
#define HDA_VERB_SET_PIN_CTRL   0x70700 /* Set PIN Widget Control (707h) */
#define HDA_VERB_SET_EAPD       0x70C00 /* Set EAPD/BTL Enable */
#define HDA_VERB_SET_POWER      0x70500 /* Set Power State */
#define HDA_VERB_SET_CONV_FMT   0x20000 /* Set Converter Format */
#define HDA_VERB_SET_GPIO_MASK  0x71600 /* Set GPIO Enable Mask */
#define HDA_VERB_SET_GPIO_DIR   0x71700 /* Set GPIO Direction */
#define HDA_VERB_SET_GPIO_DATA  0x71500 /* Set GPIO Data */

/*
 * Codec parameters (for GET_PARAM)
 */
#define HDA_PARAM_VENDOR_ID   0x00
#define HDA_PARAM_REVISION_ID 0x02
#define HDA_PARAM_NODE_COUNT  0x04
#define HDA_PARAM_FUNC_GROUP  0x05
#define HDA_PARAM_AUDIO_CAPS  0x09
#define HDA_PARAM_PIN_CAPS    0x0C
#define HDA_PARAM_CONN_LIST   0x0E
#define HDA_PARAM_OUTPUT_AMP  0x12
#define HDA_PARAM_GPIO_COUNT  0x11 /* GPIO count and capabilities */

/*
 * Pin configuration default field extraction
 * Format: [31:30] Port Connectivity, [29:28] Location (gross),
 *         [27:24] Location (fine), [23:20] Default Device,
 *         [19:16] Connection Type, [15:12] Color,
 *         [11:8] Misc, [7:4] Default Association, [3:0] Sequence
 */
#define HDA_PIN_DEF_DEV(x)  (((x) >> 20) & 0xF)
#define HDA_PIN_DEF_LOC(x)  (((x) >> 28) & 0x3)
#define HDA_PIN_DEF_PORT(x) (((x) >> 30) & 0x3)

/* Default device types */
#define HDA_DEF_DEV_LINE_OUT   0x0
#define HDA_DEF_DEV_SPEAKER    0x1
#define HDA_DEF_DEV_HP_OUT     0x2
#define HDA_DEF_DEV_CD         0x3
#define HDA_DEF_DEV_SPDIF_OUT  0x4
#define HDA_DEF_DEV_DIG_OUT    0x5
#define HDA_DEF_DEV_MODEM_LINE 0x6
#define HDA_DEF_DEV_MODEM_HAND 0x7
#define HDA_DEF_DEV_LINE_IN    0x8
#define HDA_DEF_DEV_AUX        0x9
#define HDA_DEF_DEV_MIC_IN     0xA
#define HDA_DEF_DEV_TELEPHONY  0xB
#define HDA_DEF_DEV_SPDIF_IN   0xC
#define HDA_DEF_DEV_DIG_IN     0xD
#define HDA_DEF_DEV_OTHER      0xF

/* Port connectivity */
#define HDA_PORT_CONN_JACK  0x0
#define HDA_PORT_CONN_NONE  0x1
#define HDA_PORT_CONN_FIXED 0x2
#define HDA_PORT_CONN_BOTH  0x3

/*
 * Widget types
 */
#define HDA_WIDGET_AUDIO_OUT 0x0
#define HDA_WIDGET_AUDIO_IN  0x1
#define HDA_WIDGET_AUDIO_MIX 0x2
#define HDA_WIDGET_AUDIO_SEL 0x3
#define HDA_WIDGET_PIN       0x4
#define HDA_WIDGET_POWER     0x5
#define HDA_WIDGET_VOLUME    0x6
#define HDA_WIDGET_BEEP      0x7

/*
 * Buffer size for CORB/RIRB (256 entries each)
 */
#define HDA_CORB_SIZE 256
#define HDA_RIRB_SIZE 256

/*
 * Buffer Descriptor List entry
 */
typedef struct {
    uint32_t addr_low;  /* Buffer address (lower 32 bits) */
    uint32_t addr_high; /* Buffer address (upper 32 bits) */
    uint32_t length;    /* Buffer length in bytes */
    uint32_t ioc;       /* Interrupt on completion (bit 0) */
} __attribute__((packed)) hda_bdl_entry_t;

/*
 * Maximum number of BDL entries
 */
#define HDA_BDL_MAX_ENTRIES 256

/* Intel HD Audio driver. */
extern audio_driver_t hda_driver;

/** Periodic service hook; call from the timer tick. */
void hda_tick(void);

/* Debug helpers; safe to call even if HDA is not active. */
void hda_debug_dump(void);
void hda_debug_dump_codec(void);
void hda_debug_dump_pins(void);
void hda_debug_dump_stats(void);

/* Routing override (kernel debug). */
void hda_set_output_mode(int mode); /* 0=auto 1=primary 2=hp 3=both */
int hda_get_output_mode(void);

/* Short boot-time driver summary. */
const char *hda_get_boot_status(void);

typedef struct {
    uint32_t lpib;
    uint32_t cbl;
    uint32_t ctl;
    uint8_t sts;
    uint16_t fmt;
} hda_stream_debug_snapshot_t;

/* Lightweight stream diagnostics for kernel commands. */
bool hda_debug_get_stream_snapshot(hda_stream_debug_snapshot_t *out);

#endif /* _HDA_H_ */
