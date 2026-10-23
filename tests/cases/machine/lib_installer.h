/*
 * lib_installer.h: the code the scenarios of the installer suite share: the
 * console exchanges written as tables, the reading of disk tables and of
 * the host tools' output, the SSH logins, and the files a scenario makes
 * in its evidence directory.
 */
#ifndef LIB_INSTALLER_H
#define LIB_INSTALLER_H

#include <stddef.h>
#include <stdint.h>

#include "drive.h"

/* The answers the installs of the suite give. */
#define INST_NEWPASS  "an-installed-machine"
#define INST_USER     "alice"
#define INST_USERPASS "alices-own-password"

/* One line of a console exchange: wait for `text` when `send` is 0, or type
 * it and a newline when `send` is 1. A table ends with a NULL `text`. */
struct inst_line {
    int send;
    const char *text;
};

/**
 * Report assertion `id` as etude_assert does, and count it when it failed,
 * so that inst_finish knows whether to keep the files the scenario made.
 */
void inst_assert(struct etude_case *c, const char *id, int held, const char *detail);

/**
 * Report assertion `id` as failed because what it needed did not happen,
 * with `why`, the first call of the boot that did not complete or another
 * reason, as its detail.
 */
void inst_fail(struct etude_case *c, const char *id, const char *why);

/**
 * Name `path` as a file the scenario made, which inst_finish removes when
 * every assertion held. A file made by a host tool in the evidence
 * directory is not the library's to remove.
 */
void inst_made(const char *path);

/** Remove the files named by inst_made when every assertion held. */
void inst_finish(void);

/**
 * Mark boot `d` as failed with `why`, unless a call has failed already, so
 * that its later calls do nothing, as when a value the exchange needs was
 * not on the console.
 */
void inst_refuse(struct drive *d, const char *why);

/**
 * Wait for and type the lines of table `lines` in order.
 *
 * @return 0, or -1 with `d->why` saying which line did not complete.
 */
int inst_exchange(struct drive *d, const struct inst_line *lines);

/**
 * Run host tool or artifact `argv[0]` as drive_host does, and give its
 * standard output followed by its standard error, as the old suite read
 * `2>&1`, in a string the caller frees.
 *
 * @return the text, or NULL when the output cannot be read; `*status` is
 *         the exit status, or -1 when the program did not exit.
 */
char *inst_host_text(struct etude_case *c, char *const argv[], const char *output, int *status);

/**
 * Read entry `n` of the GUID table of disk image `image`, as `mkgpt --list`
 * prints it, artifact `mkgpt` of the case, into `*first` and `*bytes`, the
 * offset and the length of the partition in bytes.
 *
 * @return 0, or -1 when the tool fails or the table has no such entry.
 */
int inst_gpt_entry(struct etude_case *c, const char *image, int n, uint64_t *first,
                   uint64_t *bytes);

/**
 * Fill the empty disk `name` of the case with the pattern of fill_pattern,
 * `bytes` long, and give its SHA-256 in `hex`.
 *
 * @return 0, or -1 when the disk cannot be made or written.
 */
int inst_pattern_disk(struct etude_case *c, const char *name, uint64_t bytes, char hex[65]);

/**
 * Copy artifact `name` to evidence `evidence` with mode 0600, as ssh(1)
 * requires of a private key, and give the copy's path in `path`.
 *
 * @return 0, or -1 when the artifact cannot be read or the copy written.
 */
int inst_private_file(struct etude_case *c, const char *name, const char *evidence, char *path,
                      size_t size);

/**
 * Give the first line of artifact `name`, without its newline, in `buf`,
 * as `$(cat FILE)` gives a one-line file.
 *
 * @return 0, or -1 when the artifact cannot be read or is empty.
 */
int inst_artifact_line(struct etude_case *c, const char *name, char *buf, size_t size);

/**
 * Log in by key over SSH to the case's first port, as `user`, with the one
 * key `key` and nothing of the host's own, run `command`, and give what ssh
 * printed in a string the caller frees. `known` is the evidence that keeps
 * the host keys of this machine.
 */
char *inst_ssh_key(struct etude_case *c, const char *known, const char *key, const char *user,
                   const char *command, const char *output);

/**
 * Log in as root by password over SSH to the case's first port, the
 * password typed at ssh's prompt by artifact `ptydrive` on a terminal of 40
 * rows and 100 columns, run `command`, and give what ssh printed in a string
 * the caller frees.
 */
char *inst_ssh_password(struct etude_case *c, const char *known, const char *password,
                        const char *command, const char *output);

/**
 * Prepare installed disk `disk` to boot its recovery system with no key of
 * the image's own: put artifact `keyless`, the recovery image staged with no
 * key, on the recovery partition in place of the image there, with the
 * tools `mdel` and `mcopy`, and arm the recovery entry for the next boot as
 * grub-editenv arms it, with next_entry set and every other variable of the
 * GRUB environment block kept.
 *
 * @return 0, or -1 with `why` saying what could not be done.
 */
int inst_recovery_boot(struct etude_case *c, const char *disk, char *why, size_t size);

/** Say whether `text` has `needle` in it after byte `from`. */
int inst_has(const char *text, size_t from, const char *needle);

#endif
