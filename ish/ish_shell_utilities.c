/*
 * The command line operations of ish; the contracts of the exported ones are
 * in `ish_shell_utilities.h`.
 */

#include "ish_shell_utilities.h"

unsigned long ish_build_argument_array_from_input(char *input, char **arguments,
                                                  unsigned long max_argument_count)
{
    unsigned long argument_index = 0;
    char *cursor = input;

    while (cursor && argument_index < max_argument_count) {
        char *token = ish_carve_token_in_cstring(cursor, ish_is_space, &cursor);
        if (token == 0) {
            break;
        }
        arguments[argument_index++] = token;
    }
    arguments[argument_index] = 0;

    unsigned long argument_count = argument_index == 0 ? 0 : argument_index - 1;
    return argument_count;
}

char *_ish_extract_next_parameter_for_argument_lexeme(char **arguments,
                                                      const char *argument_lexeme);

char *ish_get_first_environment_variable(const char *key, unsigned long key_length,
                                         char **environment)
{
    ish_check(key && environment);

    char *result = 0;
    for (char **variable = environment; *variable; ++variable) {
        if (ish_cstring_starts_with(key, *variable)) {
            result = *variable;
            result += key_length;
            break;
        }
    }

    return result;
}

char *ish_extract_last_stdin_redirection_file(char **arguments)
{
    return _ish_extract_next_parameter_for_argument_lexeme(arguments,
                                                           ISH_Standard_Input_Redirection_Lexeme);
}

char *ish_extract_last_stdout_redirection_file(char **arguments)
{
    return _ish_extract_next_parameter_for_argument_lexeme(arguments,
                                                           ISH_Standard_Output_Redirection_Lexeme);
}

/**
 * Remove the first entry of the null-terminated `arguments` that equals
 * `argument_lexeme`, ignoring case, together with the entry after it, and
 * return that entry: the file name of the redirection.
 *
 * Both slots are set to null, so the array ends where the lexeme was.
 * Returns null when the lexeme is absent or is the last entry.
 */
char *_ish_extract_next_parameter_for_argument_lexeme(char **arguments, const char *argument_lexeme)
{
    ish_check(arguments);

    char *result = 0;
    for (char **argument = arguments; *argument; ++argument) {
        if (ish_are_cstrings_equal_ignoring_case(argument_lexeme, *argument)) {
            *argument = 0;
            char **next_argument = argument + 1;
            if (*next_argument) {
                result = *next_argument;
                *next_argument = 0;
            }
        }
    }

    return result;
}
