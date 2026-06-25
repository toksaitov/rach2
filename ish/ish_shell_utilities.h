#ifndef ISH_SHELL_UTILITIES_H
#define ISH_SHELL_UTILITIES_H

#include "ish_common.h"
#include "ish_cstring_utilities.h"

static const char ISH_Standard_Input_Redirection_Lexeme[]  = "<";
static const char ISH_Standard_Output_Redirection_Lexeme[] = ">";

/** Split `input` in place into a NULL-terminated array of whitespace tokens. */
unsigned long ish_build_argument_array_from_input(char *input, char **arguments, unsigned long max_argument_count);

/** Value of the first `environment` entry starting with `key` (no case). */
char *ish_get_first_environment_variable(const char *key, unsigned long key_length, char **environment);

/** Filename of the first `<` redirection in `arguments`, removed from it. */
char *ish_extract_last_stdin_redirection_file(char **arguments);

/** Filename of the first `>` redirection in `arguments`, removed from it. */
char *ish_extract_last_stdout_redirection_file(char **arguments);

#endif
