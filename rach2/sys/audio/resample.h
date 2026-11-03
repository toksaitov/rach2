/*
 * resample.h: a sample rate converter for stereo frames of 24 bits, a
 * polyphase windowed-sinc filter in integer arithmetic.
 */
#ifndef _RESAMPLE_H_
#define _RESAMPLE_H_

#include "../lib/kint.h"

/* The largest ratio of the input rate to the output rate filtered in full;
 * a conversion down by more filters the band of the input rate over it. */
#define RESAMPLE_RATIO 8u

/* The most frames in the window, two for each zero crossing at the largest
 * ratio and one more each side (resample.c checks it against its table). */
#define RESAMPLE_TAPS 930u

/*
 * A converter from `in` frames per second to `out`, with the input frames
 * it holds. Output frame k lies at input time k * in / out from the first
 * input frame. `phase` is the fraction of an input frame from the window's
 * centre frame to that time, in 1/out. The window is the last `taps` frames
 * put, oldest first at ring[head] and doubled at ring[head + taps], so that
 * it is always one run of the array. `owed` is the frames to put before the
 * next output frame can be made. At equal rates `span` is 0 and a frame
 * goes through unchanged.
 */
typedef struct resampler {
    uint32_t in;
    uint32_t out;
    uint32_t phase;
    uint32_t step;      /* table entries per input frame, 16.16 */
    uint32_t at;        /* step * phase / out, rounded down */
    uint32_t at_rest;   /* step * phase modulo out */
    uint32_t move;      /* step * in / out, rounded down: `at`'s move a frame */
    uint32_t move_rest; /* step * in modulo out */
    uint32_t gain;      /* the lower rate over the input's, 0.32; 0 for 1 */
    uint32_t span;      /* frames each side of the output time a tap reaches */
    uint32_t taps;      /* 2 * span, or 1 at equal rates */
    uint32_t head;
    uint32_t owed;
    uint32_t mults; /* multiplies made, wrapping, for the host test's count */
    int32_t ring[2 * RESAMPLE_TAPS][2];
} resampler_t;

/**
 * Set `r` to convert from `in` to `out` frames per second, both above 0,
 * with a window of silence. The first frame put is the one the first output
 * frame lies at, and the converter takes `span` frames past it first.
 */
void resample_init(resampler_t *r, uint32_t in, uint32_t out);

/** The frames to put for the next `n` output frames, at most 65535. */
uint32_t resample_need(const resampler_t *r, uint32_t n);

/** Put input frame `frame`, left and right, of 24 bits; `r->owed` is above
 * 0. */
void resample_put(resampler_t *r, const int32_t *frame);

/** Make the next output frame in `frame`, left and right, held to 24 bits;
 * `r->owed` is 0. */
void resample_get(resampler_t *r, int32_t *frame);

#endif /* _RESAMPLE_H_ */
