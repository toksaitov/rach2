/*
 * audio_proc.h: the text of /proc/audio (audio_proc.c), which procfs serves.
 */

#ifndef _AUDIO_PROC_H_
#define _AUDIO_PROC_H_

/**
 * Write /proc/audio's text into `buf`, of `cap` bytes, with no terminator,
 * and return its whole length, which is more than `cap` when the buffer was
 * short. It sends no verb to a codec.
 */
int audio_proc_text(char *buf, int cap);

#endif
