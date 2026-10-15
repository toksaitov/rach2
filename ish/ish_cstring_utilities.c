/*
 * The string, character, token, integer and path operations of ish; the
 * contracts are in `ish_cstring_utilities.h`.
 */

#include "ish_cstring_utilities.h"

unsigned long ish_get_cstring_length(const char *string)
{
    ish_check(string);
    const char *start = string;
    for (; *string; ++string)
        ;
    return (unsigned long)(string - start);
}

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
    return character == ' ' || (unsigned int)character - '\t' < 5;
}

int ish_is_digit(int character)
{
    return (unsigned int)character - '0' < 10;
}

int ish_is_path_separator(int character)
{
    return character == ISH_Path_Separator;
}

int ish_is_lowercase(int character)
{
    return (unsigned int)character - 'a' < 26;
}

int ish_is_uppercase(int character)
{
    return (unsigned int)character - 'A' < 26;
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

char *ish_carve_token_in_cstring(char *string, int (*is_separator)(int character),
                                 char **cursor_after_token)
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

char *ish_get_token_in_cstring(char *string, int (*is_separator)(int character),
                               char **cursor_after_token)
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

int ish_are_cstrings_equal(const char *string, const char *another_string)
{
    ish_check(string && another_string);
    while (*string && *string == *another_string) {
        ++string;
        ++another_string;
    }
    return *string == *another_string;
}

int ish_are_cstrings_equal_ignoring_case(const char *string, const char *another_string)
{
    ish_check(string && another_string);
    while (*string &&
           ish_convert_to_lowercase(*string) == ish_convert_to_lowercase(*another_string)) {
        ++string;
        ++another_string;
    }
    return ish_convert_to_lowercase(*string) == ish_convert_to_lowercase(*another_string);
}

/* The digits are summed in unsigned arithmetic, so that the wrap is defined. */
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
        /* fall through */
    case '+':
        ++string;
    }

    while (ish_is_digit(*string)) {
        magnitude = magnitude * 10u + (unsigned int)(*string++ - '0');
    }

    return (int)(negative ? 0u - magnitude : magnitude);
}

int ish_combine_path_elements(const char *path_element, const char *another_path_element,
                              char *combined_path, unsigned long maximum_combined_path_length)
{
    ish_check(path_element && another_path_element && combined_path);

    unsigned long i = 0;
    int result = 0;

    while (i < maximum_combined_path_length && *path_element &&
           !ish_is_path_separator(*path_element)) {
        combined_path[i++] = *path_element++;
    }
    if (*path_element && !ish_is_path_separator(*path_element)) {
        result = -1;
    }

    if (i < maximum_combined_path_length) {
        combined_path[i++] = ISH_Directory_Separator;
    } else {
        result = -1;
    }

    while (i < maximum_combined_path_length && *another_path_element) {
        combined_path[i++] = *another_path_element++;
    }
    if (*another_path_element) {
        result = -1;
    }

    combined_path[i] = '\0';
    return result;
}
