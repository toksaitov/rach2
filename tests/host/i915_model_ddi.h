/*
 * i915_model_ddi.h: DDI B to D of the host model's HD 4400
 * (i915_model_ddi.c): the PCH's GMBUS controller and GPIO pins with a
 * monitor's DDC on each, the AUX channels of B to D with a DisplayPort
 * sink behind each, the ports' buffers and transports, and the PCH's
 * hotplug detection, which a row plugs and unplugs.
 */
#ifndef _I915_MODEL_DDI_H_
#define _I915_MODEL_DDI_H_

#include "i915_model.h"

/* The GMBUS pins a DDC hangs on, by GMBUS pin, as the PCH numbers them. */
#define MODEL_DDC_PINS 7

/*
 * A monitor's DDC on a GMBUS pin. `present` says a sink answers at the
 * EDID's address 0x50 and the segment pointer 0x30; `adaptor` that a DP++
 * adaptor answers at 0x40 with `adaptor_regs` from `adaptor_offset`.
 * `edid` holds `edid_len` bytes. `offset` and `segment` are the E-DDC
 * pointers. The counts: the
 * bytes read over GMBUS and by bit-banging, and the transfers each
 * started.
 */
struct i915_model_ddc {
    int present;
    int adaptor;
    uint8_t adaptor_regs[256];
    uint32_t adaptor_offset;
    uint8_t edid[512];
    uint32_t edid_len;
    uint32_t offset, segment;
    uint32_t gmbus_bytes, bit_bytes;
    uint32_t gmbus_xfers, bit_xfers;
};

/*
 * A DisplayPort sink behind the AUX channel of a port. `present` says it
 * answers; without it a message times out. `dpcd` is its DPCD from 0x000
 * to 0x7ff, `edid` its EDID of `edid_len` bytes with the I2C pointers.
 * Training: the lanes report clock recovery while pattern 1 is set and
 * equalization while pattern 2 or 3 is, on the lanes LANE_COUNT_SET
 * names; `trained` says pattern 2 or 3 was read with every lane done, at
 * `trained_bw` on `trained_lanes`, and `normal` that the port then sent
 * the normal pattern. `busy_us` is what is left of a message in flight
 * and `reply_ctl` the control it ends with. `messages` counts them.
 */
struct i915_model_dp_sink {
    int present;
    uint8_t dpcd[0x800];
    uint8_t edid[256];
    uint32_t edid_len;
    uint32_t offset, segment;
    int trained, normal;
    uint32_t trained_bw, trained_lanes;
    uint32_t busy_us, reply_ctl;
    uint32_t messages;
};

/*
 * A bit-banged bus as its I2C target sees it: the master's clock and data
 * pins, each an output (`scl_out`, `sda_out`) at its level (`scl_val`,
 * `sda_val`) or let go, and the target's hold on the data line
 * (`target_low`). Where the target is in a transfer: `state`, the bits of
 * the byte so far `bits` and `shift`, the address and direction it was
 * given, `first` for the first byte written, and `acked` for the master's
 * answer to the last byte it read.
 */
struct i915_model_i2c {
    int scl_out, scl_val, sda_out, sda_val;
    int target_low;
    int state;
    uint32_t bits, shift;
    uint32_t addr;
    int read, first, acked;
};

/*
 * The block's state. `ddc` by GMBUS pin; `sink` by port less one, B to D.
 * GMBUS: the pin GMBUS0 selects, GMBUS2's status bits, the target and
 * direction of the transfer under way, the bytes it has `left`, and
 * whether it began (`started`). `gmbus_hangs` transfers start and never
 * answer, as a hung controller, each spent by one. `gpio` is each GPIO
 * register's bus, by its index. `pulses` counts the hotplug pulses a row
 * sent.
 */
struct i915_model_ddi {
    struct i915_model_ddc ddc[MODEL_DDC_PINS];
    struct i915_model_dp_sink sink[3];
    uint32_t pin;
    uint32_t gmbus2;
    uint32_t addr;
    int read;
    uint32_t left;
    int started;
    uint32_t gmbus_hangs;
    struct i915_model_i2c gpio[6];
    uint32_t pulses;
};

/** The DDI block of `m`. */
struct i915_model_ddi *i915_model_ddi(struct i915_model *m);

/**
 * Plug or unplug the monitor of hotplug pin `pin` (HPD_PORT_B to D): its
 * live status in SDEISR follows `connected`, and a pulse, long or short,
 * is latched in PCH_PORT_HOTPLUG when its detection is enabled and raised
 * as the PCH's interrupt.
 */
void i915_model_ddi_pulse(struct i915_model *m, int pin, int connected, int long_pulse);

#endif /* _I915_MODEL_DDI_H_ */
