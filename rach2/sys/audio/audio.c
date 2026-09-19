/*
 * audio.c - audio subsystem initialization and driver management.
 *
 * A driver is selected by preference at boot: Intel HD Audio (PCI class
 * 0x0403), then Sound Blaster 16 (ISA, probed at 0x220), then the PC
 * speaker as a fallback that cannot play samples.
 */

#include "audio.h"
#include "sb16.h"
#include "hda.h"
#include "../serial.h"
#include "../pci.h"

audio_driver_t *audio = NULL;
static const char *audio_boot_status = "Not initialized";

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
    /* The PC speaker cannot play samples; mark playing only for state. */
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
    (void)speaker_callback;
}

static int speaker_write(const uint8_t *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    /* The PC speaker cannot play arbitrary samples. */
    return -1;
}

static void speaker_set_volume(int volume)
{
    (void)volume;
    /* The PC speaker has no volume control. */
}

/* The audio_driver_t fields, in the order audio.h declares them: name,
   type, init, shutdown, start, stop, is_playing, set_callback, write,
   set_volume. */
audio_driver_t speaker_driver = {
    "PC Speaker", AUDIO_DRIVER_SPEAKER, speaker_init,         speaker_shutdown, speaker_start,
    speaker_stop, speaker_is_playing,   speaker_set_callback, speaker_write,    speaker_set_volume};

/**
 * Detect and initialize the best available audio driver.
 *
 * Each candidate is probed in order of preference and the first whose
 * `init` succeeds becomes the active driver: HD Audio, then SB16, then
 * the PC speaker. `audio` is left NULL and the boot status reads "No
 * audio" if every probe fails.
 */
void init_audio(void)
{
    audio = NULL;
    audio_boot_status = "No audio";

    if (hda_driver.init && hda_driver.init() == 0) {
        audio = &hda_driver;
        audio_boot_status = hda_get_boot_status();
        return;
    }

    if (sb16_driver.init && sb16_driver.init() == 0) {
        audio = &sb16_driver;
        audio_boot_status = "Sound Blaster 16";
        return;
    }

    if (speaker_driver.init() == 0) {
        audio = &speaker_driver;
        audio_boot_status = "PC Speaker fallback";
        return;
    }

    audio = NULL;
    audio_boot_status = "No audio";
}

void shutdown_audio(void)
{
    if (audio && audio->shutdown) {
        audio->shutdown();
    }
    audio = NULL;
}

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

/**
 * Report whether real audio hardware backs the active driver.
 *
 * True only for the SB16 and HD Audio drivers; the PC speaker fallback,
 * which cannot play samples, counts as no real output.
 */
bool audio_has_real_output(void)
{
    if (!audio)
        return false;
    return (audio->type == AUDIO_DRIVER_SB16 || audio->type == AUDIO_DRIVER_HDA);
}
