/*
 * args.h: a program's arguments in UTF-8 on every host, for the native
 * programs whose paths and names are UTF-8, etude among them. It includes
 * windows.h, which must follow winsock2.h, so a file includes it last.
 */
#ifndef ETUDE_ARGS_H
#define ETUDE_ARGS_H

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <corecrt_startup.h>
#include <windows.h>
#endif

/**
 * Replace each of the `argc` strings of `argv`, the vector `main` received,
 * with the same argument in UTF-8; `program` names the caller in a failure.
 *
 * Windows gives a narrow `main` its arguments in the legacy code page, which
 * changes a character outside that page to another or to `?`. This asks the
 * UCRT to parse the same command line by the same rules into UTF-16, and
 * converts each argument from that, leaving the code pages of the process and
 * the host as they were. The strings live until the program exits. On other
 * hosts the arguments are already the caller's bytes, and nothing changes.
 *
 * When the UCRT gives another count of arguments, an argument is not valid
 * UTF-16 or memory runs out, it prints `PROGRAM: the arguments cannot be read
 * as UTF-8` and exits with status 2.
 */
static inline void etude_utf8_args(int argc, char **argv, const char *program)
{
#ifdef _WIN32
    char *arg;
    int i = 0, n;

    if (_configure_wide_argv(_crt_argv_unexpanded_arguments) == 0 && __wargv != NULL &&
        __argc == argc) {
        for (; i < argc; i++) {
            n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, __wargv[i], -1, NULL, 0, NULL,
                                    NULL);
            if (n <= 0 || (arg = malloc((size_t)n)) == NULL ||
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, __wargv[i], -1, arg, n, NULL,
                                    NULL) != n)
                break;
            argv[i] = arg;
        }
    }
    if (i < argc) {
        fprintf(stderr, "%s: the arguments cannot be read as UTF-8\n", program);
        exit(2);
    }
#else
    (void)argc;
    (void)argv;
    (void)program;
#endif
}

#endif
