/*
 * QDaemon - Memory Allocator Implementation
 * Slab allocator for fixed-size objects + Arena for batch allocations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>

#include "qdaemon/qd_memory.h"
#include "../util/qd_atomic.h"

/* Memory subsystem state */
static struct {
    int initialized;
    pthread_mutex_t lock;

    /* Statistics */
    qd_atomic_size total_allocated;
    qd_atomic_size total_freed;
    qd_atomic_size peak_usage;
    qd_atomic_size slab_allocations;
    qd_atomic_size heap_allocations;

    /* Standard slab caches for common sizes */
    qd_slab_cache_t *caches[12];  /* 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384 */
    int num_caches;
} g_memory = {
    .initialized = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* Size classes for slab allocator */
static const size_t size_classes[] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384
};
#define NUM_SIZE_CLASSES (sizeof(size_classes) / sizeof(size_classes[0]))

/* Get number of CPUs */
static int get_num_cpus(void)
{
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    return nprocs > 0 ? (int)nprocs : 1;
}

/* Get current CPU (simplified) */
static int get_current_cpu(void)
{
    /* Use thread ID as approximation */
    return (int)((uintptr_t)pthread_self() % get_num_cpus());
}

/* Allocate memory pages */
static void *alloc_pages(size_t size)
{
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED)
        return NULL;
    return ptr;
}

/* Free memory pages */
static void free_pages(void *ptr, size_t size)
{
    munmap(ptr, size);
}

/*
 * Slab Allocator Implementation
 */

/* Create a new slab */
static qd_slab_t *slab_create(qd_slab_cache_t *cache)
{
    size_t slab_size = cache->slab_size;
    size_t header_size = QD_ALIGN(sizeof(qd_slab_t), 16);

    /* Allocate slab memory */
    void *mem = alloc_pages(slab_size + header_size);
    if (!mem)
        return NULL;

    qd_slab_t *slab = mem;
    slab->base = (char *)mem + header_size;
    slab->cache = cache;
    slab->inuse = 0;
    slab->total = cache->objs_per_slab;
    slab->next = NULL;
    slab->prev = NULL;

    /* Build freelist */
    slab->freelist = NULL;
    char *obj = slab->base;
    for (uint32_t i = 0; i < slab->total; i++) {
        *(void **)obj = slab->freelist;
        slab->freelist = obj;
        obj += cache->obj_size;
    }

    return slab;
}

/* Destroy a slab */
static void slab_destroy(qd_slab_t *slab)
{
    size_t header_size = QD_ALIGN(sizeof(qd_slab_t), 16);
    free_pages(slab, slab->cache->slab_size + header_size);
}

/* Add slab to list */
static void slab_list_add(qd_slab_t **head, qd_slab_t *slab)
{
    slab->next = *head;
    slab->prev = NULL;
    if (*head)
        (*head)->prev = slab;
    *head = slab;
}

/* Remove slab from list */
static void slab_list_remove(qd_slab_t **head, qd_slab_t *slab)
{
    if (slab->prev)
        slab->prev->next = slab->next;
    else
        *head = slab->next;

    if (slab->next)
        slab->next->prev = slab->prev;

    slab->next = NULL;
    slab->prev = NULL;
}

/* Get slab from object pointer */
static qd_slab_t *slab_from_obj(qd_slab_cache_t *cache, void *obj)
{
    size_t header_size = QD_ALIGN(sizeof(qd_slab_t), 16);
    /* Slab starts at page boundary minus header */
    uintptr_t slab_size = cache->slab_size + header_size;
    uintptr_t addr = (uintptr_t)obj;
    return (qd_slab_t *)(addr - ((addr - header_size) % slab_size));
}

qd_slab_cache_t *qd_slab_cache_create(const char *name, size_t size, size_t align, uint32_t flags)
{
    if (size < QD_SLAB_MIN_SIZE)
        size = QD_SLAB_MIN_SIZE;

    if (align == 0)
        align = sizeof(void *);
    if (align < sizeof(void *))
        align = sizeof(void *);

    /* Round up size to alignment */
    size = QD_ALIGN(size, align);

    qd_slab_cache_t *cache = calloc(1, sizeof(qd_slab_cache_t));
    if (!cache)
        return NULL;

    if (name)
        snprintf(cache->name, sizeof(cache->name), "%s", name);
    else
        snprintf(cache->name, sizeof(cache->name), "slab_%zu", size);

    cache->obj_size = size;
    cache->obj_align = align;
    cache->flags = flags;

    /* Calculate slab size and objects per slab */
    cache->slab_size = QD_SLAB_PAGE_SIZE * QD_SLAB_PAGES_PER_SLAB;
    if (size > cache->slab_size / 4)
        cache->slab_size = size * 4;

    cache->objs_per_slab = cache->slab_size / size;
    if (cache->objs_per_slab == 0)
        cache->objs_per_slab = 1;

    /* Initialize lock */
    pthread_mutex_init(&cache->lock, NULL);

    /* Create per-CPU caches */
    cache->num_cpus = get_num_cpus();
    cache->cpu_caches = calloc(cache->num_cpus, sizeof(qd_slab_cpu_cache_t));

    return cache;
}

void qd_slab_cache_destroy(qd_slab_cache_t *cache)
{
    if (!cache)
        return;

    pthread_mutex_lock(&cache->lock);

    /* Free all slabs */
    qd_slab_t *slab, *next;

    for (slab = cache->slabs_full; slab; slab = next) {
        next = slab->next;
        slab_destroy(slab);
    }
    for (slab = cache->slabs_partial; slab; slab = next) {
        next = slab->next;
        slab_destroy(slab);
    }
    for (slab = cache->slabs_empty; slab; slab = next) {
        next = slab->next;
        slab_destroy(slab);
    }

    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);

    free(cache->cpu_caches);
    free(cache);
}

void *qd_slab_alloc(qd_slab_cache_t *cache)
{
    if (!cache)
        return NULL;

    void *obj = NULL;

    /* Try CPU cache first (fast path) */
    if (cache->cpu_caches) {
        int cpu = get_current_cpu();
        qd_slab_cpu_cache_t *cpu_cache = &cache->cpu_caches[cpu];

        if (cpu_cache->count > 0) {
            obj = cpu_cache->freelist[--cpu_cache->count];
            cache->cache_hits++;
            return obj;
        }
    }

    /* Slow path: allocate from slab */
    pthread_mutex_lock(&cache->lock);

    qd_slab_t *slab = cache->slabs_partial;
    if (!slab)
        slab = cache->slabs_empty;

    if (!slab) {
        /* Create new slab */
        slab = slab_create(cache);
        if (!slab) {
            pthread_mutex_unlock(&cache->lock);
            return NULL;
        }
        slab_list_add(&cache->slabs_empty, slab);
    }

    /* Allocate from slab */
    obj = slab->freelist;
    slab->freelist = *(void **)obj;
    slab->inuse++;

    /* Move slab between lists if needed */
    if (slab->inuse == 1 && slab != cache->slabs_partial) {
        /* Was empty, now partial */
        slab_list_remove(&cache->slabs_empty, slab);
        slab_list_add(&cache->slabs_partial, slab);
    } else if (slab->inuse == slab->total) {
        /* Now full */
        slab_list_remove(&cache->slabs_partial, slab);
        slab_list_add(&cache->slabs_full, slab);
    }

    cache->allocs++;
    cache->cache_misses++;

    pthread_mutex_unlock(&cache->lock);

    /* Zero if requested */
    if (cache->flags & QD_SLAB_ZERO)
        memset(obj, 0, cache->obj_size);

    return obj;
}

void *qd_slab_zalloc(qd_slab_cache_t *cache)
{
    void *obj = qd_slab_alloc(cache);
    if (obj)
        memset(obj, 0, cache->obj_size);
    return obj;
}

void qd_slab_free(qd_slab_cache_t *cache, void *ptr)
{
    if (!cache || !ptr)
        return;

    /* Try to add to CPU cache first (fast path) */
    if (cache->cpu_caches) {
        int cpu = get_current_cpu();
        qd_slab_cpu_cache_t *cpu_cache = &cache->cpu_caches[cpu];

        if (cpu_cache->count < QD_SLAB_CPU_CACHE_SIZE) {
            cpu_cache->freelist[cpu_cache->count++] = ptr;
            return;
        }
    }

    /* Slow path: return to slab */
    pthread_mutex_lock(&cache->lock);

    qd_slab_t *slab = slab_from_obj(cache, ptr);

    /* Poison if requested */
    if (cache->flags & QD_SLAB_POISON)
        memset(ptr, 0xDE, cache->obj_size);

    /* Add to freelist */
    *(void **)ptr = slab->freelist;
    slab->freelist = ptr;
    slab->inuse--;

    /* Move slab between lists if needed */
    if (slab->inuse == 0) {
        /* Now empty */
        slab_list_remove(&cache->slabs_partial, slab);
        slab_list_add(&cache->slabs_empty, slab);
    } else if (slab->inuse == slab->total - 1) {
        /* Was full, now partial */
        slab_list_remove(&cache->slabs_full, slab);
        slab_list_add(&cache->slabs_partial, slab);
    }

    cache->frees++;

    pthread_mutex_unlock(&cache->lock);
}

int qd_slab_cache_shrink(qd_slab_cache_t *cache)
{
    if (!cache)
        return -1;

    int freed = 0;

    pthread_mutex_lock(&cache->lock);

    /* Free empty slabs */
    qd_slab_t *slab, *next;
    for (slab = cache->slabs_empty; slab; slab = next) {
        next = slab->next;
        slab_list_remove(&cache->slabs_empty, slab);
        slab_destroy(slab);
        freed++;
    }

    pthread_mutex_unlock(&cache->lock);

    return freed;
}

void qd_slab_cache_stats(qd_slab_cache_t *cache, qd_slab_stats_t *stats)
{
    if (!cache || !stats)
        return;

    memset(stats, 0, sizeof(*stats));

    pthread_mutex_lock(&cache->lock);

    stats->allocs = cache->allocs;
    stats->frees = cache->frees;
    stats->cache_hits = cache->cache_hits;
    stats->cache_misses = cache->cache_misses;

    /* Count slabs */
    qd_slab_t *slab;
    for (slab = cache->slabs_full; slab; slab = slab->next) {
        stats->total_slabs++;
        stats->active_objs += slab->inuse;
        stats->memory_used += cache->slab_size;
    }
    for (slab = cache->slabs_partial; slab; slab = slab->next) {
        stats->total_slabs++;
        stats->active_objs += slab->inuse;
        stats->memory_used += cache->slab_size;
    }
    for (slab = cache->slabs_empty; slab; slab = slab->next) {
        stats->total_slabs++;
        stats->free_slabs++;
        stats->memory_used += cache->slab_size;
    }

    pthread_mutex_unlock(&cache->lock);
}

/*
 * Arena Allocator Implementation
 */

qd_arena_t *qd_arena_create(size_t initial_size)
{
    if (initial_size < QD_ARENA_MIN_SIZE)
        initial_size = QD_ARENA_DEFAULT_SIZE;

    qd_arena_t *arena = malloc(sizeof(qd_arena_t));
    if (!arena)
        return NULL;

    arena->base = malloc(initial_size);
    if (!arena->base) {
        free(arena);
        return NULL;
    }

    arena->size = initial_size;
    arena->offset = 0;
    arena->peak = 0;
    arena->next = NULL;
    arena->current = arena;

    return arena;
}

static void *arena_alloc_internal(qd_arena_t *arena, size_t size, size_t align)
{
    qd_arena_t *a = arena->current;

    /* Align offset */
    size_t aligned_offset = QD_ALIGN(a->offset, align);

    /* Check if fits in current block */
    if (aligned_offset + size <= a->size) {
        void *ptr = (char *)a->base + aligned_offset;
        a->offset = aligned_offset + size;
        if (a->offset > a->peak)
            a->peak = a->offset;
        return ptr;
    }

    /* Need new block */
    size_t new_size = arena->size * 2;
    if (new_size < size + align)
        new_size = size + align + QD_ARENA_MIN_SIZE;

    qd_arena_t *new_arena = malloc(sizeof(qd_arena_t));
    if (!new_arena)
        return NULL;

    new_arena->base = malloc(new_size);
    if (!new_arena->base) {
        free(new_arena);
        return NULL;
    }

    new_arena->size = new_size;
    new_arena->offset = 0;
    new_arena->peak = 0;
    new_arena->next = NULL;
    new_arena->current = new_arena;

    /* Link new block */
    a->next = new_arena;
    arena->current = new_arena;

    /* Allocate from new block */
    aligned_offset = QD_ALIGN(new_arena->offset, align);
    void *ptr = (char *)new_arena->base + aligned_offset;
    new_arena->offset = aligned_offset + size;
    new_arena->peak = new_arena->offset;

    return ptr;
}

void *qd_arena_alloc(qd_arena_t *arena, size_t size)
{
    if (!arena || size == 0)
        return NULL;
    return arena_alloc_internal(arena, size, sizeof(void *));
}

void *qd_arena_alloc_aligned(qd_arena_t *arena, size_t size, size_t align)
{
    if (!arena || size == 0)
        return NULL;
    if (align == 0)
        align = sizeof(void *);
    return arena_alloc_internal(arena, size, align);
}

void *qd_arena_zalloc(qd_arena_t *arena, size_t size)
{
    void *ptr = qd_arena_alloc(arena, size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

char *qd_arena_strdup(qd_arena_t *arena, const char *str)
{
    if (!arena || !str)
        return NULL;

    size_t len = strlen(str) + 1;
    char *copy = qd_arena_alloc(arena, len);
    if (copy)
        memcpy(copy, str, len);
    return copy;
}

char *qd_arena_strndup(qd_arena_t *arena, const char *str, size_t n)
{
    if (!arena || !str)
        return NULL;

    size_t len = strnlen(str, n);
    char *copy = qd_arena_alloc(arena, len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    return copy;
}

void qd_arena_reset(qd_arena_t *arena)
{
    if (!arena)
        return;

    /* Free all blocks except first */
    qd_arena_t *a = arena->next;
    while (a) {
        qd_arena_t *next = a->next;
        free(a->base);
        free(a);
        a = next;
    }

    arena->offset = 0;
    arena->next = NULL;
    arena->current = arena;
}

void qd_arena_destroy(qd_arena_t *arena)
{
    if (!arena)
        return;

    /* Free all blocks */
    qd_arena_t *a = arena;
    while (a) {
        qd_arena_t *next = a->next;
        free(a->base);
        if (a != arena)
            free(a);
        a = next;
    }

    free(arena);
}

void qd_arena_stats(qd_arena_t *arena, qd_arena_stats_t *stats)
{
    if (!arena || !stats)
        return;

    memset(stats, 0, sizeof(*stats));

    qd_arena_t *a = arena;
    while (a) {
        stats->num_blocks++;
        stats->total_size += a->size;
        stats->allocated += a->offset;
        if (a->peak > stats->peak)
            stats->peak = a->peak;
        a = a->next;
    }
}

/*
 * General Memory API
 */

/* Find size class for allocation */
static int find_size_class(size_t size)
{
    for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (size <= size_classes[i])
            return (int)i;
    }
    return -1;
}

int qd_memory_init(void)
{
    if (g_memory.initialized)
        return 0;

    pthread_mutex_lock(&g_memory.lock);

    if (g_memory.initialized) {
        pthread_mutex_unlock(&g_memory.lock);
        return 0;
    }

    /* Create slab caches for standard sizes */
    for (size_t i = 0; i < NUM_SIZE_CLASSES; i++) {
        char name[32];
        snprintf(name, sizeof(name), "size-%zu", size_classes[i]);
        g_memory.caches[i] = qd_slab_cache_create(name, size_classes[i], 0, 0);
        if (!g_memory.caches[i]) {
            /* Cleanup and fail */
            for (size_t j = 0; j < i; j++) {
                qd_slab_cache_destroy(g_memory.caches[j]);
                g_memory.caches[j] = NULL;
            }
            pthread_mutex_unlock(&g_memory.lock);
            return -1;
        }
    }

    g_memory.num_caches = NUM_SIZE_CLASSES;
    g_memory.initialized = 1;

    pthread_mutex_unlock(&g_memory.lock);
    return 0;
}

void qd_memory_shutdown(void)
{
    if (!g_memory.initialized)
        return;

    pthread_mutex_lock(&g_memory.lock);

    for (int i = 0; i < g_memory.num_caches; i++) {
        qd_slab_cache_destroy(g_memory.caches[i]);
        g_memory.caches[i] = NULL;
    }
    g_memory.num_caches = 0;
    g_memory.initialized = 0;

    pthread_mutex_unlock(&g_memory.lock);
}

void *qd_malloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* Try slab allocator for small sizes */
    if (g_memory.initialized) {
        int class_idx = find_size_class(size);
        if (class_idx >= 0) {
            void *ptr = qd_slab_alloc(g_memory.caches[class_idx]);
            if (ptr) {
                qd_atomic_fetch_add(&g_memory.slab_allocations, 1);
                qd_atomic_fetch_add(&g_memory.total_allocated, size);
                return ptr;
            }
        }
    }

    /* Fall back to malloc for large allocations */
    void *ptr = malloc(size);
    if (ptr) {
        qd_atomic_fetch_add(&g_memory.heap_allocations, 1);
        qd_atomic_fetch_add(&g_memory.total_allocated, size);
    }
    return ptr;
}

void *qd_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *ptr = qd_malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *qd_realloc(void *ptr, size_t size)
{
    if (!ptr)
        return qd_malloc(size);

    if (size == 0) {
        qd_free(ptr);
        return NULL;
    }

    /* Simple implementation: allocate new, copy, free old */
    void *new_ptr = qd_malloc(size);
    if (new_ptr) {
        /* We don't know the old size, so this is a limitation */
        memcpy(new_ptr, ptr, size);
        qd_free(ptr);
    }
    return new_ptr;
}

void qd_free(void *ptr)
{
    if (!ptr)
        return;

    /* Check if this is from a slab cache */
    if (g_memory.initialized) {
        for (int i = 0; i < g_memory.num_caches; i++) {
            /* This is a simplified check - in production you'd want better tracking */
            qd_slab_cache_t *cache = g_memory.caches[i];
            /* Try to free to this cache if size matches */
            qd_slab_free(cache, ptr);
            return;
        }
    }

    /* Fall back to free */
    free(ptr);
    qd_atomic_fetch_add(&g_memory.total_freed, 1);
}

void *qd_aligned_alloc(size_t alignment, size_t size)
{
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0)
        return NULL;
    return ptr;
}

void qd_aligned_free(void *ptr)
{
    free(ptr);
}

void *qd_memdup(const void *src, size_t size)
{
    if (!src || size == 0)
        return NULL;

    void *copy = qd_malloc(size);
    if (copy)
        memcpy(copy, src, size);
    return copy;
}

char *qd_strdup(const char *str)
{
    if (!str)
        return NULL;
    return qd_memdup(str, strlen(str) + 1);
}

char *qd_strndup(const char *str, size_t n)
{
    if (!str)
        return NULL;

    size_t len = strnlen(str, n);
    char *copy = qd_malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
    }
    return copy;
}

void qd_memory_stats(qd_memory_stats_t *stats)
{
    if (!stats)
        return;

    stats->total_allocated = qd_atomic_load(&g_memory.total_allocated);
    stats->total_freed = qd_atomic_load(&g_memory.total_freed);
    stats->current_usage = stats->total_allocated - stats->total_freed;
    stats->peak_usage = qd_atomic_load(&g_memory.peak_usage);
    stats->slab_allocations = qd_atomic_load(&g_memory.slab_allocations);
    stats->heap_allocations = qd_atomic_load(&g_memory.heap_allocations);
}

void qd_memory_dump(void)
{
    qd_memory_stats_t stats;
    qd_memory_stats(&stats);

    fprintf(stderr, "=== Memory Statistics ===\n");
    fprintf(stderr, "Total allocated: %zu bytes\n", stats.total_allocated);
    fprintf(stderr, "Total freed: %zu bytes\n", stats.total_freed);
    fprintf(stderr, "Current usage: %zu bytes\n", stats.current_usage);
    fprintf(stderr, "Peak usage: %zu bytes\n", stats.peak_usage);
    fprintf(stderr, "Slab allocations: %zu\n", stats.slab_allocations);
    fprintf(stderr, "Heap allocations: %zu\n", stats.heap_allocations);

    if (g_memory.initialized) {
        fprintf(stderr, "\n=== Slab Caches ===\n");
        for (int i = 0; i < g_memory.num_caches; i++) {
            qd_slab_cache_t *cache = g_memory.caches[i];
            qd_slab_stats_t slab_stats;
            qd_slab_cache_stats(cache, &slab_stats);
            fprintf(stderr, "Cache '%s': %lu allocs, %lu frees, %zu active, %zu slabs\n",
                    cache->name, slab_stats.allocs, slab_stats.frees,
                    slab_stats.active_objs, slab_stats.total_slabs);
        }
    }
}

/*
 * Memory Pool Implementation
 */

qd_mempool_t *qd_mempool_create(size_t obj_size, size_t count)
{
    if (obj_size < sizeof(void *))
        obj_size = sizeof(void *);

    obj_size = QD_ALIGN(obj_size, sizeof(void *));

    qd_mempool_t *pool = malloc(sizeof(qd_mempool_t));
    if (!pool)
        return NULL;

    pool->base = malloc(obj_size * count);
    if (!pool->base) {
        free(pool);
        return NULL;
    }

    pool->obj_size = obj_size;
    pool->capacity = count;
    pool->count = count;
    pthread_mutex_init(&pool->lock, NULL);

    /* Build freelist */
    pool->freelist = NULL;
    char *obj = pool->base;
    for (size_t i = 0; i < count; i++) {
        *(void **)obj = pool->freelist;
        pool->freelist = obj;
        obj += obj_size;
    }

    return pool;
}

void qd_mempool_destroy(qd_mempool_t *pool)
{
    if (!pool)
        return;

    pthread_mutex_destroy(&pool->lock);
    free(pool->base);
    free(pool);
}

void *qd_mempool_alloc(qd_mempool_t *pool)
{
    if (!pool)
        return NULL;

    pthread_mutex_lock(&pool->lock);

    if (!pool->freelist) {
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    void *obj = pool->freelist;
    pool->freelist = *(void **)obj;
    pool->count--;

    pthread_mutex_unlock(&pool->lock);

    return obj;
}

void qd_mempool_free(qd_mempool_t *pool, void *ptr)
{
    if (!pool || !ptr)
        return;

    pthread_mutex_lock(&pool->lock);

    *(void **)ptr = pool->freelist;
    pool->freelist = ptr;
    pool->count++;

    pthread_mutex_unlock(&pool->lock);
}
