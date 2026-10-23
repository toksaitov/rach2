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

/* The first sector of the installed machine's boot partition. */
#define GROW_BOOT_FIRST 2048

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

/** Report each of the assertions `ids`, ended by NULL, failed with `why`. */
void grow_fail_all(struct etude_case *c, const char *const ids[], const char *why);

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
 * Report each of the assertions `ids`, ended by NULL, that grow_assert has
 * not reported yet, failed with `why`, for a scenario that cannot reach the
 * rest of its checks.
 */
void grow_fail_rest(struct etude_case *c, const char *const ids[], const char *why);

/** Copy file `from` whole over file `to`. @return 0, or -1. */
int grow_copy_file(const char *from, const char *to);

/**
 * Convert the case's overlay `target` with qemu-img to the raw image
 * `NAME.raw` among the evidence, and compare it byte for byte with its base,
 * the artifact `disk`. The image is removed when they are the same and kept
 * as evidence when they are not.
 *
 * @param env the byte of the disk at which the boot environment's 1024
 *        bytes start, which are left out of the comparison, or 0 to compare
 *        every byte.
 * @param why gets why they are not the same.
 * @return 1 when they are the same, 0 otherwise.
 */
int grow_raw_same(struct etude_case *c, const char *name, uint64_t env, char *why, size_t size);

/**
 * Boot machine `recovery` of the case from the recovery stick with the
 * target and the store, mount the store at /mnt/grow, and unpack the plan
 * and its copies with faults, grow-tools.tar and grow-tests.tar, into
 * /tmp/grow-plan.
 *
 * @param mount the mount command's options before its type, "-n" when the
 *        RAM root may fill, or "".
 */
void grow_recovery_boot(struct drive *d, struct etude_case *c, const char *mount);

/**
 * Read the files that `save` writes under the store's saved/ off `store` on
 * the host, each into the evidence PREFIX followed by its name. `names` are
 * those of backup.bin, boot.img, grow.plan, ranges.txt, SHA256SUMS and
 * boot.status that are wanted, ended by NULL, or all of them for NULL.
 *
 * @return 0, or -1 when a file cannot be read.
 */
int grow_read_saved(struct etude_case *c, const char *store, const char *prefix,
                    const char *const names[]);

/**
 * Check SHA256SUMS of the saved files, `sums`, as the host does before it
 * lets the grow go on: each line a digest of 64 hexadecimal digits and the
 * name of a file `save` writes, and each file, read as the evidence PREFIX
 * followed by its name, of that digest.
 *
 * @return 1 when every line and file is such, 0 otherwise.
 */
int grow_sums_hold(struct etude_case *c, const char *sums, const char *prefix, char *why,
                   size_t size);

/**
 * Write the checksums `sums` as the store's saved/host.sha256 in the machine
 * of `d`, as the host returns them after its check.
 */
void grow_type_sums(struct drive *d, const char *sums);

/**
 * Read the files of the plan off the case's store artifact on the host: the
 * seed into the evidence plan-seed.ext2, whose path goes into `seed`, and
 * the plan's ranges.txt into the evidence plan-ranges.txt.
 *
 * @return 0, or -1 when either cannot be read.
 */
int grow_read_plan(struct etude_case *c, char *seed, size_t size);

/**
 * Find the boot environment's sector in the boot partition from the saved
 * bootctl status, `status`: its `grubenv:` line names one cluster on an MBR
 * disk's boot partition, or two of one sector, which growboot has found
 * consecutive, on a GUID table's, of 1024 bytes.
 *
 * @return the sector, or -1 when no line or more than one names it.
 */
long grow_env_sector(const char *status);

/**
 * Boot machine `target` of the case, the target disk alone, so that GRUB,
 * the recovery kernel and its RAM image all come from it, and check that it
 * starts recovery, with recovery the saved entry and no next entry, and that
 * its root has the owner's file.
 *
 * @return 1 when it does, 0 with `why` saying why otherwise.
 */
int grow_target_boots(struct etude_case *c, char *why, size_t size);

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
