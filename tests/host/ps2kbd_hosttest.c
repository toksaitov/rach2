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
 * written to the data port, the last MAX_WRITES of them. */
#define MAX_WRITES 32
static uint8_t model_status;
static uint8_t model_data;
static int data_reads;
static int port_writes;
static uint8_t data_writes[MAX_WRITES];
static int n_data_writes;

/* The codes the handler gave to the set-1 decoder, and the last of them. */
static int keys;
static uint8_t last_key;

uint8_t inb(uint16_t port)
{
    if (port == PS2_STATUS_PORT) {
        return model_status;
    }
    if (port == PS2_DATA_PORT) {
        data_reads++;
        return model_data;
    }
    return 0xFF;
}

void outb(uint16_t port, uint8_t val)
{
    port_writes++;
    if (port == PS2_DATA_PORT && n_data_writes < MAX_WRITES) {
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

/** Record the code the decoder was given. */
void input_set1_event(struct input_keyboard *kb, uint8_t code)
{
    (void)kb;
    keys++;
    last_key = code;
}

/** No event device: `input_set1_event` is a model. */
void input_keyboard_attach(struct input_keyboard *kb, const char *name, const char *phys,
                           uint16_t bustype, uint16_t vendor, uint16_t product)
{
    (void)kb;
    (void)name;
    (void)phys;
    (void)bustype;
    (void)vendor;
    (void)product;
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

/** The LED command's rows, after the handler's. */
static void leds_rows(void)
{
    int k;

    CHECK("no command before kbd_init", tick_at(100) == 0);
    kbd_init();

    /* The first command, at once, with the LEDs dark. */
    CHECK("first tick writes 0xED", tick_at(100) == 1 && last_write() == 0xED);
    CHECK("no second byte before the ACK", tick_at(101) == 0);
    keys = 0;
    CHECK("ACK of 0xED writes the LED byte", answer_at(102, 0xFA) == 1 && last_write() == 0x00);
    CHECK("ACK of 0xED is not a key", keys == 0);
    CHECK("ACK of the LED byte writes nothing", answer_at(103, 0xFA) == 0);
    CHECK("ACK of the LED byte is not a key", keys == 0);
    CHECK("nothing more while the LEDs show what is wanted", tick_at(200) == 0);

    /* A change: Caps Lock and Num Lock, 50 ms after the last command. */
    CHECK("an ACK with no command waiting is a key",
          answer_at(201, 0xFA) == 0 && keys == 1 && last_key == 0xFA);
    wanted_leds = 0x06;
    CHECK("a change starts a command", tick_at(201) == 1 && last_write() == 0xED);
    CHECK("its LED byte is the mask", answer_at(202, 0xFA) == 1 && last_write() == 0x06);
    keys = 0;
    CHECK("a key while the LED byte is out is a key", answer_at(203, 0x1E) == 0 && keys == 1);
    CHECK("the ACK still ends the command", answer_at(203, 0xFA) == 0 && keys == 1);
    wanted_leds = 0x01;
    CHECK("no command within 50 ms of the last", tick_at(207) == 0);
    CHECK("the next 50 ms after", tick_at(208) == 1 && last_write() == 0xED);

    /* RESEND writes the byte again. */
    CHECK("RESEND of 0xED writes it again", answer_at(209, 0xFE) == 1 && last_write() == 0xED);
    CHECK("RESEND is not a key", keys == 1);
    CHECK("then the LED byte", answer_at(210, 0xFA) == 1 && last_write() == 0x01);
    CHECK("RESEND of the LED byte writes it again",
          answer_at(211, 0xFE) == 1 && last_write() == 0x01);
    CHECK("the ACK ends it", answer_at(212, 0xFA) == 0 && tick_at(300) == 0);

    /* No answer: written again after 100 ms, given up after three writes. */
    wanted_leds = 0x07;
    CHECK("a command with no answer", tick_at(300) == 1 && last_write() == 0xED);
    CHECK("not written again within 100 ms", tick_at(309) == 0);
    CHECK("written again at 100 ms", tick_at(310) == 1 && last_write() == 0xED);
    CHECK("and a third time", tick_at(320) == 1 && last_write() == 0xED);
    CHECK("then given up, with a warning", tick_at(330) == 0 && warnings == 1);
    CHECK("not tried again for the same LEDs", tick_at(400) == 0);

    /* A full input buffer: nothing written until it empties. */
    wanted_leds = 0x02;
    model_status = PS2_STATUS_INPUT_FULL;
    kernel_ticks = 400;
    k = n_data_writes;
    ps2kbd_tick();
    CHECK("nothing written into a full buffer", n_data_writes == k);
    CHECK("written once it empties", tick_at(401) == 1 && last_write() == 0xED);
    kernel_ticks = 402;
    model_status = KEYBOARD_BYTE | PS2_STATUS_INPUT_FULL;
    model_data = 0xFA;
    kbd_irq(0);
    CHECK("no LED byte into a full buffer after the ACK", n_data_writes == k + 1);
    CHECK("the LED byte at the next tick", tick_at(403) == 1 && last_write() == 0x02);
    CHECK("its ACK ends the command", answer_at(404, 0xFA) == 0 && tick_at(500) == 0);
    CHECK("one warning in all", warnings == 1);
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

    if (fails == 0) {
        printf("ps2kbd-test: pass\n");
    }
    return fails ? 1 : 0;
}
