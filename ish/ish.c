#ifdef ISH_USE_STDLIB
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#endif

#include "ish_cstring_utilities.h"
#include "ish_shell_utilities.h"
#include "ish_syscalls.h"

static const unsigned long Max_Input_String_Length = 255;
static const unsigned long Max_Argument_Count = 255;
static const unsigned long Max_Executable_Path_Length = 1024;

static const char Environment_Variable_Home[] = "HOME=";
static const char Environment_Variable_Path[] = "PATH=";

static const char Command_Line_Option[] = "-c";

static const char Builtin_Command_CD[] = "cd";
static const char Builtin_Command_Exit[] = "exit";

static const unsigned short Standard_Input_File_Flags = 00000;
static const unsigned short Standard_Output_File_Mode = 00644;

static const char Fork_Error_Message[] = "fork failure\n";
static const char Redirection_Error_Message[] = "cannot open a redirection\n";
static const char Input_Length_Error_Message[] = "ish: command line too long\n";

/* The statuses a shell reports for what it could not run, which are the
 * conventional ones a caller of `sh -c` reads: 127 for a name that resolved to
 * nothing, 126 for a command found but not runnable, and 128 + N for a child a
 * signal killed. */
static const int Status_Not_Found = 127;
static const int Status_Not_Runnable = 126;
static const int Status_Signalled_Base = 128;

/**
 * Wait for `pid`, retrying interruptions, and return its shell status.
 *
 * The raw wrappers return -4 for EINTR; libc returns -1 and sets errno.
 * A failed wait returns 126. A normal exit stores its status in bits 8-15;
 * a signal death stores the signal number in bits 0-6.
 */
static int ish_wait_for_child(int pid)
{
    int child_status = 0;
    int waited;

    do {
        waited = waitpid(pid, &child_status, 0);
#ifdef ISH_USE_STDLIB
    } while (waited == -1 && errno == EINTR);
#else
    } while (waited == -4);
#endif
    if (waited != pid) {
        return Status_Not_Runnable;
    }
    if ((child_status & 0x7F) != 0) {
        return Status_Signalled_Base + (child_status & 0x7F);
    }
    return (child_status >> 8) & 0xFF;
}

/**
 * Split one line into arguments and run it. Returns the command's exit status.
 *
 * The line is split into an argument array, and either a builtin (`cd`, `exit`)
 * runs in this process or the command is resolved to an executable and run in a
 * child. Resolution tries the name as given, then each `PATH` element joined
 * with the name; a name that `stat` cannot find is skipped, and so is a joined
 * path longer than `Max_Executable_Path_Length`, since cut short it could be
 * the path of another program. Before the `execve`, `< file` and `> file`
 * redirections recorded in the arguments are applied in the child by
 * `dup2`-ing the opened descriptors over 0 and 1. The parent retries an
 * interrupted wait and returns the child's status. If the status cannot be
 * obtained, it returns 126.
 *
 * `input` is split in place, so the caller's buffer comes back tokenized.
 *
 * Redirections are opened before a builtin or child runs. An open failure
 * returns 126. Builtin operands are read after redirection tokens are removed;
 * `cd` without an operand uses HOME, including `cd > file`.
 *
 * An unresolved command returns 127. A fork or exec failure returns 126.
 * A signal death returns 128 + N. A failed `cd` returns 1.
 */
static int ish_run_line(char *input, char *home, char *paths, char **envp)
{
    ish_replace_first_character_in_cstring(input, '\n', '\0');

    char *arguments[Max_Argument_Count + 1];
    arguments[Max_Argument_Count] = 0;

    ish_build_argument_array_from_input(input, arguments, Max_Argument_Count);

    const char *command = arguments[0];
    if (!command) {
        return 0;
    }

    char *stdin_file = ish_extract_last_stdin_redirection_file(arguments);
    int stdin_descriptor = !stdin_file ? -1 : open(stdin_file, Standard_Input_File_Flags);

    char *stdout_file = ish_extract_last_stdout_redirection_file(arguments);
    int stdout_descriptor = !stdout_file ? -1 : creat(stdout_file, Standard_Output_File_Mode);

    if ((stdin_file && stdin_descriptor < 0) || (stdout_file && stdout_descriptor < 0)) {
        write(2, Redirection_Error_Message, sizeof(Redirection_Error_Message) - 1);
        if (stdin_descriptor >= 0) {
            close(stdin_descriptor);
        }
        if (stdout_descriptor >= 0) {
            close(stdout_descriptor);
        }

        return Status_Not_Runnable;
    }

    if (ish_are_cstrings_equal(command, Builtin_Command_CD)) {
        char *directory = arguments[1] ? arguments[1] : home;
        int moved = directory ? chdir(directory) : -1;

        if (stdin_descriptor >= 0) {
            close(stdin_descriptor);
        }
        if (stdout_descriptor >= 0) {
            close(stdout_descriptor);
        }

        return moved == 0 ? 0 : 1;
    }

    if (ish_are_cstrings_equal(command, Builtin_Command_Exit)) {
        int exit_status = arguments[1] ? ish_get_integer_from_cstring(arguments[1]) : 0;
        exit(exit_status);
        return 0;
    }

    char *executable = arguments[0];

    char candidate[Max_Executable_Path_Length + 1];
    candidate[Max_Executable_Path_Length] = '\0';

#ifdef ISH_USE_STDLIB
    struct stat stat_buffer;
    struct stat *stat_result = &stat_buffer;
#else
    /* Opaque scratch for the kernel's stat result; no libc struct stat. */
    char stat_result[256];
#endif
    if (stat(executable, stat_result) != 0) {
        executable = 0;
        for (char *cursor = paths; cursor && !executable;) {
            char *path = ish_get_token_in_cstring(cursor, ish_is_path_separator, &cursor);
            if (path) {
                int cut = ish_combine_path_elements(path, arguments[0], candidate,
                                                    Max_Executable_Path_Length);

                if (!cut && stat(candidate, stat_result) == 0) {
                    executable = candidate;
                }
            }
        }
    }

    if (!executable) {
        if (stdin_descriptor >= 0) {
            close(stdin_descriptor);
        }
        if (stdout_descriptor >= 0) {
            close(stdout_descriptor);
        }

        return Status_Not_Found;
    }

    int status = 0;
    int pid = fork();
    if (pid == 0) {
        if (stdin_descriptor >= 0 && stdin_descriptor != 0) {
            if (dup2(stdin_descriptor, 0) < 0) {
                exit(Status_Not_Runnable);
            }
            close(stdin_descriptor);
        }
        if (stdout_descriptor >= 0 && stdout_descriptor != 1) {
            if (dup2(stdout_descriptor, 1) < 0) {
                exit(Status_Not_Runnable);
            }
            close(stdout_descriptor);
        }
        execve(executable, arguments, envp);
        exit(Status_Not_Runnable);
    } else if (pid > 0) {
        status = ish_wait_for_child(pid);
    } else {
        write(2, Fork_Error_Message, sizeof(Fork_Error_Message) - 1);
        status = Status_Not_Runnable;
    }

    if (stdin_descriptor >= 0) {
        close(stdin_descriptor);
    }
    if (stdout_descriptor >= 0) {
        close(stdout_descriptor);
    }

    return status;
}

/**
 * Run the command supplied with `-c`, or read command lines from descriptor 0.
 *
 * `su -c` uses this form to run one command as the target account. It returns
 * that command's status.
 *
 * Input mode discards each command's status and returns 0 at EOF or a read
 * error. It retains incomplete lines between reads and runs a final line at
 * EOF without a newline. A read error discards an incomplete line. Lines over
 * 255 bytes, excluding the newline, are diagnosed and discarded in full.
 * The `exit` builtin terminates either mode with its operand's status.
 *
 * The crt0 entry hands `main` the initial-stack ABI directly: `argc`, the
 * NULL-terminated `argv`, and the NULL-terminated `envp` that follows it.
 */
int main(int argc, char **argv, char **envp)
{
    char *home = ish_get_first_environment_variable(Environment_Variable_Home,
                                                    sizeof(Environment_Variable_Home) - 1, envp);
    char *paths = ish_get_first_environment_variable(Environment_Variable_Path,
                                                     sizeof(Environment_Variable_Path) - 1, envp);

    if (argc > 2 && ish_are_cstrings_equal(argv[1], Command_Line_Option)) {
        return ish_run_line(argv[2], home, paths, envp);
    }

    char input[Max_Input_String_Length];
    char line[Max_Input_String_Length + 1];
    unsigned long length = 0;
    int discarding = 0;
    int bytes_read;
    while ((bytes_read = read(0, input, Max_Input_String_Length)) > 0) {
        int i;
        for (i = 0; i < bytes_read; ++i) {
            if (input[i] == '\n') {
                if (!discarding) {
                    line[length] = '\0';
                    ish_run_line(line, home, paths, envp);
                }
                length = 0;
                discarding = 0;
            } else if (!discarding) {
                if (length < Max_Input_String_Length) {
                    line[length++] = input[i];
                } else {
                    write(2, Input_Length_Error_Message, sizeof(Input_Length_Error_Message) - 1);
                    discarding = 1;
                }
            }
        }
    }
    if (bytes_read == 0 && length != 0 && !discarding) {
        line[length] = '\0';
        ish_run_line(line, home, paths, envp);
    }

    return 0;
}
