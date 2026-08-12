#ifdef ISH_USE_STDLIB
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif

#include "ish_cstring_utilities.h"
#include "ish_shell_utilities.h"
#include "ish_syscalls.h"

static const unsigned long Max_Input_String_Length = 255;
static const unsigned long Max_Argument_Count = 255;
static const unsigned long Max_Executable_Path_Length = 1024;

static const char Environment_Variable_Home[] = "HOME";
static const char Environment_Variable_Path[] = "PATH";

static const char Command_Line_Option[] = "-c";

static const char Builtin_Command_CD[] = "cd";
static const char Builtin_Command_Exit[] = "exit";

static const unsigned short Standard_Input_File_Flags = 00000;
static const unsigned short Standard_Output_File_Mode = 00644;

static const char Fork_Error_Message[] = "fork failure\n";

/**
 * Split one line into arguments and run it. Returns the command's exit status.
 *
 * The line is split into an argument array, and either a builtin (`cd`, `exit`)
 * runs in this process or the command is resolved to an executable and run in a
 * child. Resolution tries the name as given, then each `PATH` element joined
 * with the name; a name that `stat` cannot find is skipped. Before the
 * `execve`, `< file` and `> file` redirections recorded in the arguments are
 * applied in the child by `dup2`-ing the opened descriptors over 0 and 1. The
 * parent waits for the child.
 *
 * `input` is split in place, so the caller's buffer comes back tokenized.
 */
static int ish_run_line(char *input, char *home, char *paths, char **envp)
{
    ish_replace_first_character_in_cstring(input, '\n', '\0');

    char *arguments[Max_Argument_Count + 1];
    arguments[Max_Argument_Count] = 0;

    unsigned long argument_count = ish_build_argument_array_from_input(input, arguments, Max_Argument_Count);

    const char *command = arguments[0];
    if (!command) {
        return 0;
    }

    if (ish_are_cstrings_equal(command, Builtin_Command_CD)) {
        char *directory = argument_count == 0 ? home : arguments[1];
        if (directory) {
            chdir(directory);
        }
        return 0;
    }

    if (ish_are_cstrings_equal(command, Builtin_Command_Exit)) {
        int exit_status = argument_count == 0 ? 0 : ish_get_integer_from_cstring(arguments[1]);
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
        for (char *cursor = paths; cursor && !executable; ) {
            char *path = ish_get_token_in_cstring(cursor, ish_is_path_separator, &cursor);
            if (path) {
                ish_combine_path_elements(path, arguments[0], candidate, Max_Executable_Path_Length);
                if (stat(candidate, stat_result) == 0) {
                    executable = candidate;
                }
            }
        }
    }

    if (!executable) {
        return 0;
    }

    char *stdin_file = ish_extract_last_stdin_redirection_file(arguments);
    int stdin_descriptor = !stdin_file ? -1 : open(stdin_file, Standard_Input_File_Flags);

    char *stdout_file = ish_extract_last_stdout_redirection_file(arguments);
    int stdout_descriptor = !stdout_file ? -1 : creat(stdout_file, Standard_Output_File_Mode);

    int status = 0;
    int pid = fork();
    if (pid == 0) {
        if (stdin_descriptor > 0) {
            dup2(stdin_descriptor, 0);
            close(stdin_descriptor);
        }
        if (stdout_descriptor > 1) {
            dup2(stdout_descriptor, 1);
            close(stdout_descriptor);
        }
        execve(executable, arguments, envp);
        exit(-1);
    } else if (pid > 0) {
        int child_status = 0;
        waitpid(pid, &child_status, 0);
        /* The wait status, which puts a normal exit's code in the second
         * byte, is the ABI this kernel and Linux share. */
        status = (child_status >> 8) & 0xFF;
    } else {
        write(2, Fork_Error_Message, sizeof(Fork_Error_Message) - 1);
    }

    /* Only the parent reaches here (the child exec'd or exited). Close the
     * redirection descriptors it opened, or the shell, which runs as init,
     * leaks one descriptor per redirected command. The same guards as the
     * child's dup2 skip the standard fds, which open never returns since the
     * shell holds them. */
    if (stdin_descriptor > 0) {
        close(stdin_descriptor);
    }
    if (stdout_descriptor > 1) {
        close(stdout_descriptor);
    }


    return status;
}

/**
 * Run a command line named with `-c`, or read lines from descriptor 0 forever.
 *
 * The `-c` form is what makes this shell usable by another program: `su` and
 * `login` hand a shell `-c command` when they were asked to run one thing as
 * somebody, and a shell that read only from its input would sit waiting for a
 * line nobody is going to type. It runs exactly one line and exits with that
 * line's status, which is what a caller of `su -c` reads.
 *
 * The crt0 entry hands `main` the initial-stack ABI directly: `argc`, the
 * NULL-terminated `argv`, and the NULL-terminated `envp` that follows it.
 */
int main(int argc, char **argv, char **envp)
{
    char *home = ish_get_first_environment_variable(Environment_Variable_Home, sizeof(Environment_Variable_Home), envp);
    char *paths = ish_get_first_environment_variable(Environment_Variable_Path, sizeof(Environment_Variable_Path), envp);

    char input[Max_Input_String_Length + 1];
    input[Max_Input_String_Length] = '\0';

    if (argc > 2 && ish_are_cstrings_equal(argv[1], Command_Line_Option)) {
        /* Split in place, like every other line: argv is this process's own
         * writable stack. */
        return ish_run_line(argv[2], home, paths, envp);
    }

    int bytes_read;
    while ((bytes_read = read(0, input, Max_Input_String_Length)) > 0) {
        input[bytes_read] = '\0';
        ish_run_line(input, home, paths, envp);
    }

    return 0;
}
