//
// i_oplmusic.h - OPL3 music playback for Doom on Rach2
//
// Simplified OPL music driver using Nuked-OPL3 emulator
//

#ifndef I_OPLMUSIC_H
#define I_OPLMUSIC_H

#include "doomtype.h"
#include <stdint.h>

// Initialize OPL music subsystem
boolean OPLMusic_Init(uint32_t samplerate);

// Shutdown OPL music subsystem
void OPLMusic_Shutdown(void);

// Register a song (MUS or MIDI data from WAD)
// Returns handle or NULL on failure
void *OPLMusic_RegisterSong(void *data, int len);

// Unregister a song
void OPLMusic_UnRegisterSong(void *handle);

// Start playing a song
void OPLMusic_PlaySong(void *handle, boolean looping);

// Stop playing
void OPLMusic_StopSong(void);

// Pause/resume
void OPLMusic_PauseSong(void);
void OPLMusic_ResumeSong(void);

// Set volume (0-127)
void OPLMusic_SetVolume(int volume);

// Generate audio samples (16-bit stereo, interleaved)
// Returns number of stereo sample pairs generated
int OPLMusic_GenerateSamples(int16_t *buffer, int num_samples);

// Check if music is playing
boolean OPLMusic_IsPlaying(void);

#endif // I_OPLMUSIC_H
