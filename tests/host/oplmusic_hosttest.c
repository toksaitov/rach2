/**
 * The Doom port's OPL music player over a MIDI file of two tracks
 * (ports/doom/i_oplmusic.c).
 *
 * host-deps: ports/doom/i_oplmusic.c ports/doom/midifile.c
 * host-deps: ports/doom/mus2mid.c ports/doom/opl3.c ports/doom/src/memio.c
 * host-cflags: -I ports/doom -I ports/doom/src -DNORMALUNIX
 * host-cflags: -Wno-implicit-function-declaration
 *
 * A MUS lump converts to a MIDI file of one track, and every Doom WAD this
 * port was written for ships MUS. Freedoom ships format 1 MIDI, whose track
 * 0 holds the tempo and no notes and whose notes are on the tracks after
 * it, and a player that read track 0 alone reached the end of the track at
 * once, restarted the song, and sounded nothing while the loop took the
 * processor: on the notebook one frame a second, with the emulator taking
 * fourteen seconds of every twenty.
 *
 * The file here is that shape, built in memory: track 0 a tempo and an end,
 * track 1 four note events a beat apart. The player must take the notes
 * from track 1, generate the samples asked for, and do it in a few calls to
 * the emulator rather than one call per sample.
 *
 * `DG_TimeSpent` is the port's own counter hook, which the platform file
 * fills in on the machine; here it is the test's window into the player.
 * The instrument table is the one lump the player reads from the WAD, and
 * the stub answers with a table of zeros, which is enough to sound a note.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int printf(const char *format, ...);

#include "i_oplmusic.h"

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/* What the player counts through the port's hook: the emulator's calls, the
 * samples it made, and the events the tracks yielded. */
static unsigned int calls, samples, events;

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

/* The zone allocator the port's memory files use, here the host's. */
void *Z_Malloc(int size, int tag, void *user)
{
    (void)tag;
    (void)user;
    return malloc((size_t)size);
}

void Z_Free(void *ptr)
{
    free(ptr);
}

/* GENMIDI: the eight-byte header the player skips, then 175 instruments of
 * 36 bytes. Zeros are a legal instrument: every operator is silent, which
 * the rows here do not depend on. */
static unsigned char genmidi[8 + 175 * 36];

void *W_CacheLumpName(char *name, int tag)
{
    (void)name;
    (void)tag;
    memcpy(genmidi, "#OPL_II#", 8);
    return genmidi;
}

/* ---- the file ---- */

static unsigned char midi[256];
static int midi_len;

static void put(const unsigned char *bytes, int n)
{
    memcpy(midi + midi_len, bytes, (size_t)n);
    midi_len += n;
}

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

/* A format 1 file: two tracks, 96 ticks to the beat. */
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

int main(void)
{
    static int16_t buffer[4096 * 2];
    void *song;
    int made;

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

    /* 4096 samples are 82 ms at this rate and a beat is half a second, so
       the first call carries the tempo, the end of track 0 and the note
       that track 1 opens with, and no more. */
    calls = 0;
    samples = 0;
    events = 0;
    made = OPLMusic_GenerateSamples(buffer, 4096);
    CHECK("a call for 4096 samples makes them", made == 4096);
    CHECK("the first note of the second track is taken with the tempo track", events == 3);
    CHECK("the emulator is asked for runs of samples, not one at a time",
          calls > 0 && calls <= 4 && samples == 4096);

    /* Two seconds, which covers the four note events and the end of the
       second track. A player that read track 0 alone would have stopped at
       its end above and answered silence from here. */
    {
        int i;

        for (i = 0; i < 24; i++) {
            made = OPLMusic_GenerateSamples(buffer, 4096);
            CHECK("every later call fills the buffer too", made == 4096);
        }
    }
    CHECK("both notes are sounded and stopped, and the track ends", events >= 7);
    CHECK("in a few calls to the emulator for each buffer", calls <= 4 * 25);
    /* The emulator is asked for the song's own length, three beats of it,
       and not for the silence after the end, which is a memset. */
    CHECK("with the song's own length generated and no more",
          samples >= 70000 && samples <= 4096 * 25);
    CHECK("and the song, which does not loop, has stopped", OPLMusic_IsPlaying() == 0);

    OPLMusic_UnRegisterSong(song);
    OPLMusic_Shutdown();
    if (fails == 0) {
        printf("oplmusic_hosttest: ok\n");
    }
    return fails > 100 ? 100 : fails;
}
