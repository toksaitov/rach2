/*
 * intelbl.c: the backlight of a panel driven by the PWM of an Intel PCH,
 * Lynx Point or Wildcat Point, registered as "intel_backlight", as Linux's
 * i915 drives it without the rest of the graphics driver.
 */

#include "backlight.h"

#include "../lib/kdef.h"
#ifndef RACH2_HOSTTEST
#include "../klog.h"
#include "../pci.h"
#include "../arch/x86/board.h"
#include "../mm/paging.h"
#endif

/*
 * The registers, in the graphics device's BAR 0, as i915 names them. The
 * PCH's PWM drives the panel: its period, the highest level, in the high
 * half of BLC_PWM_PCH_CTL2 and its duty cycle, the level, in the low half.
 * With the polarity bit the duty cycle counts the dark part of the period.
 * The processor's own PWM may drive the panel in place of the PCH's until
 * the PCH's override bit is set, as on a Lynx Point the firmware left so.
 * The PCH's panel power sequencer drives the panel's backlight enable line
 * by a bit of PP_CONTROL, which takes a write only with the unlock key in
 * its high half.
 */
#define BLC_PWM_CPU_CTL2 0x48250u
#define BLC_PWM_CPU_CTL  0x48254u
#define BLC_PWM_PCH_CTL1 0xC8250u
#define BLC_PWM_PCH_CTL2 0xC8254u
#define BLM_PWM_ENABLE   0x80000000u /* CPU_CTL2: the processor's PWM is on */
#define BLM_PCH_PWM      0x80000000u /* PCH_CTL1: the PCH's PWM is on */
#define BLM_PCH_OVERRIDE 0x40000000u /* PCH_CTL1: the PCH's PWM drives the panel */
#define BLM_PCH_POLARITY 0x20000000u /* PCH_CTL1: the duty cycle is the dark part */
#define BLC_DUTY_MASK    0xFFFFu
#define PP_CONTROL       0xC7204u
#define PANEL_POWER_ON   0x00000001u /* PP_CONTROL: the panel's power is on */
#define EDP_BLC_ENABLE   0x00000004u /* PP_CONTROL: the backlight enable line is on */
#define PANEL_UNLOCK     0xABCD0000u /* PP_CONTROL: the key a write carries */
#define PANEL_KEY_MASK   0xFFFF0000u

/* The pages of BAR 0 that hold the two pairs of registers and the panel
 * power sequencer's. */
#define CPU_PAGE 0x48000u
#define PPS_PAGE 0xC7000u
#define PCH_PAGE 0xC8000u

/*
 * A PWM as the code below drives it, through `rd` and `wr` on `io`, which
 * take a register's offset in BAR 0: the mapped pages at boot, a model of
 * the registers in tests/host/intelbl_hosttest.c.
 */
struct intelbl {
    uint32_t (*rd)(void *io, uint32_t reg);
    void (*wr)(void *io, uint32_t reg, uint32_t value);
    void *io;
    int max;      /* the period, the highest level */
    int inverted; /* the polarity bit was set */
    int switched; /* the sequencer's backlight enable turns the panel off */
};

/**
 * Take the PWM the firmware left: 0, with `bl` ready for `intelbl_get` and
 * `intelbl_set`, or -1 with the reason in `*why` when the PCH's PWM is off
 * or has no period, which leaves it untouched. A processor's PWM that still
 * drives the panel gives its level to the PCH's and is turned off, with the
 * PCH's override set, as i915's lpt_setup_backlight does.
 */
static int intelbl_attach(struct intelbl *bl, const char **why)
{
    uint32_t ctl1 = bl->rd(bl->io, BLC_PWM_PCH_CTL1);
    uint32_t ctl2 = bl->rd(bl->io, BLC_PWM_PCH_CTL2);
    uint32_t cpu_ctl2, duty;

    if (!(ctl1 & BLM_PCH_PWM)) {
        *why = "the firmware left the PWM off";
        return -1;
    }
    bl->max = (int)(ctl2 >> 16);
    if (bl->max == 0) {
        *why = "the PWM has no period";
        return -1;
    }
    bl->inverted = (ctl1 & BLM_PCH_POLARITY) != 0;
    cpu_ctl2 = bl->rd(bl->io, BLC_PWM_CPU_CTL2);
    if (!(ctl1 & BLM_PCH_OVERRIDE) && (cpu_ctl2 & BLM_PWM_ENABLE)) {
        duty = bl->rd(bl->io, BLC_PWM_CPU_CTL) & BLC_DUTY_MASK;
        if (duty > (uint32_t)bl->max) {
            duty = (uint32_t)bl->max;
        }
        bl->wr(bl->io, BLC_PWM_PCH_CTL2, (ctl2 & ~BLC_DUTY_MASK) | duty);
        bl->wr(bl->io, BLC_PWM_PCH_CTL1, ctl1 | BLM_PCH_OVERRIDE);
        bl->wr(bl->io, BLC_PWM_CPU_CTL2, cpu_ctl2 & ~BLM_PWM_ENABLE);
    }
    *why = "";
    return 0;
}

/** The level the PWM gives the panel now: the backlight class's `get`. */
static int intelbl_get(void *ctx)
{
    struct intelbl *bl = (struct intelbl *)ctx;
    int duty = (int)(bl->rd(bl->io, BLC_PWM_PCH_CTL2) & BLC_DUTY_MASK);

    if (duty > bl->max) {
        duty = bl->max;
    }
    return bl->inverted ? bl->max - duty : duty;
}

/** Give the panel `level`, 0 to the period: the backlight class's `set`.
 * The period, in the same register, is written back as it was. */
static void intelbl_set(void *ctx, int level)
{
    struct intelbl *bl = (struct intelbl *)ctx;
    uint32_t ctl2 = bl->rd(bl->io, BLC_PWM_PCH_CTL2);

    if (level < 0) {
        level = 0;
    } else if (level > bl->max) {
        level = bl->max;
    }
    if (bl->inverted) {
        level = bl->max - level;
    }
    bl->wr(bl->io, BLC_PWM_PCH_CTL2, (ctl2 & ~BLC_DUTY_MASK) | (uint32_t)level);
}

/**
 * Whether the panel power sequencer lit the panel, its power and its
 * backlight enable both on, as the firmware leaves an eDP panel it drew
 * on: `switched`, and then `intelbl_power` turns the backlight off and on.
 * Nothing is written.
 */
static void intelbl_attach_switch(struct intelbl *bl)
{
    uint32_t pp = bl->rd(bl->io, PP_CONTROL);

    bl->switched = (pp & (PANEL_POWER_ON | EDP_BLC_ENABLE)) == (PANEL_POWER_ON | EDP_BLC_ENABLE);
}

/**
 * Turn the panel's backlight on or off by the sequencer's enable line,
 * leaving the duty cycle as it is: the backlight class's `power`, as
 * i915's bl_power does. The firmware's brightness keys, and the EC that
 * raises them, see the level they saw before, which a duty cycle of 0
 * would change under them. The other bits are written back as read, with
 * the unlock key.
 */
static void intelbl_power(void *ctx, int on)
{
    struct intelbl *bl = (struct intelbl *)ctx;
    uint32_t pp = bl->rd(bl->io, PP_CONTROL) & ~PANEL_KEY_MASK;

    pp = on ? pp | EDP_BLC_ENABLE : pp & ~EDP_BLC_ENABLE;
    bl->wr(bl->io, PP_CONTROL, pp | PANEL_UNLOCK);
}

#ifndef RACH2_HOSTTEST

/* The three pages of BAR 0 as `map_mmio` mapped them. */
static uintptr_t cpu_regs;
static uintptr_t pps_regs;
static uintptr_t pch_regs;
static struct intelbl the_pwm;

/** Read register `reg` of the mapped pages. */
static uint32_t mmio_rd(void *io, uint32_t reg)
{
    uintptr_t at = reg >= PCH_PAGE   ? pch_regs + (reg - PCH_PAGE)
                   : reg >= PPS_PAGE ? pps_regs + (reg - PPS_PAGE)
                                     : cpu_regs + (reg - CPU_PAGE);

    (void)io;
    return *(volatile uint32_t *)at;
}

/** Write register `reg` of the mapped pages. */
static void mmio_wr(void *io, uint32_t reg, uint32_t value)
{
    uintptr_t at = reg >= PCH_PAGE   ? pch_regs + (reg - PCH_PAGE)
                   : reg >= PPS_PAGE ? pps_regs + (reg - PPS_PAGE)
                                     : cpu_regs + (reg - CPU_PAGE);

    (void)io;
    *(volatile uint32_t *)at = value;
}

/** Whether the board's PCH is one whose PWM this driver knows. i915 takes
 * Wildcat Point for a Lynx Point with other device ids, and so does this. */
static int pch_is_lpt(const char *name)
{
    static const char *const names[] = {"lynxpoint", "lynxpoint-lp", "wildcatpoint-lp"};
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; names[i][j] != '\0' && names[i][j] == name[j]; j++) {
        }
        if (names[i][j] == '\0' && name[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* The ops with the sequencer's switch, and without it, where off is a
 * duty cycle of 0. */
static const struct backlight_ops intelbl_ops = {intelbl_get, intelbl_set, intelbl_power};
static const struct backlight_ops intelbl_ops_noswitch = {intelbl_get, intelbl_set, NULL};

/**
 * Register "intel_backlight" when the graphics device at 0:2.0 is Intel's,
 * the board's PCH is a Lynx Point or a Wildcat Point, and the firmware left
 * the PCH's PWM on with a period: the panel's backlight, which the firmware
 * lit for its own picture. Nothing else is written, so on another machine,
 * QEMU's among them, the device is left as it was.
 */
void intelbl_init(void)
{
    uint16_t vendor = pci_read_config16(0, 2, 0, PCI_VENDOR_ID);
    pci_device_t gfx;
    uint64_t bar0;
    const char *why;
    int i;

    if (vendor != 0x8086 || pci_read_config8(0, 2, 0, PCI_CLASS) != 0x03) {
        return;
    }
    if (!pch_is_lpt(board_name())) {
        klog(KLOG_INFO, "backlight", "intel: none: the PCH is %s, not a Lynx or Wildcat Point",
             board_name());
        return;
    }
    if (!(pci_read_config16(0, 2, 0, PCI_COMMAND) & PCI_CMD_MEMORY_SPACE)) {
        klog(KLOG_INFO, "backlight", "intel: none: the graphics device decodes no memory");
        return;
    }
    gfx.bus = 0;
    gfx.device = 2;
    gfx.function = 0;
    for (i = 0; i < 6; i++) {
        gfx.bar[i] = pci_read_config32(0, 2, 0, (uint8_t)(PCI_BAR0 + 4 * i));
    }
    bar0 = pci_bar_address64(&gfx, 0);
    if (bar0 == 0 || (uint64_t)(uintptr_t)(bar0 + PCH_PAGE + 4096) != bar0 + PCH_PAGE + 4096) {
        klog(KLOG_INFO, "backlight", "intel: none: BAR 0 is out of reach");
        return;
    }
    cpu_regs = map_mmio((uintptr_t)bar0 + CPU_PAGE, 4096);
    pps_regs = map_mmio((uintptr_t)bar0 + PPS_PAGE, 4096);
    pch_regs = map_mmio((uintptr_t)bar0 + PCH_PAGE, 4096);
    if (cpu_regs == 0 || pps_regs == 0 || pch_regs == 0) {
        return;
    }
    the_pwm.rd = mmio_rd;
    the_pwm.wr = mmio_wr;
    the_pwm.io = NULL;
    if (intelbl_attach(&the_pwm, &why) != 0) {
        klog(KLOG_INFO, "backlight", "intel: none: %s", why);
        return;
    }
    intelbl_attach_switch(&the_pwm);
    if (backlight_register("intel_backlight", the_pwm.max,
                           the_pwm.switched ? &intelbl_ops : &intelbl_ops_noswitch, &the_pwm) < 0) {
        return;
    }
    klog(KLOG_INFO, "backlight", "intel_backlight: %s PWM, levels 0 to %d, at %d%s%s", board_name(),
         the_pwm.max, intelbl_get(&the_pwm), the_pwm.inverted ? ", inverted" : "",
         the_pwm.switched ? ", off by the panel's enable" : ", off by a level of 0");
}

#endif /* RACH2_HOSTTEST */
