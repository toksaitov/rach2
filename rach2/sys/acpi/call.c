/*
 * call.c: /proc/acpi/call (call.h). There is no ACPI interpreter yet, so a
 * call fails and the result is that none was made.
 */

#include "call.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"

int acpi_call_write(const char *text, uint32_t len)
{
    (void)text;
    (void)len;
    return -ENODEV;
}

int acpi_call_result(char *out, int cap)
{
    static const char none[] = "not called";
    int n = (int)sizeof(none) - 1;

    if (cap <= 0) {
        return 0;
    }
    n = n < cap - 1 ? n : cap - 1;
    k_memcpy(out, none, (size_t)n);
    out[n] = '\0';
    return n;
}
