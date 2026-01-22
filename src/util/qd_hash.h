/*
 * QDaemon - Hash Table
 * Generic hash table with chaining
 */

#ifndef QD_HASH_H
#define QD_HASH_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "qd_list.h"

/* Hash table entry using intrusive list */
struct qd_hlist_node {
    struct qd_hlist_node *next;
    struct qd_hlist_node **pprev;
};

struct qd_hlist_head {
    struct qd_hlist_node *first;
};

#define QD_HLIST_HEAD_INIT { NULL }
#define QD_HLIST_HEAD(name) struct qd_hlist_head name = { NULL }

static inline void qd_hlist_init(struct qd_hlist_head *h)
{
    h->first = NULL;
}

static inline void qd_hlist_node_init(struct qd_hlist_node *n)
{
    n->next = NULL;
    n->pprev = NULL;
}

static inline int qd_hlist_empty(const struct qd_hlist_head *h)
{
    return !h->first;
}

static inline int qd_hlist_unhashed(const struct qd_hlist_node *h)
{
    return !h->pprev;
}

static inline void qd_hlist_del(struct qd_hlist_node *n)
{
    struct qd_hlist_node *next = n->next;
    struct qd_hlist_node **pprev = n->pprev;

    *pprev = next;
    if (next)
        next->pprev = pprev;

    n->next = NULL;
    n->pprev = NULL;
}

static inline void qd_hlist_add_head(struct qd_hlist_node *n, struct qd_hlist_head *h)
{
    struct qd_hlist_node *first = h->first;
    n->next = first;
    if (first)
        first->pprev = &n->next;
    h->first = n;
    n->pprev = &h->first;
}

#define qd_hlist_entry(ptr, type, member) \
    qd_container_of(ptr, type, member)

#define qd_hlist_for_each(pos, head) \
    for (pos = (head)->first; pos; pos = pos->next)

#define qd_hlist_for_each_safe(pos, n, head) \
    for (pos = (head)->first; pos && ({ n = pos->next; 1; }); pos = n)

#define qd_hlist_for_each_entry(pos, head, member) \
    for (pos = (head)->first ? qd_hlist_entry((head)->first, typeof(*pos), member) : NULL; \
         pos; \
         pos = pos->member.next ? qd_hlist_entry(pos->member.next, typeof(*pos), member) : NULL)

#define qd_hlist_for_each_entry_safe(pos, n, head, member) \
    for (pos = (head)->first ? qd_hlist_entry((head)->first, typeof(*pos), member) : NULL; \
         pos && ({ n = pos->member.next; 1; }); \
         pos = n ? qd_hlist_entry(n, typeof(*pos), member) : NULL)

/* Hash functions */

/* FNV-1a hash for strings */
static inline uint32_t qd_hash_string(const char *str)
{
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

/* FNV-1a hash for binary data */
static inline uint32_t qd_hash_bytes(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t hash = 2166136261u;
    while (len--) {
        hash ^= *p++;
        hash *= 16777619u;
    }
    return hash;
}

/* Integer hash (based on splitmix64) */
static inline uint64_t qd_hash_u64(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x ^= (x >> 31);
    return x;
}

static inline uint32_t qd_hash_u32(uint32_t x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

/* Pointer hash */
static inline uint32_t qd_hash_ptr(const void *ptr)
{
    return (uint32_t)qd_hash_u64((uint64_t)(uintptr_t)ptr);
}

/* Generic hash table structure */
typedef struct qd_hashtable {
    struct qd_hlist_head *buckets;
    size_t size;
    size_t count;
    size_t mask;
} qd_hashtable_t;

/* Initialize hash table with given number of buckets (must be power of 2) */
static inline int qd_hashtable_init(qd_hashtable_t *ht, size_t size)
{
    /* Ensure size is power of 2 */
    if (size == 0)
        size = 16;

    size_t n = 1;
    while (n < size)
        n <<= 1;
    size = n;

    ht->buckets = calloc(size, sizeof(struct qd_hlist_head));
    if (!ht->buckets)
        return -1;

    ht->size = size;
    ht->mask = size - 1;
    ht->count = 0;

    for (size_t i = 0; i < size; i++)
        qd_hlist_init(&ht->buckets[i]);

    return 0;
}

static inline void qd_hashtable_destroy(qd_hashtable_t *ht)
{
    free(ht->buckets);
    ht->buckets = NULL;
    ht->size = 0;
    ht->count = 0;
}

static inline struct qd_hlist_head *qd_hashtable_bucket(qd_hashtable_t *ht, uint32_t hash)
{
    return &ht->buckets[hash & ht->mask];
}

/* String-keyed hash table entry */
typedef struct qd_hash_entry {
    struct qd_hlist_node node;
    char *key;
    void *value;
} qd_hash_entry_t;

/* String hash map */
typedef struct qd_hashmap {
    qd_hashtable_t ht;
} qd_hashmap_t;

static inline int qd_hashmap_init(qd_hashmap_t *map, size_t size)
{
    return qd_hashtable_init(&map->ht, size);
}

static inline void qd_hashmap_destroy(qd_hashmap_t *map)
{
    /* Free all entries */
    for (size_t i = 0; i < map->ht.size; i++) {
        struct qd_hlist_node *pos, *n;
        qd_hlist_for_each_safe(pos, n, &map->ht.buckets[i]) {
            qd_hash_entry_t *entry = qd_hlist_entry(pos, qd_hash_entry_t, node);
            qd_hlist_del(pos);
            free(entry->key);
            free(entry);
        }
    }
    qd_hashtable_destroy(&map->ht);
}

static inline void *qd_hashmap_get(qd_hashmap_t *map, const char *key)
{
    uint32_t hash = qd_hash_string(key);
    struct qd_hlist_head *head = qd_hashtable_bucket(&map->ht, hash);

    qd_hash_entry_t *entry;
    qd_hlist_for_each_entry(entry, head, node) {
        if (strcmp(entry->key, key) == 0)
            return entry->value;
    }
    return NULL;
}

static inline int qd_hashmap_put(qd_hashmap_t *map, const char *key, void *value)
{
    uint32_t hash = qd_hash_string(key);
    struct qd_hlist_head *head = qd_hashtable_bucket(&map->ht, hash);

    /* Check if key exists */
    qd_hash_entry_t *entry;
    qd_hlist_for_each_entry(entry, head, node) {
        if (strcmp(entry->key, key) == 0) {
            entry->value = value;
            return 0;
        }
    }

    /* Create new entry */
    entry = malloc(sizeof(qd_hash_entry_t));
    if (!entry)
        return -1;

    entry->key = strdup(key);
    if (!entry->key) {
        free(entry);
        return -1;
    }

    entry->value = value;
    qd_hlist_node_init(&entry->node);
    qd_hlist_add_head(&entry->node, head);
    map->ht.count++;

    return 0;
}

static inline void *qd_hashmap_remove(qd_hashmap_t *map, const char *key)
{
    uint32_t hash = qd_hash_string(key);
    struct qd_hlist_head *head = qd_hashtable_bucket(&map->ht, hash);

    qd_hash_entry_t *entry;
    struct qd_hlist_node *n;
    qd_hlist_for_each_entry_safe(entry, n, head, node) {
        if (strcmp(entry->key, key) == 0) {
            void *value = entry->value;
            qd_hlist_del(&entry->node);
            free(entry->key);
            free(entry);
            map->ht.count--;
            return value;
        }
    }
    return NULL;
}

static inline int qd_hashmap_contains(qd_hashmap_t *map, const char *key)
{
    return qd_hashmap_get(map, key) != NULL;
}

static inline size_t qd_hashmap_size(qd_hashmap_t *map)
{
    return map->ht.count;
}

/* Iteration macro for hashmap */
#define qd_hashmap_for_each(entry, map) \
    for (size_t __i = 0; __i < (map)->ht.size; __i++) \
        qd_hlist_for_each_entry(entry, &(map)->ht.buckets[__i], node)

#endif /* QD_HASH_H */
