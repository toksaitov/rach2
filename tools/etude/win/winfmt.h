/*
 * winfmt.h: the parts of etude's Windows adapter that are plain C, so that
 * the host tests can run them anywhere: command lines, environment blocks and
 * time conversions.
 */
#ifndef ETUDE_WINFMT_H
#define ETUDE_WINFMT_H

#include <stddef.h>
#include <stdint.h>

/* The longest command line CreateProcessW takes, in UTF-16 units, its NUL
 * included. */
#define ETUDE_WIN_CMDLINE_MAX 32767

/* The seconds between the Windows epoch, 1601, and the Unix epoch, 1970. */
#define ETUDE_WIN_EPOCH_SECONDS 11644473600ull

/**
 * Make the command line that starts PROGRAM with the arguments ARGV.
 *
 * A Windows process receives one string and parses its own arguments; a
 * program built with the UCRT, MinGW's among them, parses it by the rules of
 * Microsoft's "Parsing C command-line arguments". PROGRAM is put between
 * quotes, since the rules read the program's name up to the next quote and
 * give a backslash no meaning there, and each argument is quoted so that
 * those rules give it back unchanged. ARGV ends with a null pointer; the
 * strings are UTF-8 and pass through byte for byte.
 *
 * @return 0 with a string from malloc in `*out`, which the caller frees;
 *         EINVAL when PROGRAM is empty or has a quote, which no file name of
 *         Windows has; ENOMEM.
 */
int etude_win_cmdline(const char *program, const char *const *argv, char **out);

/**
 * Sort the environment entries VARS, each `NAME=VALUE`, as CreateProcessW
 * expects its environment block: by name, without regard to the case of
 * ASCII letters.
 *
 * @return 0; EINVAL when an entry has no `=` after a name of at least one
 *         byte, or when two names differ only in case, since Windows would
 *         keep one of them.
 */
int etude_win_envsort(const char **vars, size_t n);

/**
 * Convert TICKS of a counter that runs at FREQ ticks a second to
 * microseconds, rounded down. It does not overflow while the result fits and
 * FREQ is below 10^13; the performance counter of Windows runs at 10 MHz.
 */
uint64_t etude_win_ticks_us(uint64_t ticks, uint64_t freq);

/**
 * Convert a FILETIME, in units of 100 ns since 1601, to microseconds since
 * 1970, rounded down; a time before 1970 is negative.
 */
int64_t etude_win_filetime_unix_us(uint64_t filetime);

/**
 * Carry WALL_US, a past time of the wall clock, onto the monotonic clock,
 * given NOW_MONO_US and NOW_WALL_US, the two clocks read together; all are in
 * microseconds. A time not before NOW_WALL_US gives NOW_MONO_US, and one
 * further back than NOW_MONO_US gives 0.
 */
uint64_t etude_win_wall_mono_us(int64_t wall_us, uint64_t now_mono_us, int64_t now_wall_us);

#endif
