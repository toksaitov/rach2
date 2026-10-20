/*
 * proof_file.h: the reading of whole files that the proof driver's catalogue
 * reader and its POSIX part share.
 */
#ifndef ETUDE_PROOF_FILE_H
#define ETUDE_PROOF_FILE_H

#include <stddef.h>

/* The largest file the proof driver reads whole: a catalogue or a source. */
#define PROOF_FILE_MAX (64u << 20)

/**
 * Read the file at `path` whole into `*data`, from malloc and followed by a
 * NUL, with its length in `*len`.
 *
 * @return 0, or -1 with errno set, EFBIG for a file past PROOF_FILE_MAX.
 */
int proof_slurp(const char *path, char **data, size_t *len);

#endif
