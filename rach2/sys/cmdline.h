/*
 * cmdline.h: the kernel command line, kept in a buffer of the kernel's own,
 * and the lookups of its flags and `key=value` options.
 *
 * Every lookup splits the line into words as Linux's next_arg does
 * (lib/cmdline.c, lines 227 to 274): words are parted by spaces, which are
 * the bytes Linux's isspace takes, but not inside double quotes, and the
 * quotes around a word or a value are dropped, so `"maxcpus=4"` and
 * `maxcpus="4"` both give `maxcpus` the value 4. A word's name ends at its
 * first `=`, and names are compared as Linux's parameq compares them
 * (kernel/params.c, lines 79 to 100), with `-` and `_` alike, so that
 * `intel-iommu=off` is `intel_iommu=off`. The lookups stop at a bare `--`,
 * as Linux's parse_args does (kernel/params.c, lines 181 to 184), since the
 * words after it are init's.
 */
#ifndef _CMDLINE_H_
#define _CMDLINE_H_

/**
 * Copy the kernel command line `cmdline` into the kernel's buffer, which
 * every lookup here reads; the shared boot calls it once, early.
 *
 * The buffer holds `BOOT_CMDLINE_MAX` - 2 bytes, 510. A longer line is cut
 * before the last word that fits, unless a space ends that word, so that no
 * word is kept in part: a cut `rach2.disk=ahcix` would otherwise read as
 * `rach2.disk=ahci`. A quoted value's spaces do not end its word. A line of
 * exactly 511 bytes is cut too, since the boot record may have cut a longer
 * one there. A NULL `cmdline` leaves the line empty.
 *
 * @return 1 when the line was cut, 0 when it was kept whole.
 */
int kcmdline_capture(const char *cmdline);

/**
 * Non-zero if the captured command line contains `flag` as a whole word,
 * with no `=`, so that `kcmdline_has("rach2.rescue")` matches the standalone
 * argument and not a substring of a longer one. A `flag` of the form
 * `name=value` matches a word with that name whose value is `value` byte for
 * byte, so that `kcmdline_has("oops=panic")` matches `oops=panic` and
 * `oops="panic"` but not `oops=panicx`.
 */
int kcmdline_has(const char *flag);

/**
 * Non-zero if a word of the line names the early parameter `name`, bare or
 * with any value, as Linux's do_early_param runs the setup of a flag such
 * as `noapic` or `nox2apic` for `noapic=1` too.
 */
int kcmdline_early(const char *name);

/**
 * Non-zero if a word of the line begins with `str`, read as Linux's
 * obsolete_checksetup reads a word for a `__setup` string with no `=`
 * (init/main.c, lines 195 to 222): the word's name, its `=` and its value
 * are compared over the length of `str`, so that `nomodeset`, `nomodeset=1`
 * and `nomodesetx` all match `nomodeset`.
 */
int kcmdline_setup(const char *str);

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
 * Read a list option as Linux reads one: every `key=` token in the line's
 * order, each value a list of items separated by commas, each item taken
 * as the first of the `nwords` `words` it begins with and passed over when
 * it begins with none. A token that is `flag`, bare or with a value, when
 * `flag` is not NULL, is taken as an item that is `words[0]`, as Linux's
 * `nointremap` sets what `intremap=off` sets. `words` holds at most 16.
 *
 * @return a mask with bit `w` set when any item was `words[w]`; `*last`,
 * when `last` is not NULL, is set to the index of the last item taken and
 * left alone when none was.
 */
int kcmdline_list(const char *key, const char *flag, const char *const *words, int nwords,
                  int *last);

/**
 * Read the integer value of the `nth` `key=value` token, counting from zero,
 * into `*out`, as Linux's get_option reads an early parameter's
 * (lib/cmdline.c, lines 56 to 79): an optional `-`, then simple_strtoull's
 * base 0, hexadecimal after `0x` and a hexadecimal digit, octal after a
 * leading 0 and decimal otherwise, cut to an int. A value that begins with
 * no digit reads 0, as there, since get_option stores the 0 simple_strtoull
 * gives it; an empty value stores nothing, as get_option stores nothing for
 * an empty string. The value is read in place, so its length is not limited.
 *
 * @return 1 when the token is present and `*out` was set, 0 when its value
 * is empty, -1 when the line has no such token; `*out` is untouched but
 * for 1.
 */
int kcmdline_option_nth(const char *key, int nth, int *out);

/**
 * Read every `key=value` token in turn with `kcmdline_option_nth`, as Linux
 * runs an early parameter once for each occurrence, so that `*out` holds the
 * value the last one that stores gives.
 *
 * @return 1 when any token stored a value, 0 otherwise, `*out` then
 * untouched.
 */
int kcmdline_option(const char *key, int *out);

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
 * The boolean module parameter `key`, read as `kcmdline_param_bool` reads
 * it, which a word that `kcmdline_setup` matches to the `__setup` string
 * `setup` sets to `setval`, each word in the line's order, so that the last
 * of either kind counts. Linux's parse_args gives each word to the
 * parameter it names (parse_one), and a word that names none to
 * unknown_bootoption, whose obsolete_checksetup runs the `__setup` it
 * begins with (Linux 6.12, kernel/params.c and init/main.c), so that
 * `printk.console_suspend=Y no_console_suspend` leaves `console_suspend` N
 * and the other order leaves it Y.
 */
int kcmdline_param_bool_setup(const char *key, const char *setup, int setval, int dflt);

/**
 * The int module parameter `key`, read as Linux's param_set_int reads it
 * (Linux 6.12.115, kernel/params.c:237): each `key=value` token in the
 * line's order whose value kstrtoint takes in base 0, an optional sign,
 * then hexadecimal after `0x`, octal after a leading 0 or decimal, and at
 * most one newline, all of it within an int, sets the value, so that the
 * last such token wins. A bare `key` or any other value sets nothing, as
 * parse_one refuses it (kernel/params.c:117-157).
 *
 * @return the value, or `dflt` when no token gives one.
 */
int kcmdline_param_int(const char *key, int dflt);

/**
 * The unsigned int module parameter `key`, read as Linux's param_set_uint
 * reads it (kernel/params.c:238), as `kcmdline_param_int` reads an int but
 * with kstrtouint: an optional `+`, no `-`, all of it within 32 bits.
 *
 * @return the value, or `dflt` when no token gives one.
 */
unsigned int kcmdline_param_uint(const char *key, unsigned int dflt);

/**
 * Read the text `text` as Linux's `kstrtobool` reads it, as `kcmdline_bool`
 * reads each value, and set `*out` to 1 or 0.
 *
 * @return 0, or -1 for a text that is no boolean, which leaves `*out` alone.
 */
int kcmdline_parse_bool(const char *text, int *out);

/**
 * The `nth` token, counting from zero, that is the parameter `key`, bare or
 * with a value, so that a caller reads each occurrence in the line's order,
 * as Linux's `parse_args` meets them (kernel/params.c:161-186).
 *
 * @return 1 for `key=value`, with the value in `out`, or empty when it does
 * not fit; 0 for a bare `key`, with `out` empty; -1 when the line has no
 * such token.
 */
int kcmdline_param_nth(const char *key, int nth, char *out, unsigned int outlen);

/** How many `key=` tokens the line has. */
int kcmdline_count(const char *key);

/** How many tokens of the line are `flag` whole, with no `=`. */
int kcmdline_bare_count(const char *flag);

/** The captured command line, a NUL-terminated string (empty if none). */
const char *kcmdline_get(void);

#endif /* _CMDLINE_H_ */
