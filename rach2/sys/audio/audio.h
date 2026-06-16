/*
 * audio.h - common audio driver interface for Rach2.
 *
 * An abstraction layer over three audio backends: Intel HD Audio (HDA, a
 * modern PCI device), Sound Blaster 16 (SB16, classic ISA), and the PC
 * speaker, a beep-only fallback.
 *
 * Detection order at boot: scan PCI for HDA (class 0x0403); else reset
 * SB16 at 0x220 and accept it on a 0xAA response; else fall back to the
 * PC speaker.
 */

#ifndef _AUDIO_H_
#define _AUDIO_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/* Audio sample formats. */
typedef enum {
    AUDIO_U8,       /* unsigned 8-bit (SB16 8-bit mode, Doom native) */
    AUDIO_S16LE     /* signed 16-bit little-endian (SB16 16-bit, HDA) */
} audio_format_t;

/* Audio driver identity. */
typedef enum {
    AUDIO_DRIVER_NONE,
    AUDIO_DRIVER_SPEAKER,   /* PC speaker (beeps only) */
    AUDIO_DRIVER_SB16,      /* Sound Blaster 16 */
    AUDIO_DRIVER_HDA        /* Intel HD Audio */
} audio_driver_type_t;

/*
 * Pull callback: the driver calls it when it needs more samples to play.
 * `buffer` receives `size` bytes of audio samples to fill.
 */
typedef void (*audio_callback_t)(uint8_t *buffer, size_t size);

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
     * Write samples straight to the audio buffer, an alternative to
     * callback-driven playback. Returns the byte count written, -1 on error.
     */
    int (*write)(const uint8_t *buffer, size_t size);

    /* Set master volume, 0 to 100; not every driver honors it. */
    void (*set_volume)(int volume);

} audio_driver_t;

/* Active audio driver, NULL when no hardware was detected. */
extern audio_driver_t *audio;

/* The available drivers, each defined in its own driver file. */
extern audio_driver_t sb16_driver;
extern audio_driver_t hda_driver;
extern audio_driver_t speaker_driver;

/** Detect and initialize the best available audio driver. */
void init_audio(void);

/* Shut the active driver down and clear `audio`. */
void shutdown_audio(void);

/* Name of the active driver, or "None" when none is active. */
const char *audio_get_driver_name(void);

/* Short boot-time audio probe summary. */
const char *audio_get_boot_status(void);

/** Report whether real audio (not the PC speaker) is available. */
bool audio_has_real_output(void);

#endif /* _AUDIO_H_ */
