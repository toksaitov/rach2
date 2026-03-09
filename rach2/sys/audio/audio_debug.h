/*
 * audio_debug.h - Kernel shell audio debug helpers
 */

#ifndef _AUDIO_DEBUG_H_
#define _AUDIO_DEBUG_H_

#include "audio.h"

bool audio_debug_handle_command(const char *cmd);
void audio_debug_print_help(void);

#endif /* _AUDIO_DEBUG_H_ */
