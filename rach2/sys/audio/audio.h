/*
 * audio.h: the operations every audio driver provides, and the choice of the
 * driver at boot.
 *
 * There are three backends: Intel HD Audio (HDA, a PCI device of class
 * 0x0403), the Sound Blaster 16 (SB16, an ISA card whose DSP responds to a
 * reset with 0xAA) and the PC speaker, which plays no samples.
 */

#ifndef _AUDIO_H_
#define _AUDIO_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* Audio sample formats. */
typedef enum {
    AUDIO_U8,   /* unsigned 8-bit (SB16 8-bit mode, Doom native) */
    AUDIO_S16LE /* signed 16-bit little-endian (SB16 16-bit, HDA) */
} audio_format_t;

/* Audio driver identity. */
typedef enum {
    AUDIO_DRIVER_NONE,
    AUDIO_DRIVER_SPEAKER, /* PC speaker (beeps only) */
    AUDIO_DRIVER_SB16,    /* Sound Blaster 16 */
    AUDIO_DRIVER_HDA      /* Intel HD Audio */
} audio_driver_type_t;

/*
 * Pull callback: the driver calls it when it needs more samples to play, to
 * fill `size` bytes at `buffer`. It runs from the driver's interrupt handler
 * or the timer tick with interrupts off, and from `start` to fill the first
 * buffers.
 */
typedef void (*audio_callback_t)(void *buffer, size_t size);

/* The direction of a stream. */
typedef enum { AUDIO_PLAYBACK, AUDIO_CAPTURE } audio_dir_t;

/*
 * A stream: one direction of the device, owned by one caller, such as an open
 * of /dev/dsp, between `stream_open` and `stream_close`.
 *
 * The caller asks for the rate, format, channels and period, and the driver's
 * `stream_setup` writes back what it grants. A period is the unit the
 * hardware plays or records before it tells the caller, a whole number of
 * frames; the driver may make it smaller, never larger. `periods` is the
 * length of the caller's buffer, which the driver may raise. The driver
 * sets `device_periods`, the periods its DMA buffer holds, which
 * `stream_start` fills: a caller that starts the stream with fewer queued
 * plays silence after them.
 *
 * The buffers are two. The caller's, `periods` times `period_bytes` in the
 * stream's format, belongs to the caller; the DMA buffer the device reads or
 * writes belongs to the driver, which may convert between the two formats.
 * Data crosses between them only through `transfer`, which the driver calls
 * to fill (playback) or to empty (capture) `size` bytes of its own at `buf`,
 * always a whole number of frames in the stream's format. A playback
 * transfer fills all `size` bytes, with silence for what the caller lacks,
 * so the stream runs on while the caller is late. The driver calls `period`
 * each time the device finishes a period, after the transfers that period
 * made room for. Both run from the driver's interrupt handler or the timer
 * tick with interrupts off, and from `stream_start` to fill the first
 * periods; neither may sleep, and neither may call the driver back.
 */
typedef struct audio_stream {
    audio_dir_t dir;
    uint32_t rate;           /* frames per second */
    audio_format_t format;   /* AUDIO_U8 or AUDIO_S16LE */
    int channels;            /* 1 or 2 */
    uint32_t period_bytes;   /* bytes of one period, a whole number of frames */
    uint32_t periods;        /* periods in the caller's buffer, at least 2 */
    uint32_t device_periods; /* periods in the driver's buffer, at least 1 */
    void (*transfer)(struct audio_stream *s, uint8_t *buf, size_t size);
    void (*period)(struct audio_stream *s);
    void *owner; /* the caller's, for `transfer` and `period` */
} audio_stream_t;

/* The sources a device can record, for the mixer's record source. */
typedef enum {
    AUDIO_SRC_MIC,     /* the microphone jack: OSS's MIC */
    AUDIO_SRC_INT_MIC, /* the internal microphone: OSS's MONITOR, as FreeBSD
                          names it */
    AUDIO_SRC_LINE,    /* the line in: OSS's LINE */
    AUDIO_SOURCES
} audio_source_t;

/* The input amplifiers of one source. */
typedef enum {
    AUDIO_IN_GAIN, /* the converter's level on the source's connection */
    AUDIO_IN_BOOST /* the gain of the source's own pin, before the converter */
} audio_input_ctl_t;

/* Audio driver interface; each hardware backend supplies these ops. */
typedef struct audio_driver {
    const char *name;
    audio_driver_type_t type;

    /* Initialize the hardware; 0 on success, -1 on failure. */
    int (*init)(void);

    /* Shut the driver down and release its resources. */
    void (*shutdown)(void);

    /*
     * Start playback. `sample_rate` is samples per second (e.g. 11025,
     * 22050, 44100); `format` is AUDIO_U8 or AUDIO_S16LE; `channels` is
     * 1 (mono) or 2 (stereo). Returns 0 on success, -EINVAL for arguments
     * the driver refuses, or -EIO when the device fails to start. Each driver
     * checks only what its device needs: HDA all three, the SB16 the rate
     * alone, and the PC speaker nothing.
     */
    int (*start)(uint32_t sample_rate, audio_format_t format, int channels);

    /* Stop playback. */
    void (*stop)(void);

    /* Report whether audio is currently playing. */
    bool (*is_playing)(void);

    /* Set the pull callback; NULL disables callback-driven playback. */
    void (*set_callback)(audio_callback_t fn);

    /*
     * Queue samples for playback, an alternative to callback-driven
     * playback. Returns the bytes queued, fewer than `size` when the queue is
     * full (the rest is dropped), -EINVAL when not playing or given no data,
     * or -EOPNOTSUPP from a driver that cannot play samples.
     */
    int (*write)(const uint8_t *buffer, size_t size);

    /* Set master volume, 0 to 100; not every driver honors it. A driver
     * that does keeps the level and applies it again when a stream starts. */
    void (*set_volume)(int volume);

    /* The master volume last set, 0 to 100, 100 until one is; NULL for a
     * driver with no volume, the PC speaker. */
    int (*get_volume)(void);

    /*
     * The stream operations: all seven, or all NULL in a driver that has no
     * streams. Each runs in process context, and none may be called from
     * `transfer` or `period`.
     */

    /* Take the stream's direction for `s`, which stays the caller's until
     * `stream_close`: 0, -EBUSY while another stream or the write path has
     * it, or -ENODEV for a direction the device lacks. Nothing plays yet. */
    int (*stream_open)(audio_stream_t *s);

    /* Grant `s` the parameters nearest to those it asks for, writing them
     * back into it, while it is stopped: 0, or -EINVAL when the device can
     * play nothing near them, which leaves the driver as it was. */
    int (*stream_setup)(audio_stream_t *s);

    /* Start the device: it calls `transfer` for its first periods before
     * this returns, then once for each period it plays. 0, or -EIO. */
    int (*stream_start)(audio_stream_t *s);

    /* Stop the device at once; what its DMA buffer holds is dropped, and no
     * `transfer` or `period` call is made after this returns. */
    void (*stream_stop)(audio_stream_t *s);

    /* Give the direction back, stopping the stream first if it runs. */
    void (*stream_close)(audio_stream_t *s);

    /* The frames of the stream the device has played or recorded since
     * `stream_start`, silence among them, wrapping at 2^32; read from the
     * device's position, so it moves within a period. It may be called with
     * interrupts off, from a wait's condition. */
    uint32_t (*stream_position)(audio_stream_t *s);

    /* The frames `s` has given the driver that the device has not played
     * yet, in the stream's rate: OSS's GETODELAY less the caller's buffer.
     * With `s` NULL, the same for the `write` path's queue. */
    uint32_t (*delay)(audio_stream_t *s);

    /*
     * The input controls, which /dev/mixer reaches: all three, or all NULL
     * in a driver that records nothing. Each runs in process context and
     * sends the device what it changes at once, so that it takes effect
     * while a stream runs; the driver keeps it for the next start.
     */

    /* The sources the device can record, a mask of 1 << audio_source_t,
     * and in `*controls` those of audio_input_ctl_t it has an amplifier
     * for on any source, a mask of 1 << the control. */
    unsigned (*input_caps)(unsigned *controls);

    /* Record `source` from now on, and return the source recorded after:
     * `source`, or for one the device cannot record, or a negative one,
     * the source it recorded before. */
    int (*input_select)(int source);

    /* Set control `ctl` of `source` to `level`, 0 to 100, where 0 mutes an
     * amplifier that can mute; with `level` negative only read it. The
     * level the control has after, in percent of its steps, or -EINVAL for
     * a source or control the device lacks. */
    int (*input_level)(int ctl, int source, int level);
} audio_driver_t;

/*
 * The driver programs reach: the console's AUDIO_* ioctls and /dev/dsp call
 * through it. It is `audio_hw`, the driver of the device, unless
 * `audio_set_dispatch` has put another in front of it. NULL until init_audio
 * chooses a driver.
 */
extern audio_driver_t *audio;

/* The driver of the device itself, which init_audio chose; NULL when none. */
extern audio_driver_t *audio_hw;

/* The available drivers, each defined in its own driver file. */
extern audio_driver_t sb16_driver;
extern audio_driver_t hda_driver;
extern audio_driver_t speaker_driver;

/**
 * Choose the audio driver: the first whose `init` succeeds of HD Audio, the
 * platform's own device (on a PC the Sound Blaster 16) and the PC speaker.
 * `audio` is left NULL and the boot status reads "No audio" if every probe
 * fails.
 */
void init_audio(void);

/**
 * Put the driver `d` in front of the device's: from now on `audio` is `d`,
 * which reaches the device through `audio_hw` itself. NULL takes it away,
 * and `audio` is `audio_hw` again. Call it while no stream is open and no
 * program is in an audio call.
 */
void audio_set_dispatch(audio_driver_t *d);

/** The calling process identity in the kernel namespace, or 0 at boot.
 * Channels keep it at open, so an inherited open stays its opener's. */
uint32_t audio_process_id(void);

/** Name of the active driver, or "None" when none is active. */
const char *audio_get_driver_name(void);

/** The boot's summary of the chosen driver, for the log: HDA's with its
 * codec, the platform device's name, "PC Speaker fallback", or "No audio";
 * "Not initialized" before init_audio. */
const char *audio_get_boot_status(void);

/** Whether the active driver plays samples: true for the SB16 and HD Audio
 * drivers, false for the PC speaker and for none. */
bool audio_has_real_output(void);

#endif /* _AUDIO_H_ */
