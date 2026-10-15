/**
 * The PS/2 keyboard's IRQ 1 handler, driven on the host against a model of
 * the 8042's status and data ports (sys/arch/x86/ps2kbd.c).
 *
 * The file is included whole so that the static handler is reachable. The
 * model's status register has the value a row sets, and a read of the data
 * port is counted and returns the row's byte. A status with the output
 * buffer full (bit 0) and bit 5 set marks a mouse byte, which the handler
 * must leave unread for the mouse's IRQ 12 handler and give to no one. With
 * bit 5 clear the byte is the keyboard's: the handler reads it and gives it
 * to the input layer.
 */

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
 * port, and the count of data port reads and of any port write. */
static uint8_t model_status;
static uint8_t model_data;
static int data_reads;
static int port_writes;

/* The codes the handler gave to the input layer, and the last of them. */
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
    (void)port;
    (void)val;
    port_writes++;
}

void input_key(uint8_t scancode)
{
    keys++;
    last_key = scancode;
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

/** `kbd_init` is not run. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** `kbd_init` is not run. */
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

    if (fails == 0) {
        printf("ps2kbd-test: pass\n");
    }
    return fails ? 1 : 0;
}
