/*
 * hda_hdmi.h: the HD Audio controller of Haswell's display and its HDMI
 * codec, which plays on the monitors of the Intel display driver's HDMI and
 * DisplayPort outputs, as Linux's snd_hda_intel and patch_hdmi.c with
 * i915's audio component.
 */
#ifndef _HDA_HDMI_H_
#define _HDA_HDMI_H_

#include "audio.h"

/* The OSS devices of the HDMI codec's PCMs: /dev/dsp1 and /dev/adsp1, at
 * Linux's numbers for the second card's dsp and adsp, then /dev/dsp2 and
 * /dev/dsp3, at its numbers for the next cards' dsp, which no other device
 * of Rach2 takes. */
#define HDMI_DSP_MINOR  19
#define HDMI_ADSP_MINOR 28
#define HDMI_DSP2_MINOR 35
#define HDMI_DSP3_MINOR 51

/**
 * Find Haswell's display audio controller, 8086:0a0c, 0c0c or 0d0c, which
 * hda.c leaves alone, and wait for the display driver: the controller and
 * codec come up once the audio component binds them (acomp.h), since they
 * need the display's power. Nothing on a machine without one. At boot,
 * before the display driver starts.
 */
void hdmi_audio_init(void);

/* Whether this driver, not hda.c, drives the PCI function `vendor`:`device`:
 * Haswell's display audio controllers. A macro, so that hda.c and its host
 * tests need nothing of this driver at the link. */
#define HDMI_AUDIO_DRIVES(vendor, device) \
    ((vendor) == 0x8086 && ((device) == 0x0a0c || (device) == 0x0c0c || (device) == 0x0d0c))

/** Serve the running streams from the timer tick: refill the periods the
 * controller has played. No lock held, interrupts off. */
void hdmi_audio_tick(void);

/** The ticks the tick may sleep before `hdmi_audio_tick` must run: 1 while
 * a stream runs, TICK_HZ otherwise. */
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
 * Write the HDMI card's text of /proc/asound into `buf`, at most `cap`
 * bytes, and return its length; with `buf` NULL only measure it. `file` 0
 * is the card's two lines of /proc/asound/cards, numbered `index`; 1 its
 * `id`; and 2 + N its eld#C.N, in Linux's snd_hdmi_print_eld_info's form.
 * -ENOENT while the card is not there, or for a file it lacks.
 */
int hdmi_audio_proc_text(int file, int index, char *buf, int cap);

/**
 * Apply `len` bytes written to eld#C.N, N being `n`, as Linux's
 * snd_hdmi_write_eld_info does at the file's close: each line of a field's
 * name and a hex value sets that field of the entry's ELD, which the next
 * read and the next open of the entry's PCM see; the monitor's name, IDs and
 * versions are not set. 0, or -ENOENT while the card or the entry is not
 * there. Process context; takes the driver's mutex.
 */
int hdmi_audio_eld_write(int n, const char *text, int len);

#endif /* _HDA_HDMI_H_ */
