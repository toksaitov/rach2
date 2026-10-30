/*
 * mixer.c: /dev/mixer (mixer.h), OSS's mixer over the audio driver's master
 * volume, its one channel.
 */

#include "mixer.h"

#include "audio.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"

/*
 * The requests are OSS's, with Linux's numbers: MIXER_READ(n) is
 * _IOR('M', n, int) and MIXER_WRITE(n) _IOWR('M', n, int), where channel 0
 * is the master volume and the high numbers are the masks.
 */
#define MIXER_READ_VOLUME     0x80044D00u
#define MIXER_WRITE_VOLUME    0xC0044D00u
#define MIXER_READ_RECSRC     0x80044DFFu
#define MIXER_READ_DEVMASK    0x80044DFEu
#define MIXER_READ_RECMASK    0x80044DFDu
#define MIXER_READ_CAPS       0x80044DFCu
#define MIXER_READ_STEREODEVS 0x80044DFBu
#define MIXER_INFO            0x805C4D65u /* _IOR('M', 101, mixer_info) */
#define OSS_GETVERSION        0x80044D76u
#define MIXER_MASK_VOLUME     0x1
#define OSS_VERSION           0x030802 /* SOUND_VERSION, as Linux gives it */

/* SOUND_MIXER_INFO's answer, OSS's `mixer_info`. */
struct mixer_info {
    char id[16];
    char name[32];
    int modify_counter; /* the volume's writes */
    int fillers[10];
};

/* The writes of the volume since boot, which SOUND_MIXER_INFO gives. */
static int mixer_writes;

int mixer_present(void)
{
    return audio && audio->get_volume;
}

/** Copy the int `value` out to the user's `arg`: 0, or -EFAULT. */
static int32_t int_out(uintptr_t arg, int value)
{
    return copy_to_user((void __user *)arg, &value, sizeof(value)) != 0 ? -EFAULT : 0;
}

/** The volume in OSS's form, the left channel's percentage in the low byte
 * and the right's in the next: the one level in both. */
static int stereo(int level)
{
    return level | level << 8;
}

int32_t mixer_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct mixer_info info;
    int value, left, right;

    (void)e;
    if (!mixer_present()) {
        return -ENODEV;
    }
    switch (request) {
    case MIXER_READ_VOLUME:
        return int_out(arg, stereo(audio->get_volume()));
    case MIXER_WRITE_VOLUME:
        /* The channel is one, so the louder side of the two is the level,
         * and the answer is the level set, as OSS's write gives it. */
        if (copy_from_user(&value, (const void __user *)arg, sizeof(value)) != 0) {
            return -EFAULT;
        }
        left = value & 0xFF;
        right = (value >> 8) & 0xFF;
        audio->set_volume(left > right ? left : right);
        mixer_writes++;
        return int_out(arg, stereo(audio->get_volume()));
    case MIXER_READ_DEVMASK:
        return int_out(arg, MIXER_MASK_VOLUME);
    case MIXER_READ_RECSRC:
    case MIXER_READ_RECMASK:
    case MIXER_READ_CAPS:
    case MIXER_READ_STEREODEVS:
        return int_out(arg, 0);
    case OSS_GETVERSION:
        return int_out(arg, OSS_VERSION);
    case MIXER_INFO:
        k_memset(&info, 0, sizeof(info));
        k_strncpy(info.id, audio->type == AUDIO_DRIVER_HDA ? "HDA" : "SB16", sizeof(info.id) - 1);
        k_strncpy(info.name, audio->name, sizeof(info.name) - 1);
        info.modify_counter = mixer_writes;
        return copy_to_user((void __user *)arg, &info, sizeof(info)) != 0 ? -EFAULT : 0;
    default:
        break;
    }
    /* Another channel of OSS's: this mixer has none of them. */
    return ((request >> 8) & 0xFFu) == 'M' ? -EINVAL : -ENOTTY;
}
