/**
 * mixer: read or set the master volume through /dev/mixer.
 *
 * `mixer` prints the level as `vol L:R`, the left and the right channel's
 * percentage, with ` muted` after it while muted. `mixer vol N` sets both
 * channels to N, `mixer vol L:R` each its own, and `mixer vol +N` and
 * `mixer vol -N` move both by N, kept between 0 and 100. `mixer mute` sets
 * the level to 0 and records the level it had in `MUTE_RECORD`, and
 * `mixer unmute` sets the recorded level back.
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

/* OSS's requests for the master volume, with Linux's numbers. musl's
 * <sys/soundcard.h> takes them from Linux's <linux/soundcard.h>, which the
 * guest's musl does not install. */
#define SOUND_MIXER_READ_VOLUME  _IOR('M', 0, int)
#define SOUND_MIXER_WRITE_VOLUME _IOWR('M', 0, int)

#define MIXER_DEVICE "/dev/mixer"

/* The level before a mute, as `L:R` and a newline. init keeps it across a
 * reboot with the level (init(8)). */
#define MUTE_RECORD "/var/run/mixer.mute"

/** Pack the channels `left` and `right`, each 0 to 100, as OSS's level. */
static int pack(int left, int right)
{
    return left | right << 8;
}

/**
 * Read the level through the open mixer `fd` into `*left` and `*right`.
 * Returns 0, or 1 after a diagnostic.
 */
static int get_level(int fd, int *left, int *right)
{
    int level;

    if (ioctl(fd, SOUND_MIXER_READ_VOLUME, &level) < 0) {
        perror("mixer: " MIXER_DEVICE);
        return 1;
    }
    *left = level & 0xff;
    *right = level >> 8 & 0xff;
    return 0;
}

/** Set the level through the open mixer `fd`. Returns 0, or 1 after a
 *  diagnostic. */
static int set_level(int fd, int left, int right)
{
    int level = pack(left, right);

    if (ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &level) < 0) {
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

/** Print the level, and ` muted` while muted. Returns 0. */
static int show(int left, int right)
{
    printf("vol %d:%d%s\n", left, right, muted(left, right) ? " muted" : "");
    return 0;
}

/** Move `level` by `delta` and keep it between 0 and 100. */
static int step(int level, int delta)
{
    level += delta;
    return level < 0 ? 0 : level > 100 ? 100 : level;
}

/**
 * Set the level as `arg` says: `N`, `L:R`, `+N` or `-N`, from the level
 * `left`:`right` it has now. Returns 0, 1 on a failure and 2 when `arg` is
 * not a level.
 */
static int set_vol(int fd, const char *arg, int left, int right)
{
    const char *colon = strchr(arg, ':');
    int l, r;

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
    if (set_level(fd, l, r) != 0) {
        return 1;
    }
    (void)unlink(MUTE_RECORD);
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
    return set_level(fd, 0, 0);
}

/** Unmute: set the recorded level back and remove the record. Unmuting
 *  while not muted changes nothing. Returns 0, or 1 on a failure. */
static int unmute(int fd, int left, int right)
{
    int l, r;

    if (!muted(left, right) || !read_record(&l, &r)) {
        return 0;
    }
    if (set_level(fd, l, r) != 0) {
        return 1;
    }
    (void)unlink(MUTE_RECORD);
    return 0;
}

/** Run the command the arguments name. Exits 0, 1 on a failure and 2 on a
 *  usage error. */
int main(int argc, char **argv)
{
    int fd, left, right, status = 2;

    if (!(argc == 1 || (argc == 2 && strcmp(argv[1], "mute") == 0) ||
          (argc == 2 && strcmp(argv[1], "unmute") == 0) ||
          (argc == 3 && strcmp(argv[1], "vol") == 0))) {
        fputs("usage: mixer [vol N | vol L:R | vol +N | vol -N | mute | unmute]\n", stderr);
        return 2;
    }
    fd = open(MIXER_DEVICE, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "mixer: " MIXER_DEVICE ": %s\n", strerror(errno));
        return 1;
    }
    if (get_level(fd, &left, &right) != 0) {
        close(fd);
        return 1;
    }
    if (argc == 1) {
        status = show(left, right);
    } else if (strcmp(argv[1], "mute") == 0) {
        status = mute(fd, left, right);
    } else if (strcmp(argv[1], "unmute") == 0) {
        status = unmute(fd, left, right);
    } else {
        status = set_vol(fd, argv[2], left, right);
        if (status == 2) {
            fprintf(stderr, "mixer: %s: not a level\n", argv[2]);
        }
    }
    close(fd);
    return status;
}
