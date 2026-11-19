/*
 * acomp.h: the audio component, Linux's drm_audio_component: how a display
 * driver and the HD Audio driver of its HDMI codec find each other, so that
 * the audio side takes the display's power and reads each port's ELD.
 */
#ifndef _ACOMP_H_
#define _ACOMP_H_

#include "../lib/kint.h"

/*
 * The display driver's side, Linux's drm_audio_component_ops, each called
 * with the display's `dev`. `get_power` holds the display awake with its
 * audio's power domain on, 0 or a negative errno; `put_power` lets one go.
 * `get_cdclk_freq` is the CD clock in kHz. `sync_audio_rate` sets port
 * `port`'s audio clocks for `rate` frames a second. `get_eld` copies the
 * ELD of port `port`, of device entry `pipe` (-1 for one without DP MST),
 * up to `max` bytes into `buf`: its size in bytes, with `*enabled` set when
 * the port plays audio, or a negative errno for no such port. Each may
 * sleep, and none takes the DRM device's mode lock.
 */
struct acomp_ops {
    int32_t (*get_power)(void *dev);
    void (*put_power)(void *dev);
    int32_t (*get_cdclk_freq)(void *dev);
    int32_t (*sync_audio_rate)(void *dev, int port, int pipe, int rate);
    int32_t (*get_eld)(void *dev, int port, int pipe, int *enabled, uint8_t *buf, int max);
};

/*
 * The audio driver's side. `bind` runs once both sides are registered,
 * with the display's operations, which stay valid until `unbind` returns;
 * `unbind` runs before the display side goes. `pin_eld_notify`, Linux's
 * audio_ops->pin_eld_notify, runs after the display enabled or disabled
 * audio on port `port`, device entry `pipe`, under the DRM device's mode
 * lock, while bound.
 */
struct acomp_audio_ops {
    void (*bind)(void *ptr, const struct acomp_ops *ops, void *dev);
    void (*unbind)(void *ptr);
    void (*pin_eld_notify)(void *ptr, int port, int pipe);
};

/** Register the display side, `ops` called with `dev`, and bind the audio
 * side when it is registered. 0, or -EBUSY while another display is. */
int32_t acomp_register_display(const struct acomp_ops *ops, void *dev);

/** Unbind the audio side and withdraw the display `dev`; nothing for one
 * not registered. */
void acomp_unregister_display(void *dev);

/** Register the audio side, called with `ptr`, and bind it when a display
 * is registered. 0, or -EBUSY while another audio side is. */
int32_t acomp_register_audio(const struct acomp_audio_ops *ops, void *ptr);

/** Tell the audio side, if bound, that port `port`'s audio changed, from
 * the display driver's enable or disable, as Linux's pin_eld_notify. */
void acomp_eld_notify(int port, int pipe);

#endif /* _ACOMP_H_ */
