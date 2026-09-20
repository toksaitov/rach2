/** Play a square-wave PCM tone through the console audio ioctls. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define AUDIO_GET_INFO 0x4101
#define AUDIO_START    0x4102
#define AUDIO_STOP     0x4103
#define AUDIO_WRITE    0x4104

struct audio_info {
    uint32_t driver_type, has_real_audio;
};
struct audio_start_params {
    uint32_t sample_rate, format, channels;
};
struct audio_write_params {
    const uint8_t *buffer;
    uint32_t size;
};
static volatile sig_atomic_t interrupted;

/** Let the main loop stop the stream after a signal. */
static void interrupt_tone(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

/** Parse a bounded decimal argument without accepting a trailing suffix. */
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

/** Return monotonic milliseconds for a stalled-write completion bound. */
static unsigned milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (unsigned)now.tv_sec * 1000u + (unsigned)now.tv_nsec / 1000000u;
}

/** Submit complete stereo frames and retry every short write. */
static int play_tone(int fd, unsigned rate, unsigned seconds, unsigned frequency)
{
    int16_t samples[512];
    unsigned remaining = rate * seconds;
    unsigned phase = 0;
    unsigned last_progress = milliseconds();

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
            if (written) {
                offset += (unsigned)written;
                last_progress = milliseconds();
            } else {
                if (milliseconds() - last_progress >= 5000) {
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

/** Open the real console so an SSH pty can drive the audio interface. */
int main(int argc, char **argv)
{
    struct audio_info info;
    struct audio_start_params start;
    unsigned rate = 48000, seconds = 2, frequency = 440;
    int fd, result;

    if (argc > 1)
        rate = number(argv[1], 8000, 384000);
    if (argc > 2)
        seconds = number(argv[2], 1, 60);
    if (argc > 3)
        frequency = number(argv[3], 20, rate / 2);
    if (argc > 4 || !rate || !seconds || !frequency) {
        fprintf(stderr, "audiotest: usage: audiotest [rate [seconds [frequency]]]\n");
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
    printf("audiotest: playing: %u hz tone, %u hz pcm, %u seconds\n", frequency, rate, seconds);
    result = play_tone(fd, rate, seconds, frequency);
    if (ioctl(fd, AUDIO_STOP, 0) < 0) {
        perror("audiotest: stop");
        result = -1;
    }
    close(fd);
    if (result == 0)
        puts("audiotest: result: complete; read /proc/audio for the programmed format");
    return result != 0;
}
