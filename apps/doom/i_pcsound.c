/*
 * i_pcsound.c - PC Speaker sound module for Doom on Rach2
 *
 * Implements a simple sound_module_t that converts Doom sound effects
 * to PC speaker beeps. The PC speaker can only play one tone at a time,
 * so we map sound effects to distinct frequencies.
 */

#include "sounds.h"
#include "i_sound.h"
#include "w_wad.h"
#include "doomtype.h"

#include <stdint.h>

/*
 * Dummy variables required by i_sound.c when FEATURE_SOUND is defined.
 * These are used for libsamplerate integration which we don't support.
 */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

/* Syscall wrappers */
extern int ioctl(int fd, unsigned long request, ...);

/* ioctl request code for PC speaker */
#define KIOCSOUND 0x4B2F

/* Get current time in ms */
extern uint32_t DG_GetTicksMs(void);

/* Sound state */
static boolean pcsound_initialized = false;
static int current_sound_channel = -1;
static uint32_t sound_end_time = 0;
static uint32_t current_freq = 0;

/*
 * Frequency mapping for Doom sound effects.
 * PC speaker is loudest at 150-600Hz. Very low (<100Hz) sounds weak.
 * Use mid-range for punch, high for sci-fi, distinct tones for each weapon.
 */
static uint32_t get_sound_frequency(int sfx_id)
{
    switch (sfx_id) {
        /* Weapons - mid frequencies where PC speaker has punch */
        case sfx_pistol:  return 280;   /* Sharp gunshot crack */
        case sfx_shotgn:  return 180;   /* Powerful boom */
        case sfx_sgcock:  return 500;   /* Metallic click */
        case sfx_dshtgn:  return 140;   /* Big double-barrel blast */
        case sfx_plasma:  return 900;   /* Sci-fi energy zap */
        case sfx_bfg:     return 120;   /* Low powerful hum */
        case sfx_chgun:   return 320;   /* Rapid staccato */
        case sfx_sawup:   return 350;   /* Engine revving */
        case sfx_sawidl:  return 280;   /* Buzzing idle */
        case sfx_sawful:  return 450;   /* High-speed whine */
        case sfx_sawhit:  return 550;   /* Cutting sound */
        case sfx_rlaunc:  return 200;   /* Whoosh launch */
        case sfx_punch:   return 240;   /* Solid thump */

        /* Explosions - low-mid range, audible but heavy */
        case sfx_rxplod:  return 110;   /* Big explosion */
        case sfx_barexp:  return 100;   /* Barrel boom */
        case sfx_firxpl:  return 130;   /* Fireball burst */

        /* Pickups - pleasant higher tones */
        case sfx_itemup:  return 1047;  /* C6 - item pickup */
        case sfx_wpnup:   return 1319;  /* E6 - weapon pickup */
        case sfx_getpow:  return 1568;  /* G6 - powerup */

        /* Doors and switches */
        case sfx_doropn:  return 262;   /* C4 - door open */
        case sfx_dorcls:  return 247;   /* B3 - door close */
        case sfx_swtchn:  return 523;   /* C5 - switch on */
        case sfx_swtchx:  return 494;   /* B4 - switch off */
        case sfx_pstart:  return 200;   /* platform start */
        case sfx_pstop:   return 180;   /* platform stop */
        case sfx_stnmov:  return 150;   /* stone move */

        /* Player sounds */
        case sfx_plpain:  return 300;   /* player pain */
        case sfx_oof:     return 280;   /* oof (wall bump) */
        case sfx_pldeth:  return 120;   /* player death */
        case sfx_pdiehi:  return 100;   /* player death high */
        case sfx_noway:   return 200;   /* can't use */
        case sfx_slop:    return 160;   /* gib sound */

        /* Teleport */
        case sfx_telept:  return 700;   /* teleport */

        /* Menu/UI sounds - use swtchn as menu sound */
        case sfx_tink:    return 1000;  /* tink (menu) */

        /* Monster sounds - various frequencies */
        case sfx_posit1:
        case sfx_posit2:
        case sfx_posit3:  return 250;   /* zombie sight */
        case sfx_podth1:
        case sfx_podth2:
        case sfx_podth3:  return 170;   /* zombie death */
        case sfx_bgdth1:
        case sfx_bgdth2:  return 140;   /* shotgun guy death */
        case sfx_cacsit:  return 320;   /* cacodemon sight */
        case sfx_cacdth:  return 200;   /* cacodemon death */
        case sfx_skldth:  return 400;   /* lost soul death */
        case sfx_brssit:  return 180;   /* baron sight */
        case sfx_brsdth:  return 100;   /* baron death */
        case sfx_cybsit:  return 90;    /* cyberdemon sight */
        case sfx_cybdth:  return 60;    /* cyberdemon death */
        case sfx_spisit:  return 280;   /* spider sight */
        case sfx_spidth:  return 70;    /* spider death */

        /* Impact/hit sounds */
        case sfx_firsht:  return 350;   /* fireball shot */
        case sfx_claw:    return 450;   /* claw attack */

        /* Default - no sound for unmapped effects */
        default:          return 0;
    }
}

/*
 * Get sound duration in milliseconds based on sound type.
 * Short and snappy for rapid-fire, longer for powerful single shots.
 */
static uint32_t get_sound_duration(int sfx_id)
{
    switch (sfx_id) {
        /* Weapons - tuned for feel */
        case sfx_pistol:  return 70;    /* Quick snap */
        case sfx_chgun:   return 45;    /* Very rapid */
        case sfx_shotgn:  return 120;   /* Solid boom */
        case sfx_dshtgn:  return 160;   /* Bigger boom */
        case sfx_sgcock:  return 50;    /* Quick click */
        case sfx_plasma:  return 35;    /* Quick zap */
        case sfx_bfg:     return 400;   /* Long charge */
        case sfx_rlaunc:  return 150;   /* Whoosh */
        case sfx_punch:   return 60;    /* Quick thud */
        case sfx_sawup:   return 250;   /* Rev up */
        case sfx_sawidl:  return 180;   /* Ongoing buzz */
        case sfx_sawful:  return 120;   /* Active cutting */
        case sfx_sawhit:  return 100;   /* Impact */

        /* Explosions - sustained rumble */
        case sfx_rxplod:  return 300;   /* Big boom */
        case sfx_barexp:  return 350;   /* Chain reaction feel */
        case sfx_firxpl:  return 200;   /* Quick burst */

        /* Pickups - medium */
        case sfx_itemup:  return 100;
        case sfx_wpnup:   return 120;
        case sfx_getpow:  return 150;

        /* Doors - longer */
        case sfx_doropn:
        case sfx_dorcls:  return 300;
        case sfx_swtchn:
        case sfx_swtchx:  return 80;

        /* Player */
        case sfx_plpain:  return 150;
        case sfx_oof:     return 80;
        case sfx_pldeth:
        case sfx_pdiehi:  return 400;
        case sfx_noway:   return 100;

        /* Teleport */
        case sfx_telept:  return 200;

        /* Default */
        default:          return 100;
    }
}

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
    (void)use_sfx_prefix;
    pcsound_initialized = true;
    current_sound_channel = -1;
    sound_end_time = 0;
    return true;
}

static void PCSoundShutdown(void)
{
    stop_tone();
    pcsound_initialized = false;
}

static int PCSoundGetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char namebuf[16];

    /* Try to find the lump, but we don't actually use it */
    namebuf[0] = 'D';
    namebuf[1] = 'P';
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

    return W_CheckNumForName(namebuf);
}

static void PCSoundUpdate(void)
{
    /* Check if current sound has finished */
    if (current_sound_channel >= 0 && sound_end_time > 0) {
        uint32_t now = DG_GetTicksMs();
        if (now >= sound_end_time) {
            stop_tone();
            current_sound_channel = -1;
            sound_end_time = 0;
        }
    }
}

static void PCSoundUpdateSoundParams(int channel, int vol, int sep)
{
    (void)channel;
    (void)vol;
    (void)sep;
    /* PC speaker doesn't support volume or panning */
}

static int PCSoundStartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    int sfx_id;
    uint32_t freq, duration;

    (void)vol;
    (void)sep;

    if (!pcsound_initialized || !sfxinfo) {
        return -1;
    }

    /* Get sfx ID from the S_sfx array index */
    sfx_id = sfxinfo - S_sfx;

    /* Get frequency for this sound */
    freq = get_sound_frequency(sfx_id);
    if (freq == 0) {
        /* No mapping for this sound */
        return -1;
    }

    /* Get duration */
    duration = get_sound_duration(sfx_id);

    /* Play the tone */
    play_tone(freq);

    /* Set end time */
    sound_end_time = DG_GetTicksMs() + duration;
    current_sound_channel = channel;

    return channel;
}

static void PCSoundStopSound(int channel)
{
    if (channel == current_sound_channel) {
        stop_tone();
        current_sound_channel = -1;
        sound_end_time = 0;
    }
}

static boolean PCSoundSoundIsPlaying(int channel)
{
    if (channel == current_sound_channel && sound_end_time > 0) {
        return DG_GetTicksMs() < sound_end_time;
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
 * Dummy music module (no music support)
 */
static boolean DummyMusicInit(void) { return true; }
static void DummyMusicShutdown(void) {}
static void DummyMusicSetVolume(int vol) { (void)vol; }
static void DummyMusicPause(void) {}
static void DummyMusicResume(void) {}
static void *DummyMusicRegister(void *data, int len) { (void)data; (void)len; return NULL; }
static void DummyMusicUnregister(void *handle) { (void)handle; }
static void DummyMusicPlay(void *handle, boolean loop) { (void)handle; (void)loop; }
static void DummyMusicStop(void) {}
static boolean DummyMusicIsPlaying(void) { return false; }

static snddevice_t music_devices[] = { SNDDEVICE_SB };

music_module_t DG_music_module = {
    music_devices,
    1,
    DummyMusicInit,
    DummyMusicShutdown,
    DummyMusicSetVolume,
    DummyMusicPause,
    DummyMusicResume,
    DummyMusicRegister,
    DummyMusicUnregister,
    DummyMusicPlay,
    DummyMusicStop,
    DummyMusicIsPlaying,
    NULL,  /* Poll */
};
