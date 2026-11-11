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
 * The IntelliMouse and Explorer answer Microsoft's rate knocks with IDs
 * 3 and 4 and send a fourth byte for wheels and extra buttons. A Genius
 * NetMouse also sends four bytes. The event device has Linux's name,
 * numbers and capabilities for each protocol.
 *
 * The bring-up asks first for a Kensington ThinkingMouse, then for a
 * Synaptics touchpad. A Synaptics pad that comes up speaks its absolute
 * protocol; one whose setup fails goes back to relative mode and skips
 * the other pads' probes. Otherwise ALPS is tried, then Elantech when no
 * ALPS pad answered. Each pad's driver takes its absolute packets, and a
 * failed setup falls back to a mouse named for its vendor. Without a
 * Synaptics or ALPS pad, Genius and TrackPoint are tried, also after an
 * Elantech setup failed; Microsoft's wheel knocks follow. A Synaptics
 * pad's pass-through port carries a TrackPoint's or a mouse's packets.
 *
 * After the bring-up the handler owns the port, as Linux's psmouse does in
 * its state PSMOUSE_ACTIVATED. A run of commands, a change of the guest's
 * settings or a reconnect, takes it back for its length: the handler then
 * leaves the mouse's bytes in the 8042 for the run to read, as Linux's
 * PSMOUSE_CMD_MODE gives them to `ps2_command`. The configured number of
 * bad packets in a row, `resetafter`, puts it in PSMOUSE_IGNORE, whose
 * bytes are dropped, until kpsmoused has reconnected the device. A pause of
 * half a second inside a packet, or a first byte after the device was idle
 * for `resync_time` seconds, puts it in PSMOUSE_RESYNCING instead, until
 * kpsmoused has polled the device for a packet and found it speaking its
 * protocol still, or reconnected it.
 *
 * Each port, the auxiliary port's serio1 and the pass-through port's
 * serio2, has Linux's psmouse files: `protocol`, which names the protocol
 * and takes a name of psmouse's table to switch the port to, `rate`,
 * `resolution`, `resetafter` and `resync_time`. A write goes through Linux's
 * `psmouse_attr_set_helper`: a run of commands with the device disabled,
 * and the touchpad too for the guest.
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
#include "../../fs/sysfs.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../lib/kstring.h"
#include "isr.h"
#include "../../kio.h"
#include "../../serial.h"
#include "../../proc/wait_queue.h"
#ifndef RACH2_HOSTTEST
#include "../../proc/percpu.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/smp.h"
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
#define MOUSE_CMD_POLL         0xEB /* Read a packet; it comes after the ACK */

/* Mouse Response Codes */
#define MOUSE_ACK    0xFA /* Command acknowledged */
#define MOUSE_RESEND 0xFE /* Resend request */

/* Configuration byte bits */
#define PS2_CONFIG_MOUSE_IRQ 0x02 /* Enable IRQ12 for mouse */
#define PS2_CONFIG_MOUSE_CLK 0x20 /* Disable mouse clock (set=disabled) */

/* The protocols, Linux's `enum psmouse_type`, which EVIOCGID gives as the
 * product, and the IDs a mouse answers once it speaks ImPS/2 and
 * ImExPS/2. */
#define PROTO_PS2        1
#define PROTO_THINKPS    3
#define PROTO_GENPS      4
#define PROTO_IMPS       5
#define PROTO_IMEX       6
#define PROTO_SYNAPTICS  7
#define PROTO_ALPS       8
#define PROTO_TRACKPOINT 10
#define PROTO_CORTRON    12
#define PROTO_ELANTECH   14
#define PROTO_SYNREL     16
#define PROTO_UNDRIVEN   24 /* a known name whose detect is not implemented */
#define PROTO_AUTO       23
#define ID_IMPS          3
#define ID_IMEX          4

/* The port's states, Linux's `enum psmouse_state` as far as the handler
 * tells them apart: it reports, it leaves the bytes to a run of commands,
 * it drops them until a reconnect, or until a resync. */
#define STATE_ACTIVE 0
#define STATE_CMD    1
#define STATE_IGNORE 2
#define STATE_RESYNC 3

/* What a byte did to its packet, Linux's `psmouse_ret_t`: it broke the
 * packet's form, the packet goes on, or it ended one. */
#define PS_BAD  -1
#define PS_GOOD 0
#define PS_FULL 1

/*
 * The settings of a port that its files show, as Linux's `struct psmouse`
 * keeps them and psmouse's module parameters give them at a connect: the
 * reports a second; the counts an inch; the bad packets in a row after
 * which the device is reconnected, or bytes for an ALPS pad, which gives
 * its own, 0 for never; and the seconds the device may be idle before its
 * next byte is taken as a resync's cue, 0 for never.
 */
struct port_settings {
    unsigned int rate;
    unsigned int resolution;
    unsigned int resetafter;
    unsigned int resync_time;
};

/* The settings a connect gives, Linux's defaults. */
static const struct port_settings defaults = {100, 200, 5, 0};

/* Ports encoded in the attribute callbacks' arguments. */
#define ON_AUX   1
#define ON_GUEST 2

/* Linux's psmouse names and aliases. A known protocol without a detect
 * follows the same reset and PS/2 fallback as a detect that fails. */
static const struct {
    int type;
    const char *name;
    const char *alias;
} protocols[] = {{PROTO_PS2, "PS/2", "bare"},
                 {PROTO_UNDRIVEN, "PS2++", "logitech"},
                 {PROTO_THINKPS, "ThinkPS/2", "thinkps"},
                 {PROTO_UNDRIVEN, "CyPS/2", "cypress"},
                 {PROTO_GENPS, "GenPS/2", "genius"},
                 {PROTO_IMPS, "ImPS/2", "imps"},
                 {PROTO_IMEX, "ImExPS/2", "exps"},
                 {PROTO_SYNAPTICS, "SynPS/2", "synaptics"},
                 {PROTO_SYNREL, "SynRelPS/2", "synaptics-relative"},
                 {PROTO_UNDRIVEN, "SynSMBus", "synaptics-smbus"},
                 {PROTO_ALPS, "AlpsPS/2", "alps"},
                 {PROTO_UNDRIVEN, "LBPS/2", "lifebook"},
                 {PROTO_TRACKPOINT, "TPPS/2", "trackpoint"},
                 {PROTO_UNDRIVEN, "touchkitPS/2", "touchkit"},
                 {PROTO_UNDRIVEN, "OLPC HGPK", "hgpk"},
                 {PROTO_ELANTECH, "ETPS/2", "elantech"},
                 {PROTO_UNDRIVEN, "ETSMBus", "elantech-smbus"},
                 {PROTO_UNDRIVEN, "FSPPS/2", "fsp"},
                 {PROTO_CORTRON, "CortronPS/2", "cortps"},
                 {PROTO_UNDRIVEN, "FocalTechPS/2", "focaltech"},
                 {PROTO_UNDRIVEN, "VirtualPS/2", "vmmouse"},
                 {PROTO_UNDRIVEN, "BYDPS/2", "byd"},
                 {PROTO_AUTO, "auto", "any"}};

/* The guest's place, as Linux's psmouse names the device of the touchpad's
 * pass-through port. */
#define GUEST_PHYS "synaptics-pt/serio0/input0"

/* The packet being assembled, the index of its next byte, and its size,
 * three bytes or four with a wheel. */
static uint8_t packet_bytes[4];
static int packet_index = 0;
static int packet_size = 3;

/* The protocol the device speaks, PROTO_*, and its event device when it
 * speaks a mouse's protocol; a touchpad's drivers keep their own. */
static int protocol = PROTO_PS2;
static struct evdev *mouse_dev;

/* The settings of the auxiliary port; the device's vendor and model, as
 * Linux's `psmouse->vendor` and `psmouse->name` name it, one string, NULL
 * until a detect names it; and the event device's name built from them. */
static struct port_settings aux = {100, 200, 5, 0};
static const char *aux_who;
static char aux_who_text[40];
static char aux_devname[64];

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
static unsigned int bad_in_a_row;

/* The tick of the device's last byte, or of the port's last change of
 * state, Linux's `psmouse->last`; the first byte of the packet under way,
 * and the byte that began the packet a resync dropped, Linux's
 * `badbyte`, which ALPS's poll reads; the resyncs tried, and whether the
 * device ever acknowledged a resync's disable, as Linux counts them. */
static uint32_t last_tick;
static uint8_t packet_first;
static uint8_t badbyte;
static unsigned int num_resyncs;
static int acks_disable;

/* What kpsmoused is asked to do: reconnect the port, the touchpad or mouse
 * and the guest after it, or the guest alone; or resync the port's device
 * or the guest. */
#define RECONNECT_PORT  1
#define RECONNECT_GUEST 2
#define RESYNC_PORT     4
#define RESYNC_GUEST    8

/* A run of commands is in progress, and a reconnect or resync is wanted,
 * RECONNECT_* and RESYNC_*. */
static volatile int cmd_busy;
static volatile int reconnect_wanted;

/* `init_mouse` is bringing the device up, with interrupts off. */
static int booting;

/* The main device reserved before a protocol switch, consumed by the
 * mouse's or touchpad's registration under the command lock. */
static struct evdev *replacement;

/* Callers waiting for the command lock. */
static wait_queue_t cmd_wq;

/* The byte 0xAA began a packet: with 0x00 after it, the announcement of a
 * device that reset itself, Linux's PSMOUSE_RET_BAT and PSMOUSE_RET_ID. */
static int bat_held;

#ifndef RACH2_HOSTTEST
/* kpsmoused, which waits on `reconnect_wq`; the callers of a run of
 * commands that wait for another's end, on `cmd_wq`; and the sleeps of
 * `ps2mouse_msleep`. */
static process_t *thread;
static wait_queue_t reconnect_wq;
static wait_queue_t sleep_wq; /* woken by nothing: the sleep ends at its tick */
#endif

/*
 * The guest, the device on the touchpad's pass-through port: its line;
 * whether it reports; its protocol, PROTO_*, and packet size; its event
 * device, NULL when there is no guest; the side and extra buttons held;
 * the stick's buttons the touchpad gave out of band, Linux's
 * `extra_buttons`, in a packet's first byte's places; its settings, of
 * which Linux zeroes `resync_time` for a pass-through port; its vendor and
 * model, as `aux_who`; the tick of its last packet; whether its packets
 * are dropped for a resync; and its resyncs, as the port's.
 */
static struct {
    const struct ps2io *io;
    int active;
    int protocol;
    int packet_size;
    struct evdev *dev;
    unsigned int side;
    uint8_t extra;
    struct port_settings set;
    const char *who;
    uint32_t last_tick;
    int resyncing;
    unsigned int num_resyncs;
    int acks_disable;
} guest;

/* The guest's vendor and model when a detect builds them, and its event
 * device's name. */
static char guest_who_text[40];
static char guest_devname[64];

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
    uint32_t start = ps2_wait_begin();
    int reads = 0;
    uint8_t status;

    while (!ps2_wait_over(start, reads++)) {
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

/* The files of each port, which /sys adds when it makes the port's
 * directory. */
static int aux_files(int dir);
static int guest_files(int dir);

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

/** Write `a`, a space and `b` to `out`, of `cap` bytes, cut to fit, and
 * return it: a name as Linux's psmouse joins a protocol's name to a
 * vendor and a model, or a vendor to a model. */
static char *join(char *out, int cap, const char *a, const char *b)
{
    int n = 0;

    for (; *a && n < cap - 1; a++) {
        out[n++] = *a;
    }
    if (n < cap - 1) {
        out[n++] = ' ';
    }
    for (; *b && n < cap - 1; b++) {
        out[n++] = *b;
    }
    out[n] = 0;
    return out;
}

/** The name of the protocol `type` in psmouse's table, which the port's
 * `protocol` file shows and the event device's name begins with. */
static const char *proto_name(int type)
{
    unsigned int i;

    for (i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
        if (protocols[i].type == type) {
            return protocols[i].name;
        }
    }
    return protocols[0].name;
}

/** Whether the `len` bytes at `buf` are the word `word`. */
static int is_word(const char *word, const char *buf, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len && word[i] && word[i] == buf[i]; i++) {
    }
    return i == len && word[i] == 0;
}

/**
 * The protocol named by the `len` bytes at `buf`, a name or an alias of
 * psmouse's table matched whole, as Linux's `psmouse_protocol_by_name`
 * matches the bytes written, a newline among them; -EINVAL for none.
 */
static int proto_lookup(const char *buf, uint32_t len)
{
    unsigned int i;

    for (i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
        if (is_word(protocols[i].name, buf, len) || is_word(protocols[i].alias, buf, len)) {
            return protocols[i].type;
        }
    }
    return -EINVAL;
}

/** Linux's `psmouse_set_rate` on `io`: the highest of its rates that is
 * not above `rate`, sent, a refusal ignored. Returns the rate sent. */
static unsigned int io_set_rate(const struct ps2io *io, unsigned int rate)
{
    static const uint8_t rates[] = {200, 100, 80, 60, 40, 20, 10, 0};
    int i = 0;

    while (rates[i] > rate) {
        i++;
    }
    (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, rates[i]);
    return rates[i];
}

/** Linux's `psmouse_set_resolution` on `io`: `resolution` counts an inch,
 * 200 for 0 or above, sent as the code of 25, 50, 100 or 200, a refusal
 * ignored. Returns the counts an inch of the code sent. */
static unsigned int io_set_resolution(const struct ps2io *io, unsigned int resolution)
{
    static const uint8_t codes[] = {0, 1, 2, 2, 3};
    uint8_t p;

    if (resolution == 0 || resolution > 200) {
        resolution = 200;
    }
    p = codes[resolution / 50];
    (void)io_cmd_arg(io, MOUSE_CMD_SET_RES, p);
    return 25u << p;
}

unsigned int ps2mouse_set_rate(unsigned int rate)
{
    return io_set_rate(&ps2mouse_aux, rate);
}

void ps2mouse_initialize(unsigned int (*set_rate)(unsigned int))
{
    aux.rate = (set_rate ? set_rate : ps2mouse_set_rate)(aux.rate);
    aux.resolution = io_set_resolution(&ps2mouse_aux, aux.resolution);
    (void)io_cmd(&ps2mouse_aux, MOUSE_CMD_SET_SCALE11);
}

unsigned int ps2mouse_port_rate(void)
{
    return aux.rate;
}

void ps2mouse_port_rate_set(unsigned int rate)
{
    aux.rate = rate;
}

/** The rate written to the auxiliary port's `rate` file, through the
 * protocol's own `set_rate`, as Linux's psmouse calls it. Returns the rate
 * it set. */
static unsigned int aux_set_rate(unsigned int rate)
{
    if (touchpad == TOUCHPAD_SYNAPTICS) {
        return synaptics_set_rate(rate);
    }
    if (touchpad == TOUCHPAD_ELANTECH) {
        return elantech_set_rate(rate);
    }
    return ps2mouse_set_rate(rate);
}

/**
 * Linux's `thinking_detect` on `io`: rate 10, resolution 0, then nine rates
 * that a Kensington ThinkingMouse or ExpertMouse answers with the ID 2, each
 * refusal ignored. Returns 0 and names the device in `*who` when it
 * answered, -1 when not.
 */
static int thinking_detect(const struct ps2io *io, const char **who)
{
    static const uint8_t seq[9] = {20, 60, 40, 20, 20, 60, 40, 20, 20};
    uint8_t id = seq[8];
    int i;

    (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, 10);
    (void)io_cmd_arg(io, MOUSE_CMD_SET_RES, 0);
    for (i = 0; i < 9; i++) {
        (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, seq[i]);
    }
    (void)ps2io_command(io, MOUSE_CMD_GET_ID, 0, 0, &id, 1);
    if (id != 2) {
        return -1;
    }
    *who = "Kensington ThinkingMouse";
    return 0;
}

/**
 * Linux's `genius_detect` on `io`: resolution 3, 1:1 scaling three times
 * and a status request, which a Genius NetMouse answers with 0x00 0x33
 * 0x55. Returns 0 and names the device in `*who` when it answered, -1 when
 * not.
 */
static int genius_detect(const struct ps2io *io, const char **who)
{
    uint8_t param[3];

    param[0] = 3;
    param[1] = 0;
    param[2] = 0;
    (void)io_cmd_arg(io, MOUSE_CMD_SET_RES, 3);
    (void)io_cmd(io, MOUSE_CMD_SET_SCALE11);
    (void)io_cmd(io, MOUSE_CMD_SET_SCALE11);
    (void)io_cmd(io, MOUSE_CMD_SET_SCALE11);
    (void)ps2io_command(io, MOUSE_CMD_GET_INFO, 0, 0, param, 3);
    if (param[0] != 0x00 || param[1] != 0x33 || param[2] != 0x55) {
        return -1;
    }
    *who = "Genius Mouse";
    return 0;
}

/** A TrackPoint's vendor and model, Linux's vendor and "TrackPoint",
 * written to `buf`, of 40 bytes, and returned. */
static const char *trackpoint_who(char *buf)
{
    return join(buf, 40, trackpoint_vendor(), "TrackPoint");
}

/** An ALPS pad's vendor and model, "ALPS GlidePoint" or "ALPS DualPoint
 * TouchPad", from the name alps.c builds after a protocol's. */
static const char *alps_who(void)
{
    const char *s = alps_mouse_name("");

    return *s == ' ' ? s + 1 : s;
}

/** Name the device of the protocol `type`, as a protocol that does not
 * name it does, when no detect has: Linux's "Generic" vendor and the
 * model of a mouse, a wheel mouse or an Explorer. */
static const char *who_default(const char *who, int type)
{
    if (who) {
        return who;
    }
    return type == PROTO_IMPS   ? "Generic Wheel Mouse"
           : type == PROTO_IMEX ? "Generic Explorer Mouse"
                                : "Generic Mouse";
}

/**
 * Find the protocol of a device that is no Synaptics pad in absolute mode,
 * as Linux's `psmouse_extensions` goes on after its Synaptics probe: when
 * no Synaptics pad answered, an ALPS touchpad (`alps_init`), which alps.c
 * sets up whole, and when no ALPS pad answered either, an Elantech touchpad
 * (`elantech_init`); each speaks its own protocol once it is set up. When
 * neither a Synaptics pad nor an ALPS pad answered, a Genius NetMouse and a
 * TrackPoint (`trackpoint_detect`), which speaks the plain protocol;
 * otherwise the IntelliMouse's knock, rates 200, 100 and 80, then the
 * Explorer's, 200, 200 and 80, with the rates 200, 80 and 40 that turn on
 * an Explorer 4.0's horizontal wheel. Then the port's rate, resolution and
 * 1:1 scaling. A mouse that answers neither knock is a plain PS/2 mouse of
 * three buttons.
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
    if (alps != 0) {
        aux_who = alps_who();
    }
    if (alps > 0) {
        protocol = PROTO_ALPS;
        aux.resetafter = (unsigned int)alps_reset_after();
        aux.resync_time = 0;
        return;
    }
    /* A pad that answered ALPS's reports is not asked for Elantech's. */
    if (touchpad == 0 && alps == 0) {
        i = elantech_init();
        if (i != 0) {
            aux_who = "Elantech Touchpad";
        }
        if (i > 0) {
            touchpad = TOUCHPAD_ELANTECH;
            protocol = PROTO_ELANTECH;
            return;
        }
        if (i < 0) {
            touchpad = -TOUCHPAD_ELANTECH;
        }
    }
    if (touchpad != -TOUCHPAD_SYNAPTICS && alps == 0 &&
        genius_detect(&ps2mouse_aux, &aux_who) == 0) {
        protocol = PROTO_GENPS;
        packet_size = 4;
    } else if (touchpad != -TOUCHPAD_SYNAPTICS && alps == 0 &&
               trackpoint_detect(&ps2mouse_aux) == 0) {
        protocol = PROTO_TRACKPOINT;
        aux_who = trackpoint_who(aux_who_text);
    } else if (knock(imps) == ID_IMPS) {
        protocol = PROTO_IMPS;
        packet_size = 4;
        if (knock(imex) == ID_IMEX) {
            protocol = PROTO_IMEX;
            for (i = 0; i < 3; i++) {
                mouse_set(MOUSE_CMD_SET_RATE, hwheel[i]);
            }
        }
    }
    ps2mouse_initialize(NULL);
}

/**
 * Find the device's protocol whole, as Linux's `psmouse_extensions` does
 * for the protocol "auto": a Kensington ThinkingMouse first, which
 * Synaptics' probe upsets; a Synaptics touchpad (`synaptics_init`), which
 * speaks its own protocol once it is set up, and one whose set-up failed
 * keeps the probes that follow from a pad's protocols; then
 * `detect_protocol`.
 */
static void probe_protocol(void)
{
    touchpad = 0;
    alps = 0;
    protocol = PROTO_PS2;
    packet_size = 3;
    if (thinking_detect(&ps2mouse_aux, &aux_who) == 0) {
        protocol = PROTO_THINKPS;
        ps2mouse_initialize(NULL);
        return;
    }
    touchpad = synaptics_init();
    if (touchpad != 0) {
        aux_who = "Synaptics TouchPad";
    }
    if (touchpad > 0) {
        protocol = PROTO_SYNAPTICS;
        aux.resync_time = 0;
        return;
    }
    detect_protocol();
}

/**
 * Detect and set up the protocol `type` on the auxiliary port, as Linux's
 * `psmouse_switch_protocol` does for a protocol written to `protocol`: its
 * detect alone, and for a touchpad its set-up after, which registers the
 * pad's event devices. Returns 0 when the device speaks it, -1 when not.
 */
static int try_protocol(int type)
{
    static const uint8_t imps[3] = {200, 100, 80};
    static const uint8_t imex[3] = {200, 200, 80};
    static const uint8_t hwheel[3] = {200, 80, 40};
    const struct ps2io *io = &ps2mouse_aux;
    int rc, i;

    switch (type) {
    case PROTO_PS2:
        return 0;
    case PROTO_THINKPS:
        return thinking_detect(io, &aux_who);
    case PROTO_GENPS:
        packet_size = 4;
        return genius_detect(io, &aux_who);
    case PROTO_IMPS:
        packet_size = 4;
        return io_knock(io, imps) == ID_IMPS ? 0 : -1;
    case PROTO_IMEX:
        packet_size = 4;
        (void)io_knock(io, imps);
        if (io_knock(io, imex) != ID_IMEX) {
            return -1;
        }
        for (i = 0; i < 3; i++) {
            (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, hwheel[i]);
        }
        return 0;
    case PROTO_TRACKPOINT:
        if (trackpoint_detect(io) < 0) {
            return -1;
        }
        aux_who = trackpoint_who(aux_who_text);
        return 0;
    case PROTO_CORTRON:
        aux_who = "Cortron PS/2 Trackball";
        return 0;
    case PROTO_SYNREL:
        rc = synaptics_init_relative();
        if (rc != 0) {
            aux_who = "Synaptics TouchPad";
            touchpad = -TOUCHPAD_SYNAPTICS;
        }
        return rc > 0 ? 0 : -1;
    case PROTO_SYNAPTICS:
        rc = synaptics_init();
        if (rc != 0) {
            aux_who = "Synaptics TouchPad";
        }
        touchpad = rc > 0 ? TOUCHPAD_SYNAPTICS : rc < 0 ? -TOUCHPAD_SYNAPTICS : 0;
        if (rc > 0) {
            aux.resync_time = 0;
        }
        return rc > 0 ? 0 : -1;
    case PROTO_ALPS:
        alps = alps_init();
        if (alps != 0) {
            aux_who = alps_who();
        }
        if (alps > 0) {
            aux.resetafter = (unsigned int)alps_reset_after();
            aux.resync_time = 0;
        }
        return alps > 0 ? 0 : -1;
    case PROTO_ELANTECH:
        rc = elantech_init();
        if (rc != 0) {
            aux_who = "Elantech Touchpad";
        }
        touchpad = rc > 0 ? TOUCHPAD_ELANTECH : rc < 0 ? -TOUCHPAD_ELANTECH : 0;
        return rc > 0 ? 0 : -1;
    default:
        return -1;
    }
}

/**
 * Linux's `psmouse->poll` on the auxiliary port: the packet the device
 * answers 0xEB with, of the protocol's size, into `buf`, of 8 bytes, or an
 * ALPS pad's own poll (`alps_poll`). Returns the packet's size, or -1 when
 * it did not come whole.
 */
static int port_poll(uint8_t *buf)
{
    int n = packet_size;

    if (alps > 0) {
        return alps_poll(badbyte, buf);
    }
    if (touchpad == TOUCHPAD_SYNAPTICS) {
        n = 6;
    } else if (touchpad == TOUCHPAD_ELANTECH) {
        n = elantech_packet_size();
    }
    return ps2io_command(&ps2mouse_aux, MOUSE_CMD_POLL, 0, 0, buf, n) < 0 ? -1 : n;
}

/**
 * Set the port up for the protocol `type`, or PROTO_AUTO for the one
 * `probe_protocol` finds, as Linux's `psmouse_switch_protocol` does: a
 * protocol the device does not speak leaves it reset and speaking PS/2.
 * Then the device is named as psmouse names it, and `resync_time` is
 * zeroed for a packet of three bytes, which a reset cannot shorten, and
 * for a device that does not answer a poll. The touchpads' drivers set
 * their rate and resolution themselves, and `probe_protocol` a mouse's;
 * here a mouse's protocol written to `protocol` gets the port's.
 */
static void set_up_protocol(int type)
{
    uint8_t buf[8];

    touchpad = 0;
    alps = 0;
    protocol = PROTO_PS2;
    packet_size = 3;
    extra_buttons = 0;
    if (type == PROTO_AUTO) {
        probe_protocol();
    } else {
        if (try_protocol(type) == 0) {
            protocol = type;
        } else {
            (void)mouse_reset();
            protocol = PROTO_PS2;
            packet_size = 3;
        }
        if (touchpad <= 0 && alps <= 0) {
            ps2mouse_initialize(NULL);
        }
    }
    if (touchpad <= 0 && alps <= 0) {
        aux_who = who_default(aux_who, protocol);
    }
    if (touchpad <= 0 && alps <= 0 && packet_size == 3) {
        aux.resync_time = 0;
    } else if (aux.resync_time && port_poll(buf) < 0) {
        aux.resync_time = 0;
    }
}

/** What a mouse of the protocol `type` has beyond two buttons and X and
 * Y, MOUSE_*, as Linux's detect of each sets it. */
static unsigned int mouse_caps(int type)
{
    switch (type) {
    case PROTO_IMPS:
        return MOUSE_POINTER | MOUSE_WHEEL;
    case PROTO_IMEX:
        return MOUSE_POINTER | MOUSE_WHEEL | MOUSE_HWHEEL | MOUSE_SIDE;
    case PROTO_GENPS:
        return MOUSE_POINTER | MOUSE_WHEEL | MOUSE_SIDE;
    case PROTO_TRACKPOINT:
        return MOUSE_POINTER | (trackpoint_buttons() < 3 ? MOUSE_NO_MIDDLE : 0);
    default:
        return MOUSE_POINTER;
    }
}

/** Register the port's main device, consuming the reservation held by a
 * protocol switch, or allocating at boot or reconnect. */
struct evdev *ps2mouse_register(const char *name, uint16_t product, const uint8_t *keys)
{
    struct evdev *dev = replacement;

    replacement = NULL;
    return evdev_register_reserved(dev, name, EVDEV_BUS_I8042, 0x0002, product, keys);
}

/** Register a relative device with its protocol's name and capabilities. */
static struct evdev *register_device(char *name, const char *phys, int type, const char *who)
{
    struct evdev *dev;
    uint8_t keys[EVDEV_KEY_BYTES];

    k_memset(keys, 0, sizeof(keys));
    dev = ps2mouse_register(join(name, 64, proto_name(type), who), (uint16_t)type, keys);
    mouse_configure(dev, phys, mouse_caps(type));
    if (type == PROTO_THINKPS) {
        evdev_set_capability(dev, EV_KEY, BTN_EXTRA);
    } else if (type == PROTO_CORTRON) {
        evdev_set_capability(dev, EV_KEY, BTN_SIDE);
    } else if (type == PROTO_TRACKPOINT) {
        evdev_set_prop(dev, INPUT_PROP_POINTING_STICK);
    }
    return dev;
}

/** Register the event device of the mouse, as `register_device` names it,
 * unless a touchpad's driver has its own: "Synaptics TouchPad", "Elantech
 * Touchpad" or the ALPS pad's model for a touchpad that answered its
 * vendor's knock and was not set up. */
static void register_mouse(void)
{
    if (touchpad > 0 || alps > 0) {
        return;
    }
    mouse_dev = register_device(aux_devname, "isa0060/serio1/input0", protocol, aux_who);
}

/** Give serio1 the files of its protocol: a TrackPoint's settings, or an
 * Elantech pad's registers. */
static void bind_files(void)
{
    evdev_sysfs_serio_files(1, protocol == PROTO_TRACKPOINT ? trackpoint_sysfs_add
                               : protocol == PROTO_ELANTECH ? elantech_sysfs_add
                                                            : NULL);
}

/** The low `bits` + 1 bits of `v` as a signed number, bit `bits` its sign,
 * as Linux's `sign_extend32`. */
static int sign_extend(unsigned v, int bits)
{
    int sign = 1 << bits;

    return (int)((v & ((unsigned)sign * 2u - 1u)) ^ (unsigned)sign) - sign;
}

/**
 * Report the whole packet `p` of a mouse of the protocol `type` on `dev`,
 * as Linux's `psmouse_process_byte` and `psmouse_report_standard_motion`
 * read it: the fourth byte of a wheel's protocol, the side and extra
 * buttons of each protocol that has them, into `*held`, which keeps an
 * Explorer's while its packets of a wheel's turn of more than 7 do not
 * carry them, the ThinkingMouse's Y overflow taken as X's top bit, then
 * the buttons, ORed with `oob`, the buttons a touchpad gave out of band,
 * and the 9-bit movements, Y turned to Linux's way up: the overflow bits
 * are not looked at, and a movement byte of 0 is no movement whatever its
 * sign.
 */
static void report(struct evdev *dev, int type, const uint8_t *p, unsigned int oob,
                   unsigned int *held)
{
    unsigned int b0 = p[0] | oob, b1 = p[1];
    uint8_t b3 = p[3];
    int dx, dy, wheel = 0, hwheel = 0;

    switch (type) {
    case PROTO_IMPS:
        wheel = -(int)(int8_t)b3;
        break;
    case PROTO_IMEX:
        switch (b3 & 0xC0) {
        case 0x80: /* an Explorer 4.0's vertical turn of up to 31 */
            wheel = -sign_extend(b3, 5);
            break;
        case 0x40: /* its horizontal turn */
            hwheel = -sign_extend(b3, 5);
            break;
        default:
            wheel = -sign_extend(b3, 3);
            *held = ((b3 & 0x10) ? MOUSE_BUTTON_SIDE : 0) | ((b3 & 0x20) ? MOUSE_BUTTON_EXTRA : 0);
            break;
        }
        break;
    case PROTO_GENPS: /* a NetMouse's wheel; a NewNet 3D's buttons in the overflow bits */
        wheel = -(int)(int8_t)b3;
        *held = ((p[0] & 0x40) ? MOUSE_BUTTON_SIDE : 0) | ((p[0] & 0x80) ? MOUSE_BUTTON_EXTRA : 0);
        break;
    case PROTO_THINKPS:
        *held = (p[0] & 0x08) ? MOUSE_BUTTON_EXTRA : 0;
        b1 |= (p[0] & 0x40u) << 1;
        break;
    case PROTO_CORTRON:
        *held = (p[0] & 0x08) ? MOUSE_BUTTON_SIDE : 0;
        break;
    default:
        break;
    }
    dx = b1 ? (int)b1 - (int)((b0 << 4) & 0x100u) : 0;
    dy = p[2] ? (int)p[2] - (int)((b0 << 3) & 0x100u) : 0;
    mouse_report(dev, (b0 & 0x07u) | *held, dx, -dy, wheel, hwheel);
}

/** Report the whole packet in `packet_bytes` on the mouse's device. */
static void report_packet(void)
{
    report(mouse_dev, protocol, packet_bytes, 0, &extra_buttons);
}

/**
 * Find what the guest on `io` speaks once it is known for a mouse, as
 * Linux's `psmouse_extensions` finds it on a pass-through port, where only
 * the protocols Linux tries there are tried: the defaults, which Linux
 * sends before the ALPS probe it skips there; a TrackPoint's ID, set up and
 * named when `set_properties` (`trackpoint_detect`) or only read; then the
 * defaults, the reset, and the IntelliMouse Explorer's knock after the
 * IntelliMouse's, with an Explorer 4.0's horizontal wheel turned on, then
 * the IntelliMouse's alone. Returns PROTO_*.
 */
static int guest_extensions(const struct ps2io *io, int set_properties)
{
    static const uint8_t imps[3] = {200, 100, 80};
    static const uint8_t imex[3] = {200, 200, 80};
    static const uint8_t hwheel[3] = {200, 80, 40};
    int i;

    (void)io_cmd(io, MOUSE_CMD_SET_DEFAULTS);
    if ((set_properties ? trackpoint_detect(io) : trackpoint_identify(io)) == 0) {
        if (set_properties) {
            guest.who = trackpoint_who(guest_who_text);
        }
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

/**
 * Find what the guest on `io` speaks, as Linux's `psmouse_probe` and
 * `psmouse_extensions` find it on a pass-through port: its ID, which must
 * be a mouse's, and the reset to the defaults, then `guest_extensions`.
 * Returns PROTO_*, or -1 when no mouse answered.
 */
static int guest_detect(const struct ps2io *io, int set_properties)
{
    uint8_t id = 0xa5;

    if (ps2io_command(io, MOUSE_CMD_GET_ID, 0, 0, &id, 1) < 0 ||
        (id != 0x00 && id != 0x03 && id != 0x04 && id != 0xff)) {
        return -1;
    }
    (void)io_cmd(io, MOUSE_CMD_SET_DEFAULTS);
    return guest_extensions(io, set_properties);
}

/** Detect the protocol `type` on the guest's line, as `try_protocol` does
 * on the auxiliary port for the protocols Rach2 can detect through a
 * touchpad. Returns 0 when the guest speaks it, -1 when not. */
static int guest_try_protocol(int type)
{
    static const uint8_t imps[3] = {200, 100, 80};
    static const uint8_t imex[3] = {200, 200, 80};
    static const uint8_t hwheel[3] = {200, 80, 40};
    const struct ps2io *io = guest.io;
    int i;

    switch (type) {
    case PROTO_PS2:
        return 0;
    case PROTO_THINKPS:
        return thinking_detect(io, &guest.who);
    case PROTO_GENPS:
        return genius_detect(io, &guest.who);
    case PROTO_IMPS:
        return io_knock(io, imps) == ID_IMPS ? 0 : -1;
    case PROTO_IMEX:
        (void)io_knock(io, imps);
        if (io_knock(io, imex) != ID_IMEX) {
            return -1;
        }
        for (i = 0; i < 3; i++) {
            (void)io_cmd_arg(io, MOUSE_CMD_SET_RATE, hwheel[i]);
        }
        return 0;
    case PROTO_TRACKPOINT:
        if (trackpoint_detect(io) < 0) {
            return -1;
        }
        guest.who = trackpoint_who(guest_who_text);
        return 0;
    case PROTO_CORTRON:
        guest.who = "Cortron PS/2 Trackball";
        return 0;
    default:
        return -1;
    }
}

/** The packet size of the mouse protocol `type`: four bytes with a wheel,
 * three without. */
static int mouse_packet_size(int type)
{
    return type == PROTO_IMPS || type == PROTO_IMEX || type == PROTO_GENPS ? 4 : 3;
}

/** Linux's `psmouse_initialize` on the guest: its rate, resolution and 1:1
 * scaling, each refusal ignored. */
static void guest_initialize(const struct ps2io *io)
{
    guest.set.rate = io_set_rate(io, guest.set.rate);
    guest.set.resolution = io_set_resolution(io, guest.set.resolution);
    (void)io_cmd(io, MOUSE_CMD_SET_SCALE11);
}

/** Register the guest's event device as `register_device` names it, at
 * "synaptics-pt/serio0/input0", with serio2's files: psmouse's, and a
 * TrackPoint's settings. */
static void guest_register(void)
{
    guest.who = who_default(guest.who, guest.protocol);
    guest.dev = register_device(guest_devname, GUEST_PHYS, guest.protocol, guest.who);
    evdev_sysfs_serio_driver(EVDEV_SERIO_PASS_THROUGH, guest_files);
    evdev_sysfs_serio_files(EVDEV_SERIO_PASS_THROUGH,
                            guest.protocol == PROTO_TRACKPOINT ? trackpoint_sysfs_add : NULL);
}

/** Take the guest's event device away, as Linux's `psmouse_disconnect`
 * does before a port is probed anew. */
static void guest_forget(void)
{
    guest.active = 0;
    guest.resyncing = 0;
    if (guest.dev) {
        evdev_unregister(guest.dev);
        guest.dev = NULL;
    }
    evdev_sysfs_serio_remove(EVDEV_SERIO_PASS_THROUGH);
    guest.extra = 0;
    guest.side = 0;
}

/** Linux's `psmouse_switch_protocol`'s rule for `resync_time` on the
 * guest: zeroed for a packet of three bytes, and for a guest that does not
 * answer a poll. */
static void guest_check_resync(void)
{
    uint8_t buf[4];

    if (guest.packet_size == 3 ||
        (guest.set.resync_time &&
         ps2io_command(guest.io, MOUSE_CMD_POLL, 0, 0, buf, guest.packet_size) < 0)) {
        guest.set.resync_time = 0;
    }
}

int ps2mouse_guest_connect(const struct ps2io *io)
{
    int proto;

    guest_forget();
    (void)deactivate(&ps2mouse_aux);
    k_udelay(10000); /* Linux lets the device behind the port settle */
    guest.who = NULL;
    proto = guest_detect(io, 1);
    if (proto < 0) {
        return 0;
    }
    guest.io = io;
    guest.protocol = proto;
    guest.packet_size = mouse_packet_size(proto);
    guest.set = defaults;
    guest.set.resync_time = 0; /* Linux's for a pass-through port */
    guest.num_resyncs = 0;
    guest.acks_disable = 0;
    guest_check_resync();
    guest_initialize(io);
    guest_register();
    synaptics_pt_activate(guest.packet_size);
    if (io_cmd(io, MOUSE_CMD_ENABLE) < 0) {
        klog(KLOG_WARN, "mouse", "pass-through: the guest did not enable reporting");
        return 1;
    }
    guest.active = 1;
    guest.last_tick = (uint32_t)kernel_ticks;
    klog(KLOG_INFO, "mouse", "pass-through: found: protocol %d", proto);
    return 1;
}

/** Wake kpsmoused to do `what`, RECONNECT_* or RESYNC_*. Any context. */
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
    guest.resyncing = 0;
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
    guest.last_tick = (uint32_t)kernel_ticks;
    return 0;
}

/** Whether `elapsed` ticks are more than `seconds`, as Linux's
 * `time_after(jiffies, last + seconds * HZ)` asks, without the product's
 * overflow. */
static int idle_for(uint32_t elapsed, unsigned int seconds)
{
    uint32_t whole = elapsed / TICK_HZ;

    return whole > seconds || (whole == seconds && elapsed % TICK_HZ != 0);
}

void ps2mouse_guest_packet(const uint8_t *bytes)
{
    uint32_t now = (uint32_t)kernel_ticks;

    if (!guest.active || guest.resyncing) {
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
    if (guest.set.resync_time && idle_for(now - guest.last_tick, guest.set.resync_time)) {
        guest.resyncing = 1;
        klog(KLOG_INFO, "mouse", "pass-through: sync: idle for %u s: resyncing",
             guest.set.resync_time);
        request_reconnect(RESYNC_GUEST);
        return;
    }
    guest.last_tick = now;
    report(guest.dev, guest.protocol, bytes, guest.extra, &guest.side);
}

int ps2mouse_guest_packet_size(void)
{
    return guest.dev ? guest.packet_size : 0;
}

void ps2mouse_guest_buttons(uint8_t buttons)
{
    if (!guest.dev || (buttons & 0x07u) == guest.extra) {
        return; /* Linux's input core passes on no unchanged key */
    }
    mouse_report(guest.dev, (buttons & 0x07u) | guest.side, 0, 0, 0, 0);
    guest.extra = (uint8_t)(buttons & 0x07u);
}

/**
 * Count a bad packet of the device, when `bad` is 1, or a whole one, as
 * Linux's `psmouse_handle_byte` counts them: the port's `resetafter`th bad
 * packet in a row, an ALPS pad's bad bytes, puts the port in STATE_IGNORE
 * and asks for a reconnect, and a whole packet ends the count. A
 * `resetafter` of 0 never reconnects.
 */
static void count_packet(int bad)
{
    unsigned int limit = aux.resetafter;

    if (bad) {
        if (++bad_in_a_row == limit) {
            state = STATE_IGNORE;
            if (alps > 0) {
                klog(KLOG_WARN, "mouse", "sync: %u bad bytes in a row: reconnecting", limit);
            } else {
                klog(KLOG_WARN, "mouse", "sync: %u bad packets in a row: reconnecting", limit);
            }
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
 * Take one byte of the device's packet, Linux's `protocol_handler`: a
 * touchpad's to synaptics.c, elantech.c or alps.c, a mouse's into the
 * packet being assembled, which is reported once whole. A mouse's first
 * byte without its framing bit, bit 3, is dropped, which Linux does not
 * check, but for a ThinkingMouse's and a Cortron trackball's, whose bit 3
 * is a button. Returns PS_*.
 */
static int handle_byte(uint8_t data)
{
    int rc;

    if (touchpad == TOUCHPAD_SYNAPTICS) {
        rc = synaptics_byte(data);
        return rc == SYNAPTICS_BAD ? PS_BAD : rc == SYNAPTICS_GOOD ? PS_GOOD : PS_FULL;
    }
    if (touchpad == TOUCHPAD_ELANTECH) {
        rc = elantech_byte(data);
        return rc == ELANTECH_BAD ? PS_BAD : rc == ELANTECH_GOOD ? PS_GOOD : PS_FULL;
    }
    if (alps > 0) {
        rc = alps_byte(data);
        return rc == ALPS_BAD ? PS_BAD : rc == ALPS_GOOD ? PS_GOOD : PS_FULL;
    }
    if (packet_index == 0 && !(data & 0x08) && protocol != PROTO_THINKPS &&
        protocol != PROTO_CORTRON) {
        return PS_BAD;
    }
    packet_bytes[packet_index++] = data;
    if (packet_index < packet_size) {
        return PS_GOOD;
    }
    report_packet();
    packet_index = 0;
    return PS_FULL;
}

/** Take one byte as `handle_byte` does, and count the packet it ended,
 * bad or whole. */
static void take_byte(uint8_t data)
{
    int rc = handle_byte(data);

    if (rc != PS_GOOD) {
        count_packet(rc == PS_BAD);
    }
}

/** Put the port in STATE_RESYNC, whose bytes are dropped, and ask
 * kpsmoused for the resync. In interrupt context. */
static void begin_resync(void)
{
    state = STATE_RESYNC;
    request_reconnect(RESYNC_PORT);
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
 * A mouse's packet is framed by byte 0, whose bit 3 is always 1: a byte
 * expected as byte 0 with that bit clear is out of step with the stream and
 * is dropped, which brings the stream back in step, and counts as a bad
 * packet. A byte of any packet more than half a second after the one
 * before, and the first byte of any packet after the device was idle for
 * `resync_time` seconds, is dropped with the packet and begins a resync, as
 * Linux's `psmouse_receive_byte` does.
 *
 * During a run of commands the handler reads nothing, and after the bad
 * packets `resetafter` allows, or while a resync is due, it drops every
 * byte until kpsmoused has reconnected or resynced the device.
 */
static void mouse_irq_handler(isr_params_t *params)
{
    uint8_t status;
    uint8_t data;
    uint32_t now;

    (void)params;
    if (state == STATE_CMD) {
        return; /* the run of commands reads it */
    }
    status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL) || !(status & PS2_STATUS_MOUSE_DATA)) {
        return;
    }
    data = inb(PS2_DATA_PORT);
    if (state == STATE_IGNORE || state == STATE_RESYNC) {
        return;
    }
    now = (uint32_t)kernel_ticks;
    if ((bat_held || !packet_start()) && now - last_tick > MS_TO_TICKS(500)) {
        klog(KLOG_INFO, "mouse", "sync: lost: a pause inside a packet: resyncing");
        badbyte = packet_first;
        begin_resync();
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
        packet_first = data;
        last_tick = now;
        return;
    }
    if (packet_start() && aux.resync_time && idle_for(now - last_tick, aux.resync_time)) {
        klog(KLOG_INFO, "mouse", "sync: idle for %u s: resyncing", aux.resync_time);
        badbyte = data;
        begin_resync();
        return;
    }
    if (packet_start()) {
        packet_first = data;
    }
    last_tick = now;
    take_byte(data);
}

/**
 * Find the device on the port and set it up, as Linux's `psmouse_connect`
 * does: the port's settings Linux's defaults; the reset and the defaults, a
 * refusal of either tolerated, since some emulators answer neither; the
 * protocol `probe_protocol` finds; the event devices and serio1's files;
 * and the guest of a Synaptics pad's pass-through port. Its reporting is
 * left for the caller to enable.
 */
static void connect_device(void)
{
    aux = defaults;
    aux_who = NULL;
    num_resyncs = 0;
    acks_disable = 0;
    if (mouse_reset() < 0) {
        klog(KLOG_WARN, "mouse", "the device did not answer its reset");
    }
    if (mouse_send_command(MOUSE_CMD_SET_DEFAULTS) < 0) {
        klog(KLOG_WARN, "mouse", "the device refused the default settings");
    }
    set_up_protocol(PROTO_AUTO);
    register_mouse();
    bind_files();
    if (touchpad == TOUCHPAD_SYNAPTICS && synaptics_has_pass_through()) {
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

    evdev_sysfs_serio_driver(1, aux_files);
    booting = 1;
    connect_device();
    booting = 0;

    response = mouse_send_command(MOUSE_CMD_ENABLE);
    if (response < 0) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        arch_irq_enable();
        return -1;
    }

    last_tick = (uint32_t)kernel_ticks;
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

/** Whether no run of commands is in progress. */
static int cmd_free(void *arg)
{
    (void)arg;
    return !cmd_busy;
}

#ifndef RACH2_HOSTTEST
/** Whether the tick in `*arg` has come. */
static int tick_passed(void *arg)
{
    return tick_count() >= *(const uint64_t *)arg;
}

/** Whether a reconnect or a resync is wanted. */
static int wanted(void *arg)
{
    (void)arg;
    return reconnect_wanted;
}

#endif

/**
 * Drop the packet under way and the count of bad packets, and start the
 * idle time again, as Linux's `__psmouse_set_state` does at each change of
 * the port's state, so that the bytes after a run of commands start a
 * packet: the run's own replies and the packet it cut short are not the
 * device's next packet. With interrupts off.
 */
static void restart_packet(void)
{
    packet_index = 0;
    bat_held = 0;
    bad_in_a_row = 0;
    last_tick = (uint32_t)kernel_ticks;
    if (touchpad == TOUCHPAD_SYNAPTICS) {
        synaptics_packet_restart();
    } else if (touchpad == TOUCHPAD_ELANTECH) {
        elantech_packet_restart();
    } else if (alps > 0) {
        alps_packet_restart();
    }
}

/** Begin a run of commands as `ps2mouse_cmd_begin` does, and return the
 * port's state before it, STATE_*. */
static int cmd_begin_state(void)
{
    uint32_t flags;
    int was;

    while (cmd_busy) {
        (void)wq_wait_event_nointr(&cmd_wq, cmd_free, NULL);
    }
    flags = kirq_save();
    was = state;
    cmd_busy = 1;
    state = STATE_CMD;
    restart_packet();
    kirq_restore(flags);
    return was;
}

void ps2mouse_cmd_begin(void)
{
    (void)cmd_begin_state();
}

/** End a run of commands as `ps2mouse_cmd_end` does, leaving the port in
 * the state `to`, STATE_*: STATE_ACTIVE, or a state a reconnect or a resync
 * is due in, whose bytes the handler drops. */
static void cmd_end_state(int to)
{
    uint32_t flags;
    uint8_t status;

    flags = kirq_save();
    state = to;
    restart_packet();
    cmd_busy = 0;
    status = inb(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OUTPUT_FULL) && (status & PS2_STATUS_MOUSE_DATA)) {
        mouse_irq_handler(NULL);
    }
    kirq_restore(flags);
    wq_wake_all(&cmd_wq);
}

void ps2mouse_cmd_end(void)
{
    cmd_end_state(STATE_ACTIVE);
}

/** Whether the port's own device is a TrackPoint, whose settings a change
 * writes in place of a guest's. */
static int port_trackpoint(void)
{
    return protocol == PROTO_TRACKPOINT && mouse_dev;
}

const struct ps2io *ps2mouse_guest_change_begin(void)
{
    ps2mouse_cmd_begin();
    if (port_trackpoint()) {
        (void)deactivate(&ps2mouse_aux);
        return &ps2mouse_aux;
    }
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
    if (!port_trackpoint() && io_cmd(guest.io, MOUSE_CMD_ENABLE) < 0) {
        klog(KLOG_WARN, "mouse", "pass-through: the guest did not enable reporting");
    }
    if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
    }
    ps2mouse_cmd_end();
}

/**
 * Begin a change through a file of the port `port`, ON_AUX or ON_GUEST, as
 * Linux's `psmouse_attr_set_helper` begins one: a run of commands, then the
 * touchpad disabled, and for the guest the guest after it. Returns 0, or
 * -ENODEV, having begun nothing, while the port waits for its reconnect or
 * the guest does not report.
 */
static int port_change_begin(int port)
{
    int was = cmd_begin_state();

    if (was == STATE_IGNORE || (port == ON_GUEST && (!guest.dev || !guest.active))) {
        cmd_end_state(was);
        return -ENODEV;
    }
    (void)deactivate(&ps2mouse_aux);
    if (port == ON_GUEST) {
        (void)deactivate(guest.io);
    }
    return 0;
}

/** End the change `port_change_begin` began, whose result is `rc`: the
 * port's device enabled again unless the change found it gone (-ENODEV),
 * and the touchpad after the guest; a resync due is no longer. */
static void port_change_end(int port, int rc)
{
    if (port == ON_GUEST) {
        if (rc != -ENODEV && guest.dev && io_cmd(guest.io, MOUSE_CMD_ENABLE) < 0) {
            klog(KLOG_WARN, "mouse", "pass-through: the guest did not enable reporting");
        }
        guest.resyncing = 0;
        guest.last_tick = (uint32_t)kernel_ticks;
    }
    if ((port == ON_GUEST || rc != -ENODEV) && mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
    }
    cmd_end_state(STATE_ACTIVE);
}

int ps2mouse_change_begin(void)
{
    return port_change_begin(ON_AUX);
}

void ps2mouse_change_end(int rc)
{
    port_change_end(ON_AUX, rc);
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
 * Set the mouse up again as Linux's `psmouse_reconnect` does: a
 * TrackPoint by `trackpoint_reconnect`, and a protocol that has no
 * reconnect of its own by the reset and the defaults, and its protocol
 * found again, which must be the one found before; then the port's rate,
 * resolution and scaling. Returns 0 when it is, and the mouse keeps its
 * event device; -1 when the mouse does not answer its reset, speaks
 * another protocol now or is a touchpad.
 */
static int mouse_reconnect(void)
{
    int old = protocol;

    if (protocol == PROTO_TRACKPOINT) {
        if (trackpoint_reconnect(&ps2mouse_aux) < 0) {
            return -1;
        }
        ps2mouse_initialize(NULL);
        return 0;
    }
    if (mouse_reset() < 0) {
        return -1;
    }
    if (mouse_send_command(MOUSE_CMD_SET_DEFAULTS) < 0) {
        klog(KLOG_WARN, "mouse", "the device refused the default settings");
    }
    if (thinking_detect(&ps2mouse_aux, &aux_who) == 0) {
        protocol = PROTO_THINKPS;
        ps2mouse_initialize(NULL);
    } else {
        detect_protocol();
    }
    return protocol == old && touchpad == 0 && alps <= 0 ? 0 : -1;
}

/**
 * Take the port's protocol away, as Linux's psmouse does before a port is
 * probed anew or switched to another protocol: the guest, the touchpad's
 * event devices, and the mouse's event device, which the caller is given
 * to remove once the next one is registered, or NULL.
 */
static struct evdev *disconnect_protocol(void)
{
    struct evdev *old = mouse_dev;

    guest_forget();
    synaptics_disconnect();
    if (touchpad == TOUCHPAD_ELANTECH) {
        elantech_disconnect();
    }
    if (alps > 0) {
        alps_disconnect();
    }
    mouse_dev = NULL;
    packet_index = 0;
    return old;
}

void ps2mouse_reconnect(void)
{
    struct evdev *old;
    int what;

    ps2mouse_cmd_begin();
    what = reconnect_wanted & (RECONNECT_PORT | RECONNECT_GUEST);
    reconnect_wanted &= ~what;
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
    } else if (touchpad == 0 && alps <= 0 && mouse_dev && mouse_reconnect() == 0) {
        packet_index = 0;
        if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
            klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        }
        klog(KLOG_INFO, "mouse", "reconnect: the mouse reports again");
    } else {
        klog(KLOG_WARN, "mouse", "reconnect: probing the port anew");
        old = disconnect_protocol();
        if (old) {
            evdev_unregister(old);
        }
        connect_device();
        if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
            klog(KLOG_WARN, "mouse", "the device did not enable reporting");
        }
    }
    bad_in_a_row = 0;
    ps2mouse_cmd_end();
}

/**
 * Resync the port's device, as Linux's `psmouse_resync` does, inside a run
 * of commands: its disable, whose missing ACK fails the resync after two
 * resyncs or once the device has acknowledged one; a poll, whose packet
 * must be whole and of the protocol's form, and is reported; then its
 * reporting, tried five times 200 ms apart. Returns 0, or -1 when the
 * device is to be reconnected.
 */
static int resync_port(void)
{
    uint8_t buf[8];
    int failed = 0, enabled = 0, rc = PS_GOOD, i, n;

    num_resyncs++;
    if (deactivate(&ps2mouse_aux) < 0) {
        failed = num_resyncs < 3 || acks_disable;
    } else {
        acks_disable = 1;
    }
    if (!failed) {
        n = port_poll(buf);
        for (i = 0; i < n; i++) {
            rc = handle_byte(buf[i]);
            if (rc != PS_GOOD) {
                break;
            }
        }
        failed = n < 0 || rc != PS_FULL;
    }
    for (i = 0; i < 5; i++) {
        if (mouse_send_command(MOUSE_CMD_ENABLE) == MOUSE_ACK) {
            enabled = 1;
            break;
        }
        ps2mouse_msleep(200);
    }
    if (!enabled) {
        klog(KLOG_WARN, "mouse", "resync: the device did not enable reporting");
        failed = 1;
    }
    return failed ? -1 : 0;
}

/**
 * Resync the guest as `resync_port` resyncs the port's device, with the
 * touchpad disabled around it, as Linux's `psmouse_resync` does for a
 * pass-through port. Returns 0, or -1 when the guest is to be reconnected.
 */
static int resync_guest(void)
{
    uint8_t buf[4];
    int failed = 0, enabled = 0, i;

    guest.resyncing = 0;
    (void)deactivate(&ps2mouse_aux);
    guest.num_resyncs++;
    if (deactivate(guest.io) < 0) {
        failed = guest.num_resyncs < 3 || guest.acks_disable;
    } else {
        guest.acks_disable = 1;
    }
    buf[3] = 0;
    if (!failed && ps2io_command(guest.io, MOUSE_CMD_POLL, 0, 0, buf, guest.packet_size) < 0) {
        failed = 1;
    } else if (!failed) {
        report(guest.dev, guest.protocol, buf, guest.extra, &guest.side);
    }
    for (i = 0; i < 5; i++) {
        if (io_cmd(guest.io, MOUSE_CMD_ENABLE) == 0) {
            enabled = 1;
            break;
        }
        ps2mouse_msleep(200);
    }
    if (!enabled) {
        klog(KLOG_WARN, "mouse", "pass-through: resync: the guest did not enable reporting");
        failed = 1;
    }
    if (mouse_send_command(MOUSE_CMD_ENABLE) != MOUSE_ACK) {
        klog(KLOG_WARN, "mouse", "the device did not enable reporting");
    }
    guest.last_tick = (uint32_t)kernel_ticks;
    return failed ? -1 : 0;
}

void ps2mouse_resync(void)
{
    int was = cmd_begin_state(), what, to = STATE_ACTIVE;

    what = reconnect_wanted & (RESYNC_PORT | RESYNC_GUEST);
    reconnect_wanted &= ~what;
    if (was == STATE_IGNORE) {
        cmd_end_state(STATE_IGNORE); /* the reconnect due does it all */
        return;
    }
    if ((what & RESYNC_PORT) && was == STATE_RESYNC && resync_port() < 0) {
        klog(KLOG_INFO, "mouse", "resync: failed: reconnecting");
        to = STATE_IGNORE;
        request_reconnect(RECONNECT_PORT);
    }
    if (to == STATE_ACTIVE && (what & RESYNC_GUEST) && guest.resyncing && guest.dev &&
        resync_guest() < 0) {
        klog(KLOG_INFO, "mouse", "pass-through: resync: failed: reconnecting");
        guest.active = 0;
        request_reconnect(RECONNECT_GUEST);
    }
    cmd_end_state(to);
}

/**
 * Switch the auxiliary port to the protocol named by the `len` bytes at
 * `buf`, as Linux's `psmouse_attr_set_protocol` does, inside a change: the
 * protocol taken away, then `set_up_protocol`, its event device registered
 * before the old one is removed, serio1's files, and a Synaptics pad's
 * guest. A replacement slot is reserved before any device is removed.
 * Returns 0, -EINVAL for a name `proto_lookup` refuses, or -ENOMEM with
 * the old protocol and devices intact when no slot is free.
 */
static int aux_set_protocol(const char *buf, uint32_t len)
{
    int type = proto_lookup(buf, len);
    struct evdev *old;

    if (type < 0) {
        return type;
    }
    if (type == protocol) {
        return 0;
    }
    replacement = evdev_reserve();
    if (!replacement) {
        return -ENOMEM;
    }
    old = disconnect_protocol();
    set_up_protocol(type);
    register_mouse();
    evdev_unregister(replacement);
    replacement = NULL;
    bind_files();
    if (old) {
        evdev_unregister(old);
    }
    if (touchpad == TOUCHPAD_SYNAPTICS && synaptics_has_pass_through()) {
        (void)ps2mouse_guest_connect(&synaptics_pt);
    }
    klog(KLOG_INFO, "mouse", "protocol: now %s", proto_name(protocol));
    return 0;
}

/**
 * Switch the guest to the protocol named by the `len` bytes at `buf`, as
 * `aux_set_protocol` switches the port, with the protocols Rach2 can
 * detect through a touchpad: the guest's new event device registered before
 * the old one is removed, then the touchpad's mode for its packet size.
 * Reserve the replacement first; return -ENOMEM with the guest unchanged
 * when no slot is free. Otherwise return 0, or -EINVAL for a bad name.
 */
static int guest_set_protocol(const char *buf, uint32_t len)
{
    int type = proto_lookup(buf, len);
    struct evdev *old;

    if (type < 0) {
        return type;
    }
    if (type == guest.protocol) {
        return 0;
    }
    replacement = evdev_reserve();
    if (!replacement) {
        return -ENOMEM;
    }
    old = guest.dev;
    guest.dev = NULL;
    guest.side = 0;
    guest.extra = 0;
    if (type == PROTO_AUTO) {
        type = guest_extensions(guest.io, 1);
    } else if (guest_try_protocol(type) < 0) {
        (void)io_reset(guest.io);
        type = PROTO_PS2;
    }
    guest.protocol = type;
    guest.packet_size = mouse_packet_size(type);
    guest_check_resync();
    guest_initialize(guest.io);
    guest_register();
    if (old) {
        evdev_unregister(old);
    }
    synaptics_pt_activate(guest.packet_size);
    klog(KLOG_INFO, "mouse", "pass-through: protocol: now %s", proto_name(type));
    return 0;
}

int ps2mouse_parse_uint(const char *buf, uint32_t len, unsigned int base, uint32_t max,
                        uint32_t *out)
{
    uint32_t i = 0, v = 0, d;
    int digits = 0, range = 0;

    if (i < len && buf[i] == '+') {
        i++;
    }
    if (base == 16 && i + 1 < len && buf[i] == '0' && (buf[i + 1] == 'x' || buf[i + 1] == 'X')) {
        i += 2;
    }
    for (; i < len; i++, digits++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            d = (uint32_t)(buf[i] - '0');
        } else if (base == 16 && buf[i] >= 'a' && buf[i] <= 'f') {
            d = (uint32_t)(buf[i] - 'a' + 10);
        } else if (base == 16 && buf[i] >= 'A' && buf[i] <= 'F') {
            d = (uint32_t)(buf[i] - 'A' + 10);
        } else {
            break;
        }
        if (v > (max - d) / base) {
            range = 1;
        } else {
            v = v * base + d;
        }
    }
    if (digits == 0) {
        return -EINVAL;
    }
    if (i < len && buf[i] == '\n') {
        i++;
    }
    if (i < len) {
        return -EINVAL;
    }
    if (range) {
        return -ERANGE;
    }
    *out = v;
    return 0;
}

/* The files of a port, which `arg` gives as the port, ON_AUX or ON_GUEST,
 * times 8 plus the file's place here. */
#define FILE_PROTOCOL    0
#define FILE_RATE        1
#define FILE_RESOLUTION  2
#define FILE_RESETAFTER  3
#define FILE_RESYNC_TIME 4
#define PORT_FILES       5

static const char *const port_files[PORT_FILES] = {"protocol", "rate", "resolution", "resetafter",
                                                   "resync_time"};

/** Write `text` and a newline into `buf`, of `cap` bytes, as much as fits;
 * return the length of the whole. */
static int32_t show_line(char *buf, uint32_t cap, const char *text)
{
    uint32_t n = 0;

    for (; text[n]; n++) {
        if (n < cap) {
            buf[n] = text[n];
        }
    }
    if (n < cap) {
        buf[n] = '\n';
    }
    return (int32_t)(n + 1);
}

/** A file of a port, as Linux's `psmouse_attr_show_protocol` and
 * `psmouse_show_int_attr` write it: the protocol's name, or the setting in
 * decimal; -ENODEV for the guest's when there is none. */
static int32_t show_port(void *obj, int arg, char *buf, uint32_t cap)
{
    const struct port_settings *set = (arg >> 3) == ON_GUEST ? &guest.set : &aux;
    char text[12];
    unsigned int v;
    int n = (int)sizeof(text) - 1;

    (void)obj;
    if ((arg >> 3) == ON_GUEST && !guest.dev) {
        return -ENODEV;
    }
    switch (arg & 7) {
    case FILE_PROTOCOL:
        return show_line(buf, cap, proto_name((arg >> 3) == ON_GUEST ? guest.protocol : protocol));
    case FILE_RATE:
        v = set->rate;
        break;
    case FILE_RESOLUTION:
        v = set->resolution;
        break;
    case FILE_RESETAFTER:
        v = set->resetafter;
        break;
    default:
        v = set->resync_time;
        break;
    }
    text[n] = 0;
    do {
        text[--n] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    return show_line(buf, cap, text + n);
}

/**
 * A write to a file of a port, inside Linux's `psmouse_attr_set_helper`'s
 * change: a protocol's name, switched to; or a decimal number, as Linux's
 * `kstrtouint` reads it, set as the rate or resolution through the
 * protocol's own `set_rate` and `set_resolution`, which keep what they
 * sent, or kept as `resetafter` or `resync_time`. Returns `len`, -ENODEV
 * while the port waits for its reconnect or the guest does not report,
 * -EINVAL for a protocol or number Linux refuses, or -ERANGE.
 */
static int32_t store_port(void *obj, int arg, const char *buf, uint32_t len)
{
    int port = arg >> 3, rc;
    struct port_settings *set = port == ON_GUEST ? &guest.set : &aux;
    uint32_t n, v = 0;

    (void)obj;
    for (n = 0; n < len && buf[n]; n++) { /* Linux's store reads a string */
    }
    rc = port_change_begin(port);
    if (rc < 0) {
        return rc;
    }
    if ((arg & 7) == FILE_PROTOCOL) {
        rc = port == ON_GUEST ? guest_set_protocol(buf, len) : aux_set_protocol(buf, len);
    } else {
        rc = ps2mouse_parse_uint(buf, n, 10, 0xFFFFFFFFu, &v);
    }
    if (rc == 0) {
        switch (arg & 7) {
        case FILE_RATE:
            set->rate = port == ON_GUEST ? io_set_rate(guest.io, v) : aux_set_rate(v);
            break;
        case FILE_RESOLUTION:
            set->resolution = io_set_resolution(port == ON_GUEST ? guest.io : &ps2mouse_aux, v);
            break;
        case FILE_RESETAFTER:
            set->resetafter = v;
            break;
        case FILE_RESYNC_TIME:
            set->resync_time = v;
            break;
        default:
            break;
        }
    }
    port_change_end(port, rc);
    return rc < 0 ? rc : (int32_t)len;
}

/** Add the files of the port `port` below its directory `dir`. */
static int add_port_files(int dir, int port)
{
    struct sysfs_attr attr;
    int i, h;

    for (i = 0; i < PORT_FILES; i++) {
        attr.name = port_files[i];
        attr.mode = 0644u;
        attr.show = show_port;
        attr.store = store_port;
        h = sysfs_add_attr(dir, &attr, 0, port * 8 + i);
        if (h < 0) {
            return h;
        }
    }
    return 0;
}

/** The auxiliary port's files, which evdev_sysfs.c adds to serio1. */
static int aux_files(int dir)
{
    return add_port_files(dir, ON_AUX);
}

/** The pass-through port's files, which evdev_sysfs.c adds to serio2. */
static int guest_files(int dir)
{
    return add_port_files(dir, ON_GUEST);
}

void ps2mouse_msleep(unsigned int ms)
{
#ifndef RACH2_HOSTTEST
    uint64_t due;
#endif

    if (booting) {
        /* The boot's process cannot sleep; IRQ 12 has no handler yet and
         * stays unrouted, so the ticks may run while it waits. */
        arch_irq_enable();
        k_udelay(ms * 1000u);
        arch_irq_disable();
        return;
    }
#ifdef RACH2_HOSTTEST
    kernel_ticks += MS_TO_TICKS(ms);
#else
    due = tick_count() + MS_TO_TICKS(ms);
    current_proc->wakeup_tick = due;
    (void)wq_wait_event_idle(&sleep_wq, tick_passed, &due);
    current_proc->wakeup_tick = 0;
#endif
}

#ifndef RACH2_HOSTTEST
/** The worker, kpsmoused: sleep until the handler asks for a reconnect or
 * a resync, then do it, holding the big lock as kernel code does; a
 * reconnect first, which makes a resync due before it needless. */
static void psmoused(void)
{
    arch_irq_enable();
    for (;;) {
        (void)wq_wait_event_idle(&reconnect_wq, wanted, NULL);
        if (reconnect_wanted & (RECONNECT_PORT | RECONNECT_GUEST)) {
            ps2mouse_reconnect();
        } else {
            ps2mouse_resync();
        }
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
