/*
 * i_pcsound.c: the sound and music modules of Doom on Rach2.
 *
 * With a driver that plays samples, the SB16's or the HDA's, the effects are
 * mixed with the music of the OPL3 player into 11,025 Hz unsigned 8-bit mono
 * and written to the console with AUDIO_WRITE. Otherwise the effects play on
 * the PC speaker, one tone at a time, and there is no music.
 */

#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "doomtype.h"
#include "i_oplmusic.h"

#include <stdint.h>

/* The resampling settings src/i_sound.c binds under FEATURE_SOUND. This
 * module does not resample, so they are not read. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* The system call of crt0.c. */
extern int ioctl(int fd, unsigned long request, ...);

/* The console's sound requests (rach2/sys/syscall.c). */
#define KIOCSOUND       0x4B2F  /* PC speaker frequency (0 = off) */
#define AUDIO_GET_INFO  0x4101  /* Get audio driver info */
#define AUDIO_START     0x4102  /* Start audio playback */
#define AUDIO_STOP      0x4103  /* Stop audio playback */
#define AUDIO_WRITE     0x4104  /* Write audio samples */
#define AUDIO_SET_VOLUME 0x4105 /* Set volume (0-100) */

/* The driver types AUDIO_GET_INFO gives. */
#define AUDIO_DRIVER_NONE    0
#define AUDIO_DRIVER_SPEAKER 1
#define AUDIO_DRIVER_SB16    2
#define AUDIO_DRIVER_HDA     3

/* The sample formats AUDIO_START takes. */
#define AUDIO_U8    0
#define AUDIO_S16LE 1

/* The argument of AUDIO_GET_INFO. */
struct audio_info {
    uint32_t driver_type;
    uint32_t has_real_audio;
};

/* The argument of AUDIO_START. */
struct audio_start_params {
    uint32_t sample_rate;
    uint32_t format;
    uint32_t channels;
};

/* The argument of AUDIO_WRITE: the samples and their length in bytes. */
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size;
};

/* The platform layer's clock and report (doomgeneric_rach2.c). */
extern uint32_t DG_GetTicksMs(void);
extern uint32_t DG_NowUs(void);
extern void DG_TimeSpent(int what, uint32_t us);

static boolean pcsound_initialized = false;
static int current_sound_channel = -1; /* the speaker effect's, or -1 */
static uint32_t current_freq = 0;      /* the speaker's tone in Hz, 0 silent */

static boolean use_real_audio = false; /* AUDIO_START succeeded */
static boolean opl_music_initialized = false;
static uint32_t audio_sample_rate = 11025; /* the output rate in Hz */

/* One chunk of output, unsigned 8-bit mono: 315 samples are 28.6 ms. */
#define MIX_BUFFER_SIZE 315
static uint8_t mix_buffer[MIX_BUFFER_SIZE];

/*
 * Output is made by the clock: an update makes as many samples as the time
 * since the last is worth at OUTPUT_SAMPLE_RATE, so that the output keeps
 * pace with the hardware when frames come unevenly. The music player makes
 * stereo 16-bit samples at OPL_SAMPLE_RATE, which the mixer averages down.
 */
#define OPL_SAMPLE_RATE 49716
#define OUTPUT_SAMPLE_RATE 11025
#define OPL_BUFFER_SAMPLES (((MIX_BUFFER_SIZE * OPL_SAMPLE_RATE) / OUTPUT_SAMPLE_RATE) + 16)
static int16_t opl_buffer[OPL_BUFFER_SAMPLES * 2];  /* stereo pairs */

/* The music's gain in the mix, which makes it about as loud as the effects. */
#define MUSIC_MIX_GAIN_NUM 6
#define MUSIC_MIX_GAIN_DEN 1

static uint32_t last_audio_ms = 0;        /* the clock at the last update */
static uint32_t audio_ms_remainder = 0;   /* output samples, in thousandths */
static uint32_t opl_sample_remainder = 0; /* player samples, in 11025ths */

/* The most samples one update makes, some 457 ms, so that a long frame
 * leaves no gap; what the kernel's ring cannot take is dropped. */
#define MAX_CATCHUP_SAMPLES (MIX_BUFFER_SIZE * 16)


/* The effects that play with sample output, one to a channel. */
#define NUM_CHANNELS 8

static struct {
    const uint8_t *data;    /* the samples after the lump's header */
    uint32_t length;        /* in bytes, no more than the lump has */
    uint32_t position;      /* the next sample */
    int volume;             /* 0 to 254, twice the engine's */
    boolean active;
} channels[NUM_CHANNELS];

/* The header of a DS lump, 8 bytes, before its unsigned 8-bit samples. */
struct doom_sound_header {
    uint16_t format;        /* 3 */
    uint16_t sample_rate;   /* ignored; the samples play at 11,025 Hz */
    uint32_t num_samples;
};

/* The header of a DP lump, the speaker's form of an effect, 4 bytes, before
 * one tone value for each 1/140 s. */
struct dp_sound_header {
    uint16_t zero;          /* 0 */
    uint16_t num_samples;   /* the tone values */
};

/* The frequency of each DP tone value in Hz: 0 is silence, and 1 to 127 are
 * quarter tones from 175 Hz, 175 times 2 to the power (value - 1) / 24. */
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

/* A DP tone lasts 1/140 s, taken as 7 ms. */
#define DP_SAMPLE_MS 7

/* The effect sounding on the speaker. */
static struct {
    const uint8_t *data;    /* the tone values after the lump's header */
    uint16_t length;        /* tone values, no more than the lump has */
    uint16_t position;      /* the tone sounding */
    uint32_t last_update;   /* the clock in ms when the tone started */
    boolean active;
} dp_state;

/** Sound `freq` Hz on the PC speaker; 0 silences it. */
static void play_tone(uint32_t freq)
{
    current_freq = freq;
    ioctl(0, KIOCSOUND, freq);
}

/** Silence the PC speaker, unless it is silent already. */
static void stop_tone(void)
{
    if (current_freq != 0) {
        current_freq = 0;
        ioctl(0, KIOCSOUND, 0);
    }
}

/**
 * Ask the console for its audio driver. When the driver plays samples,
 * start output of 11,025 Hz unsigned 8-bit mono, set the volume to 80 of
 * 100, since an HDA codec may start muted, and start the music player at
 * 49,716 Hz; otherwise the effects go to the PC speaker. `use_sfx_prefix`
 * is ignored. Returns true either way.
 */
static boolean PCSoundInit(boolean use_sfx_prefix)
{
    struct audio_info info;
    struct audio_start_params params;
    int i;

    (void)use_sfx_prefix;

    for (i = 0; i < NUM_CHANNELS; i++) {
        channels[i].data = NULL;
        channels[i].length = 0;
        channels[i].position = 0;
        channels[i].volume = 0;
        channels[i].active = false;
    }

    if (ioctl(0, AUDIO_GET_INFO, &info) == 0 && info.has_real_audio) {
        audio_sample_rate = OUTPUT_SAMPLE_RATE;
        params.sample_rate = audio_sample_rate;
        params.format = AUDIO_U8;
        params.channels = 1;

        if (ioctl(0, AUDIO_START, &params) == 0) {
            use_real_audio = true;
            ioctl(0, AUDIO_SET_VOLUME, 80);

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

/** Stop the music player, the sample output and the speaker. */
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

/**
 * The lump of an effect: DS and the first six letters of its name with
 * sample output, DP and the same on the speaker; -1 when the WAD has none.
 */
static int PCSoundGetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[16];
    int lumpnum;

    namebuf[0] = 'D';
    namebuf[1] = use_real_audio ? 'S' : 'P';
    namebuf[2] = '\0';

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

/**
 * With sample output, make the samples the time since the last update is
 * worth, no more than a second's and MAX_CATCHUP_SAMPLES, in chunks of
 * MIX_BUFFER_SIZE. For each chunk take OPL_SAMPLE_RATE / OUTPUT_SAMPLE_RATE
 * times as many stereo samples from the music player, average them to mono
 * and to the output rate, scale them to 8 bits with the music's gain, add
 * the channels at their volumes, limit each sum to a byte, and write the
 * chunk with AUDIO_WRITE until a write takes nothing; the rest is dropped.
 * On the speaker, step the effect sounding by 7 ms tones, and end it after
 * its last.
 */
static void PCSoundUpdate(void)
{
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
        uint32_t mix_t0;

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
                uint32_t t0 = DG_NowUs();

                OPLMusic_GenerateSamples(opl_buffer, opl_samples_needed);
                DG_TimeSpent(0, DG_NowUs() - t0);
            } else {
                opl_samples_needed = 0;
            }

            mix_t0 = DG_NowUs();
            for (j = 0; j < (int)chunk; j++) {
                sample_sum = 0;
                active_count = 0;

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
                        mono = mono / 256;
                        mono = (mono * MUSIC_MIX_GAIN_NUM) / MUSIC_MIX_GAIN_DEN;
                        if (mono > 127) mono = 127;
                        if (mono < -128) mono = -128;
                        sample_sum += mono;
                    }
                }

                for (i = 0; i < NUM_CHANNELS; i++) {
                    if (channels[i].active && channels[i].position < channels[i].length) {
                        /* unsigned 8-bit, centered on 128 */
                        int sample = channels[i].data[channels[i].position];
                        sample = ((sample - 128) * channels[i].volume / 255);
                        sample_sum += sample;
                        active_count++;
                        channels[i].position++;
                    } else if (channels[i].active) {
                        channels[i].active = false;
                    }
                }

                sample_sum += 128;
                if (sample_sum < 0) sample_sum = 0;
                if (sample_sum > 255) sample_sum = 255;
                mix_buffer[j] = (uint8_t)sample_sum;
            }

            {
                uint32_t off = 0;
                DG_TimeSpent(1, DG_NowUs() - mix_t0);
                while (off < chunk) {
                    int w;
                    wp.buffer = mix_buffer + off;
                    wp.size = chunk - off;
                    w = ioctl(0, AUDIO_WRITE, &wp);
                    if (w <= 0) {
                        break; /* the ring is full, or the driver failed */
                    }
                    off += (uint32_t)w;
                }
            }

            out_samples -= chunk;
        }
        return;
    }

    if (dp_state.active) {
        uint32_t now = DG_GetTicksMs();
        uint32_t elapsed = now - dp_state.last_update;

        while (elapsed >= DP_SAMPLE_MS && dp_state.active) {
            dp_state.position++;
            elapsed -= DP_SAMPLE_MS;
            dp_state.last_update += DP_SAMPLE_MS;

            if (dp_state.position >= dp_state.length) {
                stop_tone();
                dp_state.active = false;
                current_sound_channel = -1;
                break;
            }

            {
                uint8_t tone_val = dp_state.data[dp_state.position];
                if (tone_val < 128) {
                    play_tone(dp_freq_table[tone_val]);
                } else {
                    play_tone(0);  /* above 127: silence */
                }
            }
        }
    }
}

/**
 * With sample output, set the volume of `channel` to twice the engine's
 * `vol`, 0 to 127. The separation `sep` is ignored, and so is the call on
 * the speaker.
 */
static void PCSoundUpdateSoundParams(int channel, int vol, int sep)
{
    (void)sep;

    if (use_real_audio && channel >= 0 && channel < NUM_CHANNELS) {
        channels[channel].volume = vol * 2;
        return;
    }

    (void)channel;
    (void)vol;
}

/**
 * Start the effect `sfxinfo` on `channel`. With sample output its DS lump
 * plays on the channel at `vol`; on the speaker its DP lump replaces the
 * effect sounding, whatever the channel, and its first tone sounds at once.
 * The count in a lump's header is trusted no further than the lump's
 * length. Returns `channel`, or -1 when the module is not started, the
 * lump is missing, short, of another format or without samples, or, with
 * sample output, `channel` is out of range.
 */
static int PCSoundStartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    (void)sep;

    if (!pcsound_initialized || !sfxinfo) {
        return -1;
    }

    if (use_real_audio) {
        int lumpnum;
        byte *lumpdata;
        int lumplen;
        struct doom_sound_header *header;

        if (channel < 0 || channel >= NUM_CHANNELS) {
            return -1;
        }

        lumpnum = sfxinfo->lumpnum;
        if (lumpnum < 0) {
            return -1;
        }

        lumpdata = W_CacheLumpNum(lumpnum, PU_STATIC);
        if (!lumpdata) {
            return -1;
        }

        lumplen = W_LumpLength(lumpnum);
        if (lumplen < 8) {
            return -1;
        }

        header = (struct doom_sound_header *)lumpdata;

        if (header->format != 3) {
            return -1;
        }

        channels[channel].data = lumpdata + 8;
        channels[channel].length = header->num_samples;
        if (channels[channel].length > (uint32_t)(lumplen - 8)) {
            channels[channel].length = (uint32_t)(lumplen - 8);
        }
        channels[channel].position = 0;
        channels[channel].volume = vol * 2;
        channels[channel].active = true;

        return channel;
    }

    (void)vol;
    {
        int lumpnum;
        byte *lumpdata;
        int lumplen;
        struct dp_sound_header *header;
        uint8_t tone_val;

        lumpnum = sfxinfo->lumpnum;
        if (lumpnum < 0) {
            return -1;
        }

        lumpdata = W_CacheLumpNum(lumpnum, PU_STATIC);
        if (!lumpdata) {
            return -1;
        }

        lumplen = W_LumpLength(lumpnum);
        if (lumplen < 4) {
            return -1;
        }

        header = (struct dp_sound_header *)lumpdata;

        if (header->zero != 0 || header->num_samples == 0) {
            return -1;
        }

        if (dp_state.active) {
            stop_tone();
        }

        dp_state.data = lumpdata + 4;
        dp_state.length = header->num_samples;
        if (dp_state.length > (uint32_t)(lumplen - 4)) {
            dp_state.length = (uint16_t)(lumplen - 4);
        }
        if (dp_state.length == 0) {
            return -1;
        }
        dp_state.position = 0;
        dp_state.last_update = DG_GetTicksMs();
        dp_state.active = true;

        tone_val = dp_state.data[0];
        if (tone_val < 128) {
            play_tone(dp_freq_table[tone_val]);
        } else {
            play_tone(0);  /* above 127: silence */
        }

        current_sound_channel = channel;
        return channel;
    }
}

/** Stop the effect on `channel`; on the speaker, only when it is the
 * channel of the effect sounding. */
static void PCSoundStopSound(int channel)
{
    if (use_real_audio) {
        if (channel >= 0 && channel < NUM_CHANNELS) {
            channels[channel].active = false;
        }
        return;
    }

    if (channel == current_sound_channel) {
        stop_tone();
        dp_state.active = false;
        current_sound_channel = -1;
    }
}

/** Whether an effect plays on `channel`. */
static boolean PCSoundSoundIsPlaying(int channel)
{
    if (use_real_audio) {
        if (channel >= 0 && channel < NUM_CHANNELS) {
            return channels[channel].active;
        }
        return false;
    }

    if (channel == current_sound_channel) {
        return dp_state.active;
    }
    return false;
}

/** Does nothing: an effect's lump is read when the effect starts. */
static void PCSoundCacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds;
    (void)num_sounds;
}

/* The devices the sound module serves. The Sound Blaster is the engine's
 * default device, so the module is chosen on every machine. */
static snddevice_t pcsound_devices[] = {
    SNDDEVICE_PCSPEAKER,
    SNDDEVICE_SB,
};

/* The sound module src/i_sound.c takes under FEATURE_SOUND. */
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
 * The music module, over the player of i_oplmusic.c. Each call does nothing
 * unless PCSoundInit started the player.
 */

/** Whether PCSoundInit started the player. */
static boolean OPLMusicModuleInit(void)
{
    return opl_music_initialized;
}

/** Does nothing: PCSoundShutdown stops the player. */
static void OPLMusicModuleShutdown(void)
{
}

/** Set the music's volume, `vol`, 0 to 127. */
static void OPLMusicSetVolume(int vol)
{
    if (opl_music_initialized) {
        OPLMusic_SetVolume(vol);
    }
}

/** Pause the song. */
static void OPLMusicPause(void)
{
    if (opl_music_initialized) {
        OPLMusic_PauseSong();
    }
}

/** Resume the song. */
static void OPLMusicResume(void)
{
    if (opl_music_initialized) {
        OPLMusic_ResumeSong();
    }
}

/** The handle of the song lump `data`, of `len` bytes, or NULL. */
static void *OPLMusicRegister(void *data, int len)
{
    if (!opl_music_initialized) {
        return NULL;
    }
    return OPLMusic_RegisterSong(data, len);
}

/** Free the song `handle`. */
static void OPLMusicUnregister(void *handle)
{
    if (opl_music_initialized && handle) {
        OPLMusic_UnRegisterSong(handle);
    }
}

/** Play the song `handle`, from its start again at its end when `loop`. */
static void OPLMusicPlay(void *handle, boolean loop)
{
    if (opl_music_initialized && handle) {
        OPLMusic_PlaySong(handle, loop);
    }
}

/** Stop the song. */
static void OPLMusicStop(void)
{
    if (opl_music_initialized) {
        OPLMusic_StopSong();
    }
}

/** Whether a song plays and is not paused. */
static boolean OPLMusicIsPlaying(void)
{
    if (opl_music_initialized) {
        return OPLMusic_IsPlaying();
    }
    return false;
}

/* The devices the music module serves. */
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
    NULL,  /* Poll: PCSoundUpdate runs the player */
};
