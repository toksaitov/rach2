/*
 * i915_model_pc8.h: the state of the host model's LCPLL and sideband
 * blocks (i915_model_pc8.c), the parts of the HD 4400 and its PCH that the
 * Intel driver's package C8 entry and exit drive, which a test reads, and
 * sets to make the sideband or the PLL hang.
 */
#ifndef _I915_MODEL_PC8_H_
#define _I915_MODEL_PC8_H_

#include "i915_model.h"

/* The sideband transfers the model keeps; later ones are counted alone. */
#define I915_MODEL_SBI_LOG 32

/* One transfer of the sideband: the clock unit's register, whether it was
 * a write, and the value written or read. */
struct i915_model_sbi {
    uint32_t reg;
    int write;
    uint32_t value;
};

/*
 * LCPLL_CTL keeps what the driver writes but its two status bits:
 * LCPLL_PLL_LOCK reads set while LCPLL_PLL_DISABLE is clear, and
 * LCPLL_CD_SOURCE_FCLK_DONE while LCPLL_CD_SOURCE_FCLK is set, at once,
 * unless `lcpll_stuck`, which holds both as they were.
 *
 * The sideband (SBI_ADDR, SBI_DATA, SBI_CTL_STAT) reaches the clock
 * unit's SBI_SSCCTL, SBI_GEN0 and SBI_DBUFF0, `iclk[]` in that order, as
 * the firmware leaves them: spread spectrum on its normal path, and both
 * CLKOUT_DP buffers enabled, SBI_GEN0_CFG_BUFFENABLE_DISABLE set, as
 * Linux's lpt_enable_clkout_dp sets it to enable them. A transfer started
 * with SBI_BUSY ends at once, unless `sbi_stuck`, which leaves it busy;
 * one of another register of the unit, or of another unit, ends with
 * SBI_RESPONSE_FAIL. `log` holds the first transfers in order, `nlog` of
 * them, and `transfers` counts them all.
 *
 * D_COMP_HSW is the register file's, written by the pcode's
 * GEN6_PCODE_WRITE_D_COMP (i915_model_rc6.c).
 */
struct i915_model_pc8 {
    int lcpll_stuck;
    int sbi_stuck;
    uint32_t iclk[3];
    struct i915_model_sbi log[I915_MODEL_SBI_LOG];
    uint32_t nlog, transfers;
};

/** The LCPLL and sideband blocks' state of `m`. */
struct i915_model_pc8 *i915_model_pc8(struct i915_model *m);

#endif /* _I915_MODEL_PC8_H_ */
