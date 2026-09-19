/**
 * wavcheck: what a machine actually played, read off the recording.
 *
 * QEMU's `wav` audio backend writes every sample the guest produced to a file,
 * so a boot that plays a scale leaves one behind and the notes can be measured
 * afterwards, from outside, where nothing in the guest can forge them. That is
 * the only way to check the audio path end to end: the driver's own report
 * says what it was asked to play, not what came out.
 *
 * `wavcheck scale FILE FREQ [FREQ ...]` sweeps the recording in short windows,
 * labels each with whichever of the named frequencies is loudest in it, and
 * requires those labels to visit every frequency, in the order they were
 * named. The notes are not equal parts of the recording, so nothing here
 * assumes they are: the device opens during the first and the machine is
 * stopped during the last. The measurement is a Goertzel filter, which is a
 * single-frequency Fourier term and needs no library.
 *
 * Exit 0 when every part answers to its own note, 1 when one does not, 2 when
 * the file cannot be read or the arguments make no sense.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NOTES 16

/* A RIFF/WAVE header, as QEMU writes it: 16-bit samples, one or two channels,
   and the data chunk last. Only what is needed to find the samples is read. */
struct wav {
    unsigned rate;
    unsigned channels;
    unsigned bits;
    long data_at;
    long data_len;
};

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

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
            /* A chunk of no length is a header QEMU never went back to fix,
               which is what a recording from a machine that was stopped
               rather than shut down looks like. */
            break;
        } else {
            fseek(f, (long)len, SEEK_CUR);
        }
    }
    /* The frame buffer below holds eight channels of 16-bit samples, which is
       more than any recording this reads and is the bound a header cannot be
       allowed to exceed. */
    if (w->data_at == 0 || w->bits != 16 || w->channels == 0 || w->channels > 8 || w->rate == 0) {
        fprintf(stderr, "wavcheck: %s: not 16-bit PCM with a data chunk\n", path);
        fclose(f);
        return 2;
    }
    *out = f;
    return 0;
}

/* The energy at `freq` in `n` samples, by Goertzel: one Fourier term, which
   is all that is wanted and needs no transform. */
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

    /* QEMU writes the data length only when it closes the file, and a machine
       the harness stops never lets it: a zero or impossible length means the
       samples run to the end of the file. */
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
    /* One channel of it: a tone is in both, and mixing them would cancel a
       recording whose channels are out of phase. */
    for (i = 0; i < frames; i++) {
        unsigned char frame[16];

        if (fread(frame, 1, 2 * w.channels, f) != 2 * w.channels) {
            frames = i;
            break;
        }
        samples[i] = (short)(int)(short)rd16(frame);
    }
    fclose(f);

    /* The scale is somewhere inside the recording: QEMU starts writing when
       the machine opens the device and stops when it is killed, so there is
       silence at both ends and only what is between it is the tune. */
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

    /* The notes are not equal parts of the recording: the device opens during
       the first one and the machine is stopped during the last, so their
       shares differ. What is required instead is the order. The recording is
       swept in short windows, each labelled with whichever of the named
       frequencies is loudest in it, and those labels must visit every note
       once, in the order they were named, with no note before its
       predecessor. */
    {
        long win = (long)w.rate / 20; /* 50 ms, two periods of the lowest note */
        long at;
        int want = 0;
        int seen[MAX_NOTES];
        /* A note this stack plays at its ordinary volume measures in the
           thousands here; a hundredth of that is silence with noise in it. */
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
            /* A window with no sound in it belongs to no note: without this
               a machine playing almost nothing would still have every window
               labelled, and the labels would still be in order. */
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
