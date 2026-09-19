#ifndef ISH_CSTRING_UTILITIES_H
#define ISH_CSTRING_UTILITIES_H

#include "ish_common.h"

static const char ISH_Directory_Separator = '/';
static const char ISH_Path_Separator = ':';

/** Length of the NUL-terminated `string`, not counting the terminator. */
unsigned long ish_get_cstring_length(const char *string);

/** Replace the first `character` in `string` with `replacement`, in place. */
char *ish_replace_first_character_in_cstring(char *string, char character, char replacement);

int ish_is_space(int character);
int ish_is_digit(int character);
int ish_is_path_separator(int character);

int ish_is_lowercase(int character);
int ish_is_uppercase(int character);
int ish_convert_to_lowercase(int character);
int ish_convert_to_uppercase(int character);

/** Carve the first separated token out of `string`, mutating the source. */
char *ish_carve_token_in_cstring(char *string, int (*is_separator)(int character),
                                 char **cursor_after_token);

/** Find the first separated token in `string` without mutating it. */
char *ish_get_token_in_cstring(char *string, int (*is_separator)(int character),
                               char **cursor_after_token);

/** Whether `string` begins with `prefix`. */
int ish_cstring_starts_with(const char *prefix, const char *string);

/** Whether `string` begins with `prefix`, comparing case-insensitively. */
int ish_cstring_starts_with_ignoring_case(const char *prefix, const char *string);

/** Whether the two NUL-terminated strings are equal. */
int ish_are_cstrings_equal(const char *string, const char *another_string);

/** Whether the two strings are equal, comparing case-insensitively. */
int ish_are_cstrings_equal_ignoring_case(const char *string, const char *another_string);

/** Parse a leading signed decimal integer from `string`. */
int ish_get_integer_from_cstring(const char *string);

/** Join two path elements into `combined_path` separated by a `/`. */
void ish_combine_path_elements(const char *path_element, const char *another_path_element,
                               char *combined_path, unsigned long maximum_combined_path_length);

#endif
