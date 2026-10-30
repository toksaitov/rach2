/*
 * call.h: /proc/acpi/call, through which root evaluates an ACPI method of
 * the firmware by its path, as Linux's acpi_call module offers.
 */
#ifndef _ACPI_CALL_H_
#define _ACPI_CALL_H_

#include "../lib/kint.h"

/* The longest request a write of /proc/acpi/call takes, in bytes. */
#define ACPI_CALL_MAX 512

/**
 * Evaluate the method that the `len` bytes at `text` name: its absolute
 * path, such as \_SB.PCI0.LPCB.EC0._Q11, then up to seven arguments, each
 * after a space, a number in decimal or in hexadecimal after 0x, or a
 * string in double quotes; a newline may end it. The result is kept for
 * the next `acpi_call_result`, which any caller reads, and replaces the
 * last. Process context; it may sleep while the method runs. Returns 0, or
 * -EINVAL for text not of that form, -ENODEV while the interpreter is not
 * running, or the error of the evaluation.
 */
int acpi_call_write(const char *text, uint32_t len);

/**
 * Write the result of the last call into `out` of `cap` bytes,
 * NUL-terminated, as the Linux module words it: an integer as 0x and its
 * hexadecimal digits, a string in double quotes, an error as `Error: ` and
 * ACPICA's name for it, and `not called` before the first call. Returns
 * its length.
 */
int acpi_call_result(char *out, int cap);

#endif /* _ACPI_CALL_H_ */
