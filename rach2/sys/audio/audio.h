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
     * 1 (mono) or 2 (stereo). Returns 0 on success, -1 on failure.
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
     * full (the rest is dropped), or -1 when not playing or given no data.
     */
    int (*write)(const uint8_t *buffer, size_t size);

    /* Set master volume, 0 to 100; not every driver honors it. */
    void (*set_volume)(int volume);

} audio_driver_t;

/* The active audio driver; NULL until init_audio chooses one. */
extern audio_driver_t *audio;

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
