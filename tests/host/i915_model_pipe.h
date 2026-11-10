/*
 * i915_model_pipe.h: the pipes of the host model's HD 4400
 * (i915_model_pipe.c): the firmware's picture on pipe A as the Lenovo
 * leaves it, the planes' double-buffered registers, the frame counter, the
 * vblank and flip done events and the pipe CRC.
 */
#ifndef _I915_MODEL_PIPE_H_
#define _I915_MODEL_PIPE_H_

#include "i915_model.h"

/* The Lenovo's panel timings as Linux read them (debugfs's
 * i915_display_info): "1366x768": 60 76300 1366 1374 1384 1558 768 771
 * 772 816, negative syncs; the eDP link at 270000 kHz on one lane, 6 bits
 * a colour, dithered, and the link's and the data's M/N Linux computes
 * for them. */
#define LENOVO_PANEL_CLOCK  76300u
#define LENOVO_PANEL_HTOTAL 1558u
#define LENOVO_PANEL_VTOTAL 816u
#define LENOVO_LINK_M       148159u
#define LENOVO_LINK_N       0x80000u
#define LENOVO_DATA_M       ((63u << 25) | 5333777u)
#define LENOVO_DATA_N       0x800000u
#define LENOVO_FW_FRMCOUNT  0x6a8u

/* The registers of the panel power sequencer, the backlight's PWMs and DDI
 * A as the firmware left them (tests/fixtures/lenovo-gpu/firmware-dump.txt). */
#define LENOVO_FW_PP_STATUS     0x80000008u
#define LENOVO_FW_PP_CONTROL    0xabcd0007u
#define LENOVO_FW_PP_ON_DELAYS  0x07d00000u
#define LENOVO_FW_PP_OFF_DELAYS 0x01f407d0u
#define LENOVO_FW_BLC_CPU_CTL2  0x60000000u
#define LENOVO_FW_BLC_CPU_CTL   0x03a903a9u
#define LENOVO_FW_BLC_PCH_CTL1  0xc0000000u
#define LENOVO_FW_BLC_PCH_CTL2  0x03a903a9u
#define LENOVO_FW_DDI_BUF_CTL_A 0x80000001u
#define LENOVO_FW_PORT_CLK_A    0x20000000u

/*
 * A plane's registers as the pipe scans them out: what the last vblank
 * latched once the plane's surface, or the cursor's base, was written,
 * the arming write. `armed` says that write came since the last vblank.
 */
struct i915_model_plane {
    uint32_t ctl, stride, pos, size, offset, surf;
    int armed;
};

/*
 * A pipe: its three planes as scanned out, the vblanks it has had
 * (`vblanks`, beside the frame counter, which the firmware left at
 * LENOVO_FW_FRMCOUNT on pipe A), the async flips its primary plane took
 * (`flips`), and the time since its last vblank. `crc_frames` counts the
 * frames whose CRC it computed.
 */
struct i915_model_pipe {
    struct i915_model_plane primary, sprite, cursor;
    uint32_t vblanks;
    uint32_t flips;
    uint32_t crc_frames;
    uint64_t us;
};

/* The block's state: the three pipes. */
struct i915_model_pipes {
    struct i915_model_pipe pipe[3];
};

/** The pipes of `m`. */
struct i915_model_pipes *i915_model_pipes(struct i915_model *m);

/** Whether a transcoder routed to `pipe` is enabled, which the vblanks
 * need. */
int i915_model_pipe_on(struct i915_model *m, int pipe);

/**
 * One vblank of `pipe`, as the chip has it at the start of the vertical
 * blank: nothing on a pipe that is off; else the frame counter counts, the
 * planes armed since the last take their new registers, the CRC of the
 * source PIPE_CRC_CTL names is computed into PIPE_CRC_RES_HSW, and the
 * vblank event is raised in the display engine's bank.
 */
void i915_model_pipe_vblank(struct i915_model *m, int pipe);

/** The CRC the model computes for `pipe`'s planes as scanned out now and
 * the PIPE_CRC_CTL source `ctl`: a hash of the registers of the planes
 * the source sees. */
uint32_t i915_model_pipe_crc(struct i915_model *m, int pipe, uint32_t ctl);

#endif /* _I915_MODEL_PIPE_H_ */
