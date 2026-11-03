/*
 * mixer.c: /dev/mixer (mixer.h), OSS's mixer over the audio driver's master
 * volume, its input controls and its beep generator.
 */

#include "mixer.h"

#include "audio.h"
#include "vchan.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"

/*
 * The requests are OSS's, with Linux's numbers: MIXER_READ(n) is
 * _IOR('M', n, int) and MIXER_WRITE(n) _IOWR('M', n, int), where n is a
 * channel below MIXER_CHANNELS or one of the masks' numbers above them.
 */
#define MIXER_READ(n)    (0x80044D00u | (n))
#define MIXER_WRITE(n)   (0xC0044D00u | (n))
#define MIXER_CHANNELS   25    /* SOUND_MIXER_NRDEVICES */
#define MIXER_RECSRC     0xFFu /* read and written */
#define MIXER_DEVMASK    0xFEu
#define MIXER_RECMASK    0xFDu
#define MIXER_CAPS       0xFCu
#define MIXER_STEREODEVS 0xFBu
#define MIXER_INFO       0x805C4D65u /* _IOR('M', 101, mixer_info) */
#define OSS_GETVERSION   0x80044D76u
#define OSS_VERSION      0x030802 /* SOUND_VERSION, as Linux gives it */
#define CAP_EXCL_INPUT   0x1      /* SOUND_CAP_EXCL_INPUT: one source at a time */

/* The channels this mixer has, by OSS's numbers (SOUND_MIXER_*). */
#define CH_VOLUME  0
#define CH_PCM     4
#define CH_SPEAKER 5
#define CH_LINE    6
#define CH_MIC     7
#define CH_RECLEV  11
#define CH_IGAIN   12
#define CH_MONITOR 24

/* The channel of each recording source, by audio_source_t. */
static const int source_channel[AUDIO_SOURCES] = {CH_MIC, CH_MONITOR, CH_LINE};

/* SOUND_MIXER_INFO's answer, OSS's `mixer_info`, copied out whole; the
 * fillers are the layout's reserved words. */
struct mixer_info {
    char id[16];
    char name[32];
    int modify_counter; /* the writes of levels and of the record source */
    int fillers[10];
};

/*
 * The levels, 0 to 100, as last set; each is one level for both sides.
 * Over vchan_driver VOLUME is the device's master volume and PCM the
 * caller's own channels' level, which vchan_pcm keeps; over another driver
 * the master volume is VOLUME times PCM, so that both reach the same
 * amplifiers. A level set in the driver by another path, the console's
 * AUDIO_SET_VOLUME, becomes VOLUME's, and PCM returns to 100. The input
 * gain of each source is its own level times RECLEV, and IGAIN is the boost
 * of every source that has one. `ready` is 0 until the first request reads
 * the driver's levels.
 */
static struct {
    int ready;
    int volume;
    int pcm;
    int applied; /* the master volume `volume` and `pcm` gave the driver */
    int source[AUDIO_SOURCES];
    int reclev;
    int igain;
    int writes;
} mx;

int mixer_present(void)
{
    return audio && audio->get_volume;
}

/** Copy the int `value` out to the user's `arg`: 0, or -EFAULT. */
static int32_t int_out(uintptr_t arg, int value)
{
    return copy_to_user((void __user *)arg, &value, sizeof(value)) != 0 ? -EFAULT : 0;
}

/** The level in OSS's form, the left channel's percentage in the low byte
 * and the right's in the next: the one level in both. */
static int stereo(int level)
{
    return level | level << 8;
}

/** The product of two levels, rounded, itself a level. */
static int scale(int a, int b)
{
    return (a * b + 50) / 100;
}

/** The sources the driver records, a mask of 1 << audio_source_t, and in
 * `*controls` its input controls; 0 for a driver with none. */
static unsigned input_caps(unsigned *controls)
{
    *controls = 0;
    return audio->input_caps ? audio->input_caps(controls) : 0;
}

/** The source the driver records, or -1. */
static int recorded(void)
{
    int src = audio->input_select ? audio->input_select(-1) : -1;

    return src >= 0 && src < AUDIO_SOURCES ? src : -1;
}

/** Give the driver source `src`'s gain, its level times RECLEV. */
static void apply_gain(int src)
{
    audio->input_level(AUDIO_IN_GAIN, src, scale(mx.source[src], mx.reclev));
}

/** Give the driver the master volume: VOLUME, or VOLUME times PCM without
 * the mix. */
static void apply_volume(void)
{
    audio->set_volume(audio == &vchan_driver ? mx.volume : scale(mx.volume, mx.pcm));
    mx.applied = audio->get_volume();
}

/**
 * Read the driver's levels at the first request: VOLUME is its master
 * volume, PCM 100, each source's level 100, RECLEV the gain of the source
 * recorded and IGAIN its boost; then, at every request, take a master
 * volume set by another path as VOLUME.
 */
static void sync_levels(void)
{
    int i, src, level;

    if (!mx.ready) {
        mx.ready = 1;
        mx.volume = mx.applied = audio->get_volume();
        mx.pcm = 100;
        for (i = 0; i < AUDIO_SOURCES; i++)
            mx.source[i] = 100;
        mx.reclev = 100;
        mx.igain = 0;
        if ((src = recorded()) >= 0) {
            if ((level = audio->input_level(AUDIO_IN_GAIN, src, -1)) >= 0)
                mx.reclev = level;
            if ((level = audio->input_level(AUDIO_IN_BOOST, src, -1)) >= 0)
                mx.igain = level;
        }
    }
    if (audio->get_volume() != mx.applied) {
        mx.volume = mx.applied = audio->get_volume();
        mx.pcm = 100;
    }
}

/** Whether the driver's device has a beep generator. */
static int has_beep(void)
{
    return audio->beep_level && audio->beep_level(-1) >= 0;
}

/** The channels the mixer has now, a mask of 1 << channel: VOLUME and PCM,
 * SPEAKER when the driver has a beep generator, a source's channel and
 * RECLEV when it has a gain, and IGAIN when it has a boost. */
static int devmask(void)
{
    unsigned controls, sources = input_caps(&controls);
    int mask = 1 << CH_VOLUME | 1 << CH_PCM, i;

    if (has_beep())
        mask |= 1 << CH_SPEAKER;

    if (controls & (1u << AUDIO_IN_GAIN)) {
        for (i = 0; i < AUDIO_SOURCES; i++)
            if (sources & (1u << i))
                mask |= 1 << source_channel[i];
        mask |= 1 << CH_RECLEV;
    }
    if (controls & (1u << AUDIO_IN_BOOST))
        mask |= 1 << CH_IGAIN;
    return mask;
}

/** The recording sources' channels, a mask of 1 << channel. */
static int recmask(void)
{
    unsigned controls, sources = input_caps(&controls);
    int mask = 0, i;

    for (i = 0; i < AUDIO_SOURCES; i++)
        if (sources & (1u << i))
            mask |= 1 << source_channel[i];
    return mask;
}

/** The channel of the source recorded, as a mask, or 0. */
static int recsrc(void)
{
    int src = recorded();

    return src >= 0 ? 1 << source_channel[src] : 0;
}

/** The level of channel `ch`, which the mixer has. */
static int get_level(int ch)
{
    int i;

    if (ch == CH_VOLUME)
        return mx.volume;
    if (ch == CH_PCM)
        return audio == &vchan_driver ? vchan_pcm(-1) : mx.pcm;
    if (ch == CH_SPEAKER)
        return audio->beep_level(-1);
    if (ch == CH_RECLEV)
        return mx.reclev;
    if (ch == CH_IGAIN)
        return mx.igain;
    for (i = 0; i < AUDIO_SOURCES; i++)
        if (source_channel[i] == ch)
            return mx.source[i];
    return 0;
}

/** Set channel `ch`, which the mixer has, to `level` and give the driver
 * what it changes. */
static void set_level(int ch, int level)
{
    unsigned controls, sources = input_caps(&controls);
    int i;

    if (ch == CH_PCM && audio == &vchan_driver) {
        (void)vchan_pcm(level);
    } else if (ch == CH_SPEAKER) {
        (void)audio->beep_level(level);
    } else if (ch == CH_VOLUME || ch == CH_PCM) {
        *(ch == CH_VOLUME ? &mx.volume : &mx.pcm) = level;
        apply_volume();
    } else if (ch == CH_RECLEV) {
        mx.reclev = level;
        for (i = 0; i < AUDIO_SOURCES; i++)
            if (sources & (1u << i))
                apply_gain(i);
    } else if (ch == CH_IGAIN) {
        mx.igain = level;
        for (i = 0; i < AUDIO_SOURCES; i++)
            if (sources & (1u << i))
                (void)audio->input_level(AUDIO_IN_BOOST, i, level);
    } else {
        for (i = 0; i < AUDIO_SOURCES; i++)
            if (source_channel[i] == ch) {
                mx.source[i] = level;
                apply_gain(i);
            }
    }
}

/**
 * Record one of the sources in the mask `want`: of those the driver has,
 * one other than the source recorded when there is one, so that a program
 * that adds a source to the mask it read switches to it; the source
 * recorded is kept when `want` names none. Returns the mask recorded after.
 */
static int set_recsrc(int want)
{
    int cur = recorded(), i, pick = -1;

    want &= recmask();
    for (i = 0; i < AUDIO_SOURCES; i++)
        if ((want & (1 << source_channel[i])) && (pick < 0 || pick == cur))
            pick = i;
    if (pick >= 0 && pick != cur)
        (void)audio->input_select(pick);
    return recsrc();
}

int32_t mixer_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct mixer_info info;
    uint32_t n = request & 0xFFu;
    int value, left, right;

    (void)e;
    if (!mixer_present()) {
        return -ENODEV;
    }
    sync_levels();
    switch (request) {
    case MIXER_READ(MIXER_RECSRC):
        return int_out(arg, recsrc());
    case MIXER_WRITE(MIXER_RECSRC):
        if (copy_from_user(&value, (const void __user *)arg, sizeof(value)) != 0) {
            return -EFAULT;
        }
        mx.writes++;
        return int_out(arg, set_recsrc(value));
    case MIXER_READ(MIXER_DEVMASK):
        return int_out(arg, devmask());
    case MIXER_READ(MIXER_RECMASK):
        return int_out(arg, recmask());
    case MIXER_READ(MIXER_CAPS):
        return int_out(arg, recmask() ? CAP_EXCL_INPUT : 0);
    case MIXER_READ(MIXER_STEREODEVS):
        return int_out(arg, 0);
    case OSS_GETVERSION:
        return int_out(arg, OSS_VERSION);
    case MIXER_INFO:
        k_memset(&info, 0, sizeof(info));
        k_strncpy(info.id, audio->type == AUDIO_DRIVER_HDA ? "HDA" : "SB16", sizeof(info.id) - 1);
        k_strncpy(info.name, audio->name, sizeof(info.name) - 1);
        info.modify_counter = mx.writes;
        return copy_to_user((void __user *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
    default:
        break;
    }
    if ((request >> 8 & 0xFFu) != 'M') {
        return -ENOTTY;
    }
    if (n >= MIXER_CHANNELS || !(devmask() & (1 << n))) {
        return -EINVAL; /* a channel this mixer lacks */
    }
    if (request == MIXER_READ(n)) {
        return int_out(arg, stereo(get_level((int)n)));
    }
    if (request != MIXER_WRITE(n)) {
        return -EINVAL;
    }
    /* A channel is one level, so the louder side of the two is the level,
     * and the answer is the level set, as OSS's write gives it. */
    if (copy_from_user(&value, (const void __user *)arg, sizeof(value)) != 0) {
        return -EFAULT;
    }
    left = value & 0xFF;
    right = (value >> 8) & 0xFF;
    value = left > right ? left : right;
    set_level((int)n, value > 100 ? 100 : value);
    mx.writes++;
    return int_out(arg, stereo(get_level((int)n)));
}
