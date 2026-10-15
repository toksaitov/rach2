/**
 * wavcheck: check the notes a machine played, from the recording QEMU made.
 *
 * QEMU's `wav` audio backend writes every sample the guest produced to a file,
 * so a boot that plays a scale leaves one behind and the notes can be measured
 * afterwards, outside the guest, where nothing in the guest can change them.
 * That is how the audio path is checked end to end: the driver's own report
 * says what it was asked to play, not what came out.
 *
 * `wavcheck scale FILE FREQ [FREQ ...]` sweeps the recording in short windows,
 * labels each with whichever of the named frequencies is loudest in it, and
 * requires those labels to visit every frequency, in the order they were
 * named. At most 16 frequencies are read. The measurement is a Goertzel
 * filter, which computes a single term of the Fourier transform and needs no
 * library.
 *
 * Exit 0 when every note is the loudest in its turn, 1 when one is not or
 * the recording is too short, and 2 when the file cannot be read or the
 * arguments are invalid.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTES 16

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

/** The energy at `freq` in the `n` samples at `s`, recorded at `rate`, by
 * Goertzel's algorithm. */
static double energy_at(const short *s, long n, double freq, double rate)
{
    double w = 2.0 * 3.14159265358979323846 * freq / rate;
    double coeff = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    long i;

    for (i = 0; i < n; i++) {
        double t = (double)s[i] / 32768.0 + coeff * s1 - s2;

        s2 = s1;
        s1 = t;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/**
 * Check the recording against the frequencies, as the file's comment says.
 *
 * QEMU writes the data length only when it closes the file, which a machine
 * the harness stops does not let it do, so a zero length or one past the end
 * of the file means that the samples run to the end. Only the first channel
 * is read: a tone is in every channel, and mixing them would cancel it in a
 * recording whose channels are out of phase. QEMU writes from when the
 * machine opens the device until it is stopped, so the silence, samples
 * within 256 of zero, is trimmed from both ends when at least a quarter of a
 * second is left. The notes are not equal parts of what is left, since the
 * device opens during the first and the machine is stopped during the last,
 * so only their order is checked: each 50 ms window is labelled with its
 * loudest frequency, and the labels must visit every note in the order
 * named, with no note before its predecessor. A window whose loudest energy
 * is under 1.0 has no sound in it and belongs to no note; without that, a
 * machine playing almost nothing would still have every window labelled in
 * order.
 */
int main(int argc, char **argv)
{
    struct wav w;
    FILE *f;
    double freqs[MAX_NOTES];
    int n_notes = 0, i, rc, bad = 0;
    long frames, per_note, first = 0;
    short *samples;

    if (argc < 4 || strcmp(argv[1], "scale") != 0) {
        fprintf(stderr, "usage: wavcheck scale FILE FREQ [FREQ ...]\n");
        return 2;
    }
    for (i = 3; i < argc && n_notes < MAX_NOTES; i++) {
        freqs[n_notes++] = atof(argv[i]);
        if (freqs[n_notes - 1] <= 0.0) {
            fprintf(stderr, "wavcheck: %s is not a frequency\n", argv[i]);
            return 2;
        }
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
    samples = (short *)malloc((size_t)frames * sizeof(short));
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
        samples[i] = (short)(int)(short)rd16(frame);
    }
    fclose(f);

    {
        long lo = 0, hi = frames;

        while (lo < frames && samples[lo] > -256 && samples[lo] < 256) {
            lo++;
        }
        while (hi > lo && samples[hi - 1] > -256 && samples[hi - 1] < 256) {
            hi--;
        }
        if (hi - lo >= (long)w.rate / 4) {
            first = lo;
            frames = hi - lo;
        }
    }
    per_note = frames / n_notes;
    if (per_note < (long)w.rate / 20) {
        fprintf(stderr, "wavcheck: %ld frames for %d notes is too few\n", frames, n_notes);
        free(samples);
        return 1;
    }

    {
        long win = (long)w.rate / 20; /* 50 ms */
        long at;
        int want = 0;
        int seen[MAX_NOTES];
        double quiet = 1.0;

        for (i = 0; i < n_notes; i++) {
            seen[i] = 0;
        }
        for (at = first; at + win <= first + frames; at += win) {
            int loudest = 0;
            double best = energy_at(samples + at, win, freqs[0], (double)w.rate);

            for (i = 1; i < n_notes; i++) {
                double e = energy_at(samples + at, win, freqs[i], (double)w.rate);

                if (e > best) {
                    best = e;
                    loudest = i;
                }
            }
            if (best < quiet) {
                continue;
            }
            seen[loudest] = 1;
            if (loudest == want + 1) {
                want++;
            } else if (loudest > want + 1 || loudest < want) {
                printf("wavcheck: %.0f Hz sounds where %.0f Hz was asked for\n", freqs[loudest],
                       freqs[want]);
                bad++;
                break;
            }
        }
        for (i = 0; i < n_notes; i++) {
            if (!seen[i]) {
                printf("wavcheck: %.0f Hz is never the loudest\n", freqs[i]);
                bad++;
            }
        }
    }

    free(samples);
    if (bad != 0) {
        printf("wavcheck: %d of %d notes are not what was asked for\n", bad, n_notes);
        return 1;
    }
    printf("wavcheck: ok: %d notes over %ld frames, each the loudest in its "
           "turn and in the order asked for\n",
           n_notes, frames);
    return 0;
}
