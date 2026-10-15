//
// i_oplmusic.h: the music player of Doom on Rach2, which plays the game's
// MUS and MIDI songs on the Nuked OPL3 emulator with the instruments of the
// WAD's GENMIDI lump. The sound module of i_pcsound.c starts it and asks it
// for samples; it keeps time by the samples it has made.
//

#ifndef I_OPLMUSIC_H
#define I_OPLMUSIC_H

#include "doomtype.h"
#include <stdint.h>

/**
 * Start the player, making samples at `samplerate` Hz, with the WAD's
 * GENMIDI lump. Returns true when the player started or had started, and
 * false when the lump is missing, short or without its `#OPL_II#` header.
 */
boolean OPLMusic_Init(uint32_t samplerate);

/** Stop the song and the player; OPLMusic_Init starts it again. */
void OPLMusic_Shutdown(void);

/**
 * Read the song lump `data`, of `len` bytes: as MIDI when it begins with
 * `MThd`, and otherwise as MUS, which mus2mid converts. Returns a handle for
 * OPLMusic_PlaySong, which OPLMusic_UnRegisterSong frees, or NULL when the
 * player is not started or the song cannot be read.
 */
void *OPLMusic_RegisterSong(void *data, int len);

/** Free the song `handle`; NULL is ignored. A song that plays must be
 * stopped first, since the player reads it. */
void OPLMusic_UnRegisterSong(void *handle);

/**
 * Stop the song that plays and play `handle` from its start, and from its
 * start again at its end when `looping`. The tempo is 120 beats a minute
 * until the song sets one. Does nothing when the player is not started or
 * `handle` is NULL, and plays nothing when the song's first track cannot
 * be read.
 */
void OPLMusic_PlaySong(void *handle, boolean looping);

/** Stop the song and release every voice; the handle stays registered. */
void OPLMusic_StopSong(void);

/** Pause and resume the song: while it is paused, OPLMusic_GenerateSamples
 * gives silence and runs no event. */
void OPLMusic_PauseSong(void);
void OPLMusic_ResumeSong(void);

/** Set the music's volume, `volume` 0 to 127, clamped; a channel plays at
 * its own volume, no more than this. */
void OPLMusic_SetVolume(int volume);

/**
 * Fill `buffer` with `num_samples` stereo pairs of 16-bit samples, the left
 * first, running the song's events as their time comes. Gives silence when
 * the player is not started, no song plays, or the song is paused or has
 * ended. Returns `num_samples`.
 */
int OPLMusic_GenerateSamples(int16_t *buffer, int num_samples);

/** Whether a song plays and is not paused. */
boolean OPLMusic_IsPlaying(void);

#endif // I_OPLMUSIC_H
