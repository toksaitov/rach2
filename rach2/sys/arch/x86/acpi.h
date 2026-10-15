#ifndef _ACPI_H_
#define _ACPI_H_

#include "../../lib/kint.h"

/*
 * acpi: the firmware's ACPI tables, read once at boot into one record that
 * the rest of the platform reads: the processors, I/O APICs, interrupt
 * overrides and NMI wiring of the MADT; the power registers, the PM timer,
 * the reset register and the boot flags of the FADT; the HPET's address; the
 * PCI configuration windows of the MCFG; and the sleep type of the soft-off
 * state, \_S5, found by a search of the DSDT's bytes. There is no AML
 * interpreter, so nothing else the DSDT says is known: no _PRT, no _CRS, no
 * methods.
 *
 * Every table is reached through the kernel's MMIO mapping and checked
 * before a field of it is read: its length against its header and against
 * what its fields need, and its checksum. A table that fails is logged as
 * `acpi: SIG at ADDR: why` and left out of the record.
 */

#define ACPI_MAX_CPUS      64
#define ACPI_MAX_IOAPICS   8
#define ACPI_MAX_OVERRIDES 16
#define ACPI_MAX_NMIS      8
#define ACPI_MAX_TABLES    32
#define ACPI_MAX_ECAM      4

/* A Generic Address Structure: a register's address space, width and
 * address. The PM registers, the reset register, the PM timer and the HPET
 * are each given by one. */
struct acpi_gas {
    uint8_t space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
};

#define ACPI_SPACE_MEMORY 0
#define ACPI_SPACE_IO     1
#define ACPI_SPACE_PCI    2

/* A processor of the MADT: its local APIC's identity, the ACPI processor
 * identity the NMI entries name it by, and whether it may be started. */
struct acpi_cpu {
    uint32_t apic_id;
    uint32_t uid;
    uint32_t flags;
};

#define ACPI_CPU_ENABLED        0x1u
#define ACPI_CPU_ONLINE_CAPABLE 0x2u

/* An I/O APIC of the MADT: its APIC identity, the physical address of its
 * registers, and the global system interrupt of its first input. */
struct acpi_ioapic {
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
};

/* An interrupt source override: ISA line `source` arrives at global system
 * interrupt `gsi`, with the polarity and trigger of `flags`. */
struct acpi_override {
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
};

/* The polarity and trigger bits of an override's or an NMI entry's flags,
 * the MPS INTI flags. "Conforms" means the bus's own: active high and edge
 * for ISA, active low and level for PCI. */
#define ACPI_POLARITY_MASK 0x3u
#define ACPI_POLARITY_HIGH 0x1u
#define ACPI_POLARITY_LOW  0x3u
#define ACPI_TRIGGER_MASK  0xCu
#define ACPI_TRIGGER_EDGE  0x4u
#define ACPI_TRIGGER_LEVEL 0xCu

/* A local APIC's NMI input: processor `uid` (ACPI_NMI_ALL for every one)
 * takes its NMI on `lint`, 0 or 1. */
struct acpi_nmi {
    uint32_t uid;
    uint16_t flags;
    uint8_t lint;
};

#define ACPI_NMI_ALL 0xFFFFFFFFu

/* An NMI source at the I/O APICs. */
struct acpi_nmi_source {
    uint32_t gsi;
    uint16_t flags;
};

/* A window of the MCFG: the configuration space of buses `bus_first` to
 * `bus_last` of PCI segment `segment`, a mebibyte a bus from `base`. */
struct acpi_ecam {
    uint64_t base;
    uint16_t segment;
    uint8_t bus_first;
    uint8_t bus_last;
};

/* A table the root table listed, and whether it passed its checks. */
struct acpi_table {
    char sig[5];
    uint64_t address;
    uint32_t length;
    int ok;
};

/* The FADT's flags this kernel reads. */
#define ACPI_FADT_TMR_VAL_EXT   (1u << 8)
#define ACPI_FADT_RESET_REG_SUP (1u << 10)
#define ACPI_FADT_HW_REDUCED    (1u << 20)

/* IAPC_BOOT_ARCH, the FADT's word about the PC's legacy devices. */
#define ACPI_BOOT_LEGACY_DEVICES 0x0001u
#define ACPI_BOOT_8042           0x0002u
#define ACPI_BOOT_NO_VGA         0x0004u
#define ACPI_BOOT_NO_MSI         0x0008u
#define ACPI_BOOT_NO_CMOS_RTC    0x0020u

/* PM1 control: the sleep type's field and the bit that enters it, and the
 * bit that says the machine is in ACPI mode. */
#define ACPI_PM1_SCI_EN      0x0001u
#define ACPI_PM1_SLP_TYP_POS 10
#define ACPI_PM1_SLP_TYP     (7u << ACPI_PM1_SLP_TYP_POS)
#define ACPI_PM1_SLP_EN      (1u << 13)

/* The record. Zero where a table was absent or refused; `madt`, `fadt`,
 * `s5` and `hpet` are 1 when their table, or the \_S5 package, was read. */
struct acpi_info {
    int found;        /* an RSDP passed its checks */
    uint64_t rsdp;    /* its physical address */
    uint8_t revision; /* its revision: 2 and up have an XSDT */
    char oem[7];
    int xsdt;      /* the root table is the XSDT, not the RSDT */
    uint64_t root; /* the root table's physical address */
    int ntables;
    struct acpi_table tables[ACPI_MAX_TABLES];

    int madt;
    uint64_t lapic_address;
    uint32_t madt_flags; /* bit 0, PCAT_COMPAT: an 8259 pair is present */
    int ncpus;
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    int nioapics;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    int noverrides;
    struct acpi_override overrides[ACPI_MAX_OVERRIDES];
    int nnmis;
    struct acpi_nmi nmis[ACPI_MAX_NMIS];
    int nnmi_sources;
    struct acpi_nmi_source nmi_sources[ACPI_MAX_NMIS];
    int madt_dropped; /* entries past a limit above, counted and left out */

    int fadt;
    uint8_t fadt_revision;
    uint32_t fadt_flags;
    uint16_t sci_irq;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    struct acpi_gas pm1a_cnt, pm1b_cnt, pm_timer;
    int pm_timer_bits; /* the PM timer's width: 24, or 32 by TMR_VAL_EXT */
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    int reset_supported;           /* RESET_REG_SUP, and a register given */
    struct acpi_gas sleep_control; /* a hardware-reduced machine's PM1 */
    uint16_t boot_flags;
    uint8_t century; /* the CMOS register of the century, 0 for none */
    uint64_t dsdt;   /* X_DSDT where it is given, else DSDT */
    uint64_t dsdt32; /* the DSDT field, read when X_DSDT cannot be */

    int s5;
    uint8_t slp_typa, slp_typb;

    int hpet;
    uint64_t hpet_address;
    uint32_t hpet_id;
    uint8_t hpet_number;

    int necam;
    struct acpi_ecam ecam[ACPI_MAX_ECAM];
};

/*
 * The parser's view of memory: `len` bytes of physical memory at `phys`, or
 * NULL when they cannot be reached. The kernel's maps them through
 * `map_mmio`; a host test hands out a model.
 */
typedef const uint8_t *(*acpi_map_fn)(uint64_t phys, uint32_t len);

/** Whether the `len` bytes at `p` sum to zero, as every table's do. */
int acpi_checksum_ok(const uint8_t *p, uint32_t len);

/**
 * The physical address of the RSDP, found where a BIOS leaves it: the first
 * kibibyte of the extended BIOS data area, then 0xE0000 to 0xFFFFF, on
 * sixteen-byte boundaries, with its checksums good; 0 when there is none.
 */
uint64_t acpi_rsdp_search(acpi_map_fn map);

/**
 * Read the tables the RSDP at `rsdp` leads to into `out`: the XSDT when the
 * RSDP has one and it passes, the RSDT otherwise, and from it the MADT, the
 * FADT, the HPET, the MCFG and the FADT's DSDT. 0 when the RSDP was good,
 * -1 when it was not and `out` is empty.
 */
int acpi_parse(acpi_map_fn map, uint64_t rsdp, struct acpi_info *out);

/**
 * The two sleep types of \_S5 in the AML at `aml`: the name `_S5_` after
 * NameOp, then a package whose first two elements are Zero, One, or a
 * constant after BytePrefix, WordPrefix or DWordPrefix, each at most 7.
 * 0 with `*typa` and `*typb` set, -1 when no such definition is found.
 */
int acpi_s5_find(const uint8_t *aml, uint32_t len, uint8_t *typa, uint8_t *typb);

/* Where an interrupt line arrives at the I/O APICs, and how it signals. */
struct acpi_route {
    uint32_t gsi;
    int level; /* level-triggered; 0 for an edge */
    int low;   /* active low; 0 for active high */
};

/**
 * Route ISA line `line`, 0 to 15, through the MADT's overrides: the global
 * system interrupt it arrives on, and its trigger and polarity. A line a
 * PCI device uses, `pci` nonzero, is level-triggered whatever the override
 * says, and active low unless the override gives a polarity. -1 when the
 * line has no input of its own, because an override moved another line
 * onto its interrupt (ISA line 2, when line 0 arrives at 2).
 */
int acpi_isa_route(const struct acpi_info *a, int line, int pci, struct acpi_route *out);

/**
 * Read the tables, from the boot record's RSDP when `rsdp` is not 0 and
 * from the BIOS area otherwise, into the record `acpi_get` returns, and log
 * what was found as `acpi: tables: ...` and a line for each table read. The
 * boot calls it once, with paging on, since every table is reached through
 * `map_mmio`.
 */
void acpi_init(uint64_t rsdp);

/** The record `acpi_init` filled. */
const struct acpi_info *acpi_get(void);

/**
 * Read the register `g` describes into `*value`, in its own space and
 * width: an I/O port, memory, or PCI configuration space of bus 0. A PCI
 * register of 2 or 4 bytes is read at its offset rounded down to that
 * width, as `acpi_gas_write` writes it. 0, or -1 for a register this kernel
 * cannot reach.
 */
int acpi_gas_read(const struct acpi_gas *g, uint32_t *value);

/** Write `value` to the register `g` describes, in its own space and
 * width: 0, or -1 for a register this kernel cannot reach. */
int acpi_gas_write(const struct acpi_gas *g, uint32_t value);

#endif /* _ACPI_H_ */
