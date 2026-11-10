/*
 * intelbl_hosttest.c: the Intel PCH backlight's register arithmetic, against
 * a model of the four PWM registers and the panel power sequencer's.
 *
 * intelbl.c is included whole, with its attach, get and set, which i915
 * also calls (intelbl.h); the boot's probe, which maps BAR 0, is left out
 * by RACH2_HOSTTEST. The rows take the PWM as the firmware leaves it: the
 * Lenovo G50-70's 0x03A903A9, a period of 937 at full duty; one with the
 * polarity bit; one the firmware left off or without a period, which must
 * be refused untouched; and one the processor's PWM still drives, whose
 * level must pass to the PCH's before the processor's is turned off, as
 * i915 does. The sequencer's switch is taken only when the firmware left
 * the panel lit by it, and turns the backlight off and on by its enable
 * bit alone, the duty cycle kept.
 */
extern int printf(const char *fmt, ...);

#include "display/intelbl.c"

/* The registers, and the writes made to each. */
struct regs {
    uint32_t cpu_ctl2, cpu_ctl, pch_ctl1, pch_ctl2;
    int writes;
    uint32_t pp_control; /* as the hardware reads it back, without the key */
};

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

/** The model's register `reg`; another offset is a fault of the code. */
static uint32_t *reg_of(struct regs *r, uint32_t reg)
{
    switch (reg) {
    case BLC_PWM_CPU_CTL2:
        return &r->cpu_ctl2;
    case BLC_PWM_CPU_CTL:
        return &r->cpu_ctl;
    case BLC_PWM_PCH_CTL1:
        return &r->pch_ctl1;
    case BLC_PWM_PCH_CTL2:
        return &r->pch_ctl2;
    case PP_CONTROL:
        return &r->pp_control;
    default:
        return 0;
    }
}

/** Read register `reg` of the model at `io`. */
static uint32_t model_rd(void *io, uint32_t reg)
{
    uint32_t *p = reg_of((struct regs *)io, reg);

    CHECK(p != 0, "a read of a register the PWM does not have");
    return p != 0 ? *p : 0xFFFFFFFFu;
}

/** Write register `reg` of the model at `io`, and count the write. */
static void model_wr(void *io, uint32_t reg, uint32_t value)
{
    struct regs *r = (struct regs *)io;
    uint32_t *p = reg_of(r, reg);

    CHECK(p != 0, "a write of a register the PWM does not have");
    if (reg == PP_CONTROL) {
        CHECK((value & PANEL_KEY_MASK) == PANEL_UNLOCK, "PP_CONTROL is written with its key");
        value &= ~PANEL_KEY_MASK;
    }
    if (p != 0) {
        *p = value;
    }
    r->writes++;
}

/** Attach `bl` to the model `r`: intelbl_attach's result. */
static int attach(struct intelbl *bl, struct regs *r, const char **why)
{
    bl->rd = model_rd;
    bl->wr = model_wr;
    bl->io = r;
    return intelbl_attach(bl, why);
}

/** The Lenovo's PWM: a period of 937, full, no polarity, no override. */
static void check_lenovo(void)
{
    struct regs r = {0, 0, 0x80000000u, 0x03A903A9u, 0, 0};
    struct intelbl bl;
    const char *why;

    CHECK(attach(&bl, &r, &why) == 0, "the Lenovo's PWM is taken");
    CHECK(bl.max == 937, "the period, 937, is the highest level");
    CHECK(!bl.inverted, "no polarity bit, no inversion");
    CHECK(r.writes == 0, "a PWM the processor does not drive is taken without a write");
    CHECK(intelbl_get(&bl) == 937, "the level read is the full duty cycle");
    intelbl_set(&bl, 468);
    CHECK(r.pch_ctl2 == 0x03A901D4u, "a level of 468 is the duty cycle, the period kept");
    CHECK(intelbl_get(&bl) == 468, "the level reads back");
    intelbl_set(&bl, 0);
    CHECK(r.pch_ctl2 == 0x03A90000u, "a level of 0 is a duty cycle of 0");
    intelbl_set(&bl, -5);
    CHECK(r.pch_ctl2 == 0x03A90000u, "a level below 0 is taken as 0");
    intelbl_set(&bl, 5000);
    CHECK(r.pch_ctl2 == 0x03A903A9u, "a level past the period is taken as the period");
    CHECK(r.pch_ctl1 == 0x80000000u && r.cpu_ctl2 == 0, "a set writes the duty cycle alone");
}

/** The polarity bit: the duty cycle is the dark part of the period. */
static void check_inverted(void)
{
    struct regs r = {0, 0, 0xA0000000u, 0x03A90064u, 0, 0};
    struct intelbl bl;
    const char *why;

    CHECK(attach(&bl, &r, &why) == 0, "a PWM with the polarity bit is taken");
    CHECK(bl.inverted, "the polarity bit inverts");
    CHECK(intelbl_get(&bl) == 837, "a duty cycle of 100 of 937 dark is a level of 837");
    intelbl_set(&bl, 900);
    CHECK((r.pch_ctl2 & 0xFFFFu) == 37, "a level of 900 is a duty cycle of 37");
    intelbl_set(&bl, 0);
    CHECK((r.pch_ctl2 & 0xFFFFu) == 937, "a level of 0 is the whole period dark");
}

/** A PWM the firmware left off, or with no period, is refused untouched. */
static void check_refused(void)
{
    struct regs off = {0, 0, 0x00000000u, 0x03A903A9u, 0, 0};
    struct regs none = {0, 0, 0x80000000u, 0x000003A9u, 0, 0};
    struct intelbl bl;
    const char *why = "";

    CHECK(attach(&bl, &off, &why) == -1 && why[0] != '\0', "a PWM left off is refused");
    CHECK(off.writes == 0, "a PWM left off is not written");
    CHECK(attach(&bl, &none, &why) == -1 && why[0] != '\0', "a PWM with no period is refused");
    CHECK(none.writes == 0, "a PWM with no period is not written");
}

/**
 * The processor's PWM on and the PCH's override clear: its duty cycle goes
 * to the PCH's, the override is set and the processor's PWM turned off; a
 * duty cycle past the period is cut to it. With the override set already,
 * nothing is written.
 */
static void check_cpu_mode(void)
{
    struct regs r = {0x80000000u, 0x12340200u, 0x80000000u, 0x03A903A9u, 0, 0};
    struct regs big = {0x80000000u, 0x0000FFFFu, 0x80000000u, 0x03A903A9u, 0, 0};
    struct regs set = {0x80000000u, 0x00000200u, 0xC0000000u, 0x03A903A9u, 0, 0};
    struct intelbl bl;
    const char *why;

    CHECK(attach(&bl, &r, &why) == 0, "a PWM the processor drives is taken");
    CHECK(r.pch_ctl2 == 0x03A90200u, "the processor's duty cycle passes to the PCH's");
    CHECK(r.pch_ctl1 == 0xC0000000u, "the PCH's override is set");
    CHECK(r.cpu_ctl2 == 0, "the processor's PWM is turned off");
    CHECK(intelbl_get(&bl) == 512, "the level read is the processor's");
    CHECK(attach(&bl, &big, &why) == 0 && (big.pch_ctl2 & 0xFFFFu) == 937,
          "a processor's duty cycle past the period is cut to it");
    CHECK(attach(&bl, &set, &why) == 0 && set.writes == 0,
          "with the override set already nothing is written");
}

/**
 * The sequencer's switch: taken when the firmware left the panel's power
 * and backlight enable on, as the Lenovo's PP_CONTROL of 7, and not
 * otherwise; off and on change the enable bit alone and keep the duty.
 */
static void check_switch(void)
{
    struct regs r = {0, 0, 0x80000000u, 0x03A903A9u, 0, 7};
    struct regs dark = {0, 0, 0x80000000u, 0x03A903A9u, 0, 3};
    struct regs unpowered = {0, 0, 0x80000000u, 0x03A903A9u, 0, 4};
    struct intelbl bl;
    const char *why;

    CHECK(attach(&bl, &r, &why) == 0, "the Lenovo's PWM is taken");
    intelbl_attach_switch(&bl);
    CHECK(bl.switched && r.writes == 0, "a panel lit by the sequencer is switched, unwritten");
    intelbl_power(&bl, 0);
    CHECK(r.pp_control == 3, "off clears the backlight enable alone");
    CHECK(r.pch_ctl2 == 0x03A903A9u && intelbl_get(&bl) == 937, "off keeps the duty cycle");
    intelbl_power(&bl, 1);
    CHECK(r.pp_control == 7, "on sets the backlight enable again");
    CHECK(attach(&bl, &dark, &why) == 0, "a PWM beside a dark enable is taken");
    intelbl_attach_switch(&bl);
    CHECK(!bl.switched, "a backlight enable left off is no switch");
    CHECK(attach(&bl, &unpowered, &why) == 0, "a PWM beside an unpowered panel is taken");
    intelbl_attach_switch(&bl);
    CHECK(!bl.switched, "a panel the sequencer did not power is no switch");
}

int main(void)
{
    check_lenovo();
    check_inverted();
    check_refused();
    check_cpu_mode();
    check_switch();
    if (fails != 0) {
        printf("intelbl_hosttest: %d failed\n", fails);
        return 1;
    }
    printf("intelbl_hosttest: ok\n");
    return 0;
}
