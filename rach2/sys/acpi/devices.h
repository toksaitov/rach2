/*
 * devices.h: the ACPI devices of the kernel's own on ACPICA, the embedded
 * controller, the buttons, the video bus and the IdeaPad's VPC2004, as
 * boot.c starts them.
 */
#ifndef _ACPI_DEVICES_H_
#define _ACPI_DEVICES_H_

#include "../lib/kint.h"

struct evdev;

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
 * Take the IdeaPad's device VPC2004, when there is one: register its keys
 * as the event device "Ideapad extra buttons" and its Wi-Fi switch as the
 * rfkill switch ideapad_wlan, turn the controller's radio switch on, and
 * take its Notify. Called after acpi_ec_start, since its methods reach the
 * EC, in process context.
 */
void acpi_ideapad_start(void);

/** Report `code` pressed and then released on `dev`, each in a packet of
 * its own, as Linux's ACPI drivers report a button, and poke the console's
 * blanking as a keystroke does (`console_poke`). Any context. */
void acpi_report_key(struct evdev *dev, uint16_t code);

#endif /* _ACPI_DEVICES_H_ */
