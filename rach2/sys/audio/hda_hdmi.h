/*
 * hda_hdmi.h: the HD Audio controller of Haswell's display and its HDMI
 * codec, which plays on the monitors of the Intel display driver's HDMI and
 * DisplayPort outputs, as Linux's snd_hda_intel and patch_hdmi.c with
 * i915's audio component.
 */
#ifndef _HDA_HDMI_H_
#define _HDA_HDMI_H_

#include "audio.h"

/* The OSS device the HDMI codec plays through, /dev/dsp1, at Linux's
 * numbers for the second card's dsp. */
#define HDMI_DSP_MINOR 19

/* The stream operations of the HDMI codec, which /dev/dsp1 opens. */
extern audio_driver_t hdmi_audio_driver;

/**
 * Find Haswell's display audio controller, 8086:0a0c, 0c0c or 0d0c, which
 * hda.c leaves alone, and wait for the display driver: the controller and
 * codec come up once the audio component binds them (acomp.h), since they
 * need the display's power. Nothing on a machine without one. At boot,
 * before the display driver starts.
 */
void hdmi_audio_init(void);

/** Whether this driver, not hda.c, drives the PCI function `vendor`:`device`:
 * Haswell's display audio controllers. */
int hdmi_audio_drives(uint16_t vendor, uint16_t device);

/** Serve the running stream from the timer tick: refill the periods the
 * controller has played. No lock held, interrupts off. */
void hdmi_audio_tick(void);

/** The ticks the tick may sleep before `hdmi_audio_tick` must run: 1 while
 * the stream runs, TICK_HZ otherwise. */
uint32_t hdmi_audio_idle_ticks(void);

/** Whether the card is there, for /proc/asound: 1 once the codec came up,
 * until the display goes. */
int hdmi_audio_present(void);

/** The ELD files the card has, eld#C.0 to eld#C.(n-1), one for each
 * device entry of each HDMI or DisplayPort pin, as Linux's patch_hdmi.c
 * makes them; 0 when the card is not there. */
int hdmi_audio_eld_count(void);

/** The codec's address on the link, the C of eld#C.N. */
int hdmi_audio_codec_addr(void);

/**
 * Write /proc/asound's text into `buf`, at most `cap` bytes, and return its
 * length; with `buf` NULL only measure it. `file` 0 is /proc/asound/cards,
 * 1 the card's `id`, and 2 + N its eld#C.N, in Linux's
 * snd_hdmi_print_eld_info's form. -ENOENT for a file the card lacks.
 */
int hdmi_audio_proc_text(int file, char *buf, int cap);

#endif /* _HDA_HDMI_H_ */
