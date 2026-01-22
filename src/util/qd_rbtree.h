/*
 * QDaemon - Red-Black Tree
 * Intrusive red-black tree implementation
 */

#ifndef QD_RBTREE_H
#define QD_RBTREE_H

#include <stddef.h>
#include <stdint.h>

/* Red-black tree node colors */
#define QD_RB_RED   0
#define QD_RB_BLACK 1

/* Red-black tree node */
struct qd_rb_node {
    struct qd_rb_node *parent;
    struct qd_rb_node *left;
    struct qd_rb_node *right;
    int color;
};

/* Red-black tree root */
struct qd_rb_root {
    struct qd_rb_node *node;
};

#define QD_RB_ROOT_INIT { NULL }
#define QD_RB_ROOT (struct qd_rb_root) { NULL }

#define qd_rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

static inline void qd_rb_init_node(struct qd_rb_node *node)
{
    node->parent = NULL;
    node->left = NULL;
    node->right = NULL;
    node->color = QD_RB_RED;
}

static inline int qd_rb_empty(const struct qd_rb_root *root)
{
    return root->node == NULL;
}

/* Internal rotation functions */
static inline void __qd_rb_rotate_left(struct qd_rb_node *node, struct qd_rb_root *root)
{
    struct qd_rb_node *right = node->right;
    struct qd_rb_node *parent = node->parent;

    node->right = right->left;
    if (right->left)
        right->left->parent = node;

    right->left = node;
    right->parent = parent;

    if (parent) {
        if (node == parent->left)
            parent->left = right;
        else
            parent->right = right;
    } else {
        root->node = right;
    }
    node->parent = right;
}

static inline void __qd_rb_rotate_right(struct qd_rb_node *node, struct qd_rb_root *root)
{
    struct qd_rb_node *left = node->left;
    struct qd_rb_node *parent = node->parent;

    node->left = left->right;
    if (left->right)
        left->right->parent = node;

    left->right = node;
    left->parent = parent;

    if (parent) {
        if (node == parent->right)
            parent->right = left;
        else
            parent->left = left;
    } else {
        root->node = left;
    }
    node->parent = left;
}

/* Fix up tree after insertion */
static inline void qd_rb_insert_fixup(struct qd_rb_node *node, struct qd_rb_root *root)
{
    struct qd_rb_node *parent, *gparent, *uncle;

    while ((parent = node->parent) && parent->color == QD_RB_RED) {
        gparent = parent->parent;

        if (parent == gparent->left) {
            uncle = gparent->right;
            if (uncle && uncle->color == QD_RB_RED) {
                uncle->color = QD_RB_BLACK;
                parent->color = QD_RB_BLACK;
                gparent->color = QD_RB_RED;
                node = gparent;
                continue;
            }

            if (node == parent->right) {
                __qd_rb_rotate_left(parent, root);
                node = parent;
                parent = node->parent;
            }

            parent->color = QD_RB_BLACK;
            gparent->color = QD_RB_RED;
            __qd_rb_rotate_right(gparent, root);
        } else {
            uncle = gparent->left;
            if (uncle && uncle->color == QD_RB_RED) {
                uncle->color = QD_RB_BLACK;
                parent->color = QD_RB_BLACK;
                gparent->color = QD_RB_RED;
                node = gparent;
                continue;
            }

            if (node == parent->left) {
                __qd_rb_rotate_right(parent, root);
                node = parent;
                parent = node->parent;
            }

            parent->color = QD_RB_BLACK;
            gparent->color = QD_RB_RED;
            __qd_rb_rotate_left(gparent, root);
        }
    }

    root->node->color = QD_RB_BLACK;
}

/* Link node to parent */
static inline void qd_rb_link_node(struct qd_rb_node *node, struct qd_rb_node *parent,
                                    struct qd_rb_node **link)
{
    node->parent = parent;
    node->left = node->right = NULL;
    node->color = QD_RB_RED;

    *link = node;
}

/* Fix up tree after removal */
static inline void __qd_rb_erase_fixup(struct qd_rb_node *node, struct qd_rb_node *parent,
                                        struct qd_rb_root *root)
{
    struct qd_rb_node *sibling;

    while ((!node || node->color == QD_RB_BLACK) && node != root->node) {
        if (node == parent->left) {
            sibling = parent->right;
            if (sibling->color == QD_RB_RED) {
                sibling->color = QD_RB_BLACK;
                parent->color = QD_RB_RED;
                __qd_rb_rotate_left(parent, root);
                sibling = parent->right;
            }
            if ((!sibling->left || sibling->left->color == QD_RB_BLACK) &&
                (!sibling->right || sibling->right->color == QD_RB_BLACK)) {
                sibling->color = QD_RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!sibling->right || sibling->right->color == QD_RB_BLACK) {
                    if (sibling->left)
                        sibling->left->color = QD_RB_BLACK;
                    sibling->color = QD_RB_RED;
                    __qd_rb_rotate_right(sibling, root);
                    sibling = parent->right;
                }
                sibling->color = parent->color;
                parent->color = QD_RB_BLACK;
                if (sibling->right)
                    sibling->right->color = QD_RB_BLACK;
                __qd_rb_rotate_left(parent, root);
                node = root->node;
                break;
            }
        } else {
            sibling = parent->left;
            if (sibling->color == QD_RB_RED) {
                sibling->color = QD_RB_BLACK;
                parent->color = QD_RB_RED;
                __qd_rb_rotate_right(parent, root);
                sibling = parent->left;
            }
            if ((!sibling->left || sibling->left->color == QD_RB_BLACK) &&
                (!sibling->right || sibling->right->color == QD_RB_BLACK)) {
                sibling->color = QD_RB_RED;
                node = parent;
                parent = node->parent;
            } else {
                if (!sibling->left || sibling->left->color == QD_RB_BLACK) {
                    if (sibling->right)
                        sibling->right->color = QD_RB_BLACK;
                    sibling->color = QD_RB_RED;
                    __qd_rb_rotate_left(sibling, root);
                    sibling = parent->left;
                }
                sibling->color = parent->color;
                parent->color = QD_RB_BLACK;
                if (sibling->left)
                    sibling->left->color = QD_RB_BLACK;
                __qd_rb_rotate_right(parent, root);
                node = root->node;
                break;
            }
        }
    }
    if (node)
        node->color = QD_RB_BLACK;
}

/* Remove node from tree */
static inline void qd_rb_erase(struct qd_rb_node *node, struct qd_rb_root *root)
{
    struct qd_rb_node *child, *parent;
    int color;

    if (!node->left) {
        child = node->right;
    } else if (!node->right) {
        child = node->left;
    } else {
        struct qd_rb_node *old = node, *left;

        node = node->right;
        while ((left = node->left) != NULL)
            node = left;

        if (old->parent) {
            if (old->parent->left == old)
                old->parent->left = node;
            else
                old->parent->right = node;
        } else {
            root->node = node;
        }

        child = node->right;
        parent = node->parent;
        color = node->color;

        if (parent == old) {
            parent = node;
        } else {
            if (child)
                child->parent = parent;
            parent->left = child;

            node->right = old->right;
            old->right->parent = node;
        }

        node->parent = old->parent;
        node->color = old->color;
        node->left = old->left;
        old->left->parent = node;

        if (color == QD_RB_BLACK)
            __qd_rb_erase_fixup(child, parent, root);
        return;
    }

    parent = node->parent;
    color = node->color;

    if (child)
        child->parent = parent;

    if (parent) {
        if (parent->left == node)
            parent->left = child;
        else
            parent->right = child;
    } else {
        root->node = child;
    }

    if (color == QD_RB_BLACK)
        __qd_rb_erase_fixup(child, parent, root);
}

/* Get first (leftmost) node */
static inline struct qd_rb_node *qd_rb_first(const struct qd_rb_root *root)
{
    struct qd_rb_node *n = root->node;
    if (!n)
        return NULL;
    while (n->left)
        n = n->left;
    return n;
}

/* Get last (rightmost) node */
static inline struct qd_rb_node *qd_rb_last(const struct qd_rb_root *root)
{
    struct qd_rb_node *n = root->node;
    if (!n)
        return NULL;
    while (n->right)
        n = n->right;
    return n;
}

/* Get next node in order */
static inline struct qd_rb_node *qd_rb_next(const struct qd_rb_node *node)
{
    struct qd_rb_node *parent;

    if (node->right) {
        node = node->right;
        while (node->left)
            node = node->left;
        return (struct qd_rb_node *)node;
    }

    while ((parent = node->parent) && node == parent->right)
        node = parent;

    return parent;
}

/* Get previous node in order */
static inline struct qd_rb_node *qd_rb_prev(const struct qd_rb_node *node)
{
    struct qd_rb_node *parent;

    if (node->left) {
        node = node->left;
        while (node->right)
            node = node->right;
        return (struct qd_rb_node *)node;
    }

    while ((parent = node->parent) && node == parent->left)
        node = parent;

    return parent;
}

/* Replace a node with another */
static inline void qd_rb_replace_node(struct qd_rb_node *old, struct qd_rb_node *new,
                                       struct qd_rb_root *root)
{
    struct qd_rb_node *parent = old->parent;

    if (parent) {
        if (old == parent->left)
            parent->left = new;
        else
            parent->right = new;
    } else {
        root->node = new;
    }

    if (old->left)
        old->left->parent = new;
    if (old->right)
        old->right->parent = new;

    *new = *old;
}

/* Iteration macros */
#define qd_rb_for_each(pos, root) \
    for (pos = qd_rb_first(root); pos; pos = qd_rb_next(pos))

#define qd_rb_for_each_entry(pos, root, member) \
    for (pos = (root)->node ? qd_rb_entry(qd_rb_first(root), typeof(*pos), member) : NULL; \
         pos; \
         pos = qd_rb_next(&pos->member) ? qd_rb_entry(qd_rb_next(&pos->member), typeof(*pos), member) : NULL)

#define qd_rb_for_each_safe(pos, n, root) \
    for (pos = qd_rb_first(root), n = pos ? qd_rb_next(pos) : NULL; \
         pos; \
         pos = n, n = pos ? qd_rb_next(pos) : NULL)

#endif /* QD_RBTREE_H */
