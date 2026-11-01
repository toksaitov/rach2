/**
 * wavcheck: check the notes a machine played, from the recording QEMU made.
 *
 * QEMU's `wav` audio backend writes every sample the guest produced to a file,
 * so a boot that plays a scale leaves one behind and the notes can be measured
 * afterwards, outside the guest, where nothing in the guest can change them.
 * That is how the audio path is checked end to end: the driver's own report
 * says what it was asked to play, not what came out.
 *
 * `wavcheck scale FILE FREQ [FREQ ...]` sweeps the recording of a scale of
 * square waves, such as the debug shell's `audio test scale` plays, in short
 * windows, measures the frequency in each, and requires each measurement to
 * be one of the named frequencies and the measurements to visit every one of
 * them, in the order they were named, in every channel of the recording.
 * `wavcheck scale FILE FREQ [FREQ ...] / FREQ [FREQ ...]` requires the
 * frequencies before the slash of the first channel and those after it of
 * the second, so that two lists with no frequency in common prove the
 * channels distinct: a recording of one channel, or of a device that mixed
 * the two, fails. More than 16 frequencies in a list are refused.
 *
 * Exit 0 when every note sounds in its turn, 1 when one does not, another
 * tone sounds, or the recording is too short, and 2 when the file cannot be
 * read or the arguments are invalid.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTES 16

/* A sample within QUIET of zero is silence, and belongs to no run of the
   wave. A measured frequency within TOLERANCE of a named one, as a fraction
   of it, is that note; the closest notes of a scale differ by 6%. */
#define QUIET     256
#define TOLERANCE 0.03

/* A RIFF/WAVE header, as QEMU writes it: 16-bit samples, one or two channels,
   and the data chunk last. Only what is needed to find the samples is read.
   `data_at` is the file offset of the samples and `data_len` the length the
   header gives them, in bytes. */
struct wav {
    unsigned rate;
    unsigned channels;
    unsigned bits;
    long data_at;
    long data_len;
};

/** The little-endian 16-bit value at `p`. */
static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

/** The little-endian 32-bit value at `p`. */
static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

/**
 * Open the WAVE file at `path` and read its header into `*w`, leaving `*out`
 * open after the header.
 *
 * The chunks are read up to the data chunk. A chunk of length zero ends the
 * search: it is a header QEMU did not go back to complete, which is what a
 * recording from a machine that was stopped rather than shut down has. The
 * samples must be 16-bit with one to eight channels, since main's frame
 * buffer holds eight. Returns 0, or 2 having said why the file is refused.
 */
static int wav_open(const char *path, FILE **out, struct wav *w)
{
    unsigned char hdr[12];
    FILE *f = fopen(path, "rb");

    memset(w, 0, sizeof(*w));
    if (f == NULL) {
        fprintf(stderr, "wavcheck: %s: cannot be read\n", path);
        return 2;
    }
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "wavcheck: %s: not a WAVE file\n", path);
        fclose(f);
        return 2;
    }
    w->data_at = 0;
    for (;;) {
        unsigned char ch[8];
        unsigned long len;

        if (fread(ch, 1, 8, f) != 8) {
            break;
        }
        len = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            unsigned char fmt[16];

            if (len < 16 || fread(fmt, 1, 16, f) != 16) {
                break;
            }
            w->channels = rd16(fmt + 2);
            w->rate = (unsigned)rd32(fmt + 4);
            w->bits = rd16(fmt + 14);
            if (len > 16) {
                fseek(f, (long)(len - 16), SEEK_CUR);
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            w->data_at = ftell(f);
            w->data_len = (long)len;
            break;
        } else if (len == 0) {
            break;
        } else {
            fseek(f, (long)len, SEEK_CUR);
        }
    }
    if (w->data_at == 0 || w->bits != 16 || w->channels == 0 || w->channels > 8 || w->rate == 0) {
        fprintf(stderr, "wavcheck: %s: not 16-bit PCM with a data chunk\n", path);
        fclose(f);
        return 2;
    }
    *out = f;
    return 0;
}

/** Order run lengths for qsort. */
static int by_length(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;

    return (x > y) - (x < y);
}

/**
 * The frequency of the square wave in the `n` samples at `s`, recorded at
 * `rate`, or -1 when fewer than half of them are sound.
 *
 * The half period of a square wave is the length of its runs of one sign, so
 * the frequency is the rate over twice the median length of the runs that
 * begin and end in the window; `runs` has room for `n` of them. A run begins
 * at the first sample that is sound of the other sign, so a silent sample
 * between two runs, which QEMU's resampling can leave, belongs to neither. A
 * device that plays a period of its buffer again, or misses one, breaks the
 * runs at the ends of that period and leaves the others whole, so the median
 * does not move. A window of sound with fewer than two whole runs gives 0.
 */
static double frequency_of(const short *s, long n, double rate, long *runs)
{
    long i, loud = 0, start = -1, n_runs = 0;
    int sign = 0;

    for (i = 0; i < n; i++) {
        int now;

        if (s[i] > -QUIET && s[i] < QUIET) {
            continue;
        }
        loud++;
        now = s[i] > 0 ? 1 : -1;
        if (now != sign) {
            if (start >= 0) {
                runs[n_runs++] = i - start;
            }
            if (sign != 0) {
                start = i;
            }
            sign = now;
        }
    }
    if (2 * loud < n) {
        return -1.0;
    }
    if (n_runs < 2) {
        return 0.0;
    }
    qsort(runs, (size_t)n_runs, sizeof(runs[0]), by_length);
    return rate / (2.0 * (double)runs[n_runs / 2]);
}

/** The index of the frequency in `freqs` that `hz` is, or -1 for none. */
static int note_of(double hz, const double *freqs, int n_notes)
{
    int i;

    for (i = 0; i < n_notes; i++) {
        double off = (hz - freqs[i]) / freqs[i];

        if (off < TOLERANCE && off > -TOLERANCE) {
            return i;
        }
    }
    return -1;
}

/**
 * Sweep the `frames` samples at `s`, recorded at `rate`, for the `n_notes`
 * frequencies of `freqs`, and say what is wrong.
 *
 * A device that reads its buffer again before a late refill plays some of it
 * twice, so the notes' lengths are not checked, only their order: each 50 ms
 * window that is mostly sound is labelled with the note its frequency is, and
 * the labels must visit every note in the order named, with no note before
 * its predecessor. A window of less sound, which the edges of a note give,
 * belongs to no note; a window of sound at no named frequency is a wrong tone.
 * Each message begins with `who`, which names the channel swept or is empty.
 * Returns 0 when the notes are right, 1 when they are not, and 2 when memory
 * runs out.
 */
static int sweep(const short *s, long frames, double rate, const double *freqs, int n_notes,
                 const char *who)
{
    long win = (long)rate / 20; /* 50 ms */
    long at, *runs;
    int i, want = 0, bad = 0;
    int seen[MAX_NOTES];

    runs = (long *)malloc((size_t)win * sizeof(long));
    if (runs == NULL) {
        return 2;
    }
    for (i = 0; i < n_notes; i++) {
        seen[i] = 0;
    }
    for (at = 0; at + win <= frames; at += win) {
        double hz = frequency_of(s + at, win, rate, runs);
        int note;

        if (hz < 0.0) {
            continue;
        }
        note = note_of(hz, freqs, n_notes);
        if (note < 0) {
            printf("wavcheck: %sa tone of %.0f Hz sounds where %.0f Hz was asked for\n", who, hz,
                   freqs[want]);
            bad++;
            break;
        }
        seen[note] = 1;
        if (note == want + 1) {
            want++;
        } else if (note > want + 1 || note < want) {
            printf("wavcheck: %s%.0f Hz sounds where %.0f Hz was asked for\n", who, freqs[note],
                   freqs[want]);
            bad++;
            break;
        }
    }
    free(runs);
    if (bad != 0) {
        printf("wavcheck: %sthe notes are not what was asked for\n", who);
        return 1;
    }
    for (i = 0; i < n_notes; i++) {
        if (!seen[i]) {
            printf("wavcheck: %s%.0f Hz never sounds\n", who, freqs[i]);
            bad++;
        }
    }
    if (bad != 0) {
        printf("wavcheck: %s%d of %d notes are not what was asked for\n", who, bad, n_notes);
        return 1;
    }
    return 0;
}

/**
 * Judge channel `c` of the `frames` frames of `channels` channels at
 * `samples`, recorded at `rate`, against the `n_notes` frequencies of
 * `freqs`. The silence is trimmed from both ends of the channel when at
 * least a quarter of a second is left, since QEMU writes from when the
 * machine opens the device until it is stopped, and what is left is swept.
 * Messages name the channel when the recording has more than one. Returns
 * sweep's status, or 1 when the channel is too short for the notes.
 */
static int judge(const short *samples, long frames, unsigned channels, unsigned c, double rate,
                 const double *freqs, int n_notes)
{
    char who[32];
    short *s;
    long i, lo = 0, hi = frames;
    int rc;

    who[0] = '\0';
    if (channels > 1) {
        sprintf(who, "channel %u: ", c + 1);
    }
    s = (short *)malloc((size_t)frames * sizeof(short));
    if (s == NULL) {
        return 2;
    }
    for (i = 0; i < frames; i++) {
        s[i] = samples[i * (long)channels + (long)c];
    }
    while (lo < frames && s[lo] > -QUIET && s[lo] < QUIET) {
        lo++;
    }
    while (hi > lo && s[hi - 1] > -QUIET && s[hi - 1] < QUIET) {
        hi--;
    }
    if (hi - lo < (long)rate / 4) {
        lo = 0;
        hi = frames;
    }
    if ((hi - lo) / n_notes < (long)rate / 20) {
        fprintf(stderr, "wavcheck: %s%ld frames for %d notes is too few\n", who, hi - lo, n_notes);
        free(s);
        return 1;
    }
    rc = sweep(s + lo, hi - lo, rate, freqs, n_notes, who);
    free(s);
    if (rc == 0) {
        printf("wavcheck: ok: %s%d notes over %ld frames, each sounding in its turn and in the "
               "order asked for\n",
               who, n_notes, hi - lo);
    }
    return rc;
}

/**
 * Check the recording against the frequencies, as the file's comment says.
 *
 * QEMU writes the data length only when it closes the file, which a machine
 * its case stops does not let it do, so a zero length or one past the end
 * of the file means that the samples run to the end. Each channel is judged
 * on its own, never mixed with another, since mixing would cancel a tone in
 * a recording whose channels are out of phase.
 */
int main(int argc, char **argv)
{
    struct wav w;
    FILE *f;
    double freqs[2][MAX_NOTES];
    int n_notes[2] = {0, 0}, lists = 1, i, rc = 0;
    unsigned c;
    long frames;
    short *samples;

    if (argc < 4 || strcmp(argv[1], "scale") != 0) {
        fprintf(stderr, "usage: wavcheck scale FILE FREQ [FREQ ...] [/ FREQ [FREQ ...]]\n");
        return 2;
    }
    for (i = 3; i < argc; i++) {
        if (strcmp(argv[i], "/") == 0 && lists == 1) {
            lists = 2;
            continue;
        }
        if (n_notes[lists - 1] == MAX_NOTES) {
            fprintf(stderr, "wavcheck: at most %d frequencies can be checked\n", MAX_NOTES);
            return 2;
        }
        freqs[lists - 1][n_notes[lists - 1]] = atof(argv[i]);
        if (freqs[lists - 1][n_notes[lists - 1]++] <= 0.0) {
            fprintf(stderr, "wavcheck: %s is not a frequency\n", argv[i]);
            return 2;
        }
    }
    if (n_notes[0] == 0 || (lists == 2 && n_notes[1] == 0)) {
        fprintf(stderr, "usage: wavcheck scale FILE FREQ [FREQ ...] [/ FREQ [FREQ ...]]\n");
        return 2;
    }
    rc = wav_open(argv[2], &f, &w);
    if (rc != 0) {
        return rc;
    }

    fseek(f, 0, SEEK_END);
    {
        long file_end = ftell(f);

        if (w.data_len <= 0 || w.data_at + w.data_len > file_end) {
            w.data_len = file_end - w.data_at;
        }
    }
    fseek(f, w.data_at, SEEK_SET);
    frames = w.data_len / (long)(2 * w.channels);
    if (frames < (long)w.rate / 10) {
        fprintf(stderr, "wavcheck: %s holds %ld frames, too few to measure\n", argv[2], frames);
        fclose(f);
        return 1;
    }
    if (lists == 2 && w.channels < 2) {
        printf("wavcheck: %s has one channel, and two were asked for\n", argv[2]);
        fclose(f);
        return 1;
    }
    samples = (short *)malloc((size_t)frames * w.channels * sizeof(short));
    if (samples == NULL) {
        fclose(f);
        return 2;
    }
    for (i = 0; i < frames; i++) {
        unsigned char frame[16];

        if (fread(frame, 1, 2 * w.channels, f) != 2 * w.channels) {
            frames = i;
            break;
        }
        for (c = 0; c < w.channels; c++) {
            samples[(long)i * w.channels + c] = (short)(int)(short)rd16(frame + 2 * c);
        }
    }
    fclose(f);

    for (c = 0; c < (lists == 2 ? 2u : w.channels); c++) {
        int list = lists == 2 ? (int)c : 0;
        int one = judge(samples, frames, w.channels, c, (double)w.rate, freqs[list], n_notes[list]);

        if (one > rc) {
            rc = one;
        }
    }
    free(samples);
    return rc;
}
