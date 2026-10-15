/*
 * audio.c: the choice of the audio driver at boot, and the PC speaker's
 * driver, the fallback that cannot play samples.
 */

#include "audio.h"
#include "../platform.h"
#include "../speaker.h"
#include "hda.h"
#include "../serial.h"
#include "../pci.h"

audio_driver_t *audio = NULL;
static const char *audio_boot_status = "Not initialized";

static bool speaker_playing = false;
static audio_callback_t speaker_callback = NULL;

/** The speaker needs no setup, so its init always succeeds. */
static int speaker_init(void)
{
    return 0;
}

/** Silence the speaker and clear the playing state. */
static void speaker_shutdown(void)
{
    speaker_off();
    speaker_playing = false;
}

/** Mark the speaker as playing, which it cannot do with samples; the state
 * is for `is_playing` alone. */
static int speaker_start(uint32_t sample_rate, audio_format_t format, int channels)
{
    (void)sample_rate;
    (void)format;
    (void)channels;
    speaker_playing = true;
    return 0;
}

/** Silence the speaker and clear the playing state. */
static void speaker_stop(void)
{
    speaker_off();
    speaker_playing = false;
}

/** Whether speaker_start was called since the last stop. */
static bool speaker_is_playing(void)
{
    return speaker_playing;
}

/** Record `fn`, which the speaker never calls. */
static void speaker_set_callback(audio_callback_t fn)
{
    speaker_callback = fn;
    (void)speaker_callback;
}

/** Return -1 for any samples: the speaker cannot play them. */
static int speaker_write(const uint8_t *buffer, size_t size)
{
    (void)buffer;
    (void)size;
    return -1;
}

/** Ignore `volume`: the speaker has no volume control. */
static void speaker_set_volume(int volume)
{
    (void)volume;
}

/* The audio_driver_t fields, in the order audio.h declares them: name,
   type, init, shutdown, start, stop, is_playing, set_callback, write,
   set_volume. */
audio_driver_t speaker_driver = {
    "PC Speaker", AUDIO_DRIVER_SPEAKER, speaker_init,         speaker_shutdown, speaker_start,
    speaker_stop, speaker_is_playing,   speaker_set_callback, speaker_write,    speaker_set_volume};

void init_audio(void)
{
    audio = NULL;
    audio_boot_status = "No audio";

    if (hda_driver.init && hda_driver.init() == 0) {
        audio = &hda_driver;
        audio_boot_status = hda_get_boot_status();
        return;
    }

    {
        const char *name = "";
        struct audio_driver *own = platform_audio_driver(&name);

        if (own && own->init && own->init() == 0) {
            audio = own;
            audio_boot_status = name;
            return;
        }
    }

    if (speaker_driver.init() == 0) {
        audio = &speaker_driver;
        audio_boot_status = "PC Speaker fallback";
        return;
    }

    audio = NULL;
    audio_boot_status = "No audio";
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

bool audio_has_real_output(void)
{
    if (!audio)
        return false;
    return (audio->type == AUDIO_DRIVER_SB16 || audio->type == AUDIO_DRIVER_HDA);
}
