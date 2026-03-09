/*
 * audio.c - Audio subsystem initialization and driver management
 *
 * Handles audio driver detection in order of preference:
 * 1. Intel HD Audio (PCI class 0x0403)
 * 2. Sound Blaster 16 (probe at 0x220)
 * 3. PC Speaker (fallback)
 */

#include "audio.h"
#include "sb16.h"
#include "hda.h"
#include "../serial.h"
#include "../pci.h"

/* Current active audio driver */
audio_driver_t *audio = NULL;
static const char *audio_boot_status = "Not initialized";

/*
 * PC Speaker fallback driver
 * Provides basic beep functionality when no real audio hardware is available
 */
extern void speaker_on(uint32_t frequency);
extern void speaker_off(void);

static bool speaker_playing = false;
static audio_callback_t speaker_callback = NULL;

static int speaker_init(void)
{
    return 0;
}

static void speaker_shutdown(void)
{
    speaker_off();
    speaker_playing = false;
}

static int speaker_start(uint32_t sample_rate, audio_format_t format, int channels)
{
    (void)sample_rate;
    (void)format;
    (void)channels;
    /* PC speaker can't play real samples, but we mark as "playing" */
    speaker_playing = true;
    return 0;
}

static void speaker_stop(void)
{
    speaker_off();
    speaker_playing = false;
}

static bool speaker_is_playing(void)
{
    return speaker_playing;
}

static void speaker_set_callback(audio_callback_t fn)
{
    speaker_callback = fn;
    (void)speaker_callback;  /* Not used for speaker */
}

static int speaker_write(const uint8_t *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    /* PC speaker can't play arbitrary samples */
    return -1;
}

static void speaker_set_volume(int volume)
{
    (void)volume;
    /* PC speaker has no volume control */
}

audio_driver_t speaker_driver = {
    .name = "PC Speaker",
    .type = AUDIO_DRIVER_SPEAKER,
    .init = speaker_init,
    .shutdown = speaker_shutdown,
    .start = speaker_start,
    .stop = speaker_stop,
    .is_playing = speaker_is_playing,
    .set_callback = speaker_set_callback,
    .write = speaker_write,
    .set_volume = speaker_set_volume
};

/*
 * Initialize audio subsystem
 * Detects and initializes the best available audio driver
 */
void init_audio(void)
{
    audio = NULL;
    audio_boot_status = "No audio";

    /* Try Intel HD Audio first (modern, PCI-based) */
    if (hda_driver.init && hda_driver.init() == 0) {
        audio = &hda_driver;
        audio_boot_status = hda_get_boot_status();
        return;
    }

    /* Try Sound Blaster 16 (classic ISA audio) */
    if (sb16_driver.init && sb16_driver.init() == 0) {
        audio = &sb16_driver;
        audio_boot_status = "Sound Blaster 16";
        return;
    }

    /* Fall back to PC Speaker */
    if (speaker_driver.init() == 0) {
        audio = &speaker_driver;
        audio_boot_status = "PC Speaker fallback";
        return;
    }

    /* No audio available */
    audio = NULL;
    audio_boot_status = "No audio";
}

/*
 * Shutdown audio subsystem
 */
void shutdown_audio(void)
{
    if (audio && audio->shutdown) {
        audio->shutdown();
    }
    audio = NULL;
}

/*
 * Get the name of the current audio driver
 */
const char *audio_get_driver_name(void)
{
    if (audio) {
        return audio->name;
    }
    return "None";
}

const char *audio_get_boot_status(void)
{
    return audio_boot_status;
}

/*
 * Check if real audio hardware is available (not just PC speaker)
 */
bool audio_has_real_output(void)
{
    if (!audio) return false;
    return (audio->type == AUDIO_DRIVER_SB16 || audio->type == AUDIO_DRIVER_HDA);
}
