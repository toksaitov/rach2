/*
 * Kernel shell commands that exercise the audio path and report device
 * state, such as the HDA stream's LPIB link-position and DMA frame counts.
 */

#ifndef _AUDIO_DEBUG_H_
#define _AUDIO_DEBUG_H_

#include "audio.h"

bool audio_debug_handle_command(const char *cmd);
void audio_debug_print_help(void);

#endif /* _AUDIO_DEBUG_H_ */
