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
 * Tokens are whitespace-delimited, so `kcmdline_has("rach2.rescue")` matches the
 * standalone argument and not a substring of a longer one.
 */
int kcmdline_has(const char *flag);

/** The captured command line, a NUL-terminated string (empty if none). */
const char *kcmdline_get(void);

#endif /* _CMDLINE_H_ */
