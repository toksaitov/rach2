/**
 * audiotest: play a square-wave PCM tone through the console audio ioctls,
 * measure the queue across a stall of the writer, play a scale through OSS's
 * /dev/dsp, or record through /dev/dsp and check what was read against the
 * kernel's count.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* The console's audio requests and their arguments, as rach2/sys/syscall.c
 * defines them. */
#define AUDIO_GET_INFO 0x4101
#define AUDIO_START    0x4102
#define AUDIO_STOP     0x4103
#define AUDIO_WRITE    0x4104

struct audio_info {
    /* has_real_audio is 1 for a PCM driver, 0 for none or the PC speaker. */
    uint32_t driver_type, has_real_audio;
};
struct audio_start_params {
    /* format 1 is signed 16-bit little-endian samples. */
    uint32_t sample_rate, format, channels;
};
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size; /* bytes */
};

/* OSS's requests on /dev/dsp, with Linux's numbers (<linux/soundcard.h>). */
#define SNDCTL_DSP_SYNC        0x00005001u
#define SNDCTL_DSP_SPEED       0xC0045002u
#define SNDCTL_DSP_GETBLKSIZE  0xC0045004u
#define SNDCTL_DSP_SETFMT      0xC0045005u
#define SNDCTL_DSP_CHANNELS    0xC0045006u
#define SNDCTL_DSP_SETFRAGMENT 0xC004500Au
#define SNDCTL_DSP_GETISPACE   0x8010500Du
#define SNDCTL_DSP_SETTRIGGER  0x40045010u
#define SNDCTL_DSP_GETIPTR     0x800C5011u
#define AFMT_QUERY             0x00
#define AFMT_U8                0x08
#define AFMT_S16_LE            0x10

/* The answers of GETISPACE and GETIPTR. */
struct oss_buf_info {
    int fragments, fragstotal, fragsize, bytes; /* bytes ready to read */
};
struct oss_count_info {
    int bytes, blocks, ptr; /* bytes recorded since the open */
};

/* The record mode's buffer: sixteen periods of 2048 bytes, 2^11. */
#define RECORD_FRAGMENT ((16 << 16) | 11)

/* The scales `audiotest -d` plays, five notes of DSP_NOTE_MS each: in stereo
 * one in each channel, the second an octave below the first, and in mono one
 * whose half periods are near whole samples at OSS's default 8000 Hz. No
 * note of one is within 10% of a note of another, so that wavcheck tells
 * them apart, and none is above 444 Hz, whose half period is still 50
 * samples of a recording at 44100 Hz, so that a sample's rounding moves a
 * measurement by 2% at most. */
#define DSP_NOTES   5
#define DSP_NOTE_MS 400u
static const unsigned dsp_left[DSP_NOTES] = {262, 294, 330, 349, 392};
static const unsigned dsp_right[DSP_NOTES] = {131, 147, 165, 175, 196};
static const unsigned dsp_mono[DSP_NOTES] = {80, 100, 111, 222, 444};

static volatile sig_atomic_t interrupted;

/**
 * The handler of SIGINT, SIGTERM and SIGHUP. It sets `interrupted`, which ends
 * `play_tone` early and makes the run fail.
 */
static void interrupt_tone(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

/** Catch SIGINT, SIGTERM and SIGHUP with `interrupt_tone`. */
static void catch_signals(void)
{
    signal(SIGINT, interrupt_tone);
    signal(SIGTERM, interrupt_tone);
    signal(SIGHUP, interrupt_tone);
}

/**
 * Parse `text` as a decimal number from `low` to `high`.
 *
 * Returns the number, or 0 when `text` is empty, has anything after the
 * digits, or is out of range. Every caller's `low` is above 0, so 0 is never
 * a valid result.
 */
static unsigned number(const char *text, unsigned low, unsigned high)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || !*text || *end || value < low || value > high)
        return 0;
    return (unsigned)value;
}

/**
 * Store the monotonic clock in milliseconds in `*value`. The value wraps, so a
 * caller uses only the difference of two readings.
 *
 * Returns 0, or -1 with the failure reported on standard error when the clock
 * cannot be read.
 */
static int milliseconds(unsigned *value)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("audiotest: clock");
        return -1;
    }
    *value = (unsigned)now.tv_sec * 1000u + (unsigned)now.tv_nsec / 1000000u;
    return 0;
}

/**
 * Play `seconds` of a square wave of `frequency` hertz, at `rate` samples per
 * second in 16-bit stereo, on the stream that `AUDIO_START` began on `fd`.
 *
 * The samples go to `AUDIO_WRITE` 256 frames at a time. The driver may take
 * fewer bytes than it is given, so the rest is sent again; a write that takes
 * nothing is tried again after a millisecond, and five seconds without a byte
 * taken is a failure. After the last frame it sleeps two seconds, so that the
 * driver plays what it has queued before the caller stops the stream.
 *
 * Returns 0 when every frame was written, or -1 on a failed write, a stall, a
 * clock that cannot be read or a signal; each failure but the signal is
 * reported on standard error.
 */
static int play_tone(int fd, unsigned rate, unsigned seconds, unsigned frequency)
{
    int16_t samples[512];
    unsigned remaining = rate * seconds;
    unsigned phase = 0;
    unsigned last_progress, now;

    if (milliseconds(&last_progress) < 0)
        return -1;
    while (remaining && !interrupted) {
        unsigned frames = remaining < 256 ? remaining : 256;
        unsigned i, offset = 0;
        for (i = 0; i < frames; i++) {
            int16_t sample = phase < rate / 2 ? 6000 : -6000;
            samples[2 * i] = samples[2 * i + 1] = sample;
            phase = (phase + frequency) % rate;
        }
        while (offset < frames * 4 && !interrupted) {
            struct audio_write_params data;
            int written;
            data.buffer = (const uint8_t *)samples + offset;
            data.size = frames * 4 - offset;
            written = ioctl(fd, AUDIO_WRITE, &data);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                perror("audiotest: write");
                return -1;
            }
            if ((unsigned)written > data.size) {
                fprintf(stderr, "audiotest: invalid write count\n");
                return -1;
            }
            if (milliseconds(&now) < 0)
                return -1;
            if (written) {
                offset += (unsigned)written;
                last_progress = now;
            } else {
                if (now - last_progress >= 5000) {
                    fprintf(stderr, "audiotest: no playback progress\n");
                    return -1;
                }
                usleep(1000);
            }
        }
        remaining -= frames;
    }
    if (!interrupted)
        usleep(2000000);
    return interrupted ? -1 : 0;
}

/**
 * The number after the first `key` in /proc/audio, in `*value`.
 *
 * Returns 0, or -1 with the failure reported on standard error when the file
 * cannot be read or has no such field.
 */
static int proc_audio(const char *key, unsigned *value)
{
    static char text[32768];
    const char *at;
    size_t used = 0;
    ssize_t n;
    int fd = open("/proc/audio", O_RDONLY);

    if (fd < 0) {
        perror("audiotest: /proc/audio");
        return -1;
    }
    while (used < sizeof(text) - 1 && (n = read(fd, text + used, sizeof(text) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    text[used] = 0;
    at = strstr(text, key);
    if (at == NULL) {
        fprintf(stderr, "audiotest: /proc/audio has no %s\n", key);
        return -1;
    }
    *value = (unsigned)strtoul(at + strlen(key), NULL, 10);
    return 0;
}

/**
 * Write `blocks` numbered blocks of 10 ms each, signed 16-bit stereo at
 * `rate`, every sample the block's number, from `*next` on, on the stream that
 * `AUDIO_START` began on `fd`. A write the queue takes in part is tried again
 * after a millisecond, and five seconds without a byte taken is a failure.
 * After each block the queued delay is read from /proc/audio, and the largest
 * is kept in `*most`, in milliseconds.
 *
 * Returns 0, or -1 on a failed write or read, a stall of the queue, a clock
 * that cannot be read or a signal.
 */
static int write_blocks(int fd, unsigned rate, unsigned blocks, unsigned *next, unsigned *most)
{
    static int16_t samples[2 * 3840];
    unsigned frames = rate / 100;
    unsigned i, offset, delay, last_progress, now;

    if (milliseconds(&last_progress) < 0)
        return -1;
    while (blocks-- && !interrupted) {
        for (i = 0; i < 2 * frames; i++)
            samples[i] = (int16_t)*next;
        for (offset = 0; offset < frames * 4 && !interrupted;) {
            struct audio_write_params data;
            int written;

            data.buffer = (const uint8_t *)samples + offset;
            data.size = frames * 4 - offset;
            written = ioctl(fd, AUDIO_WRITE, &data);
            if (written < 0 && errno == EINTR)
                continue;
            if (written < 0 || (unsigned)written > data.size) {
                perror("audiotest: write");
                return -1;
            }
            if (milliseconds(&now) < 0)
                return -1;
            if (written) {
                offset += (unsigned)written;
                last_progress = now;
            } else if (now - last_progress >= 5000) {
                fprintf(stderr, "audiotest: no playback progress\n");
                return -1;
            } else {
                usleep(1000);
            }
        }
        if (proc_audio(" delay-ms ", &delay) < 0)
            return -1;
        if (delay > *most)
            *most = delay;
        ++*next;
    }
    return interrupted ? -1 : 0;
}

/**
 * Write a second of numbered blocks at `rate`, stall a second, and write
 * another, on the stream begun on `fd`, and check that the queued delay
 * /proc/audio gives stays within six of the driver's periods. A period is the
 * frame count nearest 23 ms at the programmed rate, in 16-bit stereo bytes
 * rounded to the nearest 128, at most 16384, as hda.c sizes it.
 *
 * Prints the largest delay and the bound. Returns 0 when the delay kept
 * within it, -1 otherwise or on a failure the callees report.
 */
static int stall(int fd, unsigned rate)
{
    unsigned next = 1, most = 0, programmed, bytes, bound;

    if (write_blocks(fd, rate, 100, &next, &most) < 0)
        return -1;
    usleep(1000000);
    if (write_blocks(fd, rate, 100, &next, &most) < 0)
        return -1;
    usleep(200000);
    if (proc_audio("programmed ", &programmed) < 0 || programmed == 0)
        return -1;
    bytes = ((programmed * 23 + 500) / 1000 * 4 + 64) / 128 * 128;
    if (bytes < 128)
        bytes = 128;
    if (bytes > 16384)
        bytes = 16384;
    bound = 6 * (bytes / 4) * 1000 / programmed;
    printf("audiotest: stall: delay-ms most %u bound %u\n", most, bound);
    return most <= bound ? 0 : -1;
}

/**
 * Ask /dev/dsp, open as `fd`, for `request` with `*value`, and leave in it
 * the value the driver granted. Returns 0, or -1 with the failure reported
 * on standard error.
 */
static int dsp_set(int fd, unsigned long request, int *value, const char *name)
{
    if (ioctl(fd, request, value) < 0) {
        fprintf(stderr, "audiotest: %s: %s\n", name, strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Write the `size` bytes at `data` to /dev/dsp, open as `fd`, whose writes
 * block until there is room. Returns 0, or -1 on a failed write or a signal.
 */
static int dsp_write(int fd, const uint8_t *data, size_t size)
{
    while (size && !interrupted) {
        ssize_t n = write(fd, data, size);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            perror("audiotest: write");
            return -1;
        }
        data += n;
        size -= (size_t)n;
    }
    return interrupted ? -1 : 0;
}

/* The format, channels and rate /dev/dsp granted. */
struct dsp_format {
    int format, channels, rate; /* AFMT_U8 or AFMT_S16_LE, 1 or 2, hertz */
};

/**
 * Open /dev/dsp for writing and ask for the format, channels and rate of
 * `want`, in that order as OSS asks, or with `want` NULL keep those the open
 * set, OSS's defaults. The values granted are stored in `*f` and printed.
 * Returns the descriptor, or -1 on a failure, which is reported on standard
 * error, or on values the scale cannot be played in.
 */
static int dsp_open(const int *want, struct dsp_format *f)
{
    int fd = open("/dev/dsp", O_WRONLY);

    if (fd < 0) {
        perror("audiotest: /dev/dsp");
        return -1;
    }
    f->format = AFMT_QUERY;
    f->channels = 0;
    f->rate = 0;
    if (want) {
        f->format = want[1] == 8 ? AFMT_U8 : AFMT_S16_LE;
        f->channels = want[2];
        f->rate = want[0];
    }
    if (dsp_set(fd, SNDCTL_DSP_SETFMT, &f->format, "SNDCTL_DSP_SETFMT") < 0 ||
        dsp_set(fd, SNDCTL_DSP_CHANNELS, &f->channels, "SNDCTL_DSP_CHANNELS") < 0 ||
        dsp_set(fd, SNDCTL_DSP_SPEED, &f->rate, "SNDCTL_DSP_SPEED") < 0) {
        close(fd);
        return -1;
    }
    if ((f->format != AFMT_U8 && f->format != AFMT_S16_LE) || f->channels < 1 || f->channels > 2 ||
        f->rate < 1000) {
        fprintf(stderr,
                "audiotest: dsp: granted format %d, %d channels, %d hz, which it cannot play\n",
                f->format, f->channels, f->rate);
        close(fd);
        return -1;
    }
    printf("audiotest: dsp: granted %d hz, %d-bit %s\n", f->rate, f->format == AFMT_U8 ? 8 : 16,
           f->channels == 1 ? "mono" : "stereo");
    return fd;
}

/**
 * Fill `buf` with `n` frames of the scale's note number `note` in the format
 * `f`: a note of `dsp_left` in the first channel and one of `dsp_right` in
 * the second, or of `dsp_mono` in one channel, each a square wave. `phase`
 * holds each channel's place in its period, in 1/rate of a period, from one
 * call to the next. Returns the bytes filled.
 */
static unsigned dsp_fill(uint8_t *buf, unsigned n, unsigned note, const struct dsp_format *f,
                         unsigned *phase)
{
    unsigned i, c, at = 0;

    for (i = 0; i < n; i++) {
        for (c = 0; c < (unsigned)f->channels; c++) {
            unsigned hz = f->channels == 1 ? dsp_mono[note] : c ? dsp_right[note] : dsp_left[note];
            int high = phase[c] < (unsigned)f->rate / 2;

            phase[c] = (phase[c] + hz) % (unsigned)f->rate;
            if (f->format == AFMT_U8) {
                buf[at++] = (uint8_t)(high ? 128 + 24 : 128 - 24);
            } else {
                int16_t v = high ? 6000 : -6000;

                buf[at++] = (uint8_t)((uint16_t)v & 0xFF);
                buf[at++] = (uint8_t)((uint16_t)v >> 8);
            }
        }
    }
    return at;
}

/**
 * Play a scale through /dev/dsp, opened by `dsp_open` with `want`: each note
 * that `dsp_fill` makes for DSP_NOTE_MS. SNDCTL_DSP_SYNC then waits until
 * the device has played it. Returns 0, or -1 on a failure, which is reported
 * on standard error, or a signal.
 */
static int dsp_scale(const int *want)
{
    static uint8_t buf[4096];
    struct dsp_format f;
    unsigned note, frame, frames, phase[2], n;
    int fd = dsp_open(want, &f);

    if (fd < 0)
        return -1;
    frame = (unsigned)f.channels * (f.format == AFMT_U8 ? 1u : 2u);
    for (note = 0; note < DSP_NOTES && !interrupted; note++) {
        frames = (unsigned)f.rate * DSP_NOTE_MS / 1000u;
        phase[0] = phase[1] = 0;
        while (frames && !interrupted) {
            n = frames < sizeof(buf) / frame ? frames : sizeof(buf) / frame;
            if (dsp_write(fd, buf, dsp_fill(buf, n, note, &f, phase)) < 0) {
                close(fd);
                return -1;
            }
            frames -= n;
        }
    }
    if (!interrupted && ioctl(fd, SNDCTL_DSP_SYNC, 0) < 0 && errno != EINTR) {
        perror("audiotest: SNDCTL_DSP_SYNC");
        close(fd);
        return -1;
    }
    close(fd);
    return interrupted ? -1 : 0;
}

/**
 * `audiotest -d [rate bits channels]`: play dsp_scale's scale through
 * /dev/dsp, in `bits` of 8 or 16, one or two `channels` and a `rate` from
 * 1000 to 192000, or with no arguments at OSS's defaults. The status is 0
 * when the scale played to its end, and 1 on a usage error, a failure or a
 * signal.
 */
static int dsp_main(int argc, char **argv)
{
    int want[3];

    if (argc != 0 && argc != 3) {
        fprintf(stderr, "audiotest: usage: audiotest -d [rate bits channels]\n");
        return 1;
    }
    if (argc == 3) {
        want[0] = (int)number(argv[0], 1000, 192000);
        want[1] = (int)number(argv[1], 8, 16);
        want[2] = (int)number(argv[2], 1, 2);
        if (!want[0] || (want[1] != 8 && want[1] != 16) || !want[2]) {
            fprintf(stderr, "audiotest: usage: audiotest -d [rate bits channels]\n");
            return 1;
        }
    }
    catch_signals();
    if (dsp_scale(argc == 3 ? want : NULL) < 0)
        return 1;
    puts("audiotest: result: complete; the scale played through /dev/dsp");
    return 0;
}

/**
 * Find the line of /proc/audio that begins `capture running ` and read the
 * input stream's granted rate, the frames it handed over and its overruns
 * from it.
 *
 * Returns 0, or -1 with the failure reported on standard error when the file
 * cannot be read or has no such line.
 */
static int proc_capture(unsigned *rate, unsigned *frames, unsigned *overruns)
{
    static char text[32768];
    const char *at;
    size_t used = 0;
    ssize_t n;
    unsigned running, requested, format, period, periods;
    int fd = open("/proc/audio", O_RDONLY);

    if (fd < 0) {
        perror("audiotest: /proc/audio");
        return -1;
    }
    while (used < sizeof(text) - 1 && (n = read(fd, text + used, sizeof(text) - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    text[used] = 0;
    at = strstr(text, "capture running ");
    if (at == NULL ||
        sscanf(at,
               "capture running %u requested %u rate %u format 0x%x period %u periods %u "
               "frames %u overruns %u",
               &running, &requested, rate, &format, &period, &periods, frames, overruns) != 8) {
        fprintf(stderr, "audiotest: /proc/audio has no capture line\n");
        return -1;
    }
    return 0;
}

/**
 * Print the `n` bytes at `p` as lines `audiotest: data: HEX`, 64 bytes to a
 * line, for a host that reads the console to rebuild them.
 */
static void dump(const unsigned char *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    char line[17 + 128 + 2];
    size_t i, k;

    for (i = 0; i < n; i += 64) {
        memcpy(line, "audiotest: data: ", 17);
        for (k = 0; k < 64 && i + k < n; k++) {
            line[17 + 2 * k] = digits[p[i + k] >> 4];
            line[18 + 2 * k] = digits[p[i + k] & 15];
        }
        line[17 + 2 * k] = '\n';
        fwrite(line, 1, 18 + 2 * k, stdout);
    }
    fflush(stdout);
}

/**
 * Open /dev/dsp for reading in signed 16-bit mono at `*rate` as near as the
 * device has it, in a buffer of RECORD_FRAGMENT. The rate granted is stored
 * in `*rate` and the bytes of a period in `*period`. Returns the descriptor,
 * or -1 on a failed call, which is reported on standard error.
 */
static int record_open(unsigned *rate, int *period)
{
    int value, fd = open("/dev/dsp", O_RDONLY);

    if (fd < 0) {
        perror("audiotest: /dev/dsp");
        return -1;
    }
    value = AFMT_S16_LE;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &value) < 0 || value != AFMT_S16_LE) {
        fprintf(stderr, "audiotest: /dev/dsp: no signed 16-bit format\n");
        close(fd);
        return -1;
    }
    value = 1;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &value) < 0 || value != 1) {
        fprintf(stderr, "audiotest: /dev/dsp: no mono\n");
        close(fd);
        return -1;
    }
    value = (int)*rate;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &value) < 0) {
        perror("audiotest: /dev/dsp: speed");
        close(fd);
        return -1;
    }
    *rate = (unsigned)value;
    value = RECORD_FRAGMENT;
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &value) < 0 ||
        ioctl(fd, SNDCTL_DSP_GETBLKSIZE, period) < 0 || *period <= 0) {
        perror("audiotest: /dev/dsp: fragments");
        close(fd);
        return -1;
    }
    return fd;
}

/**
 * Turn input off on `fd`, which stops the device, and check the counts after
 * `got` bytes were read of the `want` asked for at `rate`. The frames read
 * and those still in the buffer must be the frames /proc/audio says the
 * input stream handed over, so that none was dropped, `rate` must be
 * /proc/audio's, the frames GETIPTR says were recorded must be at least
 * those, and the stream must have had no overrun.
 *
 * Returns 0 when the counts agree, -1 otherwise or on a failed call, which
 * is reported on standard error, or a signal.
 */
static int record_check(int fd, size_t got, size_t want, unsigned rate)
{
    struct oss_buf_info left;
    struct oss_count_info recorded;
    unsigned proc_rate, handed, overruns, frames;
    int value = 0;

    if (interrupted)
        return -1;
    if (ioctl(fd, SNDCTL_DSP_SETTRIGGER, &value) < 0 ||
        ioctl(fd, SNDCTL_DSP_GETISPACE, &left) < 0 ||
        ioctl(fd, SNDCTL_DSP_GETIPTR, &recorded) < 0 ||
        proc_capture(&proc_rate, &handed, &overruns) < 0) {
        perror("audiotest: /dev/dsp: counts");
        return -1;
    }
    frames = (unsigned)((got + (size_t)left.bytes) / 2);
    printf("audiotest: record: read %lu bytes, %d left; rate %u, /proc/audio's %u; "
           "%u frames read and left, %u handed over, %u recorded; %u overruns\n",
           (unsigned long)got, left.bytes, rate, proc_rate, frames, handed,
           (unsigned)recorded.bytes / 2, overruns);
    if (got != want || rate != proc_rate || frames != handed ||
        (unsigned)recorded.bytes / 2 < handed || overruns != 0)
        return -1;
    return 0;
}

/**
 * Record `periods` periods through /dev/dsp, opened by `record_open` at
 * `rate`, with one blocking read of a period at a time; then check the
 * counts with `record_check`. With `print`, the samples read are printed
 * after the counts, as `dump` does.
 *
 * Returns 0 when the counts agree, -1 otherwise or on a failed call, which
 * is reported on standard error.
 */
static int record(unsigned rate, unsigned periods, int print)
{
    unsigned char *samples;
    size_t got = 0, want;
    int fd, period, result = 0;

    fd = record_open(&rate, &period);
    if (fd < 0)
        return -1;
    want = (size_t)periods * (size_t)period;
    samples = malloc(want);
    if (samples == NULL) {
        fprintf(stderr, "audiotest: no memory for %lu bytes\n", (unsigned long)want);
        close(fd);
        return -1;
    }
    printf("audiotest: record: %u periods of %d bytes, 16-bit mono at %u hz\n", periods, period,
           rate);
    fflush(stdout);
    while (got < want && !interrupted) {
        ssize_t n = read(fd, samples + got, (size_t)period);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            perror("audiotest: /dev/dsp: read");
            result = -1;
            break;
        }
        got += (size_t)n;
    }
    if (result == 0)
        result = record_check(fd, got, want, rate);
    close(fd);
    if (result == 0 && print)
        dump(samples, got);
    free(samples);
    return result;
}

/**
 * `audiotest -r|-R [rate [periods]]`, with `argv` after the option: record
 * `periods` periods, 50 by default and at most 1000, at `rate`, 48000 by
 * default and from 8000 to 192000, as `record` says, printing the samples
 * with `print`. The status is 0 when the counts agreed, and 1 on a usage
 * error, a failure or a signal.
 */
static int record_main(int print, int argc, char **argv)
{
    unsigned rate = 48000, periods = 50;

    if (argc > 0)
        rate = number(argv[0], 8000, 192000);
    if (argc > 1)
        periods = number(argv[1], 1, 1000);
    if (argc > 2 || !rate || !periods) {
        fprintf(stderr, "audiotest: usage: audiotest -r|-R [rate [periods]]\n");
        return 1;
    }
    catch_signals();
    if (record(rate, periods, print) != 0)
        return 1;
    puts("audiotest: result: complete; the frames read match the kernel's count");
    return 0;
}

/**
 * Play a tone of `frequency` hertz for `seconds` at `rate`, or with `stalled`
 * write numbered blocks across a stall at `rate`, through the console's audio
 * requests, as `main` says. Returns the exit status, 0 when the tone played
 * or the delay kept within its bound and 1 otherwise.
 */
static int console_main(unsigned rate, unsigned seconds, unsigned frequency, int stalled)
{
    struct audio_info info;
    struct audio_start_params start;
    int result, fd = open("/dev/console", O_WRONLY | O_NOCTTY);

    if (fd < 0) {
        perror("audiotest: open console");
        return 1;
    }
    if (ioctl(fd, AUDIO_GET_INFO, &info) < 0 || !info.has_real_audio) {
        fprintf(stderr, "audiotest: no pcm driver\n");
        close(fd);
        return 1;
    }
    start.sample_rate = rate;
    start.format = 1;
    start.channels = 2;
    if (ioctl(fd, AUDIO_START, &start) < 0) {
        perror("audiotest: start");
        close(fd);
        return 1;
    }
    if (stalled) {
        printf("audiotest: stall: %u hz pcm, a second, a second's stall, a second\n", rate);
        result = stall(fd, rate);
    } else {
        printf("audiotest: playing: %u hz tone, %u hz pcm, %u seconds\n", frequency, rate, seconds);
        result = play_tone(fd, rate, seconds, frequency);
    }
    if (ioctl(fd, AUDIO_STOP, (void *)0) < 0) {
        perror("audiotest: stop");
        result = -1;
    }
    close(fd);
    if (result == 0 && stalled)
        puts("audiotest: result: complete; the delay kept within six periods");
    else if (result == 0)
        puts("audiotest: result: complete; read /proc/audio for the programmed format");
    return result != 0;
}

/**
 * `audiotest [rate [seconds [frequency]]]`: play a tone of `frequency` hertz,
 * 440 by default and from 20 to half the rate, for `seconds`, 2 by default
 * and at most 60, at `rate` samples per second, 48000 by default and from
 * 8000 to 384000. `audiotest -s [rate]` instead writes numbered blocks
 * across a stall, as `stall` says, and fails when the queued delay passed six
 * periods; `audiotest -d` plays a scale through /dev/dsp, as `dsp_main`
 * says, and `audiotest -r` and `-R` record through it, as `record_main`
 * says.
 *
 * The kernel takes the audio requests only on a descriptor of the console, so
 * the program opens `/dev/console` rather than using its standard output,
 * which lets it run from an SSH session's pseudo-terminal. It needs a PCM
 * driver; the PC speaker plays no samples. The status is 0 when the tone
 * played to its end or the record mode's counts agreed, and 1 on a usage
 * error, no PCM driver, a failed request, counts that disagree, a clock that
 * cannot be read, a delay past its bound or a signal.
 */
int main(int argc, char **argv)
{
    unsigned rate = 48000, seconds = 2, frequency = 440;
    int stalled = argc > 1 && strcmp(argv[1], "-s") == 0;

    if (argc > 1 && strcmp(argv[1], "-d") == 0)
        return dsp_main(argc - 2, argv + 2);
    if (argc > 1 && (strcmp(argv[1], "-r") == 0 || strcmp(argv[1], "-R") == 0))
        return record_main(argv[1][1] == 'R', argc - 2, argv + 2);
    if (stalled) {
        argv++;
        argc--;
    }
    if (argc > 1)
        rate = number(argv[1], 8000, 384000);
    if (argc > 2 && !stalled)
        seconds = number(argv[2], 1, 60);
    if (argc > 3 && !stalled)
        frequency = number(argv[3], 20, rate / 2);
    if (argc > (stalled ? 2 : 4) || !rate || !seconds || !frequency) {
        fprintf(stderr, "audiotest: usage: audiotest [rate [seconds [frequency]]]\n"
                        "       audiotest -s [rate]\n"
                        "       audiotest -d [rate bits channels]\n"
                        "       audiotest -r|-R [rate [periods]]\n");
        return 1;
    }
    catch_signals();
    return console_main(rate, seconds, frequency, stalled);
}
