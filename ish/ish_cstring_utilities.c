#include "ish_cstring_utilities.h"

unsigned long ish_get_cstring_length(const char *string)
{
    ish_check(string);
    const char *start = string;
    for ( ; *string; ++string);
    return (unsigned long) (string - start);
}

/**
 * Replace the first `character` in `string` with `replacement`, in place.
 *
 * Only the first match is replaced; later matches are left alone. Returns
 * `string` for chaining.
 */
char *ish_replace_first_character_in_cstring(char *string, char character, char replacement)
{
    for (char *cursor = string; *cursor; ++cursor) {
        if (*cursor == character) {
            *cursor = replacement;
            break;
        }
    }
    return string;
}

int ish_is_space(int character)
{
    return character == ' ' || (unsigned int) character - '\t' < 5;
}

int ish_is_digit(int character)
{
    return (unsigned int) character - '0' < 10;
}

int ish_is_path_separator(int character)
{
    return character == ISH_Path_Separator;
}

int ish_is_lowercase(int character)
{
    return (unsigned int) character - 'a' < 26;
}

int ish_is_uppercase(int character)
{
    return (unsigned int) character - 'A' < 26;
}

int ish_convert_to_lowercase(int character)
{
    static const int Distance = 'a' - 'A';
    if (ish_is_uppercase(character)) {
        character += Distance;
    }
    return character;
}

int ish_convert_to_uppercase(int character)
{
    static const int Distance = 'a' - 'A';
    if (ish_is_lowercase(character)) {
        character -= Distance;
    }
    return character;
}

/**
 * Carve the next token out of `string`, mutating it in place.
 *
 * Leading separators (per `is_separator`) are skipped, then the run up to the
 * next separator is the token. This writes a `'\0'` over that separator, so the
 * returned pointer is a NUL-terminated token cut out of the caller's buffer;
 * `string` is altered. Use `ish_get_token_in_cstring` for the non-destructive
 * variant.
 *
 * Returns the token, or `0` if only separators remain. When
 * `cursor_after_token` is non-null it receives the position just past the
 * terminator (the start of the rest), or `0` at end of input, so it can seed
 * the next call.
 */
char *ish_carve_token_in_cstring(char *string, int (*is_separator)(int character), char **cursor_after_token)
{
    char *result = 0;

    while (*string && is_separator(*string)) {
        ++string;
    }

    if (*string) {
        result = string;
    }

    while (*string && !is_separator(*string)) {
        ++string;
    }

    if (cursor_after_token) {
        *cursor_after_token = 0;
    }

    if (*string) {
        *(string++) = '\0';
        if (*string && cursor_after_token) {
            *cursor_after_token = string;
        }
    }

    return result;
}

/**
 * Locate the next token in `string` without modifying it.
 *
 * Behaves like `ish_carve_token_in_cstring` but leaves the source untouched: no
 * `'\0'` is written, so the returned token runs to the next separator and is
 * not separately terminated. Use this when the buffer is read-only or shared;
 * use `ish_carve_token_in_cstring` when an in-place NUL-terminated token is
 * wanted.
 *
 * Returns the token start, or `0` if only separators remain. When
 * `cursor_after_token` is non-null it receives the separator that ends the
 * token (the start of the rest), or `0` at end of input.
 */
char *ish_get_token_in_cstring(char *string, int (*is_separator)(int character), char **cursor_after_token)
{
    char *result = 0;

    while (*string && is_separator(*string)) {
        ++string;
    }

    if (*string) {
        result = string;
    }

    while (*string && !is_separator(*string)) {
        ++string;
    }

    if (cursor_after_token) {
        *cursor_after_token = 0;
        if (*string) {
            *cursor_after_token = string;
        }
    }

    return result;
}

/**
 * Test whether `string` begins with `prefix`.
 *
 * Returns true once all of `prefix` matches, so the empty prefix always
 * matches and `string` may be longer than `prefix`.
 */
int ish_cstring_starts_with(const char *prefix, const char *string)
{
    ish_check(prefix && string);
    while (*prefix) {
        if (*prefix++ != *string++) {
            return 0;
        }
    }
    return 1;
}

/**
 * Like `ish_cstring_starts_with`, comparing letters case-insensitively.
 */
int ish_cstring_starts_with_ignoring_case(const char *prefix, const char *string)
{
    ish_check(prefix && string);
    while (*prefix) {
        if (ish_convert_to_lowercase(*prefix++) != ish_convert_to_lowercase(*string++)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Test two NUL-terminated strings for byte-for-byte equality.
 */
int ish_are_cstrings_equal(const char *string, const char *another_string)
{
    ish_check(string && another_string);
    while (*string && *string == *another_string) {
        ++string;
        ++another_string;
    }
    return *string == *another_string;
}

/**
 * Like `ish_are_cstrings_equal`, comparing letters case-insensitively.
 */
int ish_are_cstrings_equal_ignoring_case(const char *string, const char *another_string)
{
    ish_check(string && another_string);
    while (*string && ish_convert_to_lowercase(*string) == ish_convert_to_lowercase(*another_string)) {
        ++string;
        ++another_string;
    }
    return ish_convert_to_lowercase(*string) == ish_convert_to_lowercase(*another_string);
}

/**
 * Parse a leading signed decimal integer out of `string`.
 *
 * Skips leading whitespace, accepts an optional `+` or `-`, then consumes the
 * run of digits. Parsing stops at the first non-digit; trailing text is
 * ignored. There is no overflow or error reporting: an empty or non-numeric
 * field yields `0`.
 */
int ish_get_integer_from_cstring(const char *string)
{
    ish_check(string);

    unsigned int magnitude = 0;
    while (ish_is_space(*string)) {
        ++string;
    }

    int negative = 0;
    switch (*string) {
        case '-':
            negative = 1;
        case '+':
            ++string;
    }

    /* Accumulate the magnitude in unsigned arithmetic so a value beyond INT_MAX
     * wraps (defined) instead of overflowing a signed int; the sign is applied in
     * unsigned form too, so no step is undefined. Callers use only the low bits
     * (an exit status is taken modulo 256). */
    while (ish_is_digit(*string)) {
        magnitude = magnitude * 10u + (unsigned int)(*string++ - '0');
    }

    return (int)(negative ? 0u - magnitude : magnitude);
}

/**
 * Join `path_element` and `another_path_element` into `combined_path` with a
 * single `ISH_Directory_Separator` between them.
 *
 * Only the first path element up to (not including) an `ISH_Path_Separator` is
 * used, so a search-path entry can be passed straight in. The result is always
 * NUL-terminated. `maximum_combined_path_length` bounds the bytes written
 * before the terminator; the output is silently truncated if the inputs do not
 * fit.
 */
void ish_combine_path_elements(const char *path_element, const char *another_path_element, char *combined_path, unsigned long maximum_combined_path_length)
{
    ish_check(path_element && another_path_element && combined_path);

    unsigned long i = 0;

    while (i < maximum_combined_path_length && *path_element && !ish_is_path_separator(*path_element)) {
        combined_path[i++] = *path_element++;
    }

    if (i < maximum_combined_path_length) {
        combined_path[i++] = ISH_Directory_Separator;
    }

    while (i < maximum_combined_path_length && *another_path_element) {
        combined_path[i++] = *another_path_element++;
    }

    combined_path[i] = '\0';
}
