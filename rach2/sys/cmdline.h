/*
 * cmdline.h: the kernel command line, kept in a buffer of the kernel's own,
 * and the lookups of its flags and `key=value` options.
 */
#ifndef _CMDLINE_H_
#define _CMDLINE_H_

/**
 * Copy the kernel command line `cmdline` into the kernel's buffer, which
 * every lookup here reads; the shared boot calls it once, early.
 *
 * The buffer holds `BOOT_CMDLINE_MAX` - 2 bytes, 510. A longer line is cut
 * at the last space or tab that fits, so that no token is kept in part: a cut
 * `rach2.disk=ahcix` would otherwise read as `rach2.disk=ahci`. A line of
 * exactly 511 bytes is cut too, since the boot record may have cut a longer
 * one there. A NULL `cmdline` leaves the line empty.
 *
 * @return 1 when the line was cut, 0 when it was kept whole.
 */
int kcmdline_capture(const char *cmdline);

/**
 * Non-zero if the captured command line contains `flag` as a whole token.
 *
 * Tokens are delimited by spaces and tabs, so `kcmdline_has("rach2.rescue")`
 * matches the standalone argument and not a substring of a longer one.
 */
int kcmdline_has(const char *flag);

/**
 * Copy the value of the `key=value` token into `out`.
 *
 * `key` is written without the `=`, and the match is on a whole token, so
 * `rach2.ip` finds `rach2.ip=10.0.0.5/24` and not `norach2.ip=...`. The first
 * occurrence wins; a caller for which a second one is a contradiction finds
 * it with `kcmdline_count`.
 *
 * A value too long for `out` is not truncated: the call returns 0, since half
 * an address parses into a different address, and a machine that used one
 * the operator did not write is worse off than one left unconfigured.
 *
 * @return 1 when the option was present and its value fit, 0 otherwise, with
 * `out` empty.
 */
int kcmdline_value(const char *key, char *out, unsigned int outlen);

/**
 * The value of the `nth` `key=value` token, counting from zero.
 *
 * A key a machine may give more than once, one per interface, is read this
 * way; `kcmdline_value` is this with `nth` zero.
 *
 * @return 1 when there is such a token and its value fit, 0 otherwise.
 */
int kcmdline_value_nth(const char *key, int nth, char *out, unsigned int outlen);

/**
 * The value of the boolean option `key`, read as Linux's `kstrtobool` reads
 * a kernel parameter: a value that begins with y, Y, t, T or 1, or with on
 * in either case, is true; one that begins with n, N, f, F or 0, or with of
 * in either case, is false; any other is ignored. Each `key=value` token is
 * read in order, so the last one read wins, as Linux runs an early
 * parameter once for each occurrence.
 *
 * @return the value, or `dflt` when no token gives one.
 */
int kcmdline_bool(const char *key, int dflt);

/**
 * The boolean module parameter `key`, read as `kcmdline_bool` reads it,
 * except that a bare `key` token is true, as Linux's param_set_bool takes
 * it: "No equals means set" (Linux 6.12, kernel/params.c:312 to 319).
 */
int kcmdline_param_bool(const char *key, int dflt);

/**
 * Read the text `text` as Linux's `kstrtobool` reads it, as `kcmdline_bool`
 * reads each value, and set `*out` to 1 or 0.
 *
 * @return 0, or -1 for a text that is no boolean, which leaves `*out` alone.
 */
int kcmdline_parse_bool(const char *text, int *out);

/** How many `key=` tokens the line has. */
int kcmdline_count(const char *key);

/** How many tokens of the line are `flag` whole, with no `=`. */
int kcmdline_bare_count(const char *flag);

/** The captured command line, a NUL-terminated string (empty if none). */
const char *kcmdline_get(void);

#endif /* _CMDLINE_H_ */
