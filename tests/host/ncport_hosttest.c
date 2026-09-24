/**
 * Check the port operand `nc` accepts.
 *
 * `parse_port` is called directly; nothing is resolved or connected. A port
 * is decimal digits from 1 through 65535. A value past that range must be
 * refused and not cut to 16 bits, which would send `65616` to port 80, and a
 * trailing character must be refused and not ignored.
 */
#include <stdio.h>

static int failures;

/** Stand in for the resolver, which no row reaches. */
int resolve_host(const char *host, unsigned int *out)
{
    (void)host;
    *out = 0x7f000001;
    return 0;
}

#define main nc_main
#include "nc.c"
#undef main

/** Require `parse_port` to answer `want` for `text`. */
static void check_port(const char *text, int want, const char *detail)
{
    if (parse_port(text) != want) {
        fprintf(stderr, "ncport-hosttest: fail: %s\n", detail);
        failures++;
    }
}

/**
 * Run the rows, the twenty-digit one last.
 *
 * That row runs only once the others have passed. An accumulator with no
 * bound overflows on it, and the sanitizer reports an overflow by ending the
 * run, so the rows above are left to name a fault they can see first.
 */
int main(void)
{
    check_port("80", 80, "a port in range is accepted");
    check_port("1", 1, "the lowest port is accepted");
    check_port("65535", 65535, "the highest port is accepted");
    check_port("0", 0, "port 0 is refused");
    check_port("65536", 0, "a port past 65535 is refused");
    check_port("65616", 0, "a port past 65535 is not cut to 16 bits");
    check_port("80x", 0, "a trailing character is refused");
    check_port("", 0, "an empty port is refused");
    check_port("+80", 0, "a sign is refused");
    check_port("-80", 0, "a sign is refused");
    check_port(" 80", 0, "a leading space is refused");
    if (failures == 0) {
        check_port("99999999999999999999", 0, "a port of twenty digits is refused");
    }
    printf("ncport-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
