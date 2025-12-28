/*
 * audio.h - Common audio driver interface for Rach2
 *
 * Provides an abstraction layer for different audio hardware:
 * - Intel HD Audio (HDA) - modern PCI-based audio
 * - Sound Blaster 16 (SB16) - classic ISA audio
 * - PC Speaker - simple beep fallback
 *
 * Driver detection order at boot:
 * 1. Scan PCI for HDA (class 0x0403) -> use HDA driver
 * 2. Try SB16 reset at 0x220 -> if 0xAA response, use SB16
 * 3. Fall back to PC speaker beeps
 */

#ifndef _AUDIO_H_
#define _AUDIO_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * Audio sample formats
 */
typedef enum {
    AUDIO_U8,       /* Unsigned 8-bit (SB16 8-bit mode, Doom native) */
    AUDIO_S16LE     /* Signed 16-bit little-endian (SB16 16-bit, HDA) */
} audio_format_t;

/*
 * Audio driver type (for identification)
 */
typedef enum {
    AUDIO_DRIVER_NONE,
    AUDIO_DRIVER_SPEAKER,   /* PC speaker (beeps only) */
    AUDIO_DRIVER_SB16,      /* Sound Blaster 16 */
    AUDIO_DRIVER_HDA        /* Intel HD Audio */
} audio_driver_type_t;

/*
 * Audio callback function type
 * Called by driver when it needs more samples to play
 *
 * buffer: pointer to buffer to fill with audio samples
 * size:   number of bytes to fill
 */
typedef void (*audio_callback_t)(uint8_t *buffer, size_t size);

/*
 * Audio driver interface
 * Each hardware driver implements these functions
 */
typedef struct audio_driver {
    const char *name;
    audio_driver_type_t type;

    /*
     * Initialize the driver hardware
     * Returns 0 on success, -1 on failure
     */
    int (*init)(void);

    /*
     * Shutdown the driver and release resources
     */
    void (*shutdown)(void);

    /*
     * Start audio playback
     * sample_rate: samples per second (e.g., 11025, 22050, 44100)
     * format: AUDIO_U8 or AUDIO_S16LE
     * channels: 1 (mono) or 2 (stereo)
     * Returns 0 on success, -1 on failure
     */
    int (*start)(uint32_t sample_rate, audio_format_t format, int channels);

    /*
     * Stop audio playback
     */
    void (*stop)(void);

    /*
     * Check if audio is currently playing
     */
    bool (*is_playing)(void);

    /*
     * Set the audio callback function
     * The callback is invoked when the driver needs more samples
     * Set to NULL to disable callback
     */
    void (*set_callback)(audio_callback_t fn);

    /*
     * Write samples directly to the audio buffer
     * Alternative to callback-based playback
     * Returns number of bytes written, or -1 on error
     */
    int (*write)(const uint8_t *buffer, size_t size);

    /*
     * Set master volume (0-100)
     * Not all drivers support this
     */
    void (*set_volume)(int volume);

} audio_driver_t;

/*
 * Current active audio driver
 * NULL if no audio hardware detected or initialized
 */
extern audio_driver_t *audio;

/*
 * Available audio drivers (defined in respective driver files)
 */
extern audio_driver_t sb16_driver;
extern audio_driver_t hda_driver;
extern audio_driver_t speaker_driver;

/*
 * Initialize audio subsystem
 * Detects available hardware and initializes the best driver
 * Called during kernel boot
 */
void init_audio(void);

/*
 * Shutdown audio subsystem
 * Called during kernel shutdown or when audio is no longer needed
 */
void shutdown_audio(void);

/*
 * Get the name of the current audio driver
 * Returns "None" if no driver is active
 */
const char *audio_get_driver_name(void);

/*
 * Check if real audio (not PC speaker) is available
 */
bool audio_has_real_output(void);

#endif /* _AUDIO_H_ */
