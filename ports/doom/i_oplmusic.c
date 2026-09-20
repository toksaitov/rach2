//
// i_oplmusic.c - OPL3 music playback for Doom on Rach2
//
// Based on Chocolate Doom's i_oplmusic.c
// Simplified to use Nuked-OPL3 emulator with polling-based timing
//

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_swap.h"
#include "w_wad.h"
#include "z_zone.h"
#include "memio.h"
#include "mus2mid.h"
#include "midifile.h"
#include "opl3.h"
#include "i_oplmusic.h"
extern void DG_TimeSpent(int what, uint32_t n);

// OPL3 configuration
#define OPL_NUM_VOICES 9
#define GENMIDI_NUM_INSTRS 128
#define GENMIDI_NUM_PERCUSSION 47
#define GENMIDI_HEADER "#OPL_II#"
#define GENMIDI_FLAG_FIXED 0x0001
#define GENMIDI_FLAG_2VOICE 0x0004

// OPL register definitions
#define OPL_REGS_TREMOLO 0x20
#define OPL_REGS_LEVEL 0x40
#define OPL_REGS_ATTACK 0x60
#define OPL_REGS_SUSTAIN 0x80
#define OPL_REGS_FREQ_1 0xA0
#define OPL_REGS_FREQ_2 0xB0
#define OPL_REGS_FEEDBACK 0xC0
#define OPL_REGS_WAVEFORM 0xE0

// GENMIDI instrument structures
typedef struct {
    byte tremolo;
    byte attack;
    byte sustain;
    byte waveform;
    byte scale;
    byte level;
} __attribute__((packed)) genmidi_op_t;

typedef struct {
    genmidi_op_t modulator;
    byte feedback;
    genmidi_op_t carrier;
    byte unused;
    short base_note_offset;
} __attribute__((packed)) genmidi_voice_t;

typedef struct {
    unsigned short flags;
    byte fine_tuning;
    byte fixed_note;
    genmidi_voice_t voices[2];
} __attribute__((packed)) genmidi_instr_t;

// Voice state
typedef struct {
    int index;
    int op1, op2;
    genmidi_instr_t *current_instr;
    unsigned int current_instr_voice;
    int channel;
    unsigned int key;
    unsigned int note;
    unsigned int freq;
    unsigned int note_volume;
    unsigned int car_volume;
    unsigned int mod_volume;
    unsigned int priority;
} opl_voice_t;

// Channel state
typedef struct {
    genmidi_instr_t *instrument;
    int volume;
    int volume_base;
    int bend;
} opl_channel_data_t;

// Operators for each voice
static const int voice_operators[2][OPL_NUM_VOICES] = {
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 }
};

// Frequency curve
static const unsigned short frequency_curve[] = {
    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,
    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,
    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,
    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,
    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,
    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,
    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,
    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,
    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,
    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,
    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,
    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,
    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,
    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,
    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,
    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,
    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,
    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,
    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,
    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,
    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

// Volume mapping table
static const unsigned int volume_mapping_table[] = {
    0, 1, 3, 5, 6, 8, 10, 11,
    13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34,
    36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61,
    63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81,
    82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

// Global state
static opl3_chip opl_chip;
static boolean music_initialized = false;
static boolean music_playing = false;
static boolean music_paused = false;
static int music_volume = 127;

// GENMIDI data
static genmidi_instr_t *main_instrs;
static genmidi_instr_t *percussion_instrs;

// Voices
static opl_voice_t voices[OPL_NUM_VOICES];
static opl_voice_t *voice_free_list[OPL_NUM_VOICES];
static opl_voice_t *voice_alloced_list[OPL_NUM_VOICES];
static int voice_free_num;
static int voice_alloced_num;

// Channels
static opl_channel_data_t channels[16];

// Current song. A format 1 MIDI file, which is what Freedoom ships, has
// its tempo on track 0 and its notes on the tracks after it, so every
// track is iterated, each with its own wait in samples until its next
// event; a MUS lump converts to one track and is the same case with one
// iterator.
#define MAX_TRACKS 32
static midi_file_t *current_file = NULL;
static midi_track_iter_t *tracks[MAX_TRACKS];
static uint32_t track_wait[MAX_TRACKS]; /* samples until the track's next event */
static boolean track_done[MAX_TRACKS];  /* the track reached its end */
static int num_tracks = 0;
static boolean song_looping = false;

// Timing
static uint32_t sample_rate;
static uint32_t samples_per_tick;
static uint32_t ticks_per_beat;
static uint32_t us_per_beat;

// Write to OPL3 register
static void WriteRegister(uint16_t reg, uint8_t value)
{
    OPL3_WriteReg(&opl_chip, reg, value);
}

// Initialize OPL registers
static void InitRegisters(void)
{
    int i;

    // Initialize OPL3 chip
    OPL3_Reset(&opl_chip, sample_rate);

    // Enable waveform select
    WriteRegister(0x01, 0x20);

    // Clear all registers
    for (i = 0; i < OPL_NUM_VOICES; i++)
    {
        WriteRegister(OPL_REGS_FREQ_2 + i, 0);
    }
}

/** Load a complete GENMIDI instrument table with a valid header. */
static boolean LoadInstrumentTable(void)
{
    int lumpnum;
    byte *lump;

    lumpnum = W_CheckNumForName("genmidi");
    if (lumpnum < 0
     || W_LumpLength(lumpnum) < 8
        + (GENMIDI_NUM_INSTRS + GENMIDI_NUM_PERCUSSION)
          * (int)sizeof(genmidi_instr_t))
    {
        return false;
    }

    lump = W_CacheLumpNum(lumpnum, PU_STATIC);
    if (lump == NULL)
    {
        return false;
    }
    if (memcmp(lump, GENMIDI_HEADER, 8) != 0)
    {
        W_ReleaseLumpNum(lumpnum);
        return false;
    }

    main_instrs = (genmidi_instr_t *)(lump + 8);
    percussion_instrs = main_instrs + GENMIDI_NUM_INSTRS;
    return true;
}

// Initialize voices
static void InitVoices(void)
{
    int i;

    voice_free_num = OPL_NUM_VOICES;
    voice_alloced_num = 0;

    for (i = 0; i < OPL_NUM_VOICES; i++)
    {
        voices[i].index = i;
        voices[i].op1 = voice_operators[0][i];
        voices[i].op2 = voice_operators[1][i];
        voices[i].current_instr = NULL;
        voices[i].channel = -1;
        voice_free_list[i] = &voices[i];
    }
}

// Initialize channel
static void InitChannel(opl_channel_data_t *channel)
{
    channel->instrument = &main_instrs[0];
    channel->volume = music_volume;
    channel->volume_base = 100;
    channel->bend = 0;
}

// Get free voice
static opl_voice_t *GetFreeVoice(void)
{
    opl_voice_t *result;
    int i;

    if (voice_free_num == 0)
    {
        return NULL;
    }

    result = voice_free_list[0];
    voice_free_num--;

    for (i = 0; i < voice_free_num; i++)
    {
        voice_free_list[i] = voice_free_list[i + 1];
    }

    voice_alloced_list[voice_alloced_num++] = result;

    return result;
}

// Voice key off
static void VoiceKeyOff(opl_voice_t *voice)
{
    WriteRegister(OPL_REGS_FREQ_2 + voice->index, voice->freq >> 8);
}

// Release voice
static void ReleaseVoice(int index)
{
    opl_voice_t *voice;
    int i;

    if (index >= voice_alloced_num)
    {
        return;
    }

    voice = voice_alloced_list[index];
    VoiceKeyOff(voice);
    voice->channel = -1;
    voice->note = 0;

    voice_alloced_num--;
    for (i = index; i < voice_alloced_num; i++)
    {
        voice_alloced_list[i] = voice_alloced_list[i + 1];
    }

    voice_free_list[voice_free_num++] = voice;
}

// Load operator data
static void LoadOperatorData(int operator, genmidi_op_t *data,
                             boolean max_level, unsigned int *volume)
{
    int level;

    level = data->scale;
    if (max_level)
    {
        level |= 0x3f;
    }
    else
    {
        level |= data->level;
    }

    *volume = level;

    WriteRegister(OPL_REGS_LEVEL + operator, level);
    WriteRegister(OPL_REGS_TREMOLO + operator, data->tremolo);
    WriteRegister(OPL_REGS_ATTACK + operator, data->attack);
    WriteRegister(OPL_REGS_SUSTAIN + operator, data->sustain);
    WriteRegister(OPL_REGS_WAVEFORM + operator, data->waveform);
}

// Set voice instrument
static void SetVoiceInstrument(opl_voice_t *voice, genmidi_instr_t *instr,
                               unsigned int instr_voice)
{
    genmidi_voice_t *data;
    unsigned int modulating;

    if (voice->current_instr == instr && voice->current_instr_voice == instr_voice)
    {
        return;
    }

    voice->current_instr = instr;
    voice->current_instr_voice = instr_voice;

    /* DEBUG: Use simple hardcoded instrument instead of GENMIDI
     * Set to 1 to test if the issue is in GENMIDI loading
     * Set to 0 to use real GENMIDI instruments from doom1.wad */
#define USE_SIMPLE_INSTRUMENT 0
#if USE_SIMPLE_INSTRUMENT
    (void)data;
    (void)modulating;
    /* Simple sine-wave like instrument */
    WriteRegister(OPL_REGS_TREMOLO + voice->op1, 0x01);  /* Modulator: mult=1 */
    WriteRegister(OPL_REGS_LEVEL + voice->op1, 0x10);    /* Modulator level */
    WriteRegister(OPL_REGS_ATTACK + voice->op1, 0xF0);   /* Fast attack, no decay */
    WriteRegister(OPL_REGS_SUSTAIN + voice->op1, 0x77);  /* Sustain/release */
    WriteRegister(OPL_REGS_WAVEFORM + voice->op1, 0x00); /* Sine wave */

    WriteRegister(OPL_REGS_TREMOLO + voice->op2, 0x01);  /* Carrier: mult=1 */
    WriteRegister(OPL_REGS_LEVEL + voice->op2, 0x00);    /* Carrier full volume */
    WriteRegister(OPL_REGS_ATTACK + voice->op2, 0xF0);   /* Fast attack, no decay */
    WriteRegister(OPL_REGS_SUSTAIN + voice->op2, 0x77);  /* Sustain/release */
    WriteRegister(OPL_REGS_WAVEFORM + voice->op2, 0x00); /* Sine wave */

    WriteRegister(OPL_REGS_FEEDBACK + voice->index, 0x30); /* No feedback, FM mode, output L+R */

    voice->car_volume = 0x00;
    voice->mod_volume = 0x10;
    voice->priority = 10;
#else
    data = &instr->voices[instr_voice];
    modulating = (data->feedback & 0x01) == 0;

    LoadOperatorData(voice->op2, &data->carrier, true, &voice->car_volume);
    LoadOperatorData(voice->op1, &data->modulator, !modulating, &voice->mod_volume);

    WriteRegister(OPL_REGS_FEEDBACK + voice->index, data->feedback | 0x30);

    voice->priority = 0x0f - (data->carrier.attack >> 4)
                    + 0x0f - (data->carrier.sustain & 0x0f);
#endif
}

// Set voice volume
static void SetVoiceVolume(opl_voice_t *voice, unsigned int volume)
{
    genmidi_voice_t *opl_voice;
    unsigned int midi_volume;
    unsigned int full_volume;
    unsigned int car_volume;

    voice->note_volume = volume;
    opl_voice = &voice->current_instr->voices[voice->current_instr_voice];

    midi_volume = 2 * (volume_mapping_table[channels[voice->channel].volume] + 1);
    full_volume = (volume_mapping_table[voice->note_volume] * midi_volume) >> 9;
    car_volume = 0x3f - full_volume;

    if (car_volume != (voice->car_volume & 0x3f))
    {
        voice->car_volume = car_volume | (voice->car_volume & 0xc0);
        WriteRegister(OPL_REGS_LEVEL + voice->op2, voice->car_volume);
    }
}

// Get frequency for voice
static unsigned int FrequencyForVoice(opl_voice_t *voice)
{
    genmidi_voice_t *gm_voice;
    signed int freq_index;
    unsigned int octave;
    unsigned int sub_index;
    signed int note;

    note = voice->note;
    gm_voice = &voice->current_instr->voices[voice->current_instr_voice];

#if USE_SIMPLE_INSTRUMENT
    /* When using simple instrument, don't use GENMIDI offsets */
    (void)gm_voice;
#else
    if ((SHORT(voice->current_instr->flags) & GENMIDI_FLAG_FIXED) == 0)
    {
        note += (signed short)SHORT(gm_voice->base_note_offset);
    }
#endif

    while (note < 0) note += 12;
    while (note > 95) note -= 12;

    freq_index = 64 + 32 * note + channels[voice->channel].bend;

#if !USE_SIMPLE_INSTRUMENT
    if (voice->current_instr_voice != 0)
    {
        freq_index += (voice->current_instr->fine_tuning / 2) - 64;
    }
#endif

    if (freq_index < 0) freq_index = 0;

    if (freq_index < 284)
    {
        return frequency_curve[freq_index];
    }

    sub_index = (freq_index - 284) % (12 * 32);
    octave = (freq_index - 284) / (12 * 32);

    if (octave >= 7) octave = 7;

    return frequency_curve[sub_index + 284] | (octave << 10);
}

// Update voice frequency
static void UpdateVoiceFrequency(opl_voice_t *voice)
{
    unsigned int freq;

    freq = FrequencyForVoice(voice);

    if (voice->freq != freq)
    {
        WriteRegister(OPL_REGS_FREQ_1 + voice->index, freq & 0xff);
        WriteRegister(OPL_REGS_FREQ_2 + voice->index, (freq >> 8) | 0x20);
        voice->freq = freq;
    }
}

// Replace existing voice
static void ReplaceExistingVoice(void)
{
    int i, result = 0;

    for (i = 0; i < voice_alloced_num; i++)
    {
        if (voice_alloced_list[i]->current_instr_voice != 0
         || voice_alloced_list[i]->channel >= voice_alloced_list[result]->channel)
        {
            result = i;
        }
    }

    ReleaseVoice(result);
}

// Voice key on
static void VoiceKeyOn(int channel_num, genmidi_instr_t *instrument,
                       unsigned int instrument_voice,
                       unsigned int note, unsigned int key,
                       unsigned int volume)
{
    opl_voice_t *voice;

    voice = GetFreeVoice();
    if (voice == NULL)
    {
        return;
    }

    voice->channel = channel_num;
    voice->key = key;

    if ((SHORT(instrument->flags) & GENMIDI_FLAG_FIXED) != 0)
    {
        voice->note = instrument->fixed_note;
    }
    else
    {
        voice->note = note;
    }

    SetVoiceInstrument(voice, instrument, instrument_voice);
    SetVoiceVolume(voice, volume);

    voice->freq = 0;
    UpdateVoiceFrequency(voice);
}

// Get channel for event (handle MIDI/MUS percussion mapping)
static int GetChannelNum(unsigned int midi_channel)
{
    if (midi_channel == 9) return 15;
    if (midi_channel == 15) return 9;
    return midi_channel;
}

// Key off event
static void KeyOffEvent(midi_event_t *event)
{
    int channel_num;
    unsigned int key;
    int i;

    channel_num = GetChannelNum(event->data.channel.channel);
    key = event->data.channel.param1;

    for (i = 0; i < voice_alloced_num; i++)
    {
        if (voice_alloced_list[i]->channel == channel_num
         && voice_alloced_list[i]->key == key)
        {
            ReleaseVoice(i);
            i--;
        }
    }
}

// Key on event
static void KeyOnEvent(midi_event_t *event)
{
    genmidi_instr_t *instrument;
    int channel_num;
    unsigned int note, key, volume;
    boolean double_voice;

    note = event->data.channel.param1;
    key = event->data.channel.param1;
    volume = event->data.channel.param2;

    if (volume <= 0)
    {
        KeyOffEvent(event);
        return;
    }

    channel_num = GetChannelNum(event->data.channel.channel);

    // Percussion channel
    if (event->data.channel.channel == 9)
    {
        if (key < 35 || key > 81)
        {
            return;
        }
        instrument = &percussion_instrs[key - 35];
        note = 60;
    }
    else
    {
        instrument = channels[channel_num].instrument;
    }

    double_voice = (SHORT(instrument->flags) & GENMIDI_FLAG_2VOICE) != 0;

    if (voice_free_num == 0)
    {
        ReplaceExistingVoice();
    }

    VoiceKeyOn(channel_num, instrument, 0, note, key, volume);

    if (double_voice && voice_free_num > 0)
    {
        VoiceKeyOn(channel_num, instrument, 1, note, key, volume);
    }
}

// Program change event
static void ProgramChangeEvent(midi_event_t *event)
{
    int channel_num;
    int instrument;

    channel_num = GetChannelNum(event->data.channel.channel);
    instrument = event->data.channel.param1;
    channels[channel_num].instrument = &main_instrs[instrument];
}

// Controller event
static void ControllerEvent(midi_event_t *event)
{
    int channel_num;
    unsigned int controller, param;
    int i;

    channel_num = GetChannelNum(event->data.channel.channel);
    controller = event->data.channel.param1;
    param = event->data.channel.param2;

    switch (controller)
    {
        case MIDI_CONTROLLER_VOLUME_MSB:
            channels[channel_num].volume_base = param;
            if (param > music_volume)
            {
                param = music_volume;
            }
            channels[channel_num].volume = param;

            for (i = 0; i < OPL_NUM_VOICES; i++)
            {
                if (voices[i].channel == channel_num)
                {
                    SetVoiceVolume(&voices[i], voices[i].note_volume);
                }
            }
            break;

        case MIDI_CONTROLLER_ALL_NOTES_OFF:
            for (i = 0; i < voice_alloced_num; i++)
            {
                if (voice_alloced_list[i]->channel == channel_num)
                {
                    ReleaseVoice(i);
                    i--;
                }
            }
            break;
    }
}

// Pitch bend event
static void PitchBendEvent(midi_event_t *event)
{
    int channel_num;
    int i;

    channel_num = GetChannelNum(event->data.channel.channel);
    channels[channel_num].bend = event->data.channel.param2 - 64;

    for (i = 0; i < voice_alloced_num; i++)
    {
        if (voice_alloced_list[i]->channel == channel_num)
        {
            UpdateVoiceFrequency(voice_alloced_list[i]);
        }
    }
}

// Meta event (tempo changes)
static void MetaEvent(midi_event_t *event)
{
    byte *data = event->data.meta.data;
    unsigned int data_len = event->data.meta.length;

    if (event->data.meta.type == MIDI_META_SET_TEMPO && data_len == 3)
    {
        us_per_beat = (data[0] << 16) | (data[1] << 8) | data[2];
        samples_per_tick = ((uint64_t)sample_rate * us_per_beat)
                         / ((uint64_t)ticks_per_beat * 1000000);
        if (samples_per_tick == 0)
        {
            samples_per_tick = 1;
        }
    }
}

// Process event
static void ProcessEvent(midi_event_t *event)
{
    switch (event->event_type)
    {
        case MIDI_EVENT_NOTE_OFF:
            KeyOffEvent(event);
            break;
        case MIDI_EVENT_NOTE_ON:
            KeyOnEvent(event);
            break;
        case MIDI_EVENT_CONTROLLER:
            ControllerEvent(event);
            break;
        case MIDI_EVENT_PROGRAM_CHANGE:
            ProgramChangeEvent(event);
            break;
        case MIDI_EVENT_PITCH_BEND:
            PitchBendEvent(event);
            break;
        case MIDI_EVENT_META:
            MetaEvent(event);
            break;
        default:
            break;
    }
}

// Schedule next event
static void ScheduleTrack(int i)
{
    unsigned int delta_time;

    if (tracks[i] == NULL)
    {
        track_done[i] = true;
        return;
    }

    delta_time = MIDI_GetDeltaTime(tracks[i]);
    track_wait[i] = delta_time * samples_per_tick;
    if (track_wait[i] == 0 && delta_time > 0)
    {
        /* Prevent a stuck scheduler if samples_per_tick underflowed. */
        track_wait[i] = 1;
    }
}

// Restart song
static void RestartSong(void)
{
    int i;

    for (i = 0; i < num_tracks; i++)
    {
        if (tracks[i] != NULL)
        {
            MIDI_RestartIterator(tracks[i]);
        }
        track_done[i] = false;
        ScheduleTrack(i);
    }

    for (i = 0; i < 16; i++)
    {
        InitChannel(&channels[i]);
    }
}

// Initialize
boolean OPLMusic_Init(uint32_t samplerate)
{
    if (music_initialized)
    {
        return true;
    }

    sample_rate = samplerate;

    if (!LoadInstrumentTable())
    {
        return false;
    }

    InitRegisters();
    InitVoices();

    music_initialized = true;
    music_playing = false;
    music_paused = false;

    return true;
}

// Shutdown
void OPLMusic_Shutdown(void)
{
    if (!music_initialized)
    {
        return;
    }

    OPLMusic_StopSong();
    music_initialized = false;
}

// Register song
void *OPLMusic_RegisterSong(void *data, int len)
{
    midi_file_t *file = NULL;
    MEMFILE *instream;
    MEMFILE *outstream;
    void *outbuf;
    size_t outbuf_len;

    if (!music_initialized)
    {
        return NULL;
    }

    // Check if it's already MIDI
    if (len > 4 && memcmp(data, "MThd", 4) == 0)
    {
        file = MIDI_LoadFileBuffer(data, len);
    }
    else
    {
        // Assume MUS, convert to MIDI
        instream = mem_fopen_read(data, len);
        outstream = mem_fopen_write();

        if (mus2mid(instream, outstream) == 0)
        {
            mem_get_buf(outstream, &outbuf, &outbuf_len);
            file = MIDI_LoadFileBuffer(outbuf, outbuf_len);
        }

        mem_fclose(instream);
        mem_fclose(outstream);
    }

    return file;
}

// Unregister song
void OPLMusic_UnRegisterSong(void *handle)
{
    if (handle != NULL)
    {
        MIDI_FreeFile(handle);
    }
}

// Play song
void OPLMusic_PlaySong(void *handle, boolean looping)
{
    int i;

    if (!music_initialized || handle == NULL)
    {
        return;
    }

    OPLMusic_StopSong();

    current_file = handle;
    song_looping = looping;

    // One iterator per track: track 0 alone is a MUS lump's whole song and
    // a format 1 file's tempo track with no notes.
    num_tracks = (int)MIDI_NumTracks(current_file);
    if (num_tracks > MAX_TRACKS)
    {
        num_tracks = MAX_TRACKS;
    }
    {
        int i;

        for (i = 0; i < num_tracks; i++)
        {
            tracks[i] = MIDI_IterateTrack(current_file, (unsigned int)i);
            track_done[i] = tracks[i] == NULL;
        }
    }
    if (num_tracks == 0 || tracks[0] == NULL)
    {
        return;
    }

    // Setup timing
    ticks_per_beat = MIDI_GetFileTimeDivision(current_file);
    if (ticks_per_beat == 0) ticks_per_beat = 96;  /* Fallback to common value */

    us_per_beat = 500000;  // Default 120 BPM

    /* Calculate samples per tick with proper 64-bit math Formula:
     * samples_per_tick = sample_rate * us_per_beat / (ticks_per_beat * 1000000)
     * At 120 BPM with 96 ticks/beat: 49716 * 500000 / (96 * 1000000) = 259
     * samples/tick */
    samples_per_tick = ((uint64_t)sample_rate * us_per_beat) / ((uint64_t)ticks_per_beat * 1000000);

    /* Guard against zero to avoid infinite loops on bad headers. */
    if (samples_per_tick == 0)
    {
        samples_per_tick = 1;
    }

    // Initialize channels
    for (i = 0; i < 16; i++)
    {
        InitChannel(&channels[i]);
    }

    // Initialize voices
    InitVoices();

    {
        int i;

        for (i = 0; i < num_tracks; i++)
        {
            ScheduleTrack(i);
        }
    }

    music_playing = true;
    music_paused = false;
}

// Stop song
void OPLMusic_StopSong(void)
{
    int i;

    if (!music_initialized)
    {
        return;
    }

    // Release all voices
    while (voice_alloced_num > 0)
    {
        ReleaseVoice(0);
    }

    {
        int i;

        for (i = 0; i < num_tracks; i++)
        {
            if (tracks[i] != NULL)
            {
                MIDI_FreeIterator(tracks[i]);
                tracks[i] = NULL;
            }
        }
        num_tracks = 0;
    }

    current_file = NULL;
    music_playing = false;
}

// Pause
void OPLMusic_PauseSong(void)
{
    music_paused = true;
}

// Resume
void OPLMusic_ResumeSong(void)
{
    music_paused = false;
}

// Set volume
void OPLMusic_SetVolume(int volume)
{
    int i;

    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;

    music_volume = volume;

    if (!music_initialized)
    {
        return;
    }

    // Update all channels
    for (i = 0; i < 16; i++)
    {
        if (channels[i].volume_base > volume)
        {
            channels[i].volume = volume;
        }
        else
        {
            channels[i].volume = channels[i].volume_base;
        }
    }

    // Update all voices
    for (i = 0; i < OPL_NUM_VOICES; i++)
    {
        if (voices[i].channel >= 0)
        {
            SetVoiceVolume(&voices[i], voices[i].note_volume);
        }
    }
}

// Generate samples
int OPLMusic_GenerateSamples(int16_t *buffer, int num_samples)
{
    midi_event_t *event;
    int samples_generated = 0;
    int restarted = 0;
    int samples_to_generate;

    /* If music system not initialized, generate silence */
    if (!music_initialized)
    {
        memset(buffer, 0, num_samples * 2 * sizeof(int16_t));
        return num_samples;
    }

    /* If no track or not playing, generate silence */
    if (num_tracks == 0 || !music_playing || music_paused)
    {
        memset(buffer, 0, num_samples * 2 * sizeof(int16_t));
        return num_samples;
    }

    while (samples_generated < num_samples)
    {
        int i;
        int alive = 0;
        uint32_t min_wait = 0xFFFFFFFFu;

        for (i = 0; i < num_tracks; i++)
        {
            /* Every event of this track that is due now, with a cap so that
               a track of thousands of simultaneous events yields a sample. */
            int events_this_iteration = 0;
            #define MAX_EVENTS_PER_ITERATION 1024

            while (!track_done[i] && track_wait[i] == 0)
            {
                if (!MIDI_GetNextEvent(tracks[i], &event))
                {
                    track_done[i] = true;
                    break;
                }
                ProcessEvent(event);
                DG_TimeSpent(4, 1);
                if (event->event_type == MIDI_EVENT_META
                 && event->data.meta.type == MIDI_META_END_OF_TRACK)
                {
                    track_done[i] = true;
                    break;
                }
                ScheduleTrack(i);
                if (++events_this_iteration >= MAX_EVENTS_PER_ITERATION && track_wait[i] == 0)
                {
                    track_wait[i] = 1;
                }
            }
            if (!track_done[i])
            {
                alive = 1;
                if (track_wait[i] < min_wait)
                {
                    min_wait = track_wait[i];
                }
            }
        }

        if (!alive)
        {
            if (song_looping && !restarted)
            {
                restarted = 1;
                RestartSong();
                continue;
            }

            music_playing = false;
            memset(buffer + samples_generated * 2, 0,
                   (num_samples - samples_generated) * 2 * sizeof(int16_t));
            return num_samples;
        }

        // Generate samples until the nearest event on any track
        samples_to_generate = num_samples - samples_generated;
        if (min_wait < (uint32_t)samples_to_generate)
        {
            samples_to_generate = (int)min_wait;
        }
        if (samples_to_generate <= 0)
        {
            samples_to_generate = 1;
        }

        OPL3_GenerateStream(&opl_chip, buffer + samples_generated * 2, samples_to_generate);
        DG_TimeSpent(2, 1);
        DG_TimeSpent(3, (uint32_t)samples_to_generate);

        samples_generated += samples_to_generate;
        restarted = 0;
        for (i = 0; i < num_tracks; i++)
        {
            if (!track_done[i])
            {
                track_wait[i] = track_wait[i] > (uint32_t)samples_to_generate
                                    ? track_wait[i] - (uint32_t)samples_to_generate
                                    : 0;
            }
        }
    }

    return samples_generated;
}

// Check if playing
boolean OPLMusic_IsPlaying(void)
{
    return music_playing && !music_paused;
}
