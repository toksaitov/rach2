/*
 * shl-htable.h: Rach2's hash table for libtsm, in place of libtsm's LGPL
 * shl-htable.h and shl-htable.c, with the part of their interface libtsm uses.
 */

#ifndef SHL_HTABLE_H
#define SHL_HTABLE_H

#include <stdbool.h>
#include <stddef.h>

struct shl_htable_entry;

/*
 * A table of pointers to elements the caller owns, each filed under a hash
 * the caller computes. The table never reads an element except through
 * `compare`, and frees none of them but through the callback of
 * shl_htable_clear(). A zeroed table is empty and valid once
 * shl_htable_init() has set its callbacks.
 */
struct shl_htable {
	/* Whether the key `a` names the element `b`. */
	bool (*compare)(const void *a, const void *b);
	/* The hash of an element; kept for the interface, since every entry
	 * keeps the hash it was inserted under and the table never rehashes. */
	size_t (*rehash)(const void *elem, void *priv);
	void *priv;
	/* The chains, `size` of them, a power of two, or NULL while empty. */
	struct shl_htable_entry **buckets;
	size_t size;
	/* The elements in the table. */
	size_t count;
};

/**
 * Make `htable` an empty table with the callbacks given.
 *
 * Allocates nothing; the first insert does.
 */
void shl_htable_init(struct shl_htable *htable,
		     bool (*compare)(const void *a, const void *b),
		     size_t (*rehash)(const void *elem, void *priv),
		     void *priv);

/**
 * Remove every element, calling `free_cb(elem, ctx)` for each when it is not
 * NULL, and free the table's own memory.
 *
 * The table is empty and usable afterwards.
 */
void shl_htable_clear(struct shl_htable *htable,
		      void (*free_cb)(void *elem, void *ctx), void *ctx);

/**
 * Call `visit_cb(elem, ctx)` for each element, in no particular order.
 *
 * The callback must not insert or remove.
 */
void shl_htable_visit(struct shl_htable *htable,
		      void (*visit_cb)(void *elem, void *ctx), void *ctx);

/**
 * Find the element filed under `hash` that `compare(obj, elem)` accepts.
 *
 * Returns true and stores the element in `*out`, when `out` is not NULL, if
 * there is one; returns false and leaves `*out` alone if not.
 */
bool shl_htable_lookup(struct shl_htable *htable, const void *obj,
		       size_t hash, void **out);

/**
 * File the element `obj` under `hash`.
 *
 * The table does not look for a duplicate. Returns 0, or -ENOMEM when memory
 * runs out, in which case the table is as it was.
 */
int shl_htable_insert(struct shl_htable *htable, const void *obj, size_t hash);

/**
 * Remove the element filed under `hash` that `compare(obj, elem)` accepts.
 *
 * Returns true and stores the element in `*out`, when `out` is not NULL, if
 * there was one; returns false if not. The element itself is not freed.
 */
bool shl_htable_remove(struct shl_htable *htable, const void *obj,
		       size_t hash, void **out);

#endif /* SHL_HTABLE_H */
