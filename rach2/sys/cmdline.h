#ifndef _CMDLINE_H_
#define _CMDLINE_H_

/**
 * Capture the multiboot kernel command line into a kernel-owned buffer.
 *
 * The bootloader leaves the command-line string in low memory that the bump
 * allocator may later carve over, so the boot path copies it here once, in the
 * pre-paging window, before anything can clobber it. A NULL or absent command
 * line leaves the captured line empty. A line longer than the buffer is
 * truncated.
 */
void kcmdline_capture(const char *cmdline);

/**
 * Non-zero if the captured command line contains `flag` as a whole token.
 *
 * Tokens are whitespace-delimited, so `kcmdline_has("rach2.rescue")` matches
 * the standalone argument and not a substring of a longer one.
 */
int kcmdline_has(const char *flag);

/**
 * Copy the value of the `key=value` token into `out`.
 *
 * `key` is written without the `=`, and the match is on a whole token, so
 * `rach2.ip` finds `rach2.ip=10.0.0.5/24` and not `norach2.ip=...`. The first
 * occurrence wins; a second one is a contradiction the caller should refuse,
 * so `kcmdline_count` is there to notice it.
 *
 * A value too long for `out` is refused rather than truncated: half an address
 * parses into a different address, and a machine that quietly used one the
 * operator did not write is worse than one that refuses to configure.
 *
 * @return 1 when the option was present and its value fit, 0 otherwise.
 */
int kcmdline_value(const char *key, char *out, unsigned int outlen);

/** How many `key=` tokens the line carries; more than one is a
 * contradiction. */
int kcmdline_count(const char *key);

/** The captured command line, a NUL-terminated string (empty if none). */
const char *kcmdline_get(void);

#endif /* _CMDLINE_H_ */
