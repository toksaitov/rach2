/**
 * The PS/2 keyboard's IRQ 1 handler and its LED command, driven on the host
 * against a model of the 8042's status and data ports (sys/arch/x86/ps2kbd.c,
 * with the 8042's writes of i8042.c).
 *
 * The file is included whole so that the static handler is reachable. The
 * model's status register has the value a row sets, and a read of the data
 * port is counted and returns the row's byte. A status with the output
 * buffer full (bit 0) and bit 5 set marks a mouse byte, which the handler
 * must leave unread for the mouse's IRQ 12 handler and give to no one. With
 * bit 5 clear the byte is the keyboard's: the handler reads it and gives it
 * to the set-1 decoder, unless it answers the LED command.
 *
 * The LED rows set the LEDs wanted, step the tick with `kernel_ticks` set by
 * hand, answer with ACK or RESEND through the handler or not at all, and
 * read the bytes written to the data port: 0xED and then the LED byte, one
 * command at a time, 50 ms apart, each byte written again after a RESEND or
 * 100 ms without an answer and given up after three writes, and nothing
 * written while the controller's input buffer is full.
 *
 * The reconnect rows send the keyboard's BAT, 0xAA, and answer the probe
 * that follows, as Linux's atkbd_reconnect runs it: GETID, its ID bytes,
 * RESET_DIS and ENABLE, or the LED command when GETID fails. A probe that
 * fails unbinds the keyboard, whose event device goes, and in auto mode
 * probes serio0 again as a connect, which binds a new one; in manual mode
 * serio0 stays without a driver and drops its bytes until the mode is auto.
 * A BAT during an LED command waits for that command's end; a byte the
 * controller refuses counts no try, and one refused for 500 ms fails; a
 * translated port on a portable chassis skips GETID, as Linux's
 * atkbd_skip_getid; and a connect that finds no event device free leaves
 * serio0 without a driver.
 *
 * The configuration rows read the controller's configuration byte with
 * bytes waiting before the read, which the drain takes, and with a key or
 * mouse byte that arrives after the drain and answers the first READ_CONFIG:
 * the reads go on until two agree, so the translation bit kbd_init caches
 * is the controller's, the answer left over drained before the handler
 * can take it for a key; a controller whose reads never agree gives -1
 * after ten, and kbd_init then keeps translation on, so that on a portable
 * chassis a later BAT starts the probe at RESET_DIS, never at GETID.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DTICK_HZ=1000
 *
 * The rows count ticks at 1000 Hz here and at the shipped 250 Hz in
 * ps2kbd250_hosttest.c, which includes this file.
 */

#include "arch/x86/i8042.c"
#include "arch/x86/ps2kbd.c"

int printf(const char *format, ...);

static int fails;

#define CHECK(what, cond)                            \
    do {                                             \
        if (!(cond)) {                               \
            printf("ps2kbd-test: fail: %s\n", what); \
            fails++;                                 \
        }                                            \
    } while (0)

/* The two statuses of a byte waiting in the output buffer. */
#define KEYBOARD_BYTE PS2_STATUS_OUTPUT_FULL
#define MOUSE_BYTE    (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_MOUSE_DATA)

/* The model 8042: the status the row sets, the byte waiting in the data
 * port, the count of data port reads and of any port write, and the bytes
 * written to the data port, the first MAX_WRITES of them. While `queued`
 * holds bytes, the status is theirs and a read of the data port takes the
 * first. READ_CONFIG queues `stray`, when it is not -1, and then
 * `model_config`: a device byte that came after the drain and before the
 * controller's answer, the mouse's when `stray_mouse` is set. The stray
 * byte comes once, or before every answer while `stray_always` is set.
 * While `late_answers` is set, an answer that finds the output buffer
 * holding a byte waits in `pending` until the controller takes the next
 * byte written, or the row lets time pass (`catch_up`), as a controller
 * that runs its commands in order answers. The byte after WRITE_CONFIG is
 * the configuration, kept in `config_written`, not a byte for the
 * keyboard. */
#define MAX_WRITES 256
#define MAX_QUEUED 64
static uint8_t model_status;
static uint8_t model_data;
static uint8_t model_config = 0x45;
static uint8_t queued[MAX_QUEUED];
static uint8_t queued_status[MAX_QUEUED];
static int n_queued;
static int stray = -1;
static int stray_mouse;
static int stray_always;
static int config_commands;
static int late_answers;
static int pending = -1;
static int config_next;
static int config_written = -1;
static int data_reads;
static int port_writes;
static uint8_t data_writes[MAX_WRITES];
static int n_data_writes;

/* The codes the handler gave to the set-1 decoder, and the last of them. */
static int keys;
static uint8_t last_key;

/** Queue `byte` in the output buffer with `status`, as far as it holds. */
static void queue(uint8_t byte, uint8_t status)
{
    if (n_queued < MAX_QUEUED) {
        queued_status[n_queued] = status;
        queued[n_queued++] = byte;
    }
}

uint8_t inb(uint16_t port)
{
    uint8_t byte;
    int i;

    if (port == PS2_STATUS_PORT) {
        return n_queued ? queued_status[0] : model_status;
    }
    if (port == PS2_DATA_PORT) {
        data_reads++;
        if (n_queued) {
            byte = queued[0];
            for (i = 1; i < n_queued; i++) {
                queued[i - 1] = queued[i];
                queued_status[i - 1] = queued_status[i];
            }
            n_queued--;
            return byte;
        }
        return model_data;
    }
    return 0xFF;
}

/** Let time pass: the answer pending reaches the output buffer. */
static void catch_up(void)
{
    if (pending >= 0) {
        queue((uint8_t)pending, PS2_STATUS_OUTPUT_FULL);
        pending = -1;
    }
}

void outb(uint16_t port, uint8_t val)
{
    port_writes++;
    catch_up();
    if (port == PS2_CMD_PORT && val == PS2_CMD_READ_CONFIG) {
        config_commands++;
        if (stray >= 0) {
            queue((uint8_t)stray, stray_mouse ? MOUSE_BYTE : KEYBOARD_BYTE);
            if (!stray_always) {
                stray = -1;
            }
        }
        if (late_answers && n_queued > 0) {
            pending = model_config;
        } else {
            queue(model_config, PS2_STATUS_OUTPUT_FULL);
        }
    }
    if (port == PS2_CMD_PORT) {
        config_next = val == PS2_CMD_WRITE_CONFIG;
    } else if (port == PS2_DATA_PORT && config_next) {
        config_written = val;
        config_next = 0;
    } else if (port == PS2_DATA_PORT && n_data_writes < MAX_WRITES) {
        data_writes[n_data_writes++] = val;
    }
}

/* The tick the rows set, the LEDs they want, and the warnings logged. */
volatile uint64_t kernel_ticks;
static unsigned int wanted_leds;
static int warnings;

unsigned int input_leds(void)
{
    return wanted_leds;
}

/** Count a warning; the text is not checked. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    if (level == KLOG_WARN) {
        warnings++;
    }
}

/* serio0's bind mode, manual when set; whether the driver marked it
 * unbound; the keyboard's attaches and detaches; whether every event device
 * is taken, so that an attach registers none; and the chassis type the
 * firmware gives. */
static int serio0_manual;
static int serio0_unbound;
static int attaches;
static int detaches;
static int no_nodes;
static const char *chassis;

/** The chassis type alone, or NULL. */
const char *dmi_field(int field)
{
    return field == DMI_CHASSIS_TYPE ? chassis : 0;
}

/** No interrupts to mask. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing to put back. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

int evdev_sysfs_serio_manual(int port)
{
    return port == 0 && serio0_manual;
}

void evdev_sysfs_serio_unbound(int port, int unbound)
{
    if (port == 0) {
        serio0_unbound = unbound;
    }
}

/** Count the detach; the keys released are the input layer's. */
void input_keyboard_detach(struct input_keyboard *kb)
{
    kb->dev = NULL;
    detaches++;
}

/** Record the code the decoder was given. */
void input_set1_event(struct input_keyboard *kb, uint8_t code)
{
    (void)kb;
    keys++;
    last_key = code;
}

/* What an attached keyboard's `dev` points at. */
static int node;

/** An event device that is a token, or none while `no_nodes` is set:
 * `input_set1_event` is a model. */
void input_keyboard_attach(struct input_keyboard *kb, const char *name, const char *phys,
                           uint16_t bustype, uint16_t vendor, uint16_t product)
{
    attaches++;
    kb->dev = no_nodes ? NULL : (struct evdev *)(void *)&node;
    (void)name;
    (void)phys;
    (void)bustype;
    (void)vendor;
    (void)product;
}

/** No keymap: `input_set1_event` sees the raw codes. */
void input_keyboard_set1_keymap(struct input_keyboard *kb, uint16_t *map)
{
    (void)kb;
    (void)map;
}

/** The kernel's translation is the only one here. */
uintptr_t paging_enter_kernel(void)
{
    return 0;
}

/** Nothing to put back. */
void paging_leave_kernel(uintptr_t saved)
{
    (void)saved;
}

/** The handler is called directly. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** No interrupt controller. */
void pic_unmask_irq(uint8_t irq)
{
    (void)irq;
}

/** Run one IRQ 1 with `status` in the status register and `data` waiting,
 * and return the number of times it read the data port. */
static int interrupt(uint8_t status, uint8_t data)
{
    int before = data_reads;

    model_status = status;
    model_data = data;
    kbd_irq(0);
    return data_reads - before;
}

/** Step the tick at `ticks`, the controller's input buffer empty, and
 * return the number of bytes it wrote to the data port. */
static int tick_at(uint32_t ticks)
{
    int before = n_data_writes;

    model_status = 0;
    kernel_ticks = ticks;
    ps2kbd_tick();
    return n_data_writes - before;
}

/** The last byte written to the data port. */
static uint8_t last_write(void)
{
    return n_data_writes > 0 ? data_writes[n_data_writes - 1] : 0;
}

/** Answer with `code` from the keyboard at `ticks` and return the number of
 * bytes the handler wrote to the data port. */
static int answer_at(uint32_t ticks, uint8_t code)
{
    int before = n_data_writes;

    kernel_ticks = ticks;
    interrupt(KEYBOARD_BYTE, code);
    return n_data_writes - before;
}

/* The tick 10 ms * `n` after the boot, the unit the LED rows count in,
 * rounded up as Linux's msecs_to_jiffies rounds: 10 ms is 2.5 ticks at
 * 250 Hz. */
#define T(n) (((uint32_t)(n) * 10u * TICK_HZ + 999u) / 1000u)

/* The ticks of the gap between two commands and of the wait for an answer,
 * Linux's msecs_to_jiffies(50) and msecs_to_jiffies(100). */
#if TICK_HZ == 1000
#define GAP  50u
#define WAIT 100u
#elif TICK_HZ == 250
#define GAP  13u
#define WAIT 25u
#else
#error "ps2kbd_hosttest: the rows count ticks at 1000 Hz or 250 Hz"
#endif

/** The LED command's rows, after the handler's. */
static void leds_rows(void)
{
    int k;

    CHECK("no command before kbd_init", tick_at(T(100)) == 0);
    kbd_init();
    CHECK("kbd_init reads the controller's translation", kbd.translated && n_queued == 0);
    CHECK("and a chassis that names no type is not portable", !kbd.portable);

    /* The first command, at once, with the LEDs dark. */
    CHECK("first tick writes 0xED", tick_at(T(100)) == 1 && last_write() == 0xED);
    CHECK("no second byte before the ACK", tick_at(T(101)) == 0);
    keys = 0;
    CHECK("ACK of 0xED writes the LED byte", answer_at(T(102), 0xFA) == 1 && last_write() == 0x00);
    CHECK("ACK of 0xED is not a key", keys == 0);
    CHECK("ACK of the LED byte writes nothing", answer_at(T(103), 0xFA) == 0);
    CHECK("ACK of the LED byte is not a key", keys == 0);
    CHECK("nothing more while the LEDs show what is wanted", tick_at(T(200)) == 0);

    /* A change: Caps Lock and Num Lock, 50 ms after the last command. */
    CHECK("an ACK with no command waiting is a key",
          answer_at(T(201), 0xFA) == 0 && keys == 1 && last_key == 0xFA);
    wanted_leds = 0x06;
    CHECK("a change starts a command", tick_at(T(201)) == 1 && last_write() == 0xED);
    CHECK("its LED byte is the mask", answer_at(T(202), 0xFA) == 1 && last_write() == 0x06);
    keys = 0;
    CHECK("a key while the LED byte is out is a key", answer_at(T(203), 0x1E) == 0 && keys == 1);
    CHECK("the ACK still ends the command", answer_at(T(203), 0xFA) == 0 && keys == 1);
    wanted_leds = 0x01;
    CHECK("no command within 50 ms of the last", tick_at(T(203) + GAP - 1) == 0);
    CHECK("the next 50 ms after", tick_at(T(203) + GAP) == 1 && last_write() == 0xED);

    /* RESEND writes the byte again. */
    CHECK("RESEND of 0xED writes it again", answer_at(T(209), 0xFE) == 1 && last_write() == 0xED);
    CHECK("RESEND is not a key", keys == 1);
    CHECK("then the LED byte", answer_at(T(210), 0xFA) == 1 && last_write() == 0x01);
    CHECK("RESEND of the LED byte writes it again",
          answer_at(T(211), 0xFE) == 1 && last_write() == 0x01);
    CHECK("the ACK ends it", answer_at(T(212), 0xFA) == 0 && tick_at(T(300)) == 0);

    /* No answer: written again after 100 ms, given up after three writes. */
    wanted_leds = 0x07;
    CHECK("a command with no answer", tick_at(T(300)) == 1 && last_write() == 0xED);
    CHECK("not written again within 100 ms", tick_at(T(300) + WAIT - 1) == 0);
    CHECK("written again at 100 ms", tick_at(T(300) + WAIT) == 1 && last_write() == 0xED);
    CHECK("not a third time within 100 ms", tick_at(T(300) + 2 * WAIT - 1) == 0);
    CHECK("and a third time", tick_at(T(300) + 2 * WAIT) == 1 && last_write() == 0xED);
    CHECK("not given up within 100 ms", tick_at(T(300) + 3 * WAIT - 1) == 0 && warnings == 0);
    CHECK("then given up, with a warning", tick_at(T(300) + 3 * WAIT) == 0 && warnings == 1);
    CHECK("not tried again for the same LEDs", tick_at(T(400)) == 0);

    /* A full input buffer: nothing written until it empties. */
    wanted_leds = 0x02;
    model_status = PS2_STATUS_INPUT_FULL;
    kernel_ticks = T(400);
    k = n_data_writes;
    ps2kbd_tick();
    CHECK("nothing written into a full buffer", n_data_writes == k);
    CHECK("written once it empties", tick_at(T(401)) == 1 && last_write() == 0xED);
    kernel_ticks = T(402);
    model_status = KEYBOARD_BYTE | PS2_STATUS_INPUT_FULL;
    model_data = 0xFA;
    kbd_irq(0);
    CHECK("no LED byte into a full buffer after the ACK", n_data_writes == k + 1);
    CHECK("the LED byte at the next tick", tick_at(T(403)) == 1 && last_write() == 0x02);
    CHECK("its ACK ends the command", answer_at(T(404), 0xFA) == 0 && tick_at(T(500)) == 0);
    CHECK("one warning in all", warnings == 1);
}

/** Answer `n` bytes at `ticks` and return the bytes written meanwhile. */
static int answers_at(uint32_t ticks, const uint8_t *codes, int n)
{
    int i, wrote = 0;

    for (i = 0; i < n; i++) {
        wrote += answer_at(ticks, codes[i]);
    }
    return wrote;
}

/* GETID's ACK and a keyboard's ID bytes, and its ACK and a byte that
 * begins no keyboard's ID. */
static const uint8_t id[] = {0xFA, 0xAB, 0x41};
static const uint8_t not_id[] = {0xFA, 0x00};

/** The BAT's rows, after the LED command's, which leave the LEDs at 0x02
 * from T(404): 0xAA after the left Shift's press, or after E0, is a key's
 * release, and a BAT's reconnect that finds the keyboard keeps its event
 * device. */
static void bat_rows(void)
{
    keys = 0;
    CHECK("the left Shift's press is a key", answer_at(T(500), 0x2A) == 0 && keys == 1);
    CHECK("its release, 0xAA, is a key and no BAT",
          answer_at(T(501), 0xAA) == 0 && keys == 2 && last_key == 0xAA);
    CHECK("E0 AA is a fake Shift's release and no BAT",
          answer_at(T(502), 0xE0) == 0 && answer_at(T(502), 0xAA) == 0 && keys == 4);

    /* A BAT: the reconnect probes at once and keeps the event device. */
    CHECK("a BAT writes GETID", answer_at(T(510), 0xAA) == 1 && last_write() == 0xF2);
    CHECK("the BAT is no key", keys == 4);
    CHECK("a key during the reconnect is dropped", answer_at(T(511), 0x1E) == 0 && keys == 4);
    CHECK("GETID's ACK writes nothing", answer_at(T(512), 0xFA) == 0);
    CHECK("its ID bytes write RESET_DIS",
          answer_at(T(512), 0xAB) == 0 && answer_at(T(513), 0x41) == 1 && last_write() == 0xF5);
    CHECK("RESET_DIS's ACK writes ENABLE", answer_at(T(514), 0xFA) == 1 && last_write() == 0xF4);
    CHECK("ENABLE's ACK ends the reconnect", answer_at(T(515), 0xFA) == 0);
    CHECK("which kept the event device", attaches == 1 && detaches == 0 && !serio0_unbound);
    CHECK("its ACK is no key", keys == 4);
    CHECK("the LEDs are sent again", tick_at(T(516)) == 1 && last_write() == 0xED);
    CHECK("with the LEDs wanted", answer_at(T(517), 0xFA) == 1 && last_write() == 0x02);
    CHECK("and the command ends", answer_at(T(518), 0xFA) == 0 && tick_at(T(600)) == 0);
    CHECK("keys flow again", answer_at(T(600), 0x1E) == 0 && keys == 5);
}

/** GETID unanswered: the LED command then answers for the keyboard. */
static void getid_rows(void)
{
    CHECK("a second BAT writes GETID", answer_at(T(700), 0xAA) == 1 && last_write() == 0xF2);
    CHECK("GETID again after 100 ms", tick_at(T(700) + WAIT) == 1 && last_write() == 0xF2);
    CHECK("and a third time", tick_at(T(700) + 2 * WAIT) == 1 && last_write() == 0xF2);
    CHECK("then the LED command", tick_at(T(700) + 3 * WAIT) == 1 && last_write() == 0xED);
    CHECK("whose ACK writes the LEDs off", answer_at(T(800), 0xFA) == 1 && last_write() == 0x00);
    CHECK("whose ACK writes ENABLE", answer_at(T(801), 0xFA) == 1 && last_write() == 0xF4);
    CHECK("whose ACK keeps the binding",
          answer_at(T(802), 0xFA) == 0 && attaches == 1 && detaches == 0 && !serio0_unbound);
    CHECK("the LEDs follow", tick_at(T(803)) == 1 && last_write() == 0xED &&
                                 answer_at(T(804), 0xFA) == 1 && answer_at(T(805), 0xFA) == 0);
}

/** A reconnect that finds no keyboard's ID, in auto mode: unbound, then
 * probed as a connect, which binds a new event device. */
static void unbind_rows(void)
{
    int k = warnings;

    CHECK("a third BAT writes GETID", answer_at(T(900), 0xAA) == 1 && last_write() == 0xF2);
    CHECK("an ID that is no keyboard's fails the reconnect and probes anew",
          answers_at(T(901), not_id, 2) == 1 && last_write() == 0xF2);
    CHECK("the failure unbinds the keyboard", detaches == 1 && serio0_unbound && warnings > k);
    CHECK("the connect's ID writes RESET_DIS",
          answers_at(T(902), id, 3) == 1 && last_write() == 0xF5);
    CHECK("then ENABLE", answer_at(T(903), 0xFA) == 1 && last_write() == 0xF4);
    CHECK("whose ACK binds a new event device",
          answer_at(T(904), 0xFA) == 0 && attaches == 2 && detaches == 1 && !serio0_unbound);
    CHECK("the new device's LEDs follow", tick_at(T(905)) == 1 && last_write() == 0xED &&
                                              answer_at(T(906), 0xFA) == 1 &&
                                              answer_at(T(907), 0xFA) == 0);
}

/** Manual mode: a reconnect that finds a keyboard keeps the binding, and
 * one that fails leaves serio0 without a driver. */
static void manual_rows(void)
{
    serio0_manual = 1;
    CHECK("a BAT in manual mode writes GETID",
          answer_at(T(1000), 0xAA) == 1 && last_write() == 0xF2);
    CHECK("an answer that binds", answers_at(T(1001), id, 3) == 1 &&
                                      answer_at(T(1002), 0xFA) == 1 &&
                                      answer_at(T(1003), 0xFA) == 0);
    CHECK("keeps the binding in manual mode", attaches == 2 && detaches == 1 && !serio0_unbound);
    CHECK("the LEDs follow it",
          tick_at(T(1004)) == 1 && answer_at(T(1005), 0xFA) == 1 && answer_at(T(1006), 0xFA) == 0);

    CHECK("another BAT in manual mode writes GETID",
          answer_at(T(1100), 0xAA) == 1 && last_write() == 0xF2);
    CHECK("its failure probes nothing", answers_at(T(1101), not_id, 2) == 0);
    CHECK("and unbinds the keyboard", detaches == 2 && serio0_unbound && attaches == 2);
    keys = 0;
    CHECK("whose bytes are dropped, writing nothing", answer_at(T(1102), 0x1E) == 0 && keys == 0);
    CHECK("and whose LEDs are not sent", tick_at(T(1200)) == 0);
}

/** Auto mode again: the next byte probes serio0 as a connect, and a failed
 * one leaves it unbound until the byte after. */
static void auto_again_rows(void)
{
    serio0_manual = 0;
    CHECK("the first byte in auto mode writes GETID",
          answer_at(T(1300), 0x1E) == 1 && last_write() == 0xF2 && keys == 0);
    CHECK("a connect that fails leaves serio0 unbound",
          answers_at(T(1301), not_id, 2) == 0 && attaches == 2 && serio0_unbound);
    CHECK("the next byte probes again", answer_at(T(1400), 0x9E) == 1 && last_write() == 0xF2);
    CHECK("and a keyboard that answers is bound",
          answers_at(T(1401), id, 3) == 1 && answer_at(T(1402), 0xFA) == 1 &&
              answer_at(T(1403), 0xFA) == 0 && attaches == 3 && !serio0_unbound);
    CHECK("its keys reach the decoder", answer_at(T(1500), 0x1E) == 0 && keys == 1);
}

/** Step the tick at `ticks` with the controller's input buffer full, and
 * return the number of bytes written to the data port. */
static int tick_full(uint32_t ticks)
{
    int before = n_data_writes;

    model_status = PS2_STATUS_INPUT_FULL;
    kernel_ticks = ticks;
    ps2kbd_tick();
    return n_data_writes - before;
}

/** Give the keyboard's BAT at `ticks` with the controller's input buffer
 * full, and return the number of bytes written. */
static int bat_full(uint32_t ticks)
{
    int before = n_data_writes;

    kernel_ticks = ticks;
    model_status = KEYBOARD_BYTE | PS2_STATUS_INPUT_FULL;
    model_data = 0xAA;
    kbd_irq(0);
    return n_data_writes - before;
}

/** Answer at `ticks` a probe whose GETID is out as a keyboard does: ACK and
 * ID, RESET_DIS's ACK and ENABLE's ACK, which end it. */
static int probe_answered(uint32_t ticks)
{
    return answers_at(ticks, id, 3) == 1 && last_write() == 0xF5 && answer_at(ticks, 0xFA) == 1 &&
           last_write() == 0xF4 && answer_at(ticks, 0xFA) == 0 && kbd.job == JOB_NONE;
}

/** Send the LEDs at `ticks` and answer both bytes. */
static int leds_sent(uint32_t ticks)
{
    return tick_at(ticks) == 1 && last_write() == 0xED && answer_at(ticks + 1, 0xFA) == 1 &&
           answer_at(ticks + 2, 0xFA) == 0 && leds.stage == LEDS_IDLE;
}

/** A BAT while an LED command is under way: the reconnect waits for that
 * command's end, its answers or its last wait, as Linux's atkbd_reconnect
 * waits for the LED work, so that a late ACK is not taken for GETID's. */
static void leds_bat_rows(void)
{
    int a = attaches, d = detaches;

    CHECK("the new binding's LEDs start", tick_at(T(1600)) == 1 && last_write() == 0xED);
    keys = 0;
    CHECK("a BAT while 0xED is out writes no GETID", answer_at(T(1601), 0xAA) == 0 && kbd.due);
    CHECK("a key while the reconnect is due is dropped",
          answer_at(T(1601), 0x1E) == 0 && keys == 0);
    CHECK("no tick starts the probe meanwhile", tick_at(T(1602)) == 0);
    CHECK("0xED's late ACK writes the LED byte",
          answer_at(T(1603), 0xFA) == 1 && last_write() == 0x02);
    CHECK("whose ACK ends the command and writes GETID",
          answer_at(T(1604), 0xFA) == 1 && last_write() == 0xF2 && !kbd.due);
    CHECK("GETID's own ACK and ID then find the keyboard", probe_answered(T(1605)));
    CHECK("which keeps the binding", attaches == a && detaches == d && !serio0_unbound);
    CHECK("the LEDs are sent again", leds_sent(T(1606)));

    wanted_leds = 0x04;
    CHECK("a command no answer ends", tick_at(T(1700)) == 1 && last_write() == 0xED);
    CHECK("a BAT during it writes nothing", answer_at(T(1701), 0xAA) == 0);
    CHECK("0xED again at 100 ms", tick_at(T(1700) + WAIT) == 1 && last_write() == 0xED);
    CHECK("and a third time", tick_at(T(1700) + 2 * WAIT) == 1 && last_write() == 0xED);
    CHECK("the command given up writes GETID",
          tick_at(T(1700) + 3 * WAIT) == 1 && last_write() == 0xF2);
    CHECK("which finds the keyboard", probe_answered(T(1800)));
    CHECK("whose LEDs follow", leds_sent(T(1801)));
}

/** A full input buffer during a probe: a byte refused counts no try, and
 * one refused for 500 ms fails, as Linux's i8042_wait_write gives up. */
static void busy_rows(void)
{
    uint32_t t = T(1900);

    CHECK("a BAT into a full buffer writes nothing", bat_full(t) == 0 && kbd.job == JOB_RECONNECT);
    CHECK("ticks of a full buffer write nothing", tick_full(t + 1) == 0 && tick_full(t + 2) == 0 &&
                                                      tick_full(t + 3) == 0 &&
                                                      tick_full(t + 4) == 0);
    CHECK("and count no try: GETID once the buffer empties",
          tick_at(t + 5) == 1 && last_write() == 0xF2 && kbd.tries == 1);
    CHECK("which finds the keyboard", probe_answered(t + 6));
    CHECK("whose LEDs follow", leds_sent(t + 7));

    t = T(2000);
    CHECK("another BAT into a full buffer", bat_full(t) == 0 && kbd.step == PROBE_GETID);
    CHECK("GETID refused for less than 500 ms is not failed",
          tick_full(t + WRITE_WAIT_TICKS - 1) == 0 && kbd.step == PROBE_GETID);
    CHECK("refused for 500 ms it fails, as a GETID unanswered",
          tick_full(t + WRITE_WAIT_TICKS) == 0 && kbd.step == PROBE_LEDS);
    CHECK("the LED command once the buffer empties",
          tick_at(t + WRITE_WAIT_TICKS + 1) == 1 && last_write() == 0xED);
    CHECK("whose ACK writes the LEDs off",
          answer_at(t + WRITE_WAIT_TICKS + 2, 0xFA) == 1 && last_write() == 0x00);
    CHECK("then ENABLE", answer_at(t + WRITE_WAIT_TICKS + 3, 0xFA) == 1 && last_write() == 0xF4);
    CHECK("whose ACK keeps the binding",
          answer_at(t + WRITE_WAIT_TICKS + 4, 0xFA) == 0 && !serio0_unbound);
    CHECK("whose LEDs follow", leds_sent(t + WRITE_WAIT_TICKS + 5));
}

/** A portable chassis: with the controller translating, a BAT's probe
 * skips GETID for RESET_DIS, as Linux's atkbd_skip_getid, and a keyboard
 * that answers nothing stays bound; without translation GETID is asked. */
static void portable_rows(void)
{
    static const char *const yes[] = {"8", "9", "10", "14", "31", "32"};
    static const char *const no[] = {"1", "3", "100", "", "93", "3"};
    int i, a, d, w, k;

    for (i = 0; i < 6; i++) {
        chassis = yes[i];
        CHECK("Linux's portable chassis types are portable", portable_chassis());
        chassis = no[i];
        CHECK("other chassis types are not", !portable_chassis());
    }
    chassis = "9";
    kbd.portable = portable_chassis();
    kbd.translated = 0;
    CHECK("a portable machine without translation asks GETID",
          answer_at(T(2200), 0xAA) == 1 && last_write() == 0xF2);
    CHECK("which finds the keyboard", probe_answered(T(2201)));
    CHECK("whose LEDs follow", leds_sent(T(2202)));

    kbd.translated = 1;
    a = attaches;
    d = detaches;
    w = warnings;
    CHECK("with translation a BAT writes RESET_DIS",
          answer_at(T(2300), 0xAA) == 1 && last_write() == 0xF5);
    CHECK("again after 100 ms", tick_at(T(2300) + WAIT) == 1 && last_write() == 0xF5);
    CHECK("and a third time", tick_at(T(2300) + 2 * WAIT) == 1 && last_write() == 0xF5);
    CHECK("then ENABLE", tick_at(T(2300) + 3 * WAIT) == 1 && last_write() == 0xF4);
    CHECK("written three times", tick_at(T(2300) + 4 * WAIT) == 1 &&
                                     tick_at(T(2300) + 5 * WAIT) == 1 && last_write() == 0xF4);
    CHECK("then the reconnect ends with a warning",
          tick_at(T(2300) + 6 * WAIT) == 0 && kbd.job == JOB_NONE && warnings == w + 1);
    CHECK("and the keyboard that answered nothing stays bound",
          attaches == a && detaches == d && !serio0_unbound);
    k = keys;
    CHECK("its keys flow", answer_at(T(2900), 0x1E) == 0 && keys == k + 1);
    CHECK("whose LEDs follow", leds_sent(T(2901)));

    CHECK("a keyboard that answers is probed from RESET_DIS",
          answer_at(T(3000), 0xAA) == 1 && last_write() == 0xF5 && answer_at(T(3001), 0xFA) == 1 &&
              last_write() == 0xF4 && answer_at(T(3002), 0xFA) == 0 && kbd.job == JOB_NONE);
    CHECK("whose LEDs follow", leds_sent(T(3003)));
    kbd.portable = 0;
    chassis = 0;
}

/** Every event device taken: a connect that finds the keyboard binds
 * nothing and leaves serio0 without a driver, and a byte in auto mode once
 * a node is free binds it. */
static void nodes_rows(void)
{
    int a = attaches, d = detaches, w = warnings;

    no_nodes = 1;
    CHECK("a BAT writes GETID", answer_at(T(3100), 0xAA) == 1 && last_write() == 0xF2);
    CHECK("an ID that is no keyboard's probes anew",
          answers_at(T(3101), not_id, 2) == 1 && last_write() == 0xF2 && detaches == d + 1);
    CHECK("the connect finds the keyboard",
          answers_at(T(3102), id, 3) == 1 && answer_at(T(3103), 0xFA) == 1 && last_write() == 0xF4);
    CHECK("but with no event device free",
          answer_at(T(3104), 0xFA) == 0 && attaches == a + 1 && detaches == d + 2);
    CHECK("serio0 is left without a driver, with a warning",
          serio0_unbound && kbd.unbound && !ps2_keyboard.dev && warnings > w);
    CHECK("and its LEDs are not sent", tick_at(T(3200)) == 0);
    no_nodes = 0;
    keys = 0;
    CHECK("a byte in auto mode probes again",
          answer_at(T(3300), 0x1E) == 1 && last_write() == 0xF2 && keys == 0);
    CHECK("and binds the keyboard with a node free",
          probe_answered(T(3301)) && attaches == a + 2 && !serio0_unbound && ps2_keyboard.dev);
    CHECK("whose keys flow", answer_at(T(3302), 0x1E) == 0 && keys == 1);
    CHECK("and whose LEDs follow", leds_sent(T(3303)));
}

/** The configuration byte: bytes before the read drained, and a key or
 * mouse byte between the drain and the controller's answer never taken for
 * it, so kbd_init caches the controller's translation bit. */
static void config_rows(void)
{
    int c, k;

    model_status = 0;
    n_queued = 0;
    queue(0x1C, KEYBOARD_BYTE);
    queue(0x09, MOUSE_BYTE);
    c = config_commands;
    CHECK("bytes waiting before the read are drained",
          ps2_read_config() == 0x45 && config_commands == c + 2 && n_queued == 0);

    stray = 0x1E;
    c = config_commands;
    CHECK("a key byte after the drain is not the configuration",
          ps2_read_config() == 0x45 && config_commands == c + 3);
    /* The first answer waits behind the stray byte and answers the second
     * READ_CONFIG; the third's answer is left over, and drained. */
    CHECK("and the answer left over is drained", n_queued == 0);
    stray_mouse = 1;
    stray = 0x08;
    c = config_commands;
    CHECK("nor a mouse byte",
          ps2_read_config() == 0x45 && config_commands == c + 3 && n_queued == 0);
    stray_mouse = 0;

    late_answers = 1;
    stray = 0x1E;
    config_written = -1;
    c = config_commands;
    CHECK("an answer that comes late reads 0x1E, 0x45, 0x45",
          ps2_read_config() == 0x45 && config_commands == c + 3);
    CHECK("and the byte is written back", config_written == 0x45);
    catch_up();
    k = keys;
    model_status = 0;
    kbd_irq(0);
    CHECK("so the last answer came before the drain, which took it",
          pending < 0 && n_queued == 0 && keys == k);
    late_answers = 0;

    stray = 0x1E;
    kbd.translated = 0;
    kbd_init();
    CHECK("kbd_init caches the controller's translation past a key byte", kbd.translated == 1);
    c = data_reads;
    k = keys;
    model_status = 0;
    kbd_irq(0);
    CHECK("and leaves no answer for the handler, which takes no key",
          n_queued == 0 && data_reads == c && keys == k);
}

/** A controller that answers every READ_CONFIG after a fresh key byte reads
 * 0x1E and 0x45 in turn, never twice the same: -1 after ten reads. */
static void unstable_rows(void)
{
    int c = config_commands;

    stray = 0x1E;
    stray_always = 1;
    n_queued = 0;
    CHECK("reads that never agree give -1", ps2_read_config() == -1);
    CHECK("after ten of them", config_commands == c + 10);
    CHECK("leaving no byte waiting", n_queued == 0);
    stray = -1;
    stray_always = 0;
}

/** A configuration read that fails on a portable chassis: kbd_init takes
 * translation to be on, with a warning, so a later BAT starts the probe at
 * RESET_DIS and never sends the laptop's keyboard GETID. */
static void failed_config_rows(void)
{
    int w = warnings, k;

    chassis = "9";
    stray = 0x1E;
    stray_always = 1;
    kbd.translated = 0;
    kbd_init();
    stray = -1;
    stray_always = 0;
    CHECK("a failed configuration read keeps translation",
          kbd.translated == 1 && kbd.portable && warnings == w + 1 && n_queued == 0);
    CHECK("so a BAT writes RESET_DIS, not GETID",
          answer_at(T(3400), 0xAA) == 1 && last_write() == 0xF5);
    CHECK("and the keyboard answers it and ENABLE",
          answer_at(T(3401), 0xFA) == 1 && last_write() == 0xF4 && answer_at(T(3402), 0xFA) == 0 &&
              kbd.job == JOB_NONE && !serio0_unbound);
    k = keys;
    CHECK("its keys flow", answer_at(T(3500), 0x1E) == 0 && keys == k + 1);
    CHECK("whose LEDs follow", leds_sent(T(3501)));
    kbd.portable = 0;
    chassis = 0;
}

int main(void)
{
    /* A mouse byte, the first of a packet. */
    CHECK("mouse byte leaves the data port unread", interrupt(MOUSE_BYTE, 0x09) == 0);
    CHECK("mouse byte is not a key", keys == 0);

    /* A keyboard byte: the make code of Enter. */
    CHECK("keyboard byte is read", interrupt(KEYBOARD_BYTE, 0x1C) == 1);
    CHECK("keyboard byte is one key", keys == 1);
    CHECK("keyboard byte is the code given", last_key == 0x1C);

    /* An empty buffer, with and without the auxiliary mark. */
    CHECK("empty buffer leaves the data port unread", interrupt(0, 0x9C) == 0);
    CHECK("marked empty buffer leaves the data port unread",
          interrupt(PS2_STATUS_MOUSE_DATA, 0x9C) == 0);
    CHECK("empty buffer is not a key", keys == 1);

    /* A mouse byte after a key, then the key's break code. */
    CHECK("mouse byte after a key leaves the data port unread", interrupt(MOUSE_BYTE, 0xFE) == 0);
    CHECK("break code is read", interrupt(KEYBOARD_BYTE, 0x9C) == 1);
    CHECK("break code is the second key", keys == 2 && last_key == 0x9C);

    CHECK("the handler writes no port", port_writes == 0);

    leds_rows();
    bat_rows();
    getid_rows();
    unbind_rows();
    manual_rows();
    auto_again_rows();
    leds_bat_rows();
    busy_rows();
    portable_rows();
    nodes_rows();
    config_rows();
    unstable_rows();
    failed_config_rows();

    if (fails == 0) {
        printf("ps2kbd-test: pass\n");
    }
    return fails ? 1 : 0;
}
