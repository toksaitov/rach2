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
#define HDA_VERB_SET_PIN_SENSE  0x70900 /* Execute Pin Sense */
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
#define HDA_VERB_SET_BEEP       0x70A00 /* Set Beep Generation: the divider */

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
#define HDA_PARAM_INPUT_AMP   0x0D
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

/* The most entries a buffer descriptor list may have, a hardware limit. */
#define HDA_BDL_MAX_ENTRIES 256

/* The connection selector has eight bits; list length has seven. */
#define HDA_MAX_CONNECTIONS     256
#define HDA_MAX_RAW_CONNECTIONS 127

/* A widget's connection list as the codec gave it and as expanded. */
typedef struct {
    uint8_t nid;
    uint8_t long_form;                     /* 1 when the entries are 16 bits, 0 for 8 */
    int raw_count;                         /* entries read into `raw` */
    int count;                             /* nodes in `nodes`; -1 for a malformed list */
    uint16_t raw[HDA_MAX_RAW_CONNECTIONS]; /* the entries, ranges unexpanded */
    uint16_t nodes[HDA_MAX_CONNECTIONS];   /* the node IDs, ranges expanded */
} hda_connection_report_t;

/* A converter's format capabilities, a DAC's or an ADC's. */
typedef struct {
    uint8_t nid;
    uint8_t source;   /* the node read: the converter under Format Override, else the AFG */
    uint32_t pcm;     /* its PCM sizes and rates parameter (0x0A) */
    uint32_t formats; /* its stream formats parameter (0x0B) */
} hda_caps_report_t;

/* The most connections a widget's entry in the report keeps. */
#define HDA_WIDGET_REPORT_CONNS 32

/* The most widgets a function group has: its node IDs are seven bits, and
 * node 0 is the root. */
#define HDA_MAX_WIDGETS 127

/* One widget of the function group, read at discovery. The amplifier
 * capabilities are the widget's own under Amp Param Override and the
 * function group's otherwise, and 0 where it has no such amplifier. */
typedef struct {
    uint8_t nid;
    uint8_t type;                            /* HDA_WIDGET_*: bits 23:20 of `caps` */
    bool sensed;                             /* `sense` was read, and its verb
                                                answered, at the last start */
    uint32_t caps;                           /* audio widget capabilities, parameter 0x09 */
    uint32_t amp_in;                         /* input amplifier capabilities, parameter 0x0D */
    uint32_t amp_out;                        /* output amplifier capabilities, parameter 0x12 */
    uint32_t pin_caps;                       /* pin capabilities, parameter 0x0C; 0 off a pin */
    uint32_t config;                         /* configuration default; 0 off a pin */
    uint32_t sense;                          /* Get Pin Sense's word at the last start, for a pin
                                                with presence detect; bit 31 is presence */
    int conn_count;                          /* nodes in `conns`; -1 for a malformed list or one
                                                longer than HDA_WIDGET_REPORT_CONNS */
    uint16_t conns[HDA_WIDGET_REPORT_CONNS]; /* expanded, in selector order */
} hda_widget_report_t;

/* The output stream's queue, read live by hda_read_queue with no verb. The
 * byte counts are of the ring, in the format the stream was started with;
 * all but `lpib` start again at each start. */
typedef struct {
    uint32_t fill;      /* bytes queued in the write ring */
    uint32_t high;      /* the most bytes the ring has held */
    uint32_t delay_ms;  /* from a write now to its sound: the ring at the
                           requested rate and the DMA buffer ahead of the
                           position at the programmed one */
    uint32_t written;   /* bytes the ring took */
    uint32_t dropped;   /* bytes the ring refused, being full */
    uint32_t periods;   /* DMA periods filled */
    uint32_t underruns; /* periods the ring could not fill whole */
    uint32_t missed;    /* whole revolutions of the DMA buffer that passed
                           between two services */
    uint32_t lpib;      /* the position in the DMA buffer, in bytes */
} hda_queue_report_t;

/* Service diagnostics since driver initialization, across stream starts. */
typedef struct {
    uint32_t calls;         /* khda service calls, including a wake with no refill */
    uint64_t usec;          /* cycles around khda's services, converted to us */
    uint64_t max_usec;      /* longest such call, including nested interrupts */
    uint32_t missed;        /* playback revolutions missed across all sessions */
    uint64_t clock_us;      /* TSC of the last miss in us, not uptime */
    uint32_t gap_us;        /* WALCLK span from the preceding service, or the stream's start */
    uint32_t asked_us;      /* WALCLK span from its first pending PLAY request */
    uint32_t asked_valid;   /* the request came from this playback session */
    uint32_t moved_us;      /* position's modular movement, converted to us */
    uint32_t revolution_us; /* duration of the DMA buffer at that miss */
} hda_service_report_t;

/*
 * The input path enumeration chose and the input stream's last setup and
 * counters. The node IDs are 0 where there is none. The ADC reads the pin
 * through its connection `index`, which is the pin itself or, when `mid` is
 * not 0, a mixer or selector that reads the pin through `mid_index`. The
 * counters start again at each start; the service updates them as it runs.
 */
typedef struct {
    uint8_t adc, pin, index, mid, mid_index;
    uint8_t int_mic;         /* the internal microphone's pin */
    uint8_t jack_mic;        /* the microphone jack's pin */
    bool inv_dmic;           /* the internal mic's right channel is its left
                                inverted: it is muted, and the left recorded
                                in both channels */
    bool running;            /* the input stream runs */
    uint8_t level;           /* the ADC's input gain, in steps: the capture level */
    hda_caps_report_t caps;  /* the ADC's */
    uint32_t requested_rate; /* the rate the last setup asked for */
    uint32_t rate;           /* the rate it programmed, one the ADC has */
    uint16_t format;         /* the input stream's format word, read back */
    uint16_t pin_ctl;        /* the pin widget control the last start set */
    uint32_t period;         /* bytes of one DMA period */
    uint32_t periods;        /* periods in the DMA buffer */
    uint32_t frames;         /* frames handed to the reader */
    uint32_t overruns;       /* services that found a period overwritten
                                before it was handed over */
    uint32_t lpib;           /* the position in the DMA buffer, in bytes, at
                                the last service */
} hda_input_report_t;

/* A jack's state: a plug in it, none, or not known yet, since no reading of
 * its sense has succeeded. */
#define HDA_JACK_UNKNOWN (-1)
#define HDA_JACK_OUT     0
#define HDA_JACK_IN      1

/* The jacks the driver watches, by index in the report's `jacks`. */
#define HDA_JACK_HP  0 /* the headphone pin */
#define HDA_JACK_MIC 1 /* the microphone jack's pin */
#define HDA_JACKS    2

/*
 * A jack the tick polls (hda_tick): a pin in a jack with presence detect.
 * `state` changes when a reading that differs from it is seen twice in a
 * row, or by one reading at a start. A reading that fails changes nothing.
 */
typedef struct {
    uint8_t pin;      /* its pin; 0 when there is none to watch */
    int8_t state;     /* HDA_JACK_IN, HDA_JACK_OUT or HDA_JACK_UNKNOWN */
    uint32_t polls;   /* readings of its sense, from the tick and at starts */
    uint32_t failed;  /* readings whose verbs failed or found the
                         interface busy */
    uint32_t changes; /* changes of `state` */
} hda_jack_report_t;

/*
 * What enumeration found and the last stream setup, for /proc/audio. The node
 * IDs are 0 where there is none; the indices are the connections chosen on
 * the pin, the mixer and the headphone pin. The formats are read back from
 * the stream descriptor and the two DACs. caps[] is the DAC and the headphone
 * DAC; connections[] the pin, the mixer and the headphone pin. widgets[] has
 * every widget of the function group, in node order, read at discovery,
 * `queue` the stream's counters as hda_read_queue last read them, `input`
 * the input path and stream, and `jacks` the headphone and microphone jacks.
 * primary_enabled and hp_enabled are the output pins as the last start or
 * jack change left them, and the `beep` fields the beep generator's
 * (`hda_driver.beep`).
 */
typedef struct {
    uint32_t vendor;
    uint8_t codec, afg, pin, dac, mixer, hp_pin, hp_dac;
    uint8_t pin_index, mixer_index, hp_index;
    bool ready, playing;
    uint32_t requested_rate, programmed_rate;
    uint16_t stream_format, dac_format, hp_format;
    uint8_t primary_enabled, hp_enabled;
    hda_caps_report_t caps[2];
    hda_connection_report_t connections[3];
    uint32_t gpio; /* the GPIO count parameter (0x11); bits 7:0 the count */
    int widget_count;
    hda_widget_report_t widgets[HDA_MAX_WIDGETS];
    hda_queue_report_t queue;
    hda_service_report_t service;
    hda_input_report_t input;
    hda_jack_report_t jacks[HDA_JACKS];
    uint8_t beep;         /* the beep widget, or 0 */
    uint8_t beep_divider; /* the generator's divider as last sent; 0 silent */
    uint8_t beep_path;    /* a beep turned the output pins on with no stream */
    uint32_t beeps;       /* tones the generator began */
} hda_report_t;

/** Return cached enumeration and the last stream setup; sends no verbs. */
const hda_report_t *hda_get_report(void);

/** Read the output stream's counters and position into the report's
 * `queue`. It reads the position from memory or a register and sends no
 * verb, so it is safe from any context. */
void hda_read_queue(void);

/**
 * The frames a sample written now waits behind, at the rate the stream was
 * started with: the write ring's and the DMA buffer's ahead of the position,
 * the latter converted from the programmed rate. At most three periods of
 * about 23 ms in the DMA buffer and, on the write path, three in the ring; 0
 * when no stream plays. Safe from any context; it sends no verb.
 */
uint32_t hda_delay_frames(void);

/** Ask for the refill of the periods the stream has played since the last
 * refill and the handing over of the input stream's finished periods, for
 * each stream only until its first interrupt or while its next interrupt is
 * late, and every quarter second poll the jacks, moving the output and the
 * microphone recorded as a plug goes in or out. The boot processor's tick
 * calls this, with interrupts off, so playback goes on where the
 * controller's interrupt does not arrive, while a stream whose interrupts
 * come is asked for by them alone, as Linux's `azx_interrupt` acknowledges
 * the stream and wakes its writer. The service runs in the driver's thread,
 * which the tick wakes when the stream's position has passed into another
 * period, and here while the thread does not run it. The jacks' and the
 * beep's clocks count the ticks since the last call, since that tick skips
 * the call while another processor holds the big lock. */
void hda_tick(void);

/** Make the driver's thread, khda, which refills the output stream and
 * hands the input stream's periods over in process context, under
 * SCHED_FIFO at priority 50, once the first program has its pid, so that
 * init is process 1; nothing when no controller is set up or the thread is
 * made. Until then, and while the scheduler is off, the interrupt and the
 * tick run the service themselves. Process context, with the big lock. */
void hda_thread_start(void);

/*
 * The dumps of the `audio hda` shell command. Each prints on the console (the
 * screen and the serial port), and says so when the controller is not set up.
 */

/** Print the controller's PCI configuration, its registers, the codec path
 * and the output stream's state. */
void hda_debug_dump(void);

/** Print the DAC, the mixer and every pin widget of the codec, with their
 * capabilities, configuration and connection lists. */
void hda_debug_dump_codec(void);

/** Print the output and headphone pins' control, EAPD, jack sense and chosen
 * connection. */
void hda_debug_dump_pins(void);

/** Print the stream's DMA position, the playback counters, the underruns and
 * the session's throughput. */
void hda_debug_dump_stats(void);

/**
 * Set the output routing the next start uses: 0 automatic (the headphone pin
 * while its jack has a plug, else the primary pin, following the jack as it
 * changes), 1 the primary pin, 2 the headphone pin, 3 both. Another value is
 * ignored. Enumeration sets 0.
 */
void hda_set_output_mode(int mode);

/** The routing mode hda_set_output_mode set. */
int hda_get_output_mode(void);

/** The boot's summary: "Intel HD Audio, codec 0x" and the codec's vendor ID,
 * or "Intel HD Audio" when no codec was found. */
const char *hda_get_boot_status(void);

/* The output stream descriptor's registers at one moment. */
typedef struct {
    uint32_t lpib;
    uint32_t cbl;
    uint32_t ctl;
    uint8_t sts;
    uint16_t fmt;
} hda_stream_debug_snapshot_t;

/** Read the output stream's registers into `*out`; false when the controller
 * is not mapped or `out` is NULL. */
bool hda_debug_get_stream_snapshot(hda_stream_debug_snapshot_t *out);

#endif /* _HDA_H_ */
