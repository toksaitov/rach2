#ifndef _BOARD_H_
#define _BOARD_H_

#include "../../lib/kint.h"

/*
 * board: which power controller this machine has, and what may be written to
 * it.
 *
 * The power off (power.c) writes a sleep word to the PM1 control register of
 * the chipset's ACPI block. QEMU's PIIX4, the ICH9 of QEMU's q35 machine,
 * and Intel's Lynx Point and Wildcat Point PCHs have that block behind
 * different configuration registers, so each is a profile in a table, and a
 * profile is identified before a byte is written. The sleep word is a 16-bit
 * write to two consecutive I/O ports, and the low ports a wrong base could
 * reach are the DMA controller, the interrupt controller, the timer, the
 * keyboard controller, the CMOS index and data pair, and the reset control
 * register; a wrong base resets the machine or corrupts its clock. So the
 * base is read from the chipset's own register, and four checks must pass:
 * the chipset's identity, its decode enable, a programmed base, and a PM1
 * control register that answers.
 *
 * One value cannot be read from the chipset: which sleep type means "off".
 * The firmware's \_S5 says, and the power off writes that first (power.c,
 * from acpi.c's search of the DSDT); a profile is the fallback for a machine
 * whose tables give none or whose write did not act. Its sleep type is a
 * constant, with its source stated in board.c, and it is written only to a
 * chipset that was identified. Sleep types are never tried in turn: on a
 * PCH, sleep type 5 is suspend, and a machine that suspends after its file
 * system has been flushed and scheduling stopped looks off and is not.
 *
 * The decisions are functions of the values read, with no hardware access,
 * so tests/host/board_hosttest.c tests each of them without a chipset.
 */

/** One chipset's power-management registers, and what may be written to
 * them. */
typedef struct {
    const char *name;              /* for `rach2.board` and the log */
    uint8_t bus, device, function; /* where the controller answers */
    uint16_t vendor;
    uint16_t id_first, id_last;   /* accepted device ids, inclusive */
    uint8_t class_code, subclass; /* 0xFF in either means "do not check" */
    uint8_t base_reg;             /* configuration offset of the PM base */
    uint16_t base_mask;           /* the bits of it that are the base */
    uint8_t decode_reg;           /* configuration offset of the decode enable */
    uint8_t decode_bit;
    uint8_t sci_reg;     /* offset carrying SCI_IRQ_SEL, 0 if none */
    int sci_fixed;       /* the SCI line when sci_reg is 0, else -1 */
    uint16_t slp_typ;    /* the sleep type that means off */
    uint16_t smi_port;   /* 0 when this profile has no SMI command */
    uint8_t acpi_enable; /* the value that asks firmware to enable ACPI */
} board_profile_t;

/* PM1_CNT's offset in the ACPI block, and the two of its bits used here. */
#define BOARD_PM1_CNT_OFFSET 4
#define BOARD_PM1_CNT_SLP_EN 0x2000u
#define BOARD_PM1_CNT_SCI_EN 0x0001u

/* What `board_init` decided. */
#define BOARD_ABSENT  0 /* nothing matched; the power off has \_S5 alone */
#define BOARD_FOUND   1
#define BOARD_REFUSED 2 /* something matched and a check failed */

/** A profile by name, or NULL for an unknown name or a NULL `name`. Used by
 * the boot line's override and the test. */
const board_profile_t *board_lookup(const char *name);

/** Whether the identity read out of configuration space is this profile's;
 * 0 for a NULL profile. */
int board_id_matches(const board_profile_t *p, uint16_t vendor, uint16_t device, uint8_t class_code,
                     uint8_t subclass);

/**
 * The ACPI block's base: the bits of the raw base register `base_mask`
 * selects, 0 when they are clear or `p` is NULL.
 *
 * A base of 0 is one firmware never programmed, which the caller refuses:
 * bit 0 of the register is hardwired to 1 as the I/O-space indicator, so a
 * nonzero register does not show that a base was set, and a base of 0 would
 * send the sleep word to port 4.
 */
uint16_t board_pm_base(const board_profile_t *p, uint32_t base_reg_value);

/** Whether the chipset says its ACPI I/O range answers at all; 0 for a NULL
 * profile. */
int board_pm_decoding(const board_profile_t *p, uint8_t decode_reg_value);

/**
 * Which interrupt line the SCI is on, or -1 when the line is not one
 * `pic_mask_irq` can mask or `p` is NULL. A profile without a selection
 * register has its fixed line. Selections 0, 1 and 2 are lines 9, 10 and
 * 11; 4 through 7 route the SCI to I/O APIC inputs above 15, and 3 is
 * reserved.
 */
int board_sci_irq(const board_profile_t *p, uint8_t sci_reg_value);

/** The word that asks for soft off: the sleep type in bits 10 to 12, and
 * SLP_EN; 0 for a NULL profile. */
uint16_t board_sleep_word(const board_profile_t *p);

/** Whether a PM1_CNT read came from a chipset, not from an empty bus. */
int board_pm_answers(uint16_t pm1_cnt_value);

/** Whether ACPI still has to be asked for: the profile has an SMI command
 * port and an enable value, and PM1_CNT answers with SCI_EN clear. */
int board_needs_acpi_enable(const board_profile_t *p, uint16_t pm1_cnt_value);

/**
 * Identify the power controller, ask firmware for ACPI mode where the
 * profile has a way to, and mask the SCI when it can fire.
 *
 * The profiles are tried in table order, or only the one `rach2.board`
 * names, and the first whose identity the chipset has decides. Returns
 * BOARD_FOUND, and `board_pm1_cnt_port` is then the port the power off
 * writes the sleep word to; BOARD_REFUSED, with `*why` the reason for the
 * boot line, when the identified chipset fails a check or `rach2.board`
 * names a profile this kernel does not have or a chipset this machine does
 * not have; or BOARD_ABSENT. `*why` is "" except on a refusal. `rach2.nosci`
 * leaves the SCI unmasked. Called once, at boot, from `platform_board_init`.
 */
int board_init(const char **why);

int board_state(void);
/** The name of the profile whose identity `board_init` matched, whether or
 * not it passed the checks after, or "none". */
const char *board_name(void);
/** The port the power off writes the sleep word to, or 0 unless `board_init`
 * returned BOARD_FOUND. */
uint16_t board_pm1_cnt_port(void);
int board_sci_line(void);

/** The profile in force, or NULL unless `board_init` returned BOARD_FOUND. */
const board_profile_t *board_current(void);

#endif /* _BOARD_H_ */
