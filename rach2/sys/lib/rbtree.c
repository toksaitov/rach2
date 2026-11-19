/*
 * rbtree.c: red-black trees with augmented nodes and a cached leftmost node,
 * after Linux 6.12's lib/rbtree.c and include/linux/rbtree_augmented.h.
 */

#include "rbtree.h"

/*
 * The red-black properties:
 *
 *  1) a node is red or black;
 *  2) the root is black;
 *  3) all leaves (NULL) are black;
 *  4) both children of a red node are black;
 *  5) every path from a node to a leaf below it holds the same number of
 *     black nodes.
 *
 * So the longest path from the root, alternating red and black, is at most
 * twice the shortest, all black, and a tree of n nodes is at most
 * 2 log2(n + 1) deep.
 *
 * An augmented tree keeps in each node a summary of its subtree. A rotation
 * changes the subtrees of two nodes, which the `rotate` callback fixes; an
 * erase moves the successor into the erased node's place (`copy`) and
 * shortens one path (`propagate`).
 */

#define RB_RED   0u
#define RB_BLACK 1u

#define rb_color(rb)    ((unsigned int)((rb)->rb_parent_color & 1u))
#define rb_is_red(rb)   (rb_color(rb) == RB_RED)
#define rb_is_black(rb) (rb_color(rb) == RB_BLACK)

/** Set a node's parent, keeping its colour. */
static void rb_set_parent(struct rb_node *rb, struct rb_node *p)
{
    rb->rb_parent_color = (rb->rb_parent_color & 1u) | (uintptr_t)p;
}

/** Set a node's parent and colour. */
static void rb_set_parent_color(struct rb_node *rb, struct rb_node *p, unsigned int color)
{
    rb->rb_parent_color = (uintptr_t)p | color;
}

/** Make a red node black. */
static void rb_set_black(struct rb_node *rb)
{
    rb->rb_parent_color |= RB_BLACK;
}

/** The parent of a red node, whose colour bit is 0. */
static struct rb_node *rb_red_parent(struct rb_node *red)
{
    return (struct rb_node *)red->rb_parent_color;
}

/** Point `parent`'s link to `old`, or the root's, at `new_`. */
static void rb_change_child(struct rb_node *old, struct rb_node *new_, struct rb_node *parent,
                            struct rb_root *root)
{
    if (parent) {
        if (parent->rb_left == old) {
            parent->rb_left = new_;
        } else {
            parent->rb_right = new_;
        }
    } else {
        root->rb_node = new_;
    }
}

/** Put `new_` in `old`'s place as the root of a rotated subtree, giving
 * `old` the parent `new_` and the colour `color`. */
static void rb_rotate_set_parents(struct rb_node *old, struct rb_node *new_, struct rb_root *root,
                                  unsigned int color)
{
    struct rb_node *parent = rb_parent(old);

    new_->rb_parent_color = old->rb_parent_color;
    rb_set_parent_color(old, new_, color);
    rb_change_child(old, new_, parent, root);
}

/** A rotation's callback for a tree with no augmentation. */
static void rb_dummy_rotate(struct rb_node *old, struct rb_node *new_)
{
    (void)old;
    (void)new_;
}

/** A propagation's callback for a tree with no augmentation. */
static void rb_dummy_propagate(struct rb_node *node, struct rb_node *stop)
{
    (void)node;
    (void)stop;
}

static const struct rb_augment_callbacks rb_dummy_callbacks = {rb_dummy_propagate, rb_dummy_rotate,
                                                               rb_dummy_rotate};

/**
 * Restore the properties after `node` was linked in red: while its parent
 * is red, recolour when the uncle is red, else rotate once or twice and
 * stop. `augment_rotate` is told of each rotation.
 */
static void rb_insert_fix(struct rb_node *node, struct rb_root *root,
                          void (*augment_rotate)(struct rb_node *old, struct rb_node *new_))
{
    struct rb_node *parent = rb_red_parent(node), *gparent, *tmp;

    for (;;) {
        if (!parent) {
            rb_set_parent_color(node, NULL, RB_BLACK);
            break;
        }
        if (rb_is_black(parent)) {
            break;
        }
        gparent = rb_red_parent(parent);
        tmp = gparent->rb_right;
        if (parent != tmp) {
            /* parent is gparent's left child */
            if (tmp && rb_is_red(tmp)) {
                rb_set_parent_color(tmp, gparent, RB_BLACK);
                rb_set_parent_color(parent, gparent, RB_BLACK);
                node = gparent;
                parent = rb_parent(node);
                rb_set_parent_color(node, parent, RB_RED);
                continue;
            }
            tmp = parent->rb_right;
            if (node == tmp) {
                /* left rotate at parent */
                tmp = node->rb_left;
                parent->rb_right = tmp;
                node->rb_left = parent;
                if (tmp) {
                    rb_set_parent_color(tmp, parent, RB_BLACK);
                }
                rb_set_parent_color(parent, node, RB_RED);
                augment_rotate(parent, node);
                parent = node;
                tmp = node->rb_right;
            }
            /* right rotate at gparent */
            gparent->rb_left = tmp;
            parent->rb_right = gparent;
            if (tmp) {
                rb_set_parent_color(tmp, gparent, RB_BLACK);
            }
            rb_rotate_set_parents(gparent, parent, root, RB_RED);
            augment_rotate(gparent, parent);
            break;
        } else {
            tmp = gparent->rb_left;
            if (tmp && rb_is_red(tmp)) {
                rb_set_parent_color(tmp, gparent, RB_BLACK);
                rb_set_parent_color(parent, gparent, RB_BLACK);
                node = gparent;
                parent = rb_parent(node);
                rb_set_parent_color(node, parent, RB_RED);
                continue;
            }
            tmp = parent->rb_left;
            if (node == tmp) {
                /* right rotate at parent */
                tmp = node->rb_right;
                parent->rb_left = tmp;
                node->rb_right = parent;
                if (tmp) {
                    rb_set_parent_color(tmp, parent, RB_BLACK);
                }
                rb_set_parent_color(parent, node, RB_RED);
                augment_rotate(parent, node);
                parent = node;
                tmp = node->rb_left;
            }
            /* left rotate at gparent */
            gparent->rb_right = tmp;
            parent->rb_left = gparent;
            if (tmp) {
                rb_set_parent_color(tmp, gparent, RB_BLACK);
            }
            rb_rotate_set_parents(gparent, parent, root, RB_RED);
            augment_rotate(gparent, parent);
            break;
        }
    }
}

/*
 * One step of the repair after an erase, when the path through `*nodep`,
 * the left child of `*parentp` (NULL for a removed leaf), is one black node
 * short. Rotates and recolours around the sibling; returns 1 with the
 * deficit moved up to `*nodep` and `*parentp` when the step only recoloured
 * a black parent, else 0 once the deficit is paid.
 */
static int rb_erase_fix_left(struct rb_node **nodep, struct rb_node **parentp, struct rb_root *root,
                             void (*augment_rotate)(struct rb_node *old, struct rb_node *new_))
{
    struct rb_node *parent = *parentp, *sibling = parent->rb_right, *tmp1, *tmp2;

    if (rb_is_red(sibling)) {
        /* left rotate at parent */
        tmp1 = sibling->rb_left;
        parent->rb_right = tmp1;
        sibling->rb_left = parent;
        rb_set_parent_color(tmp1, parent, RB_BLACK);
        rb_rotate_set_parents(parent, sibling, root, RB_RED);
        augment_rotate(parent, sibling);
        sibling = tmp1;
    }
    tmp1 = sibling->rb_right;
    if (!tmp1 || rb_is_black(tmp1)) {
        tmp2 = sibling->rb_left;
        if (!tmp2 || rb_is_black(tmp2)) {
            /* recolour the sibling and move the deficit up */
            rb_set_parent_color(sibling, parent, RB_RED);
            if (rb_is_red(parent)) {
                rb_set_black(parent);
                return 0;
            }
            *nodep = parent;
            *parentp = rb_parent(parent);
            return *parentp != NULL;
        }
        /* right rotate at sibling */
        tmp1 = tmp2->rb_right;
        sibling->rb_left = tmp1;
        tmp2->rb_right = sibling;
        parent->rb_right = tmp2;
        if (tmp1) {
            rb_set_parent_color(tmp1, sibling, RB_BLACK);
        }
        augment_rotate(sibling, tmp2);
        tmp1 = sibling;
        sibling = tmp2;
    }
    /* left rotate at parent, colouring the sibling's far child */
    tmp2 = sibling->rb_left;
    parent->rb_right = tmp2;
    sibling->rb_left = parent;
    rb_set_parent_color(tmp1, sibling, RB_BLACK);
    if (tmp2) {
        rb_set_parent(tmp2, parent);
    }
    rb_rotate_set_parents(parent, sibling, root, RB_BLACK);
    augment_rotate(parent, sibling);
    return 0;
}

/** The mirror of `rb_erase_fix_left`, for `*nodep` the right child. */
static int rb_erase_fix_right(struct rb_node **nodep, struct rb_node **parentp,
                              struct rb_root *root,
                              void (*augment_rotate)(struct rb_node *old, struct rb_node *new_))
{
    struct rb_node *parent = *parentp, *sibling = parent->rb_left, *tmp1, *tmp2;

    if (rb_is_red(sibling)) {
        /* right rotate at parent */
        tmp1 = sibling->rb_right;
        parent->rb_left = tmp1;
        sibling->rb_right = parent;
        rb_set_parent_color(tmp1, parent, RB_BLACK);
        rb_rotate_set_parents(parent, sibling, root, RB_RED);
        augment_rotate(parent, sibling);
        sibling = tmp1;
    }
    tmp1 = sibling->rb_left;
    if (!tmp1 || rb_is_black(tmp1)) {
        tmp2 = sibling->rb_right;
        if (!tmp2 || rb_is_black(tmp2)) {
            rb_set_parent_color(sibling, parent, RB_RED);
            if (rb_is_red(parent)) {
                rb_set_black(parent);
                return 0;
            }
            *nodep = parent;
            *parentp = rb_parent(parent);
            return *parentp != NULL;
        }
        /* left rotate at sibling */
        tmp1 = tmp2->rb_left;
        sibling->rb_right = tmp1;
        tmp2->rb_left = sibling;
        parent->rb_left = tmp2;
        if (tmp1) {
            rb_set_parent_color(tmp1, sibling, RB_BLACK);
        }
        augment_rotate(sibling, tmp2);
        tmp1 = sibling;
        sibling = tmp2;
    }
    /* right rotate at parent */
    tmp2 = sibling->rb_right;
    parent->rb_left = tmp2;
    sibling->rb_right = parent;
    rb_set_parent_color(tmp1, sibling, RB_BLACK);
    if (tmp2) {
        rb_set_parent(tmp2, parent);
    }
    rb_rotate_set_parents(parent, sibling, root, RB_BLACK);
    augment_rotate(parent, sibling);
    return 0;
}

/**
 * Restore the properties after an erase left the path through `parent`'s
 * now-empty side one black node short: rotate and recolour around the
 * sibling until the deficit is paid or reaches the root.
 */
static void rb_erase_color(struct rb_node *parent, struct rb_root *root,
                           void (*augment_rotate)(struct rb_node *old, struct rb_node *new_))
{
    struct rb_node *node = NULL;
    int more;

    do {
        if (node != parent->rb_right) {
            more = rb_erase_fix_left(&node, &parent, root, augment_rotate);
        } else {
            more = rb_erase_fix_right(&node, &parent, root, augment_rotate);
        }
    } while (more);
}

/**
 * Unlink `node`, putting its successor in its place when it has two
 * children, and keep the summaries. Returns the node below which a black
 * node is now missing, for `rb_erase_color`, or NULL when colours were
 * fixed on the spot.
 */
static struct rb_node *rb_erase_unlink(struct rb_node *node, struct rb_root *root,
                                       const struct rb_augment_callbacks *augment)
{
    struct rb_node *child = node->rb_right;
    struct rb_node *tmp = node->rb_left;
    struct rb_node *parent, *rebalance;
    uintptr_t pc;

    if (!tmp) {
        /* at most one child, the right, which is then red under a black
         * node: it takes the node's place and colour */
        pc = node->rb_parent_color;
        parent = (struct rb_node *)(pc & ~(uintptr_t)3);
        rb_change_child(node, child, parent, root);
        if (child) {
            child->rb_parent_color = pc;
            rebalance = NULL;
        } else {
            rebalance = (pc & 1u) == RB_BLACK ? parent : NULL;
        }
        tmp = parent;
    } else if (!child) {
        /* only the left child, red under a black node */
        tmp->rb_parent_color = pc = node->rb_parent_color;
        parent = (struct rb_node *)(pc & ~(uintptr_t)3);
        rb_change_child(node, tmp, parent, root);
        rebalance = NULL;
        tmp = parent;
    } else {
        struct rb_node *successor = child, *child2;

        tmp = child->rb_left;
        if (!tmp) {
            /* the successor is the right child */
            parent = successor;
            child2 = successor->rb_right;
            augment->copy(node, successor);
        } else {
            /* the successor is the leftmost below the right child */
            do {
                parent = successor;
                successor = tmp;
                tmp = tmp->rb_left;
            } while (tmp);
            child2 = successor->rb_right;
            parent->rb_left = child2;
            successor->rb_right = child;
            rb_set_parent(child, successor);
            augment->copy(node, successor);
            augment->propagate(parent, successor);
        }
        tmp = node->rb_left;
        successor->rb_left = tmp;
        rb_set_parent(tmp, successor);
        pc = node->rb_parent_color;
        tmp = (struct rb_node *)(pc & ~(uintptr_t)3);
        rb_change_child(node, successor, tmp, root);
        if (child2) {
            rb_set_parent_color(child2, parent, RB_BLACK);
            rebalance = NULL;
        } else {
            rebalance = rb_is_black(successor) ? parent : NULL;
        }
        successor->rb_parent_color = pc;
        tmp = successor;
    }
    augment->propagate(tmp, NULL);
    return rebalance;
}

void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **link)
{
    node->rb_parent_color = (uintptr_t)parent;
    node->rb_left = node->rb_right = NULL;
    *link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    rb_insert_fix(node, root, rb_dummy_rotate);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *rebalance = rb_erase_unlink(node, root, &rb_dummy_callbacks);

    if (rebalance) {
        rb_erase_color(rebalance, root, rb_dummy_rotate);
    }
}

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         const struct rb_augment_callbacks *augment)
{
    rb_insert_fix(node, root, augment->rotate);
}

void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        const struct rb_augment_callbacks *augment)
{
    struct rb_node *rebalance = rb_erase_unlink(node, root, augment);

    if (rebalance) {
        rb_erase_color(rebalance, root, augment->rotate);
    }
}

struct rb_node *rb_add_augmented_cached(struct rb_node *node, struct rb_root_cached *tree,
                                        int (*less)(const struct rb_node *a,
                                                    const struct rb_node *b),
                                        const struct rb_augment_callbacks *augment)
{
    struct rb_node **link = &tree->rb_root.rb_node;
    struct rb_node *parent = NULL;
    int leftmost = 1;

    while (*link) {
        parent = *link;
        if (less(node, parent)) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = 0;
        }
    }
    rb_link_node(node, parent, link);
    /* the new leaf's ancestors may now summarise it; Linux notes this
     * propagation as suboptimal, since the rotations recompute some */
    augment->propagate(parent, NULL);
    if (leftmost) {
        tree->rb_leftmost = node;
    }
    rb_insert_augmented(node, &tree->rb_root, augment);
    return leftmost ? node : NULL;
}

void rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
                               const struct rb_augment_callbacks *augment)
{
    if (root->rb_leftmost == node) {
        root->rb_leftmost = rb_next(node);
    }
    rb_erase_augmented(node, &root->rb_root, augment);
}

struct rb_node *rb_first(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;

    if (!n) {
        return NULL;
    }
    while (n->rb_left) {
        n = n->rb_left;
    }
    return n;
}

struct rb_node *rb_last(const struct rb_root *root)
{
    struct rb_node *n = root->rb_node;

    if (!n) {
        return NULL;
    }
    while (n->rb_right) {
        n = n->rb_right;
    }
    return n;
}

struct rb_node *rb_next(const struct rb_node *node)
{
    struct rb_node *parent;
    struct rb_node *n = (struct rb_node *)(uintptr_t)node;

    if (RB_EMPTY_NODE(n)) {
        return NULL;
    }
    if (n->rb_right) {
        n = n->rb_right;
        while (n->rb_left) {
            n = n->rb_left;
        }
        return n;
    }
    while ((parent = rb_parent(n)) != NULL && n == parent->rb_right) {
        n = parent;
    }
    return parent;
}

struct rb_node *rb_prev(const struct rb_node *node)
{
    struct rb_node *parent;
    struct rb_node *n = (struct rb_node *)(uintptr_t)node;

    if (RB_EMPTY_NODE(n)) {
        return NULL;
    }
    if (n->rb_left) {
        n = n->rb_left;
        while (n->rb_right) {
            n = n->rb_right;
        }
        return n;
    }
    while ((parent = rb_parent(n)) != NULL && n == parent->rb_left) {
        n = parent;
    }
    return parent;
}
