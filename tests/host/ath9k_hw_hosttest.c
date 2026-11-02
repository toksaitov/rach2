/**
 * Test the ath9k driver's bring-up, sys/network/driver/ath9k/pci.c, hw.c
 * and eeprom.c, against the chip model of ath9k_model.h.
 *
 * host-deps: tests/host/ath9k_model.c rach2/sys/network/driver/ath9k/hw.c
 * host-deps: rach2/sys/network/driver/ath9k/eeprom.c rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/io.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The recorded chip is the Lenovo G50-70's AR9565 as Linux 6.18's ath9k
 * found it under mmiotrace (dev-log/70-wifi-chip/linux/): its revision
 * register, the values its reset leaves, and its OTP, which holds the
 * image. The probe runs through pci.c with the kernel's PCI calls answered
 * as that machine answers them, and the driver's accesses up to the end
 * of the OTP's image are held to the trace's, entry by entry, with the
 * polls of the status registers left out of both. The probe must then
 * print the boot line with the card's MAC address and subsystem IDs.
 *
 * The other rows: revision 1.1's tables, the refusals of a card that
 * reads all ones, of another family and of an unknown revision, an RTC
 * that never comes on, an image in the EEPROM, a block with a bad
 * checksum and an empty store, the boot options, a failing part, and the
 * radio operations' order of resets and calls. The parts other tasks
 * write (phy, calib, ani, mac, recv, xmit, key, intr, recover, antenna,
 * power, ampdu) are stubs that record each call.
 *
 * The test exits with the number of failed checks, at most 255.
 */

#include "ath9k_model.h"
#include "klog.h"
#include "timer.h"
#include "udelay.h"
#include "mm/paging.h"
#include "lib/kerrno.h"
#include "lib/krandom.h"
#include "lib/kstring.h"
#include "network/wireless/rc_minstrel.h"
#include "network/wireless/wiphy.h"
#include "network/driver/ath9k/ar9003_templates.h"

/* The kernel headers define the integer types themselves, so the test
 * takes no host header but the compiler's own stdarg.h. */
#include <stdarg.h>

extern int printf(const char *format, ...);
extern int vsnprintf(char *str, __SIZE_TYPE__ size, const char *format, va_list ap);

static unsigned failures;

#define CHECK(x)                                                                \
    do {                                                                        \
        if (!(x)) {                                                             \
            printf("ath9k_hw_hosttest: line %u: %s\n", (unsigned)__LINE__, #x); \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/** Whether two strings are equal; written out, with no host header. */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether `s` begins with `prefix`. */
static int starts(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* The kernel the driver links against.                                */
/* ------------------------------------------------------------------ */

/* The kernel log's lines, each "subsystem: text", since the last reset. */
#define LINES 32
static char lines[LINES][192];
static unsigned n_lines;

/** Keep a kernel log record as the console would print it. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;
    char *p;
    unsigned n = 0;

    (void)level;
    if (n_lines == LINES)
        return;
    p = lines[n_lines++];
    while (*subsystem && n < 100)
        p[n++] = *subsystem++;
    p[n++] = ':';
    p[n++] = ' ';
    va_start(ap, format);
    vsnprintf(p + n, sizeof(lines[0]) - n, format, ap);
    va_end(ap);
}

/** Whether the log holds `line`. */
static int logged(const char *line)
{
    unsigned i;

    for (i = 0; i < n_lines; i++)
        if (streq(lines[i], line))
            return 1;
    return 0;
}

/** Whether a line of the log begins with `prefix`. */
static int logged_prefix(const char *prefix)
{
    unsigned i;

    for (i = 0; i < n_lines; i++)
        if (starts(lines[i], prefix))
            return 1;
    return 0;
}

/** Print the log, for a failing row. */
static void print_log(void)
{
    unsigned i;

    for (i = 0; i < n_lines; i++)
        printf("  log: %s\n", lines[i]);
}

/** Count nothing: the model's own wait is io.h's. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** Fill `buf` with a fixed pattern, for a card without a MAC address. */
void krandom_bytes(void *buf, uint32_t n)
{
    unsigned char *p = buf;

    while (n--)
        *p++ = 0x5a;
}

/* The PCI side of the Lenovo's card, at 02:00.0: BAR 0 is a 64-bit
 * memory window at 0xc0400000, the subsystem 17aa:4026. */
static int card_present;
static uint8_t cache_line;  /* what the cache line size reads */
static uint8_t latency;     /* what the probe wrote to the latency timer */
static unsigned memory_on;  /* calls of pci_enable_memory_space */
static unsigned master_on;  /* calls of pci_enable_bus_master */
static uintptr_t mapped_at; /* what map_mmio was asked for */
static size_t mapped_len;

/** Find the card when one is present. */
bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out)
{
    unsigned i;

    if (!card_present || vendor_id != 0x168c || device_id != 0x0036)
        return 0;
    out->vendor_id = vendor_id;
    out->device_id = device_id;
    out->class_code = 0x02;
    out->subclass = 0x80;
    out->prog_if = 0;
    out->revision = 1;
    out->bus = 2;
    out->device = 0;
    out->function = 0;
    out->irq = 19;
    for (i = 0; i < 6; i++)
        out->bar[i] = 0;
    out->bar[0] = 0xc0400004u; /* memory, 64-bit, not prefetchable */
    return 1;
}

/** Whether a BAR is memory. */
bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 1);
}

/** BAR `i` as a 64-bit memory BAR, the next one its high half. */
uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    return (uint64_t)(dev->bar[i] & ~0xfu) | (uint64_t)dev->bar[i + 1] << 32;
}

/** The card's configuration bytes the probe reads. */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    return offset == PCI_CACHE_LINE_SIZE ? cache_line : 0;
}

/** The subsystem IDs. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == 0x2c)
        return 0x17aa;
    if (offset == 0x2e)
        return 0x4026;
    return 0;
}

/** Keep the cache line size and the latency timer. */
void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value)
{
    (void)bus;
    (void)device;
    (void)function;
    if (offset == PCI_CACHE_LINE_SIZE)
        cache_line = value;
    if (offset == PCI_LATENCY_TIMER)
        latency = value;
}

/** Count the command register's bits the probe turns on. */
void pci_enable_memory_space(pci_device_t *dev)
{
    (void)dev;
    memory_on++;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    (void)dev;
    master_on++;
}

/** Map nothing: the model answers the driver's accesses. */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    mapped_at = phys_addr;
    mapped_len = size;
    return 0x10000;
}

/* The station layer's side. */
static struct wlan_radio *registered;
static struct wlan the_wlan;
static int register_rc;
static unsigned minstrel_calls;
static struct wlan_work *queued; /* the last work queued */
static uint64_t queued_due;
static unsigned n_queued;

/** Keep the radio registered, as phy0 of the one station. */
int wlan_register_radio(struct wlan_radio *radio, struct wlan **out)
{
    *out = NULL;
    if (register_rc != 0)
        return register_rc;
    registered = radio;
    the_wlan.radio = radio;
    radio->wlan = &the_wlan;
    *out = &the_wlan;
    return 0;
}

/** Keep the last work queued; refuse one already queued, as work.c does. */
int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    if (work->pending)
        return -EBUSY;
    work->pending = 1;
    work->due = due;
    queued = work;
    queued_due = due;
    n_queued++;
    return 0;
}

/** Run the last work queued, as the worker does when it is due. */
static void run_queued(void)
{
    struct wlan_work *w = queued;

    queued = NULL;
    w->pending = 0;
    w->run(w);
}

/** The clock, in milliseconds. */
uint64_t tick_count(void)
{
    return 5000;
}

void wlan_minstrel_init(struct wlan_radio *radio, struct wlan_minstrel *state)
{
    (void)radio;
    (void)state;
    minstrel_calls++;
}

/* ------------------------------------------------------------------ */
/* The other parts of the driver, as stubs that record each call.      */
/* ------------------------------------------------------------------ */

static struct ath9k_model model;

/* The parts' calls in order, each with the model's log length at it. */
#define CALLS 64
static const char *calls[CALLS];
static unsigned call_at[CALLS];
static unsigned n_calls;

static int cal_rc;                           /* what ath9k_cal_init returns */
static int intr_rc;                          /* what ath9k_intr_attach returns */
static unsigned tuned;                       /* the frequency of the last ath9k_phy_set_channel */
static unsigned filter_set;                  /* the filters of the last ath9k_mac_set_filter */
static uint8_t bssid_set[6];                 /* the BSSID of the last ath9k_mac_set_bssid */
static const struct wlan_bss *ampdu_sta_bss; /* the AP of the last ath9k_ampdu_sta */
static int slot_cleared = -1;                /* the slot of the last ath9k_key_clear */

/** Record a part's call. */
static void note(const char *name)
{
    if (n_calls < CALLS) {
        call_at[n_calls] = model.log_count;
        calls[n_calls++] = name;
    }
}

/** Forget the calls and the log lines. */
static void notes_clear(void)
{
    n_calls = 0;
    n_lines = 0;
}

/** Whether the calls since notes_clear are `want`, ending at NULL. */
static int calls_are(const char *const *want)
{
    unsigned i;

    for (i = 0; want[i]; i++)
        if (i >= n_calls || !streq(calls[i], want[i])) {
            printf("  call %u: %s, wanted %s\n", i, i < n_calls ? calls[i] : "none", want[i]);
            return 0;
        }
    if (i != n_calls) {
        printf("  call %u: %s, wanted none\n", i, calls[i]);
        return 0;
    }
    return 1;
}

/** The log index at the first call of `name`, or ~0u. */
static unsigned call_index(const char *name)
{
    unsigned i;

    for (i = 0; i < n_calls; i++)
        if (streq(calls[i], name))
            return call_at[i];
    return ~0u;
}

void ath9k_phy_process_ini(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)ah;
    (void)chan;
    note("phy_process_ini");
}

/** Mark the baseband inactive, as phy.c does. */
void ath9k_phy_mark_inactive(struct ath9k *ah)
{
    note("phy_mark_inactive");
    REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_DIS);
}

void ath9k_phy_set_rfmode(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)ah;
    (void)chan;
    note("phy_set_rfmode");
}

void ath9k_phy_set_delta_slope(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)ah;
    (void)chan;
    note("phy_set_delta_slope");
}

void ath9k_phy_spur_mitigate(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)ah;
    (void)chan;
    note("phy_spur_mitigate");
}

/** Activate the baseband, as phy.c does, without its wait. */
void ath9k_phy_init_bb(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)chan;
    note("phy_init_bb");
    REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);
}

int ath9k_nf_read(struct ath9k *ah)
{
    (void)ah;
    note("nf_read");
    return 0;
}

void ath9k_cal_set_channel(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)ah;
    (void)chan;
    note("cal_set_channel");
}

int ath9k_nf_load(struct ath9k *ah)
{
    (void)ah;
    note("nf_load");
    return 0;
}

uint64_t ath9k_mac_get_tsf(struct ath9k *ah)
{
    (void)ah;
    return 0x123456789ull;
}

void ath9k_intr_detach(struct ath9k *ah)
{
    (void)ah;
    note("intr_detach");
}

void ath9k_tick(struct ath9k *ah)
{
    (void)ah;
    note("tick");
}

void ath9k_rfkill_poll(struct ath9k *ah)
{
    (void)ah;
    note("rfkill_poll");
}

int ath9k_phy_set_channel(struct ath9k *ah, const struct wlan_channel *chan)
{
    note("phy_set_channel");
    ah->phy.channel = *chan;
    tuned = chan->frequency;
    return 0;
}

int ath9k_cal_init(struct ath9k *ah, const struct wlan_channel *chan)
{
    (void)ah;
    (void)chan;
    note("cal_init");
    return cal_rc;
}

void ath9k_nf_start(struct ath9k *ah)
{
    (void)ah;
    note("nf_start");
}

void ath9k_cal_start(struct ath9k *ah)
{
    (void)ah;
    note("cal_start");
}

void ath9k_cal_stop(struct ath9k *ah)
{
    (void)ah;
    note("cal_stop");
}

void ath9k_ani_reset(struct ath9k *ah)
{
    (void)ah;
    note("ani_reset");
}

void ath9k_mac_init_queues(struct ath9k *ah)
{
    (void)ah;
    note("mac_init_queues");
}

int ath9k_mac_abort_tx(struct ath9k *ah)
{
    (void)ah;
    note("mac_abort_tx");
    return 0;
}

void ath9k_mac_set_filter(struct ath9k *ah, unsigned filters)
{
    (void)ah;
    note("mac_set_filter");
    filter_set = filters;
}

void ath9k_mac_set_address(struct ath9k *ah)
{
    (void)ah;
    note("mac_set_address");
}

void ath9k_mac_set_bssid(struct ath9k *ah, const uint8_t bssid[6], unsigned aid)
{
    unsigned i;

    (void)ah;
    (void)aid;
    note("mac_set_bssid");
    for (i = 0; i < 6; i++)
        bssid_set[i] = bssid[i];
}

void ath9k_mac_intr_enable(struct ath9k *ah)
{
    (void)ah;
    note("mac_intr_enable");
}

void ath9k_mac_intr_disable(struct ath9k *ah)
{
    (void)ah;
    note("mac_intr_disable");
}

/* The receive and transmit pages the probe holds, and tx_init's answer. */
static int rx_held, tx_held, tx_rc;

int ath9k_rx_init(struct ath9k *ah)
{
    (void)ah;
    note("rx_init");
    rx_held = 1;
    return 0;
}

void ath9k_rx_free(struct ath9k *ah)
{
    (void)ah;
    note("rx_free");
    rx_held = 0;
}

int ath9k_rx_start(struct ath9k *ah)
{
    (void)ah;
    note("rx_start");
    return 0;
}

int ath9k_rx_stop(struct ath9k *ah)
{
    (void)ah;
    note("rx_stop");
    return 0;
}

int ath9k_tx_init(struct ath9k *ah)
{
    (void)ah;
    note("tx_init");
    if (tx_rc == 0)
        tx_held = 1;
    return tx_rc;
}

void ath9k_tx_free(struct ath9k *ah)
{
    (void)ah;
    note("tx_free");
    tx_held = 0;
}

int ath9k_tx(struct ath9k *ah, const uint8_t *frame, unsigned len, const struct wlan_tx_info *info)
{
    (void)ah;
    (void)frame;
    (void)len;
    (void)info;
    note("tx");
    return 0;
}

void ath9k_tx_drain(struct ath9k *ah)
{
    (void)ah;
    note("tx_drain");
}

int ath9k_key_set(struct ath9k *ah, const uint8_t peer[6], const struct wlan_key *key, int *slot)
{
    (void)ah;
    (void)peer;
    (void)key;
    note("key_set");
    *slot = 4;
    return 0;
}

void ath9k_key_clear(struct ath9k *ah, int slot)
{
    (void)ah;
    note("key_clear");
    slot_cleared = slot;
}

void ath9k_key_restore(struct ath9k *ah)
{
    (void)ah;
    note("key_restore");
}

int ath9k_intr_attach(struct ath9k *ah)
{
    (void)ah;
    note("intr_attach");
    return intr_rc;
}

void ath9k_recover_watchdog(struct ath9k *ah)
{
    (void)ah;
}

void ath9k_recover_start(struct ath9k *ah)
{
    (void)ah;
    note("recover_start");
}

int ath9k_reset(struct ath9k *ah, enum ath9k_reset_reason why)
{
    (void)why;
    note("reset");
    ah->recover.failures = 0;
    return 0;
}

void ath9k_recover_stop(struct ath9k *ah)
{
    (void)ah;
    note("recover_stop");
}

void ath9k_ant_init(struct ath9k *ah)
{
    (void)ah;
    note("ant_init");
}

void ath9k_ant_set(struct ath9k *ah)
{
    (void)ah;
    note("ant_set");
}

void ath9k_power_init(struct ath9k *ah)
{
    (void)ah;
    note("power_init");
}

int ath9k_ampdu_action(struct ath9k *ah, enum wlan_ampdu_action action, const uint8_t peer[6],
                       unsigned tid, unsigned ssn, unsigned window)
{
    (void)ah;
    (void)action;
    (void)peer;
    (void)tid;
    (void)ssn;
    (void)window;
    note("ampdu_action");
    return 0;
}

void ath9k_ampdu_sta(struct ath9k *ah, const struct wlan_bss *bss)
{
    (void)ah;
    ampdu_sta_bss = bss;
    note("ampdu_sta");
}

/* The driver's bus side, whole, so that the test reaches its card and
 * points its register access at the model. */
#include "network/driver/ath9k/pci.c"

#include "ath9k_lenovo.h"

/* ------------------------------------------------------------------ */
/* The recorded chip.                                                  */
/* ------------------------------------------------------------------ */

/* One access of the trace: op 'R' or 'W', and whether its value counts. */
struct traced {
    char op;
    uint32_t reg;
    uint32_t value;
    int any; /* the value is the window's, which the trace does not keep */
};

/* The trace's first accesses, through the read of the baseband's chip ID:
 * AR_SREV, AR_WA, the power-on reset, the warm reset, AR_WA_BIT22, the
 * wake and the read of AR_PHY_CHIP_ID. */
static const struct traced lenovo_attach[] = {
    {'R', 0x4020, 0x002cc1ff, 0}, {'R', 0x4004, 0x0102021b, 0}, {'W', 0x4004, 0x0102421b, 0},
    {'W', 0x704c, 0x00000003, 0}, {'W', 0x4004, 0x0102421b, 0}, {'W', 0x704c, 0x00000003, 0},
    {'W', 0x7040, 0x00000000, 0}, {'W', 0x7040, 0x00000001, 0}, {'W', 0x4004, 0x0102421b, 0},
    {'W', 0x704c, 0x00000003, 0}, {'R', 0x4028, 0x00000000, 0}, {'W', 0x7000, 0x00000001, 0},
    {'W', 0x7000, 0x00000000, 0}, {'R', 0x7000, 0x00000000, 0}, {'W', 0x4000, 0x00000000, 0},
    {'W', 0x4004, 0x0142421b, 0}, {'W', 0x4004, 0x0142421b, 0}, {'R', 0x704c, 0x00000003, 0},
    {'W', 0x704c, 0x00000003, 0}, {'R', 0x8004, 0x20000000, 0}, {'W', 0x8004, 0x20000000, 0},
    {'R', 0x9818, 0xc7559163, 0}};

/* The whole of the trace's attach, the rest by its rules. */
#define TRACE_MAX 2048
static struct traced trace[TRACE_MAX];
static unsigned trace_len;

/** Append an access to the expected trace. */
static void expect(char op, uint32_t reg, uint32_t value, int any)
{
    if (trace_len < TRACE_MAX) {
        trace[trace_len].op = op;
        trace[trace_len].reg = reg;
        trace[trace_len].value = value;
        trace[trace_len].any = any;
        trace_len++;
    }
}

/**
 * Build the trace of Linux's attach of the recorded card up to its last
 * OTP read, 1,628 accesses: the prefix above; the register test, AR_STA_ID0
 * read, then 260 patterns written and read back, then restored; the
 * EEPROM's two words at each of its three header addresses, which read
 * nothing; and the OTP, read one word per byte with each word's data read
 * after it: the header at 0x3ff three times over (the search, the loop's
 * header and the block's start), the block down to byte 0x1f1, and the
 * empty header at 0x1f0 that ends the loop.
 */
static void build_trace(void)
{
    static const uint32_t pattern[4] = {0x55555555, 0xaaaaaaaa, 0x66666666, 0x99999999};
    static const uint32_t eeprom_reads[6] = {0x27fc, 0x27f8, 0x3ffc, 0x3ff8, 0x23fc, 0x23f8};
    unsigned i, w;

    trace_len = 0;
    for (i = 0; i < sizeof(lenovo_attach) / sizeof(lenovo_attach[0]); i++)
        expect(lenovo_attach[i].op, lenovo_attach[i].reg, lenovo_attach[i].value, 0);
    expect('R', AR_STA_ID0, 0, 0);
    for (i = 0; i < 0x100 + 4; i++) {
        uint32_t v = i < 0x100 ? (uint32_t)i << 16 | i : pattern[i - 0x100];

        expect('W', AR_STA_ID0, v, 0);
        expect('R', AR_STA_ID0, v, 0);
    }
    expect('W', AR_STA_ID0, 0, 0);
    for (i = 0; i < 6; i++)
        expect('R', eeprom_reads[i], 0, 1);
    for (i = 0; i < 12 + 131 * 4 + 3; i++) {
        w = i < 12 ? 0xff : i < 12 + 131 * 4 ? 0xfe - (i - 12) / 4 : 0x7b;
        expect('R', AR9300_OTP_BASE + 4 * w, otp_word_of(w), 0);
        expect('R', AR9300_OTP_READ_DATA, otp_word_of(w), 0);
    }
}

/** Set the bus up for a probe of the model's card and clear what the
 * last probe left. */
static void bus_ready(void)
{
    card_present = 1;
    cache_line = 0;
    latency = 0;
    memory_on = 0;
    master_on = 0;
    mapped_at = 0;
    mapped_len = 0;
    registered = NULL;
    register_rc = 0;
    minstrel_calls = 0;
    cal_rc = 0;
    intr_rc = 0;
    tx_rc = 0;
    rx_held = tx_held = 0;
    queued = NULL;
    n_queued = 0;
    k_memset(&the_wlan, 0, sizeof(the_wlan));
    card_io = &ath9k_model_io;
    card_ctx = &model;
    notes_clear();
}

/** Whether the register is one of the status registers the driver polls,
 * whose reads the trace comparison leaves out. */
static int polled(uint32_t reg)
{
    return reg == AR_RTC_STATUS || reg == AR_EEPROM_STATUS_DATA || reg == AR9300_OTP_STATUS;
}

/* ------------------------------------------------------------------ */
/* The rows.                                                           */
/* ------------------------------------------------------------------ */

/** The recorded card: the trace, the image and the boot line. */
static void test_recorded(void)
{
    static const char *const order[] = {"power_init",  "ant_init",    "rx_init", "tx_init",
                                        "intr_attach", "rfkill_poll", NULL};
    const struct ath9k_model_access *a;
    unsigned i, k = 0, bad = 0;
    int rc;

    lenovo_model(1);
    model.log_reads = 1;
    bus_ready();
    ath9k_poll(); /* no card yet: nothing */
    CHECK(n_calls == 0);
    build_trace();
    CHECK(trace_len == 1628);
    rc = ath9k_probe();
    CHECK(rc == 0);
    CHECK(model.log_lost == 0);
    for (i = 0; i < model.log_count && k < trace_len; i++) {
        a = &model.log[i];
        if (a->op == ATH9K_MODEL_DELAY || (a->op == ATH9K_MODEL_READ && polled(a->reg)))
            continue;
        if ((a->op == ATH9K_MODEL_READ ? 'R' : 'W') != trace[k].op || a->reg != trace[k].reg ||
            (!trace[k].any && a->value != trace[k].value)) {
            if (bad++ < 4)
                printf("  access %u: %c %05x %08x, the trace's %c %05x %08x\n", k,
                       a->op == ATH9K_MODEL_READ ? 'R' : 'W', (unsigned)a->reg, (unsigned)a->value,
                       trace[k].op, (unsigned)trace[k].reg, (unsigned)trace[k].value);
        }
        k++;
    }
    CHECK(k == trace_len);
    CHECK(bad == 0);

    CHECK(logged("ath9k: probe: 02:00.0 AR9565 rev 1.0.1 mac b8:ee:65:32:89:15 subsys 17aa:4026"));
    CHECK(n_lines == 1);
    CHECK(registered == &card.radio);
    CHECK(n_queued == 1 && queued == &card.bus.rfkill_work[0] && queued_due == 5000 + 1000);
    CHECK(card.radio.ops == &ath9k_radio_ops && card.radio.priv == &card);
    CHECK(streq(card.radio.name, "ath9k"));
    CHECK(minstrel_calls == 1);
    CHECK(calls_are(order));
    notes_clear();
    ath9k_poll();
    {
        static const char *const tick[] = {"tick", NULL};
        CHECK(calls_are(tick));
    }
    CHECK(card.bus.flags == (ATH9K_PCI_2ANT | ATH9K_PCI_BT_ANT_DIV));
    CHECK(card.bus.subsys_vendor == 0x17aa && card.bus.subsys_device == 0x4026);
    CHECK(mapped_at == 0xc0400000u && mapped_len == 0x80000);
    CHECK(memory_on == 1 && master_on == 1);
    CHECK(cache_line == 16 && latency == 0xa8);
    CHECK(model.unmapped == 0);

    /* The image: template 4 under the card's runs. */
    CHECK(card.eeprom.valid);
    CHECK(card.eeprom.image.templateVersion == 4);
    CHECK(card.eeprom.regdomain == 0x6a);
    CHECK(card.radio.country[0] == '0' && card.radio.country[1] == '0');
    CHECK(card.hw.rev && card.hw.rev->mac_rev == 1);

    /* Linux gave channels 1 to 13 18 dBm, the highest target power, which
     * no conformance limit narrows. */
    CHECK(card.radio.band.n_channels == 14);
    for (i = 0; i < 13; i++)
        CHECK(card.radio.band.channels[i].max_power == 18);
    CHECK(card.radio.band.channels[0].frequency == 2412 &&
          card.radio.band.channels[13].frequency == 2484);
    CHECK(card.radio.hw_ciphers == (WLAN_CIPHER_HW_TKIP | WLAN_CIPHER_HW_CCMP));

    /* The probe leaves the chip in full sleep: the RTC shut down. */
    CHECK(card.hw.power == ATH9K_PM_FULL_SLEEP);
    CHECK(!(ath9k_model_peek(&model, AR_RTC_RESET) & AR_RTC_RESET_EN));
    CHECK(!(ath9k_model_peek(&model, AR_WA) & AR_WA_D3_L1_DISABLE));
    if (failures)
        print_log();
}

/** The radio's operations on the recorded card, after test_recorded. */
static void test_radio(void)
{
    /* A reset's calls, in ath9k_hw_reset's order (from full sleep the
     * noise floor is not read). */
#define RESET_CALLS                                                                                \
    "cal_set_channel", "phy_mark_inactive", "phy_process_ini", "phy_set_rfmode",                   \
        "phy_set_delta_slope", "phy_spur_mitigate", "ant_set", "mac_set_address", "mac_set_bssid", \
        "phy_set_channel", "mac_init_queues", "ani_reset", "phy_init_bb", "cal_init", "nf_load",   \
        "nf_start"
    static const char *const start[] = {
        RESET_CALLS,       "key_restore", "mac_set_filter", "rx_start",
        "mac_intr_enable", "cal_start",   "recover_start",  NULL};
    static const char *const retune[] = {
        "mac_intr_disable", "mac_abort_tx",   "rx_stop",  "tx_drain",        "nf_read", RESET_CALLS,
        "key_restore",      "mac_set_filter", "rx_start", "mac_intr_enable", NULL};
#undef RESET_CALLS
    static const char *const stop[] = {
        "mac_intr_disable", "recover_stop", "cal_stop", "mac_intr_disable",
        "mac_abort_tx",     "rx_stop",      "tx_drain", NULL};
    static const char *const filter[] = {"mac_set_filter", NULL};
    static const char *const none[] = {NULL};
    static const uint8_t frame[24] = {0x08};
    struct wlan_radio *radio = &card.radio;
    const struct wlan_radio_ops *ops = radio->ops;
    struct wlan_bss bss;
    struct wlan_key key;
    struct wlan_tx_info info;
    uint64_t tsf;
    int dis, cold, en, rc, slot;

    if (registered != radio)
        return;
    model.log_reads = 0;
    ath9k_model_log_clear(&model);
    notes_clear();
    k_memset(&info, 0, sizeof(info));

    /* The switch is read each second, on the two records in turn. */
    run_queued();
    {
        static const char *const poll[] = {"rfkill_poll", NULL};
        CHECK(calls_are(poll));
    }
    CHECK(n_queued == 2 && queued == &card.bus.rfkill_work[1]);
    notes_clear();

    /* A suspended card does not start. */
    card.power.suspended = 1;
    CHECK(ops->start(radio) == -EBUSY);
    CHECK(calls_are(none) && !card.bus.started);
    card.power.suspended = 0;

    /* A stopped radio keeps the channel and the filters for its start. */
    CHECK(ops->config(radio, &radio->band.channels[5], 3) == 0);
    CHECK(calls_are(none));
    CHECK(ops->tx(radio, frame, sizeof(frame), &info) == -ENETDOWN);
    CHECK(ops->get_tsf(radio, &tsf) == -ENETDOWN);

    /* The start: the wake from full sleep is a power-on reset, and the
     * channel's reset after the baseband is turned off is cold; the
     * baseband is on again before the calibration and the LED register is
     * written last, then come the station's state and the periodic work. */
    CHECK(ops->start(radio) == 0);
    CHECK(card.bus.started);
    CHECK(calls_are(start));
    CHECK(tuned == 2437 && filter_set == 3);
    dis = ath9k_model_find(&model, 0, ATH9K_MODEL_WRITE, AR_PHY_ACTIVE);
    CHECK(dis >= 0 && model.log[dis].value == AR_PHY_ACTIVE_DIS);
    cold = ath9k_model_find(&model, (unsigned)dis, ATH9K_MODEL_WRITE, AR_RTC_RC);
    CHECK(cold > dis && model.log[cold].value == (AR_RTC_RC_MAC_WARM | AR_RTC_RC_MAC_COLD));
    en = ath9k_model_find(&model, (unsigned)dis + 1, ATH9K_MODEL_WRITE, AR_PHY_ACTIVE);
    CHECK(en > cold && model.log[en].value == AR_PHY_ACTIVE_EN);
    CHECK((unsigned)en < call_index("cal_init"));
    CHECK(model.log[call_index("key_restore") - 1].op == ATH9K_MODEL_WRITE &&
          model.log[call_index("key_restore") - 1].reg == AR_CFG_LED);
    CHECK(ath9k_model_last_write(&model, AR_CFG_LED, 0) & AR_CFG_SCLK_32KHZ);

    /* The values Linux wrote on the card. */
    CHECK(ath9k_model_last_write(&model, AR_RTC_PLL_CONTROL, 0) == 0x0004142c);
    CHECK(ath9k_model_last_write(&model, AR_RTC_SLEEP_CLK, 0) == 0x00000002);
    CHECK(ath9k_model_last_write(&model, AR_D_GBL_IFS_SIFS, 0) == 0x00000160);
    CHECK(ath9k_model_last_write(&model, AR_D_GBL_IFS_SLOT, 0) == 0x0000018c);
    CHECK(ath9k_model_last_write(&model, AR_TIME_OUT, 0) == 0x08400b00);
    CHECK((ath9k_model_last_write(&model, AR_USEC, 0) & AR_USEC_USEC) == 0x2b);
    CHECK(ath9k_model_last_write(&model, AR_OBS, 0) == 8);
    CHECK(ath9k_model_last_write(&model, AR_RSSI_THR, 0) == 0x700);
    CHECK(card.hw.power == ATH9K_PM_AWAKE);

    /* A second start does nothing. */
    notes_clear();
    CHECK(ops->start(radio) == 0);
    CHECK(calls_are(none));

    /* The same channel is the filter alone; another a reset onto it with
     * DMA stopped around it, warm since the chip is awake and idle. */
    CHECK(ops->config(radio, &radio->band.channels[5], 7) == 0);
    CHECK(calls_are(filter));
    CHECK(filter_set == 7);
    notes_clear();
    ath9k_model_log_clear(&model);
    CHECK(ops->config(radio, &radio->band.channels[0], 7) == 0);
    CHECK(calls_are(retune));
    CHECK(tuned == 2412 && card.phy.channel.frequency == 2412);
    cold = ath9k_model_find(&model, 0, ATH9K_MODEL_WRITE, AR_RTC_RC);
    CHECK(cold >= 0 && model.log[cold].value == AR_RTC_RC_MAC_WARM);

    /* A retune whose calibration fails, after the synthesizer took the
     * channel, leaves the chip off: the same channel again is a full
     * reset, not the filter alone, until one succeeds. */
    cal_rc = -EIO;
    CHECK(ops->config(radio, &radio->band.channels[10], 7) == -EIO);
    CHECK(logged("ath9k: config: failed: 2462 MHz: -5"));
    CHECK(card.phy.channel.frequency == 2462 && card.recover.failures == 1 &&
          card.recover.reason == ATH9K_RESET_CHANNEL);
    notes_clear();
    CHECK(ops->config(radio, &radio->band.channels[10], 7) == -EIO);
    CHECK(call_index("cal_init") != ~0u && call_index("mac_intr_enable") == ~0u);
    CHECK(card.recover.failures == 2);
    cal_rc = 0;
    notes_clear();
    CHECK(ops->config(radio, &radio->band.channels[10], 7) == 0);
    CHECK(calls_are(retune));
    CHECK(card.recover.failures == 0);
    /* A start of the started radio after such a failure resets again. */
    cal_rc = -EIO;
    CHECK(ops->config(radio, &radio->band.channels[0], 7) == -EIO);
    cal_rc = 0;
    notes_clear();
    CHECK(ops->start(radio) == 0);
    {
        static const char *const reset[] = {"reset", NULL};
        CHECK(calls_are(reset));
    }
    CHECK(card.recover.failures == 0 && card.phy.channel.frequency == 2412);

    /* Transmit, keys, the BSSID, aggregation and the TSF reach their parts. */
    notes_clear();
    CHECK(ops->tx(radio, frame, sizeof(frame), &info) == 0);
    k_memset(&key, 0, sizeof(key));
    slot = -1;
    CHECK(ops->set_key(radio, 1, frame, &key, &slot) == 0 && slot == 4);
    CHECK(ops->set_key(radio, 0, frame, &key, &slot) == 0 && slot_cleared == 4);
    k_memset(&bss, 0, sizeof(bss));
    bss.bssid[0] = 0x02;
    bss.bssid[5] = 0x99;
    CHECK(ops->sta_state(radio, &bss, WLAN_STA_NONE, WLAN_STA_AUTHENTICATING) == 0);
    CHECK(bssid_set[0] == 0x02 && bssid_set[5] == 0x99);
    CHECK(ampdu_sta_bss == NULL && !card.mac.associated);
    /* The association narrows the filter and starts the receive window. */
    card.recover.rx_active = 7;
    CHECK(ops->sta_state(radio, &bss, WLAN_STA_AUTHENTICATED, WLAN_STA_ASSOCIATED) == 0);
    CHECK(ampdu_sta_bss == &bss && card.mac.associated);
    CHECK(card.recover.rx_active == 0 && card.recover.window == tick_count());
    CHECK(ops->sta_state(radio, &bss, WLAN_STA_ASSOCIATED, WLAN_STA_AUTHORIZED) == 0);
    CHECK(card.mac.associated);
    CHECK(ops->sta_state(radio, NULL, WLAN_STA_AUTHORIZED, WLAN_STA_NONE) == 0);
    CHECK(bssid_set[0] == 0 && bssid_set[5] == 0 && ampdu_sta_bss == NULL);
    CHECK(!card.mac.associated);
    CHECK(ops->ampdu_action(radio, WLAN_AMPDU_TX_START, frame, 0, 0, 64) == 0);
    CHECK(ops->get_tsf(radio, &tsf) == 0 && tsf == 0x123456789ull);
    {
        static const char *const misc[] = {
            "tx",        "key_set",       "key_clear", "mac_set_bssid", "mac_set_filter",
            "ampdu_sta", "mac_set_bssid", "ampdu_sta", "ampdu_action",  NULL};
        CHECK(calls_are(misc));
    }

    /* The stop: the work and DMA stopped, then a cold reset and full sleep;
     * twice is once. */
    notes_clear();
    ath9k_model_log_clear(&model);
    ops->stop(radio);
    CHECK(calls_are(stop));
    CHECK(!card.bus.started);
    CHECK(ath9k_model_last_write(&model, AR_RTC_RC, 0) == 0);
    cold = ath9k_model_find(&model, 0, ATH9K_MODEL_WRITE, AR_RTC_RC);
    CHECK(cold >= 0 && model.log[cold].value == (AR_RTC_RC_MAC_WARM | AR_RTC_RC_MAC_COLD));
    CHECK(card.hw.power == ATH9K_PM_FULL_SLEEP);
    CHECK(!(ath9k_model_peek(&model, AR_RTC_RESET) & AR_RTC_RESET_EN));
    notes_clear();
    ops->stop(radio);
    CHECK(calls_are(none));
    CHECK(ops->tx(radio, frame, sizeof(frame), &info) == -ENETDOWN);

    /* A start that fails leaves the radio stopped and the chip asleep, and
     * the next start resets it cold again. */
    cal_rc = -EIO;
    notes_clear();
    rc = ops->start(radio);
    CHECK(rc == -EIO);
    CHECK(logged("ath9k: start: failed: -5"));
    CHECK(!card.bus.started);
    CHECK(call_index("cal_start") == ~0u);
    CHECK(card.hw.power == ATH9K_PM_FULL_SLEEP);
    cal_rc = 0;
    ath9k_model_log_clear(&model);
    CHECK(ops->start(radio) == 0);
    dis = ath9k_model_find(&model, 0, ATH9K_MODEL_WRITE, AR_PHY_ACTIVE);
    cold = dis < 0 ? -1 : ath9k_model_find(&model, (unsigned)dis, ATH9K_MODEL_WRITE, AR_RTC_RC);
    CHECK(cold >= 0 && model.log[cold].value == (AR_RTC_RC_MAC_WARM | AR_RTC_RC_MAC_COLD));
    ops->stop(radio);
}

/** Revision 1.1 names itself on the boot line and takes its radio
 * postamble, the one table of ath9k's 1.1 that is not 1.0's. */
static void test_rev11(void)
{
    struct ath9k_ini rev101;

    lenovo_model(1);
    bus_ready();
    CHECK(ath9k_probe() == 0);
    rev101 = card.hw.radio[ATH9K_INI_POST];
    lenovo_model(2);
    bus_ready();
    CHECK(ath9k_probe() == 0);
    CHECK(logged("ath9k: probe: 02:00.0 AR9565 rev 1.1 mac b8:ee:65:32:89:15 subsys 17aa:4026"));
    CHECK(card.hw.rev && card.hw.rev->mac_rev == 2);
    CHECK(card.hw.radio[ATH9K_INI_POST].words &&
          card.hw.radio[ATH9K_INI_POST].words != rev101.words);
}

/** The cards attach refuses, each with AR_SREV named, and a chip whose RTC
 * never comes on. */
static void test_refusals(void)
{
    /* Off the bus. */
    lenovo_model(1);
    model.dead = 1;
    bus_ready();
    CHECK(ath9k_probe() == -ENODEV);
    CHECK(logged("ath9k: attach: refused: AR_SREV (0x4020) reads 0xffffffff"));
    CHECK(logged("ath9k: probe: failed: 02:00.0: attach: -19"));
    CHECK(registered == NULL);

    /* An older family's form of AR_SREV. */
    lenovo_model(1);
    model.srev = 0x00000182;
    bus_ready();
    CHECK(ath9k_probe() == -ENODEV);
    CHECK(logged("ath9k: attach: refused: AR_SREV (0x4020) reads 0x182, not an AR9003"));

    /* A revision the row does not know. */
    lenovo_model(5);
    bus_ready();
    CHECK(ath9k_probe() == -ENODEV);
    CHECK(logged("ath9k: attach: refused: AR_SREV (0x4020) reads version 0x2c0 rev 5, "
                 "not an AR9565"));
    CHECK(registered == NULL);

    /* No card at all: nothing is logged. */
    lenovo_model(1);
    bus_ready();
    card_present = 0;
    CHECK(ath9k_probe() == -ENODEV);
    CHECK(n_lines == 0);

    /* An RTC that stays asleep fails the power-on reset. */
    lenovo_model(1);
    model.stuck_rtc = 1;
    bus_ready();
    CHECK(ath9k_probe() == -EIO);
    CHECK(logged_prefix("ath9k: reset: failed: AR_RTC_STATUS (0x7044) reads 0x"));
    CHECK(logged("ath9k: probe: failed: 02:00.0: attach: -5"));
    CHECK(registered == NULL);

    /* A refused registration gives the interrupt back, leaves the chip
     * asleep and the tick idle. */
    lenovo_model(1);
    bus_ready();
    register_rc = -ENOSPC;
    CHECK(ath9k_probe() == -ENOSPC);
    CHECK(logged("ath9k: probe: failed: register: -28"));
    CHECK(call_index("intr_detach") != ~0u);
    CHECK(card.hw.power == ATH9K_PM_FULL_SLEEP);
    CHECK(!rx_held && !tx_held);
    notes_clear();
    ath9k_poll();
    CHECK(n_calls == 0);

    /* A part that fails after attach leaves the chip asleep. */
    lenovo_model(1);
    bus_ready();
    intr_rc = -ENOMEM;
    CHECK(ath9k_probe() == -ENOMEM);
    CHECK(logged("ath9k: probe: failed: 02:00.0: -12"));
    CHECK(registered == NULL);
    CHECK(card.hw.power == ATH9K_PM_FULL_SLEEP);
    CHECK(!(ath9k_model_peek(&model, AR_RTC_RESET) & AR_RTC_RESET_EN));
    CHECK(!rx_held && !tx_held);

    /* Transmit's pages that cannot be had: receive's are given back, once
     * the chip is reset cold. */
    lenovo_model(1);
    bus_ready();
    ath9k_model_log_clear(&model);
    tx_rc = -ENOMEM;
    CHECK(ath9k_probe() == -ENOMEM);
    CHECK(registered == NULL && !rx_held && !tx_held);
    {
        int at = -1, cold = -1;

        while ((at = ath9k_model_find(&model, (unsigned)(at + 1), ATH9K_MODEL_WRITE, AR_RTC_RC)) >=
               0)
            if (model.log[at].value == (AR_RTC_RC_MAC_WARM | AR_RTC_RC_MAC_COLD))
                cold = at;
        CHECK(cold >= 0 && (unsigned)cold < call_index("tx_free"));
        CHECK(n_calls >= 2 && streq(calls[n_calls - 2], "tx_free") &&
              streq(calls[n_calls - 1], "rx_free"));
    }
    CHECK(card.hw.power == ATH9K_PM_FULL_SLEEP);
}

/* The EEPROM's words for the image test. */
static uint16_t eeprom[512];

/** Put byte `b` at EEPROM byte address `at`, the odd byte the high half of
 * its word, as ath9k reads them. */
static void eeprom_put(unsigned at, uint8_t b)
{
    if (at & 1)
        eeprom[at / 2] = (uint16_t)((eeprom[at / 2] & 0x00ff) | b << 8);
    else
        eeprom[at / 2] = (uint16_t)((eeprom[at / 2] & 0xff00) | b);
}

/**
 * Write an EEPROM of one _CompressBlock block at byte 0x3ff over template
 * 2 (the default) that sets the MAC address to 00:03:7f:12:34:56 and the
 * regulatory domain to 0x10; `skew` is added to its checksum.
 */
static void eeprom_image(unsigned skew)
{
    static const uint8_t runs[] = {2, 6, 0x00, 0x03, 0x7f, 0x12, 0x34, 0x56};
    uint8_t block[64];
    unsigned i, n = 0, len, sum = 0;
    unsigned reg_at = (unsigned)((const uint8_t *)&ar9300_default.baseEepHeader.regDmn[0] -
                                 (const uint8_t *)&ar9300_default);

    k_memset(eeprom, 0xff, sizeof(eeprom));
    /* The MAC address, then a skip to regDmn[0] and one byte. */
    for (i = 0; i < 8; i++)
        block[4 + n++] = runs[i];
    block[4 + n++] = (uint8_t)(reg_at - 8);
    block[4 + n++] = 1;
    block[4 + n++] = 0x10;
    len = n;
    block[0] = (uint8_t)(_CompressBlock << 5 | 2);
    block[1] = (uint8_t)(len >> 4);
    block[2] = (uint8_t)((len & 0xf) << 4);
    block[3] = 0;
    for (i = 0; i < len; i++)
        sum += block[4 + i];
    sum += skew;
    block[4 + len] = (uint8_t)sum;
    block[5 + len] = (uint8_t)(sum >> 8);
    for (i = 0; i < 4 + len + 2; i++)
        eeprom_put(0x3ff - i, block[i]);
    /* An empty header ends the blocks. */
    for (i = 0; i < 4; i++)
        eeprom_put(0x3ff - (4 + len + 2) - i, 0);
}

/** An image in the EEPROM, one with a bad checksum, and no image. */
static void test_eeprom(void)
{
    const uint8_t *image = (const uint8_t *)&card.eeprom.image;
    const uint8_t *t = (const uint8_t *)&ar9300_default;
    unsigned i, differ = 0;

    lenovo_model(1);
    ath9k_model_set_otp(&model, NULL, 0);
    eeprom_image(0);
    ath9k_model_set_eeprom(&model, eeprom, 512);
    bus_ready();
    CHECK(ath9k_probe() == 0);
    CHECK(logged("ath9k: probe: 02:00.0 AR9565 rev 1.0.1 mac 00:03:7f:12:34:56 subsys 17aa:4026"));
    CHECK(card.eeprom.regdomain == 0x10);
    CHECK(card.eeprom.image.templateVersion == 2);
    for (i = 0; i < sizeof(card.eeprom.image); i++)
        differ += image[i] != t[i];
    CHECK(differ == 6); /* the domain and the address, whose first byte is 0 in both */
    CHECK(ath9k_model_count(&model, ATH9K_MODEL_READ, AR9300_OTP_BASE + 4 * 0xff) == 0);

    /* The one block's checksum is wrong: nothing is restored. */
    lenovo_model(1);
    ath9k_model_set_otp(&model, NULL, 0);
    eeprom_image(1);
    ath9k_model_set_eeprom(&model, eeprom, 512);
    bus_ready();
    CHECK(ath9k_probe() == -EIO);
    CHECK(logged("ath9k: eeprom: failed: no block of the image restored"));
    CHECK(logged("ath9k: probe: failed: 02:00.0: attach: -5"));

    /* Neither store holds a header. */
    lenovo_model(1);
    ath9k_model_set_otp(&model, NULL, 0);
    bus_ready();
    CHECK(ath9k_probe() == -EIO);
    CHECK(logged("ath9k: eeprom: failed: no EEPROM or OTP holds an image"));
    CHECK(registered == NULL);
}

int main(void)
{
    test_recorded();
    test_radio();
    test_rev11();
    test_refusals();
    test_eeprom();
    if (failures)
        printf("ath9k_hw_hosttest: %u checks failed\n", failures);
    return failures > 255 ? 255 : (int)failures;
}
