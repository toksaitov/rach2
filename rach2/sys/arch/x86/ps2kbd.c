/*
 * ps2kbd.c: the PS/2 keyboard's interrupt, which reads the 8042's data port
 * and hands the byte on, the command that lights the keyboard's LEDs, and
 * the reconnect after the keyboard reset itself, as Linux's atkbd does.
 * Everything that is not about the 8042 is in the input layer, which takes a
 * USB keyboard's keys too and treats them the same.
 */

#include "../../dmi.h"
#include "../../idle_timer.h"
#include "i8042.h"
#include "isr.h"
#include "pic.h"
#include "portio.h"
#include "ps2kbd.h"

#include "../../evdev.h"
#include "../../input.h"
#include "../../klog.h"
#include "../../lib/kirq.h"
#include "../../lib/kstring.h"
#include "../../mm/paging.h"
#include "../../timer.h"

/* The keyboard's command that sets its LEDs, followed by a byte of them:
 * bit 0 Scroll Lock, bit 1 Num Lock, bit 2 Caps Lock, as kd.h's LED_*. The
 * keyboard answers each byte with ACK, or with RESEND when it did not take
 * it. */
#define KBD_CMD_SET_LEDS 0xED
#define KBD_ACK          0xFA
#define KBD_RESEND       0xFE

/* The time from the end of one LED command to the start of the next, and
 * how long a byte's answer may take, in ticks; how many times a byte is
 * written before the command is given up. */
#define LEDS_GAP_TICKS  MS_TO_TICKS(50)
#define LEDS_WAIT_TICKS MS_TO_TICKS(100)
#define LEDS_TRIES      3

/* How long the controller may refuse a byte, its input buffer full, before
 * the byte fails, in ticks: Linux's i8042_wait_write gives up after 10000
 * waits of 50 us, and ps2_do_sendbyte then fails the byte. A byte refused
 * is not a try. */
#define WRITE_WAIT_TICKS MS_TO_TICKS(PS2_TIMEOUT_US / 1000u)

/* Where the LED command is: none in progress, the command byte due or out,
 * the LED byte due or out. */
#define LEDS_IDLE    0
#define LEDS_COMMAND 1
#define LEDS_VALUE   2

/* The commands of Linux's atkbd_probe and atkbd_activate: GETID, answered
 * by ACK and two ID bytes; RESET_DIS, which stops the keyboard's scanning;
 * ENABLE, which starts it. BAT is what a keyboard sends after its
 * self-test, at power-up or when it was plugged in again. */
#define KBD_CMD_GETID     0xF2
#define KBD_CMD_RESET_DIS 0xF5
#define KBD_CMD_ENABLE    0xF4
#define KBD_RET_BAT       0xAA

/* How long GETID's ID bytes may take after its ACK, in ticks, the 500 ms
 * of Linux's ps2_command. */
#define PROBE_ID_TICKS MS_TO_TICKS(500)

/* The keyboard's event device, named as Linux names a translated one, and
 * its keymap. */
static struct input_keyboard ps2_keyboard;
static uint16_t keymap[INPUT_SET1_KEYMAP];

/* What the probe is doing (`kbd.job`): nothing; the reconnect of the bound
 * driver after a BAT, Linux's atkbd_reconnect; or the probe of serio0 with
 * no driver, Linux's atkbd_connect. */
#define JOB_NONE      0
#define JOB_RECONNECT 1
#define JOB_CONNECT   2

/* The probe's steps (`kbd.step`), each a byte written and its ACK awaited
 * but PROBE_ID: GETID; its two ID bytes; the LED command and its byte 0,
 * which Linux's atkbd_probe tries when GETID fails; RESET_DIS; ENABLE. */
#define PROBE_GETID    0
#define PROBE_ID       1
#define PROBE_LEDS     2
#define PROBE_LEDS_OFF 3
#define PROBE_DISABLE  4
#define PROBE_ENABLE   5

/*
 * serio0's driver and its probe, which the tick and the interrupt change as
 * they change `leds`. While a job runs, every byte from the keyboard goes
 * to it, and no key reaches the input layer, as Linux's atkbd drops the
 * bytes between a BAT and its reconnect's end. A BAT that comes while an
 * LED command is under way leaves the reconnect due (`due`) until that
 * command ends, as Linux's atkbd_reconnect waits for the LED work on
 * atkbd->mutex, so that the command's late answer is not taken for the
 * probe's.
 */
static struct {
    int off;        /* the i8042's setup failed: the port is given up */
    int unbound;    /* atkbd left serio0 after a failed reconnect */
    int translated; /* the controller translates set 2 into set 1 */
    int portable;   /* the firmware calls the machine portable */
    int due;        /* a BAT came: reconnect once the LED command ends */
    int job;        /* JOB_* */
    int step;       /* PROBE_* */
    int out;        /* the step's byte is written, or its ID bytes are awaited */
    int held;       /* the controller refused the step's byte at `at` */
    int tries;      /* the step's byte has been written this many times */
    uint32_t at;    /* `kernel_ticks` at the last write, at GETID's ACK, or at
                       the first refusal of the byte while `held` */
    uint8_t id;     /* GETID's first ID byte */
    int nid;        /* the ID bytes come so far */
    int emul;       /* the bytes still to come of an E0 or E1 sequence */
    int shift;      /* the last 0x2A or 0xAA outside a sequence was a press */
} kbd;

/*
 * The LED command's state. The tick (`ps2kbd_tick`) and the interrupt
 * (`input_ps2_reply`) both change it, each on the boot processor with
 * interrupts off: the tick is that processor's, and line 1 is routed to the
 * processor that took its handler at boot.
 */
static struct {
    int ready;          /* `kbd_init` has run */
    int stage;          /* LEDS_IDLE, LEDS_COMMAND or LEDS_VALUE */
    int out;            /* the stage's byte is written and its answer awaited */
    int held;           /* the controller refused the stage's byte at `at` */
    int tries;          /* the stage's byte has been written this many times */
    int shown;          /* the LEDs the keyboard shows, -1 before the first command */
    unsigned int value; /* the LEDs being sent */
    uint32_t at;        /* `kernel_ticks` at the last write, at the end, or at
                           the first refusal of the byte while `held` */
    int said;           /* a command given up has been logged */
} leds;

/**
 * Write `byte` to the keyboard for the LED command or the probe, whose
 * state is `*out`, `*held`, `*tries` and `*at`. A byte written counts a try
 * and is out from `now`. A byte the controller cannot take now is not out
 * and counts no try: the tick writes it again, and the first refusal's
 * tick is kept in `*at`, for the caller to fail the byte after
 * WRITE_WAIT_TICKS.
 */
static void byte_write(uint8_t byte, int *out, int *held, int *tries, uint32_t *at, uint32_t now)
{
    if (ps2_kbd_write(byte) == 0) {
        (*tries)++;
        *out = 1;
        *held = 0;
        *at = now;
    } else if (!*held) {
        *held = 1;
        *at = now;
    }
}

/** Write the byte of the LED command's stage, as `byte_write` does. */
static void leds_write(uint32_t now)
{
    uint8_t byte = leds.stage == LEDS_COMMAND ? KBD_CMD_SET_LEDS : (uint8_t)(leds.value & 0x07u);

    byte_write(byte, &leds.out, &leds.held, &leds.tries, &leds.at, now);
}

/** End the LED command, the keyboard showing `value` or taken to show it. */
static void leds_end(uint32_t now)
{
    leds.shown = (int)leds.value;
    leds.stage = LEDS_IDLE;
    leds.out = 0;
    leds.held = 0;
    leds.at = now;
}

int input_ps2_reply(uint8_t code)
{
    if (leds.stage == LEDS_IDLE || !leds.out || (code != KBD_ACK && code != KBD_RESEND)) {
        return 0;
    }
    leds.out = 0;
    if (code == KBD_RESEND) {
        if (leds.tries < LEDS_TRIES) {
            leds_write((uint32_t)kernel_ticks);
        }
    } else if (leds.stage == LEDS_COMMAND) {
        leds.stage = LEDS_VALUE;
        leds.tries = 0;
        leds_write((uint32_t)kernel_ticks);
    } else {
        leds_end((uint32_t)kernel_ticks);
    }
    return 1;
}

/**
 * Whether `code` begins a keyboard's ID, Linux's ps2_is_keyboard_id: 0xAB
 * for most, 0xAC, and the bytes a Trust and an NMB SGI keyboard send, with
 * and without the controller's translation.
 */
static int keyboard_id(uint8_t code)
{
    return code == 0xAB || code == 0xAC || code == 0x2B || code == 0x5D || code == 0x60 ||
           code == 0x47;
}

/** Write the byte of the probe's step, as `byte_write` does. */
static void probe_write(uint32_t now)
{
    static const uint8_t bytes[] = {KBD_CMD_GETID, 0, KBD_CMD_SET_LEDS, 0x00, KBD_CMD_RESET_DIS,
                                    KBD_CMD_ENABLE};

    byte_write(bytes[kbd.step], &kbd.out, &kbd.held, &kbd.tries, &kbd.at, now);
}

/** Go to the probe's step `step` and write its byte. */
static void probe_goto(int step, uint32_t now)
{
    kbd.step = step;
    kbd.tries = 0;
    kbd.out = 0;
    kbd.held = 0;
    kbd.nid = 0;
    probe_write(now);
}

/**
 * Whether the firmware's chassis type is a portable machine's, Linux's
 * atkbd_is_portable_device: Portable, Laptop, Notebook, Sub-Notebook,
 * Convertible or Detachable, types 8, 9, 10, 14, 31 and 32 of SMBIOS.
 */
static int portable_chassis(void)
{
    static const char *const types[] = {"8", "9", "10", "14", "31", "32", 0};
    const char *type = dmi_field(DMI_CHASSIS_TYPE);
    int i, j;

    for (i = 0; type && types[i]; i++) {
        for (j = 0; type[j] && type[j] == types[i][j]; j++) {
        }
        if (type[j] == types[i][j]) {
            return 1;
        }
    }
    return 0;
}

/**
 * Start the job `job`, JOB_RECONNECT or JOB_CONNECT, at GETID, or at
 * RESET_DIS on a portable machine whose controller translates, as Linux's
 * atkbd_skip_getid skips GETID there, which confuses some laptops'
 * keyboards, and takes the keyboard to be there. An LED command under way
 * is dropped, and the LEDs are sent again once the keyboard answers, as
 * Linux's atkbd_reconnect restores them.
 */
static void probe_start(int job, uint32_t now)
{
    kbd.job = job;
    kbd.due = 0;
    leds.stage = LEDS_IDLE;
    leds.out = 0;
    leds.held = 0;
    leds.shown = -1;
    probe_goto(kbd.translated && kbd.portable ? PROBE_DISABLE : PROBE_GETID, now);
}

/** Start the reconnect a BAT left due once no LED command is under way. */
static void reconnect_if_due(uint32_t now)
{
    if (kbd.due && leds.stage == LEDS_IDLE) {
        probe_start(JOB_RECONNECT, now);
    }
}

/**
 * Bind atkbd to serio0: a new event device for the keyboard, with a new
 * keymap, as Linux's atkbd_connect registers one. Returns 0, or -1 when no
 * event device could be registered, every node being taken: then the
 * keyboard is detached again and serio0 left without a driver, as Linux's
 * atkbd_connect fails when input_register_device does, so that a later byte
 * in auto mode probes the port again.
 */
static int kbd_bind(void)
{
    k_memset(&ps2_keyboard, 0, sizeof(ps2_keyboard));
    input_keyboard_attach(&ps2_keyboard, "AT Translated Set 2 keyboard", "isa0060/serio0/input0",
                          EVDEV_BUS_I8042, 0x0001, 0x0001);
    if (!ps2_keyboard.dev) {
        input_keyboard_detach(&ps2_keyboard);
        evdev_sysfs_serio_unbound(0, 1);
        kbd.unbound = 1;
        return -1;
    }
    input_keyboard_set1_keymap(&ps2_keyboard, keymap);
    evdev_sysfs_serio_unbound(0, 0);
    kbd.unbound = 0;
    return 0;
}

/**
 * End the probe, which found a keyboard when `ok` is nonzero, as Linux's
 * serio core ends a reconnect or a connect: a reconnect that found one
 * keeps the event device in either `bind_mode`; one that did not unbinds
 * atkbd, its keys released and its event device gone, and in auto mode
 * probes the port anew as a connect, which binds atkbd with a new event
 * device or leaves serio0 without a driver.
 */
static void probe_end(int ok, uint32_t now)
{
    int job = kbd.job;

    kbd.job = JOB_NONE;
    kbd.out = 0;
    kbd.held = 0;
    kbd.emul = 0;
    kbd.shift = 0;
    leds.at = now - LEDS_GAP_TICKS;
    if (job == JOB_RECONNECT && ok) {
        klog(KLOG_INFO, "kbd", "reconnect: the keyboard answers again");
        return;
    }
    if (job == JOB_CONNECT) {
        if (ok && kbd_bind() == 0) {
            klog(KLOG_INFO, "kbd", "connect: found a keyboard: bound");
        } else if (ok) {
            klog(KLOG_WARN, "kbd", "connect: no event device is free: unbound");
        } else {
            klog(KLOG_WARN, "kbd", "connect: no keyboard answers: unbound");
        }
        return;
    }
    input_keyboard_detach(&ps2_keyboard);
    evdev_sysfs_serio_unbound(0, 1);
    kbd.unbound = 1;
    if (evdev_sysfs_serio_manual(0)) {
        klog(KLOG_WARN, "kbd", "reconnect: failed: serio0's bind mode is manual: unbound");
        return;
    }
    klog(KLOG_WARN, "kbd", "reconnect: failed: probing the port anew");
    probe_start(JOB_CONNECT, now);
}

/**
 * The step's byte got no ACK in its tries, or GETID's ID bytes did not
 * come: as Linux's atkbd_probe, a GETID that failed is followed by the LED
 * command, whose failure fails the probe; RESET_DIS's failure is ignored,
 * and ENABLE's is logged and ignored, as atkbd_activate's is.
 */
static void probe_failed(uint32_t now)
{
    switch (kbd.step) {
    case PROBE_GETID:
    case PROBE_ID:
        probe_goto(PROBE_LEDS, now);
        break;
    case PROBE_DISABLE:
        probe_goto(PROBE_ENABLE, now);
        break;
    case PROBE_ENABLE:
        klog(KLOG_WARN, "kbd", "probe: the keyboard did not enable scanning");
        probe_end(1, now);
        break;
    default:
        probe_end(0, now);
        break;
    }
}

/**
 * Take a byte from the keyboard while the probe runs: the ACK or RESEND of
 * the step's byte, or an ID byte of GETID. An ID that is no keyboard's, or
 * the NCD keyboard's 0xACA1, which Linux takes only without translation,
 * fails the probe. Any other byte is dropped.
 */
static void probe_reply(uint8_t code, uint32_t now)
{
    if (!kbd.out) {
        return;
    }
    if (kbd.step == PROBE_ID) {
        if (kbd.nid == 0) {
            kbd.id = code;
            kbd.nid = 1;
            if (!keyboard_id(code)) {
                probe_end(0, now);
            }
        } else if (kbd.id == 0xAC && code == 0xA1) {
            probe_end(0, now);
        } else {
            probe_goto(PROBE_DISABLE, now);
        }
        return;
    }
    if (code == KBD_RESEND) {
        kbd.out = 0;
        if (kbd.tries < LEDS_TRIES) {
            probe_write(now);
        } else {
            probe_failed(now);
        }
        return;
    }
    if (code != KBD_ACK) {
        return;
    }
    switch (kbd.step) {
    case PROBE_GETID:
        kbd.step = PROBE_ID;
        kbd.nid = 0;
        kbd.at = now;
        break;
    case PROBE_LEDS:
        probe_goto(PROBE_LEDS_OFF, now);
        break;
    case PROBE_LEDS_OFF:
        probe_goto(PROBE_ENABLE, now);
        break;
    case PROBE_DISABLE:
        probe_goto(PROBE_ENABLE, now);
        break;
    default:
        probe_end(1, now);
        break;
    }
}

/** Move the probe on from the tick: write a byte the controller could not
 * take, write it again when its answer has not come in 100 ms, and fail a
 * byte after three writes, a byte the controller has refused for 500 ms,
 * or ID bytes that have not come in 500 ms. */
static void probe_tick(uint32_t now)
{
    if (kbd.out && now - kbd.at < (kbd.step == PROBE_ID ? PROBE_ID_TICKS : LEDS_WAIT_TICKS)) {
        return;
    }
    if (kbd.step == PROBE_ID || kbd.tries >= LEDS_TRIES ||
        (kbd.held && now - kbd.at >= WRITE_WAIT_TICKS)) {
        probe_failed(now);
        return;
    }
    kbd.out = 0;
    probe_write(now);
}

/**
 * Whether `code`, a byte for the set-1 decoder, is the keyboard's BAT, as
 * Linux's atkbd tells it from the release of the left Shift that a
 * translating controller sends as the same 0xAA: a BAT unless it comes
 * inside an E0 or E1 sequence or after the left Shift's press, 0x2A. The
 * sequences and the Shift's state are followed for each byte.
 */
static int kbd_bat(uint8_t code)
{
    int bat = code == KBD_RET_BAT && !kbd.emul && !kbd.shift;

    if (code == 0xE0 || code == 0xE1) {
        kbd.emul = code == 0xE0 ? 1 : 2;
    } else if (code != KBD_ACK && code != KBD_RESEND && code != 0xFF && code != 0x00) {
        if (!kbd.emul && (code & 0x7Fu) == 0x2Au) {
            kbd.shift = !(code & 0x80u);
        } else if (kbd.emul) {
            kbd.emul--;
        }
    }
    return bat;
}

void ps2kbd_tick(void)
{
    uint32_t now = (uint32_t)kernel_ticks;

    if (!leds.ready) {
        return;
    }
    if (kbd.job != JOB_NONE) {
        probe_tick(now);
        return;
    }
    if (kbd.unbound) {
        return;
    }
    if (leds.stage == LEDS_IDLE) {
        unsigned int wanted = input_leds();

        if (kbd.due || (int)wanted == leds.shown || now - leds.at < LEDS_GAP_TICKS) {
            return;
        }
        leds.stage = LEDS_COMMAND;
        leds.value = wanted;
        leds.tries = 0;
        leds.out = 0;
        leds.held = 0;
    }
    if (leds.out && now - leds.at < LEDS_WAIT_TICKS) {
        return;
    }
    if (leds.tries >= LEDS_TRIES || (leds.held && now - leds.at >= WRITE_WAIT_TICKS)) {
        if (!leds.said) {
            leds.said = 1;
            klog(KLOG_WARN, "kbd", "leds: the keyboard did not take %x", leds.value);
        }
        leds_end(now);
        reconnect_if_due(now);
        return;
    }
    leds.out = 0;
    leds_write(now);
}

/**
 * Handle the PS/2 keyboard interrupt: read the port and hand on the code,
 * when the controller has one of the keyboard's waiting. A second interrupt
 * for one byte, which the vector raised for a byte waiting at the switch to
 * the I/O APIC and the line's own message can make (`ps2_waiting` in
 * platform.c), finds the output buffer empty and reads nothing: the data
 * port would give the last byte again. A byte the status marks as the
 * mouse's is left unread for the mouse's IRQ 12 handler: read here, it would
 * be taken for a key and lost from the mouse's packet. While a reconnect or
 * a connect runs, every byte goes to its probe (`probe_reply`). With no
 * driver bound, a byte is dropped, and in auto mode starts a connect, as
 * Linux's serio_interrupt rescans a port without a driver. The answer to an
 * LED command goes to that command (`input_ps2_reply`); a BAT starts the
 * reconnect, as Linux's atkbd asks serio_reconnect, once the LED command
 * under way has ended, and the bytes in between but that command's answers
 * are dropped; any other byte goes to the set-1 decoder, whose keys reach
 * the keyboard's event device and the console.
 *
 * Runs in interrupt context with an arbitrary process's page directory
 * loaded, so it translates through the kernel's own directory around the
 * work, which may touch kernel data the interrupted one does not map.
 */
static void kbd_irq(isr_params_t *params)
{
    uint8_t scancode;
    uintptr_t saved;
    uint8_t status;
    uint32_t now;

    (void)params;
    if (kbd.off) {
        return;
    }
    status = inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL) || (status & PS2_STATUS_MOUSE_DATA)) {
        return;
    }
    scancode = inb(PS2_DATA_PORT);
    saved = paging_enter_kernel();
    now = (uint32_t)kernel_ticks;
    if (kbd.job != JOB_NONE) {
        probe_reply(scancode, now);
    } else if (kbd.unbound) {
        if (!evdev_sysfs_serio_manual(0)) {
            probe_start(JOB_CONNECT, now);
        }
    } else if (kbd.due) {
        (void)input_ps2_reply(scancode);
        reconnect_if_due(now);
    } else if (input_ps2_reply(scancode)) {
        /* the LED command's answer */
    } else if (kbd_bat(scancode)) {
        klog(KLOG_INFO, "kbd", "the keyboard reset itself: reconnecting");
        kbd.due = 1;
        reconnect_if_due(now);
    } else {
        input_set1_event(&ps2_keyboard, scancode);
    }
    paging_leave_kernel(saved);
}

void kbd_init(void)
{
    uint32_t flags = kirq_save();
    int config = ps2_read_config();

    kirq_restore(flags);
    if (config < 0) {
        kbd_stop();
        return;
    }
    kbd.translated = (config & PS2_CONFIG_TRANSLATE) != 0;
    kbd.portable = portable_chassis();
    if (kbd_bind() < 0) {
        klog(KLOG_WARN, "kbd", "no event device is free: unbound");
    }
    set_isr_handler(33, &kbd_irq, "kbd");
    pic_unmask_irq(1);
    leds.shown = -1;
    leds.ready = 1;
}

void kbd_stop(void)
{
    uint32_t flags = kirq_save();

    pic_mask_irq(1);
    pic_mask_irq(12);
    if (!kbd.off) {
        klog(KLOG_WARN, "kbd", "stopped: the i8042's setup failed: unbound");
    }
    if (leds.ready && !kbd.unbound) {
        input_keyboard_detach(&ps2_keyboard);
    }
    evdev_sysfs_serio_unbound(0, 1);
    kbd.off = 1;
    kbd.unbound = 1;
    kbd.job = JOB_NONE;
    kbd.due = 0;
    leds.ready = 0;
    kirq_restore(flags);
}

/** Return the next service deadline for the stopped idle tick. */
uint32_t ps2kbd_idle_ticks(void)
{
    uint32_t wait = TICK_HZ, now = (uint32_t)kernel_ticks;

    if (!leds.ready) {
        return wait;
    }
    if (kbd.job != JOB_NONE) {
        IDLE_TIMER_MIN(wait,
                       kbd.out ? kbd.at + (kbd.step == PROBE_ID ? PROBE_ID_TICKS : LEDS_WAIT_TICKS)
                               : now,
                       now);
    } else if (kbd.unbound) {
        return wait;
    } else if (leds.stage != LEDS_IDLE) {
        IDLE_TIMER_MIN(wait, leds.out ? leds.at + LEDS_WAIT_TICKS : now, now);
    } else if ((int)input_leds() != leds.shown) {
        IDLE_TIMER_MIN(wait, leds.at + LEDS_GAP_TICKS, now);
    }
    return wait;
}
