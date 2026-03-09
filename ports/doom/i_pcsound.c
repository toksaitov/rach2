/*
 * i_pcsound.c - Sound module for Doom on Rach2
 *
 * Supports two modes:
 * 1. Real audio (SB16/HDA): Plays actual Doom sound samples + OPL3 music
 * 2. PC Speaker fallback: Maps sound effects to beep frequencies (no music)
 *
 * PC speaker can only play one tone at a time, so we map sfx to frequencies.
 * Real audio mode supports proper sample playback with mixing and OPL3 music.
 */

#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doomtype.h"
#include "i_oplmusic.h"

#include <stdint.h>

/*
 * Dummy variables required by i_sound.c when FEATURE_SOUND is defined.
 * These are used for libsamplerate integration which we don't support.
 */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* Syscall wrappers */
extern int ioctl(int fd, unsigned long request, ...);

/* ioctl request codes */
#define KIOCSOUND       0x4B2F  /* PC speaker frequency (0 = off) */
#define AUDIO_GET_INFO  0x4101  /* Get audio driver info */
#define AUDIO_START     0x4102  /* Start audio playback */
#define AUDIO_STOP      0x4103  /* Stop audio playback */
#define AUDIO_WRITE     0x4104  /* Write audio samples */
#define AUDIO_SET_VOLUME 0x4105 /* Set volume (0-100) */

/* Audio driver types */
#define AUDIO_DRIVER_NONE    0
#define AUDIO_DRIVER_SPEAKER 1
#define AUDIO_DRIVER_SB16    2
#define AUDIO_DRIVER_HDA     3

/* Audio formats */
#define AUDIO_U8    0
#define AUDIO_S16LE 1

/* Audio info structure */
struct audio_info {
    uint32_t driver_type;
    uint32_t has_real_audio;
};

/* Audio start parameters */
struct audio_start_params {
    uint32_t sample_rate;
    uint32_t format;
    uint32_t channels;
};

/* Audio write parameters */
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size;
};

/* Get current time in ms */
extern uint32_t DG_GetTicksMs(void);

/* Sound state */
static boolean pcsound_initialized = false;
static int current_sound_channel = -1;
static uint32_t current_freq = 0;

/* Real audio state */
static boolean use_real_audio = false;
static boolean opl_music_initialized = false;
static uint32_t audio_sample_rate = 11025;  /* Output rate for DMA-backed audio */

/* Sound mixing buffer for real audio (mono, unsigned 8-bit) */
#define MIX_BUFFER_SIZE 315  /* Max chunk size (~28.6 ms at 11025 Hz) */
static uint8_t mix_buffer[MIX_BUFFER_SIZE];

/*
 * OPL timing and resampling
 *
 * We generate output samples based on elapsed time so audio stays aligned
 * with the hardware sample rate even if game ticks jitter. OPL samples are
 * generated at OPL_SAMPLE_RATE and downsampled to OUTPUT_SAMPLE_RATE.
 */
#define OPL_SAMPLE_RATE 49716
#define OUTPUT_SAMPLE_RATE 11025
#define OPL_BUFFER_SAMPLES (((MIX_BUFFER_SIZE * OPL_SAMPLE_RATE) / OUTPUT_SAMPLE_RATE) + 16)
static int16_t opl_buffer[OPL_BUFFER_SAMPLES * 2];  /* Stereo 16-bit from OPL */

/* Boost music to match SFX loudness at 8-bit output. */
#define MUSIC_MIX_GAIN_NUM 6
#define MUSIC_MIX_GAIN_DEN 1

/* Audio timing state (ms -> samples) */
static uint32_t last_audio_ms = 0;
static uint32_t audio_ms_remainder = 0;
static uint32_t opl_sample_remainder = 0;

/* Cap catch-up to avoid overflowing the kernel ring buffer */
/*
 * Real hardware can run Doom slower than expected, which makes the elapsed time
 * between PCSoundUpdate() calls larger and causes underruns if we clamp too low.
 *
 * Keep this comfortably below the kernel's audio ring capacity (currently 8KB)
 * but large enough to absorb occasional long frames without audible gaps.
 */
#define MAX_CATCHUP_SAMPLES (MIX_BUFFER_SIZE * 16)


/* Channel state for real audio (8 channels like classic Doom) */
#define NUM_CHANNELS 8

static struct {
    const uint8_t *data;    /* Sound sample data */
    uint32_t length;        /* Sample length in bytes */
    uint32_t position;      /* Current playback position */
    int volume;             /* Channel volume (0-255) */
    boolean active;         /* Is channel playing? */
} channels[NUM_CHANNELS];

/* Doom sound lump header (8 bytes) */
struct doom_sound_header {
    uint16_t format;        /* Always 3 */
    uint16_t sample_rate;   /* Usually 11025 */
    uint32_t num_samples;   /* Number of samples */
};

/*
 * DP (PC speaker) sound lump header (4 bytes)
 * Format: 2 bytes zeros, 2 bytes sample count (little-endian)
 * Each sample is a tone value played at 140 Hz (7.14ms per sample)
 */
struct dp_sound_header {
    uint16_t zero;          /* Always 0 */
    uint16_t num_samples;   /* Number of tone samples */
};

/*
 * DP frequency lookup table (precomputed)
 * Value 0 = silence, Values 1-127 = quarter-tone scale
 * Formula: freq = 175 * 2^((value-1)/24)
 */
static const uint16_t dp_freq_table[128] = {
    0,    175,  180,  185,  191,  196,  202,  208,   /* 0-7 */
    214,  220,  227,  233,  240,  247,  254,  262,   /* 8-15 */
    269,  277,  285,  294,  302,  311,  320,  330,   /* 16-23 */
    339,  349,  360,  370,  381,  392,  404,  415,   /* 24-31 */
    428,  440,  453,  466,  480,  494,  509,  523,   /* 32-39 */
    539,  554,  571,  587,  605,  622,  640,  659,   /* 40-47 */
    678,  698,  719,  740,  761,  784,  807,  831,   /* 48-55 */
    855,  880,  906,  932,  960,  988,  1017, 1047,  /* 56-63 */
    1078, 1109, 1142, 1175, 1210, 1245, 1282, 1319,  /* 64-71 */
    1358, 1397, 1438, 1480, 1523, 1568, 1614, 1661,  /* 72-79 */
    1710, 1760, 1812, 1865, 1920, 1976, 2034, 2094,  /* 80-87 */
    2155, 2219, 2284, 2351, 2420, 2491, 2564, 2639,  /* 88-95 */
    2716, 2795, 2877, 2961, 3047, 3136, 3228, 3322,  /* 96-103 */
    3419, 3520, 3624, 3730, 3840, 3952, 4068, 4188,  /* 104-111 */
    4311, 4438, 4568, 4702, 4840, 4982, 5128, 5278,  /* 112-119 */
    5432, 5591, 5754, 5922, 6095, 6273, 6456, 6645   /* 120-127 */
};

/* DP playback timing: 140 Hz = 7.14ms per sample, use 7ms */
#define DP_SAMPLE_MS 7

/* PC speaker DP playback state */
static struct {
    const uint8_t *data;    /* Pointer to DP tone data */
    uint16_t length;        /* Total number of samples */
    uint16_t position;      /* Current sample position */
    uint32_t last_update;   /* Timestamp of last sample advance */
    boolean active;         /* Is DP sound playing? */
} dp_state;

/*
 * Play a tone on the PC speaker
 */
static void play_tone(uint32_t freq)
{
    current_freq = freq;
    ioctl(0, KIOCSOUND, freq);
}

/*
 * Stop the PC speaker
 */
static void stop_tone(void)
{
    if (current_freq != 0) {
        current_freq = 0;
        ioctl(0, KIOCSOUND, 0);
    }
}

/*
 * Sound module callbacks
 */

static boolean PCSoundInit(boolean use_sfx_prefix)
{
    struct audio_info info;
    struct audio_start_params params;
    int i;

    (void)use_sfx_prefix;

    /* Initialize channel state */
    for (i = 0; i < NUM_CHANNELS; i++) {
        channels[i].data = NULL;
        channels[i].length = 0;
        channels[i].position = 0;
        channels[i].volume = 0;
        channels[i].active = false;
    }

    /* Check for real audio hardware */
    if (ioctl(0, AUDIO_GET_INFO, &info) == 0 && info.has_real_audio) {
        audio_sample_rate = OUTPUT_SAMPLE_RATE;
        /* Start audio playback - use original mono 8-bit format that was working */
        params.sample_rate = audio_sample_rate;
        params.format = AUDIO_U8;     /* 8-bit unsigned (Doom native format) */
        params.channels = 1;          /* Mono */

        if (ioctl(0, AUDIO_START, &params) == 0) {
            use_real_audio = true;
            /* Ensure hardware amps are unmuted on real devices (HDA codecs can boot muted). */
            ioctl(0, AUDIO_SET_VOLUME, 80);

            /* Initialize OPL music subsystem at higher rate for quality,
             * we'll downsample when mixing */
            if (OPLMusic_Init(OPL_SAMPLE_RATE)) {
                opl_music_initialized = true;
            }
        }
    }

    pcsound_initialized = true;
    current_sound_channel = -1;
    dp_state.active = false;
    last_audio_ms = DG_GetTicksMs();
    audio_ms_remainder = 0;
    opl_sample_remainder = 0;
    return true;
}

static void PCSoundShutdown(void)
{
    if (opl_music_initialized) {
        OPLMusic_Shutdown();
        opl_music_initialized = false;
    }
    if (use_real_audio) {
        ioctl(0, AUDIO_STOP, 0);
        use_real_audio = false;
    }
    stop_tone();
    pcsound_initialized = false;
}

static int PCSoundGetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[16];
    int lumpnum;

    /*
     * Build lump name with appropriate prefix:
     * - "DS" = Digital Sound (for real audio hardware)
     * - "DP" = Digital sound for PC speaker
     */
    namebuf[0] = 'D';
    namebuf[1] = use_real_audio ? 'S' : 'P';
    namebuf[2] = '\0';

    /* Concatenate sfxinfo->name */
    {
        int i;
        const char *src = sfxinfo->name;
        char *dst = namebuf + 2;
        for (i = 0; i < 6 && src[i]; i++) {
            dst[i] = src[i];
        }
        dst[i] = '\0';
    }

    lumpnum = W_CheckNumForName(namebuf);
    return lumpnum;
}

static void PCSoundUpdate(void)
{
    /* Real audio mode: mix active channels and OPL music */
    if (use_real_audio) {
        struct audio_write_params wp;
        uint32_t now_ms;
        uint32_t elapsed_ms;
        uint32_t out_samples_num;
        uint32_t out_samples;
        int i, j;
        int sample_sum;
        int active_count;
        int opl_idx_start, opl_idx_end, k;

        now_ms = DG_GetTicksMs();
        if (last_audio_ms == 0) {
            last_audio_ms = now_ms;
            return;
        }

        elapsed_ms = now_ms - last_audio_ms;
        if (elapsed_ms == 0) {
            return;
        }
        last_audio_ms = now_ms;

        if (elapsed_ms > 1000) {
            elapsed_ms = 1000;
            audio_ms_remainder = 0;
        }

        out_samples_num = elapsed_ms * OUTPUT_SAMPLE_RATE + audio_ms_remainder;
        out_samples = out_samples_num / 1000;
        audio_ms_remainder = out_samples_num % 1000;

        if (out_samples == 0) {
            return;
        }

        if (out_samples > MAX_CATCHUP_SAMPLES) {
            out_samples = MAX_CATCHUP_SAMPLES;
            audio_ms_remainder = 0;
        }

        while (out_samples > 0) {
            uint32_t chunk = out_samples > MIX_BUFFER_SIZE ? MIX_BUFFER_SIZE : out_samples;
            uint32_t opl_samples_needed;

            uint32_t opl_samples_num;

            opl_samples_num = chunk * OPL_SAMPLE_RATE + opl_sample_remainder;
            opl_samples_needed = opl_samples_num / OUTPUT_SAMPLE_RATE;
            opl_sample_remainder = opl_samples_num % OUTPUT_SAMPLE_RATE;
            if (opl_samples_needed > OPL_BUFFER_SAMPLES) {
                opl_samples_needed = OPL_BUFFER_SAMPLES;
                opl_sample_remainder = 0;
            }

            if (opl_music_initialized && opl_samples_needed > 0) {
                OPLMusic_GenerateSamples(opl_buffer, opl_samples_needed);
            } else {
                opl_samples_needed = 0;
            }

            /* Mix all active channels into the buffer */
            for (j = 0; j < (int)chunk; j++) {
                sample_sum = 0;
                active_count = 0;

                /* Add OPL music (downsample from ~49716 Hz stereo 16-bit to 11025 Hz mono 8-bit) */
                if (opl_samples_needed > 0) {
                    int32_t mono_sum = 0;
                    int count = 0;

                    opl_idx_start = (j * (int)opl_samples_needed) / (int)chunk;
                    opl_idx_end = ((j + 1) * (int)opl_samples_needed) / (int)chunk;

                    if (opl_idx_end <= opl_idx_start) {
                        opl_idx_end = opl_idx_start + 1;
                    }
                    if (opl_idx_end > (int)opl_samples_needed) {
                        opl_idx_end = (int)opl_samples_needed;
                    }

                    for (k = opl_idx_start; k < opl_idx_end; k++) {
                        int32_t left = opl_buffer[k * 2];
                        int32_t right = opl_buffer[k * 2 + 1];
                        mono_sum += (left + right) / 2;
                        count++;
                    }

                    if (count > 0) {
                        int32_t mono = mono_sum / count;
                        /* Scale from 16-bit to 8-bit range (volume handled by OPL) */
                        mono = mono / 256;
                        mono = (mono * MUSIC_MIX_GAIN_NUM) / MUSIC_MIX_GAIN_DEN;
                        if (mono > 127) mono = 127;
                        if (mono < -128) mono = -128;
                        sample_sum += mono;
                    }
                }

                /* Mix in sound effects */
                for (i = 0; i < NUM_CHANNELS; i++) {
                    if (channels[i].active && channels[i].position < channels[i].length) {
                        /* Get sample (unsigned 8-bit, center is 128) */
                        int sample = channels[i].data[channels[i].position];
                        /* Apply volume (0-255) */
                        sample = ((sample - 128) * channels[i].volume / 255);
                        sample_sum += sample;
                        active_count++;
                        channels[i].position++;
                    } else if (channels[i].active) {
                        channels[i].active = false;
                    }
                }

                /* Convert back to unsigned 8-bit, clamp to valid range */
                sample_sum += 128;
                if (sample_sum < 0) sample_sum = 0;
                if (sample_sum > 255) sample_sum = 255;
                mix_buffer[j] = (uint8_t)sample_sum;
            }

            /* Write mixed buffer to audio device */
            {
                uint32_t off = 0;
                while (off < chunk) {
                    int w;
                    wp.buffer = mix_buffer + off;
                    wp.size = chunk - off;
                    w = ioctl(0, AUDIO_WRITE, &wp);
                    if (w <= 0) {
                        break; /* Ring full or device error; drop remainder */
                    }
                    off += (uint32_t)w;
                }
            }

            out_samples -= chunk;
        }
        return;
    }

    /* PC speaker mode: advance DP sound playback */
    if (dp_state.active) {
        uint32_t now = DG_GetTicksMs();
        uint32_t elapsed = now - dp_state.last_update;

        /* Advance samples based on elapsed time (7ms per sample) */
        while (elapsed >= DP_SAMPLE_MS && dp_state.active) {
            dp_state.position++;
            elapsed -= DP_SAMPLE_MS;
            dp_state.last_update += DP_SAMPLE_MS;

            /* Check if sound is finished */
            if (dp_state.position >= dp_state.length) {
                stop_tone();
                dp_state.active = false;
                current_sound_channel = -1;
                break;
            }

            /* Play the next tone */
            {
                uint8_t tone_val = dp_state.data[dp_state.position];
                if (tone_val < 128) {
                    play_tone(dp_freq_table[tone_val]);
                } else {
                    play_tone(0);  /* Invalid value = silence */
                }
            }
        }
    }
}

static void PCSoundUpdateSoundParams(int channel, int vol, int sep)
{
    (void)sep;  /* TODO: stereo panning not yet supported */

    /* Real audio mode: update channel volume */
    if (use_real_audio && channel >= 0 && channel < NUM_CHANNELS) {
        /* Doom volume is 0-127, we use 0-255 */
        channels[channel].volume = vol * 2;
        return;
    }

    /* PC speaker doesn't support volume or panning */
    (void)channel;
    (void)vol;
}

static int PCSoundStartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    (void)sep;

    if (!pcsound_initialized || !sfxinfo) {
        return -1;
    }

    /* Real audio mode: load and play actual sound sample */
    if (use_real_audio) {
        int lumpnum;
        byte *lumpdata;
        int lumplen;
        struct doom_sound_header *header;

        /* Validate channel */
        if (channel < 0 || channel >= NUM_CHANNELS) {
            return -1;
        }

        /* Get the sound lump */
        lumpnum = sfxinfo->lumpnum;
        if (lumpnum < 0) {
            return -1;
        }

        /* Cache the lump data */
        lumpdata = W_CacheLumpNum(lumpnum, PU_STATIC);
        if (!lumpdata) {
            return -1;
        }

        lumplen = W_LumpLength(lumpnum);
        if (lumplen < 8) {
            /* Too short to be a valid Doom sound */
            return -1;
        }

        /* Parse the Doom sound header */
        header = (struct doom_sound_header *)lumpdata;

        /* Validate format (Doom uses format code 3) */
        if (header->format != 3) {
            return -1;
        }

        /* Set up channel to play this sound */
        channels[channel].data = lumpdata + 8;  /* Skip 8-byte header */
        channels[channel].length = header->num_samples;
        channels[channel].position = 0;
        channels[channel].volume = vol * 2;  /* Doom: 0-127, we use 0-255 */
        channels[channel].active = true;

        return channel;
    }

    /* PC speaker fallback mode: load and play DP lump */
    (void)vol;
    {
        int lumpnum;
        byte *lumpdata;
        int lumplen;
        struct dp_sound_header *header;
        uint8_t tone_val;

        /* Get the DP lump */
        lumpnum = sfxinfo->lumpnum;
        if (lumpnum < 0) {
            return -1;
        }

        /* Cache the lump data */
        lumpdata = W_CacheLumpNum(lumpnum, PU_STATIC);
        if (!lumpdata) {
            return -1;
        }

        lumplen = W_LumpLength(lumpnum);
        if (lumplen < 4) {
            /* Too short to be a valid DP sound */
            return -1;
        }

        /* Parse the DP sound header */
        header = (struct dp_sound_header *)lumpdata;

        /* Validate header (first 2 bytes should be 0) */
        if (header->zero != 0 || header->num_samples == 0) {
            return -1;
        }

        /* Stop any currently playing DP sound */
        if (dp_state.active) {
            stop_tone();
        }

        /* Set up DP playback state */
        dp_state.data = lumpdata + 4;  /* Skip 4-byte header */
        dp_state.length = header->num_samples;
        dp_state.position = 0;
        dp_state.last_update = DG_GetTicksMs();
        dp_state.active = true;

        /* Start playing the first tone */
        tone_val = dp_state.data[0];
        if (tone_val < 128) {
            play_tone(dp_freq_table[tone_val]);
        } else {
            play_tone(0);  /* Invalid value = silence */
        }

        current_sound_channel = channel;
        return channel;
    }
}

static void PCSoundStopSound(int channel)
{
    /* Real audio mode: stop channel */
    if (use_real_audio) {
        if (channel >= 0 && channel < NUM_CHANNELS) {
            channels[channel].active = false;
        }
        return;
    }

    /* PC speaker mode */
    if (channel == current_sound_channel) {
        stop_tone();
        dp_state.active = false;
        current_sound_channel = -1;
    }
}

static boolean PCSoundSoundIsPlaying(int channel)
{
    /* Real audio mode: check channel state */
    if (use_real_audio) {
        if (channel >= 0 && channel < NUM_CHANNELS) {
            return channels[channel].active;
        }
        return false;
    }

    /* PC speaker mode */
    if (channel == current_sound_channel) {
        return dp_state.active;
    }
    return false;
}

static void PCSoundCacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
    /* Nothing to cache for PC speaker */
}

/*
 * Supported sound devices
 */
static snddevice_t pcsound_devices[] = {
    SNDDEVICE_PCSPEAKER,
    SNDDEVICE_SB,  /* Also claim SB so we're selected by default */
};

/*
 * PC Speaker sound module definition
 */
sound_module_t DG_sound_module = {
    pcsound_devices,
    sizeof(pcsound_devices) / sizeof(*pcsound_devices),
    PCSoundInit,
    PCSoundShutdown,
    PCSoundGetSfxLumpNum,
    PCSoundUpdate,
    PCSoundUpdateSoundParams,
    PCSoundStartSound,
    PCSoundStopSound,
    PCSoundSoundIsPlaying,
    PCSoundCacheSounds,
};

/*
 * OPL Music module - uses Nuked-OPL3 for FM synthesis
 */
static boolean OPLMusicModuleInit(void)
{
    /* OPL is already initialized in PCSoundInit when real audio is available */
    return opl_music_initialized;
}

static void OPLMusicModuleShutdown(void)
{
    /* OPL is shutdown in PCSoundShutdown */
}

static void OPLMusicSetVolume(int vol)
{
    /* Doom volume is 0-127 */
    if (opl_music_initialized) {
        OPLMusic_SetVolume(vol);
    }
}

static void OPLMusicPause(void)
{
    if (opl_music_initialized) {
        OPLMusic_PauseSong();
    }
}

static void OPLMusicResume(void)
{
    if (opl_music_initialized) {
        OPLMusic_ResumeSong();
    }
}

static void *OPLMusicRegister(void *data, int len)
{
    if (!opl_music_initialized) {
        return NULL;
    }
    return OPLMusic_RegisterSong(data, len);
}

static void OPLMusicUnregister(void *handle)
{
    if (opl_music_initialized && handle) {
        OPLMusic_UnRegisterSong(handle);
    }
}

static void OPLMusicPlay(void *handle, boolean loop)
{
    if (opl_music_initialized && handle) {
        OPLMusic_PlaySong(handle, loop);
    }
}

static void OPLMusicStop(void)
{
    if (opl_music_initialized) {
        OPLMusic_StopSong();
    }
}

static boolean OPLMusicIsPlaying(void)
{
    if (opl_music_initialized) {
        return OPLMusic_IsPlaying();
    }
    return false;
}

static snddevice_t music_devices[] = { SNDDEVICE_SB, SNDDEVICE_ADLIB };

music_module_t DG_music_module = {
    music_devices,
    sizeof(music_devices) / sizeof(*music_devices),
    OPLMusicModuleInit,
    OPLMusicModuleShutdown,
    OPLMusicSetVolume,
    OPLMusicPause,
    OPLMusicResume,
    OPLMusicRegister,
    OPLMusicUnregister,
    OPLMusicPlay,
    OPLMusicStop,
    OPLMusicIsPlaying,
    NULL,  /* Poll - we generate samples in PCSoundUpdate instead */
};
