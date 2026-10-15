/**
 * Exercise MIDI validation, instrument loading and OPL playback scheduling.
 *
 * host-deps: ports/doom/i_oplmusic.c ports/doom/midifile.c
 * host-deps: ports/doom/mus2mid.c ports/doom/opl3.c ports/doom/src/memio.c
 * host-cflags: -I ports/doom -I ports/doom/src -DNORMALUNIX
 * host-cflags: -Wno-implicit-function-declaration
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "i_oplmusic.h"
#include "midifile.h"

static int fails;

#define CHECK(what, cond)                                           \
    do {                                                            \
        if (!(cond)) {                                              \
            fprintf(stderr, "oplmusic_hosttest: fail: %s\n", what); \
            fails++;                                                \
        }                                                           \
    } while (0)

static unsigned int calls, samples, events; /* counted by DG_TimeSpent */

/** Count emulator calls, generated samples and track events. */
void DG_TimeSpent(int what, uint32_t n)
{
    if (what == 2) {
        calls += n;
    } else if (what == 3) {
        samples += n;
    } else if (what == 4) {
        events += n;
    }
}

/** Allocate memory-file storage through the host allocator. */
void *Z_Malloc(int size, int tag, void *user)
{
    (void)tag;
    (void)user;
    return malloc((size_t)size);
}

/** Free memory-file storage through the host allocator. */
void Z_Free(void *ptr)
{
    free(ptr);
}

/* The GENMIDI lump: an 8-byte header and 175 instruments of 36 bytes. A
 * row sets the length the WAD declares for it, whether it is present, and
 * whether caching it fails. */
static unsigned char genmidi[8 + 175 * 36];
static int lump_length = sizeof(genmidi);
static int lump_present = 1;
static int cache_failure;
static int caches, releases;

/** Find the single instrument lump when present. */
int W_CheckNumForName(char *name)
{
    CHECK("the instrument lump is requested", strcmp(name, "genmidi") == 0);
    return lump_present ? 0 : -1;
}

/** Return the declared length, which may describe an incomplete table. */
int W_LumpLength(unsigned int lumpnum)
{
    CHECK("the instrument lump length is requested", lumpnum == 0);
    return lump_length;
}

/** Return instrument storage, or simulate a failed cache allocation. */
void *W_CacheLumpNum(int lumpnum, int tag)
{
    (void)tag;
    CHECK("the instrument lump is cached", lumpnum == 0);
    caches++;
    return cache_failure ? NULL : genmidi;
}

/** Count a rejected header's cache release. */
void W_ReleaseLumpNum(int lumpnum)
{
    CHECK("the instrument lump is released", lumpnum == 0);
    releases++;
}

/** Reject absent, truncated, uncached and incorrectly identified tables. */
static void check_instruments(void)
{
    static const int lengths[] = {0, 7, 8, 8 + 128 * 36, sizeof(genmidi) - 1};
    unsigned int i;

    memcpy(genmidi, "#OPL_II#", 8);
    lump_present = 0;
    CHECK("a missing instrument table is rejected", !OPLMusic_Init(49716));
    OPLMusic_Shutdown();
    CHECK("a missing instrument table is not cached", caches == 0);
    lump_present = 1;
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        caches = 0;
        lump_length = lengths[i];
        CHECK("a truncated instrument table is rejected", !OPLMusic_Init(49716));
        OPLMusic_Shutdown();
        CHECK("a truncated instrument table is not cached", caches == 0);
    }
    lump_length = sizeof(genmidi);
    cache_failure = 1;
    CHECK("a cache allocation failure is rejected", !OPLMusic_Init(49716));
    cache_failure = 0;
    genmidi[0] = '!';
    releases = 0;
    CHECK("an invalid instrument header is rejected", !OPLMusic_Init(49716));
    OPLMusic_Shutdown();
    CHECK("an invalid header releases the cached lump", releases == 1);
    genmidi[0] = '#';
}

static unsigned char midi[256]; /* the MIDI file a row builds */
static int midi_len;

/** Append `n` bytes to `midi`. */
static void put(const unsigned char *bytes, int n)
{
    memcpy(midi + midi_len, bytes, (size_t)n);
    midi_len += n;
}

/** Append a track chunk of `n` bytes of events to `midi`. */
static void put_track(const unsigned char *events_bytes, int n)
{
    unsigned char header[8];

    header[0] = 'M';
    header[1] = 'T';
    header[2] = 'r';
    header[3] = 'k';
    header[4] = (unsigned char)(n >> 24);
    header[5] = (unsigned char)(n >> 16);
    header[6] = (unsigned char)(n >> 8);
    header[7] = (unsigned char)n;
    put(header, 8);
    put(events_bytes, n);
}

/** Build in `midi` a format 1 file: two tracks, 96 ticks to the beat. */
static void build_midi(void)
{
    static const unsigned char header[14] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0, 96};
    /* Track 0: the tempo, half a second to the beat, and the end. */
    static const unsigned char tempo_track[11] = {0,    0xFF, 0x51, 3,    0x07, 0xA1,
                                                  0x20, 0,    0xFF, 0x2F, 0};
    /* Track 1: two notes, each held a beat, then the end. */
    static const unsigned char note_track[19] = {
        0,    0x90, 0x3C, 0x40, /* note on, middle C */
        0x60, 0x80, 0x3C, 0x40, /* a beat later, off */
        0x60, 0x90, 0x40, 0x40, /* a beat later, another note on */
        0x60, 0x80, 0x40, 0x40, /* a beat later, off */
        0,    0xFF, 0x2F        /* the end, whose length byte follows */
    };

    midi_len = 0;
    put(header, 14);
    put_track(tempo_track, 11);
    {
        unsigned char track[20];

        memcpy(track, note_track, 19);
        track[19] = 0; /* the end-of-track meta event's length */
        put_track(track, 20);
    }
}

/** Build a single-track file at the requested division. */
static void build_track(const unsigned char *bytes, int length, unsigned int division)
{
    unsigned char header[14] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 0};

    header[12] = (unsigned char)(division >> 8);
    header[13] = (unsigned char)division;
    midi_len = 0;
    put(header, sizeof(header));
    put_track(bytes, length);
}

/** Every channel parameter is seven bits, including single-parameter events. */
static void check_parameters(void)
{
    static const unsigned char statuses[] = {0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0};
    static const unsigned char values[] = {0, 127, 128, 255};
    unsigned int status, param, value;

    for (status = 0; status < sizeof(statuses); status++) {
        int count = statuses[status] == 0xc0 || statuses[status] == 0xd0 ? 1 : 2;
        unsigned char track[8] = {0, statuses[status], 0, 0, 1, 0xff, 0x2f, 0};
        int end = 2 + count;

        track[end] = 1;
        track[end + 1] = 0xff;
        track[end + 2] = 0x2f;
        track[end + 3] = 0;
        for (param = 0; param < (unsigned int)count; param++) {
            for (value = 0; value < sizeof(values); value++) {
                midi_file_t *file;

                track[2 + param] = values[value];
                build_track(track, end + 4, 96);
                file = MIDI_LoadFileBuffer(midi, midi_len);
                CHECK("channel parameters must be seven bits",
                      (file != NULL) == (values[value] < 128));
                if (file != NULL) {
                    MIDI_FreeFile(file);
                }
            }
            track[2 + param] = 0;
        }
    }
}

/** Count generated samples and events for one complete single-track song. */
static void check_duration(const unsigned char *track, int length, unsigned int division,
                           unsigned int expected_samples)
{
    int16_t buffer[1024 * 2];
    void *song;

    build_track(track, length, division);
    song = OPLMusic_RegisterSong(midi, midi_len);
    CHECK("a timing test song is registered", song != NULL);
    if (song == NULL) {
        return;
    }
    OPLMusic_PlaySong(song, 0);
    calls = samples = events = 0;
    CHECK("the timing buffer is filled", OPLMusic_GenerateSamples(buffer, 1024) == 1024);
    CHECK("tempo preserves the expected sample duration", samples == expected_samples);
    CHECK("the timing test song ends", !OPLMusic_IsPlaying());
    OPLMusic_StopSong();
    OPLMusic_UnRegisterSong(song);
}

/** Retain 64-bit tempo products and at least one sample per tick. */
static void check_tempo(void)
{
    static const unsigned char initial[] = {0, 0x90, 60, 64, 2, 0xff, 0x2f, 0};
    static const unsigned char unchanged[] = {0,    0xff, 0x51, 3, 7,    0xa1, 0x20, 0,
                                              0x90, 60,   64,   2, 0xff, 0x2f, 0};
    static const unsigned char minimum[] = {0,    0xff, 0x51, 3, 0,    0,    1, 0,
                                            0x90, 60,   64,   2, 0xff, 0x2f, 0};

    check_duration(initial, sizeof(initial), 4800, 10);
    check_duration(unchanged, sizeof(unchanged), 4800, 10);
    check_duration(minimum, sizeof(minimum), 4800, 2);
}

/** Repeat songs across and within buffers; stop zero-duration loops. */
static void check_loops(void)
{
    unsigned char track[] = {0, 0x90, 60, 64, 1, 0xff, 0x2f, 0};
    int16_t buffer[800 * 2];
    void *song;
    int i;

    build_track(track, sizeof(track), 96);
    song = OPLMusic_RegisterSong(midi, midi_len);
    CHECK("a looping song is registered", song != NULL);
    if (song == NULL) {
        return;
    }
    OPLMusic_PlaySong(song, 1);
    calls = samples = events = 0;
    OPLMusic_GenerateSamples(buffer, 258);
    CHECK("the first loop reaches the buffer boundary", events == 1 && samples == 258);
    OPLMusic_GenerateSamples(buffer, 258);
    CHECK("the boundary replays the delta-zero note", events == 3 && samples == 516);
    CHECK("the boundary loop remains playing", OPLMusic_IsPlaying());
    OPLMusic_PlaySong(song, 1);
    calls = samples = events = 0;
    OPLMusic_GenerateSamples(buffer, 800);
    CHECK("one buffer can contain several loops", events == 7 && samples == 800);
    CHECK("several loops remain playing", OPLMusic_IsPlaying());
    OPLMusic_StopSong();
    OPLMusic_UnRegisterSong(song);

    track[4] = 0;
    for (i = 0; i < 2; i++) {
        build_track(track + i * 4, sizeof(track) - i * 4, 96);
        song = OPLMusic_RegisterSong(midi, midi_len);
        CHECK("a zero-duration song is registered", song != NULL);
        if (song == NULL) {
            continue;
        }
        OPLMusic_PlaySong(song, 1);
        calls = samples = events = 0;
        memset(buffer, 1, sizeof(buffer));
        OPLMusic_GenerateSamples(buffer, 800);
        CHECK("a zero-duration loop stops", !OPLMusic_IsPlaying());
        CHECK("a zero-duration loop does not run the emulator", samples == 0 && events <= 4);
        for (unsigned int j = 0; j < sizeof(buffer) / sizeof(buffer[0]); j++) {
            CHECK("a zero-duration loop returns silence", buffer[j] == 0);
        }
        OPLMusic_StopSong();
        OPLMusic_UnRegisterSong(song);
    }
}

/**
 * Run the validation cases and the two-track playback regression. The exit
 * status is the count of failed checks, at most 100.
 *
 * At 49716 Hz, 4096 samples are 82 ms and a beat is half a second, so the
 * first call carries the tempo, the end of track 0 and the note that track
 * 1 opens with, and no more. Then two seconds cover the four note events
 * and the end of the second track; a player that read track 0 alone would
 * have stopped at its end and given silence from there. The emulator is
 * called for the song's own length, three beats of it, and not for the
 * silence after the end, which is a memset.
 */
int main(void)
{
    static int16_t buffer[4096 * 2];
    void *song;
    int made;

    check_parameters();
    check_instruments();
    build_midi();
    CHECK("the player starts", OPLMusic_Init(49716) == 1);
    song = OPLMusic_RegisterSong(midi, midi_len);
    CHECK("a two-track MIDI file is registered", song != 0);
    if (song == 0) {
        printf("oplmusic_hosttest: %d failure(s)\n", fails + 1);
        return 1;
    }
    OPLMusic_PlaySong(song, 0);
    CHECK("and is playing", OPLMusic_IsPlaying() == 1);

    calls = 0;
    samples = 0;
    events = 0;
    made = OPLMusic_GenerateSamples(buffer, 4096);
    CHECK("a call for 4096 samples makes them", made == 4096);
    CHECK("the first note of the second track is taken with the tempo track", events == 3);
    CHECK("the emulator is asked for runs of samples, not one at a time",
          calls > 0 && calls <= 4 && samples == 4096);

    {
        int i;

        for (i = 0; i < 24; i++) {
            made = OPLMusic_GenerateSamples(buffer, 4096);
            CHECK("every later call fills the buffer too", made == 4096);
        }
    }
    CHECK("both notes are sounded and stopped, and the track ends", events >= 7);
    CHECK("in a few calls to the emulator for each buffer", calls <= 4 * 25);
    CHECK("with the song's own length generated and no more",
          samples >= 70000 && samples <= 4096 * 25);
    CHECK("and the song, which does not loop, has stopped", OPLMusic_IsPlaying() == 0);

    OPLMusic_UnRegisterSong(song);
    check_tempo();
    check_loops();
    OPLMusic_Shutdown();
    if (fails == 0) {
        printf("oplmusic_hosttest: ok\n");
    }
    return fails > 100 ? 100 : fails;
}
