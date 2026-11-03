/*
 * Exercise concurrent OSS players, shared capture, the mix's rate conversion
 * and sox_ng's loopback, and stop play(1) on a terminal with Ctrl+C and
 * SIGTERM. The machine case judges the WAV samples printed as hexadecimal
 * lines and the lines of the result.
 */
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RATE   48000
#define FRAMES (RATE * 2)

/* The file the signal trials play: two minutes at 8000 Hz, which play is
 * still playing when a trial stops it. */
#define LONG_RATE   8000
#define LONG_FRAMES (LONG_RATE * 120)

/* The rate the resample case plays at, twice the mix's, the tone it plays
 * above the mix's Nyquist frequency, which would fold to 18000 Hz, and the
 * tone's amplitude, 6 dB below full scale. */
#define HIGH_RATE 96000
#define HIGH_TONE 30000
#define HIGH_AMP  16384

/* A trial's wait for play, in seconds of the kernel's ticks. */
#define TRIAL_SECONDS 30

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

/** Open an OSS direction at `want` Hz, signed 16-bit mono, with small
 * periods. */
static int dsp_at(int flags, int want)
{
    int fd = open("/dev/dsp", flags), format = AFMT_S16_LE, channels = 1, rate = want;
    int fragment = (16 << 16) | 10;

    if (fd < 0 || ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 ||
        ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0 || ioctl(fd, SNDCTL_DSP_SPEED, &rate) < 0 ||
        ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &fragment) < 0 || format != AFMT_S16_LE ||
        channels != 1 || rate != want)
        fail("configure dsp");
    return fd;
}

/** Open an OSS direction at 48 kHz, signed 16-bit mono, with small periods. */
static int dsp(int flags)
{
    return dsp_at(flags, RATE);
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

/** Make two seconds at `rate` of a square wave, or a five-note scale when
 * `hz` is 0. */
static short *wave_at(unsigned hz, unsigned rate)
{
    static const unsigned scale[] = {262, 294, 330, 349, 392};
    unsigned frames = 2 * rate, i, phase = 0, frequency;
    short *samples = malloc(frames * sizeof(*samples));

    if (!samples)
        fail("allocate samples");
    for (i = 0; i < frames; i++) {
        frequency = hz ? hz : scale[i / (frames / 5)];
        samples[i] = phase < rate / 2 ? 6000 : -6000;
        phase = (phase + frequency) % rate;
    }
    return samples;
}

/** Make two seconds at 48 kHz of a square wave, or the scale when `hz` is 0. */
static short *wave(unsigned hz)
{
    return wave_at(hz, RATE);
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

    fflush(stdout);
    puts("audioclients: record: 16-bit mono at 48000 hz");
    fflush(stdout);
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

/**
 * Play `frames` of 16-bit mono `samples` at `rate` from a child of its own
 * while a loopback reader at 48 kHz, started before it, reads `bytes`, and
 * print what the reader read.
 */
static void capture_play(const short *samples, unsigned frames, int rate, size_t bytes)
{
    unsigned char *recorded = malloc(bytes);
    int in = dsp(O_RDONLY), out, value = PCM_ENABLE_INPUT;
    size_t at;

    if (!recorded || ioctl(in, SNDCTL_DSP_SETTRIGGER, &value) < 0)
        fail("start reader");
    children[0] = fork();
    if (children[0] < 0)
        fail("fork player");
    if (children[0] == 0) {
        memset(children, 0, sizeof(children));
        close(in);
        out = dsp_at(O_WRONLY, rate);
        transfer(out, (void *)samples, frames * sizeof(*samples), 1);
        if (ioctl(out, SNDCTL_DSP_SYNC) < 0)
            fail("drain player");
        close(out);
        _exit(0);
    }
    for (at = 0; at < bytes; at += 512)
        transfer(in, recorded + at, 512, 0);
    value = 0;
    if (ioctl(in, SNDCTL_DSP_SETTRIGGER, &value) < 0)
        fail("stop reader");
    close(in);
    reap(0);
    dump(recorded, bytes);
    free(recorded);
}

/**
 * Play a second of a 30 kHz sine at 96 kHz, above the 48 kHz mix's Nyquist
 * frequency, which the mix's filter removes and a converter without one
 * folds to 18 kHz, and then the scale at 96 kHz, each read through loopback
 * at the mix's rate over a second more than it plays and printed as a
 * record of its own. The sine has five periods in 16 frames, so a table of
 * one turn gives it without libm. It fades in and out over a tenth of a
 * second, since a sine that starts or stops at once clicks, and the click
 * lies in the band.
 */
static void resample(void)
{
    static const short turn[16] = {0, 6270,  11585,  15137,  HIGH_AMP, 15137,  11585,  6270,
                                   0, -6270, -11585, -15137, -16384,   -15137, -11585, -6270};
    short *tone = malloc(HIGH_RATE * sizeof(*tone)), *scale;
    unsigned i, fade = HIGH_RATE / 10, gain;

    if (!tone)
        fail("allocate samples");
    for (i = 0; i < HIGH_RATE; i++) {
        gain = i < fade ? i : HIGH_RATE - 1 - i < fade ? HIGH_RATE - 1 - i : fade;
        tone[i] = (short)(turn[(i * (HIGH_TONE / 6000)) % 16] * (int)gain / (int)fade);
    }
    capture_play(tone, HIGH_RATE, HIGH_RATE, 2 * RATE * sizeof(short));
    free(tone);
    scale = wave_at(0, HIGH_RATE);
    capture_play(scale, 2 * HIGH_RATE, HIGH_RATE, 3 * RATE * sizeof(short));
    free(scale);
    puts("audioclients: result: complete");
}

/** Write little-endian 32-bit `n` in a WAV header. */
static void put32(unsigned char *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (8 * i));
}

/** Write `frames` 16-bit mono `samples` at `rate` Hz as a PCM WAV at `path`. */
static void write_wav(const char *path, const short *samples, unsigned frames, unsigned rate)
{
    unsigned char header[44] = {0};
    int fd;

    memcpy(header, "RIFF", 4);
    put32(header + 4, 36 + frames * 2);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16);
    header[20] = header[22] = 1;
    put32(header + 24, rate);
    put32(header + 28, rate * 2);
    header[32] = 2;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    put32(header + 40, frames * 2);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        fail("create WAV");
    transfer(fd, header, sizeof(header), 1);
    transfer(fd, (void *)samples, frames * sizeof(*samples), 1);
    close(fd);
}

/** Make a mono PCM WAV, convert it to a 24-bit FLAC with sox(1), and play the
 * FLAC with play(1) while rec(1) records the playback mix. */
static void sox(void)
{
    short *samples = wave(0);
    int fd, status;
    pid_t pid;
    FILE *f;
    unsigned char *recorded;
    size_t got;

    write_wav(files[0], samples, FRAMES, RATE);
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

/* What play has written to its terminal in the current trial. */
static char said[65536];
static size_t heard;

/** Give the kernel's count of clock ticks since boot. */
static clock_t ticks(void)
{
    struct tms t;

    return times(&t);
}

/** Read what play wrote to terminal `master`, if anything, into `said`.
 * @return 0 once `said` is full. */
static int listen_once(int master, int wait_ms)
{
    struct pollfd p;
    ssize_t n;

    p.fd = master;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, wait_ms) < 0 && errno != EINTR)
        fail("poll terminal");
    if (!(p.revents & POLLIN))
        return 1;
    n = read(master, said + heard, sizeof(said) - 1 - heard);
    if (n < 0 && errno != EINTR && errno != EAGAIN)
        fail("read terminal");
    if (n > 0)
        heard += (size_t)n;
    said[heard] = '\0';
    return heard < sizeof(said) - 1;
}

/**
 * Read play's terminal `master` until `text` appears at or after `*from`,
 * and leave `*from` just past it; with `text` NULL, until play has exited,
 * leaving its wait status in `*status`. The wait counts the kernel's ticks.
 *
 * @return 1 when it happened; 0 when play exited before `text`, the
 * transcript filled, or TRIAL_SECONDS of ticks passed.
 */
static int hear(int master, const char *text, size_t *from, int *status)
{
    clock_t start = ticks(), limit = (clock_t)sysconf(_SC_CLK_TCK) * TRIAL_SECONDS;
    const char *at;
    pid_t pid;

    while (ticks() - start < limit) {
        if (text && (at = strstr(said + *from, text)) != NULL) {
            *from = (size_t)(at - said) + strlen(text);
            return 1;
        }
        pid = waitpid(children[0], status, WNOHANG);
        if (pid < 0 && errno != EINTR)
            fail("wait for play");
        if (pid == children[0]) {
            size_t before;

            children[0] = 0;
            do {
                before = heard;
            } while (listen_once(master, 0) && heard != before);
            return text == NULL || strstr(said + *from, text) != NULL;
        }
        if (!listen_once(master, 100))
            return 0;
    }
    return 0;
}

/** Count the occurrences of `text` in the trial's transcript. */
static unsigned occurrences(const char *text)
{
    const char *at = said;
    unsigned n = 0;

    while ((at = strstr(at, text)) != NULL) {
        n++;
        at += strlen(text);
    }
    return n;
}

/** Report trial `name` as failed for `why`, with the end of the transcript,
 * its control characters shown as dots. */
static void failed(const char *name, const char *why)
{
    size_t at = heard > 300 ? heard - 300 : 0, i;
    char tail[301];

    for (i = 0; at + i < heard; i++)
        tail[i] = (unsigned char)said[at + i] < ' ' ? '.' : said[at + i];
    tail[i] = '\0';
    printf("audioclients: %s: %s; the terminal said: %s\n", name, why, tail);
}

/**
 * Start play(1) on `files[0]` in a new session whose controlling terminal
 * is the pseudo-terminal `slave`, as from an SSH session; its pid goes in
 * children[0]. `master` is the terminal's other side, which the child
 * closes.
 */
static void start_play(int master, int slave)
{
    children[0] = fork();
    if (children[0] < 0)
        fail("fork play");
    if (children[0] == 0) {
        memset(children, 0, sizeof(children));
        close(master);
        if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 || dup2(slave, 0) < 0 ||
            dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
            _exit(126);
        if (slave > 2)
            close(slave);
        execl("/bin/play", "play", files[0], (char *)NULL);
        _exit(127);
    }
}

/**
 * Stop play, which trial `name` runs on the terminal `master`, by typing
 * `keys`, or by sending `signo` when `keys` is null, and judge how it
 * ended; `*from` and `*status` are hear's.
 *
 * @return 1 when play printed "Aborted." once, warned of the unbound key
 * once, and exited 0; 0 after reporting the failure.
 */
static int stop_play(const char *name, int master, const char *keys, int signo, size_t *from,
                     int *status)
{
    if (keys) {
        if (write(master, keys, strlen(keys)) != (ssize_t)strlen(keys))
            fail("type Ctrl+C");
    } else if (kill(children[0], signo) < 0) {
        fail("signal play");
    }
    if (!hear(master, NULL, from, status))
        failed(name, "play did not exit");
    else if (!WIFEXITED(*status) || WEXITSTATUS(*status) != 0)
        failed(name, "play did not exit 0");
    else if (!strstr(said, "Aborted."))
        failed(name, "play did not say Aborted.");
    else if (occurrences("doesn't do anything") != 1)
        failed(name, "play warned of a key more than once");
    else
        return 1;
    return 0;
}

/**
 * Run play(1) on `files[0]` with a new pseudo-terminal for its controlling
 * terminal, as from an SSH session, and stop it by typing `keys` or by
 * sending `signo`. Before the stop, a key play does not bind must draw one
 * warning and let play go on.
 *
 * @return 1 when play printed "Aborted." and exited 0, as on Linux, where
 * sox_ng's handler of SIGINT and SIGTERM sets a flag its play loop reads.
 */
static int trial(const char *name, const char *keys, int signo)
{
    struct winsize size = {24, 80, 0, 0};
    int master, slave, status = 0, ok = 0;
    const char *path;
    size_t from = 0;

    heard = 0;
    said[0] = '\0';
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
        (path = ptsname(master)) == NULL)
        fail("open a terminal");
    slave = open(path, O_RDWR | O_NOCTTY);
    if (slave < 0 || ioctl(slave, TIOCSWINSZ, &size) < 0)
        fail("open the terminal's slave");
    start_play(master, slave);
    /* Two status lines: play's flow runs, with its key reader on. */
    if (!hear(master, "In:", &from, &status) || !hear(master, "In:", &from, &status))
        failed(name, "play did not show its progress");
    else if (write(master, "x", 1) != 1)
        fail("type a key");
    else if (!hear(master, "doesn't do anything", &from, &status) ||
             !hear(master, "In:", &from, &status))
        failed(name, "play did not go on after one unbound key");
    else
        ok = stop_play(name, master, keys, signo, &from, &status);
    if (children[0] > 0) {
        kill(children[0], SIGKILL);
        while (waitpid(children[0], NULL, 0) < 0 && errno == EINTR) {
        }
        children[0] = 0;
    }
    close(slave);
    close(master);
    if (ok)
        printf("audioclients: result: %s stopped play\n", name);
    return ok;
}

/** Stop play on a terminal with one Ctrl+C, two at once and SIGTERM. */
static void signals(void)
{
    short *samples = malloc(LONG_FRAMES * sizeof(*samples));
    unsigned i, phase = 0, passed = 0;

    if (!samples)
        fail("allocate samples");
    for (i = 0; i < LONG_FRAMES; i++) {
        samples[i] = phase < LONG_RATE / 2 ? 2000 : -2000;
        phase = (phase + 440) % LONG_RATE;
    }
    write_wav(files[0], samples, LONG_FRAMES, LONG_RATE);
    free(samples);
    fflush(stdout);
    passed += trial("ctrl-c", "\003", 0);
    passed += trial("ctrl-c-twice", "\003\003", 0);
    passed += trial("sigterm", NULL, SIGTERM);
    if (passed == 3)
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
    else if (strcmp(argv[1], "resample") == 0)
        resample();
    else if (strcmp(argv[1], "signal") == 0)
        signals();
    else
        return 1;
    return 0;
}
