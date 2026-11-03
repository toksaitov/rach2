/**
 * mixer: read or set the levels and the record source of /dev/mixer.
 *
 * `mixer` prints each channel of the device as `NAME L:R`, the left and the
 * right channel's percentage, by OSS's names (vol, pcm, speaker, line, mic,
 * rec, igain, monitor), with ` muted` after the volume while it is muted and
 * ` rec` after the source recorded; `mixer NAME` prints one channel.
 * `mixer NAME N` sets both channels to N, `mixer NAME L:R` each its own,
 * and `mixer NAME +N` and `mixer NAME -N` move both by N, kept between 0
 * and 100. `mixer =rec NAME` records from source NAME, as FreeBSD's
 * mixer(8) spells it. `mixer mute` sets the volume to 0 and records the
 * level it had in `MUTE_RECORD`, and `mixer unmute` sets the recorded
 * level back.
 *
 * The device is OSS's, which has no mute of its own, so the mute is the
 * record: the volume is muted while the record is there and the level is 0.
 * A level set with `vol` removes the record, so that a mute key after a
 * volume key mutes again rather than unmuting to a level from before.
 * acpid's volume action (/etc/acpi/volume) runs this program for the keys.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* OSS's requests, with Linux's numbers. musl's <sys/soundcard.h> takes
 * them from Linux's <linux/soundcard.h>, which the guest's musl does not
 * install. */
#define MIXER_READ(n)         _IOR('M', n, int)
#define MIXER_WRITE(n)        _IOWR('M', n, int)
#define SOUND_MIXER_NRDEVICES 25
#define SOUND_MIXER_VOLUME    0
#define SOUND_MIXER_RECSRC    0xFF
#define SOUND_MIXER_DEVMASK   0xFE
#define SOUND_MIXER_RECMASK   0xFD

/* OSS's names of its channels, SOUND_DEVICE_NAMES, by number. */
static const char *const names[SOUND_MIXER_NRDEVICES] = {
    "vol",  "bass", "treble", "synth", "pcm",   "speaker", "line",   "mic",   "cd",
    "mix",  "pcm2", "rec",    "igain", "ogain", "line1",   "line2",  "line3", "dig1",
    "dig2", "dig3", "phin",   "phout", "video", "radio",   "monitor"};

#define MIXER_DEVICE "/dev/mixer"

/* The level before a mute, as `L:R` and a newline. init keeps it across a
 * reboot with the level (init(8)). */
#define MUTE_RECORD "/var/run/mixer.mute"

/** Pack the channels `left` and `right`, each 0 to 100, as OSS's level. */
static int pack(int left, int right)
{
    return left | right << 8;
}

/** Read the int of request `request` through the open mixer `fd` into
 *  `*value`. Returns 0, or 1 after a diagnostic. */
static int get_int(int fd, unsigned long request, int *value)
{
    if (ioctl(fd, request, value) < 0) {
        perror("mixer: " MIXER_DEVICE);
        return 1;
    }
    return 0;
}

/**
 * Read channel `ch`'s level through the open mixer `fd` into `*left` and
 * `*right`. Returns 0, or 1 after a diagnostic.
 */
static int get_level(int fd, int ch, int *left, int *right)
{
    int level;

    if (ioctl(fd, MIXER_READ(ch), &level) < 0) {
        perror("mixer: " MIXER_DEVICE);
        return 1;
    }
    *left = level & 0xff;
    *right = level >> 8 & 0xff;
    return 0;
}

/** Set channel `ch`'s level through the open mixer `fd`. Returns 0, or 1
 *  after a diagnostic. */
static int set_level(int fd, int ch, int left, int right)
{
    int level = pack(left, right);

    if (ioctl(fd, MIXER_WRITE(ch), &level) < 0) {
        perror("mixer: " MIXER_DEVICE);
        return 1;
    }
    return 0;
}

/**
 * Parse a percentage, 0 to 100, from `s`, which ends at `end` or at its
 * terminator when `end` is NULL. Returns it, or -1 when `s` is not one.
 */
static int percentage(const char *s, const char *end)
{
    int v = 0;

    if (s == end || *s == '\0') {
        return -1;
    }
    for (; s != end && *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        v = v * 10 + (*s - '0');
        if (v > 100) {
            return -1;
        }
    }
    return v;
}

/**
 * Read the record of a mute into `*left` and `*right`. Returns 1 when there
 * is one that parses, 0 otherwise.
 */
static int read_record(int *left, int *right)
{
    char text[16];
    char *colon;
    ssize_t n;
    int fd = open(MUTE_RECORD, O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = '\0';
    text[strcspn(text, "\n")] = '\0';
    colon = strchr(text, ':');
    if (!colon) {
        return 0;
    }
    *left = percentage(text, colon);
    *right = percentage(colon + 1, NULL);
    return *left >= 0 && *right >= 0;
}

/** Record the level `left`:`right` as the one before a mute. Returns 0, or 1
 *  after a diagnostic. */
static int write_record(int left, int right)
{
    char text[16];
    int len = snprintf(text, sizeof(text), "%d:%d\n", left, right);
    int fd = open(MUTE_RECORD, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0 || write(fd, text, (size_t)len) != len) {
        perror("mixer: " MUTE_RECORD);
        if (fd >= 0) {
            close(fd);
        }
        return 1;
    }
    close(fd);
    return 0;
}

/** Whether the volume is muted: the record is there and the level is 0. */
static int muted(int left, int right)
{
    int l, r;

    return left == 0 && right == 0 && read_record(&l, &r);
}

/**
 * Print channel `ch` of the open mixer `fd`: its level when the device
 * mask `devmask` has it, ` muted` after the volume while muted, and ` rec`
 * when the record source `recsrc` is it. Returns 0, or 1 on a failure.
 */
static int show(int fd, int ch, int devmask, int recsrc)
{
    int left, right;

    printf("%s", names[ch]);
    if (devmask & 1 << ch) {
        if (get_level(fd, ch, &left, &right) != 0) {
            return 1;
        }
        printf(" %d:%d", left, right);
        if (ch == SOUND_MIXER_VOLUME && muted(left, right)) {
            printf(" muted");
        }
    }
    printf("%s\n", recsrc & 1 << ch ? " rec" : "");
    return 0;
}

/** Move `level` by `delta` and keep it between 0 and 100. */
static int step(int level, int delta)
{
    level += delta;
    return level < 0 ? 0 : level > 100 ? 100 : level;
}

/**
 * Set channel `ch`'s level as `arg` says: `N`, `L:R`, `+N` or `-N`, from
 * the level it has now. Setting the volume removes the record of a mute.
 * Returns 0, 1 on a failure and 2 when `arg` is not a level.
 */
static int set_channel(int fd, int ch, const char *arg)
{
    const char *colon = strchr(arg, ':');
    int l, r, left, right;

    if (get_level(fd, ch, &left, &right) != 0) {
        return 1;
    }
    if (arg[0] == '+' || arg[0] == '-') {
        int delta = percentage(arg + 1, NULL);

        if (delta < 0) {
            return 2;
        }
        if (arg[0] == '-') {
            delta = -delta;
        }
        l = step(left, delta);
        r = step(right, delta);
    } else if (colon) {
        l = percentage(arg, colon);
        r = percentage(colon + 1, NULL);
    } else {
        l = r = percentage(arg, NULL);
    }
    if (l < 0 || r < 0) {
        return 2;
    }
    if (set_level(fd, ch, l, r) != 0) {
        return 1;
    }
    if (ch == SOUND_MIXER_VOLUME) {
        (void)unlink(MUTE_RECORD);
    }
    return 0;
}

/** Mute: record the level `left`:`right` and set 0. Muting while muted
 *  changes nothing. Returns 0, or 1 on a failure. */
static int mute(int fd, int left, int right)
{
    if (muted(left, right)) {
        return 0;
    }
    if (write_record(left, right) != 0) {
        return 1;
    }
    return set_level(fd, SOUND_MIXER_VOLUME, 0, 0);
}

/** Unmute: set the recorded level back and remove the record. Unmuting
 *  while not muted changes nothing. Returns 0, or 1 on a failure. */
static int unmute(int fd, int left, int right)
{
    int l, r;

    if (!muted(left, right) || !read_record(&l, &r)) {
        return 0;
    }
    if (set_level(fd, SOUND_MIXER_VOLUME, l, r) != 0) {
        return 1;
    }
    (void)unlink(MUTE_RECORD);
    return 0;
}

/** The channel named `name`, or -1. */
static int channel(const char *name)
{
    int i;

    for (i = 0; i < SOUND_MIXER_NRDEVICES; i++) {
        if (strcmp(names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/** Record from the source named `name`, one of the record mask `recmask`.
 *  Returns 0, 1 on a failure and 2 when `name` is not a source. */
static int set_source(int fd, const char *name, int recmask)
{
    int ch = channel(name), mask;

    if (ch < 0 || !(recmask & 1 << ch)) {
        fprintf(stderr, "mixer: %s: not a source\n", name);
        return 2;
    }
    mask = 1 << ch;
    if (ioctl(fd, MIXER_WRITE(SOUND_MIXER_RECSRC), &mask) < 0) {
        perror("mixer: " MIXER_DEVICE);
        return 1;
    }
    if (mask != 1 << ch) {
        fprintf(stderr, "mixer: %s: not taken\n", name);
        return 1;
    }
    return 0;
}

/**
 * Check the arguments against the usage. Returns the channel the first
 * argument names, -1 for a command that names none, or -2 on a usage error.
 */
static int parse(int argc, char **argv)
{
    int ch = -1;

    if (argc == 2 && strcmp(argv[1], "mute") != 0 && strcmp(argv[1], "unmute") != 0) {
        ch = channel(argv[1]);
    } else if (argc == 3 && strcmp(argv[1], "=rec") != 0) {
        ch = channel(argv[1]);
    }
    if (argc == 1 || (argc == 3 && strcmp(argv[1], "=rec") == 0) ||
        ((argc == 2 || argc == 3) && ch >= 0)) {
        return ch;
    }
    if (argc == 2 && (strcmp(argv[1], "mute") == 0 || strcmp(argv[1], "unmute") == 0)) {
        return -1;
    }
    return -2;
}

/**
 * Run on the open mixer `fd` the command the arguments name, whose channel
 * `parse` found as `ch`. Returns 0, 1 on a failure and 2 on a usage error.
 */
static int run(int fd, int argc, char **argv, int ch)
{
    int left, right, devmask, recmask, recsrc, i, status = 0;

    if (get_int(fd, MIXER_READ(SOUND_MIXER_DEVMASK), &devmask) != 0 ||
        get_int(fd, MIXER_READ(SOUND_MIXER_RECMASK), &recmask) != 0 ||
        get_int(fd, MIXER_READ(SOUND_MIXER_RECSRC), &recsrc) != 0) {
        return 1;
    }
    if (ch >= 0 && !((devmask | recmask) & 1 << ch)) {
        fprintf(stderr, "mixer: %s: no such channel\n", argv[1]);
        return 2;
    }
    if (argc == 1) {
        for (i = 0; i < SOUND_MIXER_NRDEVICES && status == 0; i++) {
            if ((devmask | recmask) & 1 << i) {
                status = show(fd, i, devmask, recsrc);
            }
        }
    } else if (ch >= 0 && argc == 2) {
        status = show(fd, ch, devmask, recsrc);
    } else if (ch >= 0) {
        if (!(devmask & 1 << ch)) {
            fprintf(stderr, "mixer: %s: has no level\n", argv[1]);
            status = 2;
        } else if ((status = set_channel(fd, ch, argv[2])) == 2) {
            fprintf(stderr, "mixer: %s: not a level\n", argv[2]);
        }
    } else if (strcmp(argv[1], "=rec") == 0) {
        status = set_source(fd, argv[2], recmask);
    } else if (get_level(fd, SOUND_MIXER_VOLUME, &left, &right) != 0) {
        status = 1;
    } else if (strcmp(argv[1], "mute") == 0) {
        status = mute(fd, left, right);
    } else {
        status = unmute(fd, left, right);
    }
    return status;
}

/** Run the command the arguments name. Exits 0, 1 on a failure and 2 on a
 *  usage error. */
int main(int argc, char **argv)
{
    int fd, status, ch = parse(argc, argv);

    if (ch == -2) {
        fputs("usage: mixer [NAME [N | L:R | +N | -N] | =rec NAME | mute | unmute]\n", stderr);
        return 2;
    }
    fd = open(MIXER_DEVICE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "mixer: " MIXER_DEVICE ": %s\n", strerror(errno));
        return 1;
    }
    status = run(fd, argc, argv, ch);
    close(fd);
    return status;
}
