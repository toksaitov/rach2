/*
 * shl-htable.c: Rach2's hash table for libtsm, separate chaining over a
 * power-of-two array of buckets that doubles when it holds more elements
 * than buckets.
 */

#include <errno.h>
#include <stdlib.h>
#include "shl-htable.h"

/* One element in a chain, with the hash it was filed under. */
struct shl_htable_entry {
	struct shl_htable_entry *next;
	size_t hash;
	void *elem;
};

/* The buckets a table starts with. */
#define SHL_HTABLE_FIRST 16

/**
 * Give `htable` `size` buckets, a power of two, and move every entry into
 * them.
 *
 * Returns 0, or -ENOMEM with the table unchanged.
 */
static int shl_htable_resize(struct shl_htable *htable, size_t size)
{
	struct shl_htable_entry **buckets, *e, *next;
	size_t i;

	buckets = calloc(size, sizeof(*buckets));
	if (!buckets)
		return -ENOMEM;

	for (i = 0; i < htable->size; ++i) {
		for (e = htable->buckets[i]; e; e = next) {
			next = e->next;
			e->next = buckets[e->hash & (size - 1)];
			buckets[e->hash & (size - 1)] = e;
		}
	}

	free(htable->buckets);
	htable->buckets = buckets;
	htable->size = size;
	return 0;
}

void shl_htable_init(struct shl_htable *htable,
		     bool (*compare)(const void *a, const void *b),
		     size_t (*rehash)(const void *elem, void *priv),
		     void *priv)
{
	htable->compare = compare;
	htable->rehash = rehash;
	htable->priv = priv;
	htable->buckets = NULL;
	htable->size = 0;
	htable->count = 0;
}

void shl_htable_clear(struct shl_htable *htable,
		      void (*free_cb)(void *elem, void *ctx), void *ctx)
{
	struct shl_htable_entry *e, *next;
	size_t i;

	for (i = 0; i < htable->size; ++i) {
		for (e = htable->buckets[i]; e; e = next) {
			next = e->next;
			if (free_cb)
				free_cb(e->elem, ctx);
			free(e);
		}
	}

	free(htable->buckets);
	htable->buckets = NULL;
	htable->size = 0;
	htable->count = 0;
}

void shl_htable_visit(struct shl_htable *htable,
		      void (*visit_cb)(void *elem, void *ctx), void *ctx)
{
	struct shl_htable_entry *e;
	size_t i;

	for (i = 0; i < htable->size; ++i)
		for (e = htable->buckets[i]; e; e = e->next)
			visit_cb(e->elem, ctx);
}

bool shl_htable_lookup(struct shl_htable *htable, const void *obj,
		       size_t hash, void **out)
{
	struct shl_htable_entry *e;

	if (!htable->size)
		return false;

	for (e = htable->buckets[hash & (htable->size - 1)]; e; e = e->next) {
		if (e->hash == hash && htable->compare(obj, e->elem)) {
			if (out)
				*out = e->elem;
			return true;
		}
	}

	return false;
}

int shl_htable_insert(struct shl_htable *htable, const void *obj, size_t hash)
{
	struct shl_htable_entry *e;
	size_t b;

	if (!htable->size) {
		if (shl_htable_resize(htable, SHL_HTABLE_FIRST))
			return -ENOMEM;
	} else if (htable->count >= htable->size) {
		/* A failed growth only lengthens the chains. */
		shl_htable_resize(htable, htable->size * 2);
	}

	e = malloc(sizeof(*e));
	if (!e)
		return -ENOMEM;

	b = hash & (htable->size - 1);
	e->hash = hash;
	e->elem = (void *)obj;
	e->next = htable->buckets[b];
	htable->buckets[b] = e;
	++htable->count;
	return 0;
}

bool shl_htable_remove(struct shl_htable *htable, const void *obj,
		       size_t hash, void **out)
{
	struct shl_htable_entry **link, *e;

	if (!htable->size)
		return false;

	link = &htable->buckets[hash & (htable->size - 1)];
	for (e = *link; e; link = &e->next, e = *link) {
		if (e->hash == hash && htable->compare(obj, e->elem)) {
			*link = e->next;
			if (out)
				*out = e->elem;
			free(e);
			--htable->count;
			return true;
		}
	}

	return false;
}
