/*
 * QDaemon - Memory Allocator API
 * Slab allocator for fixed-size objects + Arena for batch allocations
 */

#ifndef QD_MEMORY_H
#define QD_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "qd_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct qd_slab qd_slab_t;
typedef struct qd_slab_cache qd_slab_cache_t;
typedef struct qd_arena qd_arena_t;

/* Slab configuration */
#define QD_SLAB_MIN_SIZE       8
#define QD_SLAB_MAX_SIZE       4096
#define QD_SLAB_PAGE_SIZE      4096
#define QD_SLAB_PAGES_PER_SLAB 1
#define QD_SLAB_CPU_CACHE_SIZE 32

/* Slab flags */
#define QD_SLAB_ZERO       0x01  /* Zero memory on allocation */
#define QD_SLAB_RECLAIM    0x02  /* Allow reclaiming empty slabs */
#define QD_SLAB_POISON     0x04  /* Debug: poison freed memory */
#define QD_SLAB_RED_ZONE   0x08  /* Debug: add red zones */

/* Per-CPU cache for lock-free fast path */
typedef struct qd_slab_cpu_cache {
    void *freelist[QD_SLAB_CPU_CACHE_SIZE];
    int count;
} QD_CACHE_ALIGNED qd_slab_cpu_cache_t;

/* Individual slab */
struct qd_slab {
    void *base;              /* Base address of slab memory */
    void *freelist;          /* Free object list */
    uint32_t inuse;          /* Objects in use */
    uint32_t total;          /* Total objects in slab */
    qd_slab_cache_t *cache;  /* Owning cache */
    struct qd_slab *next;    /* List linkage */
    struct qd_slab *prev;
};

/* Slab cache for a specific object size */
struct qd_slab_cache {
    char name[32];           /* Cache name for debugging */
    size_t obj_size;         /* Object size (including alignment) */
    size_t obj_align;        /* Object alignment */
    size_t slab_size;        /* Size of each slab */
    uint32_t flags;          /* Cache flags */
    uint32_t objs_per_slab;  /* Objects per slab */

    qd_slab_t *slabs_full;   /* Fully used slabs */
    qd_slab_t *slabs_partial; /* Partially used slabs */
    qd_slab_t *slabs_empty;  /* Empty slabs */

    pthread_mutex_t lock;    /* Cache lock */

    /* Per-CPU caches for fast path */
    qd_slab_cpu_cache_t *cpu_caches;
    int num_cpus;

    /* Statistics */
    uint64_t allocs;
    uint64_t frees;
    uint64_t cache_hits;
    uint64_t cache_misses;
};

/* Arena allocator for batch/temporary allocations */
struct qd_arena {
    void *base;              /* Base address */
    size_t size;             /* Total size */
    size_t offset;           /* Current allocation offset */
    size_t peak;             /* Peak usage */
    qd_arena_t *next;        /* Linked arenas for overflow */
    qd_arena_t *current;     /* Current arena for allocation */
};

/* Arena configuration */
#define QD_ARENA_DEFAULT_SIZE (64 * 1024)  /* 64KB default */
#define QD_ARENA_MIN_SIZE     4096

/*
 * Slab Allocator API
 */

/* Create a slab cache for objects of given size */
qd_slab_cache_t *qd_slab_cache_create(const char *name, size_t size, size_t align, uint32_t flags);

/* Destroy a slab cache */
void qd_slab_cache_destroy(qd_slab_cache_t *cache);

/* Allocate from slab cache */
void *qd_slab_alloc(qd_slab_cache_t *cache);

/* Allocate and zero memory */
void *qd_slab_zalloc(qd_slab_cache_t *cache);

/* Free to slab cache */
void qd_slab_free(qd_slab_cache_t *cache, void *ptr);

/* Shrink cache by freeing empty slabs */
int qd_slab_cache_shrink(qd_slab_cache_t *cache);

/* Get cache statistics */
typedef struct qd_slab_stats {
    uint64_t allocs;
    uint64_t frees;
    uint64_t cache_hits;
    uint64_t cache_misses;
    size_t active_objs;
    size_t total_slabs;
    size_t free_slabs;
    size_t memory_used;
} qd_slab_stats_t;

void qd_slab_cache_stats(qd_slab_cache_t *cache, qd_slab_stats_t *stats);

/*
 * Arena Allocator API
 */

/* Create an arena */
qd_arena_t *qd_arena_create(size_t initial_size);

/* Allocate from arena (no individual free) */
void *qd_arena_alloc(qd_arena_t *arena, size_t size);

/* Allocate with alignment */
void *qd_arena_alloc_aligned(qd_arena_t *arena, size_t size, size_t align);

/* Allocate and zero memory */
void *qd_arena_zalloc(qd_arena_t *arena, size_t size);

/* Duplicate string into arena */
char *qd_arena_strdup(qd_arena_t *arena, const char *str);

/* Duplicate n bytes of string into arena */
char *qd_arena_strndup(qd_arena_t *arena, const char *str, size_t n);

/* Reset arena (free all allocations, keep memory) */
void qd_arena_reset(qd_arena_t *arena);

/* Destroy arena (release all memory) */
void qd_arena_destroy(qd_arena_t *arena);

/* Get arena usage statistics */
typedef struct qd_arena_stats {
    size_t allocated;
    size_t peak;
    size_t total_size;
    size_t num_blocks;
} qd_arena_stats_t;

void qd_arena_stats(qd_arena_t *arena, qd_arena_stats_t *stats);

/*
 * General Memory API
 * Routes to appropriate backend based on size
 */

/* Initialize memory subsystem */
int qd_memory_init(void);

/* Shutdown memory subsystem */
void qd_memory_shutdown(void);

/* General allocation (uses slab for small, malloc for large) */
void *qd_malloc(size_t size);
void *qd_calloc(size_t nmemb, size_t size);
void *qd_realloc(void *ptr, size_t size);
void qd_free(void *ptr);

/* Aligned allocation */
void *qd_aligned_alloc(size_t alignment, size_t size);
void qd_aligned_free(void *ptr);

/* Duplicate memory */
void *qd_memdup(const void *src, size_t size);

/* Duplicate string */
char *qd_strdup(const char *str);
char *qd_strndup(const char *str, size_t n);

/* Memory statistics */
typedef struct qd_memory_stats {
    size_t total_allocated;
    size_t total_freed;
    size_t current_usage;
    size_t peak_usage;
    size_t slab_allocations;
    size_t heap_allocations;
} qd_memory_stats_t;

void qd_memory_stats(qd_memory_stats_t *stats);

/* Debug: dump memory state */
void qd_memory_dump(void);

/*
 * Memory Pool for fixed-size objects (simpler than slab)
 */
typedef struct qd_mempool {
    void *base;
    void *freelist;
    size_t obj_size;
    size_t capacity;
    size_t count;
    pthread_mutex_t lock;
} qd_mempool_t;

qd_mempool_t *qd_mempool_create(size_t obj_size, size_t count);
void qd_mempool_destroy(qd_mempool_t *pool);
void *qd_mempool_alloc(qd_mempool_t *pool);
void qd_mempool_free(qd_mempool_t *pool, void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* QD_MEMORY_H */
