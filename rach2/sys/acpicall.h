/*
 * acpicall.h: /proc/acpi/call, through which root evaluates an ACPI method
 * of the firmware by its path, as Linux's acpi_call module offers. Shared
 * code calls it; a PC defines it, in sys/acpi/call.c.
 */
#ifndef _ACPICALL_H_
#define _ACPICALL_H_

#include "lib/kint.h"

/* The longest request a write of /proc/acpi/call takes, in bytes. */
#define ACPI_CALL_MAX 512

/**
 * Evaluate the method that the `len` bytes at `text` name: its absolute
 * path, such as \_SB.PCI0.LPCB.EC0._Q11, then up to seven arguments, each
 * after a space, a number in decimal or in hexadecimal after 0x, or a
 * string in double quotes; a newline may end it. The result is kept for
 * the next `acpi_call_result`, which any caller reads, and replaces the
 * last; a method that failed, or that is not there, gives its error as the
 * result, as Linux's module does. Process context; it may sleep while the
 * method runs. Returns 0 once the method was evaluated, -EINVAL for text
 * not of that form, or -ENODEV while the interpreter is not running.
 */
int acpi_call_write(const char *text, uint32_t len);

/**
 * Write the result of the last call into `out` of `cap` bytes,
 * NUL-terminated, as the Linux module words it: an integer as 0x and its
 * hexadecimal digits, a string in double quotes, a buffer as {0x01, 0x02},
 * a package as [A, B], an error as `Error: ` and ACPICA's name for it, and
 * `not called` before the first call; a method that returns nothing gives
 * 0x0. A result longer than ACPI_CALL_MAX - 1 bytes is cut. Returns its
 * length.
 */
int acpi_call_result(char *out, int cap);

#endif /* _ACPICALL_H_ */
