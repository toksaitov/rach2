/*
 * The string, character, token, integer and path operations of ish, which
 * stand in for the C library's. A pointer argument must not be null; most of
 * the functions end the process through `ish_check` when one is.
 */

#ifndef ISH_CSTRING_UTILITIES_H
#define ISH_CSTRING_UTILITIES_H

#include "ish_common.h"

/* The separator of a path's components, and that of the elements of PATH. */
static const char ISH_Directory_Separator = '/';
static const char ISH_Path_Separator = ':';

/** Length of the NUL-terminated `string`, not counting the terminator. */
unsigned long ish_get_cstring_length(const char *string);

/**
 * Replace the first `character` in `string` with `replacement`, in place,
 * and leave later ones alone. Returns `string`.
 */
char *ish_replace_first_character_in_cstring(char *string, char character, char replacement);

/** Whether `character` is a space, `\t`, `\n`, `\v`, `\f` or `\r`. */
int ish_is_space(int character);
/** Whether `character` is an ASCII decimal digit. */
int ish_is_digit(int character);
/** Whether `character` is `ISH_Path_Separator`. */
int ish_is_path_separator(int character);

/** Whether `character` is an ASCII lowercase letter. */
int ish_is_lowercase(int character);
/** Whether `character` is an ASCII uppercase letter. */
int ish_is_uppercase(int character);
/** `character` as lowercase if it is an ASCII uppercase letter, else itself. */
int ish_convert_to_lowercase(int character);
/** `character` as uppercase if it is an ASCII lowercase letter, else itself. */
int ish_convert_to_uppercase(int character);

/**
 * Cut the next token out of `string`, in place.
 *
 * Skips the leading characters for which `is_separator` is true, takes the
 * run up to the next separator as the token, and writes a NUL over that
 * separator, so the token is a string of its own inside the caller's
 * buffer. `ish_get_token_in_cstring` finds a token without writing.
 *
 * Returns the token, or 0 when only separators remain. When
 * `cursor_after_token` is not null it receives the position after the
 * written NUL, where the next call may start, or 0 when `string` ends
 * there.
 */
char *ish_carve_token_in_cstring(char *string, int (*is_separator)(int character),
                                 char **cursor_after_token);

/**
 * Find the next token in `string` without writing to it.
 *
 * As `ish_carve_token_in_cstring`, but the token is not terminated: it runs
 * to the next separator or to the end of `string`.
 *
 * Returns the start of the token, or 0 when only separators remain. When
 * `cursor_after_token` is not null it receives the separator that ends the
 * token, where the next call may start, or 0 at the end of `string`.
 */
char *ish_get_token_in_cstring(char *string, int (*is_separator)(int character),
                               char **cursor_after_token);

/**
 * Whether `string` begins with `prefix`. The empty prefix matches every
 * string.
 */
int ish_cstring_starts_with(const char *prefix, const char *string);

/** As `ish_cstring_starts_with`, comparing ASCII letters without case. */
int ish_cstring_starts_with_ignoring_case(const char *prefix, const char *string);

/** Whether the two NUL-terminated strings have the same bytes. */
int ish_are_cstrings_equal(const char *string, const char *another_string);

/** As `ish_are_cstrings_equal`, comparing ASCII letters without case. */
int ish_are_cstrings_equal_ignoring_case(const char *string, const char *another_string);

/**
 * The signed decimal integer at the start of `string`.
 *
 * Skips leading white space, takes an optional `+` or `-` and then the run of
 * digits, and ignores what follows. An empty or non-numeric field gives 0.
 * There is no overflow report: the value wraps modulo `UINT_MAX + 1` and is
 * converted to `int`. The `exit` builtin uses only its low eight bits.
 */
int ish_get_integer_from_cstring(const char *string);

/**
 * Join `path_element` and `another_path_element` into `combined_path` with
 * one `ISH_Directory_Separator` between them.
 *
 * `path_element` is read up to its first `ISH_Path_Separator`, so an element
 * of PATH can be passed in place. At most `maximum_combined_path_length`
 * bytes are written before the NUL that always ends the result, so
 * `combined_path` needs one byte more than that.
 *
 * Returns 0 when the whole path fits, or -1 when it was cut short. A path cut
 * short can name another file, so a caller must not use it.
 */
int ish_combine_path_elements(const char *path_element, const char *another_path_element,
                              char *combined_path, unsigned long maximum_combined_path_length);

#endif
