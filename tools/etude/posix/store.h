/*
 * store.h: the parts of the artifact store that the preparation graph uses:
 * the records of the nodes' keys and the locks of preparations under way.
 */
#ifndef ETUDE_STORE_H
#define ETUDE_STORE_H

#include <stdint.h>

#include "etude/artifact.h"

/** Give a copy of `a`, `/` and `b` from malloc, or NULL. */
char *store_join(const char *a, const char *b);

/** Say whether `s` is a digest: 64 lowercase hex digits. */
int store_is_digest(const char *s);

/**
 * Read the record of key `key`.
 *
 * @return 0 with the object's digest and size, or -1 with errno ENOENT for
 *         no record, EBADMSG for one that does not parse, or an error of the
 *         system.
 */
int store_record_read(const struct etude_store *s, const char *key,
                      char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size);

/**
 * Record that node `node`, a line of printable text, published object
 * `digest` of `size` bytes under key `key`.
 *
 * The record is written whole under a partial name and then renamed to its
 * key, replacing a record there. The caller has the key's lock, so that one
 * process at a time writes a key's record.
 *
 * @return 0, or -1 with errno set.
 */
int store_record_write(struct etude_store *s, const char *key, const char *node, const char *digest,
                       uint64_t size);

/**
 * Find the product of key `key`: its record, and its object read whole and
 * checked against its digest and the record's size.
 *
 * @return 0 with the digest and size, or -1 with errno set as
 *         store_record_read or etude_store_check gives.
 */
int store_lookup(const struct etude_store *s, const char *key, char digest[ETUDE_DIGEST_HEX + 1],
                 uint64_t *size);

/**
 * Lock key `key` for its preparation without waiting.
 *
 * @return the descriptor that holds the lock until it is closed, or -1
 *         with errno EAGAIN when another process holds it.
 */
int store_lock(const struct etude_store *s, const char *key);

#endif
