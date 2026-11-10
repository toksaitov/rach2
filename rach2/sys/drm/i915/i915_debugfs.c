/*
 * i915_debugfs.c: the Intel graphics driver's files in debugfs that IGT's
 * tests open, and the error state's file in /sys, in Linux 6.18's formats
 * from i915_debugfs.c, i915_gpu_error.c and display/intel_cdclk.c.
 */

#include <stdarg.h>

#include "i915.h"
#include "i915_reg.h"

#include "../../arch/arch.h"
#include "../../fs/debugfs.h"
#include "../../iommu.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/usercopy.h"
#include "../../rtc.h"
#include "../../syscall.h"

/* Linux's DROP_ bits of i915_gem_drop_caches, all of which it takes. */
#define DROP_UNBOUND      0x001u
#define DROP_BOUND        0x002u
#define DROP_RETIRE       0x004u
#define DROP_ACTIVE       0x008u
#define DROP_FREED        0x010u
#define DROP_SHRINK_ALL   0x020u
#define DROP_IDLE         0x040u
#define DROP_RESET_ACTIVE 0x080u
#define DROP_RESET_SEQNO  0x100u
#define DROP_RCU          0x200u
#define DROP_ALL          0x3ffu

/* The command streamer's timestamp on graphics 7: 12.5 MHz, 80 ns. */
#define CS_TIMESTAMP_HZ 12500000u
#define CS_TIMESTAMP_NS 80u

/* The frequency's unit, as i915_rps.c's. */
#define FREQ_MHZ 50

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

/* How long DROP_RESET_ACTIVE waits for the engines to idle before it
 * wedges the GPU, as Linux's gt_drop_caches. */
#define DROP_IDLE_WAIT_MS 200u

/* ------------------------------------------------------------------------
 * Text
 */

/* Text made into `cap` bytes at `buf`, `len` bytes of it so far, counted
 * past `cap` so that a show returns the length it needs. */
struct emit {
    char *buf;
    uint32_t cap;
    uint32_t len;
};

/** Append the byte `c` to `e`, as far as its room goes. */
static void emit_c(struct emit *e, char c)
{
    if (e->len < e->cap) {
        e->buf[e->len] = c;
    }
    e->len++;
}

/** Append the string `s`. */
static void emit_s(struct emit *e, const char *s)
{
    while (*s) {
        emit_c(e, *s++);
    }
}

/** Append `v` in base `radix`, 10 or 16, in at least `width` columns,
 * padded with zeros when `zero`, with a minus sign when `neg`. */
static void emit_n(struct emit *e, uint64_t v, uint32_t radix, int neg, uint32_t width, int zero)
{
    char digits[24];
    uint32_t n = 0, rem, k;

    do {
        v = tick_div(v, radix, &rem);
        digits[n++] = "0123456789abcdef"[rem];
    } while (v);
    k = n + (neg ? 1u : 0u);
    for (; !zero && k < width; k++) {
        emit_c(e, ' ');
    }
    if (neg) {
        emit_c(e, '-');
    }
    for (; zero && k < width; k++) {
        emit_c(e, '0');
    }
    while (n > 0) {
        emit_c(e, digits[--n]);
    }
}

/**
 * Append `fmt` with its arguments: `%s`, `%c`, `%d`, `%u` and `%x`, each
 * with an optional `0` flag and a width, and an `L` before a number's
 * letter for a 64-bit argument; `%%` for a percent sign.
 */
static void emitf(struct emit *e, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        uint32_t width = 0;
        int zero = 0, wide = 0;
        uint64_t v;
        int64_t sv;

        if (*fmt != '%') {
            emit_c(e, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '0') {
            zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10u + (uint32_t)(*fmt++ - '0');
        }
        if (*fmt == 'L') {
            wide = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's':
            emit_s(e, va_arg(ap, const char *));
            break;
        case 'c':
            emit_c(e, (char)va_arg(ap, int));
            break;
        case 'd':
            sv = wide ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            emit_n(e, sv < 0 ? (uint64_t)-sv : (uint64_t)sv, 10, sv < 0, width, zero);
            break;
        case 'u':
        case 'x':
            v = wide ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            emit_n(e, v, *fmt == 'x' ? 16u : 10u, 0, width, zero);
            break;
        case '%':
            emit_c(e, '%');
            break;
        default:
            va_end(ap);
            return;
        }
    }
    va_end(ap);
}

/** The length `e` needs, as a show returns it, with the text ended when
 * it fit. */
static int32_t emit_end(struct emit *e)
{
    if (e->len < e->cap) {
        e->buf[e->len] = '\0';
    }
    return (int32_t)e->len;
}

/** Append each of the `n` strings at `lines` as a line. */
static void emit_lines(struct emit *e, const char *const *lines, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        emit_s(e, lines[i]);
        emit_c(e, '\n');
    }
}

/** "yes" or "no", Linux's str_yes_no. */
static const char *yes_no(int v)
{
    return v ? "yes" : "no";
}

/**
 * Append `len` bytes of the ring of `size` dwords at `p`, a power of two,
 * from dword `start`, as Linux's engine hexdump does: rows of eight
 * dwords, "[OFFSET] W0 W1 ...", and a row that repeats the one before as
 * one "*" for the run.
 */
static void hexdump_ring(struct emit *e, const uint32_t *p, uint32_t size, uint32_t start,
                         uint32_t len)
{
    uint32_t pos, i, words = len / 4u, mask = size - 1u;
    int skip = 0;

    for (pos = 0; pos < words; pos += 8u) {
        uint32_t n = words - pos < 8u ? words - pos : 8u;

        if (pos > 0 && n == 8u) {
            for (i = 0; i < 8u; i++) {
                if (p[(start + pos + i) & mask] != p[(start + pos - 8u + i) & mask]) {
                    break;
                }
            }
            if (i == 8u) {
                if (!skip) {
                    emit_s(e, "*\n");
                    skip = 1;
                }
                continue;
            }
        }
        emitf(e, "[%04x]", pos * 4u);
        for (i = 0; i < n; i++) {
            emitf(e, " %08x", p[(start + pos + i) & mask]);
        }
        emit_c(e, '\n');
        skip = 0;
    }
}

/** Append the `words` dwords at `p` as Linux's error state does an
 * object's pages without compression: "~", each dword in ASCII85, "z" for
 * a zero, and a newline. */
static void ascii85(struct emit *e, const uint32_t *p, uint32_t words)
{
    uint32_t i, rem;
    char out[5];
    int k;

    emit_c(e, '~');
    for (i = 0; i < words; i++) {
        uint64_t v = p[i];

        if (v == 0) {
            emit_c(e, 'z');
            continue;
        }
        for (k = 5; k--;) {
            v = tick_div(v, 85, &rem);
            out[k] = (char)('!' + rem);
        }
        for (k = 0; k < 5; k++) {
            emit_c(e, out[k]);
        }
    }
    emit_c(e, '\n');
}

/** Parse the `len` bytes at `s` as Linux's kstrtoull with base 0: digits
 * in decimal, octal after "0" or hex after "0x", a "+" before them and one
 * newline after allowed. 0, -EINVAL, or -ERANGE past 64 bits. */
static int32_t parse_u64(const char *s, uint32_t len, uint64_t *out)
{
    uint32_t i = 0, radix = 10, digits = 0;
    uint64_t v = 0;

    if (len > 0 && s[len - 1] == '\n') {
        len--;
    }
    if (i < len && s[i] == '+') {
        i++;
    }
    if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        radix = 16;
        i += 2;
    } else if (i < len && s[i] == '0') {
        radix = 8;
    }
    for (; i < len; i++, digits++) {
        char c = s[i];
        uint32_t d;

        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return -EINVAL;
        }
        if (d >= radix) {
            return -EINVAL;
        }
        if (v > tick_div(~(uint64_t)0 - d, radix, (uint32_t *)0)) {
            return -ERANGE;
        }
        v = v * radix + d;
    }
    if (digits == 0) {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

/** Copy a write of `len` bytes from the user's `buf` into `out`, `cap`
 * bytes, as Linux's simple_attr_write does into its buffer: the first
 * `cap` - 1 bytes and a NUL. The bytes copied, or -EFAULT. */
static int32_t copy_write(const char __user *buf, uint32_t len, char *out, uint32_t cap)
{
    uint32_t n = len < cap - 1u ? len : cap - 1u;

    if (copy_from_user(out, buf, n) != 0) {
        return -EFAULT;
    }
    out[n] = '\0';
    return (int32_t)n;
}

/* ------------------------------------------------------------------------
 * What several files print
 */

/** The ticks `t` as milliseconds. */
static uint64_t ticks_ms(uint64_t t)
{
    return t * (1000u / TICK_HZ);
}

/** Whether VT-d translates for the device, Linux's i915_vtd_active, as
 * the stolen memory's setup asks it; only x86-64's kernel reads the DMAR
 * table. */
static int vtd_active(struct drm_i915_private *i915)
{
#if defined(__x86_64__)
    return iommu_present() &&
           iommu_unit_of(i915->pci.bus, (uint8_t)(i915->pci.device << 3 | i915->pci.function)) >= 0;
#else
    (void)i915;
    return 0;
#endif
}

/** The engines the driver runs, as Linux's engine mask: RCS0 at bit 0
 * and BCS0 at bit 1. */
static uint32_t engine_mask(struct drm_i915_private *i915)
{
    uint32_t mask = 0;
    int i;

    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (i915->gt.engine[i].running) {
            mask |= 1u << i;
        }
    }
    return mask;
}

/** Linux's intel_device_info_print for Haswell GT2, from the platform's
 * features, and the flags hsw_gt2_info leaves at its own values. */
static void device_info(struct emit *e, struct drm_i915_private *i915)
{
    const struct i915_platform *p = i915->id->platform;

    emitf(e, "graphics version: %u\n", p->graphics_ver);
    emitf(e, "media version: %u\n", p->graphics_ver);
    emit_s(e, "graphics stepping: **\nmedia stepping: **\n");
    emitf(e, "gt: %u\n", p->gt);
    emit_s(e, "memory-regions: 0x21\npage-sizes: 0x1000\n");
    emitf(e, "platform: %s\n", p->name);
    emitf(e, "ppgtt-size: %u\nppgtt-type: 1\n", p->ppgtt_size);
    emitf(e, "dma_mask_size: %u\n", p->dma_mask_size);
    emit_s(e, "is_mobile: no\nrequire_force_probe: no\nis_dgfx: no\nhas_64bit_reloc: no\n"
              "has_64k_pages: no\ngpu_reset_clobbers_display: no\n");
    emitf(e, "has_reset_engine: %s\n", yes_no(p->has_reset_engine));
    emit_s(e, "has_3d_pipeline: yes\nhas_flat_ccs: no\nhas_global_mocs: no\nhas_gmd_id: no\n"
              "has_gt_uc: no\nhas_heci_pxp: no\nhas_heci_gscfi: no\nhas_guc_deprivilege: no\n"
              "has_guc_tlb_invalidation: no\nhas_l3_ccs_read: no\n");
    emitf(e, "has_l3_dpf: %s\n", yes_no(p->has_l3_dpf));
    emitf(e, "has_llc: %s\n", yes_no(p->has_llc));
    emit_s(e, "has_logical_ring_contexts: no\nhas_logical_ring_elsq: no\n"
              "has_media_ratio_mode: no\nhas_mslice_steering: no\nhas_oa_bpc_reporting: no\n"
              "has_oa_slice_contrib_limits: no\nhas_oam: no\nhas_one_eu_per_fuse_bit: no\n"
              "has_pxp: no\n");
    emitf(e, "has_rc6: %s\n", yes_no(p->has_rc6));
    emitf(e, "has_rc6p: %s\n", yes_no(p->has_rc6p));
    emit_s(e, "has_rps: yes\nhas_runtime_pm: yes\nhas_snoop: no\nhas_coherent_ggtt: yes\n"
              "tuning_thread_rr_after_dep: no\nunfenced_needs_alignment: no\n"
              "hws_needs_physical: no\nhas_pooled_eu: no\n");
}

/** Linux's intel_gt_info_print with intel_sseu_dump: the engines, then
 * the slices, subslices and EUs; with `topology`, intel_sseu_print_topology
 * after them, as the error state has it. */
static void gt_info(struct emit *e, struct drm_i915_private *i915, int topology)
{
    struct i915_sseu *s = &i915->sseu;
    uint32_t ss = 0, i;

    i915_sseu_init(i915);
    for (i = 0; i < 8u; i++) {
        ss += (s->subslice_mask >> i) & 1u;
    }
    emitf(e, "available engines: %x\n", engine_mask(i915));
    emitf(e, "slice total: %u, mask=%04x\n", s->slice_mask & 1u, s->slice_mask);
    emitf(e, "subslice total: %u\n", ss);
    emitf(e, "slice0: %u subslices, mask=%08x\n", ss, s->subslice_mask);
    emitf(e, "EU total: %u\nEU per subslice: %u\n", s->eu_total, s->eu_per_subslice);
    emit_s(e, "has slice power gating: no\nhas subslice power gating: no\n"
              "has EU power gating: no\n");
    if (!topology) {
        return;
    }
    emitf(e, "slice0: %u subslice(s) (0x%08x):\n", ss, s->subslice_mask);
    for (i = 0; i < 8u; i++) {
        if ((s->subslice_mask >> i) & 1u) {
            emitf(e, "\tsubslice%u: %u EUs (0x%x)\n", i, s->eu_per_subslice,
                  (1u << s->eu_per_subslice) - 1u);
        }
    }
}

/** Linux's intel_driver_caps_print: Haswell's ring contexts, and no
 * scheduler on its rings. */
static void driver_caps(struct emit *e)
{
    emit_s(e, "Has logical contexts? yes\nscheduler: 0x0\n");
}

/* Linux's i915_params_dump: the module's parameters at their defaults,
 * which the driver follows and has no way to change. */
static const char *const i915_params[] = {"i915.modeset=-1",
                                          "i915.enable_guc=0",
                                          "i915.guc_log_level=-1",
                                          "i915.guc_firmware_path=(null)",
                                          "i915.huc_firmware_path=(null)",
                                          "i915.gsc_firmware_path=(null)",
                                          "i915.memtest=no",
                                          "i915.mmio_debug=0",
                                          "i915.reset=3",
                                          "i915.inject_probe_failure=0",
                                          "i915.force_probe=",
                                          "i915.request_timeout_ms=20000",
                                          "i915.lmem_size=0",
                                          "i915.lmem_bar_size=0",
                                          "i915.enable_hangcheck=yes",
                                          "i915.error_capture=yes",
                                          "i915.enable_gvt=no",
                                          "i915.enable_debug_only_api=no"};

/* Linux's intel_display_device_info_print for Haswell's display, before
 * its raw clock. */
static const char *const display_info[] = {"display version: 7",
                                           "display stepping: **",
                                           "cursor_needs_physical: no",
                                           "has_cdclk_crawl: no",
                                           "has_cdclk_squash: no",
                                           "has_ddi: yes",
                                           "has_dp_mst: yes",
                                           "has_dsb: no",
                                           "has_fpga_dbg: yes",
                                           "has_gmch: no",
                                           "has_hotplug: yes",
                                           "has_hti: no",
                                           "has_ipc: no",
                                           "has_overlay: no",
                                           "has_psr: yes",
                                           "has_psr_hw_tracking: yes",
                                           "overlay_needs_physical: no",
                                           "supports_tv: no",
                                           "has_hdcp: no",
                                           "has_dmc: no",
                                           "has_dsc: no"};

/* Linux's intel_display_params_dump at the defaults, as `i915_params`. */
static const char *const display_params[] = {"i915.dmc_firmware_path=",
                                             "i915.vbt_firmware=",
                                             "i915.lvds_channel_mode=0",
                                             "i915.panel_use_ssc=-1",
                                             "i915.vbt_sdvo_panel_type=-1",
                                             "i915.enable_dc=-1",
                                             "i915.enable_dpt=yes",
                                             "i915.enable_dsb=yes",
                                             "i915.enable_flipq=no",
                                             "i915.enable_sagv=yes",
                                             "i915.disable_power_well=-1",
                                             "i915.enable_ips=yes",
                                             "i915.invert_brightness=0",
                                             "i915.edp_vswing=0",
                                             "i915.enable_dpcd_backlight=-1",
                                             "i915.load_detect_test=no",
                                             "i915.force_reset_modeset_test=no",
                                             "i915.disable_display=no",
                                             "i915.verbose_state_checks=yes",
                                             "i915.nuclear_pageflip=no",
                                             "i915.enable_dp_mst=yes",
                                             "i915.enable_fbc=-1",
                                             "i915.enable_psr=-1",
                                             "i915.enable_panel_replay=-1",
                                             "i915.psr_safest_params=no",
                                             "i915.enable_psr2_sel_fetch=yes",
                                             "i915.enable_dmc_wl=-1"};

/** The `cap` bytes at `buf` hold no text when the device is gone, since
 * its registers are; -ENODEV then, 0 otherwise. */
static int32_t gone(struct drm_i915_private *i915)
{
    return i915->drm.unplugged ? -ENODEV : 0;
}

/* ------------------------------------------------------------------------
 * The error state
 */

/** Append "SECONDS s MICROS us" for `us` microseconds. */
static void emit_time(struct emit *e, const char *name, uint64_t us)
{
    uint32_t rem_lo, rem_hi;
    uint64_t sec = tick_div(tick_div(us, 1000u, &rem_lo), 1000u, &rem_hi);

    emitf(e, "%s: %Lu s %u us\n", name, sec, rem_hi * 1000u + rem_lo);
}

/**
 * Append engine `i`'s part of the state `err`, as Linux's error_print_engine
 * and err_print_gt_engines: its registers, its hang and resets, the active
 * request's context, and its status page and ring as Linux's "HW Status"
 * and "ring" objects.
 */
static void error_engine(struct emit *e, struct drm_i915_private *i915,
                         const struct i915_error_engine *ee, int i)
{
    const char *name = i915->gt.engine[i].name;
    uint32_t n;

    emitf(e, "%s command stream:\n", name);
    emitf(e, "  CCID:  0x%08x\n  START: 0x%08x\n", ee->ccid, ee->start);
    emitf(e, "  HEAD:  0x%08x [0x%08x]\n", ee->head, ee->rq_head);
    emitf(e, "  TAIL:  0x%08x [0x%08x, 0x%08x]\n", ee->tail, ee->rq_post, ee->rq_tail);
    emitf(e, "  CTL:   0x%08x\n  MODE:  0x%08x\n  HWS:   0x%08x\n", ee->ctl, ee->mode, ee->hws_pga);
    emitf(e, "  ACTHD: 0x%08x %08x\n", 0u, ee->acthd);
    emitf(e, "  IPEIR: 0x%08x\n  IPEHR: 0x%08x\n  ESR:   0x%08x\n", ee->ipeir, ee->ipehr, ee->esr);
    emitf(e, "  INSTDONE: 0x%08x\n", ee->instdone);
    if (i == I915_RCS0) {
        emitf(e, "  SC_INSTDONE: 0x%08x\n", ee->sc_instdone);
        for (n = 0; n < I915_ERROR_SUBSLICES; n++) {
            emitf(e, "  SAMPLER_INSTDONE[0][%u]: 0x%08x\n", n, ee->sampler_instdone[n]);
        }
        for (n = 0; n < I915_ERROR_SUBSLICES; n++) {
            emitf(e, "  ROW_INSTDONE[0][%u]: 0x%08x\n", n, ee->row_instdone[n]);
        }
    }
    emitf(e, "  BBADDR: 0x%08x_%08x\n  BB_STATE: 0x%08x\n", 0u, ee->bbaddr, ee->bbstate);
    emitf(e, "  INSTPS: 0x%08x\n  INSTPM: 0x%08x\n", ee->instps, ee->instpm);
    emitf(e, "  FADDR: 0x%08x %08x\n", 0u, ee->faddr);
    emitf(e, "  RC PSMI: 0x%08x\n  FAULT_REG: 0x%08x\n", ee->rc_psmi, ee->fault_reg);
    emitf(e, "  GFX_MODE: 0x%08x\n  PP_DIR_BASE: 0x%08x\n", ee->gfx_mode, ee->pp_dir_base);
    emitf(e, "  hung: %u\n  engine reset count: %u\n", ee->hung, ee->reset_count);
    emitf(e, "  Active context: %s[%u] prio 0, guilty %u active %u, runtime total 0ns, avg 0ns\n",
          ee->comm, ee->pid, ee->guilty, ee->active);
    emitf(e, "  context timeline seqno %u\n", ee->hws_seqno);
    emitf(e, "%s --- HW Status = 0x%08x %08x\n", name, 0u, ee->hws_ggtt);
    ascii85(e, ee->hws, sizeof(ee->hws) / 4u);
    emitf(e, "%s --- ring = 0x%08x %08x\n", name, 0u, ee->ring_ggtt);
    ascii85(e, ee->ring, sizeof(ee->ring) / 4u);
}

/**
 * Write the state `err` as Linux's __err_print_to_sgl does for Haswell's
 * rings: its message when it is kept, the times, the processes of the
 * engines that had work on the chip, the GT's registers and fences, those
 * engines' parts, and the device's information and parameters. Its
 * wall-clock time is the clock's now less the state's age.
 */
static void error_text(struct emit *e, struct drm_i915_private *i915,
                       const struct i915_gpu_error *err)
{
    uint64_t now = rtc_ticks_now(), age = ticks_ms(now - err->ticks);
    int64_t sec, nsec;
    uint32_t sub;
    int i;

    rtc_realtime(&sec, &nsec);
    if (err->msg[0]) {
        emitf(e, "%s\n", err->msg);
    }
    emitf(e, "Kernel: %s %s\n", UTS_RELEASE, ARCH_NAME);
    emit_time(e, "Time",
              (uint64_t)sec * 1000000u + tick_div((uint64_t)nsec, 1000u, &sub) - age * 1000u);
    emit_time(e, "Boottime", ticks_ms(err->ticks) * 1000u);
    emit_time(e, "Uptime", ticks_ms(err->ticks - i915->gt.init_ticks) * 1000u);
    emitf(e, "Capture: %Lu jiffies; %Lu ms ago\n", err->ticks, age);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (err->engine[i].nrequests) {
            emitf(e, "Active process (on ring %s): %s [%u]\n", i915->gt.engine[i].name,
                  err->engine[i].comm, err->engine[i].pid);
        }
    }
    emitf(e, "Reset count: %u\nSuspend count: 0\n", err->reset_count);
    emitf(e, "Platform: %s\n", i915->id->platform->name);
    sub = (i915->id->ult ? 1u : 0u) | (i915->id->ulx ? 2u : 0u);
    emitf(e, "Subplatform: 0x%x\n", sub);
    emitf(e, "PCI ID: 0x%04x\nPCI Revision: 0x%02x\n", i915->pci.device_id, i915->revision);
    emitf(e, "PCI Subsystem: %04x:%04x\n", i915->subsystem_vendor, i915->subsystem_device);
    emitf(e, "IOMMU enabled?: %d\n", vtd_active(i915));
    emit_s(e, "RPM wakelock: yes\nPM suspended: no\n");
    emitf(e, "GT awake: %s\n", yes_no(err->awake));
    emitf(e, "CS timestamp frequency: %u Hz, %u ns\n", CS_TIMESTAMP_HZ, CS_TIMESTAMP_NS);
    emitf(e, "EIR: 0x%08x\nPGTBL_ER: 0x%08x\nGTIER[0]: 0x%08x\n", err->eir, err->pgtbl_er,
          err->gtier);
    for (i = 0; i < I915_NUM_FENCES; i++) {
        emitf(e, "  fence[%d] = %08Lx\n", i, err->fence[i]);
    }
    emitf(e, "FORCEWAKE: 0x%08x\nERROR: 0x%08x\nDONE_REG: 0x%08x\nERR_INT: 0x%08x\n",
          err->forcewake, err->error_gen6, err->done_reg, err->err_int);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (err->engine[i].nrequests) {
            error_engine(e, i915, &err->engine[i], i);
        }
    }
    gt_info(e, i915, 1);
    device_info(e, i915);
    driver_caps(e);
    emit_lines(e, i915_params, NELEM(i915_params));
    emit_lines(e, display_info, NELEM(display_info));
    emitf(e, "rawclk rate: %u kHz\n",
          (i915_read(&i915->hw, PCH_RAWCLK_FREQ) & RAWCLK_FREQ_MASK) * 1000u);
    emit_lines(e, display_params, NELEM(display_params));
    emitf(e, "DERRMR: 0x%08x\n", err->derrmr);
}

/* What a descriptor of i915_error_state or i915_gpu_info reads: the text
 * made at its open, `len` bytes at `text`, NULL for none, and the device,
 * held until the close. */
struct state_text {
    struct drm_i915_private *i915;
    uint32_t len;
    char *text;
};

/** Make `err`'s text into `t`. 0, or -ENOMEM. */
static int32_t state_text_make(struct state_text *t, const struct i915_gpu_error *err)
{
    struct emit e = {0, 0, 0};

    error_text(&e, t->i915, err);
    t->text = (char *)kmalloc(e.len + 1u);
    if (!t->text) {
        return -ENOMEM;
    }
    e.buf = t->text;
    e.cap = e.len + 1u;
    e.len = 0;
    error_text(&e, t->i915, err);
    t->len = e.len < e.cap ? e.len : e.cap - 1u;
    return 0;
}

/** Read the text made at the open, from the description's offset. */
static int32_t state_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct state_text *t = (struct state_text *)e->priv;
    uint64_t off = e->ofd->offset;
    uint32_t n;

    if (off >= t->len) {
        return 0;
    }
    n = t->len - (uint32_t)off;
    n = n < count ? n : count;
    if (copy_to_user(buf, t->text + off, n) != 0) {
        return -EFAULT;
    }
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/**
 * A write of i915_error_state drops the device's error state, as Linux's
 * i915_error_state_write. Linux's returns 0 when the open found none,
 * which a writer's loop takes for no progress; this one takes the bytes
 * then too (dev-log/78-intel/upstream.md).
 */
static int32_t error_state_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct state_text *t = (struct state_text *)e->priv;

    (void)buf;
    i915_gpu_error_clear(t->i915);
    return (int32_t)count;
}

/** Move the description's offset in the text, as default_llseek. */
static int64_t state_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    struct state_text *t = (struct state_text *)e->priv;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)e->ofd->offset : (int64_t)t->len;

    if (whence < 0 || whence > 2 || base + offset < 0) {
        return -EINVAL;
    }
    return base + offset;
}

/** Free the text and let the device go at the last close. */
static void state_close(fd_entry_t *e)
{
    struct state_text *t = (struct state_text *)e->priv;

    if (t) {
        drm_dev_put(&t->i915->drm);
        kfree(t->text);
        kfree(t);
    }
    e->priv = NULL;
}

static const struct file_ops error_state_ops = {
    state_read, error_state_write, NULL, NULL, state_close, NULL, state_llseek};

static const struct file_ops gpu_info_ops = {state_read,  NULL, NULL,        NULL,
                                             state_close, NULL, state_llseek};

/**
 * Open i915_error_state or, with `fresh`, i915_gpu_info: the text of the
 * state the device keeps, none when it keeps none, as Linux's
 * i915_error_state_open; or of a state captured now of every engine, as
 * its i915_gpu_info_open. The device is held until the close.
 */
static int32_t state_open(fd_entry_t *e, int fresh)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)e->priv;
    struct i915_gpu_error *err = i915->gt.error;
    struct state_text *t;
    int32_t rc = 0;

    if (fresh && gone(i915)) {
        return -ENODEV;
    }
    t = (struct state_text *)kzalloc(sizeof(*t));
    if (!t) {
        return -ENOMEM;
    }
    t->i915 = i915;
    if (fresh) {
        err = i915_gpu_coredump(i915, (1u << I915_NUM_ENGINES) - 1u, 0);
        rc = err ? state_text_make(t, err) : -ENOMEM;
        kfree(err);
    } else if (err) {
        rc = state_text_make(t, err);
    }
    if (rc != 0) {
        kfree(t);
        return rc;
    }
    drm_dev_get(&i915->drm);
    e->priv = t;
    return 0;
}

/** The open of i915_error_state. */
static int32_t error_state_open(fd_entry_t *e, int flags)
{
    (void)flags;
    return state_open(e, 0);
}

/** The open of i915_gpu_info. */
static int32_t gpu_info_open(fd_entry_t *e, int flags)
{
    (void)flags;
    return state_open(e, 1);
}

/** The card's `error` in /sys, Linux's error_state_read: the kept state's
 * text, or "No error state collected". */
static int32_t error_show(void *obj, int arg, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)obj;
    struct emit e;

    (void)arg;
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (i915->gt.error) {
        error_text(&e, i915, i915->gt.error);
    } else {
        emit_s(&e, "No error state collected\n");
    }
    return emit_end(&e);
}

/** A write of the card's `error` drops the kept state, as Linux's
 * error_state_write. */
static int32_t error_store(void *obj, int arg, const char *buf, uint32_t len)
{
    (void)arg;
    (void)buf;
    i915_gpu_error_clear((struct drm_i915_private *)obj);
    return (int32_t)len;
}

static const struct sysfs_attr error_attr = {"error", 0600u, error_show, error_store};

/* ------------------------------------------------------------------------
 * The text files
 */

/** i915_capabilities, Linux's i915_capabilities: the device's features,
 * whether VT-d translates for it, its engines and EUs, the driver's
 * capabilities and parameters. */
static int32_t capabilities_show(void *data, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (gone(i915)) {
        return -ENODEV;
    }
    device_info(&e, i915);
    emitf(&e, "iommu: %s\n", vtd_active(i915) ? "enabled" : "disabled");
    gt_info(&e, i915, 0);
    driver_caps(&e);
    emit_lines(&e, i915_params, NELEM(i915_params));
    return emit_end(&e);
}

/**
 * i915_gem_objects, Linux's i915_gem_object_info: the objects whose pages
 * a shrinker could take, those with pages but imports, and their bytes,
 * none waiting to be freed since an object goes with its last reference;
 * then the memory regions, the system's memory and the stolen memory.
 * IGT reads the first number (igt_get_stable_obj_count).
 */
static int32_t gem_objects_show(void *data, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    struct drm_gem_object *base;
    uint64_t bytes = 0;
    uint32_t count = 0;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    for (base = i915->drm.objects; base; base = base->dev_next) {
        struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)base;

        if (base->funcs == &i915_gem_object_funcs && !base->import &&
            i915_gem_object_has_pages(obj)) {
            count++;
            bytes += base->size;
        }
    }
    emitf(&e, "%u shrinkable [0 free] objects, %Lu bytes\n", count, bytes);
    emitf(&e, "system: total:0x%016Lx bytes\n", (uint64_t)frame_total_count() << PAGE_SHIFT);
    if (i915->stolen.size) {
        emitf(&e, "stolen-system: total:0x%016Lx bytes\n", i915->stolen.size);
    }
    return emit_end(&e);
}

/** Append "NAME: COUNT (NSns)" for an RPS counter in its units of 16 CS
 * timestamps, Linux's intel_gt_pm_interval_to_ns. */
static void rp_counter(struct emit *e, const char *name, uint32_t count)
{
    emitf(e, "%s: %u (%Luns)\n", name, count, (uint64_t)count * 16u * CS_TIMESTAMP_NS);
}

/**
 * i915_frequency_info, Linux's rps_frequency_dump on Haswell: RPS's
 * control and interrupt registers, the requested and actual frequency,
 * its evaluation counters with their thresholds, and the limits and state
 * of the driver's RPS.
 */
static int32_t frequency_show(void *data, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    struct i915_rps *rps = &i915->rps;
    uint32_t limits, perf, reqf, ctl, inc, dec, stat, cagf;
    uint32_t curupei, curup, prevup, curdownei, curdown, prevdown, upei, upt, downei, downt;
    uint32_t ier, imr, isr, iir, mask;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (gone(i915)) {
        return -ENODEV;
    }
    limits = i915_uncore_read(i915, GEN6_RP_STATE_LIMITS);
    perf = i915_uncore_read(i915, GEN6_GT_PERF_STATUS);
    if (i915_forcewake_get(i915) < 0) {
        return -EIO;
    }
    reqf = ((i915_uncore_read(i915, GEN6_RPNSWREQ) & ~GEN6_TURBO_DISABLE) >> HSW_FREQUENCY_SHIFT) *
           FREQ_MHZ;
    ctl = i915_uncore_read(i915, GEN6_RP_CONTROL);
    inc = i915_uncore_read(i915, GEN6_RP_UP_THRESHOLD);
    dec = i915_uncore_read(i915, GEN6_RP_DOWN_THRESHOLD);
    stat = i915_uncore_read(i915, GEN6_RPSTAT1);
    curupei = i915_uncore_read(i915, GEN6_RP_CUR_UP_EI) & GEN6_CURICONT_MASK;
    curup = i915_uncore_read(i915, GEN6_RP_CUR_UP) & GEN6_CURICONT_MASK;
    prevup = i915_uncore_read(i915, GEN6_RP_PREV_UP) & GEN6_CURICONT_MASK;
    curdownei = i915_uncore_read(i915, GEN6_RP_CUR_DOWN_EI) & GEN6_CURICONT_MASK;
    curdown = i915_uncore_read(i915, GEN6_RP_CUR_DOWN) & GEN6_CURICONT_MASK;
    prevdown = i915_uncore_read(i915, GEN6_RP_PREV_DOWN) & GEN6_CURICONT_MASK;
    upei = i915_uncore_read(i915, GEN6_RP_UP_EI);
    upt = i915_uncore_read(i915, GEN6_RP_UP_THRESHOLD);
    downei = i915_uncore_read(i915, GEN6_RP_DOWN_EI);
    downt = i915_uncore_read(i915, GEN6_RP_DOWN_THRESHOLD);
    cagf = i915_rps_read_actual_mhz(i915);
    i915_forcewake_put(i915);
    ier = i915_uncore_read(i915, GEN6_PMIER);
    imr = i915_uncore_read(i915, GEN6_PMIMR);
    isr = i915_uncore_read(i915, GEN6_PMISR);
    iir = i915_uncore_read(i915, GEN6_PMIIR);
    mask = i915_uncore_read(i915, GEN6_PMINTRMSK);

    emitf(&e, "Video Turbo Mode: %s\n", yes_no(ctl & GEN6_RP_MEDIA_TURBO));
    emitf(&e, "HW control enabled: %s\n", yes_no(ctl & GEN6_RP_ENABLE));
    emitf(&e, "SW control enabled: %s\n", yes_no((ctl & GEN6_RP_MEDIA_MODE_MASK) == 0));
    emitf(&e, "PM IER=0x%08x IMR=0x%08x, MASK=0x%08x\n", ier, imr, mask);
    emitf(&e, "PM ISR=0x%08x IIR=0x%08x\n", isr, iir);
    emitf(&e, "pm_intrmsk_mbz: 0x%08x\n", rps->pm_intrmsk_mbz);
    emitf(&e, "GT_PERF_STATUS: 0x%08x\n", perf);
    emitf(&e, "Render p-state ratio: %u\n", (perf & 0xff00u) >> 8);
    emitf(&e, "Render p-state VID: %u\n", perf & 0xffu);
    emitf(&e, "Render p-state limit: %u\n", limits & 0xffu);
    emitf(&e, "RPSTAT1: 0x%08x\nRPMODECTL: 0x%08x\n", stat, ctl);
    emitf(&e, "RPINCLIMIT: 0x%08x\nRPDECLIMIT: 0x%08x\n", inc, dec);
    emitf(&e, "RPNSWREQ: %uMHz\nCAGF: %uMHz\n", reqf, cagf);
    rp_counter(&e, "RP CUR UP EI", curupei);
    rp_counter(&e, "RP CUR UP", curup);
    rp_counter(&e, "RP PREV UP", prevup);
    emitf(&e, "Up threshold: %d%%\n", rps->up_threshold);
    rp_counter(&e, "RP UP EI", upei);
    rp_counter(&e, "RP UP THRESHOLD", upt);
    rp_counter(&e, "RP CUR DOWN EI", curdownei);
    rp_counter(&e, "RP CUR DOWN", curdown);
    rp_counter(&e, "RP PREV DOWN", prevdown);
    emitf(&e, "Down threshold: %d%%\n", rps->down_threshold);
    rp_counter(&e, "RP DOWN EI", downei);
    rp_counter(&e, "RP DOWN THRESHOLD", downt);
    emitf(&e, "Lowest (RPN) frequency: %dMHz\n", rps->min_freq * FREQ_MHZ);
    emitf(&e, "Nominal (RP1) frequency: %dMHz\n", rps->rp1_freq * FREQ_MHZ);
    emitf(&e, "Max non-overclocked (RP0) frequency: %dMHz\n", rps->rp0_freq * FREQ_MHZ);
    emitf(&e, "Max overclocked frequency: %dMHz\n", rps->max_freq * FREQ_MHZ);
    emitf(&e, "Current freq: %d MHz\n", rps->cur_freq * FREQ_MHZ);
    emitf(&e, "Actual freq: %u MHz\n", cagf);
    emitf(&e, "Idle freq: %d MHz\n", rps->idle_freq * FREQ_MHZ);
    emitf(&e, "Min freq: %d MHz\n", rps->min_freq * FREQ_MHZ);
    emitf(&e, "Boost freq: %d MHz\n", rps->boost_freq * FREQ_MHZ);
    emitf(&e, "Max freq: %d MHz\n", rps->max_freq * FREQ_MHZ);
    emitf(&e, "efficient (RPe) frequency: %d MHz\n", rps->efficient_freq * FREQ_MHZ);
    return emit_end(&e);
}

/**
 * Append the request `rq` of engine `en` as Linux's engine_dump_request:
 * i915_request_show's line after `prefix`, the ring's offsets, and the
 * request's commands in the ring. `started` says the chip has begun it,
 * the request before it on the ring having completed.
 */
static void engine_request(struct emit *e, struct i915_engine *en, struct i915_request *rq,
                           const char *prefix, int started)
{
    struct dma_fence *f = &rq->fence.base;
    uint32_t space = (en->head - en->tail - I915_RING_GAP) & (en->ring_size - 1u);
    uint32_t size = (rq->tail - rq->head) & (en->ring_size - 1u);

    emitf(e, "%s%c %Lx:%Lu%s%s  prio=0 @ %Lums: %s\n", prefix,
          rq->submitted ? 'E'
          : rq->awaits  ? 'U'
                        : 'R',
          f->context, f->seqno,
          i915_engine_passed(en, rq->seqno) ? "!"
          : started                         ? "*"
                                            : "",
          f->signaled            ? "+"
          : f->signaling_enabled ? "-"
                                 : "",
          ticks_ms(rtc_ticks_now() - rq->emitted), f->ops->get_timeline_name(f));
    emitf(e, "\t\tring->start:  0x%08x\n\t\tring->head:   0x%08x\n", en->ring_ggtt, en->head);
    emitf(e, "\t\tring->tail:   0x%08x\n\t\tring->emit:   0x%08x\n", en->hw_tail, en->tail);
    emitf(e, "\t\tring->space:  0x%08x\n", space);
    emitf(e, "\t\tring->hwsp:   0x%08x\n", en->hws_ggtt + I915_GEM_HWS_SEQNO_ADDR);
    emitf(e, "[head %04x, postfix %04x, tail %04x, batch 0x%08x_%08x]:\n", rq->head, rq->postfix,
          rq->tail, rq->batch ? 0u : ~0u, rq->batch ? rq->batch : ~0u);
    hexdump_ring(e, en->ring, en->ring_size / 4u, rq->head / 4u, size);
}

/**
 * Append engine `en` as Linux's intel_engine_dump for a legacy ring: its
 * wake and resets, its properties, its requests on the ring, the hung one
 * first, its registers, its status page, and its interrupt.
 */
static void engine_dump(struct emit *e, struct drm_i915_private *i915, struct i915_engine *en,
                        int i)
{
    uint32_t base = en->mmio_base, ctl, mode, flags;
    struct i915_request *rq, *hung = 0;
    int started = 1;

    emitf(e, "%s\n", en->name);
    if (i915->gt.wedged) {
        emit_s(e, "*** WEDGED ***\n");
    }
    emitf(e, "\tAwake? %d\n", en->requests ? 1 : 0);
    emit_s(e, "\tBarriers?: no\n\tForcewake: 0 domains, 0 active\n");
    emitf(e, "\tReset count: %u (global %u)\n",
          i915->gt.reset_engine_count[i915_engine_uabi_class(en)], i915->gt.reset_count);
    emitf(e, "\tProperties:\n\t\theartbeat_interval_ms: %u [default %u]\n",
          I915_HEARTBEAT_INTERVAL_MS, I915_HEARTBEAT_INTERVAL_MS);
    emit_s(e, "\t\tmax_busywait_duration_ns: 8000 [default 8000]\n"
              "\t\tpreempt_timeout_ms: 640 [default 640]\n"
              "\t\tstop_timeout_ms: 100 [default 100]\n"
              "\t\ttimeslice_duration_ms: 1 [default 1]\n");
    emit_s(e, "\tRequests:\n");
    flags = spin_lock_irqsave(&en->lock);
    for (rq = en->requests; rq; rq = rq->next) {
        if (!i915_engine_passed(en, rq->seqno)) {
            hung = rq->submitted ? rq : 0;
            break;
        }
    }
    if (hung) {
        engine_request(e, en, hung, "\t\thung", 1);
    }
    for (rq = en->requests; rq; rq = rq->next) {
        if (i915_engine_passed(en, rq->seqno)) {
            continue;
        }
        if (rq != hung) {
            engine_request(e, en, rq,
                           rq->submitted ? "\t\tactive on engine" : "\t\tactive in queue", started);
        }
        started = 0;
    }
    spin_unlock_irqrestore(&en->lock, flags);
    emit_s(e, "\tOn hold?: 0\n");
    emitf(e, "\tMMIO base:  0x%08x\n", base);
    if (i == I915_RCS0) {
        emitf(e, "\tCCID: 0x%08x\n", i915_uncore_read(i915, CCID(base)));
    }
    emitf(e, "\tRING_START: 0x%08x\n", i915_uncore_read(i915, RING_START(base)));
    emitf(e, "\tRING_HEAD:  0x%08x\n", i915_uncore_read(i915, RING_HEAD(base)) & HEAD_ADDR);
    emitf(e, "\tRING_TAIL:  0x%08x\n", i915_uncore_read(i915, RING_TAIL(base)) & TAIL_ADDR);
    ctl = i915_uncore_read(i915, RING_CTL(base));
    emitf(e, "\tRING_CTL:   0x%08x%s\n", ctl,
          ctl & (RING_WAIT | RING_WAIT_SEMAPHORE) ? " [waiting]" : "");
    mode = i915_uncore_read(i915, RING_MI_MODE(base));
    emitf(e, "\tRING_MODE:  0x%08x%s\n", mode, mode & MODE_IDLE ? " [idle]" : "");
    emitf(e, "\tRING_IMR:   0x%08x\n", i915_uncore_read(i915, RING_IMR(base)));
    emitf(e, "\tRING_ESR:   0x%08x\n", i915_uncore_read(i915, RING_ESR(base)));
    emitf(e, "\tRING_EMR:   0x%08x\n", i915_uncore_read(i915, RING_EMR(base)));
    emitf(e, "\tRING_EIR:   0x%08x\n", i915_uncore_read(i915, RING_EIR(base)));
    emitf(e, "\tACTHD:  0x%08x_%08x\n", 0u, i915_uncore_read(i915, RING_ACTHD(base)));
    emitf(e, "\tBBADDR: 0x%08x_%08x\n", 0u, i915_uncore_read(i915, RING_BBADDR(base)));
    emitf(e, "\tDMA_FADDR: 0x%08x_%08x\n", 0u, i915_uncore_read(i915, RING_DMA_FADD(base)));
    emitf(e, "\tIPEIR: 0x%08x\n", i915_uncore_read(i915, RING_IPEIR(base)));
    emitf(e, "\tIPEHR: 0x%08x\n", i915_uncore_read(i915, RING_IPEHR(base)));
    emitf(e, "\tPP_DIR_BASE: 0x%08x\n", i915_uncore_read(i915, RING_PP_DIR_BASE(base)));
    emitf(e, "\tPP_DIR_BASE_READ: 0x%08x\n", i915_uncore_read(i915, RING_PP_DIR_BASE_READ(base)));
    emitf(e, "\tPP_DIR_DCLV: 0x%08x\n", i915_uncore_read(i915, RING_PP_DIR_DCLV(base)));
    emit_s(e, "HWSP:\n");
    if (en->hws) {
        hexdump_ring(e, en->hws, I915_GTT_PAGE_SIZE / 4u, 0, I915_GTT_PAGE_SIZE);
    }
    emitf(e, "Idle? %s\n", yes_no(!en->requests));
    emitf(e, "IRQ: %s\n", en->irq_refs ? "enabled" : "disabled");
}

/** i915_engine_info, Linux's i915_engine_info: whether the GT is awake
 * and for how long it has been, the timestamp's frequency, and each
 * engine the driver runs. */
static int32_t engine_info_show(void *data, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    struct i915_gt *gt = &i915->gt;
    uint64_t awake = gt->awake_ticks;
    struct emit e;
    int i;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    if (gone(i915)) {
        return -ENODEV;
    }
    if (gt->awake) {
        awake += rtc_ticks_now() - gt->unpark_ticks;
    }
    emitf(&e, "GT awake? %s [%d], %Lums\n", yes_no(gt->awake), gt->awake ? 1 : 0, ticks_ms(awake));
    emitf(&e, "CS timestamp frequency: %u Hz, %u ns\n", CS_TIMESTAMP_HZ, CS_TIMESTAMP_NS);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        if (gt->engine[i].running) {
            engine_dump(&e, i915, &gt->engine[i], i);
        }
    }
    return emit_end(&e);
}

/** i915_cdclk_info, Linux's i915_cdclk_info_show: the CD clock now, the
 * most it may be, and the fastest pixel clock that allows. IGT reads the
 * second (igt_get_max_cdclk). */
static int32_t cdclk_info_show(void *data, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emitf(&e, "Current CD clock frequency: %u kHz\n", i915->display.cdclk);
    emitf(&e, "Max CD clock frequency: %u kHz\n", i915->display.max_cdclk);
    emitf(&e, "Max pixel clock frequency: %u kHz\n", intel_max_dotclock(i915));
    return emit_end(&e);
}

/** i915_wedged, Linux's i915_wedged_get: 1 while the GPU is wedged, else
 * 0, as "%llu\n". */
static int32_t wedged_show(void *data, char *buf, uint32_t cap)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    struct emit e;

    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emitf(&e, "%u\n", i915->gt.wedged ? 1u : 0u);
    return emit_end(&e);
}

/**
 * A write of i915_wedged, Linux's i915_wedged_set: the number written, in
 * any base kstrtoull takes, is a mask of engines by Linux's engine ids,
 * which `i915_handle_error` recovers as from a hang with its state
 * captured: each engine reset alone, the whole GPU where that fails or
 * the GPU is wedged, which IGT's igt_force_gpu_reset writes -1 for.
 */
static int32_t wedged_store(void *data, const char __user *buf, uint32_t len)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    char text[24], msg[48];
    struct emit e;
    uint64_t val;
    int32_t rc;

    rc = copy_write(buf, len, text, sizeof(text));
    if (rc >= 0) {
        rc = parse_u64(text, (uint32_t)rc, &val);
    }
    if (rc < 0) {
        return rc;
    }
    if (gone(i915)) {
        return -ENODEV;
    }
    e.buf = msg;
    e.cap = sizeof(msg);
    e.len = 0;
    emitf(&e, "Manually reset engine mask to %Lx", val);
    (void)emit_end(&e);
    msg[sizeof(msg) - 1u] = '\0';
    i915_handle_error(i915, (uint32_t)val, msg);
    return (int32_t)len;
}

/** i915_gem_drop_caches, Linux's i915_drop_caches_get: the DROP_ bits it
 * takes, as "0x%08llx\n", which IGT's igt_drop_caches_has reads. */
static int32_t drop_caches_show(void *data, char *buf, uint32_t cap)
{
    struct emit e;

    (void)data;
    e.buf = buf;
    e.cap = cap;
    e.len = 0;
    emitf(&e, "0x%08x\n", DROP_ALL);
    return emit_end(&e);
}

/** Whether every engine has finished its requests within `ms`, as
 * Linux's wait_for(intel_engines_are_idle(gt), ms): each last request
 * waited for, uninterruptibly, until the time runs out. */
static int idle_within(struct drm_i915_private *i915, uint32_t ms)
{
    uint64_t end = rtc_ticks_now() + tick_div((uint64_t)ms * TICK_HZ, 1000u, (uint32_t *)0);

    for (;;) {
        struct i915_request *rq = 0;
        uint64_t now;
        int32_t err;
        int i;

        i915_retire_requests(i915);
        for (i = 0; i < I915_NUM_ENGINES && !rq; i++) {
            rq = i915->gt.engine[i].requests_tail;
        }
        if (!rq) {
            return 1;
        }
        now = rtc_ticks_now();
        if (now >= end) {
            return 0;
        }
        (void)dma_fence_get(&rq->fence.base);
        err = i915_request_wait(rq, 0, (int64_t)ticks_ms(end - now) * 1000000);
        i915_request_put(rq);
        if (err < 0) {
            return 0;
        }
    }
}

/**
 * A write of i915_gem_drop_caches, Linux's i915_drop_caches_set with the
 * DROP_ bits written: RESET_ACTIVE wedges a GPU that does not idle within
 * 200 ms and resets it after, RETIRE retires, ACTIVE and IDLE wait for the
 * GPU to idle, which parks it, and BOUND, UNBOUND and SHRINK_ALL purge the
 * objects with bindings, without and both. FREED, RESET_SEQNO and RCU have
 * nothing to do, since objects go at their last reference. The wait for
 * idle may sleep and a signal ends it with -EINTR.
 */
static int32_t drop_caches_store(void *data, const char __user *buf, uint32_t len)
{
    struct drm_i915_private *i915 = (struct drm_i915_private *)data;
    char text[24];
    uint64_t val;
    int32_t rc;

    rc = copy_write(buf, len, text, sizeof(text));
    if (rc >= 0) {
        rc = parse_u64(text, (uint32_t)rc, &val);
    }
    if (rc < 0) {
        return rc;
    }
    if (gone(i915)) {
        return -ENODEV;
    }
    if ((val & DROP_RESET_ACTIVE) && !idle_within(i915, DROP_IDLE_WAIT_MS)) {
        i915_gt_set_wedged(i915);
    }
    if (val & DROP_RETIRE) {
        i915_retire_requests(i915);
    }
    if (val & (DROP_IDLE | DROP_ACTIVE)) {
        rc = i915_gt_wait_for_idle(i915, 1);
        if (rc < 0) {
            return rc;
        }
    }
    if ((val & DROP_RESET_ACTIVE) && i915->gt.wedged) {
        i915_reset_gt(i915, (1u << I915_NUM_ENGINES) - 1u, "drop caches");
    }
    if (val & DROP_BOUND) {
        (void)i915_gem_shrink_flags(i915, ~0u, I915_SHRINK_BOUND);
    }
    if (val & DROP_UNBOUND) {
        (void)i915_gem_shrink_flags(i915, ~0u, I915_SHRINK_UNBOUND);
    }
    if (val & DROP_SHRINK_ALL) {
        (void)i915_gem_shrink_flags(i915, ~0u, I915_SHRINK_BOUND | I915_SHRINK_UNBOUND);
    }
    return (int32_t)len;
}

static const struct debugfs_text_ops capabilities_ops = {capabilities_show, NULL};
static const struct debugfs_text_ops gem_objects_ops = {gem_objects_show, NULL};
static const struct debugfs_text_ops frequency_ops = {frequency_show, NULL};
static const struct debugfs_text_ops engine_info_ops = {engine_info_show, NULL};
static const struct debugfs_text_ops cdclk_info_ops = {cdclk_info_show, NULL};
static const struct debugfs_text_ops wedged_ops = {wedged_show, wedged_store};
static const struct debugfs_text_ops drop_caches_ops = {drop_caches_show, drop_caches_store};

/* The text files, with Linux's modes. */
static const struct {
    const char *name;
    uint32_t mode;
    const struct debugfs_text_ops *ops;
} text_files[] = {
    {"i915_capabilities", 0444u, &capabilities_ops},  {"i915_gem_objects", 0444u, &gem_objects_ops},
    {"i915_frequency_info", 0444u, &frequency_ops},   {"i915_engine_info", 0444u, &engine_info_ops},
    {"i915_cdclk_info", 0444u, &cdclk_info_ops},      {"i915_wedged", 0644u, &wedged_ops},
    {"i915_gem_drop_caches", 0644u, &drop_caches_ops}};

void i915_debugfs_register(struct drm_i915_private *i915)
{
    char path[64];
    int32_t rc = 0;
    int card;
    unsigned i;

    if (!i915->drm.debugfs) {
        return;
    }
    for (i = 0; rc == 0 && i < NELEM(text_files); i++) {
        drm_debugfs_path(&i915->drm, text_files[i].name, path, sizeof(path));
        rc = debugfs_create_text(path, text_files[i].mode, text_files[i].ops, i915);
    }
    if (rc == 0) {
        drm_debugfs_path(&i915->drm, "i915_error_state", path, sizeof(path));
        rc = debugfs_create_file(path, 0644u, &error_state_ops, error_state_open, i915);
    }
    if (rc == 0) {
        drm_debugfs_path(&i915->drm, "i915_gpu_info", path, sizeof(path));
        rc = debugfs_create_file(path, 0644u, &gpu_info_ops, gpu_info_open, i915);
    }
    if (rc != 0) {
        klog(KLOG_WARN, "i915", "debugfs: %s: error %d", path, (int)rc);
    }
    card = drm_sysfs_card_dir(&i915->drm);
    if (card > 0 && (rc = sysfs_add_attr(card, &error_attr, i915, 0)) < 0) {
        klog(KLOG_WARN, "i915", "sysfs: error not added: error %d", (int)rc);
    }
}
