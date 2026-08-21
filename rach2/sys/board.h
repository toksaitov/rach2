#ifndef _BOARD_H_
#define _BOARD_H_

#include "lib/kint.h"

/*
 * board: which power controller this machine has, and what may be written to
 * it.
 *
 * `power.c` could always park the CPU, and until this file it could ask only
 * one chipset to remove power: QEMU's PIIX4, whose ACPI block it found in PCI
 * configuration space and refused to use unless three checks passed. The
 * laptop this milestone targets has an Intel Lynx Point PCH instead, where the
 * same block lives behind different registers, so a machine asked to power off
 * halted instead. That is a fair failure and a bad experience: the person has
 * to hold the power button, and over SSH there is no person.
 *
 * The shape is a table of profiles, and every profile is identified before a
 * single byte is written. The reason is what a wrong address does. The sleep
 * word is a 16-bit write to two consecutive I/O ports, and the low ports it
 * could land on are the DMA controller, the interrupt controller, the timer,
 * the keyboard controller, the CMOS index and data pair, and the reset control
 * register. A wrong base does not fail quietly; it resets the machine or
 * corrupts its clock. So the base is derived from the chipset's own register,
 * not assumed, and four separate checks must pass.
 *
 * One value cannot be derived and is not: which sleep type means "off". That
 * lives in the firmware's ACPI tables, in AML this kernel does not interpret.
 * It is carried per profile as a constant of belief, with its provenance in
 * the comment, and it is written only to a chipset that was positively
 * identified. It is never swept for: on a PCH, sleep type 5 is suspend, and a
 * machine that suspends after its file system has been flushed and scheduling
 * stopped looks off to the person in the room and is not.
 *
 * The pure half is here so a host test can reach every judgement without a
 * chipset, which matters because the only one of these three profiles that
 * exists on this desk is QEMU's.
 */

/** One chipset's power-management registers, and what may be written to
 * them. */
typedef struct {
    const char *name;
    uint8_t bus, device, function;
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

/* Where PM1_CNT sits inside the ACPI block, and its two bits that matter. */
#define BOARD_PM1_CNT_OFFSET 4
#define BOARD_PM1_CNT_SLP_EN 0x2000u
#define BOARD_PM1_CNT_SCI_EN 0x0001u

/* What `board_init` decided. */
#define BOARD_ABSENT  0 /* nothing matched; a power-off will halt instead */
#define BOARD_FOUND   1
#define BOARD_REFUSED 2 /* something matched and a check failed */

/** A profile by name, or NULL. Used by the boot line's override and the
 * test. */
const board_profile_t *board_lookup(const char *name);

/** Whether the identity read out of configuration space is this profile's. */
int board_id_matches(const board_profile_t *p, uint16_t vendor, uint16_t device, uint8_t class_code,
                     uint8_t subclass);

/**
 * The ACPI block's base from the raw base register, or 0 to refuse.
 *
 * Zero is a refusal and not an address: bit 0 of that register is hardwired to
 * 1 as the I/O-space indicator, so a nonzero register says nothing about
 * whether firmware ever programmed a base, and a base of zero would send the
 * sleep word to port 4.
 */
uint16_t board_pm_base(const board_profile_t *p, uint32_t base_reg_value);

/** Whether the chipset says its ACPI I/O range answers at all. */
int board_pm_decoding(const board_profile_t *p, uint8_t decode_reg_value);

/**
 * Which interrupt line the SCI is on, or -1 when a PIC-only kernel cannot see
 * it. Selections 0, 1 and 2 are lines 9, 10 and 11; 4 through 7 route the SCI
 * to IOAPIC inputs this kernel has no way to receive, and 3 is reserved.
 */
int board_sci_irq(const board_profile_t *p, uint8_t sci_reg_value);

/** The word that asks for soft off: the sleep type in place, plus SLP_EN. */
uint16_t board_sleep_word(const board_profile_t *p);

/** Whether a PM1_CNT read came from a chipset, not from an empty bus. */
int board_pm_answers(uint16_t pm1_cnt_value);

/** Whether ACPI still has to be asked for, given what PM1_CNT reports. */
int board_needs_acpi_enable(const board_profile_t *p, uint16_t pm1_cnt_value);

/**
 * Identify the power controller, and mask the SCI when one can fire.
 *
 * Called once at boot. On success `board_pm1_cnt_port` gives the port
 * `power.c` writes the sleep word to; on anything else it gives 0 and the
 * power-off halts, which is what it did before this file existed. `*why` names
 * the refusal for the boot line.
 */
int board_init(const char **why);

/** What `board_init` decided, the profile's name, and the port to write. */
int board_state(void);
const char *board_name(void);
uint16_t board_pm1_cnt_port(void);
int board_sci_line(void);

/** The profile in force, or NULL when none was identified. */
const board_profile_t *board_current(void);

#endif /* _BOARD_H_ */
