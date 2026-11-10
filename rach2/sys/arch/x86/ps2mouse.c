/*
 * ps2mouse.c: the PS/2 mouse, on the 8042's auxiliary port, as ps2mouse.h
 * describes it. The mouse shares the controller with the keyboard: both use
 * port 0x60 for data and 0x64 for status and commands, and the waits around a
 * write are i8042.c's. The mouse interrupts on IRQ 12, vector 44.
 *
 * A packet is three bytes:
 *   Byte 0: [Yovf|Xovf|Ysign|Xsign|1|Middle|Right|Left]
 *   Byte 1: X movement, the low 8 bits of a 9-bit value, Xsign its sign
 *   Byte 2: Y movement, the low 8 bits of a 9-bit value, Ysign its sign
 * A mouse that answers Microsoft's knocks, sample rates set in a fixed order
 * and then its ID read, sends a fourth: the IntelliMouse (ID 3) its wheel's
 * turn as a signed byte, and the IntelliMouse Explorer (ID 4) a wheel's turn
 * with two more buttons, decoded as Linux's psmouse decodes them. The event
 * device has Linux's name, numbers and capabilities for each protocol.
 *
 * Before the knocks the bring-up asks for a Synaptics touchpad, as Linux's
 * psmouse does: one that answers speaks its own absolute protocol from then
 * on, and synaptics.c takes its bytes; one whose setup fails is set back to
 * relative mode and goes on as a mouse named a Synaptics TouchPad. A pad
 * with a pass-through port is asked for the device behind it, the guest: a
 * TrackPoint (trackpoint.c) or a mouse, whose packets the pad carries. When
 * no Synaptics pad answers, the protocol's probe asks for an ALPS touchpad
 * and then an Elantech one, in psmouse's order: alps.c or elantech.c takes
 * the bytes of one that comes up, and one whose setup fails goes on as a
 * mouse named for its vendor.
 *
 * After the bring-up the handler owns the port, as Linux's psmouse does in
 * its state PSMOUSE_ACTIVATED. A run of commands, a change of the guest's
 * settings or a reconnect, takes it back for its length: the handler then
 * leaves the mouse's bytes in the 8042 for the run to read, as Linux's
 * PSMOUSE_CMD_MODE gives them to `ps2_command`. Five packets out of form in
 * a row put the port in Linux's PSMOUSE_IGNORE, whose bytes are dropped,
 * until kpsmoused has reconnected the device.
 */
#include "i8042.h"
#include "portio.h"
#include "elantech.h"
#include "ps2mouse.h"
#include "alps.h"
#include "synaptics.h"
#include "trackpoint.h"
#include "../arch.h"
#include "../../platform.h"
#include "../../evdev.h"
#include "../../klog.h"
#include "../../mouse.h"
#include "../../timer.h"
#include "../../udelay.h"
#include "../../lib/kirq.h"
#include "isr.h"
#include "../../kio.h"
#include "../../serial.h"
#ifndef RACH2_HOSTTEST
#include "../../proc/percpu.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/smp.h"
#include "../../proc/wait_queue.h"
#endif

/* 8042 Controller Commands (write to 0x64) */
#define PS2_CMD_READ_CONFIG   0x20 /* Read command byte */
#define PS2_CMD_WRITE_CONFIG  0x60 /* Write command byte */
#define PS2_CMD_DISABLE_MOUSE 0xA7 /* Disable auxiliary port */
#define PS2_CMD_ENABLE_MOUSE  0xA8 /* Enable auxiliary port */
#define PS2_CMD_TEST_MOUSE    0xA9 /* Test auxiliary port */
#define PS2_CMD_WRITE_MOUSE   0xD4 /* Write to auxiliary device */

/* Mouse Commands (sent via PS2_CMD_WRITE_MOUSE) */
#define MOUSE_CMD_RESET        0xFF /* Reset mouse */
#define MOUSE_CMD_SET_RES      0xE8 /* Set resolution */
#define MOUSE_CMD_SET_SCALE11  0xE6 /* Set 1:1 scaling */
#define MOUSE_CMD_RESEND       0xFE /* Resend last packet */
#define MOUSE_CMD_SET_DEFAULTS 0xF6 /* Set default settings */
#define MOUSE_CMD_DISABLE      0xF5 /* Disable data reporting */
#define MOUSE_CMD_ENABLE       0xF4 /* Enable data reporting */
#define MOUSE_CMD_SET_RATE     0xF3 /* Set sample rate */
#define MOUSE_CMD_GET_ID       0xF2 /* Get device ID */
#define MOUSE_CMD_GET_INFO     0xE9 /* Status request */

/* Mouse Response Codes */
#define MOUSE_ACK    0xFA /* Command acknowledged */
#define MOUSE_RESEND 0xFE /* Resend request */

/* Configuration byte bits */
#define PS2_CONFIG_MOUSE_IRQ 0x02 /* Enable IRQ12 for mouse */
#define PS2_CONFIG_MOUSE_CLK 0x20 /* Disable mouse clock (set=disabled) */

/* The protocols, Linux's `enum psmouse_type`, which EVIOCGID gives as the
 * product, and the IDs a mouse answers once it speaks the second and
 * third. */
#define PROTO_PS2        1
#define PROTO_IMPS       5
#define PROTO_IMEX       6
#define PROTO_TRACKPOINT 10
#define ID_IMPS          3
#define ID_IMEX          4

/* The port's states, Linux's `enum psmouse_state` as far as the handler
 * tells them apart: it reports, it leaves the bytes to a run of commands,
 * or it drops them until a reconnect. */
#define STATE_ACTIVE 0
#define STATE_CMD    1
#define STATE_IGNORE 2

/* Linux's `resetafter`: the bad packets in a row after which the device is
 * reconnected. */
#define RESET_AFTER 5

/* The guest's place, as Linux's psmouse names the device of the touchpad's
 * pass-through port. */
#define GUEST_PHYS "synaptics-pt/serio0/input0"

/* The packet being assembled, the index of its next byte, and its size,
 * three bytes or four with a wheel. */
static uint8_t packet_bytes[4];
static int packet_index = 0;
static int packet_size = 3;

/* The protocol the mouse speaks, PROTO_*, and its event device. */
static int protocol = PROTO_PS2;
static struct evdev *mouse_dev;

/* What `synaptics_init` and `elantech_init` found: 1 a Synaptics and 2 an
 * Elantech touchpad in absolute mode, whose bytes their drivers take, 0 no
 * touchpad, -1 a Synaptics and -2 an Elantech touchpad set up as a mouse. */
#define TOUCHPAD_SYNAPTICS 1
#define TOUCHPAD_ELANTECH  2
static int touchpad;

/* What `alps_init` found, as `touchpad` says for a Synaptics pad. */
static int alps;

/* The side and extra buttons held, MOUSE_BUTTON_*: an Explorer's packet
 * of a wheel's turn of more than 7 does not carry them. */
static unsigned int extra_buttons;

/* The port's state, STATE_*, which the handler reads; and the device's
 * bad packets in a row. */
static volatile int state = STATE_ACTIVE;
static int bad_in_a_row;

/* What kpsmoused is asked to reconnect: the port, the touchpad or mouse
 * and the guest after it, or the guest alone. */
#define RECONNECT_PORT  1
#define RECONNECT_GUEST 2

/* A run of commands is in progress, and a reconnect is wanted,
 * RECONNECT_*. */
static volatile int cmd_busy;
static volatile int reconnect_wanted;

/* The byte 0xAA began a packet: with 0x00 after it, the announcement of a
 * device that reset itself, Linux's PSMOUSE_RET_BAT and PSMOUSE_RET_ID. */
static int bat_held;

#ifndef RACH2_HOSTTEST
/* kpsmoused, which waits on `reconnect_wq`; the callers of a run of
 * commands that wait for another's end, on `cmd_wq`; and the sleeps of
 * `ps2mouse_msleep`. */
static process_t *thread;
static wait_queue_t reconnect_wq;
static wait_queue_t cmd_wq;
static wait_queue_t sleep_wq; /* woken by nothing: the sleep ends at its tick */
#endif

/*
 * The guest, the device on the touchpad's pass-through port: its line;
 * whether it reports; its protocol, PROTO_*, and packet size; its event
 * device, NULL when there is no guest; the Explorer's side buttons held;
 * and the stick's buttons the touchpad gave out of band, Linux's
 * `extra_buttons`, in a packet's first byte's places.
 */
static struct {
    const struct ps2io *io;
    int active;
    int protocol;
    int packet_size;
    struct evdev *dev;
    unsigned int side;
    uint8_t extra;
} guest;

/* The guest's name, built for a TrackPoint from its vendor. */
static char guest_name[40];

/* How many bytes the first drain reads before it calls the controller
   stuck. A PS/2 output buffer has one byte, so anything past a handful is a
   controller that is not answering. */
#define PS2_DRAIN_LIMIT 4096

/**
 * Inside a run of commands, let another processor that waits for the big
 * lock have it, so that the run's waits for the mouse stop no other kernel
 * work: an interrupt handler there runs, the keyboard's among them.
 */
static void wait_turn(void)
{
#ifndef RACH2_HOSTTEST
    uint32_t flags;

    if (cmd_busy) {
        flags = kirq_save();
        bkl_yield();
        kirq_restore(flags);
    }
#endif
}

/**
 * Spin until the 8042 output buffer has a byte from the mouse, ready to read.
 *
 * During the bring-up a keyboard byte waiting in the buffer is read and
 * dropped: the buffer holds one byte, so the mouse's cannot arrive until the
 * keyboard's is taken, and a key pressed then is lost. Inside a run of
 * commands, with interrupts on, the keyboard's handler takes it.
 *
 * @return 0 when a mouse byte is present, -1 on timeout.
 */
static int ps2_wait_mouse_output(void)
{
    int timeout = PS2_TIMEOUT;
    uint8_t status;

    while (timeout-- > 0) {
        status = inb(PS2_STATUS_PORT);
        if ((status & PS2_STATUS_OUTPUT_FULL) == 0) {
            wait_turn();
            continue;
        }
        if (status & PS2_STATUS_MOUSE_DATA) {
            return 0;
        }
        if (cmd_busy) {
            wait_turn();
            continue;
        }
        inb(PS2_DATA_PORT);
    }
    return -1;
}

/**
 * Send a byte to the mouse through the 8042 auxiliary port.
 *
 * Each command must be prefixed with PS2_CMD_WRITE_MOUSE so the
 * controller routes the following data byte to the auxiliary device. The
 * reply is the next byte the status marks as the mouse's, and a keyboard
 * byte before it is dropped. A MOUSE_RESEND reply means the mouse saw a
 * framing error, and a timeout means the controller or the mouse did not
 * answer; either way the command is tried again, three times in all. Any
 * other byte is returned to the caller, since some commands answer with
 * data and not an ACK.
 *
 * @return the mouse's response byte, or -1 after three failed tries.
 */
static int mouse_send_command(uint8_t cmd)
{
    int retries = 3;
    uint8_t response;
    uint32_t flags;

    while (retries-- > 0) {
        /* Prefix tells the 8042 to forward the next byte to the mouse; the
         * keyboard's LED command, from the tick, must not come between. */
        flags = kirq_save();
        if (ps2_send_command(PS2_CMD_WRITE_MOUSE) < 0 || ps2_wait_input() < 0) {
            kirq_restore(flags);
            continue;
        }
        outb(PS2_DATA_PORT, cmd);
        kirq_restore(flags);

        if (ps2_wait_mouse_output() < 0) {
            continue;
        }

        response = inb(PS2_DATA_PORT);
        if (response == MOUSE_ACK) {
            return response;
        } else if (response == MOUSE_RESEND) {
            continue;
        } else {
            return response;
        }
    }

    return -1;
}

int ps2mouse_send(uint8_t cmd)
{
    return mouse_send_command(cmd);
}

int ps2mouse_receive(void)
{
    return ps2_wait_mouse_output() < 0 ? -1 : inb(PS2_DATA_PORT);
}

const struct ps2io ps2mouse_aux = {ps2mouse_send, ps2mouse_receive};

int ps2io_command(const struct ps2io *io, uint8_t cmd, const uint8_t *param, int nparam,
                  uint8_t *result, int nresult)
{
    int i, b;

    if (io->send(cmd) != MOUSE_ACK) {
        return -1;
    }
    for (i = 0; i < nparam; i++) {
        if (io->send(param[i]) != MOUSE_ACK) {
            return -1;
        }
    }
    for (i = 0; i < nresult; i++) {
        b = io->receive();
        if (b < 0) {
            return -1;
        }
        result[i] = (uint8_t)b;
    }
    return 0;
}

/** Send `cmd` with no argument and no answer on `io`; 0 when it was
 * acknowledged. */
static int io_cmd(const struct ps2io *io, uint8_t cmd)
{
    return ps2io_command(io, cmd, 0, 0, 0, 0);
}

/** Send `cmd` and its argument `arg` on `io`, Linux's `ps2_command` with
 * one parameter. */
static int io_cmd_arg(const struct ps2io *io, uint8_t cmd, uint8_t arg)
{
    return ps2io_command(io, cmd, &arg, 1, 0, 0);
}

/**
 * Disable the reporting of the device on `io`, Linux's `psmouse_deactivate`:
 * bytes of a packet already on the way before the ACK are passed over, as
 * Linux's handler takes them as packets'. 0 once acknowledged.
 */
static int deactivate(const struct ps2io *io)
{
    int reply = io->send(MOUSE_CMD_DISABLE), n = 0;

    while (reply >= 0 && reply != MOUSE_ACK && n++ < 16) {
        reply = io->receive();
    }
    return reply == MOUSE_ACK ? 0 : -1;
}

/**
 * Reset the mouse and read the two bytes it sends after its ACK, 0xAA for a
 * passed self-test and 0x00 for its device ID. Their values are not checked,
 * and a timeout on either is tolerated.
 *
 * @return 0 when the mouse answered the reset, -1 when it did not.
 */
static int mouse_reset(void)
{
    if (mouse_send_command(MOUSE_CMD_RESET) < 0) {
        return -1;
    }
    if (ps2_wait_mouse_output() == 0) {
        inb(PS2_DATA_PORT);
    }
    if (ps2_wait_mouse_output() == 0) {
        inb(PS2_DATA_PORT);
    }
    return 0;
}

/**
 * Send the three sample rates of `rates` and read the ID the mouse answers
 * then: one of Microsoft's knocks, which a mouse with a wheel answers by
 * speaking its protocol from then on. Returns the ID, or -1 when the mouse
 * did not answer.
 */
static int knock(const uint8_t *rates)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (mouse_send_command(MOUSE_CMD_SET_RATE) != MOUSE_ACK ||
            mouse_send_command(rates[i]) != MOUSE_ACK) {
            return -1;
        }
    }
    if (mouse_send_command(MOUSE_CMD_GET_ID) != MOUSE_ACK || ps2_wait_mouse_output() < 0) {
        return -1;
    }
    return inb(PS2_DATA_PORT);
}

/** Send `cmd` and its argument `arg`; a refusal is ignored, since the
 * mouse works on at its old setting. */
static void mouse_set(uint8_t cmd, uint8_t arg)
{
    if (mouse_send_command(cmd) == MOUSE_ACK) {
        mouse_send_command(arg);
    }
}

/**
 * Find the protocol the mouse speaks, as Linux's psmouse does, and set it as
 * Linux sets it: when no Synaptics pad answered, an ALPS touchpad
 * (`alps_init`), which alps.c sets up whole, and when no ALPS pad answered
 * either, an Elantech touchpad (`elantech_init`); each speaks its own
 * protocol once it is set up. Then the IntelliMouse's knock, rates 200, 100
 * and 80, then the Explorer's, 200, 200 and 80, with the rates 200, 80 and
 * 40 that turn on an Explorer 4.0's horizontal wheel; then 100 samples a
 * second, 8 counts a millimetre and 1:1 scaling. A mouse that answers
 * neither knock is a plain PS/2 mouse of three buttons.
 */
static void detect_protocol(void)
{
    static const uint8_t imps[3] = {200, 100, 80};
    static const uint8_t imex[3] = {200, 200, 80};
    static const uint8_t hwheel[3] = {200, 80, 40};
    int i;

    protocol = PROTO_PS2;
    packet_size = 3;
    alps = touchpad == 0 ? alps_init() : 0;
    if (alps > 0) {
        return;
    }
    /* A pad that answered ALPS's reports is not asked for Elantech's. */
    if (touchpad == 0 && alps == 0) {
        i = elantech_init();
        if (i > 0) {
            touchpad = TOUCHPAD_ELANTECH;
            return;
        }
        if (i < 0) {
            touchpad = -TOUCHPAD_ELANTECH;
        }
    }
    if (knock(imps) == ID_IMPS) {
        protocol = PROTO_IMPS;
        packet_size = 4;
        if (knock(imex) == ID_IMEX) {
            protocol = PROTO_IMEX;
            for (i = 0; i < 3; i++) {
                mouse_set(MOUSE_CMD_SET_RATE, hwheel[i]);
            }
        }
    }
    mouse_set(MOUSE_CMD_SET_RATE, 100);
    mouse_set(MOUSE_CMD_SET_RES, 3);
    mouse_send_command(MOUSE_CMD_SET_SCALE11);
}

/** Register the event device of the mouse, with Linux's name, numbers and
 * capabilities for its protocol: the protocol's name, then the vendor and
 * the device, "Synaptics TouchPad" or "Elantech Touchpad" for a touchpad
 * that answered its vendor's knock and was not set up, and the ALPS pad's
 * name for one whose set-up failed. A pad in ALPS's absolute protocol has
 * alps.c's devices instead. */
static void register_mouse(void)
{
    static const char *const names[3][3] = {
        {"PS/2 Generic Mouse", "ImPS/2 Generic Wheel Mouse", "ImExPS/2 Generic Explorer Mouse"},
        {"PS/2 Synaptics TouchPad", "ImPS/2 Synaptics TouchPad", "ImExPS/2 Synaptics TouchPad"},
        {"PS/2 Elantech Touchpad", "ImPS/2 Elantech Touchpad", "ImExPS/2 Elantech Touchpad"}};
    static const char *const protocols[3] = {"PS/2", "ImPS/2", "ImExPS/2"};
    const char *const *name = names[-touchpad];
    unsigned int caps = MOUSE_POINTER;
    int kind = 0;

    if (alps > 0) {
        return;
    }

    if (protocol == PROTO_IMPS) {
        kind = 1;
        caps |= MOUSE_WHEEL;
    } else if (protocol == PROTO_IMEX) {
        kind = 2;
        caps |= MOUSE_WHEEL | MOUSE_HWHEEL | MOUSE_SIDE;
    }
    mouse_dev =
        mouse_register(alps < 0 ? alps_mouse_name(protocols[kind]) : name[kind],
                       "isa0060/serio1/input0", EVDEV_BUS_I8042, 0x0002, (uint16_t)protocol, caps);
}

/** The low `bits` + 1 bits of `v` as a signed number, bit `bits` its sign,
 * as Linux's `sign_extend32`. */
static int sign_extend(unsigned v, int bits)
{
    int sign = 1 << bits;

    return (int)((v & ((unsigned)sign * 2u - 1u)) ^ (unsigned)sign) - sign;
}

/**
 * Report the whole packet in `packet_bytes`: its buttons, its 9-bit
 * movements, Y turned to Linux's way up, and the fourth byte of a wheel's
 * protocol, as Linux's `psmouse_process_byte` reads it. A packet with an
 * overflow bit set, bit 6 or 7, has a movement that cannot be used, and is
 * dropped.
 */
static void report_packet(void)
{
    uint8_t flags = packet_bytes[0];
    uint8_t b3 = packet_bytes[3];
    int dx = packet_bytes[1];
    int dy = packet_bytes[2];
    int wheel = 0, hwheel = 0;

    if (flags & 0xC0) {
        return;
    }
    if (flags & 0x10) {
        dx -= 256;
    }
    if (flags & 0x20) {
        dy -= 256;
    }
    if (protocol == PROTO_IMPS) {
        wheel = -(int)(int8_t)b3;
    } else if (protocol == PROTO_IMEX) {
        switch (b3 & 0xC0) {
        case 0x80: /* an Explorer 4.0's vertical turn of up to 31 */
            wheel = -sign_extend(b3, 5);
            break;
        case 0x40: /* its horizontal turn */
            hwheel = -sign_extend(b3, 5);
            break;
        default:
            wheel = -sign_extend(b3, 3);
            extra_buttons =
                ((b3 & 0x10) ? MOUSE_BUTTON_SIDE : 0) | ((b3 & 0x20) ? MOUSE_BUTTON_EXTRA : 0);
            break;
        }
    }
    mouse_report(mouse_dev, (flags & 0x07u) | extra_buttons, dx, -dy, wheel, hwheel);
}

/** Linux's `psmouse_reset` on `io`: the reset and the two bytes after
 * its ACK, 0xAA and the ID. */
static int io_reset(const struct ps2io *io)
{
    uint8_t reply[2];

    return ps2io_command(io, MOUSE_CMD_RESET, 0, 0, reply, 2);
}

/** One of Microsoft's knocks on `io`, as Linux's `intellimouse_detect`
 * sends it: the three rates, each refusal ignored, then the ID, which is
 * the last rate when the ID did not come. */
static int io_knock(const struct ps2io *io, const uint8_t *rates)
{
    uint8_t id = rates[2];
    int i;

    for (i = 0; i < 3; i++) {
        (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, rates[i]);
    }
    (void)ps2io_command(io, MOUSE_CMD_GET_ID, 0, 0, &id, 1);
    return id;
}

/**
 * Find what the guest on `io` speaks, as Linux's `psmouse_probe` and
 * `psmouse_extensions` find it on a pass-through port, where only the
 * protocols Linux tries there are tried: its ID, which must be a mouse's,
 * and the reset to the defaults; the defaults again, which Linux sends
 * before the ALPS probe it skips there; a TrackPoint's ID, set up when
 * `set_properties` (`trackpoint_detect`) or only read; then the defaults,
 * the reset, and the IntelliMouse Explorer's knock after the
 * IntelliMouse's, with an Explorer 4.0's horizontal wheel turned on, then
 * the IntelliMouse's alone. Returns PROTO_*, or -1 when no mouse answered.
 */
static int guest_detect(const struct ps2io *io, int set_properties)
{
    static const uint8_t imps[3] = {200, 100, 80};
    static const uint8_t imex[3] = {200, 200, 80};
    static const uint8_t hwheel[3] = {200, 80, 40};
    uint8_t id = 0xa5;
    int i;

    if (ps2io_command(io, MOUSE_CMD_GET_ID, 0, 0, &id, 1) < 0 ||
        (id != 0x00 && id != 0x03 && id != 0x04 && id != 0xff)) {
        return -1;
    }
    (void)io_cmd(io, MOUSE_CMD_SET_DEFAULTS);
    (void)io_cmd(io, MOUSE_CMD_SET_DEFAULTS);
    if ((set_properties ? trackpoint_detect(io) : trackpoint_identify(io)) == 0) {
        return PROTO_TRACKPOINT;
    }
    (void)io_cmd(io, MOUSE_CMD_SET_DEFAULTS);
    (void)io_reset(io);
    (void)io_knock(io, imps);
    if (io_knock(io, imex) == ID_IMEX) {
        for (i = 0; i < 3; i++) {
            (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, hwheel[i]);
        }
        return PROTO_IMEX;
    }
    if (io_knock(io, imps) == ID_IMPS) {
        return PROTO_IMPS;
    }
    return PROTO_PS2;
}

/** Linux's `psmouse_initialize` on the guest: 100 samples a second, 200
 * dpi and 1:1 scaling, each refusal ignored. */
static void guest_initialize(const struct ps2io *io)
{
    (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, 100);
    (void)io_cmd_arg(io, MOUSE_CMD_SET_RES, 3);
    (void)io_cmd(io, MOUSE_CMD_SET_SCALE11);
}

/** Register the guest's event device with the name, numbers and
 * capabilities Linux's psmouse gives its protocol: "TPPS/2 IBM TrackPoint"
 * and the like, with a middle button when the stick has three and the
 * pointing stick's property, or a mouse's as the auxiliary port's. */
static void guest_register(void)
{
    const char *name = "PS/2 Generic Mouse";
    unsigned int caps = MOUSE_POINTER;
    const char *v;
    int n = 0;

    if (guest.protocol == PROTO_IMPS) {
        name = "ImPS/2 Generic Wheel Mouse";
        caps |= MOUSE_WHEEL;
    } else if (guest.protocol == PROTO_IMEX) {
        name = "ImExPS/2 Generic Explorer Mouse";
        caps |= MOUSE_WHEEL | MOUSE_HWHEEL | MOUSE_SIDE;
    } else if (guest.protocol == PROTO_TRACKPOINT) {
        for (v = "TPPS/2 "; *v; v++) {
            guest_name[n++] = *v;
        }
        for (v = trackpoint_vendor(); *v && n < (int)sizeof(guest_name) - 12; v++) {
            guest_name[n++] = *v;
        }
        for (v = " TrackPoint"; *v; v++) {
            guest_name[n++] = *v;
        }
        guest_name[n] = 0;
        name = guest_name;
        if (trackpoint_buttons() < 3) {
            caps |= MOUSE_NO_MIDDLE;
        }
    }
    guest.dev =
        mouse_register(name, GUEST_PHYS, EVDEV_BUS_I8042, 0x0002, (uint16_t)guest.protocol, caps);
    if (guest.protocol == PROTO_TRACKPOINT) {
        evdev_set_prop(guest.dev, INPUT_PROP_POINTING_STICK);
        evdev_sysfs_serio_files(EVDEV_SERIO_PASS_THROUGH, trackpoint_sysfs_add);
    } else {
        evdev_sysfs_serio_files(EVDEV_SERIO_PASS_THROUGH, NULL);
    }
}

/** Take the guest's event device away, as Linux's `psmouse_disconnect`
 * does before a port is probed anew. */
static void guest_forget(void)
{
    guest.active = 0;
    if (guest.dev) {
        evdev_unregister(guest.dev);
        guest.dev = NULL;
    }
    guest.extra = 0;
    guest.side = 0;
}

int ps2mouse_guest_connect(const struct ps2io *io)
{
    int proto;

    guest_forget();
    (void)deactivate(&ps2mouse_aux);
    k_udelay(10000); /* Linux lets the device behind the port settle */
    proto = guest_detect(io, 1);
    if (proto < 0) {
        return 0;
    }
    guest.io = io;
    guest.protocol = proto;
    guest.packet_size = proto == PROTO_IMPS || proto == PROTO_IMEX ? 4 : 3;
    guest_initialize(io);
    guest_register();
    synaptics_pt_activate(guest.packet_size);
    if (io_cmd(io, MOUSE_CMD_ENABLE) < 0) {
        klog(KLOG_WARN, "mouse", "pass-through: the guest did not enable reporting");
        return 1;
    }
    guest.active = 1;
    klog(KLOG_INFO, "mouse", "pass-through: found: protocol %d", proto);
    return 1;
}

/** Wake kpsmoused to reconnect `what`, RECONNECT_*. Any context. */
static void request_reconnect(int what)
{
    reconnect_wanted |= what;
#ifndef RACH2_HOSTTEST
    wq_wake_all(&reconnect_wq);
#endif
}

/**
 * Reconnect the guest after its touchpad, as Linux's `psmouse_reconnect`
 * does for a pass-through port: the pad disabled; a TrackPoint's own
 * reconnect, or for a mouse the reset and the probe, which must find the
 * protocol it had; then the guest's rate, resolution and scaling, the pad's
 * mode for it, and its reporting. Returns 0, or -1 when the guest is gone
 * or another, which the caller then probes anew.
 */
static int guest_reconnect(void)
{
    const struct ps2io *io = guest.io;

    (void)deactivate(&ps2mouse_aux);
    guest.active = 0;
    if (guest.protocol == PROTO_TRACKPOINT) {
        if (trackpoint_reconnect(io) < 0) {
            return -1;
        }
    } else {
        (void)io_reset(io);
        if (guest_detect(io, 0) != guest.protocol) {
            return -1;
        }
    }
    guest_initialize(io);
    synaptics_pt_activate(guest.packet_size);
    if (io_cmd(io, MOUSE_CMD_ENABLE) < 0) {
        return -1;
    }
    guest.active = 1;
    return 0;
}

void ps2mouse_guest_packet(const uint8_t *bytes)
{
    unsigned int b0;
    uint8_t b3 = bytes[3];
    int dx, dy, wheel = 0, hwheel = 0;

    if (!guest.active) {
        return;
    }
    if (bytes[0] == 0xAA && bytes[1] == 0x00) {
        /* The guest reset itself, as Linux's psmouse_receive_byte sees
         * it: dropped until its reconnect. */
        guest.active = 0;
        klog(KLOG_WARN, "mouse", "pass-through: the guest announced itself anew: reconnecting");
        request_reconnect(RECONNECT_GUEST);
        return;
    }
    if (guest.protocol == PROTO_IMPS) {
        wheel = -(int)(int8_t)b3;
    } else if (guest.protocol == PROTO_IMEX) {
        switch (b3 & 0xC0) {
        case 0x80:
            wheel = -sign_extend(b3, 5);
            break;
        case 0x40:
            hwheel = -sign_extend(b3, 5);
            break;
        default:
            wheel = -sign_extend(b3, 3);
            guest.side =
                ((b3 & 0x10) ? MOUSE_BUTTON_SIDE : 0) | ((b3 & 0x20) ? MOUSE_BUTTON_EXTRA : 0);
            break;
        }
    }
    /* Linux's psmouse_report_standard_packet: no overflow check, and a
     * byte of 0 no movement whatever its sign bit. */
    b0 = bytes[0] | guest.extra;
    dx = bytes[1] ? (int)bytes[1] - (int)((b0 << 4) & 0x100u) : 0;
    dy = bytes[2] ? (int)bytes[2] - (int)((b0 << 3) & 0x100u) : 0;
    mouse_report(guest.dev, (b0 & 0x07u) | guest.side, dx, -dy, wheel, hwheel);
}

int ps2mouse_guest_packet_size(void)
{
    return guest.dev ? guest.packet_size : 0;
}

void ps2mouse_guest_buttons(uint8_t buttons)
{
    if (!guest.dev) {
        return;
    }
    mouse_report(guest.dev, (buttons & 0x07u) | guest.side, 0, 0, 0, 0);
    guest.extra = (uint8_t)(buttons & 0x07u);
}

/**
 * Count a bad packet of the device, when `bad` is 1, or a whole one, as
 * Linux's `psmouse_handle_byte` counts them: the fifth bad packet in a row
 * puts the port in STATE_IGNORE and asks for a reconnect, and a whole
 * packet ends the count.
 */
static void count_packet(int bad)
{
    if (bad) {
        if (++bad_in_a_row == RESET_AFTER) {
            state = STATE_IGNORE;
            klog(KLOG_WARN, "mouse", "sync: %d bad packets in a row: reconnecting", RESET_AFTER);
            request_reconnect(RECONNECT_PORT);
        }
    } else if (bad_in_a_row) {
        bad_in_a_row = 0;
        klog(KLOG_INFO, "mouse", "sync: resynced");
    }
}

/** Whether the device's next byte begins a packet. */
static int packet_start(void)
{
    if (touchpad == TOUCHPAD_SYNAPTICS) {
        return synaptics_packet_start();
    }
    if (touchpad == TOUCHPAD_ELANTECH) {
        return elantech_packet_start();
    }
    if (alps > 0) {
        return alps_packet_start();
    }
    return packet_index == 0;
}

/**
 * Take one byte of the device's packet: a touchpad's to synaptics.c,
 * elantech.c or alps.c, a mouse's into the packet being assembled, which
 * is reported once whole. A touchpad's packet that its driver refuses is a
 * bad packet, and so is a mouse's byte dropped for its framing bit.
 */
static void take_byte(uint8_t data)
{
    int rc;

    if (touchpad == TOUCHPAD_SYNAPTICS) {
        rc = synaptics_byte(data);
        if (rc != SYNAPTICS_GOOD) {
            count_packet(rc == SYNAPTICS_BAD);
        }
        return;
    }
    if (touchpad == TOUCHPAD_ELANTECH) {
        rc = elantech_byte(data);
        if (rc != ELANTECH_GOOD) {
            count_packet(rc == ELANTECH_BAD);
        }
        return;
    }
    if (alps > 0) {
        rc = alps_byte(data);
        if (rc != ALPS_GOOD) {
            count_packet(rc == ALPS_BAD);
        }
        return;
    }
    /* Byte 0 has the always-1 framing bit (bit 3). */
    if (packet_index == 0 && !(data & 0x08)) {
        count_packet(1);
        return;
    }
    packet_bytes[packet_index++] = data;
    if (packet_index >= packet_size) {
        report_packet();
        packet_index = 0;
        count_packet(0);
    }
}

/**
 * The IRQ 12 handler, which assembles a packet one byte an interrupt and
 * reports it.
 *
 * The 8042 raises IRQ 12 for every auxiliary byte. The handler reads the
 * data port only when status bit 0 (PS2_STATUS_OUTPUT_FULL) says a byte
 * waits and bit 5 (PS2_STATUS_MOUSE_DATA) says it is the auxiliary
 * device's. A keyboard byte is left in the output buffer for the keyboard's
 * IRQ 1 handler, and the packet is left as it was.
 *
 * A packet is framed by byte 0, whose bit 3 is always 1: a byte expected as
 * byte 0 with that bit clear is out of step with the stream and is dropped,
 * which brings the stream back in step, and counts as a bad packet.
 *
 * During a run of commands the handler reads nothing, and after five bad
 * packets in a row it drops every byte until kpsmoused has reconnected the
 * device.
 */
static void mouse_irq_handler(isr_params_t *params)
{
    uint8_t status;
    uint8_t data;

    (void)params;
    if (state == STATE_CMD) {
        return; /* the run of commands reads it */
    }
    status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL) || !(status & PS2_STATUS_MOUSE_DATA)) {
        return;
    }
    data = inb(PS2_DATA_PORT);
    if (state == STATE_IGNORE) {
        return;
    }
    if (bat_held) {
        bat_held = 0;
        if (data == 0x00) {
            state = STATE_IGNORE;
            klog(KLOG_WARN, "mouse", "the device announced itself anew: reconnecting");
            request_reconnect(RECONNECT_PORT);
            return;
        }
        take_byte(0xAA); /* not an announcement: the packet's first byte */
    } else if (data == 0xAA && packet_start()) {
        bat_held = 1;
        return;
    }
    take_byte(data);
}

/**
 * Find the device on the port and set it up, as Linux's `psmouse_connect`
 * does: the reset and the defaults, a refusal of either tolerated, since
 * some emulators answer neither; a Synaptics touchpad and the guest of its
 * pass-through port, or a mouse's protocol; and the event devices. Its
 * reporting is left for the caller to enable.
 */
static void connect_device(void)
{
    if (mouse_reset() < 0) {
        klog(KLOG_WARN, "mouse", "the device did not answer its reset");
    }
    if (mouse_send_command(MOUSE_CMD_SET_DEFAULTS) < 0) {
        klog(KLOG_WARN, "mouse", "the device refused the default settings");
    }
    touchpad = synaptics_init();
    if (touchpad <= 0) {
        detect_protocol();
    }
    if (touchpad <= 0) {
        register_mouse();
    } else if (touchpad == TOUCHPAD_SYNAPTICS && synaptics_has_pass_through()) {
        (void)ps2mouse_guest_connect(&synaptics_pt);
    }
}

/**
 * Interrupts are off so that the handshake's bytes are not taken by a stray
 * IRQ 12. The output buffer is drained first of what the firmware left in
 * it, at most PS2_DRAIN_LIMIT bytes: the buffer is one byte deep and a
 * working controller clears in a read or two, but one that reports a full
 * buffer for good would otherwise stop the boot here, with interrupts off
 * and nothing said, which on a laptop without a serial port leaves nothing
 * to read. A failed reset is tolerated because some emulators do not answer
 * it, and so is a refused default; a failed port enable, configuration byte
 * or reporting enable fails the bring-up.
 */
int init_mouse(void)
{
    uint8_t config;
    int response;

    arch_irq_disable();

    {
        int drained = PS2_DRAIN_LIMIT;

        while ((inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            inb(PS2_DATA_PORT);
            if (--drained <= 0) {
                klog(KLOG_WARN, "mouse", "the 8042 buffer never drained");
                arch_irq_enable();
                return -1;
            }
        }
    }

    if (ps2_send_command(PS2_CMD_ENABLE_MOUSE) < 0) {
        klog(KLOG_WARN, "mouse", "the aux port would not enable");
        arch_irq_enable();
        return -1;
    }

    if (ps2_send_command(PS2_CMD_READ_CONFIG) < 0) {
        klog(KLOG_WARN, "mouse", "the configuration byte would not read");
        arch_irq_enable();
        return -1;
    }
    if (ps2_wait_output() < 0) {
        klog(KLOG_WARN, "mouse", "reading the configuration byte timed out");
        arch_irq_enable();
        return -1;
    }
    config = inb(PS2_DATA_PORT);

    config |= PS2_CONFIG_MOUSE_IRQ;  /* enable IRQ12 */
    config &= ~PS2_CONFIG_MOUSE_CLK; /* clear the clock-disable bit */

    if (ps2_send_command(PS2_CMD_WRITE_CONFIG) < 0) {
        klog(KLOG_WARN, "mouse", "the configuration byte would not write");
        arch_irq_enable();
        return -1;
    }
    if (ps2_wait_input() < 0) {
        klog(KLOG_WARN, "mouse", "writing the configuration byte timed out");
        arch_irq_enable();
        return -1;
    }
    outb(PS2_DATA_PORT, config);

    connect_device();

    response = mouse_send_command(MOUSE_CMD_ENABLE);
    if (response < 0) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        arch_irq_enable();
        return -1;
    }

    register_isr_handler(44, mouse_irq_handler, "mouse");

    arch_irq_enable();

    return 0;
}

/** `mouse_clear_packets` calls this when the queue is emptied, so that half
 * a packet assembled before is not finished with the bytes after. */
void platform_mouse_resync(void)
{
    packet_index = 0;
}

#ifndef RACH2_HOSTTEST
/** Whether no run of commands is in progress. */
static int cmd_free(void *arg)
{
    (void)arg;
    return !cmd_busy;
}

/** Whether the tick in `*arg` has come. */
static int tick_passed(void *arg)
{
    return tick_count() >= *(const uint64_t *)arg;
}

/** Whether a reconnect is wanted. */
static int wanted(void *arg)
{
    (void)arg;
    return reconnect_wanted;
}

#endif

void ps2mouse_cmd_begin(void)
{
#ifndef RACH2_HOSTTEST
    while (cmd_busy) {
        (void)wq_wait_event_nointr(&cmd_wq, cmd_free, NULL);
    }
#endif
    cmd_busy = 1;
    state = STATE_CMD;
}

void ps2mouse_cmd_end(void)
{
    uint32_t flags;
    uint8_t status;

    flags = kirq_save();
    state = STATE_ACTIVE;
    cmd_busy = 0;
    status = inb(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OUTPUT_FULL) && (status & PS2_STATUS_MOUSE_DATA)) {
        mouse_irq_handler(NULL);
    }
    kirq_restore(flags);
#ifndef RACH2_HOSTTEST
    wq_wake_all(&cmd_wq);
#endif
}

const struct ps2io *ps2mouse_guest_change_begin(void)
{
    ps2mouse_cmd_begin();
    if (!guest.active) {
        ps2mouse_cmd_end();
        return NULL;
    }
    (void)deactivate(&ps2mouse_aux);
    (void)deactivate(guest.io);
    return guest.io;
}

void ps2mouse_guest_change_end(void)
{
    if (io_cmd(guest.io, MOUSE_CMD_ENABLE) < 0) {
        klog(KLOG_WARN, "mouse", "pass-through: the guest did not enable reporting");
    }
    if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
    }
    ps2mouse_cmd_end();
}

/**
 * Set the guest up again after its touchpad's reconnect, or after it
 * announced itself anew, as Linux's serio core reconnects a pass-through
 * port: a guest found before is reconnected, and probed anew when that
 * fails; a port that had none is probed anew. The touchpad's reporting is
 * enabled after.
 */
static void reconnect_guest(void)
{
    if (guest.dev && guest_reconnect() < 0) {
        klog(KLOG_WARN, "mouse", "pass-through: the guest's reconnect failed: probing anew");
        guest_forget();
    }
    if (!guest.dev) {
        (void)ps2mouse_guest_connect(&synaptics_pt);
    }
    if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
    }
}

/**
 * Set the mouse up again as Linux's `psmouse_reconnect` does for a protocol
 * that has no reconnect of its own: the reset and the defaults, and its
 * protocol found again, which must be the one found before. Returns 0 when
 * it is, and the mouse keeps its event device; -1 when the mouse does not
 * answer its reset, speaks another protocol now or is a touchpad.
 */
static int mouse_reconnect(void)
{
    int old = protocol;

    if (mouse_reset() < 0) {
        return -1;
    }
    if (mouse_send_command(MOUSE_CMD_SET_DEFAULTS) < 0) {
        klog(KLOG_WARN, "mouse", "the device refused the default settings");
    }
    detect_protocol();
    return protocol == old && touchpad == 0 && alps == 0 ? 0 : -1;
}

void ps2mouse_reconnect(void)
{
    int what;

    ps2mouse_cmd_begin();
    what = reconnect_wanted;
    reconnect_wanted = 0;
    bat_held = 0;
    if (what == RECONNECT_GUEST) {
        if (touchpad == TOUCHPAD_SYNAPTICS && synaptics_has_pass_through()) {
            reconnect_guest();
        }
    } else if (touchpad == TOUCHPAD_SYNAPTICS && synaptics_reconnect() == 0) {
        (void)mouse_send_command(MOUSE_CMD_ENABLE);
        if (synaptics_has_pass_through()) {
            reconnect_guest();
        }
        klog(KLOG_INFO, "mouse", "reconnect: the touchpad reports again");
    } else if ((touchpad == TOUCHPAD_ELANTECH && elantech_reconnect() == 0) ||
               (alps > 0 && alps_reconnect() == 0)) {
        if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
            klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        }
        klog(KLOG_INFO, "mouse", "reconnect: the touchpad reports again");
    } else if (touchpad == 0 && alps == 0 && mouse_dev && mouse_reconnect() == 0) {
        packet_index = 0;
        if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
            klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        }
        klog(KLOG_INFO, "mouse", "reconnect: the mouse reports again");
    } else {
        klog(KLOG_WARN, "mouse", "reconnect: probing the port anew");
        guest_forget();
        synaptics_disconnect();
        if (touchpad == TOUCHPAD_ELANTECH) {
            elantech_disconnect();
        }
        if (alps > 0) {
            alps_disconnect();
        }
        if (mouse_dev) {
            evdev_unregister(mouse_dev);
            mouse_dev = NULL;
        }
        packet_index = 0;
        connect_device();
        if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
            klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        }
    }
    bad_in_a_row = 0;
    ps2mouse_cmd_end();
}

void ps2mouse_msleep(unsigned int ms)
{
#ifdef RACH2_HOSTTEST
    kernel_ticks += MS_TO_TICKS(ms);
#else
    uint64_t due = tick_count() + MS_TO_TICKS(ms);

    current_proc->wakeup_tick = due;
    (void)wq_wait_event_idle(&sleep_wq, tick_passed, &due);
    current_proc->wakeup_tick = 0;
#endif
}

#ifndef RACH2_HOSTTEST
/** The worker, kpsmoused: sleep until the handler asks for a reconnect,
 * then reconnect, holding the big lock as kernel code does. */
static void psmoused(void)
{
    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&reconnect_wq, wanted, NULL);
        ps2mouse_reconnect();
    }
}

void ps2mouse_thread_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (thread) {
        return;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "mouse", "worker: refused: no record or stack for it");
        return;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kpsmoused");
    setup_process_stack(p, psmoused);
    thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "mouse", "worker: kpsmoused is pid %d", (int)p->pid);
}
#else
void ps2mouse_thread_start(void)
{}
#endif
