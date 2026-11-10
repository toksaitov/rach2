/*
 * i915_reg.h: the registers of Haswell's graphics device that the Intel
 * driver uses, by Linux 6.12's names and with Haswell's values only.
 */
#ifndef _I915_REG_H_
#define _I915_REG_H_

/*
 * Each block names the Linux 6.12 file it is transcribed from, under
 * drivers/gpu/drm/i915/ unless the path says otherwise. Linux's _MMIO()
 * wrapper is dropped: a register here is its offset in BAR 0, an unsigned
 * int, and a register of an engine, pipe, transcoder or port is a macro of
 * that index. Where Linux has a value per generation, the one Haswell
 * (graphics version 7.5, display version 7) takes is the one kept, and a
 * name that Linux gives several generations keeps Linux's name. A field
 * that a later part of the driver needs and that is not here is added to
 * its register's block, from the same file.
 *
 * Nothing here is read or written but through i915_hw.h.
 */

/* ------------------------------------------------------------------------
 * PCI configuration space (intel_pci_config.h, include/drm/intel/i915_drm.h)
 */

#define I915_GTTMMADR_BAR 0 /* BAR 0: registers, then the GTT */
#define I915_GMADR_BAR    2 /* BAR 2: the aperture */
#define I915_IO_BAR       4

/* The GMCH graphics control word, at 0x50 of 00:02.0 and of 00:00.0. */
#define SNB_GMCH_CTRL       0x50
#define SNB_GMCH_GGMS_SHIFT 8 /* GTT size: 0 none, 1 1 MiB, 2 2 MiB */
#define SNB_GMCH_GGMS_MASK  0x3
#define SNB_GMCH_GMS_SHIFT  3 /* stolen memory in units of 32 MiB */
#define SNB_GMCH_GMS_MASK   0x1f

/* The base of stolen memory; its low bit is the lock. */
#define INTEL_BSM      0x5c
#define INTEL_BSM_MASK 0xfff00000u

/* The aperture's size, BAR 2's, by its code in bits 2:1 (Haswell's
 * datasheet, MSAC of device 2): 0 128 MiB, 1 256 MiB, 3 512 MiB. Linux
 * takes the size from the BAR, which the kernel does not size here, since
 * the console draws through it. */
#define HSW_MSAC            0x62
#define HSW_MSAC_SIZE_SHIFT 1
#define HSW_MSAC_SIZE_MASK  0x3

#define ASLE  0xe4
#define SWSCI 0xe8
#define LBPC  0xf4
#define ASLS  0xfc /* the OpRegion's physical address */

/* ------------------------------------------------------------------------
 * The layout of BAR 0 (gt/intel_ggtt.c: gen6_gttmmadr_size and
 * gen6_gttadr_offset; intel_uncore.c: intel_uncore_setup_mmio)
 */

#define I915_GTTMMADR_SIZE 0x400000u /* 4 MiB */
#define I915_MMIO_SIZE     0x200000u /* the registers, the first half */
#define I915_GSM_OFFSET    0x200000u /* the GTT, the second half */

/* ------------------------------------------------------------------------
 * Uncore and forcewake (gt/intel_gt_regs.h, intel_mchbar_regs.h)
 */

#define GAM_ECOCHK                      0x4090
#define ECOCHK_PPGTT_WB_HSW             (3u << 3)
#define GAC_ECO_BITS                    0x14090
#define ECOBITS_PPGTT_CACHE64B          (3u << 8)
#define HSW_IDICR                       0x9008
#define HSW_PAVP_FUSE1                  0x911c
#define HSW_F1_EU_DIS_SHIFT             16 /* EUs disabled per subslice */
#define HSW_F1_EU_DIS_MASK              (3u << 16)
#define IDIHASHMSK(x)                   (((x) & 0x3fu) << 16)
#define ECOBUS                          0xa180
#define FORCEWAKE_MT_ENABLE             (1u << 5)
#define FORCEWAKE_MT                    0xa188 /* masked: bits 31:16 enable 15:0 */
#define GTFIFODBG                       0x120000
#define GT_FIFO_SBDROPERR               (1u << 6)
#define GT_FIFO_BLOBDROPERR             (1u << 5)
#define GT_FIFO_SB_READ_ABORTERR        (1u << 4)
#define GT_FIFO_DROPERR                 (1u << 3)
#define GT_FIFO_OVFERR                  (1u << 2)
#define GT_FIFO_IAWRERR                 (1u << 1)
#define GT_FIFO_IARDERR                 (1u << 0)
#define GTFIFOCTL                       0x120008
#define GT_FIFO_FREE_ENTRIES_MASK       0x7fu
#define GT_FIFO_NUM_RESERVED_ENTRIES    20
#define HSW_EDRAM_CAP                   0x120010
#define EDRAM_ENABLED                   (1u << 0)
#define FORCEWAKE_ACK_HSW               0x130044
#define FORCEWAKE_KERNEL                (1u << 0)
#define FORCEWAKE_USER                  (1u << 1)
#define FORCEWAKE_KERNEL_FALLBACK       (1u << 15)
#define GEN6_GT_THREAD_STATUS_REG       0x13805c
#define GEN6_GT_THREAD_STATUS_CORE_MASK 0x7u
#define GEN6_STOLEN_RESERVED            0x1082c0 /* i915_reg.h */
#define GEN7_STOLEN_RESERVED_ADDR_MASK  (0x3fffu << 18)
#define GEN7_STOLEN_RESERVED_SIZE_MASK  (1u << 5)
#define GEN7_STOLEN_RESERVED_1M         (0u << 5)
#define GEN7_STOLEN_RESERVED_256K       (1u << 5)
#define GEN6_STOLEN_RESERVED_ENABLE     (1u << 0)
#define FPGA_DBG                        0x42300
#define FPGA_DBG_RM_NOCLAIM             (1u << 31)

/* A masked register: the high half selects which bits of the low half a
 * write changes (i915_reg_defs.h: _MASKED_BIT_ENABLE and _DISABLE). */
#define I915_MASKED_ENABLE(a)  (((uint32_t)(a) << 16) | (uint32_t)(a))
#define I915_MASKED_DISABLE(a) ((uint32_t)(a) << 16)

/* The MCHBAR mirror in BAR 0 (intel_mchbar_regs.h). */
#define MCHBAR_MIRROR_BASE_SNB 0x140000
#define MAD_DIMM_C0            (MCHBAR_MIRROR_BASE_SNB + 0x5004)
#define MAD_DIMM_C1            (MCHBAR_MIRROR_BASE_SNB + 0x5008)
#define MAD_DIMM_B_SIZE_MASK   (0xffu << 8)
#define MAD_DIMM_A_SIZE_MASK   (0xffu << 0)
#define GEN6_GT_PERF_STATUS    (MCHBAR_MIRROR_BASE_SNB + 0x5948)
#define GEN6_RP_STATE_LIMITS   (MCHBAR_MIRROR_BASE_SNB + 0x5994)
#define GEN6_RP_STATE_CAP      (MCHBAR_MIRROR_BASE_SNB + 0x5998)
#define DCLK                   (MCHBAR_MIRROR_BASE_SNB + 0x5e04)
#define D_COMP_HSW             (MCHBAR_MIRROR_BASE_SNB + 0x5f0c)

/* ------------------------------------------------------------------------
 * Interrupts (i915_reg.h): the display engine's, the GT's, the power
 * management's and the south display's, each a status, mask, identity
 * and enable register.
 */

#define DEISR                         0x44000
#define DEIMR                         0x44004
#define DEIIR                         0x44008
#define DEIER                         0x4400c
#define DE_MASTER_IRQ_CONTROL         (1u << 31)
#define DE_ERR_INT_IVB                (1u << 30)
#define DE_GSE_IVB                    (1u << 29)
#define DE_PCH_EVENT_IVB              (1u << 28)
#define DE_DP_A_HOTPLUG_IVB           (1u << 27)
#define DE_AUX_CHANNEL_A_IVB          (1u << 26)
#define DE_EDP_PSR_INT_HSW            (1u << 19)
#define DE_PLANE_FLIP_DONE_IVB(plane) (1u << (3 + 5 * (plane)))
#define DE_PIPE_VBLANK_IVB(pipe)      (1u << ((pipe) * 5))

#define GTISR                                  0x44010
#define GTIMR                                  0x44014
#define GTIIR                                  0x44018
#define GTIER                                  0x4401c
#define GT_BLT_FLUSHDW_NOTIFY_INTERRUPT        (1u << 26)
#define GT_BLT_CS_ERROR_INTERRUPT              (1u << 25)
#define GT_BLT_USER_INTERRUPT                  (1u << 22)
#define GT_BSD_CS_ERROR_INTERRUPT              (1u << 15)
#define GT_BSD_USER_INTERRUPT                  (1u << 12)
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT_S1 (1u << 11)
#define GT_RENDER_L3_PARITY_ERROR_INTERRUPT    (1u << 5)
#define GT_RENDER_PIPECTL_NOTIFY_INTERRUPT     (1u << 4)
#define GT_CS_MASTER_ERROR_INTERRUPT           (1u << 3)
#define GT_RENDER_USER_INTERRUPT               (1u << 0)

#define GEN6_PMISR                  0x44020
#define GEN6_PMIMR                  0x44024
#define GEN6_PMIIR                  0x44028
#define GEN6_PMIER                  0x4402c
#define PM_VEBOX_CS_ERROR_INTERRUPT (1u << 12)
#define PM_VEBOX_USER_INTERRUPT     (1u << 10)
#define GEN6_PM_RP_DOWN_TIMEOUT     (1u << 6)
#define GEN6_PM_RP_UP_THRESHOLD     (1u << 5)
#define GEN6_PM_RP_DOWN_THRESHOLD   (1u << 4)
#define GEN6_PM_RP_UP_EI_EXPIRED    (1u << 2)
#define GEN6_PM_RP_DOWN_EI_EXPIRED  (1u << 1)
#define GEN6_PM_RPS_EVENTS                                                             \
    (GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_UP_THRESHOLD | GEN6_PM_RP_DOWN_EI_EXPIRED | \
     GEN6_PM_RP_DOWN_THRESHOLD | GEN6_PM_RP_DOWN_TIMEOUT)

#define GEN7_ERR_INT                0x44040
#define DERRMR                      0x44050 /* the display's events the GT may wait on, masked */
#define ERR_INT_PIPE_CRC_DONE(pipe) (1u << (2 + (pipe) * 3))
#define ERR_INT_FIFO_UNDERRUN(pipe) (1u << ((pipe) * 3))

/* The eDP panel's self refresh (display/intel_psr_regs.h). */
#define EDP_PSR_IMR 0x64834
#define EDP_PSR_IIR 0x64838

#define SDEISR                0xc4000
#define SDEIMR                0xc4004
#define SDEIIR                0xc4008
#define SDEIER                0xc400c
#define SERR_INT              0xc4040
#define SDE_AUXD_CPT          (1u << 27)
#define SDE_AUXC_CPT          (1u << 26)
#define SDE_AUXB_CPT          (1u << 25)
#define SDE_PORTD_HOTPLUG_CPT (1u << 23)
#define SDE_PORTC_HOTPLUG_CPT (1u << 22)
#define SDE_PORTB_HOTPLUG_CPT (1u << 21)
#define SDE_CRT_HOTPLUG_CPT   (1u << 19)
#define SDE_GMBUS_CPT         (1u << 17)
#define SDE_ERROR_CPT         (1u << 16)
#define PCH_PORT_HOTPLUG      0xc4030

/* ------------------------------------------------------------------------
 * Engines (i915_reg.h, gt/intel_engine_regs.h, gt/intel_gt_regs.h): each
 * engine's registers are at its base.
 */

#define RENDER_RING_BASE   0x02000
#define GEN6_BSD_RING_BASE 0x12000
#define VEBOX_RING_BASE    0x1a000
#define BLT_RING_BASE      0x22000

#define RING_TAIL(base)             ((base) + 0x30)
#define TAIL_ADDR                   0x001ffff8u
#define RING_HEAD(base)             ((base) + 0x34)
#define HEAD_WRAP_COUNT             0xffe00000u
#define HEAD_ADDR                   0x001ffffcu
#define RING_START(base)            ((base) + 0x38)
#define RING_CTL(base)              ((base) + 0x3c)
#define RING_CTL_SIZE(size)         ((uint32_t)(size) - 4096u) /* bytes, to pages */
#define RING_NR_PAGES               0x001ff000u
#define RING_VALID                  0x00000001u
#define RING_WAIT_SEMAPHORE         (1u << 10)
#define RING_WAIT                   (1u << 11)
#define RING_PSMI_CTL(base)         ((base) + 0x50)
#define RING_MAX_IDLE(base)         ((base) + 0x54)
#define GEN6_BSD_GO_INDICATOR       (1u << 4)
#define GEN6_BSD_SLEEP_INDICATOR    (1u << 3)
#define GEN6_PSMI_SLEEP_MSG_DISABLE (1u << 0)
#define RING_IPEIR(base)            ((base) + 0x64)
#define RING_IPEHR(base)            ((base) + 0x68)
#define RING_INSTDONE(base)         ((base) + 0x6c)
#define RING_INSTPS(base)           ((base) + 0x70)
#define RING_ACTHD(base)            ((base) + 0x74)
#define RING_DMA_FADD(base)         ((base) + 0x78)
#define RING_HWS_PGA(base)          ((base) + 0x80)
#define RING_NOPID(base)            ((base) + 0x94)
#define RING_HWSTAM(base)           ((base) + 0x98)
#define RING_MI_MODE(base)          ((base) + 0x9c)
#define MODE_IDLE                   (1u << 9)
#define STOP_RING                   (1u << 8)
#define RING_IMR(base)              ((base) + 0xa8)
#define RING_EIR(base)              ((base) + 0xb0)
#define RING_EMR(base)              ((base) + 0xb4)
#define RING_ESR(base)              ((base) + 0xb8)
#define RING_INSTPM(base)           ((base) + 0xc0)
#define RING_BBSTATE(base)          ((base) + 0x110)
#define RING_BB_PPGTT               (1u << 5)
#define RING_SBBADDR(base)          ((base) + 0x114)
#define RING_SBBSTATE(base)         ((base) + 0x118)
#define RING_BBADDR(base)           ((base) + 0x140)
#define CCID(base)                  ((base) + 0x180)
#define RING_PP_DIR_DCLV(base)      ((base) + 0x220)
#define PP_DIR_DCLV_2G              0xffffffffu
#define RING_PP_DIR_BASE(base)      ((base) + 0x228)
#define RING_PP_DIR_BASE_READ(base) ((base) + 0x518)
#define RING_MODE_GEN7(base)        ((base) + 0x29c)
#define GFX_RUN_LIST_ENABLE         (1u << 15)
#define GFX_INTERRUPT_STEERING      (1u << 14)
#define GFX_TLB_INVALIDATE_EXPLICIT (1u << 13)
#define GFX_PPGTT_ENABLE            (1u << 9)
#define RING_TIMESTAMP(base)        ((base) + 0x358)
#define RING_TIMESTAMP_UDW(base)    ((base) + 0x358 + 4)

/* The status page addresses of Haswell's four engines. */
#define RENDER_HWS_PGA_GEN7 0x4080
#define BSD_HWS_PGA_GEN7    0x4180
#define BLT_HWS_PGA_GEN7    0x4280
#define VEBOX_HWS_PGA_GEN7  0x4380

#define HWSTAM                            0x2098
#define EIR                               0x20b0
#define EMR                               0x20b4
#define ESR                               0x20b8
#define INSTPM                            0x20c0
#define INSTPM_TLB_INVALIDATE             (1u << 9) /* of RING_INSTPM, masked */
#define INSTPM_FORCE_ORDERING             (1u << 7)
#define INSTPM_SYNC_FLUSH                 (1u << 5)
#define GEN7_FF_THREAD_MODE               0x20a0
#define GFX_MODE                          0x2520
#define ERROR_GEN6                        0x40a0
#define DONE_REG                          0x40b0
#define ARB_MODE                          0x4030
#define ARB_MODE_SWIZZLE_IVB              (1u << 5)
#define HSW_GTT_CACHE_EN                  0x4024
#define CACHE_MODE_0_GEN7                 0x7000
#define RC_OP_FLUSH_ENABLE                (1u << 0) /* of CACHE_MODE_0_GEN7, masked */
#define CACHE_MODE_1                      0x7004
#define PIXEL_SUBSPAN_COLLECT_OPT_DISABLE (1u << 6) /* of CACHE_MODE_1, masked */
#define GEN7_GT_MODE                      0x7008
#define GEN7_SC_INSTDONE                  0x7100
#define GEN7_L3CNTLREG1                   0xb01c
#define GEN7_L3SQCREG4                    0xb034
#define HSW_SCRATCH1                      0xb038
#define GEN7_SAMPLER_INSTDONE             0xe160
#define GEN7_ROW_INSTDONE                 0xe164
#define BCS_SWCTRL                        0x22200
/* The four stream-out buffers' write offsets, which I915_EXEC_GEN7_SOL_RESET
 * clears before a batch (i915_reg.h). */
#define GEN7_SO_WRITE_OFFSET(n)           (0x5280u + (uint32_t)(n) * 4u)

/* ------------------------------------------------------------------------
 * Commands (gt/intel_gpu_commands.h): a command's first dword holds its
 * opcode in bits 28:23 and, for one of more than a dword, its length less
 * 2 in the low bits. The status page's dwords (gt/intel_engine.h).
 */

#define MI_INSTR(opcode, flags)    (((uint32_t)(opcode) << 23) | (uint32_t)(flags))
#define MI_NOOP                    MI_INSTR(0, 0)
#define MI_USER_INTERRUPT          MI_INSTR(0x02, 0)
#define MI_BATCH_BUFFER_END        MI_INSTR(0x0a, 0)
#define MI_STORE_DWORD_IMM_GEN4    MI_INSTR(0x20, 2)
#define MI_USE_GGTT                (1u << 22)
#define MI_STORE_DWORD_INDEX       MI_INSTR(0x21, 1)
#define MI_STORE_DWORD_INDEX_SHIFT 2
#define MI_OPCODE(dw)              (((dw) >> 23) & 0x3fu)
#define MI_LENGTH(dw)              (((dw) & 0x3fu) + 2u)
#define MI_ARB_ON_OFF              MI_INSTR(0x08, 0)
#define MI_ARB_ENABLE              (1u << 0)
#define MI_ARB_DISABLE             (0u << 0)
#define MI_SET_CONTEXT             MI_INSTR(0x18, 0)
#define MI_MM_SPACE_GTT            (1u << 8)
#define MI_SAVE_EXT_STATE_EN       (1u << 3)
#define MI_RESTORE_EXT_STATE_EN    (1u << 2)
#define MI_FORCE_RESTORE           (1u << 1)
#define MI_RESTORE_INHIBIT         (1u << 0)
#define MI_LOAD_REGISTER_IMM(n)    MI_INSTR(0x22, 2u * (n) - 1u)
#define MI_STORE_REGISTER_MEM      MI_INSTR(0x24, 1)
#define MI_SRM_LRM_GLOBAL_GTT      (1u << 22)
/* MI_BATCH_BUFFER_START on graphics 6 and 7: the batch's address follows.
 * Bit 8 is the address space, the PPGTT when set, which the blitter's
 * gen6_emit_bb_start sets as MI_BATCH_NON_SECURE_I965 and Haswell's render
 * ring as MI_BATCH_PPGTT_HSW; Haswell's render ring takes non-secure in bit
 * 13 (gt/gen6_engine_cs.c). A secure batch sets neither. */
#define MI_BATCH_BUFFER_START      MI_INSTR(0x31, 0)
#define MI_BATCH_NON_SECURE_I965   (1u << 8)
#define MI_BATCH_PPGTT_HSW         (1u << 8)
#define MI_BATCH_NON_SECURE_HSW    (1u << 13)
#define MI_FLUSH_DW                MI_INSTR(0x26, 1)
#define MI_FLUSH_DW_STORE_INDEX    (1u << 21)
#define MI_INVALIDATE_TLB          (1u << 18)
#define MI_FLUSH_DW_OP_STOREDW     (1u << 14)
#define MI_FLUSH_DW_USE_GTT        (1u << 2)

/* The render pipe's flush and post-sync write: four dwords, the flags,
 * the GTT address and the value. */
#define GFX_OP_PIPE_CONTROL(len)                  ((3u << 29) | (3u << 27) | (2u << 24) | ((len) - 2u))
#define PIPE_CONTROL_GLOBAL_GTT_IVB               (1u << 24)
#define PIPE_CONTROL_CS_STALL                     (1u << 20)
#define PIPE_CONTROL_TLB_INVALIDATE               (1u << 18)
#define PIPE_CONTROL_MEDIA_STATE_CLEAR            (1u << 16)
#define PIPE_CONTROL_QW_WRITE                     (1u << 14)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH    (1u << 12)
#define PIPE_CONTROL_INSTRUCTION_CACHE_INVALIDATE (1u << 11)
#define PIPE_CONTROL_TEXTURE_CACHE_INVALIDATE     (1u << 10)
#define PIPE_CONTROL_FLUSH_ENABLE                 (1u << 7)
#define PIPE_CONTROL_DC_FLUSH_ENABLE              (1u << 5)
#define PIPE_CONTROL_VF_CACHE_INVALIDATE          (1u << 4)
#define PIPE_CONTROL_CONST_CACHE_INVALIDATE       (1u << 3)
#define PIPE_CONTROL_STATE_CACHE_INVALIDATE       (1u << 2)
#define PIPE_CONTROL_STALL_AT_SCOREBOARD          (1u << 1)
#define PIPE_CONTROL_DEPTH_CACHE_FLUSH            (1u << 0)

#define I915_GEM_HWS_SEQNO      0x40 /* the dword the breadcrumb writes */
#define I915_GEM_HWS_SEQNO_ADDR (I915_GEM_HWS_SEQNO << MI_STORE_DWORD_INDEX_SHIFT)
#define I915_GEM_HWS_SCRATCH    0x80 /* the dword a flush's post-sync write takes */
#define HWS_SCRATCH_ADDR        (I915_GEM_HWS_SCRATCH << MI_STORE_DWORD_INDEX_SHIFT)

/* ------------------------------------------------------------------------
 * The command parser's commands and registers (gt/intel_gpu_commands.h,
 * gt/intel_gt_regs.h, gt/intel_engine_regs.h, i915_reg.h): what the tables
 * of i915_cmd_parser.c name beside the commands above. A command's client
 * is in bits 31:29, MI 0, the blitter's 2 and the render pipe's 3, whose
 * subclient is in bits 28:27; the media subclient is 2.
 */

#define INSTR_CLIENT_SHIFT    29
#define INSTR_MI_CLIENT       0x0u
#define INSTR_BC_CLIENT       0x2u
#define INSTR_RC_CLIENT       0x3u
#define INSTR_SUBCLIENT_SHIFT 27
#define INSTR_SUBCLIENT_MASK  0x18000000u
#define INSTR_MEDIA_SUBCLIENT 0x2u

#define MI_SET_PREDICATE                MI_INSTR(0x01, 0)
#define MI_WAIT_FOR_EVENT               MI_INSTR(0x03, 0)
#define MI_FLUSH                        MI_INSTR(0x04, 0)
#define MI_ARB_CHECK                    MI_INSTR(0x05, 0)
#define MI_RS_CONTROL                   MI_INSTR(0x06, 0)
#define MI_REPORT_HEAD                  MI_INSTR(0x07, 0)
#define MI_URB_ATOMIC_ALLOC             MI_INSTR(0x09, 0)
#define MI_SUSPEND_FLUSH                MI_INSTR(0x0b, 0)
#define MI_PREDICATE                    MI_INSTR(0x0c, 0)
#define MI_TOPOLOGY_FILTER              MI_INSTR(0x0d, 0)
#define MI_SET_APPID                    MI_INSTR(0x0e, 0)
#define MI_RS_CONTEXT                   MI_INSTR(0x0f, 0)
#define MI_LOAD_SCAN_LINES_INCL         MI_INSTR(0x12, 0)
#define MI_LOAD_SCAN_LINES_EXCL         MI_INSTR(0x13, 0)
#define MI_DISPLAY_FLIP                 MI_INSTR(0x14, 2)
#define MI_SEMAPHORE_MBOX               MI_INSTR(0x16, 1)
#define MI_URB_CLEAR                    MI_INSTR(0x19, 0)
#define MI_STORE_DWORD_IMM              MI_INSTR(0x20, 1)
#define MI_UPDATE_GTT                   MI_INSTR(0x23, 0)
#define MI_CLFLUSH                      MI_INSTR(0x27, 0)
#define MI_REPORT_PERF_COUNT            MI_INSTR(0x28, 0)
#define MI_REPORT_PERF_COUNT_GGTT       (1u << 0)
#define MI_LOAD_REGISTER_MEM            MI_INSTR(0x29, 1)
#define MI_LOAD_REGISTER_REG            MI_INSTR(0x2a, 1)
#define MI_RS_STORE_DATA_IMM            MI_INSTR(0x2b, 0)
#define MI_LOAD_URB_MEM                 MI_INSTR(0x2c, 0)
#define MI_STORE_URB_MEM                MI_INSTR(0x2d, 0)
#define MI_CONDITIONAL_BATCH_BUFFER_END MI_INSTR(0x36, 0)
#define MI_FLUSH_DW_OP_MASK             (3u << 14)
#define MI_FLUSH_DW_NOTIFY              (1u << 8)

#define GFX_OP_3DSTATE_VF_STATISTICS         ((3u << 29) | (1u << 27) | (0u << 24) | (0x0bu << 16))
#define PIPELINE_SELECT                      ((3u << 29) | (1u << 27) | (1u << 24) | (0x04u << 16))
#define MEDIA_VFE_STATE                      ((3u << 29) | (2u << 27) | (0u << 24) | (0x00u << 16))
#define MEDIA_VFE_STATE_MMIO_ACCESS_MASK     0x18u
#define GPGPU_OBJECT                         ((3u << 29) | (2u << 27) | (1u << 24) | (0x04u << 16))
#define GPGPU_WALKER                         ((3u << 29) | (2u << 27) | (1u << 24) | (0x05u << 16))
#define GFX_OP_3DSTATE_DX9_CONSTANTF_VS      ((3u << 29) | (3u << 27) | (0u << 24) | (0x39u << 16))
#define GFX_OP_3DSTATE_DX9_CONSTANTF_PS      ((3u << 29) | (3u << 27) | (0u << 24) | (0x3au << 16))
#define GFX_OP_3DSTATE_BINDING_TABLE_EDIT_VS ((3u << 29) | (3u << 27) | (0u << 24) | (0x43u << 16))
#define GFX_OP_3DSTATE_BINDING_TABLE_EDIT_GS ((3u << 29) | (3u << 27) | (0u << 24) | (0x44u << 16))
#define GFX_OP_3DSTATE_BINDING_TABLE_EDIT_HS ((3u << 29) | (3u << 27) | (0u << 24) | (0x45u << 16))
#define GFX_OP_3DSTATE_BINDING_TABLE_EDIT_DS ((3u << 29) | (3u << 27) | (0u << 24) | (0x46u << 16))
#define GFX_OP_3DSTATE_BINDING_TABLE_EDIT_PS ((3u << 29) | (3u << 27) | (0u << 24) | (0x47u << 16))
#define GFX_OP_3DSTATE_SO_DECL_LIST          ((3u << 29) | (3u << 27) | (1u << 24) | (0x17u << 16))
#define PIPE_CONTROL_MMIO_WRITE              (1u << 23)
#define PIPE_CONTROL_STORE_DATA_INDEX        (1u << 21)
#define PIPE_CONTROL_POST_SYNC_OP_MASK       (3u << 14)
#define PIPE_CONTROL_NOTIFY                  (1u << 8)
#define COLOR_BLT                            ((2u << 29) | (0x40u << 22))
#define SRC_COPY_BLT                         ((2u << 29) | (0x43u << 22))

/* The video ring's base on graphics 4 and 5, whose timestamp Linux's
 * allowlists name on graphics 7 too (dev-log/78-intel/upstream.md). */
#define BSD_RING_BASE 0x04000

/* The pipeline's statistics, each the low half of a 64-bit counter whose
 * high half is 4 bytes above it. */
#define GPGPU_THREADS_DISPATCHED 0x2290
#define HS_INVOCATION_COUNT      0x2300
#define DS_INVOCATION_COUNT      0x2308
#define IA_VERTICES_COUNT        0x2310
#define IA_PRIMITIVES_COUNT      0x2318
#define VS_INVOCATION_COUNT      0x2320
#define GS_INVOCATION_COUNT      0x2328
#define GS_PRIMITIVES_COUNT      0x2330
#define CL_INVOCATION_COUNT      0x2338
#define CL_PRIMITIVES_COUNT      0x2340
#define PS_INVOCATION_COUNT      0x2348
#define PS_DEPTH_COUNT           0x2350

#define MI_PREDICATE_SRC0(base)                    ((base) + 0x400) /* 64-bit */
#define MI_PREDICATE_SRC1(base)                    ((base) + 0x408) /* 64-bit */
#define GEN7_3DPRIM_END_OFFSET                     0x2420
#define GEN7_3DPRIM_START_VERTEX                   0x2430
#define GEN7_3DPRIM_VERTEX_COUNT                   0x2434
#define GEN7_3DPRIM_INSTANCE_COUNT                 0x2438
#define GEN7_3DPRIM_START_INSTANCE                 0x243c
#define GEN7_3DPRIM_BASE_VERTEX                    0x2440
#define GEN7_GPGPU_DISPATCHDIMX                    0x2500
#define GEN7_GPGPU_DISPATCHDIMY                    0x2504
#define GEN7_GPGPU_DISPATCHDIMZ                    0x2508
#define GEN8_RING_CS_GPR(base, n)                  ((base) + 0x600 + (n) * 8)     /* 64-bit */
#define GEN7_SO_NUM_PRIMS_WRITTEN(n)               (0x5200u + (uint32_t)(n) * 8u) /* 64-bit */
#define GEN7_SO_PRIM_STORAGE_NEEDED(n)             (0x5240u + (uint32_t)(n) * 8u) /* 64-bit */
#define GEN7_L3SQCREG1                             0xb010
#define GEN7_L3CNTLREG2                            0xb020
#define GEN7_L3CNTLREG3                            0xb024
#define HSW_SCRATCH1_L3_DATA_ATOMICS_DISABLE       (1u << 27)
#define HSW_ROW_CHICKEN3                           0xe49c
#define HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE (1u << 6)

/* ------------------------------------------------------------------------
 * Reset (gt/intel_gt_regs.h): a domain's bit is set to reset it and reads
 * back clear when the reset is done.
 */

#define GEN6_GDRST        0x941c
#define GEN6_GRDOM_FULL   (1u << 0)
#define GEN6_GRDOM_RENDER (1u << 1)
#define GEN6_GRDOM_MEDIA  (1u << 2)
#define GEN6_GRDOM_BLT    (1u << 3)
#define GEN6_GRDOM_VECS   (1u << 4)

/* The error registers a hang's state reads and its recovery clears
 * (i915_reg.h, gt/intel_gt_regs.h): the GTT's errors, the render ring's
 * instruction parser's, the legacy identity register's error bit, and each
 * engine's fault register, whose valid bit is cleared. */
#define PGTBL_ER                    0x2024
#define IPEIR_I965                  0x2064
#define GEN2_IIR                    0x20a4
#define I915_MASTER_ERROR_INTERRUPT (1u << 15)
#define RING_FAULT_REG_RCS          0x4094
#define RING_FAULT_REG_BCS          0x4294
#define RING_FAULT_VALID            (1u << 0)

/* ------------------------------------------------------------------------
 * Fences and swizzling (i915_reg.h, gt/intel_gt_regs.h)
 */

#define I915_FENCE_REGS_HSW         32
#define FENCE_REG_GEN6_LO(i)        (0x100000 + (i) * 8)
#define FENCE_REG_GEN6_HI(i)        (0x100000 + (i) * 8 + 4)
#define GEN6_FENCE_PITCH_SHIFT      32 /* of the 64-bit pair */
#define GEN7_FENCE_MAX_PITCH_VAL    0x0800
#define I965_FENCE_TILING_Y_SHIFT   1
#define I965_FENCE_REG_VALID        (1u << 0)
#define TILECTL                     0x101000
#define TILECTL_SWZCTL              (1u << 0)
#define DISP_ARB_CTL                0x45000
#define DISP_TILE_SURFACE_SWIZZLING (1u << 13)
#define DISP_ARB_CTL2               0x45004

/* ------------------------------------------------------------------------
 * The global GTT's entries (gt/intel_gtt.h): one dword a page, with bits
 * 39:32 of the address in bits 10:4.
 */

#define GEN6_PTE_VALID                 (1u << 0)
#define HSW_CACHEABILITY_CONTROL(bits) ((((bits) & 0x7u) << 1) | (((bits) & 0x8u) << (11 - 3)))
#define HSW_WB_LLC_AGE3                HSW_CACHEABILITY_CONTROL(0x2u)
#define HSW_WB_LLC_AGE0                HSW_CACHEABILITY_CONTROL(0x3u)
#define HSW_WB_ELLC_LLC_AGE3           HSW_CACHEABILITY_CONTROL(0x8u)
#define HSW_WB_ELLC_LLC_AGE0           HSW_CACHEABILITY_CONTROL(0xbu)
#define HSW_WT_ELLC_LLC_AGE3           HSW_CACHEABILITY_CONTROL(0x7u)
#define HSW_WT_ELLC_LLC_AGE0           HSW_CACHEABILITY_CONTROL(0x6u)
#define HSW_PTE_UNCACHED               0u
#define HSW_GTT_ADDR_ENCODE(addr)      ((uint32_t)(addr) | ((uint32_t)((uint64_t)(addr) >> 28) & 0x7f0u))
/* A PPGTT directory entry names a table with bits 39:32 of its address in
 * bits 11:4. */
#define GEN6_PDE_VALID                 (1u << 0)
#define GEN6_PDE_ADDR_ENCODE(addr) \
    ((uint32_t)(addr) | ((uint32_t)((uint64_t)(addr) >> 28) & 0xff0u))
#define GFX_FLSH_CNTL_GEN6 0x101008 /* the GTT's cache */
#define GFX_FLSH_CNTL_EN   (1u << 0)
#define GEN6_PDE_SHIFT     22
#define GEN6_PDE_VALID     (1u << 0)

/* ------------------------------------------------------------------------
 * RC6, frequency and the pcode mailbox (i915_reg.h, gt/intel_gt_regs.h)
 */

#define GEN6_RPNSWREQ                                   0xa008
#define GEN6_TURBO_DISABLE                              (1u << 31)
#define HSW_FREQUENCY(x)                                ((uint32_t)(x) << 24)
#define HSW_FREQUENCY_SHIFT                             24
#define GEN6_RC_VIDEO_FREQ                              0xa00c
#define GEN6_RP_DOWN_TIMEOUT                            0xa010
#define GEN6_RP_INTERRUPT_LIMITS                        0xa014
#define GEN6_RPSTAT1                                    0xa01c
#define HSW_CAGF_SHIFT                                  7
#define HSW_CAGF_MASK                                   (0x7fu << 7) /* bits 13:7 */
#define GEN6_RP_CONTROL                                 0xa024
#define GEN6_RP_MEDIA_TURBO                             (1u << 11)
#define GEN6_RP_MEDIA_HW_NORMAL_MODE                    (2u << 9)
#define GEN6_RP_MEDIA_MODE_MASK                         (3u << 9) /* 0: software mode */
#define GEN6_RP_MEDIA_IS_GFX                            (1u << 8)
#define GEN6_RP_ENABLE                                  (1u << 7)
#define GEN6_RP_UP_BUSY_AVG                             (2u << 3)
#define GEN6_RP_DOWN_IDLE_AVG                           (2u << 0)
#define GEN6_RP_UP_THRESHOLD                            0xa02c
#define GEN6_RP_DOWN_THRESHOLD                          0xa030
#define GEN6_RP_CUR_UP_EI                               0xa050
#define GEN6_CURICONT_MASK                              0xffffffu /* of RP_CUR_ and RP_PREV_ */
#define GEN6_RP_CUR_UP                                  0xa054
#define GEN6_RP_PREV_UP                                 0xa058
#define GEN6_RP_CUR_DOWN_EI                             0xa05c
#define GEN6_RP_CUR_DOWN                                0xa060
#define GEN6_RP_PREV_DOWN                               0xa064
#define GEN6_RP_UP_EI                                   0xa068
#define GEN6_RP_DOWN_EI                                 0xa06c
#define GEN6_RP_IDLE_HYSTERSIS                          0xa070
#define GEN6_RC_CONTROL                                 0xa090
#define GEN6_RC_CTL_RC6pp_ENABLE                        (1u << 16)
#define GEN6_RC_CTL_RC6p_ENABLE                         (1u << 17)
#define GEN6_RC_CTL_RC6_ENABLE                          (1u << 18)
#define GEN6_RC_CTL_EI_MODE(x)                          ((uint32_t)(x) << 27)
#define GEN6_RC_CTL_HW_ENABLE                           (1u << 31)
#define GEN6_RC_STATE                                   0xa094
#define RC_SW_TARGET_STATE_SHIFT                        16
#define GEN6_RC1_WAKE_RATE_LIMIT                        0xa098
#define GEN6_RC6_WAKE_RATE_LIMIT                        0xa09c
#define GEN6_RC6pp_WAKE_RATE_LIMIT                      0xa0a0
#define GEN6_RC_EVALUATION_INTERVAL                     0xa0a8
#define GEN6_RC_IDLE_HYSTERSIS                          0xa0ac
#define GEN6_RC_SLEEP                                   0xa0b0
#define GEN6_RC1e_THRESHOLD                             0xa0b4
#define GEN6_RC6_THRESHOLD                              0xa0b8
#define GEN6_RC6p_THRESHOLD                             0xa0bc
#define GEN6_RC6pp_THRESHOLD                            0xa0c0
#define GEN6_PMINTRMSK                                  0xa168
#define GEN6_GT_GFX_RC6_LOCKED                          0x138104
#define GEN6_GT_GFX_RC6                                 0x138108
#define GEN6_GT_GFX_RC6p                                0x13810c
#define GEN6_GT_GFX_RC6pp                               0x138110
#define GEN6_PCODE_MAILBOX                              0x138124
#define GEN6_PCODE_READY                                (1u << 31)
#define GEN6_PCODE_ERROR_MASK                           0xffu
#define GEN6_PCODE_SUCCESS                              0x0
#define GEN6_PCODE_ILLEGAL_CMD                          0x1
#define GEN7_PCODE_TIMEOUT                              0x2
#define GEN7_PCODE_ILLEGAL_DATA                         0x3
#define GEN11_PCODE_ILLEGAL_SUBCOMMAND                  0x4
#define GEN11_PCODE_LOCKED                              0x6
#define GEN7_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE 0x10
#define GEN11_PCODE_REJECTED                            0x11
#define GEN6_PCODE_READ_RC6VIDS                         0x5
#define GEN6_PCODE_WRITE_MIN_FREQ_TABLE                 0x8
#define GEN6_PCODE_READ_MIN_FREQ_TABLE                  0x9
#define GEN6_READ_OC_PARAMS                             0xc
#define HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL            0x1a
#define GEN6_PCODE_DATA                                 0x138128
#define GEN6_PCODE_FREQ_IA_RATIO_SHIFT                  8
#define GEN6_PCODE_FREQ_RING_RATIO_SHIFT                16
#define GEN6_PCODE_DATA1                                0x13812c

/* ------------------------------------------------------------------------
 * Display power wells (i915_reg.h): four request registers, BIOS, driver,
 * KVMR and debug, each a request bit and a state bit per well; Haswell's
 * one well is index 15, the bits 31 and 30.
 */

#define HSW_PWR_WELL_CTL1           0x45400 /* BIOS */
#define HSW_PWR_WELL_CTL2           0x45404 /* driver */
#define HSW_PWR_WELL_CTL3           0x45408 /* KVMR */
#define HSW_PWR_WELL_CTL4           0x4540c /* debug */
#define HSW_PW_CTL_IDX_GLOBAL       15
#define HSW_PWR_WELL_CTL_REQ(idx)   (0x2u << ((idx) * 2))
#define HSW_PWR_WELL_CTL_STATE(idx) (0x1u << ((idx) * 2))
#define HSW_NDE_RSTWRN_OPT          0x46408

/* ------------------------------------------------------------------------
 * Pipes, transcoders, planes and cursors (i915_reg.h,
 * display/i9xx_plane_regs.h, display/intel_cursor_regs.h,
 * display/intel_display_device.c's offsets): pipes A to C are 0 to 2, and
 * the eDP transcoder, which only pipe A's panel uses, is 3.
 */

#define I915_PIPE_A    0
#define I915_PIPE_B    1
#define I915_PIPE_C    2
#define I915_TRANS_EDP 3

/* A transcoder's register at its A offset `a`: A, B and C 0x1000 apart and
 * the eDP transcoder at 0x6f000 in place of 0x60000. */
#define I915_TRANS_REG(trans, a) \
    ((trans) == I915_TRANS_EDP ? (a) - 0x60000 + 0x6f000 : (a) + (trans) * 0x1000)
/* A transcoder's configuration, in the pipe range 0x70000, with the same
 * spacing. */
#define I915_TRANS2_REG(trans, a) \
    ((trans) == I915_TRANS_EDP ? (a) - 0x70000 + 0x7f000 : (a) + (trans) * 0x1000)

#define TRANS_HTOTAL(t)               I915_TRANS_REG(t, 0x60000)
#define TRANS_HBLANK(t)               I915_TRANS_REG(t, 0x60004)
#define TRANS_HSYNC(t)                I915_TRANS_REG(t, 0x60008)
#define TRANS_VTOTAL(t)               I915_TRANS_REG(t, 0x6000c)
#define TRANS_VBLANK(t)               I915_TRANS_REG(t, 0x60010)
#define TRANS_VSYNC(t)                I915_TRANS_REG(t, 0x60014)
#define TRANS_VSYNCSHIFT(t)           I915_TRANS_REG(t, 0x60028)
#define PIPESRC(p)                    (0x6001c + (p) * 0x1000)
#define TRANSCONF(t)                  I915_TRANS2_REG(t, 0x70008)
#define TRANSCONF_ENABLE              (1u << 31)
#define TRANSCONF_STATE_ENABLE        (1u << 30)
#define TRANSCONF_INTERLACE_MASK      (7u << 21)
#define TRANSCONF_INTERLACE_PF_PD_ILK (0u << 21)
#define TRANSCONF_DITHER_EN           (1u << 4)
#define TRANSCONF_DITHER_TYPE_SP      (0u << 2)
#define PIPE_MISC(p)                  (0x70030 + (p) * 0x1000)
#define PIPE_MISC_BPC_MASK            (7u << 5)
#define PIPE_MISC_BPC_8               (0u << 5)
#define PIPE_MISC_BPC_10              (1u << 5)
#define PIPE_MISC_BPC_6               (2u << 5)
#define PIPE_MISC_DITHER_ENABLE       (1u << 4)
#define PIPE_FRMCOUNT_G4X(p)          (0x70040 + (p) * 0x1000)
#define PIPE_FLIPCOUNT_G4X(p)         (0x70044 + (p) * 0x1000)

/* A transcoder's DisplayPort M/N values: the data's, with the transfer
 * unit's size in DATA_M1's bits 30:25, and the link's, which give the
 * pixel clock as M/N of the link's symbol clock. */
#define PIPE_DATA_M1(t) I915_TRANS_REG(t, 0x60030)
#define TU_SIZE_SHIFT   25
#define PIPE_DATA_N1(t) I915_TRANS_REG(t, 0x60034)
#define PIPE_LINK_M1(t) I915_TRANS_REG(t, 0x60040)
#define PIPE_LINK_N1(t) I915_TRANS_REG(t, 0x60044)
#define PIPE_DATA_M2(t) I915_TRANS_REG(t, 0x60038)
#define PIPE_DATA_N2(t) I915_TRANS_REG(t, 0x6003c)
#define PIPE_LINK_M2(t) I915_TRANS_REG(t, 0x60048)
#define PIPE_LINK_N2(t) I915_TRANS_REG(t, 0x6004c)
#define TU_SIZE(x)      (((uint32_t)(x) - 1u) << TU_SIZE_SHIFT)

/* The main stream attributes a DisplayPort transcoder sends: the clocks
 * synchronous, and the colour depth in bits 7:5, 0 for 6 bits. */
#define TRANS_MSA_MISC(t)   I915_TRANS2_REG(t, 0x60410)
#define TRANS_MSA_SYNC_CLK  (1u << 0)
#define TRANS_MSA_BPC_SHIFT 5

/* A transcoder's chicken bits, eDP's at 0x420cc, and its frame start delay
 * in bits 28:27, the delay less one. */
#define CHICKEN_TRANS(t)           ((t) == I915_TRANS_EDP ? 0x420cc : 0x420c0 + (t) * 4)
#define HSW_FRAME_START_DELAY_MASK (3u << 27)
#define HSW_FRAME_START_DELAY(x)   ((uint32_t)(x) << 27)
#define DATA_LINK_M_N_MASK         0xffffffu

/* The legacy VGA plane, which scans out the VGA's text and modes in place
 * of the primary plane while bit 31 is clear. */
#define CPU_VGACNTRL     0x41000
#define VGA_DISP_DISABLE (1u << 31)

#define DSPCNTR(p)                (0x70180 + (p) * 0x1000)
#define DISP_ENABLE               (1u << 31)
#define DISP_PIPE_GAMMA_ENABLE    (1u << 30)
#define DISP_FORMAT_MASK          (0xfu << 26)
#define DISP_FORMAT_8BPP          (2u << 26)
#define DISP_FORMAT_BGRX565       (5u << 26)
#define DISP_FORMAT_BGRX888       (6u << 26)
#define DISP_FORMAT_RGBX101010    (8u << 26)
#define DISP_FORMAT_BGRX101010    (10u << 26)
#define DISP_FORMAT_RGBX888       (14u << 26)
#define DISP_FORMAT_RGBX161616    (12u << 26)
#define DISP_PIPE_CSC_ENABLE      (1u << 24)
#define DISP_ROTATE_180           (1u << 15)
#define DISP_TRICKLE_FEED_DISABLE (1u << 14)
#define DISP_TILED                (1u << 10)
#define DISP_ASYNC_FLIP           (1u << 9)
#define DSPSTRIDE(p)              (0x70188 + (p) * 0x1000)
#define DSPSURF(p)                (0x7019c + (p) * 0x1000)
#define DISP_ADDR_MASK            0xfffff000u
#define DSPOFFSET(p)              (0x701a4 + (p) * 0x1000) /* y << 16 | x */
#define DSPSURFLIVE(p)            (0x701ac + (p) * 0x1000)

#define CURCNTR(p)                (0x70080 + (p) * 0x1000)
#define MCURSOR_MODE_MASK         0x3fu
#define MCURSOR_MODE_DISABLE      0x00u
#define MCURSOR_MODE_128_ARGB_AX  0x22u
#define MCURSOR_MODE_256_ARGB_AX  0x23u
#define MCURSOR_MODE_64_ARGB_AX   0x27u
#define MCURSOR_PIPE_GAMMA_ENABLE (1u << 26)
#define MCURSOR_PIPE_CSC_ENABLE   (1u << 24)
#define MCURSOR_ROTATE_180        (1u << 15)
#define CURBASE(p)                (0x70084 + (p) * 0x1000)
#define CURPOS(p)                 (0x70088 + (p) * 0x1000)
#define CURSOR_POS_Y_SIGN         (1u << 31)
#define CURSOR_POS_X_SIGN         (1u << 15)
#define CURSOR_POS_MASK           0x7fffu
#define CURSURFLIVE(p)            (0x700ac + (p) * 0x1000)

/* The sprite planes of Ivy Bridge and Haswell
 * (display/intel_sprite_regs.h), one a pipe. */
#define SPRCTL(p)                          (0x70280 + (p) * 0x1000)
#define SPRITE_ENABLE                      (1u << 31)
#define SPRITE_PIPE_GAMMA_ENABLE           (1u << 30)
#define SPRITE_FORMAT_YUV422               (0u << 25)
#define SPRITE_FORMAT_RGBX101010           (1u << 25)
#define SPRITE_FORMAT_RGBX888              (2u << 25)
#define SPRITE_FORMAT_RGBX161616           (3u << 25)
#define SPRITE_PIPE_CSC_ENABLE             (1u << 24)
#define SPRITE_RGB_ORDER_RGBX              (1u << 20)
#define SPRITE_YUV_TO_RGB_CSC_FORMAT_BT709 (1u << 18)
#define SPRITE_YUV_ORDER_YUYV              (0u << 16)
#define SPRITE_YUV_ORDER_YVYU              (1u << 16)
#define SPRITE_YUV_ORDER_UYVY              (2u << 16)
#define SPRITE_YUV_ORDER_VYUY              (3u << 16)
#define SPRITE_ROTATE_180                  (1u << 15)
#define SPRITE_PLANE_GAMMA_DISABLE         (1u << 13)
#define SPRITE_TILED                       (1u << 10)
#define SPRSTRIDE(p)                       (0x70288 + (p) * 0x1000)
#define SPRPOS(p)                          (0x7028c + (p) * 0x1000)
#define SPRSIZE(p)                         (0x70290 + (p) * 0x1000)
#define SPRKEYVAL(p)                       (0x70294 + (p) * 0x1000)
#define SPRKEYMSK(p)                       (0x70298 + (p) * 0x1000)
#define SPRSURF(p)                         (0x7029c + (p) * 0x1000)
#define SPRKEYMAX(p)                       (0x702a0 + (p) * 0x1000)
#define SPROFFSET(p)                       (0x702a4 + (p) * 0x1000) /* y << 16 | x */
#define SPRSURFLIVE(p)                     (0x702ac + (p) * 0x1000)
#define SPRSCALE(p)                        (0x70304 + (p) * 0x1000)

/* The pipe CRC (display/intel_pipe_crc_regs.h): one result on Haswell. */
#define PIPE_CRC_CTL(p)             (0x60050 + (p) * 0x1000)
#define PIPE_CRC_ENABLE             (1u << 31)
#define PIPE_CRC_SOURCE_MASK_IVB    (3u << 29)
#define PIPE_CRC_SOURCE_PRIMARY_IVB (0u << 29)
#define PIPE_CRC_SOURCE_SPRITE_IVB  (1u << 29)
#define PIPE_CRC_SOURCE_PF_IVB      (2u << 29)
#define PIPE_CRC_EXP_HSW(p)         (0x60054 + (p) * 0x1000)
#define PIPE_CRC_RES_HSW(p)         (0x60064 + (p) * 0x1000)

/* The panel fitters: three, 0x800 apart. */
#define PF_WIN_POS(i)         (0x68070 + (i) * 0x800)
#define PF_WIN_SZ(i)          (0x68074 + (i) * 0x800)
#define PF_CTL(i)             (0x68080 + (i) * 0x800)
#define PF_ENABLE             (1u << 31)
#define PF_PIPE_SEL_IVB(pipe) ((uint32_t)(pipe) << 29)
#define PF_FILTER_MED_3x3     (1u << 23)
#define PF_WIN_XPOS(x)        ((uint32_t)(x) << 16)
#define PF_WIN_YPOS(y)        ((uint32_t)(y))
#define PF_WIN_XSIZE(w)       ((uint32_t)(w) << 16)
#define PF_WIN_YSIZE(h)       ((uint32_t)(h))

/* Watermarks: pipe C's level 0 is apart from A's and B's. */
#define WM0_PIPE_ILK(p)            ((p) == I915_PIPE_C ? 0x45200 : 0x45100 + (p) * 4)
#define WM1_LP_ILK                 0x45108
#define WM2_LP_ILK                 0x4510c
#define WM3_LP_ILK                 0x45110
#define WM1S_LP_ILK                0x45120
#define WM2S_LP_IVB                0x45124
#define WM3S_LP_IVB                0x45128
#define WM_MISC                    0x45260
#define WM_MISC_DATA_PARTITION_5_6 (1u << 0)
#define WM0_PIPE_PRIMARY_SHIFT     16
#define WM0_PIPE_SPRITE_SHIFT      8
#define WM_LP_ENABLE               (1u << 31)
#define WM_LP_LATENCY_SHIFT        24
#define WM_LP_FBC_ILK_SHIFT        20
#define WM_LP_PRIMARY_SHIFT        8
#define DISP_FBC_WM_DIS            (1u << 15) /* of DISP_ARB_CTL */

/* A pipe's line time in eighths of a microsecond, at the pixel clock in
 * bits 8:0 and at the CD clock, for IPS, in bits 24:16. */
/* A pipe's gamma mode, 8 bits through the legacy table when 0, and its
 * colour matrix's mode, which sits before the gamma with CSC_POSITION. */
#define GAMMA_MODE(p)             (0x4a480 + (p) * 0x800)
#define GAMMA_MODE_MODE_8BIT      0u
#define PIPE_CSC_MODE(p)          (0x49028 + (p) * 0x100)
#define CSC_POSITION_BEFORE_GAMMA (1u << 1)

#define WM_LINETIME(p)         (0x45270 + (p) * 4)
#define HSW_LINETIME_MASK      0x1ffu
#define HSW_IPS_LINETIME_SHIFT 16

/* The memory latencies of the watermark levels, which the pcode leaves in
 * the MCHBAR's mirror (intel_mchbar_regs.h): level 0 in 0.1 us in bits
 * 63:56, or 3:0 when those are 0, levels 1 to 4 in 0.5 us in bits 11:4,
 * 19:12, 28:20 and 40:32. Read as two dwords. */
#define MCH_SSKPD    0x145d10
#define MCH_SSKPD_HI 0x145d14

/* The fuses: the CD clock held to 450 MHz. */
#define FUSE_STRAP      0x42014
#define HSW_CDCLK_LIMIT (1u << 24)

/* ------------------------------------------------------------------------
 * DDI, its clocks and PLLs (i915_reg.h): ports A to E are 0 to 4.
 */

#define DDI_BUF_CTL(port)               (0x64000 + (port) * 0x100)
#define DDI_BUF_CTL_ENABLE              (1u << 31)
#define DDI_BUF_TRANS_SELECT(n)         ((uint32_t)(n) << 24)
#define DDI_BUF_EMP_MASK                (0xfu << 24)
#define DDI_BUF_PORT_REVERSAL           (1u << 16)
#define DDI_BUF_IS_IDLE                 (1u << 7)
#define DDI_A_4_LANES                   (1u << 4)
#define DDI_PORT_WIDTH(width)           (((uint32_t)(width) - 1) << 1)
#define DDI_PORT_WIDTH_MASK             (7u << 1)
#define DDI_INIT_DISPLAY_DETECTED       (1u << 0)
#define DDI_BUF_TRANS(port)             (0x64e00 + (port) * 0x60)
#define DDI_BUF_TRANS_LO(port, i)       (DDI_BUF_TRANS(port) + (i) * 8)
#define DDI_BUF_TRANS_HI(port, i)       (DDI_BUF_TRANS(port) + (i) * 8 + 4)
#define DP_TP_CTL(port)                 (0x64040 + (port) * 0x100)
#define DP_TP_CTL_ENABLE                (1u << 31)
#define DP_TP_CTL_MODE_SST              (0u << 27)
#define DP_TP_CTL_ENHANCED_FRAME_ENABLE (1u << 18)
#define DP_TP_CTL_LINK_TRAIN_MASK       (7u << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT1       (0u << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT2       (1u << 8)
#define DP_TP_CTL_LINK_TRAIN_IDLE       (2u << 8)
#define DP_TP_CTL_LINK_TRAIN_NORMAL     (3u << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT3       (4u << 8)
#define DP_TP_STATUS(port)              (0x64044 + (port) * 0x100)
#define DP_TP_STATUS_IDLE_DONE          (1u << 25)
#define TRANS_DDI_FUNC_CTL(t)           I915_TRANS_REG(t, 0x60400)
#define TRANS_DDI_FUNC_ENABLE           (1u << 31)
#define TRANS_DDI_PORT_SHIFT            28
#define TRANS_DDI_PORT_MASK             (7u << 28)
#define TRANS_DDI_SELECT_PORT(x)        ((uint32_t)(x) << 28)
#define TRANS_DDI_MODE_SELECT_MASK      (7u << 24)
#define TRANS_DDI_MODE_SELECT_HDMI      (0u << 24)
#define TRANS_DDI_MODE_SELECT_DVI       (1u << 24)
#define TRANS_DDI_MODE_SELECT_DP_SST    (2u << 24)
#define TRANS_DDI_BPC_MASK              (7u << 20)
#define TRANS_DDI_BPC_8                 (0u << 20)
#define TRANS_DDI_BPC_10                (1u << 20)
#define TRANS_DDI_BPC_6                 (2u << 20)
#define TRANS_DDI_BPC_12                (3u << 20)
#define TRANS_DDI_PVSYNC                (1u << 17)
#define TRANS_DDI_PHSYNC                (1u << 16)
#define TRANS_DDI_EDP_INPUT_MASK        (7u << 12)
#define TRANS_DDI_EDP_INPUT_A_ON        (0u << 12)
#define TRANS_DDI_EDP_INPUT_A_ONOFF     (4u << 12)
#define TRANS_DDI_EDP_INPUT_B_ONOFF     (5u << 12)
#define TRANS_DDI_EDP_INPUT_C_ONOFF     (6u << 12)
#define TRANS_DDI_PORT_WIDTH_MASK       (7u << 1)
#define TRANS_DDI_PORT_WIDTH(w)         (((uint32_t)(w) - 1u) << 1)

#define SPLL_CTL                0x46020
#define SPLL_PLL_ENABLE         (1u << 31)
#define WRPLL_CTL(pll)          (0x46040 + (pll) * 0x20)
#define WRPLL_PLL_ENABLE        (1u << 31)
#define PORT_CLK_SEL(port)      (0x46100 + (port) * 4)
#define PORT_CLK_SEL_MASK       (7u << 29)
#define PORT_CLK_SEL_LCPLL_2700 (0u << 29)
#define PORT_CLK_SEL_LCPLL_1350 (1u << 29)
#define PORT_CLK_SEL_LCPLL_810  (2u << 29)
#define PORT_CLK_SEL_SPLL       (3u << 29)
#define PORT_CLK_SEL_WRPLL1     (4u << 29)
#define PORT_CLK_SEL_WRPLL2     (5u << 29)
#define PORT_CLK_SEL_NONE       (7u << 29)
#define TRANS_CLK_SEL(t)        (0x46140 + (t) * 4)
#define TRANS_CLK_SEL_DISABLED  (0u << 29)
#define TRANS_CLK_SEL_PORT(x)   (((uint32_t)(x) + 1) << 29)

/* The CD clock's source. */
#define LCPLL_CTL                 0x130040
#define LCPLL_PLL_DISABLE         (1u << 31)
#define LCPLL_PLL_LOCK            (1u << 30)
#define LCPLL_CLK_FREQ_MASK       (3u << 26)
#define LCPLL_CLK_FREQ_450        (0u << 26)
#define LCPLL_CD_CLOCK_DISABLE    (1u << 25)
#define LCPLL_POWER_DOWN_ALLOW    (1u << 22)
#define LCPLL_CD_SOURCE_FCLK      (1u << 21)
#define LCPLL_CD_SOURCE_FCLK_DONE (1u << 19)

#define SFUSE_STRAP 0xc2014

/* The PCH's raw clock in MHz, bits 9:0, which the panel power sequencer's
 * divisor counts. */
#define PCH_RAWCLK_FREQ  0xc6204
#define RAWCLK_FREQ_MASK 0x3ffu

/* The HDMI infoframes of transcoder `t` (i915_reg.h). */
#define HSW_TVIDEO_DIP_CTL(t)      I915_TRANS_REG(t, 0x60200)
#define HSW_TVIDEO_DIP_AVI_DATA(t) I915_TRANS_REG(t, 0x60220)
#define VIDEO_DIP_ENABLE_VSC_HSW   (1u << 20)
#define VIDEO_DIP_ENABLE_GCP_HSW   (1u << 16)
#define VIDEO_DIP_ENABLE_AVI_HSW   (1u << 12)
#define VIDEO_DIP_ENABLE_VS_HSW    (1u << 8)
#define VIDEO_DIP_ENABLE_GMP_HSW   (1u << 4)
#define VIDEO_DIP_ENABLE_SPD_HSW   (1u << 0)
#define VIDEO_DIP_ENABLE_DRM_GLK   (1u << 28)

/* ------------------------------------------------------------------------
 * The panel power sequencer and backlight (display/intel_pps_regs.h,
 * display/intel_backlight_regs.h), in the PCH's range on Haswell.
 */

#define PCH_PPS_BASE                 0xc7200
#define PP_STATUS                    (PCH_PPS_BASE + 0x00)
#define PP_ON                        (1u << 31)
#define PP_READY                     (1u << 30)
#define PP_SEQUENCE_MASK             (3u << 28)
#define PP_SEQUENCE_NONE             (0u << 28)
#define PP_SEQUENCE_POWER_UP         (1u << 28)
#define PP_SEQUENCE_POWER_DOWN       (2u << 28)
#define PP_CYCLE_DELAY_ACTIVE        (1u << 27)
#define PP_SEQUENCE_STATE_MASK       0xfu
#define PP_SEQUENCE_STATE_OFF_IDLE   0x0u
#define PP_SEQUENCE_STATE_ON_IDLE    0x8u
#define PP_CONTROL                   (PCH_PPS_BASE + 0x04)
#define PANEL_UNLOCK_MASK            0xffff0000u
#define PANEL_UNLOCK_REGS            0xabcd0000u
#define EDP_FORCE_VDD                (1u << 3)
#define EDP_BLC_ENABLE               (1u << 2)
#define PANEL_POWER_RESET            (1u << 1)
#define PANEL_POWER_ON               (1u << 0)
#define PP_ON_DELAYS                 (PCH_PPS_BASE + 0x08)
#define PP_OFF_DELAYS                (PCH_PPS_BASE + 0x0c)
#define PP_DIVISOR                   (PCH_PPS_BASE + 0x10)
/* The delays' fields, in 100 us units: power up and light on in
 * PP_ON_DELAYS, power down and light off in PP_OFF_DELAYS; the reference
 * divider and the power cycle delay, in 100 ms units plus one, in
 * PP_DIVISOR. */
#define PANEL_POWER_UP_DELAY_SHIFT   16
#define PANEL_POWER_DOWN_DELAY_SHIFT 16
#define PANEL_DELAY_FIELD_MASK       0x1fffu
#define PP_REFERENCE_DIVIDER_SHIFT   8
#define PANEL_POWER_CYCLE_DELAY_MASK 0x1fu

#define BLC_PWM_CPU_CTL2          0x48250
#define BLM_PWM_ENABLE            (1u << 31)
#define BLC_PWM_CPU_CTL           0x48254
#define UTIL_PIN_CTL              0x48400
#define BLC_PWM_PCH_CTL1          0xc8250
#define BLM_PCH_PWM_ENABLE        (1u << 31)
#define BLM_PCH_OVERRIDE_ENABLE   (1u << 30)
#define BLM_PCH_POLARITY          (1u << 29)
#define BLC_PWM_PCH_CTL2          0xc8254
#define BACKLIGHT_DUTY_CYCLE_MASK 0xffffu
/* The PCH's PWM counts in steps of 128 of its clock with this bit, else
 * of 16. */
#define SOUTH_CHICKEN2            0xc2004
#define LPT_PWM_GRANULARITY       (1u << 5)

/* ------------------------------------------------------------------------
 * DisplayPort AUX (display/intel_dp_aux_regs.h): port A's channel is the
 * CPU's, B to D the PCH's; each has a control and five data registers.
 */

#define DP_AUX_CH_CTL(port)             ((port) == 0 ? 0x64010 : 0xe4110 + ((port) - 1) * 0x100)
#define DP_AUX_CH_DATA(port, i)         (DP_AUX_CH_CTL(port) + 4 + (i) * 4)
#define DP_AUX_CH_CTL_SEND_BUSY         (1u << 31)
#define DP_AUX_CH_CTL_DONE              (1u << 30)
#define DP_AUX_CH_CTL_INTERRUPT         (1u << 29)
#define DP_AUX_CH_CTL_TIME_OUT_ERROR    (1u << 28)
#define DP_AUX_CH_CTL_TIME_OUT_MASK     (3u << 26)
#define DP_AUX_CH_CTL_TIME_OUT_400us    (0u << 26)
#define DP_AUX_CH_CTL_TIME_OUT_600us    (1u << 26)
#define DP_AUX_CH_CTL_TIME_OUT_800us    (2u << 26)
#define DP_AUX_CH_CTL_RECEIVE_ERROR     (1u << 25)
#define DP_AUX_CH_CTL_MESSAGE_SIZE_MASK (0x1fu << 20)
#define DP_AUX_CH_CTL_MESSAGE_SIZE(x)   ((uint32_t)(x) << 20)
#define DP_AUX_CH_CTL_PRECHARGE_2US(x)  ((uint32_t)(x) << 16)
#define DP_AUX_CH_CTL_BIT_CLOCK_2X_MASK 0x7ffu

/* ------------------------------------------------------------------------
 * GMBUS and the GPIO pins (display/intel_gmbus_regs.h), at the PCH's
 * display base 0xc0000.
 */

#define PCH_DISPLAY_BASE        0xc0000
#define GPIO(pin)               (PCH_DISPLAY_BASE + 0x5010 + 4 * (pin))
#define GPIO_CLOCK_DIR_MASK     (1u << 0)
#define GPIO_CLOCK_DIR_OUT      (1u << 1)
#define GPIO_CLOCK_VAL_MASK     (1u << 2)
#define GPIO_CLOCK_VAL_OUT      (1u << 3)
#define GPIO_CLOCK_VAL_IN       (1u << 4)
#define GPIO_DATA_DIR_MASK      (1u << 8)
#define GPIO_DATA_DIR_OUT       (1u << 9)
#define GPIO_DATA_VAL_MASK      (1u << 10)
#define GPIO_DATA_VAL_OUT       (1u << 11)
#define GPIO_DATA_VAL_IN        (1u << 12)
#define GMBUS0                  (PCH_DISPLAY_BASE + 0x5100)
#define GMBUS_RATE_100KHZ       (0u << 8)
#define GMBUS_HOLD_EXT          (1u << 7)
#define GMBUS1                  (PCH_DISPLAY_BASE + 0x5104)
#define GMBUS_SW_CLR_INT        (1u << 31)
#define GMBUS_SW_RDY            (1u << 30)
#define GMBUS_ENT               (1u << 29)
#define GMBUS_CYCLE_NONE        (0u << 25)
#define GMBUS_CYCLE_WAIT        (1u << 25)
#define GMBUS_CYCLE_INDEX       (2u << 25)
#define GMBUS_CYCLE_STOP        (4u << 25)
#define GMBUS_BYTE_COUNT_SHIFT  16
#define GMBUS_BYTE_COUNT_MAX    256u
#define GMBUS_SLAVE_INDEX_SHIFT 8
#define GMBUS_SLAVE_ADDR_SHIFT  1
#define GMBUS_SLAVE_READ        (1u << 0)
#define GMBUS2                  (PCH_DISPLAY_BASE + 0x5108)
#define GMBUS_INUSE             (1u << 15)
#define GMBUS_HW_WAIT_PHASE     (1u << 14)
#define GMBUS_STALL_TIMEOUT     (1u << 13)
#define GMBUS_INT               (1u << 12)
#define GMBUS_HW_RDY            (1u << 11)
#define GMBUS_SATOER            (1u << 10)
#define GMBUS_ACTIVE            (1u << 9)
#define GMBUS3                  (PCH_DISPLAY_BASE + 0x510c)
#define GMBUS4                  (PCH_DISPLAY_BASE + 0x5110)
#define GMBUS5                  (PCH_DISPLAY_BASE + 0x5120)

#endif /* _I915_REG_H_ */
