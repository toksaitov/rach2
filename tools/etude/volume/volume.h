/*
 * volume.h: the two volumes of etude's tooling machine, which etude-volume
 * makes and reads for `etude provision tooling`, so that a job needs no program
 * on the host but QEMU. The seed of the first boot is a FAT16 file system,
 * which cloud-init finds by its label. The job volume is a raw disk with a tar
 * archive at its start: the host writes the job's files there, and the guest's
 * runner reads them with tar(1) and writes the job's results over them in the
 * same form. tools/etude/Readme.md describes both.
 */
#ifndef ETUDE_VOLUME_H
#define ETUDE_VOLUME_H

#include <stddef.h>
#include <stdint.h>

/* The seed volume's size, in sectors of 512 bytes: 8 MiB. */
#define ETUDE_VOLUME_SEED_SECTORS 16384u

/**
 * Make `img` a FAT16 volume of ETUDE_VOLUME_SEED_SECTORS sectors labelled
 * `label`, whose root directory has the files `names` of directory `dir`,
 * each under its own name, a long name for every one.
 *
 * @return 0, or -1 with `why` saying what failed: a name that is not
 *         printable ASCII of at most 255 characters, a file that cannot be
 *         read, files that do not fit, or a write that failed.
 */
int etude_volume_seed(const char *img, const char *label, const char *dir, const char *const *names,
                      char *why, size_t whylen);

/* A job volume being written. */
struct etude_volume;

/**
 * Begin a job volume: `img`, made anew, a raw disk of `size` bytes. The
 * archive is ustar, with a pax header for a name of more than 100 bytes or a
 * file of 8 GiB or more. `why` is kept, and every later call that fails
 * writes its reason there. A size of zero writes an archive without padding
 * it to a disk size.
 *
 * @return the volume, or NULL with `why` set.
 */
struct etude_volume *etude_volume_create(const char *img, uint64_t size, char *why, size_t whylen);

/**
 * Add host file or directory `path` as entry `name`, a directory with all
 * it has. A file's mode is 0755 when the host gives its owner execution and
 * 0644 otherwise, and a directory's 0755; anything but a file or a
 * directory, and a tree deeper than 64, is refused.
 */
int etude_volume_add(struct etude_volume *v, const char *name, const char *path);

/** Add an empty directory `name`. */
int etude_volume_dir(struct etude_volume *v, const char *name);

/** Add a file `name` of the `n` bytes of `data`, of mode 0644. */
int etude_volume_text(struct etude_volume *v, const char *name, const char *data, size_t n);

/**
 * End the archive, extend the disk to its size and close it; `v` is freed.
 * An archive that does not leave room for its end in the disk fails.
 *
 * @return 0, or -1 when this or an earlier call failed.
 */
int etude_volume_close(struct etude_volume *v);

/**
 * Read the archive at the start of volume `img`, as tar(1) writes one in
 * the ustar, pax or GNU formats, long names included, and write each file
 * and directory to the host, making the directories above it.
 *
 * `map` is a list of pairs, a name and a host path, ended by NULL. An entry
 * goes to the path of the first pair whose name is the entry's or a
 * directory above it, with the rest of the entry's name appended; so the
 * pairs `out/a`, `/tmp/a` and `out`, `/tmp/o` put `out/a/b` at `/tmp/a/b`
 * and `out/c` at `/tmp/o/c`. Every pair must take at least one entry.
 *
 * @return 0, or -1 with `why` saying what failed: a header whose checksum
 *         is wrong, which is a damaged volume; a volume that ends before its
 *         archive does, or whose archive has no end, which is a truncated
 *         one; a link or a special file; a name that is absolute or leaves
 *         the archive; an entry that no pair takes, or a pair that takes no
 *         entry; or a host file that cannot be written.
 */
int etude_volume_read(const char *img, const char *const *map, char *why, size_t whylen);

/** Read one regular file, exactly once, from an archive. Check all headers
 * and names, but do not extract its other entries. */
int etude_volume_file(const char *img, const char *name, const char *path,
                      char *why, size_t whylen);

#endif
