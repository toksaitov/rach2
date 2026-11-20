/*
 * devices.h: the ACPI devices of the kernel's own on ACPICA, the embedded
 * controller, the buttons, the video bus, the IdeaPad's VPC2004 and the PCI
 * hot plug slots, and the PNP IDs of the 8042's devices, as boot.c starts
 * them.
 */
#ifndef _ACPI_DEVICES_H_
#define _ACPI_DEVICES_H_

#include "../lib/kint.h"

struct acpi_resource;
struct evdev;

/* What a resource of a _CRS describes (`acpi_resource_range`). */
#define ACPI_RANGE_IO     1 /* I/O ports */
#define ACPI_RANGE_MEMORY 2 /* memory */
#define ACPI_RANGE_BUS    3 /* bus numbers, which only an address descriptor gives */

/**
 * Find the embedded controller, PNP0C09, and install the handler of its
 * address space, without running the firmware's _REG for it. Called once
 * the namespace is loaded and before AcpiEnableSubsystem and the objects'
 * _INI, which may read the EC's fields. Logs the EC found, or nothing when
 * there is none.
 */
void acpi_ec_probe(void);

/**
 * Run the EC's _REG and take its GPE, whose SCI_EVT runs its queries. Called
 * after the objects' _INI, since a firmware's _REG may read what _INI set,
 * as the Lenovo G50-70's reads OSYS, and before AcpiUpdateAllGpes.
 */
void acpi_ec_start(void);

/**
 * Register the power and sleep buttons as event devices, the FADT's fixed
 * buttons and the devices PNP0C0C and PNP0C0E, and take their events.
 * Called once ACPICA's objects are initialized.
 */
void acpi_buttons_start(void);

/** Find the video buses, register each as an event device, call its _DOS,
 * and take the notifications of the bus and its outputs. As above. */
void acpi_video_start(void);

/**
 * Make the first output with _BCL and _BCM that `acpi_video_start` found
 * the backlight acpi_video0, unless a backlight is registered by then, as
 * Linux's acpi_video_register_backlight waits for the GPU drivers, so that
 * a native backlight registered late, as i915's where the firmware left
 * the PWM off, is the panel's only one. Once, from `arch_drm_probe` after
 * the native display drivers, in process context.
 */
void acpi_video_register_backlight(void);

/**
 * Take the IdeaPad's device VPC2004, when there is one: register its keys
 * as the event device "Ideapad extra buttons" and its Wi-Fi switch as the
 * rfkill switch ideapad_wlan, turn the controller's radio switch on, and
 * take its Notify. Called after acpi_ec_start, since its methods reach the
 * EC, in process context.
 */
void acpi_ideapad_start(void);

/**
 * Find the 8042's keyboard and PS/2 mouse among the namespace's present
 * devices with _CRS, as Linux's i8042 PNP driver finds them, and record
 * their PNP IDs with `i8042_pnp_device`, so that the mouse's bring-up after
 * this can match the touchpad's quirks by them. Called once ACPICA's
 * objects are initialized and the EC runs, since a _STA may read it.
 */
void acpi_pnp_i8042(void);

/**
 * Hold for the host, so that no PCI BAR over them is claimed
 * (`pci_host_reserve`), the FADT's fixed register blocks, as Linux's
 * acpi_reserve_resources, and the ports from 0x100 up and the memory of
 * each present system device's _CRS, PNP0C01 or PNP0C02, as Linux's
 * pnp/system.c. Called once ACPICA's objects are initialized and the EC
 * runs, since a _STA may read it.
 */
void acpi_pnp_reserve(void);

/**
 * The range a resource `res` of a _CRS describes, as Linux's
 * acpi_dev_resource_memory, _io, _address_space and _ext_address_space
 * read it: an I/O or fixed I/O descriptor's ports; a 24-bit, 32-bit or
 * fixed 32-bit descriptor's memory, a 24-bit one's address and length
 * shifted up by 8; and a Word, DWord, QWord or Extended address
 * descriptor's memory, ports or bus numbers, its translation added to its
 * start. Sets `*start` and `*len`, which may be 0, and `*address` to 1 for
 * an address descriptor, the only kind a bridge's window comes in, and
 * returns an `ACPI_RANGE_` kind; 0 for any other resource.
 */
int acpi_resource_range(struct acpi_resource *res, uint64_t *start, uint64_t *len, int *address);

/**
 * Take the hot plug slots of the host bridge of bus 0, its children with
 * _ADR and _EJ0, and their Notify, as Linux's acpiphp does, with the
 * bridge's windows from its _CRS for the BARs of a card put in. Called
 * once ACPICA's objects are initialized and before AcpiUpdateAllGpes,
 * whose GPE method gives the Notify.
 */
void acpi_pcihp_start(void);

/** Report `code` pressed and then released on `dev`, each in a packet of
 * its own, as Linux's ACPI drivers report a button, and poke the console's
 * blanking as a keystroke does (`console_poke`). Any context. */
void acpi_report_key(struct evdev *dev, uint16_t code);

#endif /* _ACPI_DEVICES_H_ */
