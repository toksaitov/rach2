#include "ish_shell_utilities.h"

/**
 * Split `input` into whitespace-separated tokens, in place.
 *
 * Tokens are carved out of `input` itself: the separator ending each token is
 * overwritten with a NUL (leading and repeated separators are skipped), so the
 * caller's buffer is mutated and the `arguments` entries point into it. At most
 * `max_argument_count` slots are filled, and a NULL terminator is written after
 * the last token (so `arguments` must hold one more slot). Returns the number
 * of tokens after the first (the token count minus one, or 0 when empty), so it
 * is the operand count for the command in `arguments[0]`.
 */
unsigned long ish_build_argument_array_from_input(char *input, char **arguments, unsigned long max_argument_count)
{
    unsigned long argument_index = 0;

    for (char *cursor = input; cursor && argument_index < max_argument_count; ++argument_index) {
        arguments[argument_index] = ish_carve_token_in_cstring(cursor, ish_is_space, &cursor);
    }
    arguments[argument_index] = 0;

    unsigned long argument_count = argument_index == 0 ? 0 : argument_index - 1;
    return argument_count;
}

char *_ish_extract_next_parameter_for_argument_lexeme(char **arguments, const char *argument_lexeme);

/**
 * Return the value of the first environment entry whose name starts with `key`.
 *
 * Scans `environment` (a NULL-terminated `name=value` array) for the first
 * entry that begins with `key`, compared without case. The result points
 * `key_length` bytes into that entry, so for a `key` that includes the `=`
 * the result is the value. Returns NULL when no entry matches.
 */
char *ish_get_first_environment_variable(const char *key, unsigned long key_length, char **environment)
{
    ish_check(key && environment);

    char *result = 0;
    for (char **variable = environment; *variable; ++variable) {
        if (ish_cstring_starts_with_ignoring_case(key, *variable)) {
            result = *variable;
            result += key_length;
            break;
        }
    }

    return result;
}

/**
 * Pull the first `<` redirection target out of `arguments`.
 *
 * Returns the filename after the first `<` and clears that `<` and the
 * filename from `arguments` so they are not passed to the command. Returns
 * NULL when there is no `<`. See
 * `_ish_extract_next_parameter_for_argument_lexeme`.
 */
char *ish_extract_last_stdin_redirection_file(char **arguments)
{
    return _ish_extract_next_parameter_for_argument_lexeme(arguments, ISH_Standard_Input_Redirection_Lexeme);
}

/**
 * Pull the first `>` redirection target out of `arguments`.
 *
 * As `ish_extract_last_stdin_redirection_file`, for the `>` lexeme.
 */
char *ish_extract_last_stdout_redirection_file(char **arguments)
{
    return _ish_extract_next_parameter_for_argument_lexeme(arguments, ISH_Standard_Output_Redirection_Lexeme);
}

/**
 * Strip the first `argument_lexeme` and its parameter from `arguments`.
 *
 * Scans the NULL-terminated `arguments` for the first entry equal to
 * `argument_lexeme` (compared without case), clears that slot and the slot
 * after it to NULL (removing the lexeme and its filename from the argv the
 * command will run with), and returns that filename. Returns NULL when no
 * lexeme is present.
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
