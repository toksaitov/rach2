/*
 * i8042.c: the waits and writes of the 8042 controller, shared by the PS/2
 * keyboard and mouse, and the firmware's names of its devices, as i8042.h
 * describes them.
 */

#include "i8042.h"
#include "portio.h"
#include "../../lib/kctypes.h"
#include "../../lib/kstring.h"
#ifndef RACH2_HOSTTEST
#include "../../timer.h"
#include "../../udelay.h"
#endif

/* The most reads of the configuration byte that may pass before two in a
 * row agree, Linux's in i8042_controller_init. */
#define PS2_CONFIG_READS 10

uint32_t ps2_wait_begin(void)
{
#ifndef RACH2_HOSTTEST
    return timer_now_us();
#else
    return 0;
#endif
}

int ps2_wait_over(uint32_t start, int reads)
{
    if (reads >= PS2_TIMEOUT) {
        return 1;
    }
#ifndef RACH2_HOSTTEST
    return (reads & 63) == 0 && timer_now_us() - start >= PS2_TIMEOUT_US;
#else
    (void)start;
    return 0;
#endif
}

/** Read the status until its bits `mask` equal `want`: 0, or -1 when the
 * wait is over (`ps2_wait_over`). */
static int ps2_wait(uint8_t mask, uint8_t want)
{
    uint32_t start = ps2_wait_begin();
    int reads = 0;

    do {
        if ((inb(PS2_STATUS_PORT) & mask) == want) {
            return 0;
        }
    } while (!ps2_wait_over(start, ++reads));
    return -1;
}

int ps2_wait_input(void)
{
    return ps2_wait(PS2_STATUS_INPUT_FULL, 0);
}

int ps2_wait_output(void)
{
    return ps2_wait(PS2_STATUS_OUTPUT_FULL, PS2_STATUS_OUTPUT_FULL);
}

int ps2_send_command(uint8_t cmd)
{
    if (ps2_wait_input() < 0) {
        return -1;
    }
    outb(PS2_CMD_PORT, cmd);
    return 0;
}

/** Send READ_CONFIG and return the next byte of the output buffer, of
 * whichever device, or -1 when the controller does not take the command or
 * gives no byte. */
static int read_config_once(void)
{
    if (ps2_send_command(PS2_CMD_READ_CONFIG) < 0 || ps2_wait_output() < 0) {
        return -1;
    }
    return inb(PS2_DATA_PORT);
}

/** Read and drop the bytes waiting in the output buffer, at most 16 of
 * them, Linux's i8042_flush. */
static void ps2_drain(void)
{
    int drained;

    for (drained = 0; drained < 16 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0;
         drained++) {
        inb(PS2_DATA_PORT);
    }
}

/** Send READ_CONFIG until two answers in a row agree, 50 us apart: the
 * byte, or -1 when a read fails or ten pass without agreement. */
static int read_config_agreed(void)
{
    int config[2];
    int n = 0;

    do {
        if (n >= PS2_CONFIG_READS) {
            return -1;
        }
#ifndef RACH2_HOSTTEST
        if (n != 0) {
            k_udelay(50);
        }
#endif
        config[n % 2] = read_config_once();
        if (config[n++ % 2] < 0) {
            return -1;
        }
    } while (n < 2 || config[0] != config[1]);
    return config[0];
}

int ps2_read_config(void)
{
    int config;

    if (inb(PS2_STATUS_PORT) == 0xFF) {
        return -1;
    }
    ps2_drain();
    config = read_config_agreed();
    ps2_drain();
    return config;
}

int ps2_kbd_write(uint8_t byte)
{
    uint8_t status = inb(PS2_STATUS_PORT);

    if (status == 0xFF || (status & PS2_STATUS_INPUT_FULL) != 0) {
        return -1;
    }
    outb(PS2_DATA_PORT, byte);
    return 0;
}

/* The PNP IDs of a keyboard and of a PS/2 mouse, Linux 6.12's
 * `pnp_kbd_devids` and `pnp_aux_devids` (i8042-acpipnpio.h). */
static const char *const kbd_ids[] = {
    "PNP0300", "PNP0301", "PNP0302", "PNP0303", "PNP0304", "PNP0305", "PNP0306", "PNP0309",
    "PNP030a", "PNP030b", "PNP0320", "PNP0343", "PNP0344", "PNP0345", "CPQA0D7", 0};
static const char *const aux_ids[] = {"AUI0200", "FJC6000", "FJC6001", "PNP0f03",
                                      "PNP0f0b", "PNP0f0e", "PNP0f12", "PNP0f13",
                                      "PNP0f19", "PNP0f1c", "SYN0801", 0};

/* Each port's firmware ID, "" until ACPI names a device for it. */
static char firmware_id[2][I8042_FIRMWARE_ID_MAX];

/** Whether `id` is an ID pnpacpi takes, three capitals and four
 * hexadecimal digits in capitals, Linux's `ispnpidacpi`. */
static int pnp_acpi_id(const char *id)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (id[i] < 'A' || id[i] > 'Z') {
            return 0;
        }
    }
    for (; i < 7; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'F'))) {
            return 0;
        }
    }
    return id[7] == 0;
}

/** Whether the device ID `a` is the driver's ID `b`, Linux's
 * `compare_pnp_id`: the vendor exact, the four digits without case, an X
 * in either matching any digit. */
static int pnp_id_is(const char *a, const char *b)
{
    int i;

    if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) {
        return 0;
    }
    for (i = 3; i < 7; i++) {
        if (a[i] != 'X' && b[i] != 'X' && k_toupper(a[i]) != k_toupper(b[i])) {
            return 0;
        }
    }
    return 1;
}

/** Whether one of the `n` IDs of `ids` is in `table`. */
static int pnp_listed(char ids[][8], int n, const char *const *table)
{
    int i, j;

    for (i = 0; i < n; i++) {
        for (j = 0; table[j]; j++) {
            if (pnp_id_is(ids[i], table[j])) {
                return 1;
            }
        }
    }
    return 0;
}

/** Whether the `len` bytes at `a` and `b` are the same, without case. */
static int same_nocase(const char *a, const char *b, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (k_tolower(a[i]) != k_tolower(b[i])) {
            return 0;
        }
    }
    return 1;
}

/** Append `s` to `dst`, of `size` bytes, as far as it holds, as strlcat. */
static void append(char *dst, const char *s, size_t size)
{
    size_t len = k_strlen(dst);

    while (*s && len + 1 < size) {
        dst[len++] = *s++;
    }
    dst[len] = 0;
}

int i8042_pnp_device(const char *const *ids, int n)
{
    char taken[8][8];
    const char *first = 0;
    int count = 0, i, j, port;

    for (i = 0; i < n && count < 8; i++) {
        if (!ids[i] || !pnp_acpi_id(ids[i])) {
            continue;
        }
        /* pnpacpi adds the first such ID, then each other that is not it. */
        if (first && same_nocase(ids[i], first, 8)) {
            continue;
        }
        if (!first) {
            first = ids[i];
        }
        for (j = 0; j < 7; j++) {
            taken[count][j] = j < 3 ? ids[i][j] : k_tolower(ids[i][j]);
        }
        taken[count][7] = 0;
        count++;
    }
    if (pnp_listed(taken, count, kbd_ids)) {
        port = I8042_KBD_PORT;
    } else if (pnp_listed(taken, count, aux_ids)) {
        port = I8042_AUX_PORT;
    } else {
        return -1;
    }
    k_strncpy(firmware_id[port], "PNP:", I8042_FIRMWARE_ID_MAX);
    for (i = 0; i < count; i++) {
        append(firmware_id[port], " ", I8042_FIRMWARE_ID_MAX);
        append(firmware_id[port], taken[i], I8042_FIRMWARE_ID_MAX);
    }
    return port;
}

const char *i8042_firmware_id(int port)
{
    return port == I8042_KBD_PORT || port == I8042_AUX_PORT ? firmware_id[port] : "";
}

int i8042_matches_pnp_id(int port, const char *const *table)
{
    const char *p = i8042_firmware_id(port);
    const char *end;
    size_t len;
    int i;

    if (p[0] != 'P' || p[1] != 'N' || p[2] != 'P' || p[3] != ':' || p[4] != ' ') {
        return 0;
    }
    for (p += 5; *p; p = *end ? end + 1 : end) {
        for (end = p; *end && *end != ' '; end++) {
        }
        len = (size_t)(end - p);
        for (i = 0; table[i]; i++) {
            if (k_strlen(table[i]) == len && same_nocase(p, table[i], len)) {
                return 1;
            }
        }
    }
    return 0;
}
