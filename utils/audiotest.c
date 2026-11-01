/**
 * audiotest: play a square-wave PCM tone through the console audio ioctls, or
 * measure the queue across a stall of the writer.
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
 * `audiotest [rate [seconds [frequency]]]`: play a tone of `frequency` hertz,
 * 440 by default and from 20 to half the rate, for `seconds`, 2 by default
 * and at most 60, at `rate` samples per second, 48000 by default and from
 * 8000 to 384000. `audiotest -s [rate]` instead writes numbered blocks
 * across a stall, as `stall` says, and fails when the queued delay passed six
 * periods.
 *
 * The kernel takes the audio requests only on a descriptor of the console, so
 * the program opens `/dev/console` rather than using its standard output,
 * which lets it run from an SSH session's pseudo-terminal. It needs a PCM
 * driver; the PC speaker plays no samples. The status is 0 when the tone
 * played to its end, and 1 on a usage error, no PCM driver, a failed request,
 * a clock that cannot be read, a delay past its bound or a signal.
 */
int main(int argc, char **argv)
{
    struct audio_info info;
    struct audio_start_params start;
    unsigned rate = 48000, seconds = 2, frequency = 440;
    int fd, result, stalled = argc > 1 && strcmp(argv[1], "-s") == 0;

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
                        "       audiotest -s [rate]\n");
        return 1;
    }
    signal(SIGINT, interrupt_tone);
    signal(SIGTERM, interrupt_tone);
    signal(SIGHUP, interrupt_tone);
    fd = open("/dev/console", O_WRONLY | O_NOCTTY);
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
