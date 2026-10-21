/*
 * lib_installer_repair.h: the code the scenarios of the installer-repair cases
 * share (tests/cases/machine.installer-repair.json): the boots of a machine
 * from the install stick and from its own disk, the files of an attempt on the
 * host, and the readings of disk tables and consoles their judgements take.
 */
#ifndef LIB_INSTALLER_REPAIR_H
#define LIB_INSTALLER_REPAIR_H

#include <stddef.h>
#include <stdint.h>

#include "drive.h"

/* The disk image and the root image on the test stick's volume. */
#define REPAIR_DISK_NAME "installer-test.disk"
#define REPAIR_ROOT_NAME "installer-root.ext2"

/* The password the rows that change a machine give root. */
#define REPAIR_NEWPASS "an-installed-machine"

#define REPAIR_MIB 1048576ULL

/* The byte of an MBR that has the type of its first entry. */
#define REPAIR_MBR_TYPE1 450

/**
 * Report assertion `id` as etude_assert does, and count it when it failed,
 * so that repair_finish knows whether to keep the attempt's files.
 *
 * @return `held`, made 0 or 1.
 */
int repair_assert(struct etude_case *c, const char *id, int held, const char *detail);

/**
 * Give in `buf` the path of file `name` in the attempt's own directory,
 * `work` beside its evidence, made at the first call, or of the directory
 * itself when `name` is empty.
 *
 * @return 0, or -1 when the path does not fit or the directory cannot be made.
 */
int repair_path(struct etude_case *c, const char *name, char *buf, size_t size);

/**
 * Remove the attempt's own directory when every assertion reported held, and
 * keep it as evidence otherwise. A scenario calls it once, before it returns.
 */
void repair_finish(struct etude_case *c);

/**
 * Read entry `n` of the table of disk `disk` with host tool `tool`, the
 * artifact `mkgpt` for a GUID table or `mkmbr` for an MBR, run as
 * `TOOL --list DISK`, and give its first byte and length.
 *
 * @return 0, or -1 when the tool failed or listed no such entry.
 */
int repair_entry(struct etude_case *c, const char *tool, const char *disk, int n, uint64_t *start,
                 uint64_t *len);

/**
 * Give the byte at `off` of file `path`.
 *
 * @return the byte, or -1 when the file cannot be read there.
 */
int repair_byte(const char *path, uint64_t off);

/**
 * Copy into `buf` the rest of the last line of `text` that begins with
 * `prefix`, as `grep ^PREFIX | tail -1` finds it.
 *
 * @return 0, or -1 when `text` has no such line.
 */
int repair_last_field(const char *text, const char *prefix, char *buf, size_t size);

/**
 * Say whether one line of `text` has every text of `parts`, a list ended by
 * NULL, in that order, as a regular expression `A.*B` matches a line.
 */
int repair_line_has(const char *text, const char *const parts[]);

/**
 * Give in `hex` the SHA-256 of `alen` bytes of file `a` from `aoff` followed
 * by `blen` bytes of file `b` from `boff`.
 *
 * @return 0, or -1 when either file cannot be read or is shorter.
 */
int repair_digest_join(const char *a, uint64_t aoff, uint64_t alen, const char *b, uint64_t boff,
                       uint64_t blen, char hex[65]);

/**
 * Write the whole of file `from` into file `to` from byte `at`, as
 * `dd conv=notrunc seek=` does. When `mode` is not 0, `to` is made new, or
 * emptied, with that mode.
 *
 * @return 0, or -1 when either file cannot be read or written.
 */
int repair_copy(const char *from, const char *to, uint64_t at, int mode);

/**
 * Read the file `path` whole, its last newline taken off, into `buf`.
 *
 * @return 0, or -1 when it cannot be read or does not fit.
 */
int repair_read_line(const char *path, char *buf, size_t size);

/**
 * Run host tool or artifact `argv[0]`, keeping its output as evidence
 * `output`, and give in `out` its standard output and standard error, each
 * with a NUL after it, which the caller frees; `out` may be NULL.
 *
 * @return its exit status, or -1 when it did not exit or could not run.
 */
int repair_run(struct etude_case *c, char *const argv[], const char *output, char **out);

/**
 * Run mtools program `tool` on the FAT volume at byte `at` of `disk`, as
 * `TOOL -i DISK@@AT ARGS...`, `args` a list ended by NULL, keeping its
 * output as evidence `output`, and give its output in `out` as repair_run
 * does.
 *
 * @return its exit status, or -1.
 */
int repair_volume(struct etude_case *c, const char *tool, const char *disk, uint64_t at,
                  const char *const args[], const char *output, char **out);

/**
 * Boot `machine`, the stick and the disk under repair, to the recovery
 * system's shell, and mount the stick's volume on /m.
 *
 * @return 0, or -1 with `d->why` saying why.
 */
int repair_recovery(struct drive *d, struct etude_case *c, const char *machine);

/**
 * Boot `machine` from its disk and log in as root with `password`, to the
 * shell's prompt.
 *
 * @return 0, or -1 with `d->why` saying why.
 */
int repair_login(struct drive *d, struct etude_case *c, const char *machine, const char *password);

/**
 * Give in `buf` the demo password that the images built in this tree give
 * root, the artifact `password`.
 *
 * @return 0, or -1 when it cannot be read.
 */
int repair_password(struct etude_case *c, char *buf, size_t size);

#endif
