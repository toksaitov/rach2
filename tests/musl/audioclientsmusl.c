/*
 * Exercise concurrent OSS players, shared capture and sox_ng's loopback.
 * The machine case judges the WAV samples printed as hexadecimal lines.
 */
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE   48000
#define FRAMES (RATE * 2)

static pid_t children[3];
static char workspace[256];
static char files[3][300];

/** Reap every child and remove the files on every exit, including failure. */
static void cleanup(void)
{
    unsigned i;

    for (i = 0; i < sizeof(children) / sizeof(children[0]); i++) {
        if (children[i] > 0) {
            kill(children[i], SIGKILL);
            while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {
            }
        }
    }
    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (files[i][0])
            unlink(files[i]);
    if (workspace[0])
        rmdir(workspace);
}

/** Exit through atexit when the case stops its guest. */
static void interrupted(int signo)
{
    exit(128 + signo);
}

/** Report the failed operation and stop; atexit performs all cleanup. */
static void fail(const char *what)
{
    fprintf(stderr, "audioclients: %s: %s\n", what, strerror(errno));
    exit(1);
}

/** Transfer exactly `n` bytes on `fd`; a short transfer is retried. */
static void transfer(int fd, void *buffer, size_t n, int writing)
{
    unsigned char *p = buffer;
    ssize_t done;

    while (n) {
        done = writing ? write(fd, p, n) : read(fd, p, n);
        if (done < 0 && errno == EINTR)
            continue;
        if (done <= 0)
            fail(writing ? "write" : "read");
        p += done;
        n -= (size_t)done;
    }
}

/** Open an OSS direction at 48 kHz, signed 16-bit mono, with small periods. */
static int dsp(int flags)
{
    int fd = open("/dev/dsp", flags), format = AFMT_S16_LE, channels = 1, rate = RATE;
    int fragment = (16 << 16) | 10;

    if (fd < 0 || ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 ||
        ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0 || ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0 ||
        ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &fragment) < 0 || format != AFMT_S16_LE ||
        channels != 1 || rate != RATE)
        fail("configure dsp");
    return fd;
}

/** Reap child `slot` and require a successful exit. */
static void reap(unsigned slot)
{
    int status;
    pid_t pid;

    do {
        pid = waitpid(children[slot], &status, 0);
    } while (pid < 0 && errno == EINTR);
    if (pid != children[slot])
        fail("waitpid");
    children[slot] = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EIO;
        fail("child failed");
    }
}

/** Make two seconds of a square wave, or a five-note scale when `hz` is 0. */
static short *wave(unsigned hz)
{
    static const unsigned scale[] = {262, 294, 330, 349, 392};
    short *samples = malloc(FRAMES * sizeof(*samples));
    unsigned i, phase = 0, frequency;

    if (!samples)
        fail("allocate samples");
    for (i = 0; i < FRAMES; i++) {
        frequency = hz ? hz : scale[i / (FRAMES / 5)];
        samples[i] = phase < RATE / 2 ? 6000 : -6000;
        phase = (phase + frequency) % RATE;
    }
    return samples;
}

/** Start a player held on pipe `gate` after opening its own DSP descriptor. */
static void player(unsigned slot, unsigned hz, int gate[2], int ready[2])
{
    children[slot] = fork();
    if (children[slot] < 0)
        fail("fork player");
    if (children[slot] == 0) {
        short *samples;
        char byte = 'r';
        int fd;

        memset(children, 0, sizeof(children));
        close(gate[1]);
        close(ready[0]);
        fd = dsp(O_WRONLY);
        samples = wave(hz);
        transfer(ready[1], &byte, 1, 1);
        transfer(gate[0], &byte, 1, 0);
        transfer(fd, samples, FRAMES * sizeof(*samples), 1);
        if (ioctl(fd, SNDCTL_DSP_SYNC) < 0)
            fail("drain player");
        close(fd);
        free(samples);
        _exit(0);
    }
}

/** Print a mono recording for the host's independent WAV checker. */
static void dump(const unsigned char *samples, size_t bytes)
{
    static const char digits[] = "0123456789abcdef";
    static const char prefix[] = "audioclients: data: ";
    char line[160];
    size_t at, i, used;

    puts("audioclients: record: 16-bit mono at 48000 hz");
    for (at = 0; at < bytes; at += 64) {
        memcpy(line, prefix, sizeof(prefix) - 1);
        used = sizeof(prefix) - 1;
        for (i = at; i < bytes && i < at + 64; i++) {
            line[used++] = digits[samples[i] >> 4];
            line[used++] = digits[samples[i] & 15];
        }
        line[used++] = '\n';
        transfer(STDOUT_FILENO, line, used, 1);
    }
}

/** Play two tones together, or capture a scale through two independent
 * loopback readers and require identical samples from both. */
static void clients(int loopback)
{
    int gate[2], ready[2], in[2] = {-1, -1}, value = PCM_ENABLE_INPUT;
    char bytes[2] = {'g', 'g'};
    unsigned char *recorded[2] = {NULL, NULL};
    size_t at, n = FRAMES * sizeof(short);
    unsigned i;

    if (pipe(gate) < 0 || pipe(ready) < 0)
        fail("pipes");
    if (loopback) {
        for (i = 0; i < 2; i++) {
            in[i] = dsp(O_RDONLY);
            recorded[i] = malloc(n);
            if (!recorded[i] || ioctl(in[i], SNDCTL_DSP_SETTRIGGER, &value) < 0)
                fail("start reader");
        }
    }
    player(0, loopback ? 0 : 440, gate, ready);
    if (!loopback)
        player(1, 660, gate, ready);
    close(ready[1]);
    close(gate[0]);
    transfer(ready[0], bytes, loopback ? 1 : 2, 0);
    transfer(gate[1], bytes, loopback ? 1 : 2, 1);
    close(ready[0]);
    close(gate[1]);
    if (loopback) {
        /* The first clock periods precede the player. Both readers start
         * at the same boot position while only silence is being mixed. */
        for (at = 0; at < n; at += 512) {
            transfer(in[0], recorded[0] + at, 512, 0);
            transfer(in[1], recorded[1] + at, 512, 0);
        }
        value = 0;
        for (i = 0; i < 2; i++) {
            if (ioctl(in[i], SNDCTL_DSP_SETTRIGGER, &value) < 0)
                fail("stop reader");
            close(in[i]);
        }
        {
            size_t a = 0, b = 0, overlap;

            while (a + 1 < n && recorded[0][a] == 0 && recorded[0][a + 1] == 0)
                a += 2;
            while (b + 1 < n && recorded[1][b] == 0 && recorded[1][b + 1] == 0)
                b += 2;
            overlap = n - (a > b ? a : b);
            if (overlap < RATE || memcmp(recorded[0] + a, recorded[1] + b, overlap) != 0) {
                errno = EIO;
                fail("shared capture differs");
            }
        }
        dump(recorded[0], n);
        free(recorded[0]);
        free(recorded[1]);
    }
    reap(0);
    if (!loopback)
        reap(1);
    puts("audioclients: result: complete");
}

/** Write little-endian 32-bit `n` in a WAV header. */
static void put32(unsigned char *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (8 * i));
}

/** Make a mono PCM WAV, convert it to a 24-bit FLAC with sox(1), and play the
 * FLAC with play(1) while rec(1) records the playback mix. */
static void sox(void)
{
    unsigned char header[44] = {0};
    short *samples = wave(0);
    int fd, status;
    pid_t pid;
    FILE *f;
    unsigned char *recorded;
    size_t got;

    memcpy(header, "RIFF", 4);
    put32(header + 4, 36 + FRAMES * 2);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);
    header[20] = header[22] = 1;
    put32(header + 24, RATE);
    put32(header + 28, RATE * 2);
    header[32] = 2;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    put32(header + 40, FRAMES * 2);
    fd = open(files[0], O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        fail("create WAV");
    transfer(fd, header, sizeof(header), 1);
    transfer(fd, samples, FRAMES * sizeof(*samples), 1);
    close(fd);
    free(samples);
    children[2] = fork();
    if (children[2] < 0)
        fail("fork sox");
    if (children[2] == 0) {
        memset(children, 0, sizeof(children));
        execl("/bin/sox", "sox", files[0], "-b", "24", files[2], (char *)NULL);
        _exit(127);
    }
    reap(2);
    /* A reader primes the playback clock before rec and play start. The
     * parent waits for rec to create its output before starting play. */
    fd = dsp(O_RDONLY);
    status = PCM_ENABLE_INPUT;
    if (ioctl(fd, SNDCTL_DSP_SETTRIGGER, &status) < 0)
        fail("start clock");
    children[0] = fork();
    if (children[0] < 0)
        fail("fork rec");
    if (children[0] == 0) {
        memset(children, 0, sizeof(children));
        close(fd);
        execl("/bin/rec", "rec", "-q", "-r", "48000", "-c", "1", "-b", "16", "-t", "raw", "-e",
              "signed-integer", "-L", files[1], "trim", "0", "144000s", (char *)NULL);
        _exit(127);
    }
    /* GETIPTR advances on the device clock, so this wait measures frames,
     * never elapsed host time. Give rec a clock period before play. */
    do {
        struct count_info count;

        if (ioctl(fd, SNDCTL_DSP_GETIPTR, &count) < 0)
            fail("clock position");
        if (access(files[1], F_OK) == 0)
            break;
        pid = waitpid(children[0], &status, WNOHANG);
        if (pid != 0)
            fail("rec ended before creating its output");
        {
            unsigned char block[512];

            transfer(fd, block, sizeof(block), 0);
        }
    } while (1);
    children[1] = fork();
    if (children[1] < 0)
        fail("fork play");
    if (children[1] == 0) {
        memset(children, 0, sizeof(children));
        close(fd);
        execl("/bin/play", "play", "-q", files[2], (char *)NULL);
        _exit(127);
    }
    close(fd);
    reap(1);
    reap(0);
    f = fopen(files[1], "rb");
    if (!f)
        fail("open recording");
    recorded = malloc(RATE * 6);
    if (!recorded)
        fail("allocate recording");
    got = fread(recorded, 1, RATE * 6, f);
    if (!got || ferror(f))
        fail("read recording");
    fclose(f);
    dump(recorded, got);
    free(recorded);
    puts("audioclients: result: complete");
}

/** Run the machine case's requested mode, with cleanup installed first. */
int main(int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR");

    if (atexit(cleanup) != 0)
        return 1;
    signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted);
    signal(SIGPIPE, interrupted);
    if (snprintf(workspace, sizeof(workspace), "%s/rach2-audioclients.XXXXXX",
                 tmp ? tmp : "/tmp") >= (int)sizeof(workspace))
        return 1;
    if (!mkdtemp(workspace))
        fail("scratch directory");
    snprintf(files[0], sizeof(files[0]), "%s/played.wav", workspace);
    snprintf(files[1], sizeof(files[1]), "%s/recorded.raw", workspace);
    snprintf(files[2], sizeof(files[2]), "%s/played.flac", workspace);
    if (argc != 2)
        return 1;
    if (setenv("AUDIODRIVER", "oss", 1) < 0)
        fail("select OSS");
    if (strcmp(argv[1], "mix") == 0)
        clients(0);
    else if (strcmp(argv[1], "loopback") == 0)
        clients(1);
    else if (strcmp(argv[1], "sox") == 0)
        sox();
    else
        return 1;
    return 0;
}
