/*
 * asound.h: /proc/asound, Linux's ALSA card list and card directories, over
 * the analog HD Audio card (hda.c) and the HDMI codec's (hda_hdmi.c).
 */

#ifndef _ASOUND_H_
#define _ASOUND_H_

/*
 * A file of /proc/asound is numbered ASOUND_FILE(card, f): f ASOUND_CARDS,
 * with card 0, for /proc/asound/cards; ASOUND_ID for cardK/id; and
 * ASOUND_ELD + N for the HDMI card's eld#C.N. The cards are numbered as
 * Linux's OSS emulation ties /dev/dspK to card K: the analog card 0, while
 * hda.c drives one, and the HDMI card after it.
 */
#define ASOUND_PER_CARD      64
#define ASOUND_FILE(card, f) ((card) * ASOUND_PER_CARD + (f))
#define ASOUND_CARDS         0
#define ASOUND_ID            1
#define ASOUND_ELD           2

/** The cards there now, 0 to 2. */
int asound_card_count(void);

/** The file named by the `len` bytes of `name` in card `card`'s directory,
 * or -1 for none. */
int asound_lookup(int card, const char *name, int len);

/**
 * The name of file `file` within its card's directory, or "cards" for
 * ASOUND_CARDS, into `name` of `cap` bytes, NUL-terminated. 0, or -1 for a
 * file the cards lack now or a name that does not fit.
 */
int asound_name(int file, char *name, int cap);

/** The `n`th file of card `card`'s directory, its id first and then its
 * ELD files, or -1 past the last. */
int asound_entry(int card, int n);

/**
 * Write file `file`'s text into `buf`, at most `cap` bytes, and return its
 * length; with `buf` NULL only measure it. -ENOENT for a file the cards
 * lack now. Process context, since the HDMI card's text takes its mutex.
 */
int asound_text(int file, char *buf, int cap);

/** The mode of file `file`: 0644 for an ELD file, which root may write,
 * as Linux's eld_proc_new sets it, 0444 for the others. */
int asound_mode(int file);

/** Apply the `len` bytes written to the ELD file `file`, at its close.
 * 0, -ENOENT for a file that is gone, or -EACCES for one not writable. */
int asound_write(int file, const char *text, int len);

#endif /* _ASOUND_H_ */
