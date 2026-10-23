/*
 * capture.h: the capture of a case's output streams and the scan of its
 * standard output for `child` records, shared by the executors of every
 * platform.
 */
#ifndef ETUDE_CAPTURE_H
#define ETUDE_CAPTURE_H

#include <stddef.h>
#include <stdio.h>

#include "etude/exec.h"
#include "etude/report.h"

/* The scan of one stream for report records, a line at a time. */
struct capture_lines {
    char line[ETUDE_REPORT_LINE_MAX - 1]; /* a record without its newline */
    size_t len;
    int overlong; /* the current line is longer than a record can be */
};

/* Files owned by the controller, never inherited by the case. */
struct capture_files {
    FILE *out, *err;
};

/** Open optional output paths in binary truncate mode, sharing equal paths. */
int capture_files_open(struct capture_files *f, const struct etude_exec_spec *s);
/** Flush a complete chunk; return errno on a write failure. */
int capture_files_add(FILE *f, const char *bytes, size_t n);
/** Close each distinct file once. */
void capture_files_close(struct capture_files *f);

/** Make `o` an empty stream that keeps at most `max` bytes. */
int capture_init(struct etude_output *o, size_t max);

/** Add `n` bytes to `o`, keeping what fits under `max` and counting all. */
void capture_add(struct etude_output *o, size_t max, const char *buf, size_t n);

/** Free the bytes `o` kept. */
void capture_free(struct etude_output *o);

/**
 * Scan `n` bytes of standard output and call `found` with the PID of each
 * complete `child` record among them.
 */
void capture_scan(struct capture_lines *s, const char *buf, size_t n,
                  void (*found)(void *ctx, long pid), void *ctx);

#endif
