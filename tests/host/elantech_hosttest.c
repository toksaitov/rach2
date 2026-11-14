/**
 * The Elantech touchpad's detection, setup and packets (sys/arch/x86/
 * elantech.c), through the PS/2 mouse's bring-up and IRQ 12 handler
 * (ps2mouse.c) after the Synaptics probe (synaptics.c), against a model of
 * the 8042 and of an Elantech pad of each hardware version behind it, with
 * the event devices read as a program reads them (evdev.c) and the mouse
 * device's packets (mouse.c).
 *
 * host-deps: rach2/sys/evdev.c rach2/sys/mouse.c rach2/sys/arch/x86/smbios.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 *
 * The driver's files are included whole, so that their static state is
 * reachable. The model pad answers as Documentation/input/devices/
 * elantech.rst and Linux 6.12's elantech.c have it: 0x3C 0x03 0xC8 to the
 * magic knock, three 1:1 scalings and a status request; a query's three
 * bytes to a Synaptics sliced query, four resolutions after 1:1 scaling
 * and a status request, or to 0xF8, the query and a status request; and
 * register reads and writes in each version's sequence, which a status
 * request or a final 1:1 scaling ends. Each pad is a row of `profiles`,
 * its replies to the queries 0x00 to 0x05 and the machine SMBIOS names,
 * from the firmware and capability table in Linux's elantech.c where a
 * machine has one: a version 1 pad with a rocker switch and one whose
 * first touches jump, version 2 pads of the old constant bits, with
 * pressure, of a queried range and of a fixed resolution, a version 3 pad
 * with a pointing stick and a clickpad with CRC packets on a Gigabyte
 * U2442, and version 4 clickpads, an IC body 7 pad of sample byte 0x2A, a
 * Fujitsu CELSIUS H730 with a stick, a new IC with a pattern and SMBus,
 * and the firmware 0x381f17 that loses its mode at a rate; and a version 3
 * pad with no stick, given a stick's packet, which Linux reports to no
 * device (dev-log/76-display/upstream.md). A row can make
 * the pad refuse a register write, leave register reads unanswered, refuse
 * version 1's absolute bit, or be a Logitech mouse that answers the knock.
 * The probe for an ALPS touchpad, which alps_hosttest.c tests, finds none
 * here.
 *
 * The capabilities and the events each row expects are what Linux 6.12's
 * elantech.c, with its input core, gives the same pad and the same
 * packets: its `elantech_query_info`, `elantech_set_input_params` and
 * decoders were run over these replies and packets, and their output is
 * the tables below. The capabilities are printed as /proc/bus/input/devices
 * prints them and compared as strings.
 */

#include "arch/x86/i8042.c"
#include "arch/x86/ps2mouse.c"
#include "arch/x86/synaptics.c"
#include "arch/x86/elantech.c"
#include "arch/x86/trackpoint.c"

#include "fs/fd.h"
#include "lib/kerrno.h"
#include "proc/wait_queue.h"

int printf(const char *format, ...);
int sprintf(char *s, const char *format, ...);
int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                              \
    do {                                               \
        if (!(cond)) {                                 \
            printf("elantech-test: fail: %s\n", what); \
            fails++;                                   \
        }                                              \
    } while (0)

/* The status of a byte the mouse sent, and the configuration byte the
 * firmware leaves and the one a bring-up writes. */
#define MOUSE_BYTE      (PS2_STATUS_OUTPUT_FULL | PS2_STATUS_MOUSE_DATA)
#define FIRMWARE_CONFIG 0x65
#define MOUSE_CONFIG    0x47

/* The ticks of the kernel's clock, which a row moves on. */
volatile uint64_t kernel_ticks;

/* A pad of `profiles`: its replies to the queries, the SMBIOS maker and
 * product of its machine, and what Linux 6.12 makes of it: the hardware
 * version, the firmware version, the range's maxima, the resolution, the
 * width of a trace, the product ID, whether it has a stick, and its
 * properties, keys and axes in the form of /proc/bus/input/devices, with
 * each axis's code, minimum, maximum and resolution but the slot's and
 * tracking ID's. */
struct profile {
    const char *name;
    uint8_t fw[3];
    uint8_t caps[3];
    uint8_t samples[3];
    uint8_t fw_id[3]; /* the range of versions 3 and 4 */
    uint8_t res[3];
    uint8_t icbody[3];
    const char *maker;
    const char *product;
    int hw;
    uint32_t fw_version;
    uint32_t x_max, y_max, x_res, y_res, width, product_id;
    int stick;
    const char *prop, *key, *abs, *limits;
};

enum {
    V1_ROCKER,
    V1_JUMPY,
    V2_OLD,
    V2_PRESSURE,
    V2_QUERIED,
    V2_FIXED_DPI,
    V3_STICK,
    V3_CRC,
    V4_CLICKPAD,
    V4_2A,
    V4_H730,
    V4_NEW_IC,
    V4_TP500,
    V3_K53SV,
    V3_BROKEN,
    N_PROFILES
};

/* An event a row expects. */
struct event {
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/* Which device a packet's events go to. */
#define PAD   0
#define STICK 1

/* A packet of a pad, four bytes on version 1 and six on the others, whether
 * Linux drops it as out of form, and the events Linux gives it. */
struct step {
    int profile;
    int dev;
    uint8_t bytes[6];
    int bad;
    const char *what;
    int n;
    struct event want[16];
};

/* The pads, in the order of their enum: the replies to the queries 0x01, 0x02,
 * 0x03, 0x00, 0x04 and 0x05, the machine, and what Linux 6.12 makes of them. */
static const struct profile profiles[] = {
    {"V1_ROCKER",
     {0x01, 0x04, 0x00},
     {0x04, 0x00, 0x00},
     {0, 0, 0},
     {0, 0, 0},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     1,
     0x010400,
     544,
     352,
     31,
     31,
     0,
     0x0,
     0,
     "1",
     "6420 630000 0 0 0 0",
     "3",
     "0:32:544:31 1:32:352:31"},
    {"V1_JUMPY",
     {0x02, 0x00, 0x22},
     {0x00, 0x00, 0x00},
     {0, 0, 0},
     {0, 0, 0},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     1,
     0x020022,
     544,
     352,
     31,
     31,
     0,
     0x0,
     0,
     "1",
     "6420 30000 0 0 0 0",
     "3",
     "0:32:544:31 1:32:352:31"},
    {"V2_OLD",
     {0x02, 0x00, 0x30},
     {0x00, 0x14, 0x0e},
     {0x01, 0x10, 0x64},
     {0, 0, 0},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     2,
     0x020030,
     1152,
     768,
     31,
     31,
     0,
     0x10,
     0,
     "9",
     "e420 30000 0 0 0 0",
     "260800000000003",
     "0:0:1152:31 1:0:768:31 35:0:1152:31 36:0:768:31"},
    {"V2_PRESSURE",
     {0x02, 0x08, 0x00},
     {0x00, 0x14, 0x0e},
     {0x01, 0x10, 0x64},
     {0, 0, 0},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     2,
     0x020800,
     1152,
     768,
     31,
     31,
     0,
     0x10,
     0,
     "9",
     "e420 30000 0 0 0 0",
     "260800011000003",
     "0:0:1152:31 1:0:768:31 18:0:255:0 1c:0:15:0 35:0:1152:31 36:0:768:31"},
    {"V2_QUERIED",
     {0x04, 0x01, 0x00},
     {0x00, 0x13, 0x0c},
     {0x01, 0x10, 0x64},
     {0x00, 0x00, 0x00},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     2,
     0x040100,
     1088,
     640,
     31,
     31,
     0,
     0x10,
     0,
     "9",
     "e420 30000 0 0 0 0",
     "260800011000003",
     "0:0:1088:31 1:0:640:31 18:0:255:0 1c:0:15:0 35:0:1088:31 36:0:640:31"},
    {"V2_FIXED_DPI",
     {0x14, 0x0a, 0x00},
     {0x00, 0x16, 0x0e},
     {0x00, 0x50, 0x50},
     {0x00, 0x10, 0x00},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     2,
     0x140a00,
     800,
     480,
     31,
     31,
     0,
     0x50,
     0,
     "9",
     "e420 30000 0 0 0 0",
     "260800011000003",
     "0:0:800:31 1:0:480:31 18:0:255:0 1c:0:15:0 35:0:800:31 36:0:480:31"},
    {"V3_STICK",
     {0x35, 0x0f, 0x02},
     {0xb9, 0x15, 0x0c},
     {0x01, 0x22, 0x64},
     {0x25, 0xbe, 0xd0},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     3,
     0x350f02,
     1470,
     720,
     31,
     31,
     0,
     0x22,
     1,
     "9",
     "6420 30000 0 0 0 0",
     "260800011000003",
     "0:0:1470:31 1:0:720:31 18:0:255:0 1c:0:15:0 35:0:1470:31 36:0:720:31"},
    {"V3_CRC",
     {0x45, 0x5f, 0x00},
     {0x10, 0x15, 0x0e},
     {0x01, 0x22, 0x64},
     {0x25, 0xbe, 0xd0},
     {0, 0, 0},
     {0, 0, 0},
     "GIGABYTE",
     "U2442",
     3,
     0x455f00,
     1470,
     720,
     31,
     31,
     0,
     0x22,
     0,
     "d",
     "6420 10000 0 0 0 0",
     "260800011000003",
     "0:0:1470:31 1:0:720:31 18:0:255:0 1c:0:15:0 35:0:1470:31 36:0:720:31"},
    {"V4_CLICKPAD",
     {0x58, 0x1f, 0x01},
     {0x50, 0x17, 0x0d},
     {0x02, 0x0d, 0x64},
     {0x38, 0xa0, 0xfc},
     {0x00, 0x45, 0x00},
     {0, 0, 0},
     0,
     0,
     4,
     0x581f01,
     2208,
     1020,
     33,
     32,
     100,
     0xd,
     0,
     "5",
     "e420 10000 0 0 0 0",
     "661800011000003",
     "0:0:2208:33 1:0:1020:32 18:0:255:0 1c:0:15:0 30:0:1500:0 35:0:2208:33 36:0:1020:32 "
     "3a:0:255:0"},
    {"V4_2A",
     {0x47, 0x0f, 0x01},
     {0x05, 0x12, 0x09},
     {0x01, 0x2a, 0x64},
     {0x34, 0x00, 0x20},
     {0x00, 0x33, 0x00},
     {0, 0, 0},
     0,
     0,
     4,
     0x470f01,
     1024,
     800,
     32,
     32,
     60,
     0x2a,
     0,
     "1",
     "e420 30000 0 0 0 0",
     "661800011000003",
     "0:0:1024:32 1:0:800:32 18:0:255:0 1c:0:15:0 30:0:900:0 35:0:1024:32 36:0:800:32 3a:0:255:0"},
    {"V4_H730",
     {0x57, 0x0f, 0x00},
     {0xc0, 0x14, 0x0c},
     {0x01, 0x0c, 0x64},
     {0x37, 0x80, 0x84},
     {0x00, 0x55, 0x00},
     {0, 0, 0},
     "FUJITSU",
     "CELSIUS H730",
     4,
     0x570f00,
     1920,
     900,
     33,
     33,
     101,
     0xc,
     1,
     "1",
     "e420 70000 0 0 0 0",
     "661800011000003",
     "0:0:1920:33 1:0:900:33 18:0:255:0 1c:0:15:0 30:0:1515:0 35:0:1920:33 36:0:900:33 3a:0:255:0"},
    {"V4_NEW_IC",
     {0x7f, 0x20, 0x01},
     {0x80, 0x1a, 0x0e},
     {0x00, 0x0e, 0x64},
     {0x3a, 0x00, 0xf0},
     {0x00, 0x56, 0x04},
     {0x00, 0x11, 0x02},
     0,
     0,
     4,
     0x7f2001,
     2560,
     1008,
     33,
     33,
     102,
     0xe,
     1,
     "1",
     "e420 70000 0 0 0 0",
     "661800011000003",
     "0:0:2560:33 1:0:1008:33 18:0:255:0 1c:0:15:0 30:0:1530:0 35:0:2560:33 36:0:1008:33 "
     "3a:0:255:0"},
    {"V4_TP500",
     {0x38, 0x1f, 0x17},
     {0x10, 0x14, 0x0e},
     {0x01, 0x0e, 0x64},
     {0x37, 0x80, 0x84},
     {0x00, 0x55, 0x00},
     {0, 0, 0},
     0,
     0,
     4,
     0x381f17,
     1920,
     900,
     33,
     33,
     101,
     0xe,
     0,
     "5",
     "e420 10000 0 0 0 0",
     "661800011000003",
     "0:0:1920:33 1:0:900:33 18:0:255:0 1c:0:15:0 30:0:1515:0 35:0:1920:33 36:0:900:33 3a:0:255:0"},
    {"V3_K53SV",
     {0x45, 0x0f, 0x01},
     {0x78, 0x15, 0x0c},
     {0x01, 0x22, 0x64},
     {0x25, 0xbe, 0xd0},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     3,
     0x450f01,
     1470,
     720,
     31,
     31,
     0,
     0x22,
     0,
     "9",
     "6420 30000 0 0 0 0",
     "260800011000003",
     "0:0:1470:31 1:0:720:31 18:0:255:0 1c:0:15:0 35:0:1470:31 36:0:720:31"},
    {"V3_BROKEN",
     {0x45, 0x0f, 0x00},
     {0x10, 0x15, 0x0e},
     {0x01, 0x74, 0x64},
     {0x25, 0xbe, 0xd0},
     {0, 0, 0},
     {0, 0, 0},
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0,
     0},
};

/* The packets, each with the events Linux 6.12 gives it. */
static const struct step steps[] = {
    {V1_ROCKER,
     PAD,
     {0x29, 0x84, 0x2c, 0xc8},
     0,
     "one finger, the left button down",
     6,
     {{EV_KEY, BTN_TOUCH, 1},
      {EV_ABS, ABS_X, 300},
      {EV_ABS, ABS_Y, 152},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_KEY, BTN_LEFT, 1},
      {EV_SYN, SYN_REPORT, 0}}},
    {V1_ROCKER,
     PAD,
     {0x78, 0x84, 0x36, 0xcd},
     0,
     "the left button up, the rocker up",
     5,
     {{EV_ABS, ABS_X, 310},
      {EV_ABS, ABS_Y, 147},
      {EV_KEY, BTN_LEFT, 0},
      {EV_KEY, BTN_FORWARD, 1},
      {EV_SYN, SYN_REPORT, 0}}},
    {V1_ROCKER,
     PAD,
     {0x8c, 0xa4, 0x40, 0xd2},
     0,
     "three fingers, the rocker down",
     7,
     {{EV_ABS, ABS_X, 320},
      {EV_ABS, ABS_Y, 142},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_KEY, BTN_TOOL_TRIPLETAP, 1},
      {EV_KEY, BTN_FORWARD, 0},
      {EV_KEY, BTN_BACK, 1},
      {EV_SYN, SYN_REPORT, 0}}},
    {V1_ROCKER,
     PAD,
     {0x2c, 0x84, 0x2c, 0xc8},
     1,
     "a packet whose third parity bit is wrong is dropped",
     0,
     {}},
    {V1_ROCKER,
     PAD,
     {0x3c, 0x00, 0x00, 0x00},
     0,
     "the fingers lifted",
     4,
     {{EV_KEY, BTN_TOUCH, 0},
      {EV_KEY, BTN_TOOL_TRIPLETAP, 0},
      {EV_KEY, BTN_BACK, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V1_JUMPY, PAD, {0x58, 0x00, 0x64, 0x64}, 0, "the first report of a finger is dropped", 0, {}},
    {V1_JUMPY, PAD, {0x78, 0x00, 0x65, 0x64}, 0, "and the second", 0, {}},
    {V1_JUMPY,
     PAD,
     {0x7e, 0x00, 0x66, 0x65},
     0,
     "the third is reported, with the right button",
     6,
     {{EV_KEY, BTN_TOUCH, 1},
      {EV_ABS, ABS_X, 102},
      {EV_ABS, ABS_Y, 251},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_KEY, BTN_RIGHT, 1},
      {EV_SYN, SYN_REPORT, 0}}},
    {V1_JUMPY,
     PAD,
     {0xbc, 0x00, 0x96, 0x78},
     0,
     "two fingers",
     6,
     {{EV_ABS, ABS_X, 150},
      {EV_ABS, ABS_Y, 232},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 1},
      {EV_KEY, BTN_RIGHT, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V1_JUMPY,
     PAD,
     {0xac, 0x00, 0x96, 0x78},
     1,
     "a packet whose first parity bit is wrong is dropped",
     0,
     {}},
    {V1_JUMPY,
     PAD,
     {0x3c, 0x00, 0x00, 0x00},
     0,
     "the fingers lifted",
     3,
     {{EV_KEY, BTN_TOUCH, 0}, {EV_KEY, BTN_TOOL_DOUBLETAP, 0}, {EV_SYN, SYN_REPORT, 0}}},
    {V2_OLD,
     PAD,
     {0x7d, 0x01, 0xf4, 0x38, 0x01, 0x2c},
     0,
     "one finger and the left button",
     9,
     {{EV_KEY, BTN_TOUCH, 1},
      {EV_ABS, ABS_X, 500},
      {EV_ABS, ABS_Y, 468},
      {EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 500},
      {EV_ABS, ABS_MT_POSITION_Y, 468},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_KEY, BTN_LEFT, 1},
      {EV_SYN, SYN_REPORT, 0}}},
    {V2_OLD,
     PAD,
     {0x8e, 0x64, 0x3c, 0x08, 0xc8, 0x96},
     0,
     "two fingers at a quarter resolution, the right button",
     13,
     {{EV_ABS, ABS_X, 400},
      {EV_ABS, ABS_Y, 528},
      {EV_ABS, ABS_MT_POSITION_X, 400},
      {EV_ABS, ABS_MT_POSITION_Y, 528},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, 1},
      {EV_ABS, ABS_MT_POSITION_X, 800},
      {EV_ABS, ABS_MT_POSITION_Y, 168},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 1},
      {EV_KEY, BTN_LEFT, 0},
      {EV_KEY, BTN_RIGHT, 1},
      {EV_SYN, SYN_REPORT, 0}}},
    {V2_OLD,
     PAD,
     {0xfc, 0x02, 0x08, 0xb8, 0x01, 0x36},
     0,
     "four fingers",
     12,
     {{EV_ABS, ABS_X, 520},
      {EV_ABS, ABS_Y, 458},
      {EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_POSITION_X, 520},
      {EV_ABS, ABS_MT_POSITION_Y, 458},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_POSITION_X, 0},
      {EV_ABS, ABS_MT_POSITION_Y, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 0},
      {EV_KEY, BTN_TOOL_QUADTAP, 1},
      {EV_KEY, BTN_RIGHT, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V2_OLD,
     PAD,
     {0x84, 0xff, 0xff, 0x02, 0xff, 0xff},
     0,
     "a debounce packet is not reported",
     0,
     {}},
    {V2_OLD,
     PAD,
     {0x7d, 0x01, 0xf4, 0x00, 0x01, 0x2c},
     1,
     "a packet whose constant bits are wrong is dropped",
     0,
     {}},
    {V2_OLD,
     PAD,
     {0x3c, 0x00, 0x00, 0x38, 0x00, 0x00},
     0,
     "the fingers lifted",
     7,
     {{EV_KEY, BTN_TOUCH, 0},
      {EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_TOOL_QUADTAP, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V2_PRESSURE,
     PAD,
     {0x54, 0x52, 0x58, 0x12, 0xa1, 0x90},
     0,
     "one finger with its pressure and width",
     10,
     {{EV_KEY, BTN_TOUCH, 1},
      {EV_ABS, ABS_X, 600},
      {EV_ABS, ABS_Y, 368},
      {EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 600},
      {EV_ABS, ABS_MT_POSITION_Y, 368},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_ABS, ABS_PRESSURE, 90},
      {EV_ABS, ABS_TOOL_WIDTH, 5},
      {EV_SYN, SYN_REPORT, 0}}},
    {V2_PRESSURE,
     PAD,
     {0x94, 0x2c, 0xc8, 0x12, 0x90, 0xfa},
     0,
     "two fingers",
     13,
     {{EV_ABS, ABS_X, 1200},
      {EV_ABS, ABS_Y, -32},
      {EV_ABS, ABS_MT_POSITION_X, 1200},
      {EV_ABS, ABS_MT_POSITION_Y, -32},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, 1},
      {EV_ABS, ABS_MT_POSITION_X, 1600},
      {EV_ABS, ABS_MT_POSITION_Y, -232},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 1},
      {EV_ABS, ABS_PRESSURE, 127},
      {EV_ABS, ABS_TOOL_WIDTH, 7},
      {EV_SYN, SYN_REPORT, 0}}},
    {V2_PRESSURE,
     PAD,
     {0xd4, 0x52, 0x62, 0x12, 0xa1, 0x90},
     0,
     "three fingers",
     13,
     {{EV_ABS, ABS_X, 610},
      {EV_ABS, ABS_Y, 368},
      {EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_POSITION_X, 610},
      {EV_ABS, ABS_MT_POSITION_Y, 368},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_POSITION_X, 0},
      {EV_ABS, ABS_MT_POSITION_Y, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 0},
      {EV_KEY, BTN_TOOL_TRIPLETAP, 1},
      {EV_ABS, ABS_PRESSURE, 90},
      {EV_ABS, ABS_TOOL_WIDTH, 5},
      {EV_SYN, SYN_REPORT, 0}}},
    {V2_PRESSURE,
     PAD,
     {0x44, 0x52, 0x58, 0x08, 0xa1, 0x90},
     1,
     "a packet whose constant bits are wrong is dropped",
     0,
     {}},
    {V2_PRESSURE,
     PAD,
     {0x04, 0x00, 0x00, 0x02, 0x00, 0x00},
     0,
     "the fingers lifted",
     9,
     {{EV_KEY, BTN_TOUCH, 0},
      {EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_TOOL_TRIPLETAP, 0},
      {EV_ABS, ABS_PRESSURE, 0},
      {EV_ABS, ABS_TOOL_WIDTH, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_STICK,
     PAD,
     {0x55, 0x42, 0xbc, 0x22, 0x61, 0x2c},
     0,
     "one finger and the left button",
     11,
     {{EV_KEY, BTN_TOUCH, 1},
      {EV_ABS, ABS_X, 700},
      {EV_ABS, ABS_Y, 420},
      {EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 700},
      {EV_ABS, ABS_MT_POSITION_Y, 420},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_KEY, BTN_LEFT, 1},
      {EV_ABS, ABS_PRESSURE, 70},
      {EV_ABS, ABS_TOOL_WIDTH, 6},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_STICK,
     PAD,
     {0x84, 0x32, 0x8a, 0x02, 0x21, 0x18},
     0,
     "the head of two fingers is kept",
     0,
     {}},
    {V3_STICK,
     PAD,
     {0x8c, 0x33, 0x84, 0x0c, 0x21, 0x5e},
     0,
     "the tail reports both",
     14,
     {{EV_ABS, ABS_X, 650},
      {EV_ABS, ABS_Y, 440},
      {EV_ABS, ABS_MT_POSITION_X, 650},
      {EV_ABS, ABS_MT_POSITION_Y, 440},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, 1},
      {EV_ABS, ABS_MT_POSITION_X, 900},
      {EV_ABS, ABS_MT_POSITION_Y, 370},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 1},
      {EV_KEY, BTN_LEFT, 0},
      {EV_ABS, ABS_PRESSURE, 50},
      {EV_ABS, ABS_TOOL_WIDTH, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_STICK,
     STICK,
     {0x21, 0x80, 0x00, 0x16, 0x05, 0xfd},
     0,
     "a stick's packet: the left button, 5 right and 3 down",
     4,
     {{EV_KEY, BTN_LEFT, 1}, {EV_REL, REL_X, 5}, {EV_REL, REL_Y, 3}, {EV_SYN, SYN_REPORT, 0}}},
    {V3_STICK,
     STICK,
     {0x14, 0x00, 0x80, 0x26, 0xfe, 0x07},
     0,
     "the middle button, 2 left and 7 up",
     5,
     {{EV_KEY, BTN_LEFT, 0},
      {EV_KEY, BTN_MIDDLE, 1},
      {EV_REL, REL_X, -2},
      {EV_REL, REL_Y, -7},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_STICK, STICK, {0x14, 0x00, 0x80, 0x26, 0x80, 0x07}, 0, "a move of -128 is dropped", 0, {}},
    {V3_STICK,
     STICK,
     {0x10, 0x00, 0x00, 0x16, 0x05, 0xfd},
     0,
     "a stick's packet of no known form is dropped",
     0,
     {}},
    {V3_STICK,
     PAD,
     {0xc4, 0xff, 0xff, 0x02, 0xff, 0xff},
     0,
     "a debounce packet is not reported",
     0,
     {}},
    {V3_STICK,
     PAD,
     {0x48, 0x02, 0xbc, 0x02, 0x01, 0x2c},
     1,
     "a packet of no known form is dropped",
     0,
     {}},
    {V3_STICK,
     PAD,
     {0x04, 0x00, 0x00, 0x02, 0x00, 0x00},
     0,
     "the fingers lifted",
     8,
     {{EV_KEY, BTN_TOUCH, 0},
      {EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 0},
      {EV_ABS, ABS_PRESSURE, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_CRC,
     PAD,
     {0x43, 0x21, 0x50, 0x08, 0xa0, 0xc8},
     0,
     "one finger, both buttons as the left",
     10,
     {{EV_KEY, BTN_TOUCH, 1},
      {EV_ABS, ABS_X, 336},
      {EV_ABS, ABS_Y, 520},
      {EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 336},
      {EV_ABS, ABS_MT_POSITION_Y, 520},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_KEY, BTN_LEFT, 1},
      {EV_ABS, ABS_PRESSURE, 42},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_CRC,
     PAD,
     {0x80, 0x21, 0x54, 0x08, 0xa0, 0xd2},
     0,
     "the head of two fingers is kept",
     0,
     {}},
    {V3_CRC,
     PAD,
     {0x80, 0x21, 0xf4, 0x09, 0xa1, 0x04},
     0,
     "the tail reports both",
     12,
     {{EV_ABS, ABS_X, 340},
      {EV_ABS, ABS_Y, 510},
      {EV_ABS, ABS_MT_POSITION_X, 340},
      {EV_ABS, ABS_MT_POSITION_Y, 510},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, 1},
      {EV_ABS, ABS_MT_POSITION_X, 500},
      {EV_ABS, ABS_MT_POSITION_Y, 460},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 1},
      {EV_KEY, BTN_LEFT, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_CRC,
     PAD,
     {0x21, 0x80, 0x00, 0x16, 0x05, 0xfd},
     1,
     "a stick's form is no packet of a CRC pad",
     0,
     {}},
    {V3_CRC,
     PAD,
     {0x00, 0x00, 0x00, 0x08, 0x00, 0x00},
     0,
     "the fingers lifted",
     8,
     {{EV_KEY, BTN_TOUCH, 0},
      {EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 0},
      {EV_ABS, ABS_PRESSURE, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_CLICKPAD,
     PAD,
     {0x00, 0x01, 0x00, 0x10, 0x00, 0x00},
     0,
     "a status packet before any head reports nothing",
     0,
     {}},
    {V4_CLICKPAD,
     PAD,
     {0x30, 0x43, 0xe8, 0x31, 0x81, 0xf4},
     0,
     "the head of finger 1",
     12,
     {{EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 1000},
      {EV_ABS, ABS_MT_POSITION_Y, 520},
      {EV_ABS, ABS_MT_PRESSURE, 72},
      {EV_ABS, ABS_MT_TOUCH_MAJOR, 300},
      {EV_ABS, ABS_TOOL_WIDTH, 3},
      {EV_KEY, BTN_TOUCH, 1},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_ABS, ABS_X, 1000},
      {EV_ABS, ABS_Y, 520},
      {EV_ABS, ABS_PRESSURE, 72},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_CLICKPAD,
     PAD,
     {0x30, 0x03, 0xfe, 0x12, 0x00, 0x00},
     0,
     "finger 1 moves, five times its bytes",
     5,
     {{EV_ABS, ABS_MT_POSITION_X, 1015},
      {EV_ABS, ABS_MT_POSITION_Y, 530},
      {EV_ABS, ABS_X, 1015},
      {EV_ABS, ABS_Y, 530},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_CLICKPAD,
     PAD,
     {0x21, 0x35, 0xdc, 0x71, 0x02, 0x58},
     0,
     "the head of finger 3, and a click",
     11,
     {{EV_ABS, ABS_MT_SLOT, 2},
      {EV_ABS, ABS_MT_TRACKING_ID, 1},
      {EV_ABS, ABS_MT_POSITION_X, 1500},
      {EV_ABS, ABS_MT_POSITION_Y, 420},
      {EV_ABS, ABS_MT_PRESSURE, 48},
      {EV_ABS, ABS_MT_TOUCH_MAJOR, 200},
      {EV_ABS, ABS_TOOL_WIDTH, 2},
      {EV_KEY, BTN_LEFT, 1},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 1},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_CLICKPAD,
     PAD,
     {0x21, 0x01, 0x01, 0x72, 0xfd, 0x02},
     0,
     "fingers 1 and 3 move",
     9,
     {{EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_POSITION_X, 1016},
      {EV_ABS, ABS_MT_POSITION_Y, 529},
      {EV_ABS, ABS_MT_SLOT, 2},
      {EV_ABS, ABS_MT_POSITION_X, 1497},
      {EV_ABS, ABS_MT_POSITION_Y, 418},
      {EV_ABS, ABS_X, 1016},
      {EV_ABS, ABS_Y, 529},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_CLICKPAD,
     PAD,
     {0x00, 0x04, 0x00, 0x10, 0x00, 0x00},
     0,
     "finger 1 lifted, the click let go",
     9,
     {{EV_ABS, ABS_MT_SLOT, 0},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_LEFT, 0},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_KEY, BTN_TOOL_DOUBLETAP, 0},
      {EV_ABS, ABS_X, 1497},
      {EV_ABS, ABS_Y, 418},
      {EV_ABS, ABS_PRESSURE, 48},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_CLICKPAD,
     PAD,
     {0x00, 0x00, 0x00, 0x10, 0x00, 0x00},
     0,
     "finger 3 lifted",
     6,
     {{EV_ABS, ABS_MT_SLOT, 2},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_TOUCH, 0},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_ABS, ABS_PRESSURE, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_CLICKPAD,
     PAD,
     {0x38, 0x43, 0xe8, 0x31, 0x81, 0xf4},
     1,
     "a packet with bit 3 of its first byte set is dropped",
     0,
     {}},
    {V4_CLICKPAD,
     PAD,
     {0xc0, 0x01, 0x01, 0x12, 0x00, 0x00},
     0,
     "a motion packet of finger 6 is not reported",
     0,
     {}},
    {V4_2A,
     PAD,
     {0x4a, 0x53, 0x20, 0x51, 0x01, 0x90},
     0,
     "a head with bit 3 of its first byte set, and the right button",
     14,
     {{EV_ABS, ABS_MT_SLOT, 1},
      {EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 800},
      {EV_ABS, ABS_MT_POSITION_Y, 400},
      {EV_ABS, ABS_MT_PRESSURE, 80},
      {EV_ABS, ABS_MT_TOUCH_MAJOR, 240},
      {EV_ABS, ABS_TOOL_WIDTH, 4},
      {EV_KEY, BTN_RIGHT, 1},
      {EV_KEY, BTN_TOUCH, 1},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_ABS, ABS_X, 800},
      {EV_ABS, ABS_Y, 400},
      {EV_ABS, ABS_PRESSURE, 80},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_2A,
     PAD,
     {0x44, 0xfc, 0x04, 0x12, 0x00, 0x00},
     0,
     "a motion packet, the middle button",
     6,
     {{EV_ABS, ABS_MT_POSITION_X, 796},
      {EV_ABS, ABS_MT_POSITION_Y, 396},
      {EV_KEY, BTN_RIGHT, 0},
      {EV_ABS, ABS_X, 796},
      {EV_ABS, ABS_Y, 396},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_2A,
     PAD,
     {0x00, 0x00, 0x00, 0x14, 0x00, 0x00},
     1,
     "a packet with bit 2 of its fourth byte set is dropped",
     0,
     {}},
    {V4_2A,
     PAD,
     {0x00, 0x00, 0x00, 0x10, 0x00, 0x00},
     0,
     "the finger lifted",
     5,
     {{EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_TOUCH, 0},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_ABS, ABS_PRESSURE, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_H730,
     PAD,
     {0x24, 0x42, 0xbc, 0x21, 0x01, 0x2c},
     0,
     "a head of the CRC form and the middle button",
     13,
     {{EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 700},
      {EV_ABS, ABS_MT_POSITION_Y, 600},
      {EV_ABS, ABS_MT_PRESSURE, 64},
      {EV_ABS, ABS_MT_TOUCH_MAJOR, 202},
      {EV_ABS, ABS_TOOL_WIDTH, 2},
      {EV_KEY, BTN_MIDDLE, 1},
      {EV_KEY, BTN_TOUCH, 1},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_ABS, ABS_X, 700},
      {EV_ABS, ABS_Y, 600},
      {EV_ABS, ABS_PRESSURE, 64},
      {EV_SYN, SYN_REPORT, 0}}},
    {V4_H730,
     STICK,
     {0x01, 0x80, 0x80, 0x36, 0x06, 0x02},
     0,
     "a stick's packet: the left button, 6 right and 2 up",
     4,
     {{EV_KEY, BTN_LEFT, 1}, {EV_REL, REL_X, 6}, {EV_REL, REL_Y, -2}, {EV_SYN, SYN_REPORT, 0}}},
    {V4_H730,
     PAD,
     {0x20, 0x42, 0xbc, 0x29, 0x01, 0x2c},
     1,
     "a packet with bit 3 of its fourth byte set is dropped",
     0,
     {}},
    {V4_H730,
     PAD,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     0,
     "the finger lifted",
     6,
     {{EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_MIDDLE, 0},
      {EV_KEY, BTN_TOUCH, 0},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_ABS, ABS_PRESSURE, 0},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_K53SV,
     PAD,
     {0x54, 0x42, 0xbc, 0x22, 0x61, 0x2c},
     0,
     "one finger",
     10,
     {{EV_KEY, BTN_TOUCH, 1},
      {EV_ABS, ABS_X, 700},
      {EV_ABS, ABS_Y, 420},
      {EV_ABS, ABS_MT_TRACKING_ID, 0},
      {EV_ABS, ABS_MT_POSITION_X, 700},
      {EV_ABS, ABS_MT_POSITION_Y, 420},
      {EV_KEY, BTN_TOOL_FINGER, 1},
      {EV_ABS, ABS_PRESSURE, 70},
      {EV_ABS, ABS_TOOL_WIDTH, 6},
      {EV_SYN, SYN_REPORT, 0}}},
    {V3_K53SV,
     PAD,
     {0x21, 0x80, 0x00, 0x16, 0x05, 0xfd},
     0,
     "a stick's packet on a pad with no stick is not reported",
     0,
     {}},
    {V3_K53SV,
     PAD,
     {0x04, 0x00, 0x00, 0x02, 0x00, 0x00},
     0,
     "the finger lifted",
     6,
     {{EV_KEY, BTN_TOUCH, 0},
      {EV_ABS, ABS_MT_TRACKING_ID, -1},
      {EV_KEY, BTN_TOOL_FINGER, 0},
      {EV_ABS, ABS_PRESSURE, 0},
      {EV_ABS, ABS_TOOL_WIDTH, 0},
      {EV_SYN, SYN_REPORT, 0}}},
};

/* The model 8042's output buffer, a queue of bytes the device sent. */
#define MODEL_BYTES 64
static uint8_t model_data[MODEL_BYTES];
static int model_head;
static int model_count;

/* The controller's configuration byte, and what the next write to port
 * 0x60 is for: 0, or the command that named it. */
static uint8_t ctl_config;
static uint8_t ctl_next;

/* What the auxiliary device is. */
#define KIND_PAD      1 /* the Elantech pad of `pro` */
#define KIND_LOGITECH 2 /* a mouse that answers the knock and is no pad */

/*
 * The device: its kind and pad; the command whose argument comes next, 0
 * for none; the byte the resolutions since the last 1:1 scaling make, and
 * their count; the sliced bytes made since the last other command; the
 * bytes after each 0xF8 since then; whether the next byte is one of them;
 * whether a read left unanswered makes the next 0xF8 start a sequence
 * again; the 1:1 scalings in a row; its registers and register 0x07's second
 * byte; its resolution, rate and whether it reports; and every byte it
 * took. A fault: the register whose write it leaves unanswered, -1 for
 * none; the status requests of register reads it leaves unanswered; and
 * whether its register 0x10 never holds the absolute bit.
 */
static int dev_kind;
static const struct profile *pro;
static uint8_t arg_for;
static uint8_t special;
static int res_count;
static uint8_t sliced_seq[4];
static int n_sliced;
static uint8_t custom_seq[4];
static int n_custom;
static int custom_next;
static int stale;
static int scales;
static uint8_t regs[0x30];
static uint8_t reg07_hi;
static uint8_t dev_res;
static uint8_t dev_rate;
static int reporting;
static int refuse_reg;
static int drop_reads;
static int no_abs_bit;
#define TOOK_MAX 1024
static uint8_t took[TOOK_MAX];
static int n_took;

/* Interrupts as `arch_irq_disable` and `arch_irq_enable` leave them, the
 * port accesses made while they were on, the handler registered, the
 * warnings logged, and the waits asked of `k_udelay`. */
static int irq_on = 1;
static int ports_with_irq_on;
static isr_handler_t registered_handler;
static int warnings;
static int debug_messages;
static const char *last_warning;
static const char *last_info;
static int waits;
static uint32_t waited_us;
static int waits_irq_on; /* those made with interrupts on */

/** Put `data` in the output buffer behind what waits there. */
static void model_put(uint8_t data)
{
    if (model_count == 0) {
        model_head = 0;
    }
    if (model_head + model_count >= MODEL_BYTES) {
        printf("elantech-test: fail: the model's output buffer overflowed\n");
        fails++;
        return;
    }
    model_data[model_head + model_count] = data;
    model_count++;
}

/** Put the three bytes at `p` in the output buffer. */
static void put3(const uint8_t *p)
{
    model_put(p[0]);
    model_put(p[1]);
    model_put(p[2]);
}

/** Answer the query `q` from the pad's profile, or as a Logitech mouse
 * answers the firmware's query: with its status and a rate of 100. */
static void answer_query(uint8_t q)
{
    static const uint8_t zero[3] = {0, 0, 0};
    static const uint8_t logitech[3] = {0x10, 0x03, 100};

    if (dev_kind == KIND_LOGITECH) {
        put3(logitech);
        return;
    }
    switch (q) {
    case 0x00:
        put3(pro->fw_id);
        break;
    case 0x01:
        put3(pro->fw);
        break;
    case 0x02:
        put3(pro->caps);
        break;
    case 0x03:
        put3(pro->samples);
        break;
    case 0x04:
        put3(pro->res);
        break;
    case 0x05:
        put3(pro->icbody);
        break;
    default:
        put3(zero);
        break;
    }
}

/** Answer a read of register `reg`: its value in the first byte, or on
 * version 4 in the second; register 0x07 of a version 4 pad gives its two
 * bytes. */
static void answer_reg(uint8_t reg)
{
    uint8_t r[3] = {0, 0, 0};
    uint8_t val = reg < sizeof(regs) ? regs[reg] : 0;

    if (reg == 0x10 && no_abs_bit) {
        val &= (uint8_t)~0x04u;
    }
    if (pro->hw == 4) {
        r[0] = reg == 0x07 ? val : 0;
        r[1] = reg == 0x07 ? reg07_hi : val;
    } else {
        r[0] = val;
    }
    put3(r);
}

/** The status request: the magic knock's reply after three 1:1 scalings,
 * a register's or a query's after their sequences, or the mouse's status.
 * Returns -1 for a register read the model leaves unanswered. */
static int answer_status(void)
{
    static const uint8_t magic[3] = {0x3c, 0x03, 0xc8};
    int read_code = pro && pro->hw == 2 ? 0x10 : 0x00;
    uint8_t status[3];

    if (n_custom == 2 && custom_seq[0] == read_code) {
        if (drop_reads > 0) {
            drop_reads--;
            stale = 1;
            return -1;
        }
        model_put(0xFA);
        answer_reg(custom_seq[1]);
    } else if (n_sliced == 2 && sliced_seq[0] == 0x10) {
        model_put(0xFA);
        answer_reg(sliced_seq[1]);
    } else if (n_custom == 1) {
        model_put(0xFA);
        answer_query(custom_seq[0]);
    } else if (n_sliced == 1 && n_custom == 0) {
        model_put(0xFA);
        answer_query(sliced_seq[0]);
    } else if (scales == 3) {
        model_put(0xFA);
        put3(magic);
    } else {
        model_put(0xFA);
        status[0] = reporting ? 0x20 : 0x00;
        status[1] = dev_res;
        status[2] = dev_rate;
        put3(status);
    }
    return 0;
}

/** Write `val` to register `reg`, unless the row refuses it; -1 then. */
static int write_reg(uint8_t reg, uint8_t val)
{
    if (reg == refuse_reg) {
        return -1;
    }
    if (reg < sizeof(regs)) {
        regs[reg] = val;
    }
    return 0;
}

/** The 1:1 scaling that ends a register write of the pad's version, or
 * Linux's `elantech_write_reg_params` of register 0x07; -1 for a write
 * the model leaves unanswered. */
static int end_write(void)
{
    int code = pro && pro->hw == 2 ? 0x11 : 0x00;

    if (n_sliced == 3 && sliced_seq[0] == 0x11) {
        return write_reg(sliced_seq[1], sliced_seq[2]);
    }
    if (n_custom == 3 && custom_seq[0] == code && pro->hw <= 3) {
        return write_reg(custom_seq[1], custom_seq[2]);
    }
    if (n_custom == 4 && custom_seq[0] == 0x00 && pro->hw == 4) {
        if (custom_seq[2] == 0x00) {
            return write_reg(custom_seq[1], custom_seq[3]);
        }
        reg07_hi = custom_seq[3]; /* register 0x07's two bytes */
        return write_reg(custom_seq[1], custom_seq[2]);
    }
    return 0;
}

/** Forget the sequences of a register access or a query. */
static void end_sequences(void)
{
    n_sliced = 0;
    n_custom = 0;
    res_count = 0;
    special = 0;
    stale = 0;
}

/** Take `b`, a byte the controller sent on to the device. */
static void dev_take(uint8_t b)
{
    uint8_t cmd;

    if (n_took < TOOK_MAX) {
        took[n_took] = b;
    }
    n_took++;
    if ((ctl_config & 0x20) != 0) {
        return; /* its clock is off */
    }
    if (custom_next) {
        custom_next = 0;
        if (n_custom < 4) {
            custom_seq[n_custom++] = b;
        }
        model_put(0xFA);
        return;
    }
    if (arg_for) {
        cmd = arg_for;
        arg_for = 0;
        model_put(0xFA);
        if (cmd == 0xE8) {
            special = (uint8_t)(special << 2 | (b & 3u));
            dev_res = b;
            if (++res_count == 4) {
                if (n_sliced < 4) {
                    sliced_seq[n_sliced++] = special;
                }
                res_count = 0;
                special = 0;
            }
        } else {
            dev_rate = b;
        }
        return;
    }
    if (b == 0xE9) {
        if (answer_status() == 0) {
            end_sequences();
            scales = 0;
        }
        return;
    }
    if (b == 0xE6) {
        if (end_write() < 0) {
            return; /* refused: no answer, and the sequence stands */
        }
        model_put(0xFA);
        if (n_sliced == 3 || n_custom >= 3) {
            end_sequences();
        }
        res_count = 0;
        special = 0;
        scales++;
        return;
    }
    model_put(0xFA);
    scales = 0;
    switch (b) {
    case 0xE8:
    case 0xF3:
        arg_for = b;
        return;
    case 0xF8:
        if (stale) {
            end_sequences();
        }
        custom_next = 1;
        return;
    case 0xF2:
        model_put(0x00);
        break;
    case 0xFF:
        model_put(0xAA);
        model_put(0x00);
        k_memset(regs, 0, sizeof(regs));
        reg07_hi = 0;
        reporting = 0;
        dev_res = 2;
        dev_rate = 100;
        break;
    case 0xF4:
        reporting = 1;
        break;
    case 0xF5:
    case 0xF6:
        reporting = 0;
        break;
    default:
        break;
    }
    end_sequences();
}

uint8_t inb(uint16_t port)
{
    uint8_t data;

    if (irq_on) {
        ports_with_irq_on++;
    }
    if (port == PS2_STATUS_PORT) {
        return model_count > 0 ? MOUSE_BYTE : 0;
    }
    if (port == PS2_DATA_PORT && model_count > 0) {
        data = model_data[model_head];
        model_head++;
        model_count--;
        return data;
    }
    return 0;
}

void outb(uint16_t port, uint8_t val)
{
    uint8_t next = ctl_next;

    if (irq_on) {
        ports_with_irq_on++;
    }
    if (port == PS2_CMD_PORT) {
        ctl_next = 0;
        if (val == PS2_CMD_ENABLE_MOUSE) {
            ctl_config &= ~0x20;
        } else if (val == PS2_CMD_READ_CONFIG) {
            model_put(ctl_config);
        } else if (val == PS2_CMD_WRITE_CONFIG || val == PS2_CMD_WRITE_MOUSE) {
            ctl_next = val;
        }
    } else if (port == PS2_DATA_PORT) {
        ctl_next = 0;
        if (next == PS2_CMD_WRITE_CONFIG) {
            ctl_config = val;
        } else if (next == PS2_CMD_WRITE_MOUSE) {
            dev_take(val);
        }
    }
}

void arch_irq_disable(void)
{
    irq_on = 0;
}

void arch_irq_enable(void)
{
    irq_on = 1;
}

/** The keyboard, which a failed controller gives up; no row here has one. */
void kbd_stop(void)
{}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)name;
    registered_handler = handler;
}

/** Count the waits and their time; no time passes. */
void k_udelay(uint32_t usec)
{
    waits++;
    waited_us += usec;
    if (irq_on) {
        waits_irq_on++;
    }
}

/** No sysfs tree is published by this model. */
void evdev_sysfs_serio_remove(int port)
{
    (void)port;
}

/** This model has no competing command caller. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

/* What the guest of a Synaptics pad's pass-through port needs, which an
 * Elantech pad never calls; trackpoint_hosttest.c tests it. */
void evdev_sysfs_serio_add(int port)
{
    (void)port;
}

void evdev_sysfs_serio_files(int port, int (*add)(int dir))
{
    (void)port;
    (void)add;
}

void evdev_sysfs_serio_driver(int port, int (*add)(int dir))
{
    (void)port;
    (void)add;
}

int evdev_sysfs_serio_manual(int port)
{
    (void)port;
    return 0;
}

int alps_poll(uint8_t badbyte, uint8_t *buf)
{
    (void)badbyte;
    (void)buf;
    return -1;
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)attr;
    (void)obj;
    (void)arg;
    return 0;
}

/** Nothing is mapped: the SMBIOS table is always one a row gives. */
uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    CHECK("map_mmio is not asked when the SMBIOS table is given", 0);
    return 0;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    if (level == KLOG_WARN) {
        warnings++;
        last_warning = format;
    } else if (level == KLOG_INFO) {
        last_info = format;
    } else if (level == KLOG_DEBUG) {
        debug_messages++;
    }
}

/** The console's blanking is not modelled. */
void console_poke(void)
{}

/** No ALPS touchpad answers, so the probe goes on to Elantech's; the ALPS
 * probe is alps_hosttest.c's. */
int alps_init(void)
{
    return 0;
}

/** No ALPS pad's bytes come, since `alps_init` found none. */
int alps_byte(uint8_t data)
{
    (void)data;
    return ALPS_GOOD;
}

/* The rest of alps.h, which a port without an ALPS pad never calls;
 * alps_hosttest.c tests it. */
void alps_packet_restart(void)
{}

int alps_reset_after(void)
{
    return 12;
}

int alps_packet_start(void)
{
    return 1;
}

int alps_reconnect(void)
{
    return -1;
}

void alps_disconnect(void)
{}

/** No ALPS pad's set-up fails, so no mouse takes its name. */
const char *alps_mouse_name(const char *proto)
{
    return proto;
}

/* What evdev.c needs: one processor, fixed clocks, nothing to repeat,
 * reads that never sleep, since every open is nonblocking, and calls of
 * x86-64 programs. */

/** Every call here is an x86-64 program's. */
int in_compat_syscall(void)
{
    return 0;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

void input_get_repeat(int *delay_ms, int *period_ms)
{
    *delay_ms = 250;
    *period_ms = 33;
}

/** No keyboard here takes its keys through input.c. */
int input_keyboard_release(struct evdev *dev, unsigned int keycode)
{
    (void)dev;
    (void)keycode;
    return 0;
}

void input_set_repeat(int delay_ms, int period_ms)
{
    (void)delay_ms;
    (void)period_ms;
}

/** No device of a row has keys the console takes. */
void input_console_key(const void *device, unsigned int keycode, int value)
{
    (void)device;
    (void)keycode;
    (void)value;
}

/** No row writes to an event device. */
void input_written(uint16_t type, uint16_t code, int32_t value)
{
    (void)type;
    (void)code;
    (void)value;
}

void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    return cond(arg) ? 0 : -EINTR;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    k_memcpy((void *)dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/* The ioctls, by Linux's macros: _IOC(dir, 'E', nr, size). */
#define IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ('E' << 8) | (nr))
#define EVIOCGID           0x80084502u
#define EVIOCGNAME(n)      IOC(2, 0x06, n)
#define EVIOCGPHYS(n)      IOC(2, 0x07, n)
#define EVIOCGPROP(n)      IOC(2, 0x09, n)
#define EVIOCGBIT(t, n)    IOC(2, 0x20 + (t), n)
#define EVIOCGABS(a)       IOC(2, 0x40 + (a), sizeof(struct input_absinfo))

/* An evdev record as the host's C library lays it out, 24 bytes. */
struct record {
    unsigned long sec;
    unsigned long usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
};

/* The opens of the pad's and the stick's event devices. */
static fd_entry_t open_e[2];
static struct open_file open_ofd[2];
static int is_open[2];

/* The records the last `take` read. */
static struct record got[64];
static int n_got;

/** Open the node whose name is `name`, nonblocking, as the open `which`.
 * Returns 0, or -1 when no node has that name. */
static int open_named(int which, const char *name)
{
    char text[64];
    int n;

    for (n = 0; n < EVDEV_MAX; n++) {
        k_memset(&open_e[which], 0, sizeof(open_e[which]));
        k_memset(&open_ofd[which], 0, sizeof(open_ofd[which]));
        open_ofd[which].status_flags = OFD_NONBLOCK;
        open_e[which].ofd = &open_ofd[which];
        if (evdev_open(&open_e[which], n) != 0) {
            continue;
        }
        if (evdev_ioctl(&open_e[which], EVIOCGNAME(64), (uintptr_t)text) > 0 &&
            strcmp(text, name) == 0) {
            is_open[which] = 1;
            return 0;
        }
        evdev_close(&open_e[which]);
    }
    return -1;
}

/** How many nodes are named `name`; every node it opens it closes. */
static int count_named(const char *name)
{
    fd_entry_t e;
    struct open_file ofd;
    char text[64];
    int n, count = 0;

    for (n = 0; n < EVDEV_MAX; n++) {
        k_memset(&e, 0, sizeof(e));
        k_memset(&ofd, 0, sizeof(ofd));
        ofd.status_flags = OFD_NONBLOCK;
        e.ofd = &ofd;
        if (evdev_open(&e, n) != 0) {
            continue;
        }
        if (evdev_ioctl(&e, EVIOCGNAME(64), (uintptr_t)text) > 0 && strcmp(text, name) == 0) {
            count++;
        }
        evdev_close(&e);
    }
    return count;
}

/** Read every whole packet of the open `which` into `got`. */
static int32_t take(int which)
{
    int32_t rc = evdev_read(&open_e[which], (char __user *)got, sizeof(got));

    n_got = rc > 0 ? rc / (int32_t)sizeof(got[0]) : 0;
    return rc;
}

/** Whether `got` holds the `n` events of `want` and nothing else. */
static int got_events(const struct event *want, int n)
{
    int i;

    if (n_got != n) {
        printf("elantech-test: read %d records, not %d\n", n_got, n);
        for (i = 0; i < n_got; i++) {
            printf("elantech-test:   %u 0x%x %d\n", got[i].type, got[i].code, (int)got[i].value);
        }
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (got[i].type != want[i].type || got[i].code != want[i].code ||
            got[i].value != want[i].value) {
            printf("elantech-test: record %d is %u 0x%x %d, not %u 0x%x %d\n", i, got[i].type,
                   got[i].code, (int)got[i].value, want[i].type, want[i].code, (int)want[i].value);
            return 0;
        }
    }
    return 1;
}

/* The SMBIOS tables of the rows' machines: none named, and the two that
 * Linux's DMI tables name, a System Information structure each. */
static const uint8_t no_system[] = {127, 4, 0, 0, 0, 0};
static const uint8_t fujitsu_h730[] = {1,   6,   0,   0,   1,   2,   'F', 'U', 'J', 'I', 'T', 'S',
                                       'U', 0,   'C', 'E', 'L', 'S', 'I', 'U', 'S', ' ', 'H', '7',
                                       '3', '0', 0,   0,   127, 4,   0,   0,   0,   0};
static const uint8_t gigabyte_u2442[] = {1,   6,   0,   0,   1, 2,   'G', 'I', 'G', 'A',
                                         'B', 'Y', 'T', 'E', 0, 'U', '2', '4', '4', '2',
                                         0,   0,   127, 4,   0, 0,   0,   0};

/** Take away what the last row registered, and put the controller and the
 * device as the firmware leaves them, a device of `kind` with the pad
 * `p`, its machine's SMBIOS table, and no fault. */
static void model_reset(int kind, int p)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (is_open[i]) {
            evdev_close(&open_e[i]);
            is_open[i] = 0;
        }
    }
    evdev_unregister(etd_pad);
    etd_pad = NULL;
    evdev_unregister(etd_tp);
    etd_tp = NULL;
    evdev_unregister(mouse_dev);
    mouse_dev = NULL;
    evdev_unregister(pad);
    pad = NULL;
    touchpad = 0;
    state = STATE_ACTIVE;
    bad_in_a_row = 0;
    bat_held = 0;
    reconnect_wanted = 0;
    cmd_busy = 0;
    packet_index = 0;
    model_head = 0;
    model_count = 0;
    ctl_config = FIRMWARE_CONFIG;
    ctl_next = 0;
    dev_kind = kind;
    pro = &profiles[p];
    if (pro->maker && strcmp(pro->maker, "FUJITSU") == 0) {
        smbios_set_table(fujitsu_h730, sizeof(fujitsu_h730));
    } else if (pro->maker && strcmp(pro->maker, "GIGABYTE") == 0) {
        smbios_set_table(gigabyte_u2442, sizeof(gigabyte_u2442));
    } else {
        smbios_set_table(no_system, sizeof(no_system));
    }
    arg_for = 0;
    special = 0;
    res_count = 0;
    n_sliced = 0;
    n_custom = 0;
    custom_next = 0;
    stale = 0;
    scales = 0;
    k_memset(regs, 0, sizeof(regs));
    reg07_hi = 0;
    dev_res = 2;
    dev_rate = 100;
    reporting = 0;
    refuse_reg = -1;
    drop_reads = 0;
    no_abs_bit = 0;
    n_took = 0;
    irq_on = 1;
    ports_with_irq_on = 0;
    registered_handler = 0;
    warnings = 0;
    last_warning = 0;
    last_info = 0;
    waits = 0;
    waited_us = 0;
    waits_irq_on = 0;
    etd_pktcnt = 0;
    etd_sync_losses = 0;
    k_memset(etd_mt, 0, sizeof(etd_mt));
}

/* The device's bytes a row expects, built with the `expect_*` calls. */
static uint8_t want_took[TOOK_MAX];
static int n_want;

/** Expect the byte `b` sent to the device. */
static void expect(uint8_t b)
{
    if (n_want < TOOK_MAX) {
        want_took[n_want] = b;
    }
    n_want++;
}

/** Expect `c` sliced, Linux's `ps2_sliced_command`. */
static void expect_sliced(uint8_t c)
{
    int i;

    expect(0xE6);
    for (i = 6; i >= 0; i -= 2) {
        expect(0xE8);
        expect((uint8_t)((c >> i) & 3u));
    }
}

/** Expect the query `c` as a Synaptics sliced query. */
static void expect_squery(uint8_t c)
{
    expect_sliced(c);
    expect(0xE9);
}

/** Expect the query `c` of versions 3 and 4, after 0xF8. */
static void expect_equery(uint8_t c)
{
    expect(0xF8);
    expect(c);
    expect(0xE9);
}

/** Expect each byte of the `n` at `seq` after 0xF8, then `end`. */
static void expect_custom(const uint8_t *seq, int n, uint8_t end)
{
    int i;

    for (i = 0; i < n; i++) {
        expect(0xF8);
        expect(seq[i]);
    }
    expect(end);
}

/** Expect the bring-up's start, the reset and the defaults, a Kensington
 * ThinkingMouse's rates and ID, which psmouse tries first, the Synaptics
 * knock, which an Elantech pad does not answer as a Synaptics pad, and the
 * Elantech magic knock with its firmware query, Linux's
 * `elantech_detect`. */
static void expect_start(void)
{
    static const uint8_t thinking[9] = {20, 60, 40, 20, 20, 60, 40, 20, 20};
    int i;

    n_want = 0;
    expect(0xFF);
    expect(0xF6);
    expect(0xF3);
    expect(10);
    expect(0xE8);
    expect(0x00);
    for (i = 0; i < 9; i++) {
        expect(0xF3);
        expect(thinking[i]);
    }
    expect(0xF2);
    for (i = 0; i < 4; i++) {
        expect(0xE8);
        expect(0x00);
    }
    expect(0xE9);
    expect(0xF6);
    expect(0xF5);
    expect(0xE6);
    expect(0xE6);
    expect(0xE6);
    expect(0xE9);
    expect_squery(0x01);
}

/** Expect psmouse_initialize's rate, resolution and scaling, then the
 * enable. */
static void expect_initialize(void)
{
    expect(0xF3);
    expect(100);
    expect(0xE8);
    expect(0x03);
    expect(0xE6);
    expect(0xF4);
}

/** Whether the device took the bytes expected; prints the first that
 * differs. */
static int took_expected(void)
{
    int i;

    if (n_took != n_want) {
        printf("elantech-test: the device took %d bytes, not %d\n", n_took, n_want);
    }
    for (i = 0; i < n_took && i < n_want && i < TOOK_MAX; i++) {
        if (took[i] != want_took[i]) {
            printf("elantech-test: byte %d to the device is 0x%02x, not 0x%02x\n", i, took[i],
                   want_took[i]);
            return 0;
        }
    }
    return n_took == n_want;
}

/** Bring up the mouse on the model as it is; whether a pad came up. */
static int bring_up_now(void)
{
    return init_mouse() == 0 && touchpad == TOUCHPAD_ELANTECH;
}

/** Bring up the pad `p` on a fresh model; whether it came up as a pad. */
static int bring_up(int p)
{
    model_reset(KIND_PAD, p);
    return bring_up_now();
}

/** Four pads brought up, one of each hardware version: Linux's commands in
 * their order, the registers they leave, and the handler. */
static void bringup_rows(void)
{
    static const uint8_t v2_writes[3][3] = {
        {0x11, 0x10, 0x54}, {0x11, 0x11, 0x88}, {0x11, 0x21, 0x60}};
    static const uint8_t v2_read[2] = {0x10, 0x10};
    static const uint8_t v3_write[3] = {0x00, 0x10, 0x0b};
    static const uint8_t v3_read[2] = {0x00, 0x10};
    static const uint8_t v4_write[4] = {0x00, 0x07, 0x00, 0x01};
    int i;

    CHECK("a version 1 pad comes up", bring_up(V1_ROCKER));
    expect_start();
    expect(0xFF); /* psmouse_reset */
    expect_squery(0x01);
    expect_squery(0x02);
    expect_sliced(0x11); /* register 0x10 = 0x16 */
    expect_sliced(0x10);
    expect_sliced(0x16);
    expect(0xE6);
    expect_sliced(0x11); /* register 0x11 = 0x8f */
    expect_sliced(0x11);
    expect_sliced(0x8f);
    expect(0xE6);
    expect_sliced(0x10); /* register 0x10 read back */
    expect_sliced(0x10);
    expect(0xE9);
    expect_initialize();
    CHECK("version 1: Linux's commands, in Linux's order", took_expected());
    CHECK("version 1: registers 0x10 and 0x11 as Linux sets them",
          regs[0x10] == 0x16 && regs[0x11] == 0x8f);
    CHECK("version 1: the pad reports", reporting);
    CHECK("version 1: four-byte packets", etd_pktsize == 4);
    CHECK("the handler is registered", registered_handler == mouse_irq_handler);
    CHECK("the configuration byte enables IRQ 12 and the aux clock", ctl_config == MOUSE_CONFIG);
    CHECK("the bring-up touches no port with interrupts on", ports_with_irq_on == 0);
    CHECK("the bring-up turns interrupts on again", irq_on);
    CHECK("the bring-up logs the pad and no warning", warnings == 0 && last_info != 0);
    CHECK("no mouse device is registered beside it", mouse_dev == NULL);
    CHECK("every byte is read", model_count == 0);

    CHECK("a version 2 pad comes up", bring_up(V2_PRESSURE));
    expect_start();
    expect(0xFF);
    expect_squery(0x01);
    expect_squery(0x02);
    expect_squery(0x03);
    for (i = 0; i < 3; i++) {
        expect_custom(v2_writes[i], 3, 0xE6);
    }
    expect_custom(v2_read, 2, 0xE9);
    expect_initialize();
    CHECK("version 2: Linux's commands, in Linux's order", took_expected());
    CHECK("version 2: the Windows driver's registers",
          regs[0x10] == 0x54 && regs[0x11] == 0x88 && regs[0x21] == 0x60);
    CHECK("version 2: six-byte packets", etd_pktsize == 6);

    CHECK("a version 3 pad comes up", bring_up(V3_STICK));
    expect_start();
    expect(0xFF);
    expect_squery(0x01);
    expect_equery(0x02);
    expect_equery(0x03);
    expect_equery(0x00);
    expect_custom(v3_write, 3, 0xE6);
    expect_custom(v3_read, 2, 0xE9);
    expect_initialize();
    CHECK("version 3: Linux's commands, in Linux's order", took_expected());
    CHECK("version 3: register 0x10 with the hardware's resolution", regs[0x10] == 0x0b);

    CHECK("a version 4 pad comes up", bring_up(V4_CLICKPAD));
    expect_start();
    expect(0xFF);
    expect_squery(0x01);
    expect_equery(0x02);
    expect_equery(0x03);
    expect_equery(0x04);
    expect_equery(0x00);
    expect_custom(v4_write, 4, 0xE6);
    expect_initialize();
    CHECK("version 4: Linux's commands, in Linux's order", took_expected());
    CHECK("version 4: register 0x07 sets absolute mode", regs[0x07] == 0x01);
}

/** The pads Linux knows by their firmware, which need commands more: the
 * IC body and the stick's report ID of a new IC, and the mode written
 * again after the rate. */
static void firmware_rows(void)
{
    static const uint8_t v4_write[4] = {0x00, 0x07, 0x00, 0x01};
    static const uint8_t report_id[4] = {0x00, 0x07, 0x10, 0x03};
    static const uint8_t report_read[2] = {0x00, 0x07};

    CHECK("a new IC comes up", bring_up(V4_NEW_IC));
    expect_start();
    expect(0xFF);
    expect_squery(0x01);
    expect_equery(0x02);
    expect_equery(0x03);
    expect_equery(0x05); /* the IC body of a pad with a pattern */
    expect_custom(report_id, 4, 0xE6);
    expect_custom(report_read, 2, 0xE9);
    expect_equery(0x04);
    expect_equery(0x00);
    expect_custom(v4_write, 4, 0xE6);
    expect_initialize();
    CHECK("a new IC: the IC body, then the stick's report ID set and read back", took_expected());
    CHECK("a new IC: IC body 0x11 and a product of two bytes",
          etd_info.ic_version == 0x11 && etd_info.product_id == 0x0e);
    CHECK("a new IC: SMBus Host Notify offered", etd_info.bus == 4);
    CHECK("a new IC that is no clickpad has a middle button", etd_info.has_middle_button);

    CHECK("firmware 0x381f17 comes up", bring_up(V4_TP500));
    expect_start();
    expect(0xFF);
    expect_squery(0x01);
    expect_equery(0x02);
    expect_equery(0x03);
    expect_equery(0x04);
    expect_equery(0x00);
    expect_custom(v4_write, 4, 0xE6);
    expect(0xF3);
    expect(100);
    expect_custom(v4_write, 4, 0xE6); /* the mode again after the rate */
    expect(0xE8);
    expect(0x03);
    expect(0xE6);
    expect(0xF4);
    CHECK("firmware 0x381f17: register 0x07 written again after the rate", took_expected());

    CHECK("the CELSIUS H730 comes up", bring_up(V4_H730));
    CHECK("the CELSIUS H730: CRC packets and a middle button, by its DMI",
          etd_info.crc_enabled && etd_info.has_middle_button);
    CHECK("the U2442 comes up", bring_up(V3_CRC));
    CHECK("the U2442: register 0x10 without the hardware's resolution, by its DMI",
          regs[0x10] == 0x01 && !etd_info.set_hw_resolution);
}

/** `n` longs of the bits at `bytes`, as /proc/bus/input/devices prints a
 * set: from the highest long that is not 0, each in hex, or a single 0. */
static void proc_bits(char *out, const uint8_t *bytes, int n)
{
    int i, j, started = 0;
    unsigned long long word;

    out[0] = '\0';
    for (i = n - 1; i >= 0; i--) {
        word = 0;
        for (j = 7; j >= 0; j--) {
            word = word << 8 | bytes[i * 8 + j];
        }
        if (!word && !started) {
            continue;
        }
        out += sprintf(out, "%s%llx", started ? " " : "", word);
        started = 1;
    }
    if (!started) {
        sprintf(out, "0");
    }
}

/** The bits EVIOCGBIT or EVIOCGPROP of the open `which` gives for
 * `request`, `longs` of them, printed as /proc/bus/input/devices prints
 * them into `text`. */
static void read_bits(int which, uint32_t request, int longs, char *text)
{
    uint8_t bits[96];

    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&open_e[which], request, (uintptr_t)bits);
    proc_bits(text, bits, longs);
}

/** Each axis of the pad that EVIOCGBIT names but the slot and the
 * tracking ID, as
 * "code:minimum:maximum:resolution", into `text`. */
static void read_limits(char *text)
{
    struct input_absinfo a;
    uint8_t bits[8];
    uint16_t code;
    const char *sep = "";

    text[0] = '\0';
    k_memset(bits, 0, sizeof(bits));
    evdev_ioctl(&open_e[PAD], EVIOCGBIT(EV_ABS, 8), (uintptr_t)bits);
    for (code = 0; code < ABS_CNT; code++) {
        if (!(bits[code / 8] & (1u << (code % 8))) || code == ABS_MT_SLOT ||
            code == ABS_MT_TRACKING_ID ||
            evdev_ioctl(&open_e[PAD], EVIOCGABS(code), (uintptr_t)&a) != 0) {
            continue;
        }
        text += sprintf(text, "%s%x:%d:%d:%d", sep, code, (int)a.minimum, (int)a.maximum,
                        (int)a.resolution);
        sep = " ";
    }
}

/** Check that `text` is `want`, printing both when not. */
static void check_text(const char *what, const char *text, const char *want)
{
    if (strcmp(text, want) != 0) {
        printf("elantech-test: %s is \"%s\", not \"%s\"\n", what, text, want);
    }
    CHECK(what, strcmp(text, want) == 0);
}

/** Every pad brought up: what the queries found, and its event devices'
 * names, numbers and capabilities against what Linux gives it. */
static void capability_rows(void)
{
    char text[512];
    char what[96];
    uint16_t id[4];
    int p;

    for (p = 0; p < V3_BROKEN; p++) {
        const struct profile *w = &profiles[p];

        sprintf(what, "%s comes up", w->name);
        CHECK(what, bring_up(p));
        sprintf(what, "%s: hardware version %d, firmware 0x%06x", w->name, w->hw,
                (unsigned)w->fw_version);
        CHECK(what, etd_info.hw_version == w->hw && etd_info.fw_version == w->fw_version);
        sprintf(what, "%s: its range, resolution, trace width and product", w->name);
        CHECK(what, etd_info.x_max == w->x_max && etd_info.y_max == w->y_max &&
                        etd_info.x_res == w->x_res && etd_info.y_res == w->y_res &&
                        etd_info.width == w->width && etd_info.product_id == w->product_id);
        sprintf(what, "%s: its pad's event device opens", w->name);
        CHECK(what, open_named(PAD, "ETPS/2 Elantech Touchpad") == 0);
        if (!is_open[PAD]) {
            continue;
        }
        evdev_ioctl(&open_e[PAD], EVIOCGPHYS(64), (uintptr_t)text);
        check_text("the pad is at isa0060/serio1/input0", text, "isa0060/serio1/input0");
        evdev_ioctl(&open_e[PAD], EVIOCGID, (uintptr_t)id);
        sprintf(what, "%s: I: Bus=0011 Vendor=0002 Product=000e Version=0000", w->name);
        CHECK(what, id[0] == 0x11 && id[1] == 2 && id[2] == 14 && id[3] == 0);
        read_bits(PAD, EVIOCGPROP(8), 1, text);
        sprintf(what, "%s: B: PROP", w->name);
        check_text(what, text, w->prop);
        read_bits(PAD, EVIOCGBIT(0, 8), 1, text);
        sprintf(what, "%s: B: EV", w->name);
        check_text(what, text, "b");
        read_bits(PAD, EVIOCGBIT(EV_KEY, 96), 12, text);
        sprintf(what, "%s: B: KEY", w->name);
        check_text(what, text, w->key);
        read_bits(PAD, EVIOCGBIT(EV_ABS, 8), 1, text);
        sprintf(what, "%s: B: ABS", w->name);
        check_text(what, text, w->abs);
        read_limits(text);
        sprintf(what, "%s: each axis's limits and resolution", w->name);
        check_text(what, text, w->limits);
        sprintf(what, "%s: a stick's device where Linux has one", w->name);
        CHECK(what, (open_named(STICK, "ETPS/2 Elantech TrackPoint") == 0) == w->stick);
        if (!is_open[STICK]) {
            continue;
        }
        evdev_ioctl(&open_e[STICK], EVIOCGPHYS(64), (uintptr_t)text);
        check_text("the stick is at isa0060/serio1/input1", text, "isa0060/serio1/input1");
        evdev_ioctl(&open_e[STICK], EVIOCGID, (uintptr_t)id);
        CHECK("the stick: I: Bus=0011 Vendor=0002 Product=000e Version=0000",
              id[0] == 0x11 && id[1] == 2 && id[2] == 14 && id[3] == 0);
        read_bits(STICK, EVIOCGPROP(8), 1, text);
        check_text("the stick: B: PROP, a pointer and a pointing stick", text, "21");
        read_bits(STICK, EVIOCGBIT(0, 8), 1, text);
        check_text("the stick: B: EV", text, "7");
        read_bits(STICK, EVIOCGBIT(EV_KEY, 96), 12, text);
        check_text("the stick: B: KEY, three buttons", text, "70000 0 0 0 0");
        read_bits(STICK, EVIOCGBIT(EV_REL, 8), 1, text);
        check_text("the stick: B: REL", text, "3");
    }
}

/** Run IRQ 12 once with the device's byte `b` waiting. */
static void irq_byte(uint8_t b)
{
    model_head = 0;
    model_count = 0;
    model_put(b);
    mouse_irq_handler(0);
}

/** Every packet of `steps`, each pad brought up afresh, read from its
 * event devices against the events Linux gives the same packets. */
static void packet_rows(void)
{
    char what[160];
    unsigned int s;
    int p = -1, i, losses;

    for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        const struct step *st = &steps[s];

        if (st->profile != p) {
            p = st->profile;
            sprintf(what, "%s comes up for its packets", profiles[p].name);
            CHECK(what, bring_up(p) && open_named(PAD, "ETPS/2 Elantech Touchpad") == 0);
            if (profiles[p].stick) {
                open_named(STICK, "ETPS/2 Elantech TrackPoint");
            }
        }
        losses = (int)etd_sync_losses;
        for (i = 0; i < etd_pktsize; i++) {
            irq_byte(st->bytes[i]);
        }
        sprintf(what, "%s: %s", profiles[p].name, st->what);
        if (st->n == 0) {
            CHECK(what, take(PAD) == -EAGAIN && (!is_open[STICK] || take(STICK) == -EAGAIN));
        } else {
            CHECK(what, take(st->dev) > 0 && got_events(st->want, st->n));
        }
        sprintf(what, "%s: %s, counted as lost or not as Linux drops it", profiles[p].name,
                st->what);
        CHECK(what, (int)etd_sync_losses - losses == st->bad && etd_pktcnt == 0);
    }
}

/** Debug level 1 logs discarded cursor and stick packets, and dumps an
 * unexpected stick packet once at both levels 1 and 2. */
static void logging_rows(void)
{
    static const uint8_t jump[4] = {0x58, 0x00, 0x64, 0x64};
    static const uint8_t range[6] = {0x14, 0x00, 0x80, 0x26, 0x80, 0x07};
    static const uint8_t unknown[6] = {0x10, 0x00, 0x00, 0x16, 0x05, 0xfd};
    int i, level;

    CHECK("the jumpy pad comes up for its debug row", bring_up(V1_JUMPY));
    etd_info.debug = 1;
    debug_messages = 0;
    for (i = 0; i < 4; i++) {
        irq_byte(jump[i]);
    }
    CHECK("debug 1 logs a discarded jumpy-cursor packet", debug_messages == 1);
    CHECK("the stick comes up for its debug row", bring_up(V3_STICK));
    debug_messages = 0;
    for (i = 0; i < 6; i++) {
        irq_byte(range[i]);
    }
    CHECK("debug 1 logs an out-of-range stick packet", debug_messages == 1);
    for (level = 0; level <= 2; level++) {
        etd_info.debug = (uint8_t)level;
        debug_messages = 0;
        for (i = 0; i < 6; i++) {
            irq_byte(unknown[i]);
        }
        CHECK("an unexpected stick packet is dumped once when debug is nonzero",
              debug_messages == (level != 0));
    }
}

/** The pointer of the mouse device moves with the stick, and a byte after
 * a pause queues a resync and a failed poll reconnects the pad. */
static void stream_rows(void)
{
    static const uint8_t stick[6] = {0x21, 0x80, 0x00, 0x16, 0x05, 0xfd};
    static const uint8_t finger[6] = {0x55, 0x42, 0xbc, 0x22, 0x61, 0x2c};
    struct mouse_packet m[4];
    struct input_absinfo a;
    int i, n;

    CHECK("the stick's pad comes up", bring_up(V3_STICK));
    mouse_clear_packets();
    for (i = 0; i < 6; i++) {
        irq_byte(stick[i]);
    }
    n = mouse_read_packets(m, 4);
    CHECK("the stick moves the mouse device's pointer, up positive",
          n == 1 && m[0].buttons == 1 && m[0].dx == 5 && m[0].dy == -3);

    irq_byte(stick[0]);
    irq_byte(stick[1]);
    kernel_ticks += MS_TO_TICKS(600);
    irq_byte(0);
    CHECK("a pause inside the pad's packet queues a resync",
          state == STATE_RESYNC && (reconnect_wanted & RESYNC_PORT));
    ps2mouse_resync();
    CHECK("the unanswered poll queues a reconnect",
          state == STATE_IGNORE && (reconnect_wanted & RECONNECT_PORT));
    ps2mouse_reconnect();
    CHECK("the pad reports after reconnect", state == STATE_ACTIVE);

    for (i = 0; i < 6; i++) {
        irq_byte(finger[i]);
    }
    CHECK("the interrupted packet stays discarded", etd_sync_losses == 0 && etd_pktcnt == 0);
    CHECK("the packet after the pause is read whole, a finger at 700",
          open_named(PAD, "ETPS/2 Elantech Touchpad") == 0 &&
              evdev_get_abs(etd_pad, ABS_X, &a) == 0 && a.value == 700);
    mouse_clear_packets();
}

/** Expect the bytes of a version 3 pad's reconnect, Linux's
 * `elantech_reconnect` then `psmouse_initialize` and the enable. */
static void expect_v3_reconnect(void)
{
    static const uint8_t v3_write[3] = {0x00, 0x10, 0x0b};
    static const uint8_t v3_read[2] = {0x00, 0x10};

    n_want = 0;
    expect(0xFF);
    expect(0xF6);
    expect(0xF5);
    expect(0xE6);
    expect(0xE6);
    expect(0xE6);
    expect(0xE9);
    expect_squery(0x01);
    expect_custom(v3_write, 3, 0xE6);
    expect_custom(v3_read, 2, 0xE9);
    expect_initialize();
}

/** Feed the handler `n` bytes of `bytes`, or `n` zeros when `bytes` is
 * NULL, which no version takes as a packet. */
static void irq_feed(const uint8_t *bytes, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        irq_byte(bytes ? bytes[i] : 0x00);
    }
}

/* A version 3 packet of one finger at 700, which the pad takes whole. */
static const uint8_t v3_finger[6] = {0x55, 0x42, 0xbc, 0x22, 0x61, 0x2c};

/** A pad reconnected after five packets out of form in a row and after its
 * announcement, keeping its event devices, as Linux's psmouse and
 * `elantech_reconnect` do. */
static void reconnect_rows(void)
{
    struct evdev *pad_dev, *stick_dev;

    CHECK("the stick's pad comes up", bring_up(V3_STICK));
    pad_dev = etd_pad;
    stick_dev = etd_tp;
    irq_feed(NULL, 24);
    CHECK("four packets out of form are four bad packets",
          bad_in_a_row == 4 && state == STATE_ACTIVE && !reconnect_wanted);
    irq_feed(v3_finger, 6);
    CHECK("a whole packet ends the count", bad_in_a_row == 0);
    irq_feed(NULL, 30);
    CHECK("the fifth in a row stops the port and asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT && last_warning &&
              strcmp(last_warning, "sync: %u bad packets in a row: reconnecting") == 0);
    irq_byte(v3_finger[0]);
    CHECK("the port then drops every byte", etd_pktcnt == 0);
    n_took = 0;
    ps2mouse_reconnect();
    expect_v3_reconnect();
    CHECK("the pad is reconnected with Linux's commands, in Linux's order", took_expected());
    CHECK("its absolute mode is set again and it reports",
          regs[0x10] == 0x0b && reporting && state == STATE_ACTIVE && bad_in_a_row == 0);
    CHECK("it keeps its event devices", etd_pad == pad_dev && etd_tp == stick_dev &&
                                            count_named("ETPS/2 Elantech Touchpad") == 1 &&
                                            count_named("ETPS/2 Elantech TrackPoint") == 1);
    irq_feed(v3_finger, 6);
    CHECK("and its packets are read", etd_pktcnt == 0 && bad_in_a_row == 0);

    irq_byte(0xAA);
    irq_byte(0x00);
    CHECK("its announcement 0xAA 0x00 asks for a reconnect",
          state == STATE_IGNORE && reconnect_wanted == RECONNECT_PORT);
    n_took = 0;
    ps2mouse_reconnect();
    expect_v3_reconnect();
    CHECK("which sends the same commands", took_expected() && etd_pad == pad_dev);
}

/** A mouse that answers in the pad's place at its reconnect, probed anew
 * as Linux's serio core probes a port whose reconnect failed. */
static void reconnect_mouse_rows(void)
{
    CHECK("the stick's pad comes up", bring_up(V3_STICK));
    irq_feed(NULL, 30);
    dev_kind = KIND_LOGITECH;
    ps2mouse_reconnect();
    CHECK("a mouse that answers in the pad's place is probed anew",
          touchpad == 0 && etd_pad == NULL && etd_tp == NULL && reporting && state == STATE_ACTIVE);
    CHECK("the pad's devices are gone and the mouse's is registered",
          count_named("ETPS/2 Elantech Touchpad") == 0 &&
              count_named("ETPS/2 Elantech TrackPoint") == 0 &&
              count_named("PS/2 Generic Mouse") == 1);
}

/** A mouse that answers the knock, and pads whose setup fails, go on as
 * mice, as Linux's psmouse sets them up. */
static void fallback_rows(void)
{
    uint64_t ticks_before;

    model_reset(KIND_LOGITECH, V4_CLICKPAD);
    CHECK("a mouse that answers the knock comes up", init_mouse() == 0);
    CHECK("its version, a rate, says it is no Elantech pad", touchpad == 0 && etd_pad == NULL);
    CHECK("it is PS/2 Generic Mouse", open_named(PAD, "PS/2 Generic Mouse") == 0);
    CHECK("no warning", warnings == 0);

    model_reset(KIND_PAD, V3_BROKEN);
    CHECK("a version 3 pad of sample byte 0x74 comes up", init_mouse() == 0);
    CHECK("as a mouse, since its absolute mode is broken",
          touchpad == -TOUCHPAD_ELANTECH && etd_pad == NULL);
    CHECK("it is PS/2 Elantech Touchpad", open_named(PAD, "PS/2 Elantech Touchpad") == 0);
    CHECK("the broken mode is logged",
          last_warning &&
              strcmp(last_warning, "query: absolute mode broken, set up as a mouse") == 0);

    model_reset(KIND_PAD, V3_STICK);
    refuse_reg = 0x10;
    CHECK("a pad that refuses its mode comes up", init_mouse() == 0);
    CHECK("as a mouse", touchpad == -TOUCHPAD_ELANTECH && etd_pad == NULL && etd_tp == NULL);
    CHECK("each command of the write tried three times, half a second apart",
          waits == 2 && waited_us == 1000000);
    CHECK("with interrupts on while it waits, so that the ticks run", waits_irq_on == 2);
    CHECK("the refusal is logged",
          last_warning && strcmp(last_warning, "mode: the registers would not set") == 0);
    CHECK("it is reset and named PS/2 Elantech Touchpad",
          open_named(PAD, "PS/2 Elantech Touchpad") == 0 && reporting);
    CHECK("the failed setup touches no port with interrupts on", ports_with_irq_on == 0);
    refuse_reg = -1;
    reconnect_wanted = RECONNECT_PORT;
    ps2mouse_reconnect();
    CHECK("reconnect retries absolute mode after the refused register write",
          touchpad == TOUCHPAD_ELANTECH && etd_pad && etd_tp && !mouse_dev && reporting);

    model_reset(KIND_PAD, V1_ROCKER);
    no_abs_bit = 1;
    CHECK("a version 1 pad whose register 0x10 has no absolute bit comes up", init_mouse() == 0);
    CHECK("as a mouse", touchpad == -TOUCHPAD_ELANTECH);
    CHECK("its refusal is logged", warnings == 2);

    model_reset(KIND_PAD, V2_PRESSURE);
    drop_reads = 3;
    CHECK("a version 2 pad that answers its read back late comes up", bring_up_now());
    CHECK("the read back tried again after three tries of half a second and two seconds",
          waits == 3 && waited_us == 3000000);
    CHECK("its registers are set", regs[0x10] == 0x54);
    CHECK("each wait with interrupts on", waits_irq_on == 3 && irq_on);

    drop_reads = 3;
    waits = 0;
    ticks_before = kernel_ticks;
    reconnect_wanted = RECONNECT_PORT;
    ps2mouse_reconnect();
    CHECK("at its reconnect kpsmoused sleeps the same waits, as Linux's msleep does",
          touchpad == TOUCHPAD_ELANTECH && waits == 0 &&
              kernel_ticks - ticks_before == MS_TO_TICKS(3000) && regs[0x10] == 0x54);
}

int main(void)
{
    bringup_rows();
    firmware_rows();
    capability_rows();
    packet_rows();
    logging_rows();
    stream_rows();
    fallback_rows();
    reconnect_rows();
    reconnect_mouse_rows();
    model_reset(KIND_PAD, V1_ROCKER);
    if (fails == 0) {
        printf("elantech-test: pass\n");
    }
    return fails ? 1 : 0;
}
