/*
 * hid_input.c: a parsed HID device's usages as event devices, Linux's
 * hid-input.c for the generic desktop, keyboard, button, consumer and LED
 * pages, and its input reports read as Linux's hid-core.c reads them.
 *
 * A usage names what a control is: the X axis, button 3, the key "a",
 * Volume Up. hid-input maps each one to an event type and code once, when
 * the device is connected; a report then carries each control's value,
 * and a value that changed is reported as the event its usage was mapped
 * to. The tables and rules below are Linux 6.12's, so that a device gives
 * the same event device, and the same events, as it does on Linux.
 *
 * Reports with digitizer contact IDs are grouped into type B slot frames.
 */

#include "hid_input.h"

#include "usb.h"

#include "../evdev.h"
#include "../mm/kheap.h"
#include "../input.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mouse.h"

/* Usage pages and usages, Linux's HID_UP_ and HID_GD_ values. */
#define HID_USAGE_PAGE                0xFFFF0000u
#define HID_USAGE                     0x0000FFFFu
#define HID_UP_UNDEFINED              0x00000000u
#define HID_UP_GENDESK                0x00010000u
#define HID_UP_SIMULATION             0x00020000u
#define HID_UP_GENDEVCTRLS            0x00060000u
#define HID_UP_KEYBOARD               0x00070000u
#define HID_UP_LED                    0x00080000u
#define HID_UP_BUTTON                 0x00090000u
#define HID_UP_TELEPHONY              0x000B0000u
#define HID_UP_CONSUMER               0x000C0000u
#define HID_UP_DIGITIZER              0x000D0000u
#define HID_UP_PID                    0x000F0000u
#define HID_UP_BATTERY                0x00850000u
#define HID_UP_CAMERA                 0x00900000u
#define HID_UP_CUSTOM                 0x00FF0000u
#define HID_UP_MSVENDOR               0xFF000000u
#define HID_UP_HPVENDOR2              0xFF010000u
#define HID_UP_LOGIVENDOR2            0xFF090000u
#define HID_UP_LOGIVENDOR3            0xFF430000u
#define HID_UP_HPVENDOR               0xFF7F0000u
#define HID_UP_LOGIVENDOR             0xFFBC0000u
#define HID_DC_BATTERYSTRENGTH        0x00060020u
#define HID_BAT_CHARGING              0x00850044u
#define HID_BAT_ABSOLUTESTATEOFCHARGE 0x00850065u
#define HID_GD_POINTER                0x00010001u
#define HID_GD_MOUSE                  0x00010002u
#define HID_GD_JOYSTICK               0x00010004u
#define HID_GD_GAMEPAD                0x00010005u
#define HID_GD_KEYBOARD               0x00010006u
#define HID_GD_KEYPAD                 0x00010007u
#define HID_GD_MULTIAXIS              0x00010008u
#define HID_GD_WIRELESS               0x0001000Cu
#define HID_GD_SYSMULTIAXIS           0x0001000Eu
#define HID_GD_X                      0x00010030u
#define HID_GD_Y                      0x00010031u
#define HID_GD_Z                      0x00010032u
#define HID_GD_RX                     0x00010033u
#define HID_GD_RY                     0x00010034u
#define HID_GD_RZ                     0x00010035u
#define HID_GD_SLIDER                 0x00010036u
#define HID_GD_DIAL                   0x00010037u
#define HID_GD_WHEEL                  0x00010038u
#define HID_GD_HATSWITCH              0x00010039u
#define HID_GD_START                  0x0001003Du
#define HID_GD_SELECT                 0x0001003Eu
#define HID_GD_RESOLUTION             0x00010048u
#define HID_GD_SYSCONTROL             0x00010080u
#define HID_GD_UP                     0x00010090u
#define HID_GD_DOWN                   0x00010091u
#define HID_GD_RIGHT                  0x00010092u
#define HID_GD_LEFT                   0x00010093u
#define HID_GD_RFKILL_BTN             0x000100C6u
#define HID_CP_CONSUMER               0x000C0001u
#define HID_CP_PROGBUTTONS            0x000C0003u
#define HID_DG_DIGITIZER              0x000D0001u
#define HID_DG_PEN                    0x000D0002u
#define HID_DG_TOUCHSCREEN            0x000D0004u
#define HID_DG_TOUCHPAD               0x000D0005u
#define HID_DG_WHITEBOARD             0x000D0006u
#define HID_DG_STYLUS                 0x000D0020u
#define HID_DG_FINGER                 0x000D0022u
#define HID_DG_TIPPRESSURE            0x000D0030u
#define HID_DG_AZIMUTH                0x000D003fu
#define HID_DG_INRANGE                0x000D0032u
#define HID_DG_TIPSWITCH              0x000D0042u
#define HID_DG_CONFIDENCE             0x000D0047u
#define HID_DG_WIDTH                  0x000D0048u
#define HID_DG_HEIGHT                 0x000D0049u
#define HID_DG_CONTACTID              0x000D0051u
#define HID_DG_INPUTMODE              0x000D0052u
#define HID_DG_CONTACTCOUNT           0x000D0054u
#define HID_DG_CONTACTMAX             0x000D0055u
#define HID_MS_MT_FEATURE             0xff0000c5u

#define HID_MT_COUNT   0xffffu /* marks the frame's Contact Count usage */
#define HID_MT_BUTTON  0xfffeu /* a precision touchpad's frame button */
#define HID_MT_DEFAULT 10
#define HID_MT_AXES    (ABS_MT_LAST - ABS_MT_FIRST + 1)

/* One contact in a HID packet. `next` and the input's head are one-based
 * contact record numbers, 0 at the end. */
struct hid_mt_contact {
    uint16_t next;
    uint16_t axes; /* values present in the packet, from ABS_MT_FIRST */
    uint8_t seen;
    uint8_t identified;
    uint8_t tip;
    uint8_t in_range;
    uint8_t confidence;
    int32_t id;
    int32_t value[HID_MT_AXES];
};

/* The event codes this file names beyond evdev.h's, Linux's values. */
#define KEY_ESC            1
#define KEY_ENTER          28
#define KEY_KPMINUS        74
#define KEY_KPPLUS         78
#define KEY_UP             103
#define KEY_LEFT           105
#define KEY_RIGHT          106
#define KEY_DOWN           108
#define KEY_HELP           138
#define KEY_MENU           139
#define KEY_PROG1          148
#define KEY_EXIT           174
#define KEY_UNKNOWN        240
#define KEY_RFKILL         247
#define KEY_SELECT         353
#define KEY_POWER2         356
#define KEY_RESTART        408
#define KEY_CONTEXT_MENU   438
#define KEY_ACCESSIBILITY  590
#define KEY_DO_NOT_DISTURB 591
#define KEY_NUMERIC_0      512
#define KEY_CAMERA_FOCUS   528
#define KEY_MACRO1         656
#define BTN_MISC           0x100
#define BTN_MOUSE          0x110
#define BTN_JOYSTICK       0x120
#define BTN_DEAD           0x12f
#define BTN_GAMEPAD        0x130
#define BTN_SELECT         0x13a
#define BTN_START          0x13b
#define BTN_TRIGGER_HAPPY  0x2c0
#define REL_MISC           0x09
#define REL_MAX            0x0f
#define ABS_Z              0x02
#define ABS_RX             0x03
#define ABS_RY             0x04
#define ABS_RZ             0x05
#define ABS_THROTTLE       0x06
#define ABS_RUDDER         0x07
#define ABS_WHEEL          0x08
#define ABS_GAS            0x09
#define ABS_BRAKE          0x0a
#define ABS_HAT0X          0x10
#define ABS_VOLUME         0x20
#define ABS_MISC           0x28
#define ABS_MAX            0x3f
#define LED_COMPOSE        3
#define LED_KANA           4
#define LED_SLEEP          5
#define LED_SUSPEND        6
#define LED_MUTE           7
#define LED_MISC           8
#define LED_MAIL           9
#define LED_CHARGING       10
#define LED_MAX            0x0f
#define MSC_MAX            0x07

#define unk KEY_UNKNOWN

/* The keyboard page's usages as key codes, Linux's `hid_keyboard`; 0 is a
 * usage that maps to nothing. */
static const uint16_t hid_keyboard[256] = {
    0,   0,   0,   0,   30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,  50,  49,  24,
    25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,  2,   3,   4,   5,   6,   7,   8,   9,
    10,  11,  28,  1,   14,  15,  57,  12,  13,  26,  27,  43,  43,  39,  40,  41,  51,  52,  53,
    58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  87,  88,  99,  70,  119, 110, 102, 104,
    111, 107, 109, 106, 105, 108, 103, 69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,
    71,  72,  73,  82,  83,  86,  127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192,
    193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113, 115, 114, unk, unk, unk,
    121, unk, 89,  93,  124, 92,  94,  95,  unk, unk, unk, 122, 123, 90,  91,  85,  unk, unk, unk,
    unk, unk, unk, unk, 111, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk,
    unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, 179, 180, unk, unk, unk, unk, unk, unk,
    unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk, unk,
    unk, unk, unk, unk, unk, unk, unk, 111, unk, unk, unk, unk, unk, unk, unk, 29,  42,  56,  125,
    97,  54,  100, 126, 164, 166, 165, 163, 161, 115, 114, 113, 150, 158, 159, 128, 136, 177, 178,
    176, 142, 152, 173, 140, unk, unk, unk, unk};

/* A usage of a page and the key, axis or LED it maps to. */
struct usage_key {
    uint16_t usage;
    uint16_t key;
};

/* The consumer page's keys, in the order of Linux's switch. Volume (0xe0)
 * and AC Pan (0x238) are axes and are mapped in code; a usage not listed
 * here, and not 0, is KEY_UNKNOWN. */
static const struct usage_key consumer_keys[] = {
    {0x030, 116}, /* KEY_POWER */
    {0x031, 408}, /* KEY_RESTART */
    {0x032, 142}, /* KEY_SLEEP */
    {0x034, 142}, /* KEY_SLEEP */
    {0x035, 228}, /* KEY_KBDILLUMTOGGLE */
    {0x036, 256}, /* BTN_MISC */
    {0x040, 139}, /* KEY_MENU */
    {0x041, 353}, /* KEY_SELECT */
    {0x042, 103}, /* KEY_UP */
    {0x043, 108}, /* KEY_DOWN */
    {0x044, 105}, /* KEY_LEFT */
    {0x045, 106}, /* KEY_RIGHT */
    {0x046, 1},   /* KEY_ESC */
    {0x047, 78},  /* KEY_KPPLUS */
    {0x048, 74},  /* KEY_KPMINUS */
    {0x060, 358}, /* KEY_INFO */
    {0x061, 370}, /* KEY_SUBTITLE */
    {0x063, 379}, /* KEY_VCR */
    {0x065, 212}, /* KEY_CAMERA */
    {0x069, 398}, /* KEY_RED */
    {0x06a, 399}, /* KEY_GREEN */
    {0x06b, 401}, /* KEY_BLUE */
    {0x06c, 400}, /* KEY_YELLOW */
    {0x06d, 375}, /* KEY_ASPECT_RATIO */
    {0x06f, 225}, /* KEY_BRIGHTNESSUP */
    {0x070, 224}, /* KEY_BRIGHTNESSDOWN */
    {0x072, 431}, /* KEY_BRIGHTNESS_TOGGLE */
    {0x073, 592}, /* KEY_BRIGHTNESS_MIN */
    {0x074, 593}, /* KEY_BRIGHTNESS_MAX */
    {0x075, 244}, /* KEY_BRIGHTNESS_AUTO */
    {0x076, 587}, /* KEY_CAMERA_ACCESS_ENABLE */
    {0x077, 588}, /* KEY_CAMERA_ACCESS_DISABLE */
    {0x078, 589}, /* KEY_CAMERA_ACCESS_TOGGLE */
    {0x079, 230}, /* KEY_KBDILLUMUP */
    {0x07a, 229}, /* KEY_KBDILLUMDOWN */
    {0x07c, 228}, /* KEY_KBDILLUMTOGGLE */
    {0x082, 241}, /* KEY_VIDEO_NEXT */
    {0x083, 405}, /* KEY_LAST */
    {0x084, 28},  /* KEY_ENTER */
    {0x088, 376}, /* KEY_PC */
    {0x089, 377}, /* KEY_TV */
    {0x08a, 150}, /* KEY_WWW */
    {0x08b, 389}, /* KEY_DVD */
    {0x08c, 169}, /* KEY_PHONE */
    {0x08d, 362}, /* KEY_PROGRAM */
    {0x08e, 416}, /* KEY_VIDEOPHONE */
    {0x08f, 417}, /* KEY_GAMES */
    {0x090, 396}, /* KEY_MEMO */
    {0x091, 383}, /* KEY_CD */
    {0x092, 379}, /* KEY_VCR */
    {0x093, 386}, /* KEY_TUNER */
    {0x094, 174}, /* KEY_EXIT */
    {0x095, 138}, /* KEY_HELP */
    {0x096, 384}, /* KEY_TAPE */
    {0x097, 378}, /* KEY_TV2 */
    {0x098, 381}, /* KEY_SAT */
    {0x09a, 366}, /* KEY_PVR */
    {0x09c, 402}, /* KEY_CHANNELUP */
    {0x09d, 403}, /* KEY_CHANNELDOWN */
    {0x0a0, 380}, /* KEY_VCR2 */
    {0x0b0, 207}, /* KEY_PLAY */
    {0x0b1, 119}, /* KEY_PAUSE */
    {0x0b2, 167}, /* KEY_RECORD */
    {0x0b3, 208}, /* KEY_FASTFORWARD */
    {0x0b4, 168}, /* KEY_REWIND */
    {0x0b5, 163}, /* KEY_NEXTSONG */
    {0x0b6, 165}, /* KEY_PREVIOUSSONG */
    {0x0b7, 166}, /* KEY_STOPCD */
    {0x0b8, 161}, /* KEY_EJECTCD */
    {0x0bc, 439}, /* KEY_MEDIA_REPEAT */
    {0x0b9, 410}, /* KEY_SHUFFLE */
    {0x0bf, 409}, /* KEY_SLOW */
    {0x0cd, 164}, /* KEY_PLAYPAUSE */
    {0x0cf, 582}, /* KEY_VOICECOMMAND */
    {0x0d8, 586}, /* KEY_DICTATE */
    {0x0d9, 585}, /* KEY_EMOJI_PICKER */
    {0x0e2, 113}, /* KEY_MUTE */
    {0x0e5, 209}, /* KEY_BASSBOOST */
    {0x0e9, 115}, /* KEY_VOLUMEUP */
    {0x0ea, 114}, /* KEY_VOLUMEDOWN */
    {0x0f5, 409}, /* KEY_SLOW */
    {0x181, 576}, /* KEY_BUTTONCONFIG */
    {0x182, 156}, /* KEY_BOOKMARKS */
    {0x183, 171}, /* KEY_CONFIG */
    {0x184, 421}, /* KEY_WORDPROCESSOR */
    {0x185, 422}, /* KEY_EDITOR */
    {0x186, 423}, /* KEY_SPREADSHEET */
    {0x187, 424}, /* KEY_GRAPHICSEDITOR */
    {0x188, 425}, /* KEY_PRESENTATION */
    {0x189, 426}, /* KEY_DATABASE */
    {0x18a, 155}, /* KEY_MAIL */
    {0x18b, 427}, /* KEY_NEWS */
    {0x18c, 428}, /* KEY_VOICEMAIL */
    {0x18d, 429}, /* KEY_ADDRESSBOOK */
    {0x18e, 397}, /* KEY_CALENDAR */
    {0x18f, 577}, /* KEY_TASKMANAGER */
    {0x190, 578}, /* KEY_JOURNAL */
    {0x191, 219}, /* KEY_FINANCE */
    {0x192, 140}, /* KEY_CALC */
    {0x193, 387}, /* KEY_PLAYER */
    {0x194, 144}, /* KEY_FILE */
    {0x196, 150}, /* KEY_WWW */
    {0x199, 216}, /* KEY_CHAT */
    {0x19c, 433}, /* KEY_LOGOFF */
    {0x19e, 152}, /* KEY_COFFEE */
    {0x19f, 579}, /* KEY_CONTROLPANEL */
    {0x1a2, 580}, /* KEY_APPSELECT */
    {0x1a3, 407}, /* KEY_NEXT */
    {0x1a4, 412}, /* KEY_PREVIOUS */
    {0x1a6, 138}, /* KEY_HELP */
    {0x1a7, 235}, /* KEY_DOCUMENTS */
    {0x1ab, 432}, /* KEY_SPELLCHECK */
    {0x1ae, 374}, /* KEY_KEYBOARD */
    {0x1b1, 581}, /* KEY_SCREENSAVER */
    {0x1b4, 144}, /* KEY_FILE */
    {0x1b6, 442}, /* KEY_IMAGES */
    {0x1b7, 392}, /* KEY_AUDIO */
    {0x1b8, 393}, /* KEY_VIDEO */
    {0x1bc, 430}, /* KEY_MESSENGER */
    {0x1bd, 358}, /* KEY_INFO */
    {0x1cb, 583}, /* KEY_ASSISTANT */
    {0x201, 181}, /* KEY_NEW */
    {0x202, 134}, /* KEY_OPEN */
    {0x203, 206}, /* KEY_CLOSE */
    {0x204, 174}, /* KEY_EXIT */
    {0x207, 234}, /* KEY_SAVE */
    {0x208, 210}, /* KEY_PRINT */
    {0x209, 130}, /* KEY_PROPS */
    {0x21a, 131}, /* KEY_UNDO */
    {0x21b, 133}, /* KEY_COPY */
    {0x21c, 137}, /* KEY_CUT */
    {0x21d, 135}, /* KEY_PASTE */
    {0x21f, 136}, /* KEY_FIND */
    {0x221, 217}, /* KEY_SEARCH */
    {0x222, 354}, /* KEY_GOTO */
    {0x223, 172}, /* KEY_HOMEPAGE */
    {0x224, 158}, /* KEY_BACK */
    {0x225, 159}, /* KEY_FORWARD */
    {0x226, 128}, /* KEY_STOP */
    {0x227, 173}, /* KEY_REFRESH */
    {0x22a, 156}, /* KEY_BOOKMARKS */
    {0x22d, 418}, /* KEY_ZOOMIN */
    {0x22e, 419}, /* KEY_ZOOMOUT */
    {0x22f, 420}, /* KEY_ZOOMRESET */
    {0x232, 372}, /* KEY_FULL_SCREEN */
    {0x233, 177}, /* KEY_SCROLLUP */
    {0x234, 178}, /* KEY_SCROLLDOWN */
    {0x23d, 176}, /* KEY_EDIT */
    {0x25f, 223}, /* KEY_CANCEL */
    {0x269, 110}, /* KEY_INSERT */
    {0x26a, 111}, /* KEY_DELETE */
    {0x279, 182}, /* KEY_REDO */
    {0x289, 232}, /* KEY_REPLY */
    {0x28b, 233}, /* KEY_FORWARDMAIL */
    {0x28c, 231}, /* KEY_SEND */
    {0x29d, 584}, /* KEY_KBD_LAYOUT_NEXT */
    {0x2a2, 204}, /* KEY_ALL_APPLICATIONS */
    {0x2c7, 608}, /* KEY_KBDINPUTASSIST_PREV */
    {0x2c8, 609}, /* KEY_KBDINPUTASSIST_NEXT */
    {0x2c9, 610}, /* KEY_KBDINPUTASSIST_PREVGROUP */
    {0x2ca, 611}, /* KEY_KBDINPUTASSIST_NEXTGROUP */
    {0x2cb, 612}, /* KEY_KBDINPUTASSIST_ACCEPT */
    {0x2cc, 613}, /* KEY_KBDINPUTASSIST_CANCEL */
    {0x29f, 120}, /* KEY_SCALE */
};

/* The directions of a hat switch, 0 for centred, as X and Y. */
static const struct {
    int8_t x;
    int8_t y;
} hat_to_axis[9] = {{0, 0}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};

/** Whether bit `n` of `bits` is on. */
static int test_bit(const uint8_t *bits, unsigned n)
{
    return (bits[n >> 3] >> (n & 7)) & 1;
}

/** Turn bit `n` of `bits` on. */
static void set_bit(uint8_t *bits, unsigned n)
{
    bits[n >> 3] = (uint8_t)(bits[n >> 3] | (1u << (n & 7)));
}

/** Turn bit `n` of `bits` off. */
static void clear_bit(uint8_t *bits, unsigned n)
{
    bits[n >> 3] = (uint8_t)(bits[n >> 3] & ~(1u << (n & 7)));
}

/** The field `i` of `hid`. */
static struct hid_field *field_at(struct hid_dev *hid, uint32_t i)
{
    return &hid->desc.fields[i];
}

/** The usages of `f`, `f->maxusage` of them. */
static struct hid_usage *usages_of(struct hid_dev *hid, const struct hid_field *f)
{
    return &hid->desc.usages[f->usage];
}

/** The bits of `type` in `in`, and in `*max` the largest code, as Linux's
 * `hid_map_usage` chooses them; NULL for a type it does not map. */
static uint8_t *bits_of(struct hid_input *in, unsigned type, unsigned *max)
{
    switch (type) {
    case EV_ABS:
        *max = ABS_MAX;
        return in->abs;
    case EV_REL:
        *max = REL_MAX;
        return in->rel;
    case EV_KEY:
        *max = KEY_MAX;
        return in->keys;
    case EV_LED:
        *max = LED_MAX;
        return in->led;
    case EV_MSC:
        *max = MSC_MAX;
        return in->msc;
    default:
        *max = 0;
        return 0;
    }
}

/* What `configure_usage`'s cases decide: a mapping, with the code's bit
 * cleared first or not, none, or a battery usage, which Linux types EV_PWR
 * and gives its power supply instead of an event. */
#define MAP_NONE  0
#define MAP_SET   1
#define MAP_CLEAR 2
#define MAP_PWR   3

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/** The code `id` has in `table` of `n` entries, or -1 when it has none;
 * a code may be 0, as LED_NUML is. */
static int lookup(const struct usage_key *table, unsigned n, uint32_t id)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (table[i].usage == id) {
            return table[i].key;
        }
    }
    return -1;
}

/** The key `usage` of the consumer page maps to: one of the table's, 0
 * for usage 0, or KEY_UNKNOWN. */
static unsigned consumer_key(uint32_t usage)
{
    int key;

    if (usage == 0) {
        return 0;
    }
    key = lookup(consumer_keys, COUNT(consumer_keys), usage);
    return key >= 0 ? (unsigned)key : KEY_UNKNOWN;
}

/** Whether the collection of `f`'s first usage is a named array whose usage
 * is `usage`, Linux's `hidinput_field_in_collection`. */
static int field_in_named_array(struct hid_dev *hid, const struct hid_field *f, uint32_t usage)
{
    const struct hid_collection *c;
    uint32_t index = usages_of(hid, f)[0].collection;

    if (index >= hid->desc.ncollections) {
        return 0;
    }
    c = &hid->desc.collections[index];
    return c->type == HID_COLLECTION_NAMED_ARRAY && c->usage == usage;
}

/** The mapping `unknown` gives a usage no case names, by the shape of its
 * field: a bit is a button, or an LED in an output report, a relative one
 * REL_MISC and any other ABS_MISC. */
static int map_unknown(const struct hid_field *f, unsigned report_type, unsigned *type,
                       unsigned *code)
{
    if (f->size == 1) {
        if (report_type == HID_OUTPUT_REPORT) {
            *type = EV_LED;
            *code = LED_MISC;
            return MAP_SET;
        }
        *type = EV_KEY;
        *code = BTN_MISC;
        return MAP_SET;
    }
    *type = (f->flags & HID_MAIN_ITEM_RELATIVE) ? EV_REL : EV_ABS;
    *code = (f->flags & HID_MAIN_ITEM_RELATIVE) ? REL_MISC : ABS_MISC;
    return MAP_SET;
}

/** A keyboard-page usage, which also gives `in` EV_REP. */
static int map_keyboard(struct hid_input *in, uint32_t id, unsigned *type, unsigned *code)
{
    in->ev |= 1u << EV_REP;
    *type = EV_KEY;
    if (id < 256) {
        if (!hid_keyboard[id]) {
            return MAP_NONE;
        }
        *code = hid_keyboard[id];
        return MAP_CLEAR;
    }
    *code = KEY_UNKNOWN;
    return MAP_SET;
}

/** The first code of the buttons of a field whose collection's usage is
 * `usage`, a mouse's, a joystick's or a gamepad's, or 0 for another. */
static unsigned button_base(uint32_t usage, unsigned joystick, unsigned gamepad)
{
    switch (usage) {
    case HID_GD_MOUSE:
    case HID_GD_POINTER:
        return BTN_MOUSE;
    case HID_GD_JOYSTICK:
        return joystick;
    case HID_GD_GAMEPAD:
        return gamepad;
    default:
        return 0;
    }
}

/** A button-page usage: a button of the kind its application, or else its
 * physical collection, names. */
static int map_button(struct hid_dev *hid, const struct hid_field *f, const struct hid_usage *u,
                      unsigned *type, unsigned *code)
{
    unsigned c = (u->hid - 1) & HID_USAGE;
    unsigned high = BTN_TRIGGER_HAPPY - 0x10;
    unsigned base;

    *type = EV_KEY;
    base =
        button_base(f->application, c <= 0xf ? BTN_JOYSTICK : high, c <= 0xf ? BTN_GAMEPAD : high);
    if (base) {
        *code = c + base;
        return MAP_SET;
    }
    if (f->application == HID_CP_CONSUMER && field_in_named_array(hid, f, HID_CP_PROGBUTTONS)) {
        *code = c + (c <= 0x1d ? KEY_MACRO1 : BTN_TRIGGER_HAPPY - 0x1e);
        return MAP_SET;
    }
    base = button_base(f->physical, BTN_JOYSTICK, BTN_GAMEPAD);
    *code = c + (base ? base : BTN_MISC);
    return MAP_SET;
}

/* The simulation page's axes, the LED page's LEDs, and the keys of the
 * telephony, camera and two HP vendor pages, as Linux maps them. */
static const struct usage_key simulation_axes[] = {{0xba, ABS_RUDDER},
                                                   {0xbb, ABS_THROTTLE},
                                                   {0xc4, ABS_GAS},
                                                   {0xc5, ABS_BRAKE},
                                                   {0xc8, ABS_WHEEL}};
static const struct usage_key led_codes[] = {
    {0x01, LED_NUML}, {0x02, LED_CAPSL}, {0x03, LED_SCROLLL}, {0x04, LED_COMPOSE},
    {0x05, LED_KANA}, {0x27, LED_SLEEP}, {0x4c, LED_SUSPEND}, {0x09, LED_MUTE},
    {0x4b, LED_MISC}, {0x19, LED_MAIL},  {0x4d, LED_CHARGING}};
static const struct usage_key camera_keys[] = {{0x020, KEY_CAMERA_FOCUS}, {0x021, KEY_CAMERA}};
static const struct usage_key hp_keys[] = {{0x021, 210}, {0x070, 211}, {0x071, 212}, {0x072, 213},
                                           {0x073, 214}, {0x080, 215}, {0x081, 216}, {0x082, 217},
                                           {0x083, 218}, {0x084, 219}, {0x085, 220}, {0x086, 221}};
static const struct usage_key hp2_keys[] = {
    {0x001, KEY_MICMUTE}, {0x003, KEY_BRIGHTNESSDOWN}, {0x004, KEY_BRIGHTNESSUP}};

/** The key or axis `id` has in `table` as `*type`: MAP_CLEAR when it has
 * one, else MAP_NONE. */
static int map_table(const struct usage_key *table, unsigned n, uint32_t id, unsigned ev,
                     unsigned *type, unsigned *code)
{
    int found = lookup(table, n, id);

    *type = ev;
    *code = found >= 0 ? (unsigned)found : 0;
    return found >= 0 ? MAP_CLEAR : MAP_NONE;
}

/** The generic desktop page's System Control usages, 0x80 to 0xbf, as keys:
 * MAP_CLEAR with `*code` set, MAP_NONE for one Linux ignores, among them
 * the D-pad's 0x90 to 0x93, which Linux tests only after these, or -1 for
 * a usage outside them. */
static int map_system(const struct hid_field *f, uint32_t usage, unsigned report_type,
                      unsigned *type, unsigned *code)
{
    static const uint16_t system[16] = {
        0,         KEY_POWER, KEY_SLEEP,  KEY_WAKEUP, KEY_CONTEXT_MENU, KEY_MENU,
        KEY_PROG1, KEY_HELP,  KEY_EXIT,   KEY_SELECT, KEY_RIGHT,        KEY_LEFT,
        KEY_UP,    KEY_DOWN,  KEY_POWER2, KEY_RESTART};
    static const struct usage_key others[] = {{0x9b, KEY_DO_NOT_DISTURB},
                                              {0xa9, KEY_MICMUTE},
                                              {0xaa, KEY_ACCESSIBILITY},
                                              {0xb5, KEY_SWITCHVIDEOMODE}};
    uint32_t low = usage & 0xff;

    if (low < 0x80 || low > 0xbf) {
        return -1;
    }
    if (low < 0x90 && !system[low & 0xf]) {
        return map_unknown(f, report_type, type, code);
    }
    if (low < 0x90) {
        *code = system[low & 0xf];
        return MAP_CLEAR;
    }
    return map_table(others, COUNT(others), low, EV_KEY, type, code);
}

/** A generic desktop usage outside System Control: an axis, a wheel, a
 * hat switch or a few buttons, as Linux maps them. */
static int map_desktop(struct hid_input *in, struct hid_field *f, struct hid_usage *u,
                       unsigned report_type, unsigned *type, unsigned *code)
{
    unsigned relative = (f->flags & HID_MAIN_ITEM_RELATIVE) != 0;

    *code = u->hid & 0xf;
    switch (u->hid) {
    case HID_GD_X:
    case HID_GD_Y:
    case HID_GD_Z:
    case HID_GD_RX:
    case HID_GD_RY:
    case HID_GD_RZ:
        *type = relative ? EV_REL : EV_ABS;
        return relative ? MAP_SET : MAP_CLEAR;
    case HID_GD_WHEEL:
        *type = relative ? EV_REL : EV_ABS;
        if (relative) {
            set_bit(in->rel, REL_WHEEL);
            *code = REL_WHEEL_HI_RES;
        }
        return MAP_SET;
    case HID_GD_SLIDER:
    case HID_GD_DIAL:
        *type = relative ? EV_REL : EV_ABS;
        return MAP_SET;
    case HID_GD_HATSWITCH:
        /* Linux's hat_min < hat_max, kept in 16 bits: a range stored
         * backwards, such as 0 to 0xFFFFFFFF, or one whose ends cut to 16
         * bits are not in order, such as INT32_MIN to INT32_MAX, is an
         * axis. */
        u->hat = (int16_t)f->logical_minimum < (int16_t)f->logical_maximum;
        *type = EV_ABS;
        *code = ABS_HAT0X;
        return MAP_SET;
    case HID_GD_START:
    case HID_GD_SELECT:
        *code = u->hid == HID_GD_START ? BTN_START : BTN_SELECT;
        return MAP_CLEAR;
    case HID_GD_RFKILL_BTN:
        if (f->application == HID_GD_WIRELESS) {
            /* The button reports only its press; its release is made up,
             * as for a relative key. */
            f->flags |= HID_MAIN_ITEM_RELATIVE;
            *code = KEY_RFKILL;
            return MAP_CLEAR;
        }
        return map_unknown(f, report_type, type, code);
    default:
        return map_unknown(f, report_type, type, code);
    }
}

/** A generic desktop usage. */
static int map_gendesk(struct hid_input *in, struct hid_field *f, struct hid_usage *u,
                       unsigned report_type, unsigned *type, unsigned *code)
{
    int map;

    *type = EV_KEY;
    map = map_system(f, u->hid, report_type, type, code);
    if (map >= 0) {
        return map;
    }
    /* Vendors that declare 255 System Control usages would make axes of
     * them. */
    if (f->application == HID_GD_SYSCONTROL) {
        return MAP_NONE;
    }
    return map_desktop(in, f, u, report_type, type, code);
}

/** A consumer-page usage: the volume axis, AC Pan, or a key. */
static int map_consumer(struct hid_input *in, uint32_t id, unsigned *type, unsigned *code)
{
    if (id == 0x0e0) {
        *type = EV_ABS;
        *code = ABS_VOLUME;
        return MAP_CLEAR;
    }
    if (id == 0x238) { /* AC Pan */
        set_bit(in->rel, REL_HWHEEL);
        *type = EV_REL;
        *code = REL_HWHEEL_HI_RES;
        return MAP_SET;
    }
    *type = EV_KEY;
    *code = consumer_key(id);
    return *code ? MAP_CLEAR : MAP_NONE;
}

/** A usage of a page with nothing but keys: telephony, the HP vendor pages
 * and PID, or -1 for another page. */
static int map_keys_page(struct hid_input *in, uint32_t page, uint32_t id, unsigned *type,
                         unsigned *code)
{
    *type = EV_KEY;
    switch (page) {
    case HID_UP_TELEPHONY:
        if (id == 0x2f) {
            *code = KEY_MICMUTE;
            return MAP_CLEAR;
        }
        if (id >= 0xb0 && id <= 0xbf) {
            *code = KEY_NUMERIC_0 + (id - 0xb0);
            return MAP_CLEAR;
        }
        return MAP_NONE;
    case HID_UP_CAMERA:
        return map_table(camera_keys, COUNT(camera_keys), id, EV_KEY, type, code);
    case HID_UP_HPVENDOR:
        in->ev |= 1u << EV_REP;
        return map_table(hp_keys, COUNT(hp_keys), id, EV_KEY, type, code);
    case HID_UP_HPVENDOR2:
        in->ev |= 1u << EV_REP;
        return map_table(hp2_keys, COUNT(hp2_keys), id, EV_KEY, type, code);
    case HID_UP_CUSTOM:
        in->ev |= 1u << EV_REP;
        return MAP_NONE;
    case HID_UP_PID:
        *code = BTN_DEAD;
        return id == 0xa4 ? MAP_CLEAR : MAP_NONE;
    default:
        return -1;
    }
}

/**
 * Decide what `u`, a usage of field `f` on input device `in`, maps to, as
 * the page cases of Linux's `hidinput_configure_usage`. Sets `*type` and
 * `*code`, and may add EV_REP or a second relative axis to `in`, or set
 * `u`'s hat. Returns MAP_SET, MAP_CLEAR, MAP_NONE or MAP_PWR.
 */
static int map_usage(struct hid_dev *hid, struct hid_input *in, struct hid_field *f,
                     struct hid_usage *u, unsigned *type, unsigned *code)
{
    unsigned report_type = hid->desc.reports[f->report].type;
    uint32_t page = u->hid & HID_USAGE_PAGE;
    uint32_t id = u->hid & HID_USAGE;
    int map;

    switch (page) {
    case HID_UP_UNDEFINED:
    case HID_UP_DIGITIZER:
    case HID_UP_MSVENDOR:
    case HID_UP_LOGIVENDOR:
    case HID_UP_LOGIVENDOR2:
    case HID_UP_LOGIVENDOR3:
        return MAP_NONE;
    case HID_UP_GENDEVCTRLS:
    case HID_UP_BATTERY:
        /* Battery Strength, Absolute State Of Charge and Charging feed
         * Linux's power supply, which has no event; the rest are unknown. */
        if (u->hid == HID_DC_BATTERYSTRENGTH || u->hid == HID_BAT_ABSOLUTESTATEOFCHARGE ||
            u->hid == HID_BAT_CHARGING) {
            return MAP_PWR;
        }
        return map_unknown(f, report_type, type, code);
    case HID_UP_KEYBOARD:
        return map_keyboard(in, id, type, code);
    case HID_UP_BUTTON:
        return map_button(hid, f, u, type, code);
    case HID_UP_SIMULATION:
        map = map_table(simulation_axes, COUNT(simulation_axes), id, EV_ABS, type, code);
        return map == MAP_CLEAR ? MAP_SET : map;
    case HID_UP_GENDESK:
        return map_gendesk(in, f, u, report_type, type, code);
    case HID_UP_LED:
        map = map_table(led_codes, COUNT(led_codes), id, EV_LED, type, code);
        return map == MAP_CLEAR ? MAP_SET : map;
    case HID_UP_CONSUMER:
        return map_consumer(in, id, type, code);
    default:
        map = map_keys_page(in, page, id, type, code);
        return map >= 0 ? map : map_unknown(f, report_type, type, code);
    }
}

/** Whether `application` describes a touch surface. */
static int touch_surface(uint32_t application)
{
    return application == HID_DG_TOUCHSCREEN || application == HID_DG_TOUCHPAD;
}

/** Whether Input Mode permits publishing field `f`. */
static int input_field_enabled(const struct hid_dev *hid, const struct hid_field *f)
{
    return !(hid->mt_mode_failed && hid->desc.reports[f->report].multitouch &&
             touch_surface(f->application));
}

/** The number of usages to map. A variable contact field can only
 * report its declared data elements. */
static uint32_t input_usage_count(const struct hid_dev *hid, const struct hid_field *f)
{
    if (hid->desc.reports[f->report].multitouch && touch_surface(f->application) &&
        (f->flags & HID_MAIN_ITEM_VARIABLE)) {
        return f->count;
    }
    return f->maxusage;
}

/** The one-based Finger collection of `u`, or its application for a
 * serial report whose usages are directly in the top-level collection. */
static uint16_t mt_collection(const struct hid_dev *hid, const struct hid_usage *u)
{
    int i = u->collection;

    while (i >= 0 && (uint32_t)i < hid->desc.ncollections) {
        const struct hid_collection *c = &hid->desc.collections[i];

        if (c->usage == HID_DG_FINGER || c->type == HID_COLLECTION_APPLICATION) {
            return (uint16_t)(i + 1);
        }
        i = c->parent;
    }
    return 0;
}

/** The per-contact event axis for `u`, or zero for a control field. */
static unsigned mt_axis(const struct hid_usage *u, int duplicate)
{
    switch (u->hid) {
    case HID_GD_X:
        return duplicate ? ABS_MT_TOOL_X : ABS_MT_POSITION_X;
    case HID_GD_Y:
        return duplicate ? ABS_MT_TOOL_Y : ABS_MT_POSITION_Y;
    case HID_DG_TIPPRESSURE:
        return ABS_MT_PRESSURE;
    case HID_DG_WIDTH:
        return ABS_MT_TOUCH_MAJOR;
    case HID_DG_HEIGHT:
        return ABS_MT_TOUCH_MINOR;
    case HID_DG_AZIMUTH:
        return ABS_MT_ORIENTATION;
    case HID_DG_INRANGE:
        return ABS_MT_DISTANCE;
    case HID_DG_CONFIDENCE:
        return ABS_MT_TOOL_TYPE;
    default:
        return 0;
    }
}

/* Contact fields that do not use an axis code. */
#define MT_ID_BIT  (1u << HID_MT_AXES)
#define MT_TIP_BIT (MT_ID_BIT << 1)
#define MT_COORDS \
    ((1u << (ABS_MT_POSITION_X - ABS_MT_FIRST)) | (1u << (ABS_MT_POSITION_Y - ABS_MT_FIRST)))
#define MT_COMPLETE (MT_COORDS | MT_ID_BIT)

/** Give a field its contact record. A collection change or a repetition
 * after X, Y and Contact ID begins another record. */
static uint16_t mt_group(struct hid_report *r, uint16_t collection, uint32_t bit)
{
    if (!collection || !bit) {
        return 0;
    }
    if (r->mt_collection != collection ||
        ((r->mt_fields & bit) && (r->mt_fields & MT_COMPLETE) == MT_COMPLETE)) {
        if (r->mt_groups >= EVDEV_MT_SLOTS) {
            return 0;
        }
        r->mt_collection = collection;
        r->mt_fields = 0;
        r->mt_groups++;
    }
    if (r->mt_fields & bit) {
        return 0;
    }
    r->mt_fields |= bit;
    return r->mt_groups;
}

/** Map a touch-surface button, including the Precision Touchpad's two
 * sources of BTN_LEFT and its external BTN_RIGHT. */
static void mt_map_button(struct hid_input *in, const struct hid_field *f, struct hid_usage *u)
{
    unsigned source = (u->hid - 1) & HID_USAGE;
    unsigned code = BTN_LEFT + source;

    if (in->mt_precision && f->application == HID_DG_TOUCHPAD && code > BTN_LEFT) {
        code--;
    }
    if (code >= KEY_CNT) {
        return;
    }
    in->mt_buttons++;
    in->mt_flags |= INPUT_MT_POINTER;
    in->ev |= 1u << EV_KEY;
    set_bit(in->keys, code);
    u->type = EV_KEY;
    u->code = (uint16_t)code;
    if (in->mt_precision && source < 32) {
        u->mt_group = HID_MT_BUTTON;
        in->mt_button_codes |= 1u << (code - BTN_LEFT);
        in->mt_button_target[source] = (uint8_t)(code - BTN_LEFT);
    }
}

/** Map a usage of a multitouch report, with duplicate axes shared by
 * every contact. Return whether the generic mapper should skip it. */
static int configure_mt(struct hid_dev *hid, struct hid_input *in, struct hid_field *f,
                        struct hid_usage *u)
{
    struct hid_report *r = &hid->desc.reports[f->report];
    uint32_t bit = 0;
    unsigned code;
    int i;

    if (!hid->desc.reports[f->report].multitouch || !touch_surface(f->application) ||
        !(f->flags & HID_MAIN_ITEM_VARIABLE)) {
        return 0;
    }
    in->mt_flags |= f->application == HID_DG_TOUCHPAD ? INPUT_MT_POINTER : INPUT_MT_DIRECT;
    if ((u->hid & HID_USAGE_PAGE) == HID_UP_BUTTON) {
        mt_map_button(in, f, u);
        return 1;
    }
    if (u->hid == HID_DG_CONTACTCOUNT) {
        u->mt_group = HID_MT_COUNT;
        return 1;
    }
    i = (int)(u - usages_of(hid, f));
    code = mt_axis(u, i > 0 && u[-1].hid == u->hid && u[-1].collection == u->collection);
    if (!in->mt_precision && (code == ABS_MT_DISTANCE || code == ABS_MT_TOOL_TYPE)) {
        return 1;
    }
    if (code) {
        bit = 1u << (code - ABS_MT_FIRST);
    } else if (u->hid == HID_DG_CONTACTID) {
        bit = MT_ID_BIT;
    } else if (u->hid == HID_DG_TIPSWITCH) {
        bit = MT_TIP_BIT;
    }
    u->mt_group = mt_group(r, mt_collection(hid, u), bit);
    if (u->mt_group && code) {
        in->ev |= 1u << EV_ABS;
        set_bit(in->abs, code);
        u->type = EV_ABS;
        u->code = (uint16_t)code;
    }
    return bit != 0;
}

/**
 * Map usage `u`, the `index`th of field `f`, to an event of input device
 * `in`, as Linux's `hidinput_configure_usage`: a constant field, an empty
 * one and a usage of an output field other than an LED map to nothing; a
 * code the input device has already is a duplicate and maps to nothing,
 * unless its case clears the bit first; a battery usage is typed EV_PWR,
 * which no event reports and the keymap skips.
 */
static void configure_usage(struct hid_dev *hid, struct hid_input *in, struct hid_field *f,
                            struct hid_usage *u)
{
    const struct hid_report *r = &hid->desc.reports[f->report];
    unsigned type = 0, code = 0, max;
    uint8_t *bits;
    int how;

    u->type = 0;
    u->code = 0;
    u->mt_group = 0;
    if (f->flags & HID_MAIN_ITEM_CONSTANT) {
        return;
    }
    if (f->count < 1) {
        return;
    }
    if (r->type == HID_OUTPUT_REPORT && (u->hid & HID_USAGE_PAGE) != HID_UP_LED) {
        return;
    }
    if (configure_mt(hid, in, f, u)) {
        return;
    }
    how = map_usage(hid, in, f, u, &type, &code);
    if (how == MAP_PWR) {
        u->type = EV_PWR;
        return;
    }
    if (how == MAP_NONE) {
        return;
    }
    bits = bits_of(in, type, &max);
    if (!bits || code > max) {
        return;
    }
    if (how == MAP_CLEAR) {
        clear_bit(bits, code);
    }
    in->ev |= 1u << type;
    if (test_bit(bits, code)) {
        u->hat = 0;
        return;
    }
    set_bit(bits, code);
    u->type = (uint8_t)type;
    u->code = (uint16_t)code;
    if (type == EV_ABS && u->hat) {
        if (code + 1 <= max) {
            set_bit(bits, code + 1);
        }
    }
    if (type == EV_ABS && (f->flags & HID_MAIN_ITEM_RELATIVE) && code == ABS_VOLUME) {
        set_bit(in->keys, KEY_VOLUMEUP);
        set_bit(in->keys, KEY_VOLUMEDOWN);
    }
    if (type == EV_KEY) {
        in->ev |= 1u << EV_MSC;
        set_bit(in->msc, MSC_SCAN);
    }
}

/** `b - a` as Linux computes it, built with -fno-strict-overflow, so that
 * a range as wide as INT32_MIN to INT32_MAX wraps instead of overflowing. */
static int32_t wrap_sub(int32_t b, int32_t a)
{
    return (int32_t)((uint32_t)b - (uint32_t)a);
}

/**
 * An axis's resolution, in units per millimetre for a length and per radian
 * for an angle, from its logical and physical ranges and unit, as Linux's
 * `hidinput_calc_abs_res`; 0 when the unit is not one it converts.
 */
static int32_t abs_resolution(const struct hid_field *f, unsigned code)
{
    int32_t exponent = f->unit_exponent;
    int32_t logical = wrap_sub(f->logical_maximum, f->logical_minimum);
    int32_t physical = wrap_sub(f->physical_maximum, f->physical_minimum);
    int32_t prev;

    if (logical <= 0 || physical <= 0) {
        return 0;
    }
    switch (code) {
    case ABS_X:
    case ABS_Y:
    case ABS_Z:
    case ABS_MT_POSITION_X:
    case ABS_MT_POSITION_Y:
        if (f->unit == 0x11) { /* centimetres */
            exponent += 1;
        } else if (f->unit == 0x13) { /* inches */
            prev = physical;
            if (physical > 0x7FFFFFFF / 254) {
                return 0;
            }
            physical *= 254;
            if (physical < prev) {
                return 0;
            }
            exponent -= 1;
        } else {
            return 0;
        }
        break;
    case ABS_RX:
    case ABS_RY:
    case ABS_RZ:
    case ABS_WHEEL:
        if (f->unit == 0x14) { /* degrees */
            if (logical > 0x7FFFFFFF / 573) {
                return 0;
            }
            logical *= 573;
            exponent += 1;
        } else if (f->unit != 0x12) { /* radians */
            return 0;
        }
        break;
    default:
        return 0;
    }
    for (; exponent < 0; exponent++) {
        if (logical > 0x7FFFFFFF / 10) {
            return 0;
        }
        logical *= 10;
    }
    for (; exponent > 0; exponent--) {
        if (physical > 0x7FFFFFFF / 10) {
            return 0;
        }
        physical *= 10;
    }
    return (logical + physical / 2) / physical;
}

/** Whether `usage` is an application or physical collection input handles,
 * Linux's IS_INPUT_APPLICATION. */
static int input_application(uint32_t usage)
{
    return (usage >= HID_UP_GENDESK && usage <= HID_GD_MULTIAXIS) ||
           (usage >= HID_DG_DIGITIZER && usage <= HID_DG_WHITEBOARD) ||
           usage == HID_GD_SYSCONTROL || usage == HID_CP_CONSUMER || usage == HID_GD_WIRELESS;
}

/** The suffix hid-generic gives the input device of `application` on a
 * device of several applications, or NULL. */
static const char *suffix_of(uint32_t application)
{
    switch (application) {
    case HID_GD_KEYBOARD:
        return "Keyboard";
    case HID_GD_KEYPAD:
        return "Keypad";
    case HID_GD_MOUSE:
        return "Mouse";
    case HID_DG_PEN:
        return "Stylus";
    case HID_DG_STYLUS:
        return "Pen";
    case HID_DG_TOUCHSCREEN:
        return "Touchscreen";
    case HID_DG_TOUCHPAD:
        return "Touchpad";
    case HID_GD_SYSCONTROL:
        return "System Control";
    case HID_CP_CONSUMER:
        return "Consumer Control";
    case HID_GD_WIRELESS:
        return "Wireless Radio Control";
    case HID_GD_SYSMULTIAXIS:
        return "System Multi Axis";
    default:
        return 0;
    }
}

/** Append `text` to the string `out` of `size` bytes, cut to fit. */
static void append(char *out, size_t size, const char *text)
{
    size_t i = k_strlen(out);

    while (*text && i + 1 < size) {
        out[i++] = *text++;
    }
    out[i] = 0;
}

/** Whether the strings `a` and `b` are equal. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** A new input device of `hid` for `application`, named as Linux's
 * `hidinput_allocate` names it; NULL when HID_MAX_INPUTS are made. */
static struct hid_input *allocate_input(struct hid_dev *hid, uint32_t application)
{
    struct hid_input *in;
    const char *suffix = 0;
    size_t name_len, suffix_len;

    if (hid->ninputs == HID_MAX_INPUTS) {
        return 0;
    }
    in = &hid->inputs[hid->ninputs++];
    k_memset(in, 0, sizeof(*in));
    in->application = application;
    in->mt_precision = hid->mt_precision;
    in->mt_drop_unused = hid->mt_drop_unused;
    if (hid->desc.maxapplication > 1) {
        suffix = suffix_of(application);
    }
    in->name[0] = 0;
    append(in->name, sizeof(in->name), hid->name);
    if (suffix) {
        name_len = k_strlen(hid->name);
        suffix_len = k_strlen(suffix);
        if (name_len < suffix_len || !same(hid->name + name_len - suffix_len, suffix)) {
            append(in->name, sizeof(in->name), " ");
            append(in->name, sizeof(in->name), suffix);
        }
    }
    return in;
}

/** The input device of `hid` that a report of `application` joins, Linux's
 * `hidinput_match_application`, or NULL. */
static struct hid_input *match_application(struct hid_dev *hid, uint32_t application)
{
    uint32_t i;

    for (i = 0; i < hid->ninputs; i++) {
        struct hid_input *in = &hid->inputs[i];

        if (in->application == application) {
            return in;
        }
        if ((application == HID_GD_SYSCONTROL || application == HID_CP_CONSUMER) &&
            in->application == HID_GD_KEYBOARD) {
            return in;
        }
    }
    return 0;
}

/** Whether input device `in` maps anything, Linux's
 * `hidinput_has_been_populated`. */
static int populated(const struct hid_input *in)
{
    unsigned i;
    unsigned any = in->ev | in->rel[0] | in->rel[1] | in->led[0] | in->led[1] | in->msc[0];

    for (i = 0; i < sizeof(in->keys); i++) {
        any |= in->keys[i];
    }
    for (i = 0; i < sizeof(in->abs); i++) {
        any |= in->abs[i];
    }
    return any != 0;
}

/** The index of `in` in `hid`'s input devices. */
static uint8_t input_index(struct hid_dev *hid, const struct hid_input *in)
{
    return (uint8_t)(in - hid->inputs);
}

/** The multiplier the Resolution Multiplier field `f` gives with its value
 * `v`, Linux's `hid_calculate_multiplier`; the one quotient that traps on
 * Linux, INT32_MIN over -1, gives 1. */
static int calculate_multiplier(const struct hid_field *f, int32_t v)
{
    int32_t range = wrap_sub(f->logical_maximum, f->logical_minimum);
    int32_t dist = wrap_sub(v, f->logical_minimum);
    int32_t m;

    if (range == 0 || (range == -1 && dist == -0x7fffffffL - 1)) {
        return 1;
    }
    m = (int32_t)((uint32_t)(dist / range) *
                      (uint32_t)wrap_sub(f->physical_maximum, f->physical_minimum) +
                  (uint32_t)f->physical_minimum);
    if (m == 0 || m > 255 || m < -255) {
        m = 1;
    }
    return (int)m;
}

/** The collection `index` of `hid` with its parent, or a top-level one of
 * type 0 when the descriptor has none, as Linux's zeroed first entry. */
static void collection_at(struct hid_dev *hid, int index, uint8_t *type, int *parent)
{
    if (index < 0 || (uint32_t)index >= hid->desc.ncollections) {
        *type = 0;
        *parent = -1;
        return;
    }
    *type = hid->desc.collections[index].type;
    *parent = hid->desc.collections[index].parent;
}

/**
 * Give the wheels the multiplier field `m` governs its value `v`, as
 * Linux's `hid_apply_multiplier`: the usages of the input reports whose
 * collections lie in the logical collection that holds `m`.
 */
static void apply_multiplier(struct hid_dev *hid, const struct hid_field *m, int32_t v)
{
    int effective = calculate_multiplier(m, v);
    int mc = usages_of(hid, m)[0].collection;
    uint8_t type;
    int parent;
    uint32_t i, j;

    collection_at(hid, mc, &type, &parent);
    while (parent != -1 && type != HID_COLLECTION_LOGICAL) {
        mc = parent;
        collection_at(hid, mc, &type, &parent);
    }
    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (hid->desc.reports[f->report].type != HID_INPUT_REPORT) {
            continue;
        }
        for (j = 0; j < f->maxusage; j++) {
            struct hid_usage *u = &usages_of(hid, f)[j];
            int c = u->collection;

            collection_at(hid, c, &type, &parent);
            while (parent != -1 && c != mc) {
                c = parent;
                collection_at(hid, c, &type, &parent);
            }
            if (parent != -1) {
                u->multiplier = (int16_t)effective;
            }
        }
    }
}

/** Each Resolution Multiplier field of `hid` with value `v`, or with its
 * logical maximum when `use_max`, applied, as Linux's
 * `hid_setup_resolution_multiplier`. */
static void setup_multipliers(struct hid_dev *hid, int use_max)
{
    uint32_t i, j;

    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (hid->desc.reports[f->report].type != HID_FEATURE_REPORT || f->count < 1) {
            continue;
        }
        for (j = 0; j < f->maxusage; j++) {
            if (usages_of(hid, f)[j].hid == HID_GD_RESOLUTION) {
                apply_multiplier(hid, f, use_max ? f->logical_maximum : usages_of(hid, f)[0].value);
            }
        }
    }
}

/**
 * The usage of report `r` of `hid` that keymap entry `ke` names, counting
 * the places of the usages mapped to a key or to nothing in `*at` over the
 * report's fields in turn. NULL when the report has none.
 */
static struct hid_usage *find_key_in(struct hid_dev *hid, uint32_t r,
                                     const struct evdev_keymap_entry *ke, uint32_t *at)
{
    uint32_t i, j;

    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (f->report != r || !input_field_enabled(hid, f)) {
            continue;
        }
        for (j = 0; j < input_usage_count(hid, f); j++) {
            struct hid_usage *u = &usages_of(hid, f)[j];

            if (u->type != EV_KEY && u->type != 0) {
                continue;
            }
            if (ke->by_index ? *at == ke->index : u->hid == ke->scancode) {
                return u;
            }
            ++*at;
        }
    }
    return NULL;
}

/**
 * The usage of `hid` that keymap entry `ke` names, as Linux's
 * hidinput_find_key finds it: over the input reports, then the output
 * reports, each in the order the descriptor first names it, and over each
 * report's fields in turn, the usages mapped to a key or to nothing are
 * counted, and the one at place `ke->index`, or with usage `ke->scancode`,
 * is the entry, its place stored in `*index`. NULL for none.
 */
static struct hid_usage *find_key(struct hid_dev *hid, const struct evdev_keymap_entry *ke,
                                  uint32_t *index)
{
    struct hid_usage *u;
    uint32_t type, r, at = 0;

    for (type = HID_INPUT_REPORT; type <= HID_OUTPUT_REPORT; type++) {
        for (r = 0; r < hid->desc.nreports; r++) {
            if (hid->desc.reports[r].type != type) {
                continue;
            }
            u = find_key_in(hid, r, ke, &at);
            if (u) {
                *index = at;
                return u;
            }
        }
    }
    return NULL;
}

/** EVIOCGKEYCODE of a HID device, Linux's hidinput_getkeycode: the entry's
 * usage is its scan code, and an unmapped one has KEY_RESERVED. */
static int keymap_get(void *ctx, struct evdev_keymap_entry *ke)
{
    struct hid_usage *u = find_key((struct hid_dev *)ctx, ke, &ke->index);

    if (!u) {
        return -EINVAL;
    }
    ke->scancode = u->hid;
    ke->keycode = u->type == EV_KEY ? u->code : 0;
    return 0;
}

/** EVIOCSKEYCODE of a HID device, Linux's hidinput_setkeycode: the usage
 * becomes that key. Precision frame buttons have fixed codes. */
static int keymap_set(void *ctx, const struct evdev_keymap_entry *ke, uint16_t *old)
{
    uint32_t index;
    struct hid_usage *u = find_key((struct hid_dev *)ctx, ke, &index);

    if (!u || u->mt_group == HID_MT_BUTTON) {
        return -EINVAL;
    }
    *old = u->type == EV_KEY ? u->code : 0;
    u->code = ke->keycode;
    u->type = EV_KEY;
    return 0;
}

/** Whether a usage of the HID device at `ctx` is mapped to key `code`. */
static int keymap_maps(void *ctx, uint16_t code)
{
    struct hid_dev *hid = (struct hid_dev *)ctx;
    uint32_t i, j;

    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (!input_field_enabled(hid, f) ||
            hid->desc.reports[f->report].type == HID_FEATURE_REPORT) {
            continue;
        }
        for (j = 0; j < input_usage_count(hid, f); j++) {
            const struct hid_usage *u = &usages_of(hid, f)[j];

            if (u->type == EV_KEY && u->code == code) {
                return 1;
            }
        }
    }
    return 0;
}

/** Set up the slots using the descriptor's Contact Count Maximum bound,
 * or the ten slots Linux uses when it has no maximum. */
static int setup_mt_slots(struct hid_dev *hid, struct hid_input *in, struct evdev *dev)
{
    int slots = HID_MT_DEFAULT;
    uint32_t i, j;

    for (i = 0; i < hid->desc.nfields; i++) {
        const struct hid_field *f = field_at(hid, i);
        const struct hid_usage *u = usages_of(hid, f);

        if (hid->desc.reports[f->report].type != HID_FEATURE_REPORT ||
            (f->application != in->application && touch_surface(f->application))) {
            continue;
        }
        for (j = 0; j < f->maxusage; j++) {
            if (u[j].hid == HID_DG_CONTACTMAX) {
                int32_t reported = u[j].value;

                if (reported <= 0 || reported > EVDEV_MT_SLOTS) {
                    reported = f->logical_maximum;
                }
                if (reported > 0 && reported <= EVDEV_MT_SLOTS) {
                    slots = reported;
                }
            }
        }
    }
    if ((in->mt_flags & INPUT_MT_POINTER) && in->mt_buttons == 1) {
        evdev_set_prop(dev, INPUT_PROP_BUTTONPAD);
    }
    if (test_bit(in->abs, ABS_MT_TOUCH_MINOR) && !test_bit(in->abs, ABS_MT_ORIENTATION)) {
        evdev_set_abs(dev, ABS_MT_ORIENTATION, 0, 1, 0, 0);
    }
    return evdev_init_slots(dev, slots, in->mt_flags);
}

/** Set the absolute axes in descriptor order. */
static void register_axes(struct hid_dev *hid, struct hid_input *in, struct evdev *dev)
{
    uint32_t i, j;

    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (f->input != input_index(hid, in)) {
            continue;
        }
        for (j = 0; j < f->maxusage; j++) {
            struct hid_usage *u = &usages_of(hid, f)[j];
            int32_t a = f->logical_minimum;
            int32_t b = f->logical_maximum;

            if (u->type != EV_ABS) {
                continue;
            }
            if (u->mt_group && u->code == ABS_MT_TOOL_TYPE) {
                evdev_set_abs(dev, u->code, MT_TOOL_FINGER, MT_TOOL_PALM, 0, 0);
            } else if (u->mt_group && u->code == ABS_MT_ORIENTATION) {
                evdev_set_abs(dev, u->code, -(b / 4), b / 4, 0, 0);
            } else if (f->application == HID_GD_GAMEPAD || f->application == HID_GD_JOYSTICK) {
                evdev_set_abs(dev, u->code, a, b, wrap_sub(b, a) >> 8, wrap_sub(b, a) >> 4);
            } else {
                evdev_set_abs(dev, u->code, a, b, 0, 0);
            }
            evdev_set_abs_res(dev, u->code, abs_resolution(f, u->code));
            if (u->hat || u->hat_dir) {
                evdev_set_abs(dev, u->code, -1, 1, 0, 0);
                if (u->code + 1 <= ABS_MAX) {
                    evdev_set_abs(dev, (uint16_t)(u->code + 1), -1, 1, 0, 0);
                }
            }
        }
    }
}

/** Register input device `in` of `hid` with its capabilities. Return 1
 * on success or 0 if evdev could not take it. */
static int register_input(struct hid_dev *hid, struct hid_input *in)
{
    struct evdev_keymap km;
    struct evdev *dev;
    unsigned code;

    dev = in->mt_flags
              ? evdev_reserve()
              : evdev_register(in->name, EVDEV_BUS_USB, hid->vendor, hid->product, in->keys);
    if (!dev) {
        return 0;
    }
    if (in->ev & (1u << EV_REP)) {
        evdev_describe(dev, hid->phys, EVDEV_SCAN | EVDEV_KEYBOARD);
        if (!in->mt_flags) {
            input_keyboard_attach_dev(&in->kb, dev);
        }
    } else {
        evdev_describe(dev, hid->phys, test_bit(in->msc, MSC_SCAN) ? EVDEV_SCAN : 0);
    }
    km.get = keymap_get;
    km.set = keymap_set;
    km.maps = keymap_maps;
    km.ctx = hid;
    evdev_set_keymap(dev, &km);
    evdev_set_uniq(dev, hid->uniq);
    evdev_set_version(dev, hid->version);
    for (code = 0; code <= REL_MAX; code++) {
        if (test_bit(in->rel, code)) {
            evdev_set_capability(dev, EV_REL, (uint16_t)code);
        }
    }
    for (code = 0; code <= LED_MAX; code++) {
        if (test_bit(in->led, code)) {
            evdev_set_capability(dev, EV_LED, (uint16_t)code);
        }
    }
    register_axes(hid, in, dev);
    if (in->mt_flags) {
        if (setup_mt_slots(hid, in, dev) != 0) {
            evdev_unregister(dev);
            return 0;
        }
        evdev_register_reserved(dev, in->name, EVDEV_BUS_USB, hid->vendor, hid->product, in->keys);
        if (in->ev & (1u << EV_REP)) {
            input_keyboard_attach_dev(&in->kb, dev);
        }
    }
    in->dev = dev;
    in->mouse = !(in->ev & (1u << EV_REP)) && test_bit(in->rel, REL_X) && test_bit(in->rel, REL_Y);
    if (in->mouse) {
        /* An open of `mouse` fails until a mouse of any kind is found. */
        input_mouse_found();
    }
    return 1;
}

/** Whether this USB device reports its active contacts without explicit
 * lifts, as specified in Linux 6.12's hid-multitouch device table. */
static int mt_drop_unused(const struct hid_dev *hid)
{
    static const uint32_t ids[] = {
        0x06030600u,                           /* Novatek PCT */
        0x0fb81109u,                           /* Wistron optical touch */
        0x14771006u, 0x14771007u, 0x1477100eu, /* Xiroku */
        0x14771021u, 0x14771022u, 0x14771023u, 0x14771024u,
        0x14771025u, 0x14771026u, 0x1aad000fu, /* GoodTouch */
        0x1e5e0313u,                           /* Touch International */
        0x222a0001u,                           /* Ilitek dual touch */
        0x227d0709u, 0x227d0a19u,              /* Unitec */
        0x25050220u,                           /* XAT CSR */
        0x25b50002u,                           /* Flatfrog 3200 */
    };
    uint32_t id = ((uint32_t)hid->vendor << 16) | hid->product;
    unsigned int i;

    for (i = 0; i < COUNT(ids); i++) {
        if (id == ids[i]) {
            return 1;
        }
    }
    return hid->mt_precision && (id == 0x17ef60feu || id == 0x17ef61aeu || id == 0x046dc548u);
}

/** Mark reports carrying Contact Identifier and identify the profile. */
static int detect_mt_reports(struct hid_dev *hid)
{
    uint32_t i, j;
    int any = 0;

    for (i = 0; i < hid->desc.nfields; i++) {
        const struct hid_field *f = field_at(hid, i);
        struct hid_report *r = &hid->desc.reports[f->report];
        const struct hid_usage *u = usages_of(hid, f);

        if (r->type == HID_FEATURE_REPORT && f->count && u[0].hid == HID_MS_MT_FEATURE) {
            hid->mt_precision = 1;
        }
        if (r->type != HID_INPUT_REPORT || !touch_surface(f->application) ||
            (f->flags & HID_MAIN_ITEM_CONSTANT) || !(f->flags & HID_MAIN_ITEM_VARIABLE)) {
            continue;
        }
        for (j = 0; j < f->count; j++) {
            if (u[j].hid == HID_DG_CONTACTID) {
                r->multitouch = 1;
                any = 1;
            }
        }
    }
    hid->mt_drop_unused = mt_drop_unused(hid);
    return any;
}

/* Feature reports to read, and whether their value can be built from
 * zero when the device refuses GET_REPORT. */
#define MT_FEATURE_READ  1u
#define MT_FEATURE_MODE  2u
#define MT_FEATURE_OTHER 4u

/** The relevant fields in feature report `report`, including fields that
 * must be preserved by a SET_REPORT. */
static unsigned mt_feature_fields(struct hid_dev *hid, unsigned report)
{
    unsigned int i, j, flags = 0;

    for (i = 0; i < hid->desc.nfields; i++) {
        const struct hid_field *f = field_at(hid, i);
        const struct hid_usage *u = usages_of(hid, f);

        if (f->report != report || (f->flags & HID_MAIN_ITEM_CONSTANT)) {
            continue;
        }
        for (j = 0; j < f->count; j++) {
            if ((f->flags & HID_MAIN_ITEM_VARIABLE) && u[j].hid == HID_DG_INPUTMODE) {
                flags |= MT_FEATURE_MODE;
            } else {
                flags |= MT_FEATURE_OTHER;
                if (u[j].hid == HID_DG_CONTACTMAX || u[j].hid == HID_MS_MT_FEATURE) {
                    flags |= MT_FEATURE_READ;
                }
            }
        }
    }
    return flags;
}

int hidinput_mt_features(struct hid_dev *hid)
{
    unsigned int r;

    hid->mt_feature = hid->mt_set_feature = -1;
    if (!detect_mt_reports(hid)) {
        return 0;
    }
    for (r = 0; r < hid->desc.nreports; r++) {
        if (hid->desc.reports[r].type == HID_FEATURE_REPORT &&
            (mt_feature_fields(hid, r) & (MT_FEATURE_READ | MT_FEATURE_MODE))) {
            return 1;
        }
    }
    return 0;
}

int hidinput_mt_feature_request(struct hid_dev *hid, uint8_t *data, int size, int *value,
                                int *write)
{
    const struct hid_report *r;
    unsigned int i;
    int len;

    *write = hid->mt_set_feature >= 0;
    if (*write) {
        i = (unsigned int)hid->mt_set_feature;
        hid->mt_set_feature = -1;
    } else {
        for (i = hid->mt_next_feature; i < hid->desc.nreports; i++) {
            r = &hid->desc.reports[i];
            if (r->type == HID_FEATURE_REPORT && hid_report_len(r) <= (uint32_t)size &&
                (mt_feature_fields(hid, i) & (MT_FEATURE_READ | MT_FEATURE_MODE))) {
                break;
            }
        }
        if (i == hid->desc.nreports) {
            return 0;
        }
        hid->mt_next_feature = i + 1;
    }
    r = &hid->desc.reports[i];
    len = (int)hid_report_len(r);
    if (*write && len <= size) {
        k_memcpy(data, hid->outbuf, (uint32_t)len);
    }
    hid->mt_feature = (int)i;
    hid->mt_feature_write = *write;
    *value = (HID_FEATURE_REPORT + 1) << 8 | r->id;
    return len <= size ? len : 0;
}

/** The Input Mode value of a touchscreen (2) or a touchpad (3). */
static int mt_input_mode(struct hid_dev *hid)
{
    unsigned int i, j;

    for (i = 0; i < hid->desc.nfields; i++) {
        const struct hid_field *f = field_at(hid, i);
        const struct hid_usage *u = usages_of(hid, f);

        if (!hid->desc.reports[f->report].multitouch || (f->flags & HID_MAIN_ITEM_CONSTANT)) {
            continue;
        }
        if (f->application == HID_DG_TOUCHPAD) {
            return 3;
        }
        for (j = 0; f->application == HID_DG_TOUCHSCREEN && j < f->count; j++) {
            if ((u[j].hid & HID_USAGE_PAGE) == HID_UP_BUTTON) {
                return 3;
            }
        }
    }
    return 2;
}

/** Read a contact limit or replace Input Mode in the saved feature
 * packet. Only the first `bits` were read back. Return whether to send
 * SET_REPORT. */
static int mt_update_feature(struct hid_dev *hid, unsigned int report, uint8_t *data,
                             unsigned int bits, int can_write)
{
    const uint32_t mode = (uint32_t)mt_input_mode(hid);
    unsigned int i, j;
    int set = 0;

    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);
        struct hid_usage *u = usages_of(hid, f);

        if (f->report != report || !(f->flags & HID_MAIN_ITEM_VARIABLE) ||
            (f->flags & HID_MAIN_ITEM_CONSTANT)) {
            continue;
        }
        for (j = 0; j < f->count; j++) {
            unsigned int offset = f->offset + j * f->size;
            int present = offset + f->size <= bits;

            if (present && u[j].hid == HID_DG_CONTACTMAX) {
                uint32_t raw = hid_field_extract(data, offset, f->size);

                u[j].value = f->logical_minimum < 0 ? hid_snto32(raw, f->size) : (int32_t)raw;
            } else if (u[j].hid == HID_DG_INPUTMODE) {
                if (present && hid_field_extract(data, offset, f->size) == mode) {
                    continue;
                }
                if (f->size < 2 || !can_write) {
                    hid->mt_mode_failed = 1;
                } else {
                    hid_field_implement(data, offset, f->size, mode);
                    set = 1;
                }
            }
        }
    }
    return set;
}

void hidinput_mt_feature_answer(struct hid_dev *hid, const uint8_t *data, int len)
{
    int index = hid->mt_feature;
    const struct hid_report *r;
    int prefix, bytes, can_write;
    unsigned int bits = 0;

    hid->mt_feature = -1;
    if (index < 0) {
        return;
    }
    r = &hid->desc.reports[index];
    bytes = (int)hid_report_len(r);
    if (hid->mt_feature_write) {
        hid->mt_mode_failed |= len != bytes;
        return;
    }
    prefix = r->id != 0;
    if (!data || len < prefix || (prefix && data[0] != r->id)) {
        len = 0;
    }
    if (len > bytes) {
        len = bytes;
    }
    k_memset(hid->outbuf, 0, (uint32_t)bytes);
    if (len > 0) {
        k_memcpy(hid->outbuf, data, (uint32_t)len);
        bits = (unsigned int)(len - prefix) * 8;
    }
    if (prefix) {
        hid->outbuf[0] = r->id;
    }
    can_write = len == bytes || mt_feature_fields(hid, (unsigned int)index) == MT_FEATURE_MODE;
    if (mt_update_feature(hid, (unsigned int)index, hid->outbuf + prefix, bits, can_write)) {
        hid->mt_set_feature = index;
    }
}

/** Set aside enough contact records for the largest multitouch report. */
static int mt_workspace(struct hid_dev *hid)
{
    unsigned int i, contacts = 0;

    for (i = 0; i < hid->desc.nreports; i++) {
        if (hid->desc.reports[i].mt_groups > contacts) {
            contacts = hid->desc.reports[i].mt_groups;
        }
    }
    if (contacts) {
        hid->contacts = kzalloc(contacts * sizeof(*hid->contacts));
        if (!hid->contacts) {
            return -ENOMEM;
        }
    }
    return 0;
}

/** Assign a field and map its usages to input device `in`. */
static void configure_input_field(struct hid_dev *hid, struct hid_input *in, struct hid_field *f)
{
    uint32_t i, count = input_usage_count(hid, f);

    f->input = input_index(hid, in);
    for (i = 0; i < count; i++) {
        configure_usage(hid, in, f, &usages_of(hid, f)[i]);
    }
}

/** Map fields in a shared multitouch report to their own applications. */
static void configure_mixed_report(struct hid_dev *hid, uint32_t report)
{
    uint32_t i, app;

    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);
        struct hid_input *in;

        if (f->report != report || !input_field_enabled(hid, f)) {
            continue;
        }
        app = f->application ? f->application : hid->desc.reports[report].application;
        in = match_application(hid, app);
        if (!in) {
            in = allocate_input(hid, app);
        }
        if (in) {
            configure_input_field(hid, in, f);
        }
    }
}

/** Map input fields before output fields so LEDs can join the keyboard. */
static void configure_reports(struct hid_dev *hid)
{
    struct hid_input *in = NULL;
    uint32_t i, k, f;
    int per_app = hid->desc.maxapplication > 1;

    for (k = HID_INPUT_REPORT; k <= HID_OUTPUT_REPORT; k++) {
        for (i = 0; i < hid->desc.nreports; i++) {
            struct hid_report *r = &hid->desc.reports[i];
            int fields = 0;

            if (r->type != k) {
                continue;
            }
            if (r->multitouch && per_app) {
                configure_mixed_report(hid, i);
                continue;
            }
            for (f = 0; f < hid->desc.nfields; f++) {
                const struct hid_field *field = field_at(hid, f);

                fields += field->report == i && input_field_enabled(hid, field);
            }
            if (!fields) {
                continue;
            }
            if (per_app) {
                in = match_application(hid, r->application);
            }
            if (!in) {
                in = allocate_input(hid, r->application);
                if (!in) {
                    break;
                }
            }
            for (f = 0; f < hid->desc.nfields; f++) {
                struct hid_field *field = field_at(hid, f);

                if (field->report == i && input_field_enabled(hid, field)) {
                    configure_input_field(hid, in, field);
                }
            }
        }
    }
}

int hidinput_connect(struct hid_dev *hid)
{
    uint32_t i, f, registered = 0;

    hid->ninputs = 0;
    hid->multiplier = -1;
    for (i = 0; i < hid->desc.ncollections; i++) {
        const struct hid_collection *c = &hid->desc.collections[i];

        if ((c->type == HID_COLLECTION_APPLICATION || c->type == HID_COLLECTION_PHYSICAL) &&
            input_application(c->usage)) {
            break;
        }
    }
    if (i == hid->desc.ncollections) {
        return -1;
    }
    (void)detect_mt_reports(hid);
    setup_multipliers(hid, 0);
    configure_reports(hid);
    if (mt_workspace(hid) != 0) {
        return -1;
    }
    for (i = 0; i < hid->ninputs; i++) {
        struct hid_input *it = &hid->inputs[i];

        if (populated(it) && register_input(hid, it)) {
            registered++;
        } else {
            /* Its fields belong to no input device. */
            for (f = 0; f < hid->desc.nfields; f++) {
                if (field_at(hid, f)->input == i) {
                    field_at(hid, f)->input = HID_NO_INPUT;
                }
            }
        }
    }
    if (!registered) {
        kfree(hid->contacts);
        hid->contacts = NULL;
        return -1;
    }
    return (int)registered;
}

void hidinput_disconnect(struct hid_dev *hid)
{
    uint32_t i;

    for (i = 0; i < hid->ninputs; i++) {
        struct hid_input *in = &hid->inputs[i];

        if (in->ev & (1u << EV_REP)) {
            input_keyboard_detach(&in->kb);
        } else {
            evdev_unregister(in->dev);
        }
        in->dev = 0;
    }
    hid->ninputs = 0;
    kfree(hid->contacts);
    hid->contacts = NULL;
}

/** Report event `type`, `code`, `value` of input device `in`; a keyboard's
 * keys go through the input layer, which reports MSC_SCAN `scan` before a
 * key that changed, as hid-input does. */
static void input_event(struct hid_input *in, unsigned type, unsigned code, int32_t value,
                        uint32_t scan)
{
    if (type == EV_KEY && code < KEY_CNT) {
        int down = value != 0;

        if (in->ev & (1u << EV_REP)) {
            input_key_event(&in->kb, scan, code, down);
            usb_stamp_wake();
            if (down) {
                set_bit(in->down, code);
            } else {
                clear_bit(in->down, code);
            }
            return;
        }
        if ((!test_bit(in->down, code)) == value) {
            evdev_report(in->dev, EV_MSC, MSC_SCAN, (int32_t)scan);
        }
        if (value != 2) {
            if (down) {
                set_bit(in->down, code);
            } else {
                clear_bit(in->down, code);
            }
        }
        if (code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE) {
            unsigned bit = 1u << (code - BTN_LEFT);

            in->buttons = down ? in->buttons | bit : in->buttons & ~bit;
        }
    } else if (type == EV_REL && code == REL_X) {
        in->dx += value;
    } else if (type == EV_REL && code == REL_Y) {
        in->dy += value;
    }
    evdev_report(in->dev, (uint16_t)type, (uint16_t)code, value);
}

/** A wheel's turn `value` on a hi-res axis: whole detents on the low-res
 * axis and 120ths on the hi-res one, as Linux's `hidinput_handle_scroll`. */
static void handle_scroll(struct hid_input *in, struct hid_usage *u, int32_t value)
{
    int32_t hi_res, lo_res;
    unsigned code = u->code == REL_WHEEL_HI_RES ? REL_WHEEL : REL_HWHEEL;

    if (value == 0) {
        return;
    }
    hi_res = value * 120 / (u->multiplier ? u->multiplier : 1);
    u->wheel += hi_res;
    lo_res = u->wheel / 120;
    if (lo_res) {
        u->wheel -= lo_res * 120;
    }
    input_event(in, EV_REL, code, lo_res, 0);
    input_event(in, EV_REL, u->code, hi_res, 0);
}

/**
 * The direction, 1 to 8 or 0 for centred, of hat switch field `f` at
 * `value`, Linux's `(value - hat_min) * 8 / (hat_max - hat_min + 1) + 1`,
 * with its minimum and maximum cut to 16 bits as Linux keeps them, so that
 * the divisor is from 2 to 65536, and the product wrapped to 32 bits as
 * Linux, built with -fno-strict-overflow, wraps it. The caller centres a
 * result past 8 or below 0.
 */
static int hat_direction(const struct hid_field *f, int32_t value)
{
    int32_t min = (int16_t)f->logical_minimum, max = (int16_t)f->logical_maximum;
    int32_t product = (int32_t)(((uint32_t)value - (uint32_t)min) * 8u);

    return (int)(product / (max - min + 1) + 1);
}

/** The event of hat switch or D-pad usage `u` of field `f` at `value`: a
 * direction as the two axes of its code and the next. */
static void hat_event(struct hid_input *in, const struct hid_field *f, const struct hid_usage *u,
                      int32_t value)
{
    int dir = u->hat_dir;

    if (!dir) {
        dir = hat_direction(f, value);
    }
    if (dir < 0 || dir > 8) {
        dir = 0;
    }
    input_event(in, u->type, u->code, hat_to_axis[dir].x, 0);
    input_event(in, u->type, u->code + 1u, hat_to_axis[dir].y, 0);
}

/** Bring `*value` of variable field `f` into its logical range: returns 0
 * when it is to be ignored, out of range where the field has a null state,
 * and clamps it where it has not, as Microsoft's driver does. */
static int in_range(const struct hid_field *f, int32_t *value)
{
    if (!(f->flags & HID_MAIN_ITEM_VARIABLE) || f->logical_minimum >= f->logical_maximum) {
        return 1;
    }
    if (*value < f->logical_minimum || *value > f->logical_maximum) {
        if (f->flags & HID_MAIN_ITEM_NULL_STATE) {
            return 0;
        }
        *value = *value < f->logical_minimum ? f->logical_minimum : f->logical_maximum;
    }
    return 1;
}

/** A relative volume's `value` as presses of Volume Up or Volume Down,
 * each pressed and released in a report of its own. */
static void volume_steps(struct hid_input *in, const struct hid_usage *u, int32_t value)
{
    int count = value < 0 ? -value : value;
    unsigned key = value > 0 ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;
    int n;

    for (n = 0; n < count; n++) {
        input_event(in, EV_KEY, key, 1, u->hid);
        evdev_report(in->dev, EV_SYN, SYN_REPORT, 0);
        input_event(in, EV_KEY, key, 0, u->hid);
        evdev_report(in->dev, EV_SYN, SYN_REPORT, 0);
    }
}

/** Give the event of usage `u` of field `f` at `value` when it is one of
 * the kinds handled apart: a key with no code, which gives nothing, a
 * hi-res wheel and a relative volume. Returns whether it was. */
static int special_event(struct hid_input *in, const struct hid_field *f, struct hid_usage *u,
                         int32_t value)
{
    if (u->type == EV_KEY && u->code == 0) {
        return 1;
    }
    if (u->type == EV_REL && (u->code == REL_WHEEL_HI_RES || u->code == REL_HWHEEL_HI_RES)) {
        handle_scroll(in, u, value);
        return 1;
    }
    if (u->type == EV_ABS && (f->flags & HID_MAIN_ITEM_RELATIVE) && u->code == ABS_VOLUME) {
        volume_steps(in, u, value);
        return 1;
    }
    return 0;
}

/** Collect a contact field or the frame's count from a HID report. */
static void mt_collect(struct hid_dev *hid, struct hid_input *in, const struct hid_field *f,
                       const struct hid_usage *u, int32_t value)
{
    struct hid_mt_contact *c;
    unsigned int axis;
    int32_t full;

    if (u->mt_group == HID_MT_COUNT) {
        in->mt_count = value >= 0 && value <= EVDEV_MT_SLOTS ? value : -1;
        return;
    }
    if (u->mt_group == HID_MT_BUTTON) {
        unsigned int mask = 1u << ((u->hid - 1) & HID_USAGE);

        in->mt_button_fields |= mask;
        if (value) {
            in->mt_pressed |= mask;
        }
        return;
    }
    c = &hid->contacts[u->mt_group - 1];
    if (!c->seen) {
        c->seen = 1;
        c->confidence = 1;
        if (in->mt_last) {
            hid->contacts[in->mt_last - 1].next = u->mt_group;
        } else {
            in->mt_first = u->mt_group;
        }
        in->mt_last = u->mt_group;
    }
    if (u->hid == HID_DG_CONTACTID) {
        c->id = value;
        c->identified = 1;
    } else if (u->hid == HID_DG_TIPSWITCH) {
        c->tip = value != 0;
    } else if (u->hid == HID_DG_CONFIDENCE) {
        c->confidence = value != 0;
    } else if (u->hid == HID_DG_INRANGE) {
        c->in_range = value != 0;
    } else {
        if (u->hid == HID_DG_AZIMUTH) {
            full = (f->logical_maximum / 4) * 4;
            if (value >= 0 && value <= f->logical_maximum) {
                value = value > full / 2 ? full - value : -value;
            } else {
                value = 0;
            }
        }
        axis = u->code - ABS_MT_FIRST;
        c->value[axis] = value;
        c->axes |= (uint16_t)(1u << axis);
    }
}

/** Give one packet's contact to the slot keyed by its HID identifier. */
static void mt_contact(struct hid_input *in, struct hid_mt_contact *c)
{
    static const uint16_t axes[] = {ABS_MT_POSITION_X,  ABS_MT_POSITION_Y,  ABS_MT_TOOL_X,
                                    ABS_MT_TOOL_Y,      ABS_MT_ORIENTATION, ABS_MT_PRESSURE,
                                    ABS_MT_TOUCH_MAJOR, ABS_MT_TOUCH_MINOR};
    struct evdev *dev = in->dev;
    int slot = evdev_mt_get_slot_by_key(dev, c->id);
    unsigned int i, axis;
    int32_t width, height;

    if (slot < 0) {
        return;
    }
    evdev_report(dev, EV_ABS, ABS_MT_SLOT, slot);
    if (!evdev_mt_report_slot_state(dev, c->confidence ? MT_TOOL_FINGER : MT_TOOL_PALM,
                                    c->tip || c->in_range)) {
        return;
    }
    if (test_bit(in->abs, ABS_MT_DISTANCE)) {
        evdev_report(dev, EV_ABS, ABS_MT_DISTANCE, !c->tip);
    }
    if (c->axes & (1u << (ABS_MT_TOUCH_MINOR - ABS_MT_FIRST))) {
        width = c->value[ABS_MT_TOUCH_MAJOR - ABS_MT_FIRST];
        height = c->value[ABS_MT_TOUCH_MINOR - ABS_MT_FIRST];
        c->value[ABS_MT_TOUCH_MAJOR - ABS_MT_FIRST] = width > height ? width : height;
        c->value[ABS_MT_TOUCH_MINOR - ABS_MT_FIRST] = width < height ? width : height;
        c->axes |= 1u << (ABS_MT_TOUCH_MAJOR - ABS_MT_FIRST);
        if (!(c->axes & (1u << (ABS_MT_ORIENTATION - ABS_MT_FIRST)))) {
            c->value[ABS_MT_ORIENTATION - ABS_MT_FIRST] = width > height;
            c->axes |= 1u << (ABS_MT_ORIENTATION - ABS_MT_FIRST);
        }
    }
    for (i = 0; i < COUNT(axes); i++) {
        axis = axes[i] - ABS_MT_FIRST;
        if (c->axes & (1u << axis)) {
            evdev_report(dev, EV_ABS, axes[i], c->value[axis]);
        }
    }
}

/** Report the frame buttons, combining the left-button sources of a pad. */
static void mt_report_buttons(struct hid_input *in)
{
    unsigned int i, buttons = 0;

    for (i = 0; i < 32; i++) {
        if (in->mt_frame_buttons & (1u << i)) {
            buttons |= 1u << in->mt_button_target[i];
        }
    }
    for (i = 0; i < 32; i++) {
        if (in->mt_button_codes & (1u << i)) {
            evdev_report(in->dev, EV_KEY, (uint16_t)(BTN_LEFT + i), (buttons >> i) & 1u);
        }
    }
}

/** Report the packet and return whether its frame is complete. A positive
 * Contact Count starts a frame, and zero continues one still under way. */
static int mt_packet(struct hid_dev *hid, struct hid_input *in)
{
    struct hid_mt_contact *c;
    unsigned int i = in->mt_first;

    if (in->mt_count > 0 || !in->mt_left) {
        in->mt_frame_buttons &= ~in->mt_button_fields;
        in->mt_frame_buttons |= in->mt_pressed;
    }
    if (in->mt_count > 0) {
        if (in->mt_left > 0) {
            evdev_mt_end_frame(in->dev);
        }
        in->mt_left = in->mt_count;
    }
    while (i) {
        c = &hid->contacts[i - 1];
        if (c->identified && (in->mt_count < 0 || in->mt_left > 0)) {
            mt_contact(in, c);
            if (in->mt_left > 0) {
                in->mt_left--;
            }
        }
        i = c->next;
        k_memset(c, 0, sizeof(*c));
    }
    if (in->mt_left > 0) {
        return 0;
    }
    if (in->mt_drop_unused) {
        evdev_mt_drop_unused(in->dev);
    } else {
        evdev_mt_end_frame(in->dev);
    }
    if (in->mt_precision && in->mt_buttons) {
        mt_report_buttons(in);
    }
    evdev_mt_report_pointer_emulation(in->dev, (in->mt_flags & INPUT_MT_POINTER) != 0);
    return 1;
}

/**
 * The event of usage `u` of field `f` with value `value`, as Linux's
 * `hidinput_hid_event` gives it for the pages this driver maps.
 */
static void hid_event(struct hid_dev *hid, struct hid_field *f, struct hid_usage *u, int32_t value)
{
    struct hid_input *in;

    if (f->input == HID_NO_INPUT) {
        return;
    }
    in = &hid->inputs[f->input];
    if (u->mt_group) {
        mt_collect(hid, in, f, u, value);
        return;
    }
    if (!u->type || u->type == EV_PWR) {
        return;
    }
    in->touched = 1;
    if (u->hat || u->hat_dir) {
        hat_event(in, f, u, value);
        return;
    }
    if (!in_range(f, &value) || special_event(in, f, u, value)) {
        return;
    }
    /* An absolute value that did not change is not reported: some
     * keyboards report the keys of other layouts as constant 0. */
    if (!(f->flags & (HID_MAIN_ITEM_RELATIVE | HID_MAIN_ITEM_BUFFERED_BYTE)) &&
        (f->flags & HID_MAIN_ITEM_VARIABLE) && value == u->value) {
        return;
    }
    input_event(in, u->type, u->code, value, u->hid);
    if ((f->flags & HID_MAIN_ITEM_RELATIVE) && u->type == EV_KEY && value) {
        evdev_report(in->dev, EV_SYN, SYN_REPORT, 0);
        input_event(in, u->type, u->code, 0, u->hid);
    }
}

/** Whether `value` names a usage of the array field `f`, Linux's
 * `hid_array_value_is_valid`. */
static int array_value_valid(const struct hid_field *f, int32_t value)
{
    return value >= f->logical_minimum && value <= f->logical_maximum &&
           (uint32_t)value - (uint32_t)f->logical_minimum < f->maxusage;
}

/** Read the elements of `f` from `data` into their `new_value`, as Linux's
 * `hid_input_fetch_field`; an array that holds ErrorRollOver, the keyboard
 * page's usage 1, is marked ignored. */
static void fetch_field(struct hid_dev *hid, struct hid_field *f, const uint8_t *data)
{
    struct hid_usage *u = usages_of(hid, f);
    uint32_t n;

    for (n = 0; n < f->count; n++) {
        u[n].new_value = 0;
    }
    f->ignored = 0;
    for (n = 0; n < f->count; n++) {
        uint32_t raw = hid_field_extract(data, f->offset + n * f->size, f->size);
        int32_t v = f->logical_minimum < 0 ? hid_snto32(raw, f->size) : (int32_t)raw;

        u[n].new_value = v;
        if (!(f->flags & HID_MAIN_ITEM_VARIABLE) && array_value_valid(f, v) &&
            u[(uint32_t)v - (uint32_t)f->logical_minimum].hid == HID_UP_KEYBOARD + 1) {
            f->ignored = 1;
            return;
        }
    }
}

/** Whether `value` is among the `count` values of `u`, read from
 * `new_value` when `fresh`. */
static int among(const struct hid_usage *u, uint32_t count, int32_t value, int fresh)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        if ((fresh ? u[i].new_value : u[i].value) == value) {
            return 1;
        }
    }
    return 0;
}

/** The changes of the array field `f`: a usage gone from it is released and
 * one new in it pressed, as Linux's `hid_input_array_field`. */
static void array_field(struct hid_dev *hid, struct hid_field *f)
{
    struct hid_usage *u = usages_of(hid, f);
    int32_t min = f->logical_minimum;
    uint32_t n;

    if (f->ignored) {
        return;
    }
    for (n = 0; n < f->count; n++) {
        if (array_value_valid(f, u[n].value) && !among(u, f->count, u[n].value, 1)) {
            hid_event(hid, f, &u[u[n].value - min], 0);
        }
        if (array_value_valid(f, u[n].new_value) && !among(u, f->count, u[n].new_value, 0)) {
            hid_event(hid, f, &u[u[n].new_value - min], 1);
        }
    }
    for (n = 0; n < f->count; n++) {
        u[n].value = u[n].new_value;
    }
}

/** End complete packets and give the console mouse its relative motion. */
static void sync_inputs(struct hid_dev *hid)
{
    uint32_t i;

    for (i = 0; i < hid->ninputs; i++) {
        struct hid_input *in = &hid->inputs[i];

        if (!in->dev) {
            continue;
        }
        if ((in->mt_first || in->mt_count >= 0 || in->mt_button_fields) && !mt_packet(hid, in)) {
            continue;
        }
        if (in->mt_left > 0) {
            continue;
        }
        evdev_report(in->dev, EV_SYN, SYN_REPORT, 0);
        if (in->mouse && in->touched) {
            if (!evdev_grabbed(in->dev)) {
                mouse_push_packet((uint8_t)(in->buttons & 0x07u), in->dx, -in->dy);
            }
            in->dx = 0;
            in->dy = 0;
        }
    }
}

void hid_input_report(struct hid_dev *hid, const uint8_t *data, uint32_t size)
{
    struct hid_report *r;
    const uint8_t *cdata = data;
    uint32_t csize = size, rsize, i, n;
    int numbered = hid->desc.numbered[HID_INPUT_REPORT];

    if (!size) {
        return;
    }
    r = hid_find_report(&hid->desc, HID_INPUT_REPORT, numbered ? data[0] : 0);
    if (!r) {
        return;
    }
    if (numbered) {
        cdata++;
        csize--;
    }
    rsize = hid_report_len(r) - (r->id > 0 ? 1u : 0u);
    if (csize < rsize) {
        if (!hid->inbuf || rsize > hid->insize) {
            return;
        }
        k_memset(hid->inbuf, 0, rsize);
        k_memcpy(hid->inbuf, cdata, csize);
        cdata = hid->inbuf;
    }

    for (i = 0; i < hid->ninputs; i++) {
        hid->inputs[i].touched = 0;
        hid->inputs[i].mt_first = 0;
        hid->inputs[i].mt_last = 0;
        hid->inputs[i].mt_count = -1;
        hid->inputs[i].mt_pressed = 0;
        hid->inputs[i].mt_button_fields = 0;
    }
    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (&hid->desc.reports[f->report] == r) {
            fetch_field(hid, f, cdata);
        }
    }
    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (&hid->desc.reports[f->report] != r) {
            continue;
        }
        if (f->flags & HID_MAIN_ITEM_VARIABLE) {
            for (n = 0; n < f->count; n++) {
                hid_event(hid, f, &usages_of(hid, f)[n], usages_of(hid, f)[n].new_value);
            }
        } else {
            array_field(hid, f);
        }
    }
    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);

        if (&hid->desc.reports[f->report] == r && (f->flags & HID_MAIN_ITEM_VARIABLE)) {
            for (n = 0; n < f->count; n++) {
                usages_of(hid, f)[n].value = usages_of(hid, f)[n].new_value;
            }
        }
    }

    sync_inputs(hid);
}

/** Write the elements of `f` into `data` from `value`, as Linux's
 * `hid_output_field`; a signed field's values are cut to its size. */
static void output_field(struct hid_dev *hid, const struct hid_field *f, uint8_t *data,
                         int (*value)(const struct hid_field *, const struct hid_usage *, void *),
                         void *arg)
{
    uint32_t n;

    for (n = 0; n < f->count; n++) {
        int32_t v = value(f, &usages_of(hid, f)[n], arg);
        uint32_t bits;

        if (f->logical_minimum < 0 && f->size > 0 && f->size < 32) {
            int32_t a = v >> (f->size - 1);

            if (a && a != -1) {
                bits = v < 0 ? 1u << (f->size - 1) : (1u << (f->size - 1)) - 1;
            } else {
                bits = (uint32_t)v & ((1u << f->size) - 1);
            }
        } else {
            bits = (uint32_t)v;
        }
        hid_field_implement(data, f->offset + n * f->size, f->size, bits);
    }
}

/** Build report `r` of `hid` in `out`, `size` bytes, each field's element
 * given by `value`, as Linux's `hid_output_report`. Returns its bytes, or
 * -1 when it does not fit. */
static int output_report(struct hid_dev *hid, const struct hid_report *r, uint8_t *out, int size,
                         int (*value)(const struct hid_field *, const struct hid_usage *, void *),
                         void *arg)
{
    uint32_t len = hid_report_len(r);
    uint8_t *data = out;
    uint32_t i;

    if (len == 0 || len > (uint32_t)size) {
        return -1;
    }
    k_memset(out, 0, len);
    if (r->id > 0) {
        *data++ = r->id;
    }
    for (i = 0; i < hid->desc.nfields; i++) {
        const struct hid_field *f = field_at(hid, i);

        if (&hid->desc.reports[f->report] == r) {
            output_field(hid, f, data, value, arg);
        }
    }
    return (int)len;
}

/* The console's locks, which a keyboard shows in place of its own. */
#define LOCK_LEDS ((1u << LED_NUML) | (1u << LED_CAPSL) | (1u << LED_SCROLLL))

/* What `led_value` reads: the device, and the console's locks as LED_*
 * bits. */
struct led_source {
    struct hid_dev *hid;
    unsigned int locks;
};

/** An LED usage's value from the LEDs of the input device that owns its
 * field, as Linux sets each from its own input's `led` bits: those programs
 * lit on its event device, and on a keyboard the console's three locks in
 * place of its own. 0 for any other usage. */
static int led_value(const struct hid_field *f, const struct hid_usage *u, void *arg)
{
    const struct led_source *src = (const struct led_source *)arg;
    const struct hid_input *in;
    unsigned int lit;

    if (u->type != EV_LED || f->input == HID_NO_INPUT) {
        return 0;
    }
    in = &src->hid->inputs[f->input];
    lit = evdev_leds_lit(in->dev);
    if (in->ev & (1u << EV_REP)) {
        lit = (lit & ~LOCK_LEDS) | (src->locks & LOCK_LEDS);
    }
    return (int)((lit >> u->code) & 1u);
}

int hidinput_led_report(struct hid_dev *hid, unsigned int locks, uint8_t *out, int size, int *value)
{
    struct led_source src;
    uint32_t i, j;

    src.hid = hid;
    src.locks = locks;
    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);
        struct hid_report *r = &hid->desc.reports[f->report];

        if (r->type != HID_OUTPUT_REPORT) {
            continue;
        }
        for (j = 0; j < f->maxusage; j++) {
            if (usages_of(hid, f)[j].type == EV_LED) {
                *value = (HID_OUTPUT_REPORT + 1) << 8 | r->id;
                return output_report(hid, r, out, size, led_value, &src);
            }
        }
    }
    return -1;
}

/** A Resolution Multiplier's value, its field's logical maximum at `arg`'s
 * field; 0 for any other usage. */
static int multiplier_value(const struct hid_field *f, const struct hid_usage *u, void *arg)
{
    const int32_t *max = (const int32_t *)arg;

    (void)f;
    return u->hid == HID_GD_RESOLUTION ? *max : 0;
}

/** Whether field `f` of `hid` has a Resolution Multiplier usage. */
static int has_multiplier(struct hid_dev *hid, const struct hid_field *f)
{
    uint32_t j;

    for (j = 0; j < f->maxusage; j++) {
        if (usages_of(hid, f)[j].hid == HID_GD_RESOLUTION) {
            return 1;
        }
    }
    return 0;
}

/** Whether every field of report `r` of `hid` is a multiplier of one
 * element whose logical maximum is `max`. */
static int only_multipliers(struct hid_dev *hid, const struct hid_report *r, int32_t max)
{
    uint32_t k;

    for (k = 0; k < hid->desc.nfields; k++) {
        struct hid_field *g = field_at(hid, k);

        if (&hid->desc.reports[g->report] == r &&
            (g->count != 1 || usages_of(hid, g)[0].hid != HID_GD_RESOLUTION ||
             g->logical_maximum != max)) {
            return 0;
        }
    }
    return 1;
}

/** The Feature report of `hid` with a Resolution Multiplier, and in `*only`
 * whether every field of it is a multiplier of one element; NULL for none. */
static struct hid_report *multiplier_report(struct hid_dev *hid, int *only, int32_t *max)
{
    uint32_t i;

    for (i = 0; i < hid->desc.nfields; i++) {
        struct hid_field *f = field_at(hid, i);
        struct hid_report *r = &hid->desc.reports[f->report];

        if (r->type == HID_FEATURE_REPORT && f->count == 1 && has_multiplier(hid, f)) {
            *max = f->logical_maximum;
            *only = only_multipliers(hid, r, *max);
            return r;
        }
    }
    return 0;
}

int hidinput_multiplier_report(struct hid_dev *hid, uint8_t *out, int size, int *value)
{
    int only;
    int32_t max;
    struct hid_report *r = multiplier_report(hid, &only, &max);

    if (!r || !only) {
        return -1;
    }
    *value = (HID_FEATURE_REPORT + 1) << 8 | r->id;
    return output_report(hid, r, out, size, multiplier_value, &max);
}

void hidinput_multiplier_set(struct hid_dev *hid, int ok)
{
    if (ok) {
        setup_multipliers(hid, 1);
    }
}
