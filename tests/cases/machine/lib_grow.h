/*
 * lib_grow.h: what the scenarios of the grow suite share
 * (tests/cases/machine.grow.json): commands typed at a recovery shell that
 * end in a token with their status, the console text each one printed, and
 * the host's reading of disk files and of the plan's ranges.
 */
#ifndef LIB_GROW_H
#define LIB_GROW_H

#include <stddef.h>
#include <stdint.h>

#include "drive.h"

/**
 * Boot machine `machine` of the case with `disks`, and wait for the
 * recovery system's banner and then for `prompt`, the root shell's prompt,
 * which recovery gives at the console with nothing typed.
 *
 * @return 0, or -1 with `d->why` saying why.
 */
int grow_boot(struct drive *d, struct etude_case *c, const char *machine, const char *const disks[],
              const char *prompt);

/**
 * Type the shell command that `fmt` gives, followed by `echo TAG=$? TAGEND`
 * with the token written so that the typed line does not hold it, and wait
 * for TAGEND. `tag` is a word of capitals that nothing else on the console
 * holds.
 *
 * @param out when not NULL, gets the console text from the end of the last
 *        wait before the command to the token, without carriage returns,
 *        which the caller frees, or NULL when the command did not end.
 * @return the command's exit status, or -1 when it did not end, with
 *         `d->why` saying why.
 */
int grow_shell(struct drive *d, const char *tag, char **out, const char *fmt, ...);

/**
 * Type a command as grow_shell does, and require it to end with status 0;
 * any other status fails `d` with the command in `d->why`.
 *
 * @return 0, or -1.
 */
int grow_run(struct drive *d, const char *fmt, ...);

/**
 * Say whether the console of the boot of `d` so far has `text` in it.
 *
 * @return 1 when it has, 0 when it has not or cannot be read.
 */
int grow_console_has(const struct drive *d, const char *text);

/**
 * Report assertion `id` as etude_assert does, counting it when it failed,
 * so that grow_clean knows whether the scenario's evidence is wanted.
 */
void grow_assert(struct etude_case *c, const char *id, int held, const char *detail);

/**
 * Note file `path`, which the scenario or a host program it ran wrote among
 * the evidence and libetude-case does not know of, for grow_clean.
 */
void grow_made(const char *path);

/**
 * Remove the files grow_made noted when every assertion grow_assert reported
 * held, as libetude-case removes its own evidence after a pass; keep them
 * otherwise. A scenario calls it last.
 */
void grow_clean(void);

/**
 * Copy the path of the evidence `name` into `buf`, since etude_evidence
 * gives every path in one buffer, which its next call overwrites.
 *
 * @return `buf`, or NULL when the name is no file name or the path does not
 *         fit.
 */
char *grow_evidence(struct etude_case *c, const char *name, char *buf, size_t size);

/**
 * Copy `len` bytes of file `from`, from byte `off`, to a new file `to`.
 *
 * @return 0, or -1 when either file cannot be used or `from` is shorter.
 */
int grow_copy_range(const char *from, uint64_t off, uint64_t len, const char *to);

/** Write `text` to a new file `path`. @return 0, or -1. */
int grow_write_text(const char *path, const char *text);

/**
 * Compare files `a` and `b` byte for byte, but for the `skip_len` bytes from
 * `skip_off`, which may be 0.
 *
 * @param why gets, when they differ, where they first do.
 * @return 1 when they are the same, 0 when they differ or cannot be read.
 */
int grow_same(const char *a, const char *b, uint64_t skip_off, uint64_t skip_len, char *why,
              size_t size);

/**
 * Judge the writes of a grow that `qemu-img map` lists in overlay `disk`:
 * every extent must lie in the boot partition, sectors 2048 to 133119, whose
 * environment the grow writes beside the seed, or in a range of `ranges`,
 * the plan's ranges.txt; growset, given the seed `seed` at sector 266240
 * grown to 131072 sectors, says which extents lie outside the grow set.
 * Keeps the map and the coverage as evidence `NAME.map` and `NAME.cover`.
 *
 * @param why gets what was found when the writes are not judged inside.
 * @return 1 when every write was inside, 0 otherwise.
 */
int grow_writes_inside(struct etude_case *c, const char *name, const char *disk, const char *seed,
                       const char *ranges, char *why, size_t size);

/**
 * Rehearse the installed machine's grow, tools/dev/machine_grow.sh, on the
 * case's target disk and store, which give the partition table: save the
 * target, check the backup on the host, grow an 8 MiB seed to 64 MiB, and
 * restore every byte but the boot environment's, every write inside the
 * saved sectors. The scenarios of both tables call it
 * (tests/cases/machine/grow_machine_mbr.c and grow_machine_gpt.c).
 *
 * @return what etude_scenario returns.
 */
int grow_machine(struct etude_case *c);

#endif
