/*
 * i915_lenovo.h: the Lenovo G50-70's HD 4400 as Linux read it, for the
 * host model of the chip: its place and ids, its configuration space and
 * its stolen memory.
 *
 * The facts are from dev-log/70-wifi-chip/linux/dmesg.txt (lines 190 to
 * 200, the RMRR, and 319 to 322, the BARs) and from
 * tests/fixtures/lenovo-gpu/facts.out and lspci-igd.out (Debian 13's
 * Linux 6.12 on the machine): the board's ids, the GMCH control word,
 * BDSM, the OpRegion's address and the capability list. 78-10a adds the
 * VBT and the OpRegion's bytes.
 *
 * Include it after i915_model.h.
 */
#ifndef _I915_LENOVO_H_
#define _I915_LENOVO_H_

/* 00:02.0, 8086:0a16 revision 0b, class 030000, Lenovo 17aa:380c. */
#define LENOVO_GPU_BUS       0
#define LENOVO_GPU_DEVICE    2
#define LENOVO_GPU_FUNCTION  0
#define LENOVO_GPU_VENDOR    0x8086
#define LENOVO_GPU_ID        0x0a16
#define LENOVO_GPU_REVISION  0x0b
#define LENOVO_GPU_SUBVENDOR 0x17aa
#define LENOVO_GPU_SUBDEVICE 0x380c

/* BAR 0, the registers and the GTT, 4 MiB of 64-bit memory; BAR 2, the
 * aperture, 256 MiB of 64-bit prefetchable memory; BAR 4, 64 ports. */
#define LENOVO_GPU_BAR0      0xc0000000u
#define LENOVO_GPU_BAR0_SIZE 0x400000u
#define LENOVO_GPU_BAR2      0xb0000000u
#define LENOVO_GPU_BAR2_SIZE 0x10000000u
#define LENOVO_GPU_BAR4      0x4000u

/* The GMCH control word (GGC): the lock, 2 MiB of GTT and 64 MiB of
 * stolen memory; BDSM, the stolen memory's base with its lock; the
 * aperture's size code (MSAC), 256 MiB; and ASLS, the OpRegion. */
#define LENOVO_GPU_GGC  0x0211u
#define LENOVO_GPU_BDSM 0xaba00001u
#define LENOVO_GPU_MSAC 0x02u
#define LENOVO_GPU_ASLS 0xaafb4018u

/* The stolen memory Linux reserved ("Reserving Intel graphics memory at
 * [mem 0xaba00000-0xaf9fffff]"), and the RMRR that covers it and the GTT
 * below it. */
#define LENOVO_STOLEN_BASE 0xaba00000u
#define LENOVO_STOLEN_END  0xaf9fffffu
#define LENOVO_RMRR_BASE   0xab800000u
#define LENOVO_RMRR_END    0xaf9fffffu

/* The firmware's framebuffer, as Linux found it at load
 * (tests/fixtures/lenovo-gpu/debugfs/i915_gem_framebuffer): 1366x768 at
 * 32 bits, linear, at GTT offset 0 and stolen offset 0, 0x408000 bytes,
 * which make a stride of 5504 bytes; pipe A's primary plane scans it out,
 * and the console draws on it at the aperture's base. */
#define LENOVO_FW_FB_WIDTH  1366
#define LENOVO_FW_FB_HEIGHT 768
#define LENOVO_FW_FB_STRIDE 5504
#define LENOVO_FW_FB_GGTT   0x0u
#define LENOVO_FW_FB_BYTES  0x408000u

/* The panel's route as the firmware left it (firmware-dump.txt):
 * TRANS_DDI_FUNC_CTL_EDP enabled on DDI A in DisplayPort SST from pipe A
 * always on, and the eDP transcoder enabled and active. */
#define LENOVO_FW_EDP_DDI_FUNC  0x82200000u
#define LENOVO_FW_EDP_TRANSCONF 0xc0000010u

/* What else the firmware leaves set, as gfxtool read it on the release
 * (dev-log/78-intel/78-4b.md): an unclaimed access latched in FPGA_DBG's
 * bit 31 and in GEN7_ERR_INT's bit 13, and PM interrupt sources enabled,
 * every one masked. The bind clears all three. */
#define LENOVO_FW_FPGA_DBG 0x8000e380u
#define LENOVO_FW_ERR_INT  0x00002000u
#define LENOVO_FW_PMIER    0x03000076u

/* The capability list: MSI at 0x90 (one 32-bit message, not maskable),
 * power management at 0xd0 (version 2, DSI) and Advanced Features at 0xa4
 * (transaction pending and FLR). */
#define LENOVO_GPU_CAP_MSI 0x90
#define LENOVO_GPU_CAP_PM  0xd0
#define LENOVO_GPU_CAP_AF  0xa4

/* A field of the configuration space: `len` bytes at `at`, little-endian. */
struct lenovo_gpu_field {
    uint8_t at, len;
    uint32_t value;
};

/*
 * The chip's configuration space as the firmware left it: memory and I/O
 * decoding and bus mastering on, the INTx line disabled, and the message
 * off, which Linux turns on. Every byte not named is zero.
 */
static const struct lenovo_gpu_field lenovo_gpu_config[] = {
    {0x00, 2, LENOVO_GPU_VENDOR},
    {0x02, 2, LENOVO_GPU_ID},
    {0x04, 2, 0x0407u}, /* I/O, memory, bus master, INTx disabled */
    {0x06, 2, 0x0090u}, /* capability list, fast back to back */
    {0x08, 1, LENOVO_GPU_REVISION},
    {0x09, 1, 0x00},
    {0x0a, 1, 0x00},
    {0x0b, 1, 0x03},
    {0x10, 4, LENOVO_GPU_BAR0 | 0x4u}, /* 64-bit memory */
    {0x14, 4, 0},
    {0x18, 4, LENOVO_GPU_BAR2 | 0xcu}, /* 64-bit prefetchable memory */
    {0x1c, 4, 0},
    {0x20, 4, LENOVO_GPU_BAR4 | 0x1u}, /* I/O */
    {0x2c, 2, LENOVO_GPU_SUBVENDOR},
    {0x2e, 2, LENOVO_GPU_SUBDEVICE},
    {0x34, 1, LENOVO_GPU_CAP_MSI},
    {0x3d, 1, 0x02}, /* INTB# */
    {0x50, 2, LENOVO_GPU_GGC},
    {0x5c, 4, LENOVO_GPU_BDSM},
    {0x62, 1, LENOVO_GPU_MSAC},
    {LENOVO_GPU_CAP_MSI + 0, 1, 0x05}, /* MSI */
    {LENOVO_GPU_CAP_MSI + 1, 1, LENOVO_GPU_CAP_PM},
    {LENOVO_GPU_CAP_MSI + 2, 2, 0x0000u}, /* one message, 32-bit, off */
    {LENOVO_GPU_CAP_PM + 0, 1, 0x01},     /* power management */
    {LENOVO_GPU_CAP_PM + 1, 1, LENOVO_GPU_CAP_AF},
    {LENOVO_GPU_CAP_PM + 2, 2, 0x0022u}, /* version 2, DSI */
    {LENOVO_GPU_CAP_AF + 0, 1, 0x13},    /* Advanced Features */
    {LENOVO_GPU_CAP_AF + 1, 1, 0x00},
    {LENOVO_GPU_CAP_AF + 2, 1, 0x06},
    {LENOVO_GPU_CAP_AF + 3, 1, 0x03}, /* TP and FLR */
    {0xfc, 4, LENOVO_GPU_ASLS},
};

#endif /* _I915_LENOVO_H_ */
