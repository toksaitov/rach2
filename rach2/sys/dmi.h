/*
 * dmi.h: the firmware's description of the machine, the fields of its
 * SMBIOS table that Linux's DMI code keeps and /sys/class/dmi/id shows.
 */
#ifndef _DMI_H_
#define _DMI_H_

/* The fields `dmi_field` gives, as Linux's DMI_ numbers name them, in the
 * order of /sys/class/dmi/id's files. */
#define DMI_BIOS_VENDOR         0
#define DMI_BIOS_VERSION        1
#define DMI_BIOS_DATE           2
#define DMI_BIOS_RELEASE        3
#define DMI_EC_FIRMWARE_RELEASE 4
#define DMI_SYS_VENDOR          5
#define DMI_PRODUCT_NAME        6
#define DMI_PRODUCT_VERSION     7
#define DMI_PRODUCT_SERIAL      8
#define DMI_PRODUCT_UUID        9
#define DMI_PRODUCT_SKU         10
#define DMI_PRODUCT_FAMILY      11
#define DMI_BOARD_VENDOR        12
#define DMI_BOARD_NAME          13
#define DMI_BOARD_VERSION       14
#define DMI_BOARD_SERIAL        15
#define DMI_BOARD_ASSET_TAG     16
#define DMI_CHASSIS_VENDOR      17
#define DMI_CHASSIS_TYPE        18
#define DMI_CHASSIS_VERSION     19
#define DMI_CHASSIS_SERIAL      20
#define DMI_CHASSIS_ASSET_TAG   21
#define DMI_FIELDS              22

/**
 * The text of the field `field`, one of the DMI_ numbers, as Linux's
 * dmi_get_system_info gives it, or NULL when the firmware's table does not
 * hold it: a string, empty when the structure names none or one of spaces
 * alone; a release as "MAJOR.MINOR"; the chassis type in decimal; the UUID
 * in lowercase with its dashes. A string is cut at 63 bytes. The table is
 * read on the first call (arch/x86/smbios.c).
 */
const char *dmi_field(int field);

#endif /* _DMI_H_ */
