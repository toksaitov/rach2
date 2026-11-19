/*
 * rbtree.h: red-black trees with augmented nodes and a cached leftmost node,
 * Linux 6.12's lib/rbtree.c and include/linux/rbtree_augmented.h.
 */
#ifndef _RBTREE_H_
#define _RBTREE_H_

#include "kdef.h"
#include "kint.h"

/*
 * A node, embedded in the structure it orders. `rb_parent_color` holds the
 * parent's address with the colour in bit 0 (0 red, 1 black); a node is
 * aligned to at least 4 bytes, so the low bits of the address are free.
 */
struct rb_node {
    uintptr_t rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

/* A tree: its root node, or NULL when it is empty. */
struct rb_root {
    struct rb_node *rb_node;
};

/* A tree that also keeps its leftmost node, the least, or NULL. */
struct rb_root_cached {
    struct rb_root rb_root;
    struct rb_node *rb_leftmost;
};

/*
 * What an augmented tree does to keep each node's summary of its subtree:
 * `propagate` recomputes the summaries from `node` up to, not including,
 * `stop` (NULL for the root), stopping early where a summary does not
 * change; `copy` gives `new_` the summary of `old`, whose place it takes;
 * `rotate` gives `new_`, the new root of a rotated subtree, the summary
 * `old` had, and recomputes `old`'s.
 */
struct rb_augment_callbacks {
    void (*propagate)(struct rb_node *node, struct rb_node *stop);
    void (*copy)(struct rb_node *old, struct rb_node *new_);
    void (*rotate)(struct rb_node *old, struct rb_node *new_);
};

/* The structure of type `type` whose member `member` is the node `ptr`. */
#define rb_entry(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* The parent of a node, NULL at the root. */
#define rb_parent(r) ((struct rb_node *)((r)->rb_parent_color & ~(uintptr_t)3))

/* Whether a node is in no tree, as `rb_clear_node` marks it. */
#define RB_EMPTY_NODE(node) ((node)->rb_parent_color == (uintptr_t)(node))
#define RB_CLEAR_NODE(node) ((node)->rb_parent_color = (uintptr_t)(node))

/**
 * Hang `node` at `*link`, a child pointer of `parent` found by a descent,
 * or the root's pointer with `parent` NULL. The caller then calls
 * `rb_insert_color` or an augmented insert to rebalance.
 */
void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **link);

/** Rebalance after `rb_link_node`, for a tree with no augmentation. */
void rb_insert_color(struct rb_node *node, struct rb_root *root);

/** Take `node` out of a tree with no augmentation. */
void rb_erase(struct rb_node *node, struct rb_root *root);

/**
 * Rebalance after `rb_link_node` in an augmented tree. The caller has
 * propagated the summaries along the new node's path first, as
 * `rb_add_augmented_cached` does.
 */
void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         const struct rb_augment_callbacks *augment);

/** Take `node` out of an augmented tree, keeping every summary. */
void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        const struct rb_augment_callbacks *augment);

/**
 * Put `node` in a cached augmented tree in the order `less` gives, after
 * the nodes equal to it. Returns `node` when it became the leftmost, else
 * NULL.
 */
struct rb_node *rb_add_augmented_cached(struct rb_node *node, struct rb_root_cached *tree,
                                        int (*less)(const struct rb_node *a,
                                                    const struct rb_node *b),
                                        const struct rb_augment_callbacks *augment);

/** Take `node` out of a cached augmented tree, moving the leftmost on. */
void rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
                               const struct rb_augment_callbacks *augment);

/** The least node of a tree, or NULL when it is empty. */
struct rb_node *rb_first(const struct rb_root *root);

/** The greatest node of a tree, or NULL when it is empty. */
struct rb_node *rb_last(const struct rb_root *root);

/** The node after `node` in order, or NULL after the greatest. */
struct rb_node *rb_next(const struct rb_node *node);

/** The node before `node` in order, or NULL before the least. */
struct rb_node *rb_prev(const struct rb_node *node);

#endif
