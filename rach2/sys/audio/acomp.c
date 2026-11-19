/*
 * acomp.c: the audio component (acomp.h), which binds the display driver
 * to the HD Audio driver of its HDMI codec, as Linux's component framework
 * binds i915 to snd_hda_intel (sound/hda/hdac_component.c).
 */

#include "acomp.h"

#include "../lib/kerrno.h"
#include "../proc/mutex.h"

/*
 * The two sides and whether they are bound, under `lock`, which bind,
 * unbind and each notification hold, so that a notification never runs
 * beside an unbind. Its order: after the DRM device's mode lock, before
 * the audio driver's own.
 */
static struct mutex lock;
static int inited;
static const struct acomp_ops *display_ops;
static void *display_dev;
static const struct acomp_audio_ops *audio_ops;
static void *audio_ptr;
static int bound;

/** Make the lock on the first registration, which runs at boot. */
static void acomp_init(void)
{
    if (!inited) {
        mutex_init(&lock, "acomp");
        inited = 1;
    }
}

/** Bind the sides when both are there and not bound yet; under `lock`. */
static void try_bind(void)
{
    if (display_ops && audio_ops && !bound) {
        bound = 1;
        audio_ops->bind(audio_ptr, display_ops, display_dev);
    }
}

int32_t acomp_register_display(const struct acomp_ops *ops, void *dev)
{
    acomp_init();
    mutex_lock(&lock);
    if (display_ops) {
        mutex_unlock(&lock);
        return -EBUSY;
    }
    display_ops = ops;
    display_dev = dev;
    try_bind();
    mutex_unlock(&lock);
    return 0;
}

void acomp_unregister_display(void *dev)
{
    if (!inited) {
        return;
    }
    mutex_lock(&lock);
    if (display_ops && display_dev == dev) {
        if (bound) {
            bound = 0;
            audio_ops->unbind(audio_ptr);
        }
        display_ops = NULL;
        display_dev = NULL;
    }
    mutex_unlock(&lock);
}

int32_t acomp_register_audio(const struct acomp_audio_ops *ops, void *ptr)
{
    acomp_init();
    mutex_lock(&lock);
    if (audio_ops) {
        mutex_unlock(&lock);
        return -EBUSY;
    }
    audio_ops = ops;
    audio_ptr = ptr;
    try_bind();
    mutex_unlock(&lock);
    return 0;
}

void acomp_eld_notify(int port, int pipe)
{
    if (!inited) {
        return;
    }
    mutex_lock(&lock);
    if (bound) {
        audio_ops->pin_eld_notify(audio_ptr, port, pipe);
    }
    mutex_unlock(&lock);
}
