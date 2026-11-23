/* Events in the optional display and rendering timeline. */
#ifndef _DRM_FRAME_TRACE_H_
#define _DRM_FRAME_TRACE_H_

#include "../lib/kint.h"

enum drm_frame_event {
    DRM_FRAME_SURFACE = 1,
    DRM_FRAME_ARM,
    DRM_FRAME_IRQ,
    DRM_FRAME_DELIVER,
    DRM_FRAME_STAMP,
    DRM_FRAME_FENCE_WAIT,
    DRM_FRAME_FENCE_SIGNAL,
    DRM_FRAME_REQUEST_SUBMIT,
    DRM_FRAME_REQUEST_DONE
};

#ifdef RACH2_DRM_FRAME_TRACE
extern volatile int drm_frame_trace_enabled;

/** Append one event stamped by CLOCK_MONOTONIC. Any context. */
void drm_frame_trace(enum drm_frame_event event, uint32_t object, uint64_t a, uint64_t b,
                     uint64_t c);
/** Register the stopped capture in debugfs during boot. */
void drm_frame_trace_init(void);
#else
#define drm_frame_trace(event, object, a, b, c) ((void)0)
#define drm_frame_trace_init()                  ((void)0)
#endif

#endif
