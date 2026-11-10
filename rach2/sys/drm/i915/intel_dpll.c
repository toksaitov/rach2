/*
 * intel_dpll.c: Haswell's display PLLs, after Linux's
 * display/intel_dpll_mgr.c: the two WRPLLs that clock HDMI, the SPLL, and
 * the LCPLL's outputs that clock DisplayPort, shared between pipes.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* The LCPLL's frequency in MHz, and the WRPLL's limits: P's range and
 * step, the reference's and the VCO's ranges in MHz. */
#define LC_FREQ    2700u
#define LC_FREQ_2K ((uint64_t)LC_FREQ * 2000u)
#define P_MIN      2u
#define P_MAX      64u
#define P_INC      2u
#define REF_MIN    48u
#define REF_MAX    400u
#define VCO_MIN    2400u
#define VCO_MAX    4800u

/* The references in kHz: the LCPLL, and the PCH's spread clock that a ULT
 * part's special reference is. */
#define REFCLK_LCPLL 2700000u
#define REFCLK_SSC   135000u

/* How long a WRPLL or the SPLL takes to lock once enabled. */
#define PLL_LOCK_US 20u

static const char *const dpll_names[INTEL_NUM_DPLLS] = {"WRPLL 1",   "WRPLL 2",    "SPLL",
                                                        "LCPLL 810", "LCPLL 1350", "LCPLL 2700"};

/* ------------------------------------------------------------------------
 * The WRPLL's dividers
 */

/* The best dividers found so far; `p` is 0 before the first. */
struct wrpll_rnp {
    uint32_t p, n2, r2;
};

/** The error in parts per million a clock of `clock` Hz may have, Linux's
 * hsw_wrpll_get_budget_for_freq. */
static uint32_t wrpll_budget(uint32_t clock)
{
    switch (clock) {
    case 25175000u:
    case 25200000u:
    case 27000000u:
    case 27027000u:
    case 37762500u:
    case 37800000u:
    case 40500000u:
    case 40541000u:
    case 54000000u:
    case 54054000u:
    case 59341000u:
    case 59400000u:
    case 72000000u:
    case 74176000u:
    case 74250000u:
    case 81000000u:
    case 81081000u:
    case 89012000u:
    case 89100000u:
    case 108000000u:
    case 108108000u:
    case 111264000u:
    case 111375000u:
    case 148352000u:
    case 148500000u:
    case 162000000u:
    case 162162000u:
    case 222525000u:
    case 222750000u:
    case 296703000u:
    case 297000000u:
        return 0;
    case 233500000u:
    case 245250000u:
    case 247750000u:
    case 253250000u:
    case 298000000u:
        return 1500;
    case 169128000u:
    case 169500000u:
    case 179500000u:
    case 202000000u:
        return 2000;
    case 256250000u:
    case 262500000u:
    case 270000000u:
    case 272500000u:
    case 273750000u:
    case 280750000u:
    case 281250000u:
    case 286000000u:
    case 291750000u:
        return 4000;
    case 267250000u:
    case 268500000u:
        return 5000;
    default:
        return 1000;
    }
}

/** |a - b|. */
static uint64_t abs_diff64(uint64_t a, uint64_t b)
{
    return a > b ? a - b : b - a;
}

/**
 * Keep `r2`, `n2` and `p` in `best` when they beat it, Linux's
 * hsw_wrpll_update_rnp: the output is LC_FREQ_2K / 2000 * N / (P * R),
 * against `freq2k`; outside the budget the closer wins, and within it the
 * larger reference times VCO, N / (P * R^2).
 */
static void wrpll_update_rnp(uint64_t freq2k, uint32_t budget, uint32_t r2, uint32_t n2, uint32_t p,
                             struct wrpll_rnp *best)
{
    uint64_t a, b, c, d, diff, diff_best;

    if (best->p == 0) {
        best->p = p;
        best->n2 = n2;
        best->r2 = r2;
        return;
    }
    a = freq2k * budget * p * r2;
    b = freq2k * budget * best->p * best->r2;
    diff = abs_diff64(freq2k * p * r2, LC_FREQ_2K * n2);
    diff_best = abs_diff64(freq2k * best->p * best->r2, LC_FREQ_2K * best->n2);
    c = 1000000u * diff;
    d = 1000000u * diff_best;
    if (a < c && b < d) {
        /* Both over the budget: the closer. */
        if ((uint64_t)best->p * best->r2 * diff < (uint64_t)p * r2 * diff_best) {
            best->p = p;
            best->n2 = n2;
            best->r2 = r2;
        }
    } else if (a >= c && b < d) {
        /* The new one within the budget, the best not. */
        best->p = p;
        best->n2 = n2;
        best->r2 = r2;
    } else if (a >= c && b >= d) {
        /* Both within it: the larger N / R^2. */
        if (n2 * best->r2 * best->r2 > best->n2 * r2 * r2) {
            best->p = p;
            best->n2 = n2;
            best->r2 = r2;
        }
    }
}

void intel_dpll_calc_wrpll(uint32_t clock, uint32_t *r2_out, uint32_t *n2_out, uint32_t *p_out)
{
    struct wrpll_rnp best = {0, 0, 0};
    uint64_t freq2k = clock / 100u;
    uint32_t budget = wrpll_budget(clock), p, n2, r2;

    /* 540 MHz bypasses the WRPLL: the LCPLL passes straight through. */
    if (freq2k == 5400000u) {
        *n2_out = 2;
        *p_out = 1;
        *r2_out = 2;
        return;
    }
    /* REF_MIN <= LC_FREQ / R <= REF_MAX, with R2 = 2R, and VCO_MIN <= N *
     * LC_FREQ / R <= VCO_MAX, with N2 = 2N. */
    for (r2 = LC_FREQ * 2u / REF_MAX + 1u; r2 <= LC_FREQ * 2u / REF_MIN; r2++) {
        for (n2 = VCO_MIN * r2 / LC_FREQ + 1u; n2 <= VCO_MAX * r2 / LC_FREQ; n2++) {
            for (p = P_MIN; p <= P_MAX; p += P_INC) {
                wrpll_update_rnp(freq2k, budget, r2, n2, p, &best);
            }
        }
    }
    *n2_out = best.n2;
    *p_out = best.p;
    *r2_out = best.r2;
}

uint32_t intel_dpll_wrpll_freq(uint32_t wrpll)
{
    uint32_t refclk, n, p, r;

    switch (wrpll & WRPLL_REF_MASK) {
    case WRPLL_REF_SPECIAL_HSW:
    case WRPLL_REF_PCH_SSC:
        refclk = REFCLK_SSC;
        break;
    case WRPLL_REF_LCPLL:
        refclk = REFCLK_LCPLL;
        break;
    default:
        return 0;
    }
    r = wrpll & WRPLL_DIVIDER_REF_MASK;
    p = (wrpll & WRPLL_DIVIDER_POST_MASK) >> WRPLL_DIVIDER_POST_SHIFT;
    n = (wrpll & WRPLL_DIVIDER_FB_MASK) >> WRPLL_DIVIDER_FB_SHIFT;
    if (p == 0 || r == 0) {
        return 0;
    }
    /* P and R have a fixed point bit; the result is in kHz. */
    return (refclk * n / 10u) / (p * r) * 2u;
}

uint32_t intel_dpll_spll_freq(uint32_t spll)
{
    switch (spll & SPLL_FREQ_MASK) {
    case SPLL_FREQ_810MHz:
        return 81000u * 2u;
    case SPLL_FREQ_1350MHz:
        return 135000u * 2u;
    case SPLL_FREQ_2700MHz:
        return 270000u * 2u;
    default:
        return 0;
    }
}

uint32_t intel_dpll_spll_compute(uint32_t port_clock)
{
    if (port_clock / 2u != 135000u) {
        return 0;
    }
    return SPLL_PLL_ENABLE | SPLL_FREQ_1350MHz | SPLL_REF_MUXED_SSC;
}

/* ------------------------------------------------------------------------
 * The PLLs
 */

/** The LCPLL output of a link of `port_clock` kHz, Linux's
 * hsw_ddi_lcpll_get_dpll; DPLL_ID_NONE for a rate it does not give. */
static int lcpll_id(uint32_t port_clock)
{
    switch (port_clock / 2u) {
    case 81000u:
        return DPLL_ID_LCPLL_810;
    case 135000u:
        return DPLL_ID_LCPLL_1350;
    case 270000u:
        return DPLL_ID_LCPLL_2700;
    default:
        return DPLL_ID_NONE;
    }
}

/** Whether PLL `id` has a control register of its own, a WRPLL or the
 * SPLL. */
static int dpll_has_ctl(int id)
{
    return id == DPLL_ID_WRPLL1 || id == DPLL_ID_WRPLL2 || id == DPLL_ID_SPLL;
}

/** PLL `id`'s control register. */
static uint32_t dpll_ctl(int id)
{
    return id == DPLL_ID_SPLL ? SPLL_CTL : WRPLL_CTL(id);
}

uint32_t intel_dpll_port_clk_sel(int id)
{
    switch (id) {
    case DPLL_ID_WRPLL1:
        return PORT_CLK_SEL_WRPLL1;
    case DPLL_ID_WRPLL2:
        return PORT_CLK_SEL_WRPLL2;
    case DPLL_ID_SPLL:
        return PORT_CLK_SEL_SPLL;
    case DPLL_ID_LCPLL_810:
        return PORT_CLK_SEL_LCPLL_810;
    case DPLL_ID_LCPLL_1350:
        return PORT_CLK_SEL_LCPLL_1350;
    case DPLL_ID_LCPLL_2700:
        return PORT_CLK_SEL_LCPLL_2700;
    default:
        return PORT_CLK_SEL_NONE;
    }
}

void intel_dpll_init(struct drm_i915_private *i915)
{
    struct intel_dpll *pll;
    uint32_t v;
    int id;

    for (id = 0; id < INTEL_NUM_DPLLS; id++) {
        pll = &i915->display.dpll[id];
        k_memset(pll, 0, sizeof(*pll));
        pll->name = dpll_names[id];
        pll->id = id;
        if (dpll_has_ctl(id)) {
            v = i915_read(&i915->hw, dpll_ctl(id));
            pll->hw = v;
            pll->on = (v & WRPLL_PLL_ENABLE) != 0;
        } else {
            pll->on = 1;
        }
    }
}

void intel_dpll_plan_init(struct drm_i915_private *i915, uint32_t leaving,
                          struct intel_dpll_plan *plan)
{
    int id;

    for (id = 0; id < INTEL_NUM_DPLLS; id++) {
        plan->pipes[id] = i915->display.dpll[id].pipes & ~leaving;
        plan->hw[id] = i915->display.dpll[id].hw;
    }
}

/** Linux's intel_find_shared_dpll over the WRPLLs: one with pipes whose
 * value is `hw`, else the first without pipes; DPLL_ID_NONE for neither. */
static int find_wrpll(const struct intel_dpll_plan *plan, uint32_t hw)
{
    int id, unused = DPLL_ID_NONE;

    for (id = DPLL_ID_WRPLL1; id <= DPLL_ID_WRPLL2; id++) {
        if (plan->pipes[id] == 0) {
            if (unused == DPLL_ID_NONE) {
                unused = id;
            }
            continue;
        }
        if (plan->hw[id] == hw) {
            return id;
        }
    }
    return unused;
}

int32_t intel_dpll_compute(struct drm_i915_private *i915, struct intel_dpll_plan *plan, int pipe,
                           struct intel_crtc_config *cfg)
{
    uint32_t r2, n2, p, hw;
    int id;

    (void)i915;
    if (cfg->output == INTEL_OUTPUT_HDMI) {
        intel_dpll_calc_wrpll(cfg->port_clock * 1000u, &r2, &n2, &p);
        hw = WRPLL_PLL_ENABLE | WRPLL_REF_LCPLL | WRPLL_DIVIDER_REFERENCE(r2) |
             WRPLL_DIVIDER_FEEDBACK(n2) | WRPLL_DIVIDER_POST(p);
        id = find_wrpll(plan, hw);
        if (id == DPLL_ID_NONE) {
            klog(KLOG_DEBUG, "i915", "dpll: pipe %c: no WRPLL free for %u kHz", 'A' + pipe,
                 (unsigned)cfg->port_clock);
            return -EBUSY;
        }
        cfg->port_clock = intel_dpll_wrpll_freq(hw);
    } else {
        id = lcpll_id(cfg->port_clock);
        if (id == DPLL_ID_NONE) {
            klog(KLOG_DEBUG, "i915", "dpll: invalid clock for DP: %u", (unsigned)cfg->port_clock);
            return -EINVAL;
        }
        hw = 0;
    }
    plan->pipes[id] |= 1u << pipe;
    plan->hw[id] = hw;
    cfg->pll = id;
    cfg->pll_hw = hw;
    return 0;
}

void intel_dpll_enable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg)
{
    struct intel_dpll *pll;
    uint32_t ctl;

    if (cfg->pll < 0 || cfg->pll >= INTEL_NUM_DPLLS) {
        return;
    }
    pll = &i915->display.dpll[cfg->pll];
    pll->pipes |= 1u << pipe;
    if (pll->active == 0 && dpll_has_ctl(pll->id)) {
        if (pll->on) {
            klog(KLOG_WARN, "i915", "dpll: %s on with no pipe; set again", pll->name);
        }
        ctl = dpll_ctl(pll->id);
        pll->hw = cfg->pll_hw;
        i915_write(&i915->hw, ctl, pll->hw);
        i915_posting_read(&i915->hw, ctl);
        i915_delay_us(&i915->hw, PLL_LOCK_US);
        pll->on = 1;
    } else if (dpll_has_ctl(pll->id) && pll->hw != cfg->pll_hw) {
        klog(KLOG_WARN, "i915", "dpll: pipe %c shares %s at 0x%x, not 0x%x", 'A' + pipe, pll->name,
             (unsigned)pll->hw, (unsigned)cfg->pll_hw);
    }
    pll->active |= 1u << pipe;
    klog(KLOG_DEBUG, "i915", "dpll: enable %s for pipe %c (active 0x%x)", pll->name, 'A' + pipe,
         (unsigned)pll->active);
}

/** Turn PLL `pll` off: a WRPLL's or the SPLL's enable cleared, Linux's
 * hsw_ddi_wrpll_disable and hsw_ddi_spll_disable. */
static void dpll_off(struct drm_i915_private *i915, struct intel_dpll *pll)
{
    uint32_t ctl = dpll_ctl(pll->id);

    i915_rmw(&i915->hw, ctl, WRPLL_PLL_ENABLE, 0);
    i915_posting_read(&i915->hw, ctl);
    pll->on = 0;
}

void intel_dpll_disable(struct drm_i915_private *i915, int pipe,
                        const struct intel_crtc_config *cfg)
{
    struct intel_dpll *pll;

    if (cfg->pll < 0 || cfg->pll >= INTEL_NUM_DPLLS) {
        return;
    }
    pll = &i915->display.dpll[cfg->pll];
    pll->active &= ~(1u << pipe);
    pll->pipes &= ~(1u << pipe);
    klog(KLOG_DEBUG, "i915", "dpll: disable %s for pipe %c (active 0x%x)", pll->name, 'A' + pipe,
         (unsigned)pll->active);
    if (pll->active == 0 && dpll_has_ctl(pll->id) && pll->on) {
        dpll_off(i915, pll);
    }
}

int32_t intel_dpll_readout(struct drm_i915_private *i915, int pipe, uint32_t sel,
                           struct intel_crtc_config *cfg)
{
    struct intel_dpll *pll;
    int id;

    switch (sel & PORT_CLK_SEL_MASK) {
    case PORT_CLK_SEL_WRPLL1:
        id = DPLL_ID_WRPLL1;
        break;
    case PORT_CLK_SEL_WRPLL2:
        id = DPLL_ID_WRPLL2;
        break;
    case PORT_CLK_SEL_SPLL:
        id = DPLL_ID_SPLL;
        break;
    case PORT_CLK_SEL_LCPLL_810:
        id = DPLL_ID_LCPLL_810;
        break;
    case PORT_CLK_SEL_LCPLL_1350:
        id = DPLL_ID_LCPLL_1350;
        break;
    case PORT_CLK_SEL_LCPLL_2700:
        id = DPLL_ID_LCPLL_2700;
        break;
    default:
        return -EINVAL;
    }
    pll = &i915->display.dpll[id];
    if (!pll->on) {
        return -EINVAL;
    }
    cfg->pll = id;
    cfg->pll_hw = dpll_has_ctl(id) ? pll->hw : 0;
    cfg->port_clock = id == DPLL_ID_SPLL ? intel_dpll_spll_freq(pll->hw)
                      : id == DPLL_ID_WRPLL1 || id == DPLL_ID_WRPLL2
                          ? intel_dpll_wrpll_freq(pll->hw)
                      : id == DPLL_ID_LCPLL_810  ? 81000u * 2u
                      : id == DPLL_ID_LCPLL_1350 ? 135000u * 2u
                                                 : 270000u * 2u;
    pll->pipes |= 1u << pipe;
    pll->active |= 1u << pipe;
    return 0;
}

void intel_dpll_sanitize(struct drm_i915_private *i915)
{
    struct intel_dpll *pll;
    int id;

    for (id = 0; id < INTEL_NUM_DPLLS; id++) {
        pll = &i915->display.dpll[id];
        if (dpll_has_ctl(id) && pll->on && pll->active == 0) {
            klog(KLOG_INFO, "i915", "dpll: %s enabled but not in use, disabling", pll->name);
            dpll_off(i915, pll);
        }
    }
}
