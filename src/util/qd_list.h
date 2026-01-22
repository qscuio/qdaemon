/*
 * QDaemon - Intrusive Doubly Linked List
 * Based on Linux kernel list implementation
 */

#ifndef QD_LIST_H
#define QD_LIST_H

#include <stddef.h>

/* Intrusive list head */
struct qd_list_head {
    struct qd_list_head *next;
    struct qd_list_head *prev;
};

/* Initialize list head */
#define QD_LIST_HEAD_INIT(name) { &(name), &(name) }

#define QD_LIST_HEAD(name) \
    struct qd_list_head name = QD_LIST_HEAD_INIT(name)

static inline void qd_list_init(struct qd_list_head *list)
{
    list->next = list;
    list->prev = list;
}

/* Internal: insert new entry between two entries */
static inline void __qd_list_add(struct qd_list_head *new,
                                  struct qd_list_head *prev,
                                  struct qd_list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

/* Add entry after head */
static inline void qd_list_add(struct qd_list_head *new,
                                struct qd_list_head *head)
{
    __qd_list_add(new, head, head->next);
}

/* Add entry before head (at tail) */
static inline void qd_list_add_tail(struct qd_list_head *new,
                                     struct qd_list_head *head)
{
    __qd_list_add(new, head->prev, head);
}

/* Internal: remove entry by connecting prev and next */
static inline void __qd_list_del(struct qd_list_head *prev,
                                  struct qd_list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

/* Remove entry from list */
static inline void qd_list_del(struct qd_list_head *entry)
{
    __qd_list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

/* Remove and reinitialize entry */
static inline void qd_list_del_init(struct qd_list_head *entry)
{
    __qd_list_del(entry->prev, entry->next);
    qd_list_init(entry);
}

/* Replace old entry with new */
static inline void qd_list_replace(struct qd_list_head *old,
                                    struct qd_list_head *new)
{
    new->next = old->next;
    new->next->prev = new;
    new->prev = old->prev;
    new->prev->next = new;
}

/* Move entry from one list to another (after head) */
static inline void qd_list_move(struct qd_list_head *entry,
                                 struct qd_list_head *head)
{
    __qd_list_del(entry->prev, entry->next);
    qd_list_add(entry, head);
}

/* Move entry from one list to another (before head) */
static inline void qd_list_move_tail(struct qd_list_head *entry,
                                      struct qd_list_head *head)
{
    __qd_list_del(entry->prev, entry->next);
    qd_list_add_tail(entry, head);
}

/* Check if list is empty */
static inline int qd_list_empty(const struct qd_list_head *head)
{
    return head->next == head;
}

/* Check if entry is the last one */
static inline int qd_list_is_last(const struct qd_list_head *entry,
                                   const struct qd_list_head *head)
{
    return entry->next == head;
}

/* Check if entry is the first one */
static inline int qd_list_is_first(const struct qd_list_head *entry,
                                    const struct qd_list_head *head)
{
    return entry->prev == head;
}

/* Check if list has only one entry */
static inline int qd_list_is_singular(const struct qd_list_head *head)
{
    return !qd_list_empty(head) && (head->next == head->prev);
}

/* Splice list after head */
static inline void __qd_list_splice(const struct qd_list_head *list,
                                     struct qd_list_head *prev,
                                     struct qd_list_head *next)
{
    struct qd_list_head *first = list->next;
    struct qd_list_head *last = list->prev;

    first->prev = prev;
    prev->next = first;

    last->next = next;
    next->prev = last;
}

static inline void qd_list_splice(const struct qd_list_head *list,
                                   struct qd_list_head *head)
{
    if (!qd_list_empty(list))
        __qd_list_splice(list, head, head->next);
}

static inline void qd_list_splice_tail(const struct qd_list_head *list,
                                        struct qd_list_head *head)
{
    if (!qd_list_empty(list))
        __qd_list_splice(list, head->prev, head);
}

/* Get container structure from list entry */
#define qd_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define qd_list_entry(ptr, type, member) \
    qd_container_of(ptr, type, member)

#define qd_list_first_entry(ptr, type, member) \
    qd_list_entry((ptr)->next, type, member)

#define qd_list_last_entry(ptr, type, member) \
    qd_list_entry((ptr)->prev, type, member)

#define qd_list_first_entry_or_null(ptr, type, member) \
    (!qd_list_empty(ptr) ? qd_list_first_entry(ptr, type, member) : NULL)

#define qd_list_next_entry(pos, member) \
    qd_list_entry((pos)->member.next, typeof(*(pos)), member)

#define qd_list_prev_entry(pos, member) \
    qd_list_entry((pos)->member.prev, typeof(*(pos)), member)

/* Iterate over list */
#define qd_list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define qd_list_for_each_prev(pos, head) \
    for (pos = (head)->prev; pos != (head); pos = pos->prev)

#define qd_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

#define qd_list_for_each_prev_safe(pos, n, head) \
    for (pos = (head)->prev, n = pos->prev; pos != (head); \
         pos = n, n = pos->prev)

/* Iterate over list entries */
#define qd_list_for_each_entry(pos, head, member) \
    for (pos = qd_list_first_entry(head, typeof(*pos), member); \
         &pos->member != (head); \
         pos = qd_list_next_entry(pos, member))

#define qd_list_for_each_entry_reverse(pos, head, member) \
    for (pos = qd_list_last_entry(head, typeof(*pos), member); \
         &pos->member != (head); \
         pos = qd_list_prev_entry(pos, member))

#define qd_list_for_each_entry_safe(pos, n, head, member) \
    for (pos = qd_list_first_entry(head, typeof(*pos), member), \
         n = qd_list_next_entry(pos, member); \
         &pos->member != (head); \
         pos = n, n = qd_list_next_entry(n, member))

/* Count entries in list */
static inline size_t qd_list_count(const struct qd_list_head *head)
{
    size_t count = 0;
    struct qd_list_head *pos;
    qd_list_for_each(pos, head)
        count++;
    return count;
}

#endif /* QD_LIST_H */
