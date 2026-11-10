/*
 * i915_fake.c: the fake chip, a backend behind i915_hw.h for tests in QEMU,
 * where no Intel GPU is: it answers as the Lenovo's HD 4400, runs every
 * command the moment the driver hands it over, and injects faults.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../fs/debugfs.h"
#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"
#include "../../mm/usercopy.h"
#include "../../udelay.h"

/*
 * What the fake is. A PCI function of QEMU's `edu` device stands in for
 * the chip's: the driver binds that function, through i915_driver_bind,
 * with the fake as its backend, and the fake answers for the chip's
 * identity in configuration space, 8086:0a16 revision 0b on a board of
 * 17aa:380c, which /sys then reports for the function, so that libdrm and
 * Mesa's loader find a Haswell GT2. Its GMCH control word gives a GTT of
 * 2 MiB of entries and no stolen memory, and MSAC an aperture of 256 MiB,
 * which nothing behind it backs. The function's own command word, BARs and
 * MSI capability stay edu's, so the device's message is a real one: the
 * fake raises it by a write to edu's interrupt register, which sends the
 * MSI the driver enabled on edu.
 *
 * Registers. BAR 0's registers are a file of 2 MiB in memory. A register
 * holds what was last written but where the chip does more: the masked
 * registers (FORCEWAKE_MT, each ring's MI_MODE, INSTPM and RING_MODE) take
 * the bits their high half enables; forcewake's acknowledgement follows
 * its request at once; the GT FIFO always has 64 free entries; a power
 * well's state follows its requests at once; INSTPM's flush is done as it
 * is asked for; the identity and error registers clear the bits written;
 * the status registers take nothing; GEN6_GDRST resets the engines
 * whose domains it names and reads 0 again at once; GEN6_RP_STATE_CAP
 * gives the Lenovo's limits, and a request in GEN6_RPNSWREQ is run within
 * them at once and shown in GEN6_RPSTAT1; and the pcode mailbox finishes
 * each command as it is posted, with the Lenovo's RPe for the duty cycle,
 * 0 for the overclocking parameters and the RC6 voltages, the frequency
 * table taken, and any other command refused as an illegal one. The
 * GTT's entries are memory of the size the GMCH control word gives.
 *
 * The aperture. Nothing backs BAR 2, so the fake gives each page of the
 * aperture that is reached a shadow page of its own (`aperture_page`),
 * filled from the memory the GTT's entry maps, or, inside a fence, from
 * where the fence's tiling and the controller's swizzle put each byte, as
 * the chip's detiler does; the controller swizzles as the chip's on the
 * Lenovo, bits 9 and 10 for X and 9 for Y, while its channels' DIMMs
 * (MAD_DIMM_C0 and C1) are of one size. A shadow page remembers the entry
 * and the fence it was filled under and is filled again when either has
 * changed. The driver moves the bytes between a shadow and memory where
 * the chip's aperture would have (`aperture_sync`): to memory once writes
 * through it are flushed, and from memory when the object moves into the
 * aperture's domain; a shadow whose entry or fence has changed since it
 * was filled is not written back.
 *
 * Rings. A ring runs from its head to its tail whenever the driver writes
 * its tail, its control or its MI_MODE, while it is valid, not stopped and
 * not hung, and its head reaches the tail before the write returns. Each
 * command is read through the GTT's entries from the memory they map and
 * done: MI_STORE_DWORD_IMM, MI_STORE_DWORD_INDEX, and the post-sync writes
 * of MI_FLUSH_DW and PIPE_CONTROL store their values, which is how the
 * sequence numbers reach the status page; MI_USER_INTERRUPT and
 * PIPE_CONTROL's notify raise the ring's user interrupt;
 * MI_BATCH_BUFFER_START completes the batch at once, without reading it;
 * every other command of the MI, 2D and 3D types is passed over by its
 * length, and a command of no known type hangs the ring, as does a
 * command that cannot be read through the GTT. MI_MODE reads MODE_IDLE
 * while the ring is stopped and has nothing left to run.
 *
 * Interrupts. An event latches in its bank's identity register when the
 * bank's mask lets it through, and a user interrupt only when the ring's
 * RING_IMR does too. The device's line is the master enable and any
 * identity bit that is enabled, the south display's behind the PCH event,
 * and each rise of the line raises one message.
 *
 * Faults, each armed for a number of times or for always through the
 * device's debugfs file (`fake_file_write`): `submit` fails the next
 * submission with -EIO and `bind` the next bind into the GTT with -ENOSPC,
 * at the driver's fault points (I915_FAULT_*); `hang` hangs the ring at
 * the next command it would run, with its head there, ACTHD at it and
 * IPEHR its first dword, until a reset of its domain; `irq` loses the next
 * user interrupt, whose breadcrumb is still written. Each injection is
 * counted and logged as `i915: fake: injected: NAME: detail`.
 *
 * Locking. The fake's lock is taken, with interrupts off, around each
 * access the driver makes, so that the handler, on any processor, sees a
 * register file between accesses; it is innermost, and the message is
 * raised after it is dropped.
 */

/* QEMU's edu device, whose function the fake stands on, and the register
 * of its BAR 0 a write to which raises its interrupt. */
#define FAKE_EDU_VENDOR 0x1234
#define FAKE_EDU_DEVICE 0x11e8
#define FAKE_EDU_RAISE  0x60
#define FAKE_EDU_BYTES  0x1000u

/* The identity the fake answers with, the Lenovo's (i915_lenovo.h). */
#define FAKE_ID_DWORD       0x0a168086u
#define FAKE_CLASS_REVISION 0x0300000bu /* VGA-compatible display, revision 0b */
#define FAKE_SUBSYSTEM      0x380c17aau
#define FAKE_GMCH_CTRL      (2u << SNB_GMCH_GGMS_SHIFT) /* 2 MiB of GTT, no stolen memory */
#define FAKE_MSAC           (1u << HSW_MSAC_SIZE_SHIFT) /* a 256 MiB aperture */

/* The GT's frequency limits, the Lenovo's (i915_model_rc6.h): RPn 4, RP1 4
 * and RP0 20, in units of 50 MHz; and its efficient frequency RPe 10, in
 * bits 15:8 of the pcode's duty cycle answer. */
#define FAKE_RP_STATE_CAP 0x00040414u
#define FAKE_PCODE_DDCC   0x00000a00u

/* The GT FIFO's free entries, always. */
#define FAKE_FIFO_ENTRIES 64u

/* The faults, the first two at the driver's fault points by their
 * I915_FAULT_ numbers. */
#define FAKE_SUBMIT  I915_FAULT_SUBMIT
#define FAKE_BIND    I915_FAULT_BIND
#define FAKE_HANG    2
#define FAKE_IRQ     3
#define FAKE_NFAULTS 4

/* An armed count that never runs out. */
#define FAKE_ALWAYS 0xffffffffu

/* Commands the rings do, beyond i915_reg.h's (gt/intel_gpu_commands.h). */
#define FAKE_MI_BATCH_BUFFER_START 0x31u
#define FAKE_MI_FLUSH_DW           0x26u
#define FAKE_MI_LOAD_REGISTER_IMM  0x22u
#define FAKE_POST_SYNC_MASK        (3u << 14) /* of MI_FLUSH_DW and PIPE_CONTROL's flags */
#define FAKE_POST_SYNC_WRITE       (1u << 14)
#define FAKE_STORE_INDEX           (1u << 21) /* the address is a status page offset */
#define FAKE_PIPE_CONTROL          0x7a000000u
#define FAKE_PIPE_CONTROL_NOTIFY   (1u << 8)
#define FAKE_PIPELINE_SELECT       0x69040000u

/* The longest command whose dwords the fake reads. */
#define FAKE_CMD_MAX 8u

static const char *const fake_fault_names[FAKE_NFAULTS] = {"submit", "bind", "hang", "irq"};

/* The aperture's pages, 256 MiB of them, and the shadow pages that stand
 * in for them, in directories of FAKE_SHADOW_DIR made as they are used. */
#define FAKE_APERTURE_PAGES (0x10000000u >> 12)
#define FAKE_SHADOW_DIR     512u
#define FAKE_SHADOW_DIRS    (FAKE_APERTURE_PAGES / FAKE_SHADOW_DIR)

/* The bytes the detiler moves together: a run of a Y tile's row, inside
 * which neither the tiling nor the swizzle moves a byte. */
#define FAKE_DETILE_RUN 16u

/* The shadow of one page of the aperture: `phys` its frame, 0 until it is
 * first reached, a frame of the allocator's and not of the kernel's heap,
 * since a full aperture is 256 MiB; `pte` the GTT's entry and `fence_lo`
 * and `fence_hi` the fence over it when it was last filled, 0 for none. */
struct fake_shadow {
    uintptr_t phys;
    uint32_t pte;
    uint32_t fence_lo;
    uint32_t fence_hi;
};

/* One of the two rings the driver starts. `hung` is set from a hang until
 * a reset of `domain`; `commands` and `batches` count what it ran, and
 * `resets` the resets of its domain alone. */
struct fake_ring {
    const char *name;
    uint32_t base;
    uint32_t hws_pga;
    uint32_t irq_bit;
    uint32_t domain;
    uint8_t hung;
    uint32_t commands;
    uint32_t batches;
    uint32_t resets;
};

/*
 * The fake chip. `regs` is BAR 0's registers and `gtt` the GTT's
 * `gtt_entries` entries. `armed` is how many more times each fault is
 * injected, FAKE_ALWAYS for always, and `injected` how many times it was.
 * `line` is the device's interrupt line and `messages` counts its rises;
 * `raise_pending` is set by a rise until the access that made it ends.
 * `raise` sends the message: the write to edu's `doorbell` in the kernel,
 * the test's own in a host test. `i915` is the device once registered,
 * and `light` first light's last result, or 1 before it ran. `shadow`
 * holds the aperture's shadow pages, FAKE_SHADOW_DIR to a directory, each
 * made at its first use. `full_resets` counts the resets of the whole GPU.
 */
struct i915_fake {
    struct spinlock lock;
    uint32_t *regs;
    uint32_t *gtt;
    uint32_t gtt_entries;
    struct fake_shadow *shadow[FAKE_SHADOW_DIRS];
    struct fake_ring ring[2];
    uint32_t armed[FAKE_NFAULTS];
    uint32_t injected[FAKE_NFAULTS];
    int line;
    uint32_t messages;
    int raise_pending;
    void (*raise)(struct i915_fake *f);
    uintptr_t doorbell;
    pci_device_t pdev;
    struct drm_i915_private *i915;
    char path[64];
    int32_t light;
    uint32_t full_resets;
};

/* ---- the register file ---- */

/** The register at `reg`, as stored. */
#define R(f, reg) ((f)->regs[(reg) / 4u])

/** Whether `reg` is a masked register: one whose high half enables the
 * bits of the low half that a write changes. */
static int fake_masked(uint32_t reg)
{
    int i;

    if (reg == FORCEWAKE_MT) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        uint32_t b = i ? BLT_RING_BASE : RENDER_RING_BASE;

        if (reg == RING_MI_MODE(b) || reg == RING_INSTPM(b) || reg == RING_MODE_GEN7(b)) {
            return 1;
        }
    }
    return 0;
}

/** The ring whose registers hold `reg`, or NULL. */
static struct fake_ring *fake_ring_of(struct i915_fake *f, uint32_t reg)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (reg >= f->ring[i].base && reg < f->ring[i].base + 0x1000u) {
            return &f->ring[i];
        }
    }
    return 0;
}

/** Take one injection of the fault `which` when it is armed: 1 and
 * counted, or 0. */
static int fake_take(struct i915_fake *f, int which)
{
    if (f->armed[which] == 0) {
        return 0;
    }
    if (f->armed[which] != FAKE_ALWAYS) {
        f->armed[which]--;
    }
    f->injected[which]++;
    return 1;
}

/* ---- interrupts ---- */

/** Follow a change of the identity and enable registers: the south
 * display's level raises the PCH event, and a rise of the device's line
 * raises a message once the access ends. */
static void fake_irq_eval(struct i915_fake *f)
{
    uint32_t de_ier = R(f, DEIER);
    int south = (R(f, SDEIIR) & R(f, SDEIER)) != 0, line;

    if (south && !(R(f, DEISR) & DE_PCH_EVENT_IVB)) {
        R(f, DEISR) |= DE_PCH_EVENT_IVB;
        if (!(R(f, DEIMR) & DE_PCH_EVENT_IVB)) {
            R(f, DEIIR) |= DE_PCH_EVENT_IVB;
        }
    } else if (!south) {
        R(f, DEISR) &= ~DE_PCH_EVENT_IVB;
    }
    line = (de_ier & DE_MASTER_IRQ_CONTROL) &&
           ((R(f, DEIIR) & de_ier & ~DE_MASTER_IRQ_CONTROL) || (R(f, GTIIR) & R(f, GTIER)) ||
            (R(f, GEN6_PMIIR) & R(f, GEN6_PMIER)));
    if (line && !f->line) {
        f->messages++;
        f->raise_pending = 1;
    }
    f->line = line;
}

/** The user interrupt of ring `r`: latched in GTIIR when RING_IMR and
 * GTIMR let it through, unless a lost interrupt is armed. */
static void fake_user_interrupt(struct i915_fake *f, struct fake_ring *r)
{
    if (R(f, RING_IMR(r->base)) & r->irq_bit) {
        return;
    }
    if (fake_take(f, FAKE_IRQ)) {
        klog(KLOG_INFO, "i915", "fake: injected: irq: %s's user interrupt lost", r->name);
        return;
    }
    R(f, GTIIR) |= r->irq_bit & ~R(f, GTIMR);
    fake_irq_eval(f);
}

/* ---- memory through the GTT ---- */

/** The physical address the GTT maps the offset `addr` to, from Haswell's
 * entry (hsw_pte_encode), or 0 for an invalid entry or one past the GTT. */
static uint64_t fake_gtt_phys(struct i915_fake *f, uint32_t addr)
{
    uint32_t index = addr >> 12, pte;

    if (!f->gtt || index >= f->gtt_entries) {
        return 0;
    }
    pte = f->gtt[index];
    if (!(pte & GEN6_PTE_VALID)) {
        return 0;
    }
    return (uint64_t)(pte & 0xfffff000u) | ((uint64_t)(pte & 0x7f0u) << 28) | (addr & 0xfffu);
}

/** Read (`write` 0) or write the dword at the GTT offset `addr` from or to
 * `*v`: 0, or -1 when the GTT maps nothing there. */
static int fake_mem(struct i915_fake *f, uint32_t addr, uint32_t *v, int write)
{
    uint64_t phys = fake_gtt_phys(f, addr & ~3u);
    uint8_t *page;

    if (phys == 0 || phys > (uint64_t)(uintptr_t)-1) {
        return -1;
    }
    page = (uint8_t *)kmap_temp((uintptr_t)(phys & ~(uint64_t)0xfffu));
    if (!page) {
        return -1;
    }
    if (write) {
        *(volatile uint32_t *)(void *)(page + (phys & 0xffcu)) = *v;
    } else {
        *v = *(volatile uint32_t *)(void *)(page + (phys & 0xffcu));
    }
    kunmap_temp(page);
    return 0;
}

/* ---- the rings ---- */

/** The dwords of the command whose first is `d0`, or 0 for a command of
 * no type the rings know. */
static uint32_t fake_cmd_length(uint32_t d0)
{
    switch (d0 >> 29) {
    case 0: /* MI: one dword below opcode 0x10, else its length field */
        if (MI_OPCODE(d0) < 0x10u) {
            return 1;
        }
        return (d0 & (MI_OPCODE(d0) == FAKE_MI_LOAD_REGISTER_IMM ? 0xffu : 0x3fu)) + 2u;
    case 2: /* 2D */
        return (d0 & 0xffu) + 2u;
    case 3: /* 3D and the render pipe */
        return (d0 & 0xffff0000u) == FAKE_PIPELINE_SELECT ? 1u : (d0 & 0xffu) + 2u;
    default:
        return 0;
    }
}

/** A post-sync write of `value` to `addr`, an offset into ring `r`'s
 * status page when `index` is set and a GTT offset otherwise: 0, or -1. */
static int fake_store(struct i915_fake *f, struct fake_ring *r, uint32_t addr, int index,
                      uint32_t value)
{
    if (index) {
        addr = (R(f, r->hws_pga) & ~0xfffu) + (addr & 0xffcu);
    }
    return fake_mem(f, addr, &value, 1);
}

/** Do the command `d`, of `len` dwords, on ring `r`: 0, or -1 for one the
 * ring cannot do, which hangs it. */
static int fake_cmd(struct i915_fake *f, struct fake_ring *r, const uint32_t *d, uint32_t len)
{
    if ((d[0] >> 29) == 3) {
        if ((d[0] & 0xffff0000u) == FAKE_PIPE_CONTROL && len >= 4u) {
            if ((d[1] & FAKE_POST_SYNC_MASK) == FAKE_POST_SYNC_WRITE &&
                fake_store(f, r, d[2] & ~3u, (d[1] & FAKE_STORE_INDEX) != 0, d[3]) < 0) {
                return -1;
            }
            if (d[1] & FAKE_PIPE_CONTROL_NOTIFY) {
                fake_user_interrupt(f, r);
            }
        }
        return 0;
    }
    if ((d[0] >> 29) != 0) {
        return 0;
    }
    switch (MI_OPCODE(d[0])) {
    case 0x02: /* MI_USER_INTERRUPT */
        fake_user_interrupt(f, r);
        return 0;
    case FAKE_MI_BATCH_BUFFER_START:
        r->batches++;
        return 0;
    case 0x20: /* MI_STORE_DWORD_IMM: the address in d[2], the value in d[3] */
        return len >= 4u ? fake_store(f, r, d[2] & ~3u, 0, d[3]) : -1;
    case 0x21: /* MI_STORE_DWORD_INDEX */
        return len >= 3u ? fake_store(f, r, d[1], 1, d[2]) : -1;
    case FAKE_MI_FLUSH_DW:
        if (len >= 3u && (d[0] & FAKE_POST_SYNC_MASK) == FAKE_POST_SYNC_WRITE) {
            return fake_store(f, r, d[1] & ~7u, (d[0] & FAKE_STORE_INDEX) != 0, d[2]);
        }
        return 0;
    default:
        return 0;
    }
}

/** Hang ring `r` at the GTT offset `at` on the command `d0`. */
static void fake_hang(struct i915_fake *f, struct fake_ring *r, uint32_t at, uint32_t d0)
{
    r->hung = 1;
    R(f, RING_ACTHD(r->base)) = at;
    R(f, RING_IPEHR(r->base)) = d0;
}

/**
 * Run ring `r` from its head to its tail, while it is valid, not stopped
 * and not hung: each command read through the GTT and done, and the head
 * moved past it.
 */
static void fake_ring_run(struct i915_fake *f, struct fake_ring *r)
{
    uint32_t ctl = R(f, RING_CTL(r->base));
    uint32_t size = (ctl & RING_NR_PAGES) + 4096u;
    uint32_t start = R(f, RING_START(r->base)) & ~0xfffu;
    uint32_t head = R(f, RING_HEAD(r->base)) & HEAD_ADDR;
    uint32_t tail = R(f, RING_TAIL(r->base)) & TAIL_ADDR;
    uint32_t steps = size / 4u, d[FAKE_CMD_MAX], len, i;

    if (!(ctl & RING_VALID) || (R(f, RING_MI_MODE(r->base)) & STOP_RING) || r->hung) {
        return;
    }
    head &= size - 1u;
    tail &= size - 1u;
    while (head != tail && steps-- > 0) {
        if (fake_mem(f, start + head, &d[0], 0) < 0) {
            fake_hang(f, r, start + head, 0xffffffffu);
            break;
        }
        if (fake_take(f, FAKE_HANG)) {
            klog(KLOG_INFO, "i915", "fake: injected: hang: %s at 0x%x on %08x", r->name,
                 (unsigned)(start + head), (unsigned)d[0]);
            fake_hang(f, r, start + head, d[0]);
            break;
        }
        len = fake_cmd_length(d[0]);
        for (i = 1; i < len && i < FAKE_CMD_MAX; i++) {
            if (fake_mem(f, start + ((head + 4u * i) & (size - 1u)), &d[i], 0) < 0) {
                d[i] = 0;
            }
        }
        if (len == 0 || fake_cmd(f, r, d, len) < 0) {
            klog(KLOG_ERR, "i915", "fake: %s: hung at 0x%x on %08x, which it cannot run", r->name,
                 (unsigned)(start + head), (unsigned)d[0]);
            fake_hang(f, r, start + head, d[0]);
            break;
        }
        head = (head + 4u * len) & (size - 1u);
        r->commands++;
    }
    R(f, RING_HEAD(r->base)) = (R(f, RING_HEAD(r->base)) & ~HEAD_ADDR) | head;
    if (!r->hung) {
        R(f, RING_ACTHD(r->base)) = start + head;
    }
}

/** Reset the engines of the domains `domains` (GEN6_GRDOM_*): each such
 * ring stopped, emptied, disabled and no longer hung, as at power-on. */
static void fake_reset(struct i915_fake *f, uint32_t domains)
{
    int i;

    if (domains & GEN6_GRDOM_FULL) {
        f->full_resets++;
    }
    for (i = 0; i < 2; i++) {
        struct fake_ring *r = &f->ring[i];

        if (!(domains & (GEN6_GRDOM_FULL | r->domain))) {
            continue;
        }
        if (!(domains & GEN6_GRDOM_FULL)) {
            r->resets++;
        }
        r->hung = 0;
        R(f, RING_CTL(r->base)) = 0;
        R(f, RING_HEAD(r->base)) = 0;
        R(f, RING_TAIL(r->base)) = 0;
        R(f, RING_START(r->base)) = 0;
        R(f, RING_ACTHD(r->base)) = 0;
        R(f, RING_IPEHR(r->base)) = 0;
        R(f, RING_MI_MODE(r->base)) = 0;
        R(f, RING_IMR(r->base)) = 0xffffffffu;
    }
}

/* ---- the accesses ---- */

/** A register as the chip reads it. */
static uint32_t fake_reg_read(struct i915_fake *f, uint32_t reg)
{
    struct fake_ring *r;

    switch (reg) {
    case FORCEWAKE_ACK_HSW:
        return R(f, FORCEWAKE_MT) & 0xffffu;
    case GTFIFOCTL:
        return (R(f, GTFIFOCTL) & ~GT_FIFO_FREE_ENTRIES_MASK) | FAKE_FIFO_ENTRIES;
    case HSW_PWR_WELL_CTL1:
    case HSW_PWR_WELL_CTL2:
    case HSW_PWR_WELL_CTL3:
    case HSW_PWR_WELL_CTL4:
        return R(f, reg) | ((R(f, HSW_PWR_WELL_CTL1) | R(f, HSW_PWR_WELL_CTL2) |
                             R(f, HSW_PWR_WELL_CTL3) | R(f, HSW_PWR_WELL_CTL4))
                                ? HSW_PWR_WELL_CTL_STATE(HSW_PW_CTL_IDX_GLOBAL)
                                : 0);
    default:
        break;
    }
    r = fake_ring_of(f, reg);
    if (r && reg == RING_MI_MODE(r->base)) {
        uint32_t mode = R(f, reg) & ~MODE_IDLE;
        int empty =
            (R(f, RING_HEAD(r->base)) & HEAD_ADDR) == (R(f, RING_TAIL(r->base)) & TAIL_ADDR);

        if ((mode & STOP_RING) && (empty || !(R(f, RING_CTL(r->base)) & RING_VALID))) {
            mode |= MODE_IDLE;
        }
        return mode;
    }
    return R(f, reg);
}

/** The pcode's command `cmd`, posted with the ready bit, finished: its
 * answer in GEN6_PCODE_DATA, then the ready bit cleared and the status in
 * the mailbox's low byte. */
static void fake_pcode(struct i915_fake *f, uint32_t cmd)
{
    uint32_t status = 0;

    switch (cmd & 0xffu) {
    case HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL:
        R(f, GEN6_PCODE_DATA) = FAKE_PCODE_DDCC;
        break;
    case GEN6_READ_OC_PARAMS:
    case GEN6_PCODE_READ_RC6VIDS:
        R(f, GEN6_PCODE_DATA) = 0;
        break;
    case GEN6_PCODE_WRITE_MIN_FREQ_TABLE:
        break;
    default:
        status = GEN6_PCODE_ILLEGAL_CMD;
        break;
    }
    R(f, GEN6_PCODE_MAILBOX) = status & GEN6_PCODE_ERROR_MASK;
}

/** A frequency request in GEN6_RPNSWREQ, run within RPn and RP0 and shown
 * in GEN6_RPSTAT1's CAGF. */
static void fake_rps(struct i915_fake *f, uint32_t value)
{
    uint32_t want = value >> HSW_FREQUENCY_SHIFT, cap = R(f, GEN6_RP_STATE_CAP);
    uint32_t rpn = (cap >> 16) & 0xffu, rp0 = cap & 0xffu;

    want = want < rpn ? rpn : want > rp0 ? rp0 : want;
    R(f, GEN6_RPSTAT1) = want << HSW_CAGF_SHIFT;
}

/** A register written as the chip takes it, and whatever the write
 * starts. */
static void fake_reg_write(struct i915_fake *f, uint32_t reg, uint32_t value)
{
    struct fake_ring *r = fake_ring_of(f, reg);

    if (fake_masked(reg)) {
        value = (R(f, reg) & ~(value >> 16)) | (value & (value >> 16) & 0xffffu);
    }
    switch (reg) {
    case DEIIR:
    case GTIIR:
    case GEN6_PMIIR:
    case SDEIIR:
    case GEN7_ERR_INT:
    case SERR_INT:
    case FPGA_DBG:
    case GTFIFODBG:
        R(f, reg) &= ~value;
        break;
    case DEISR:
    case GTISR:
    case GEN6_PMISR:
    case SDEISR:
        break;
    case HSW_PWR_WELL_CTL1:
    case HSW_PWR_WELL_CTL2:
    case HSW_PWR_WELL_CTL3:
    case HSW_PWR_WELL_CTL4:
        R(f, reg) = value & HSW_PWR_WELL_CTL_REQ(HSW_PW_CTL_IDX_GLOBAL);
        break;
    case GEN6_GDRST:
        fake_reset(f, value);
        R(f, reg) = 0;
        break;
    case GEN6_PCODE_MAILBOX:
        R(f, reg) = value;
        if (value & GEN6_PCODE_READY) {
            fake_pcode(f, value);
        }
        break;
    case GEN6_RPNSWREQ:
        R(f, reg) = value;
        fake_rps(f, value);
        break;
    default:
        R(f, reg) = value;
        break;
    }
    if (r && reg == RING_INSTPM(r->base)) {
        R(f, reg) &= ~(INSTPM_SYNC_FLUSH | INSTPM_TLB_INVALIDATE);
    }
    if (r &&
        (reg == RING_TAIL(r->base) || reg == RING_CTL(r->base) || reg == RING_MI_MODE(r->base))) {
        fake_ring_run(f, r);
    }
    fake_irq_eval(f);
}

/** Raise the message a rise of the line asked for, once the access that
 * made it has dropped the lock. */
static void fake_raise(struct i915_fake *f, int pending)
{
    if (pending && f->raise) {
        f->raise(f);
    }
}

/** i915_hw.h's read: the register as the chip reads it, under the lock. */
static uint32_t fake_read32(struct i915_hw *hw, uint32_t reg)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint32_t flags = spin_lock_irqsave(&f->lock), value;

    value = fake_reg_read(f, reg);
    spin_unlock_irqrestore(&f->lock, flags);
    return value;
}

/** i915_hw.h's write, under the lock; the message after it. */
static void fake_write32(struct i915_hw *hw, uint32_t reg, uint32_t value)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint32_t flags = spin_lock_irqsave(&f->lock);
    int pending;

    fake_reg_write(f, reg, value);
    pending = f->raise_pending;
    f->raise_pending = 0;
    spin_unlock_irqrestore(&f->lock, flags);
    fake_raise(f, pending);
}

/** Time on the fake is the processor's: nothing it does takes any. */
static void fake_delay_us(struct i915_hw *hw, uint32_t us)
{
    (void)hw;
    k_udelay(us);
}

/** The GTT's entries, `size` bytes of memory, zeroed: 0 or -ENOMEM. */
static int32_t fake_gtt_map(struct i915_hw *hw, uint64_t phys, uint32_t size)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint32_t *gtt;

    (void)phys;
    if (size < 4u) {
        return -ENOMEM;
    }
    gtt = (uint32_t *)kzalloc(size);
    if (!gtt) {
        return -ENOMEM;
    }
    kfree(f->gtt);
    f->gtt = gtt;
    f->gtt_entries = size / 4u;
    hw->gsm = gtt;
    return 0;
}

/** The chip's configuration space, as far as the driver reads it from the
 * backend; 0 elsewhere. */
static uint32_t fake_config_read32(struct i915_hw *hw, uint32_t offset)
{
    (void)hw;
    switch (offset) {
    case 0x00:
        return FAKE_ID_DWORD;
    case 0x08:
        return FAKE_CLASS_REVISION;
    case 0x2c:
        return FAKE_SUBSYSTEM;
    case SNB_GMCH_CTRL:
        return FAKE_GMCH_CTRL;
    case HSW_MSAC & ~3u:
        return (uint32_t)FAKE_MSAC << ((HSW_MSAC & 2u) * 8u);
    default:
        return 0;
    }
}

/** A fault point of the driver: the armed fault's error, logged. */
static int32_t fake_fault(struct i915_hw *hw, int point)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint32_t flags;
    int32_t err = 0;

    if (point != FAKE_SUBMIT && point != FAKE_BIND) {
        return 0;
    }
    flags = spin_lock_irqsave(&f->lock);
    if (fake_take(f, point)) {
        err = point == FAKE_SUBMIT ? -EIO : -ENOSPC;
    }
    spin_unlock_irqrestore(&f->lock, flags);
    if (err < 0) {
        klog(KLOG_INFO, "i915", "fake: injected: %s: error %d", fake_fault_names[point], (int)err);
    }
    return err;
}

/* ---- the aperture ---- */

/** The fence over the aperture's offset `addr`, by its two registers, in
 * `*lo` and `*hi`; both 0 when no valid fence covers it. */
static void fake_fence_at(struct i915_fake *f, uint32_t addr, uint32_t *lo, uint32_t *hi)
{
    int i;

    *lo = *hi = 0;
    for (i = 0; i < I915_NUM_FENCES; i++) {
        uint32_t l = R(f, FENCE_REG_GEN6_LO(i)), h = R(f, FENCE_REG_GEN6_HI(i));

        if ((l & I965_FENCE_REG_VALID) && addr >= (l & 0xfffff000u) &&
            addr < (h & 0xfffff000u) + 4096u) {
            *lo = l;
            *hi = h;
            return;
        }
    }
}

/** The swizzle the fake's controller gives tiles of `tiling`, as the
 * driver reads it: bits 9 and 10 for X and 9 for Y when both channels
 * hold DIMMs of one size. */
static uint32_t fake_swizzle(struct i915_fake *f, uint32_t tiling)
{
    uint32_t mask = MAD_DIMM_A_SIZE_MASK | MAD_DIMM_B_SIZE_MASK;

    if ((R(f, MAD_DIMM_C0) & mask) != (R(f, MAD_DIMM_C1) & mask)) {
        return I915_BIT_6_SWIZZLE_NONE;
    }
    return tiling == I915_TILING_X ? I915_BIT_6_SWIZZLE_9_10 : I915_BIT_6_SWIZZLE_9;
}

/**
 * Copy the aperture's page at `addr` between its shadow `sh` and memory,
 * to memory when `to_memory`, a run at a time through the GTT's entries
 * and, under the fence `lo` and `hi`, through its tiling. A run the GTT
 * maps nothing at reads as zeroes and is not written.
 */
static void fake_shadow_copy(struct i915_fake *f, struct fake_shadow *sh, uint32_t addr,
                             uint32_t lo, uint32_t hi, int to_memory)
{
    uint32_t start = lo & 0xfffff000u, stride = ((hi & 0xfffu) + 1u) * 128u;
    uint32_t tiling = (lo & (1u << I965_FENCE_TILING_Y_SHIFT)) ? I915_TILING_Y : I915_TILING_X;
    uint32_t swizzle = fake_swizzle(f, tiling), off;
    uintptr_t mapped = 0;
    uint8_t *page = 0, *va = (uint8_t *)kmap_temp(sh->phys);

    if (!va) {
        return;
    }
    for (off = 0; off < 4096u; off += FAKE_DETILE_RUN) {
        uint32_t at = addr + off;
        uint64_t phys;

        if (lo) {
            at = start + (uint32_t)i915_tiled_offset(tiling, stride, swizzle, at - start);
        }
        phys = fake_gtt_phys(f, at);
        if (phys == 0 || phys > (uint64_t)(uintptr_t)-1) {
            if (!to_memory) {
                k_memset(va + off, 0, FAKE_DETILE_RUN);
            }
            continue;
        }
        if (!page || mapped != (uintptr_t)(phys & ~(uint64_t)0xfffu)) {
            if (page) {
                kunmap_temp(page);
            }
            mapped = (uintptr_t)(phys & ~(uint64_t)0xfffu);
            page = (uint8_t *)kmap_temp(mapped);
            if (!page) {
                break;
            }
        }
        if (to_memory) {
            k_memcpy(page + (phys & 0xfffu), va + off, FAKE_DETILE_RUN);
        } else {
            k_memcpy(va + off, page + (phys & 0xfffu), FAKE_DETILE_RUN);
        }
    }
    if (page) {
        kunmap_temp(page);
    }
    kunmap_temp(va);
}

/** The shadow of the aperture's page `index`, made when `make`; NULL when
 * there is none or no memory for it. */
static struct fake_shadow *fake_shadow_of(struct i915_fake *f, uint32_t index, int make)
{
    struct fake_shadow **dir = &f->shadow[index / FAKE_SHADOW_DIR];
    struct fake_shadow *sh;

    if (!*dir) {
        if (!make) {
            return 0;
        }
        *dir = (struct fake_shadow *)kzalloc(FAKE_SHADOW_DIR * sizeof(**dir));
        if (!*dir) {
            return 0;
        }
    }
    sh = &(*dir)[index % FAKE_SHADOW_DIR];
    if (!sh->phys && make) {
        sh->phys = alloc_frame();
    }
    return sh->phys ? sh : 0;
}

/** Fill the shadow `sh` of the aperture's page at `addr` from memory and
 * remember the entry and fence it was filled under. */
static void fake_shadow_fill(struct i915_fake *f, struct fake_shadow *sh, uint32_t addr)
{
    fake_fence_at(f, addr, &sh->fence_lo, &sh->fence_hi);
    sh->pte = f->gtt && (addr >> 12) < f->gtt_entries ? f->gtt[addr >> 12] : 0;
    fake_shadow_copy(f, sh, addr, sh->fence_lo, sh->fence_hi, 0);
}

/** Whether the shadow `sh` of the page at `addr` was filled under the
 * entry and fence over it now. */
static int fake_shadow_current(struct i915_fake *f, const struct fake_shadow *sh, uint32_t addr)
{
    uint32_t lo, hi, pte = f->gtt && (addr >> 12) < f->gtt_entries ? f->gtt[addr >> 12] : 0;

    fake_fence_at(f, addr, &lo, &hi);
    return sh->pte == pte && sh->fence_lo == lo && sh->fence_hi == hi;
}

/**
 * i915_hw.h's `aperture_page`: the shadow of the aperture's page at
 * `offset`, made and filled at its first use and filled again when its
 * entry or fence has changed. The driver calls it under the big kernel
 * lock, as it does every aperture operation, and the interrupt handler
 * changes neither the entries nor the fences, so the fake's lock is not
 * taken. 0, or -1 past the aperture or without memory.
 */
static int fake_aperture_page(struct i915_hw *hw, uint64_t offset, uintptr_t *phys)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint32_t index = (uint32_t)(offset >> 12), addr = index << 12;
    struct fake_shadow *sh;
    int fresh;

    if (offset >= (uint64_t)FAKE_APERTURE_PAGES << 12) {
        return -1;
    }
    fresh = !fake_shadow_of(f, index, 0);
    sh = fake_shadow_of(f, index, 1);
    if (!sh) {
        return -1;
    }
    if (fresh || !fake_shadow_current(f, sh, addr)) {
        fake_shadow_fill(f, sh, addr);
    }
    *phys = sh->phys;
    return 0;
}

/** i915_hw.h's `aperture_sync`: the shadows of [offset, offset + size)
 * written to memory, those filled under the entry and fence over them
 * now, or filled from it. */
static void fake_aperture_sync(struct i915_hw *hw, uint64_t offset, uint64_t size, int to_memory)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint64_t at, end = offset + size;

    if (end > (uint64_t)FAKE_APERTURE_PAGES << 12) {
        end = (uint64_t)FAKE_APERTURE_PAGES << 12;
    }
    for (at = offset & ~(uint64_t)0xfffu; at < end; at += 4096u) {
        struct fake_shadow *sh = fake_shadow_of(f, (uint32_t)(at >> 12), 0);

        if (!sh) {
            continue;
        }
        if (!to_memory) {
            fake_shadow_fill(f, sh, (uint32_t)at);
        } else if (fake_shadow_current(f, sh, (uint32_t)at)) {
            fake_shadow_copy(f, sh, (uint32_t)at, sh->fence_lo, sh->fence_hi, 1);
        }
    }
}

/** Free the aperture's shadow pages. */
static void fake_shadow_free(struct i915_fake *f)
{
    uint32_t d, i;

    for (d = 0; d < FAKE_SHADOW_DIRS; d++) {
        if (!f->shadow[d]) {
            continue;
        }
        for (i = 0; i < FAKE_SHADOW_DIR; i++) {
            if (f->shadow[d][i].phys) {
                free_frame(f->shadow[d][i].phys);
            }
        }
        kfree(f->shadow[d]);
        f->shadow[d] = 0;
    }
}

/** Free the fake and its memory. */
static void i915_fake_free(struct i915_fake *f)
{
    if (!f) {
        return;
    }
    fake_shadow_free(f);
    kfree(f->gtt);
    kfree(f->regs);
    kfree(f);
}

/* ---- the debugfs file ---- */

/* What a descriptor of the file reads: the text made at its open, and
 * the device, held until its close. */
struct fake_text {
    struct i915_fake *f;
    uint32_t len;
    char text[512];
};

/** Append `s` to `t`, as far as it has room. */
static void text_put(struct fake_text *t, const char *s)
{
    while (*s && t->len < sizeof(t->text) - 1u) {
        t->text[t->len++] = *s++;
    }
    t->text[t->len] = '\0';
}

/** Append `n` in decimal, with a minus sign when `negative`. */
static void text_num(struct fake_text *t, uint32_t n, int negative)
{
    char digits[12];
    int i = 0;

    do {
        digits[i++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n);
    if (negative) {
        digits[i++] = '-';
    }
    while (i > 0) {
        char c[2];
        c[0] = digits[--i];
        c[1] = '\0';
        text_put(t, c);
    }
}

/**
 * The file's text, one line each: `fault NAME armed N injected M` for each
 * fault, with `always` for N; `ring NAME commands N batches M hung 0|1` for
 * each ring; `resets rcs0 N bcs0 M full K`, the resets of each ring's
 * domain alone and of the whole GPU; `messages N`; and `light never`,
 * `light ok` or `light error -E`.
 */
static void fake_text(struct i915_fake *f, struct fake_text *t)
{
    uint32_t flags = spin_lock_irqsave(&f->lock);
    int i;

    t->len = 0;
    t->text[0] = '\0';
    for (i = 0; i < FAKE_NFAULTS; i++) {
        text_put(t, "fault ");
        text_put(t, fake_fault_names[i]);
        text_put(t, " armed ");
        if (f->armed[i] == FAKE_ALWAYS) {
            text_put(t, "always");
        } else {
            text_num(t, f->armed[i], 0);
        }
        text_put(t, " injected ");
        text_num(t, f->injected[i], 0);
        text_put(t, "\n");
    }
    for (i = 0; i < 2; i++) {
        text_put(t, "ring ");
        text_put(t, f->ring[i].name);
        text_put(t, " commands ");
        text_num(t, f->ring[i].commands, 0);
        text_put(t, " batches ");
        text_num(t, f->ring[i].batches, 0);
        text_put(t, f->ring[i].hung ? " hung 1\n" : " hung 0\n");
    }
    text_put(t, "resets rcs0 ");
    text_num(t, f->ring[0].resets, 0);
    text_put(t, " bcs0 ");
    text_num(t, f->ring[1].resets, 0);
    text_put(t, " full ");
    text_num(t, f->full_resets, 0);
    text_put(t, "\n");
    text_put(t, "messages ");
    text_num(t, f->messages, 0);
    spin_unlock_irqrestore(&f->lock, flags);
    if (f->light > 0) {
        text_put(t, "\nlight never\n");
    } else if (f->light == 0) {
        text_put(t, "\nlight ok\n");
    } else {
        text_put(t, "\nlight error ");
        text_num(t, (uint32_t)-f->light, 1);
        text_put(t, "\n");
    }
}

/** Whether the `n` bytes at `s` are the word `w`. */
static int fake_word(const char *s, uint32_t n, const char *w)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (w[i] != s[i]) {
            return 0;
        }
    }
    return w[n] == '\0';
}

/**
 * Do one command of the file: `NAME` arms the fault NAME once, `NAME N`
 * N times (0 disarms it) and `NAME always` for good; `clear` disarms
 * every fault; and `light` runs first light now (i915_firstlight_run),
 * whose result the text's `light` line then gives. A hung ring stays hung
 * until a reset of its domain whatever is armed.
 *
 * @return 0, or -EINVAL for a command of another form.
 */
static int32_t fake_command(struct i915_fake *f, const char *s, uint32_t n)
{
    uint32_t i = 0, w, count = 1, flags;
    int which;

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    while (i < n && s[i] != ' ') {
        i++;
    }
    w = i;
    if (fake_word(s, w, "clear") && w == n) {
        flags = spin_lock_irqsave(&f->lock);
        k_memset(f->armed, 0, sizeof(f->armed));
        spin_unlock_irqrestore(&f->lock, flags);
        return 0;
    }
    if (fake_word(s, w, "light") && w == n) {
        if (!f->i915) {
            return -ENODEV;
        }
        f->light = i915_firstlight_run(f->i915);
        return 0;
    }
    for (which = 0; which < FAKE_NFAULTS; which++) {
        if (fake_word(s, w, fake_fault_names[which])) {
            break;
        }
    }
    if (which == FAKE_NFAULTS) {
        return -EINVAL;
    }
    while (i < n && s[i] == ' ') {
        i++;
    }
    if (i < n && fake_word(s + i, n - i, "always")) {
        count = FAKE_ALWAYS;
    } else if (i < n) {
        count = 0;
        for (; i < n; i++) {
            if (s[i] < '0' || s[i] > '9' || count > 100000000u) {
                return -EINVAL;
            }
            count = count * 10u + (uint32_t)(s[i] - '0');
        }
    }
    flags = spin_lock_irqsave(&f->lock);
    f->armed[which] = count;
    spin_unlock_irqrestore(&f->lock, flags);
    return 0;
}

/** Read the text made at the open, from the description's offset. */
static int32_t fake_file_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct fake_text *t = (struct fake_text *)e->priv;
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

/** Do the command written, 63 bytes at most (`fake_command`); then the
 * text is made again, so that a read of the same descriptor sees it. */
static int32_t fake_file_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct fake_text *t = (struct fake_text *)e->priv;
    char cmd[64];
    int32_t err;

    if (count >= sizeof(cmd)) {
        return -EINVAL;
    }
    if (copy_from_user(cmd, buf, count) != 0) {
        return -EFAULT;
    }
    err = fake_command(t->f, cmd, count);
    if (err < 0) {
        return err;
    }
    fake_text(t->f, t);
    return (int32_t)count;
}

/** Move the description's offset in the text. */
static int64_t fake_file_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    struct fake_text *t = (struct fake_text *)e->priv;
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)e->ofd->offset : (int64_t)t->len;

    if (whence < 0 || whence > 2 || base + offset < 0) {
        return -EINVAL;
    }
    return base + offset;
}

/** Free the text and let the device go at the last close. */
static void fake_file_close(fd_entry_t *e)
{
    struct fake_text *t = (struct fake_text *)e->priv;

    if (t) {
        drm_dev_put(&t->f->i915->drm);
        kfree(t);
    }
    e->priv = NULL;
}

static const struct file_ops fake_file_ops = {
    fake_file_read, fake_file_write, NULL, NULL, fake_file_close, NULL, fake_file_llseek};

/** The open: the text made now, and a reference to the device, so that
 * the fake outlives the descriptor. */
static int32_t fake_file_open(fd_entry_t *e, int flags)
{
    struct i915_fake *f = (struct i915_fake *)e->priv;
    struct fake_text *t;

    (void)flags;
    if (!f->i915) {
        return -ENODEV;
    }
    t = (struct fake_text *)kzalloc(sizeof(*t));
    if (!t) {
        return -ENOMEM;
    }
    t->f = f;
    fake_text(f, t);
    drm_dev_get(&f->i915->drm);
    e->priv = t;
    return 0;
}

/* ---- the device's life ---- */

/** Raise the device's message by a write to edu's interrupt register,
 * while the driver has its message on. */
static void fake_raise_edu(struct i915_fake *f)
{
    if (f->doorbell && f->i915 && f->i915->msi_vector >= 0) {
        mmio_write32(f->doorbell + FAKE_EDU_RAISE, 1u);
    }
}

/** The device is registered: map edu's interrupt register and make the
 * device's debugfs file. */
static void fake_attach(struct i915_hw *hw, struct drm_i915_private *i915)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint64_t bar = pci_bar_address64(&f->pdev, 0);
    int32_t err;

    f->i915 = i915;
    if (bar) {
        f->doorbell = mmio_map(bar, FAKE_EDU_BYTES, PTE_NOCACHE);
    }
    drm_debugfs_path(&i915->drm, "i915_fake", f->path, sizeof(f->path));
    err = debugfs_create_file(f->path, 0600u, &fake_file_ops, fake_file_open, f);
    if (err < 0) {
        klog(KLOG_ERR, "i915", "fake: %s: error %d", f->path, (int)err);
        f->path[0] = '\0';
    }
}

/** The device is unplugged: its file goes, and edu stops being rung. */
static void fake_detach(struct i915_hw *hw)
{
    struct i915_fake *f = (struct i915_fake *)hw->priv;
    uint32_t flags;
    uintptr_t doorbell;

    if (f->path[0]) {
        debugfs_remove(f->path);
        f->path[0] = '\0';
    }
    flags = spin_lock_irqsave(&f->lock);
    doorbell = f->doorbell;
    f->doorbell = 0;
    spin_unlock_irqrestore(&f->lock, flags);
    if (doorbell) {
        mmio_unmap(doorbell, FAKE_EDU_BYTES);
    }
}

/** The device's record goes: so does the fake. */
static void fake_release(struct i915_hw *hw)
{
    i915_fake_free((struct i915_fake *)hw->priv);
}

static const struct i915_hw_ops fake_ops = {
    "fake",     fake_read32, fake_write32, fake_delay_us, fake_gtt_map,       fake_config_read32,
    fake_fault, fake_attach, fake_detach,  fake_release,  fake_aperture_page, fake_aperture_sync};

/**
 * Make a fake chip at power-on, with every interrupt source masked, every
 * ring disabled and its RING_IMR set, and `hw` the backend over it, which
 * answers for the 2 MiB of registers. NULL when memory is short.
 */
static struct i915_fake *i915_fake_create(struct i915_hw *hw)
{
    static const uint32_t imr[] = {DEIMR, GTIMR, GEN6_PMIMR, SDEIMR};
    struct i915_fake *f = (struct i915_fake *)kzalloc(sizeof(*f));
    unsigned i;

    if (!f) {
        return 0;
    }
    f->regs = (uint32_t *)kzalloc(I915_MMIO_SIZE);
    if (!f->regs) {
        kfree(f);
        return 0;
    }
    f->ring[0].name = "rcs0";
    f->ring[0].base = RENDER_RING_BASE;
    f->ring[0].hws_pga = RENDER_HWS_PGA_GEN7;
    f->ring[0].irq_bit = GT_RENDER_USER_INTERRUPT;
    f->ring[0].domain = GEN6_GRDOM_RENDER;
    f->ring[1].name = "bcs0";
    f->ring[1].base = BLT_RING_BASE;
    f->ring[1].hws_pga = BLT_HWS_PGA_GEN7;
    f->ring[1].irq_bit = GT_BLT_USER_INTERRUPT;
    f->ring[1].domain = GEN6_GRDOM_BLT;
    for (i = 0; i < sizeof(imr) / sizeof(imr[0]); i++) {
        R(f, imr[i]) = 0xffffffffu;
    }
    R(f, GEN6_RP_STATE_CAP) = FAKE_RP_STATE_CAP;
    R(f, GEN6_RPSTAT1) = ((FAKE_RP_STATE_CAP >> 16) & 0xffu) << HSW_CAGF_SHIFT;
    fake_reset(f, GEN6_GRDOM_FULL);
    f->light = 1;
    hw->ops = &fake_ops;
    hw->priv = f;
    hw->size = I915_MMIO_SIZE;
    hw->timeouts = 0;
    hw->last_timeout = 0;
    hw->gtt_entries = 0;
    hw->gsm = 0;
    hw->faults = 0;
    return f;
}

int32_t i915_fake_probe(void)
{
    pci_device_t pdev;
    struct i915_hw hw;
    struct i915_fake *f;
    int32_t err, bound = 0;
    int nth;

    for (nth = 0; pci_find_nth_by_id(FAKE_EDU_VENDOR, FAKE_EDU_DEVICE, nth, &pdev); nth++) {
        f = i915_fake_create(&hw);
        if (!f) {
            klog(KLOG_ERR, "i915", "fake: no memory for the registers");
            break;
        }
        f->pdev = pdev;
        f->raise = fake_raise_edu;
        klog(KLOG_INFO, "i915", "fake: %02x:%02x.%x: edu stands in for 8086:0a16",
             (unsigned)pdev.bus, (unsigned)pdev.device, (unsigned)pdev.function);
        err = i915_driver_bind(&pdev, &hw);
        if (err < 0) {
            klog(KLOG_ERR, "i915", "fake: %02x:%02x.%x: error %d", (unsigned)pdev.bus,
                 (unsigned)pdev.device, (unsigned)pdev.function, (int)err);
            i915_fake_free(f);
            continue;
        }
        bound++;
    }
    if (nth == 0) {
        klog(KLOG_ERR, "i915", "fake: no edu device to stand on; QEMU's -device edu gives one");
    }
    return bound > 0 ? bound : -ENODEV;
}
