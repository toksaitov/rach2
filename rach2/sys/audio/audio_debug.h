/*
 * Kernel shell commands that exercise the audio path and report device
 * state, such as the HDA stream's LPIB link-position and DMA frame counts.
 */

#ifndef _AUDIO_DEBUG_H_
#define _AUDIO_DEBUG_H_

#include "audio.h"

/**
 * Run the kernel shell's `audio` command if `cmd` is one, a line whose first
 * word is `audio`. Returns true when it was (the command ran, or its usage was
 * printed), false when the line belongs to another command.
 */
bool audio_debug_handle_command(const char *cmd);

/** Print the forms of the `audio` command on the console. */
void audio_debug_print_help(void);

#endif /* _AUDIO_DEBUG_H_ */
