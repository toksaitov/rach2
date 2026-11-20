/**
 * mixercmd_hosttest.c: the locks of the mixer command, utils/mixer.c, over
 * a model of /dev/mixer, /var/run/mixer.lock and the user who runs it.
 *
 * `utils/mixer.c` is included whole, with open, close, flock, ioctl,
 * unlink, usleep and geteuid replaced by the functions below, so that a
 * row can choose who runs the command, make /var/run/mixer.lock fail to
 * open, as on a read-only /var/run, and have another user hold /dev/mixer.
 * The model's flock answers a blocking request on the held device with
 * EINTR and counts it, since the real call would wait for as long as the
 * holder keeps the lock; its usleep adds up the time slept and returns at
 * once. Rows check that root's `vol 55`, with MIXER_LOCK open and with it
 * failing, tries the held device's lock 100 times over a second and then
 * sets the level without it, and that an ordinary user waits for the
 * device's lock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The descriptors the model gives the device and the lock file. */
#define DEV_FD  1000
#define LOCK_FD 1001

static int failures;
static const char *row; /* the row under way, named in a failure */

/* The model: who runs the command, how the open of MIXER_LOCK fails (0 for
 * not), whether another user holds /dev/mixer, and what the command did. */
static struct {
    unsigned euid;
    int lock_errno;
    int held;
    int nonblocking; /* LOCK_NB tries of the device's lock */
    int blocking;    /* blocking requests of the device's lock */
    int locked;      /* the device's lock was granted */
    long slept_us;
    int volume;
} m;

/** Count and name a failure when `ok` is false. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("mixercmd-hosttest: fail: %s: %s\n", row, what);
        failures++;
    }
}

/* The stand-ins, defined after the include, where OSS's requests are. */
static int t_open(const char *path, int flags, ...);
static int t_close(int fd);
static int t_flock(int fd, int op);
static int t_ioctl(int fd, unsigned long request, void *arg);
static int t_unlink(const char *path);
static int t_usleep(useconds_t us);
static uid_t t_geteuid(void);

#define open    t_open
#define close   t_close
#define flock   t_flock
#define ioctl   t_ioctl
#define unlink  t_unlink
#define usleep  t_usleep
#define geteuid t_geteuid
#define main    mixer_main
#include "mixer.c"
#undef main
#undef geteuid
#undef usleep
#undef unlink
#undef ioctl
#undef flock
#undef close
#undef open

/** open(2) of the device, the lock file, and nothing else. */
static int t_open(const char *path, int flags, ...)
{
    (void)flags;
    if (strcmp(path, "/dev/mixer") == 0) {
        return DEV_FD;
    }
    if (strcmp(path, "/var/run/mixer.lock") == 0) {
        if (m.lock_errno != 0) {
            errno = m.lock_errno;
            return -1;
        }
        return LOCK_FD;
    }
    errno = ENOENT;
    return -1;
}

/** close(2) of the model's descriptors. */
static int t_close(int fd)
{
    return fd == DEV_FD || fd == LOCK_FD ? 0 : -1;
}

/** flock(2): MIXER_LOCK is always free; the device is busy while another
 *  user holds it, and a blocking request on it then gives EINTR. */
static int t_flock(int fd, int op)
{
    if (fd == LOCK_FD) {
        return 0;
    }
    if (op & LOCK_NB) {
        m.nonblocking++;
    } else {
        m.blocking++;
    }
    if (m.held) {
        errno = op & LOCK_NB ? EWOULDBLOCK : EINTR;
        return -1;
    }
    m.locked = 1;
    return 0;
}

/** ioctl(2) of OSS's mixer: a volume alone, and the volume's level. */
static int t_ioctl(int fd, unsigned long request, void *arg)
{
    int *value = arg;

    if (fd != DEV_FD) {
        errno = EBADF;
        return -1;
    }
    if (request == MIXER_READ(SOUND_MIXER_DEVMASK)) {
        *value = 1 << SOUND_MIXER_VOLUME;
    } else if (request == MIXER_READ(SOUND_MIXER_RECMASK) ||
               request == MIXER_READ(SOUND_MIXER_RECSRC)) {
        *value = 0;
    } else if (request == MIXER_READ(SOUND_MIXER_VOLUME)) {
        *value = m.volume;
    } else if (request == MIXER_WRITE(SOUND_MIXER_VOLUME)) {
        m.volume = *value;
    } else {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/** unlink(2) of the mute record, which is not there. */
static int t_unlink(const char *path)
{
    (void)path;
    errno = ENOENT;
    return -1;
}

/** usleep(3), which adds up the time and returns at once. */
static int t_usleep(useconds_t us)
{
    m.slept_us += (long)us;
    return 0;
}

/** geteuid(2), the row's user. */
static uid_t t_geteuid(void)
{
    return (uid_t)m.euid;
}

/** Run `mixer vol 55` as `euid`, with the open of MIXER_LOCK failing with
 *  `lock_errno` (0 for not) and the device held by another user when
 *  `held`. Returns the command's status. */
static int vol55(unsigned euid, int lock_errno, int held)
{
    char *argv[] = {"mixer", "vol", "55", NULL};

    memset(&m, 0, sizeof(m));
    m.euid = euid;
    m.lock_errno = lock_errno;
    m.held = held;
    m.volume = 50 | 50 << 8;
    return mixer_main(3, argv);
}

/** Root on a read-only /var/run, with a user holding /dev/mixer: the open
 *  of MIXER_LOCK fails, and root still tries the device for a second and
 *  goes on, as flock(LOCK_EX | LOCK_NB) gives Linux's EWOULDBLOCK, rather
 *  than waiting in a blocking flock for as long as the user holds it. */
static void row_root_readonly_run(void)
{
    int status;

    row = "root, /var/run read-only, device held";
    status = vol55(0, EROFS, 1);
    check(status == 0, "the command does not exit 0");
    check(m.blocking == 0, "root waits in a blocking flock of the device");
    check(m.nonblocking == DEVICE_TRIES, "root does not try the device 100 times");
    check(m.slept_us == (long)DEVICE_TRIES * DEVICE_TRY_MS * 1000,
          "root's tries do not span a second");
    check(m.volume == (55 | 55 << 8), "the level is not 55:55");
}

/** Root with MIXER_LOCK open and the device held: the same second. */
static void row_root_held(void)
{
    int status;

    row = "root, device held";
    status = vol55(0, 0, 1);
    check(status == 0, "the command does not exit 0");
    check(m.blocking == 0, "root waits in a blocking flock of the device");
    check(m.nonblocking == DEVICE_TRIES, "root does not try the device 100 times");
    check(m.volume == (55 | 55 << 8), "the level is not 55:55");
}

/** Root with the device free takes its lock at the first try. */
static void row_root_free(void)
{
    int status;

    row = "root, /var/run read-only, device free";
    status = vol55(0, EROFS, 0);
    check(status == 0, "the command does not exit 0");
    check(m.locked && m.nonblocking == 1 && m.blocking == 0,
          "root does not take the device's lock at its first try");
    check(m.slept_us == 0, "root sleeps");
    check(m.volume == (55 | 55 << 8), "the level is not 55:55");
}

/** An ordinary user, who cannot open MIXER_LOCK, waits for the device's
 *  lock: granted when free, and the wait's failure reported when held. */
static void row_user(void)
{
    int status;

    row = "user, device free";
    status = vol55(1000, EACCES, 0);
    check(status == 0, "the command does not exit 0");
    check(m.locked && m.blocking == 1 && m.nonblocking == 0,
          "the user does not wait for the device's lock");
    check(m.volume == (55 | 55 << 8), "the level is not 55:55");

    row = "user, device held";
    status = vol55(1000, EACCES, 1);
    check(status == 1, "an interrupted wait does not exit 1");
    check(m.blocking == 1 && m.nonblocking == 0, "the user does not wait for the device's lock");
    check(m.volume == (50 | 50 << 8), "the level changes without the lock");
}

int main(void)
{
    row_root_readonly_run();
    row_root_held();
    row_root_free();
    row_user();
    if (failures == 0) {
        printf("mixercmd-hosttest: pass\n");
    }
    return failures > 255 ? 255 : failures;
}
