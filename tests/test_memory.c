/*
 * QDaemon Memory Allocator Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <qdaemon/qd_memory.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

/* Test basic allocation */
TEST(basic_alloc)
{
    void *p = qd_malloc(100);
    assert(p != NULL);
    memset(p, 0xAB, 100);
    qd_free(p);
}

/* Test calloc zeroes memory */
TEST(calloc_zeroes)
{
    unsigned char *p = qd_calloc(100, 1);
    assert(p != NULL);
    for (int i = 0; i < 100; i++) {
        assert(p[i] == 0);
    }
    qd_free(p);
}

/* Test realloc */
TEST(realloc_basic)
{
    char *p = qd_malloc(50);
    assert(p != NULL);
    strcpy(p, "Hello");
    
    p = qd_realloc(p, 100);
    assert(p != NULL);
    assert(strcmp(p, "Hello") == 0);
    
    qd_free(p);
}

/* Test realloc NULL acts like malloc */
TEST(realloc_null)
{
    void *p = qd_realloc(NULL, 50);
    assert(p != NULL);
    qd_free(p);
}

/* Test strdup */
TEST(strdup_basic)
{
    const char *orig = "Test string";
    char *dup = qd_strdup(orig);
    assert(dup != NULL);
    assert(strcmp(dup, orig) == 0);
    assert(dup != orig);
    qd_free(dup);
}

/* Test slab allocator */
/* Test slab allocator */
TEST(slab_basic)
{
    qd_slab_cache_t *cache = qd_slab_cache_create("test_cache", 64, 16, 0);
    assert(cache != NULL);
    
    void *objects[16];
    for (int i = 0; i < 16; i++) {
        objects[i] = qd_slab_alloc(cache);
        assert(objects[i] != NULL);
    }
    
    for (int i = 0; i < 16; i++) {
        qd_slab_free(cache, objects[i]);
    }
    
    qd_slab_cache_destroy(cache);
}

/* Test slab reuse */
TEST(slab_reuse)
{
    qd_slab_cache_t *cache = qd_slab_cache_create("reuse_cache", 32, 8, 0);
    assert(cache != NULL);
    
    void *p1 = qd_slab_alloc(cache);
    qd_slab_free(cache, p1);
    void *p2 = qd_slab_alloc(cache);
    
    /* Freed object should be reused */
    assert(p1 == p2);
    (void)p1; (void)p2;
    
    qd_slab_free(cache, p2);
    qd_slab_cache_destroy(cache);
}

/* Test arena allocator */
TEST(arena_basic)
{
    qd_arena_t *arena = qd_arena_create(4096);
    assert(arena != NULL);
    
    void *p1 = qd_arena_alloc(arena, 100);
    void *p2 = qd_arena_alloc(arena, 200);
    void *p3 = qd_arena_alloc(arena, 300);
    
    assert(p1 != NULL);
    assert(p2 != NULL);
    assert(p3 != NULL);
    
    /* Arena allocations are contiguous */
    assert((char*)p2 > (char*)p1);
    assert((char*)p3 > (char*)p2);
    
    (void)p1; (void)p2; (void)p3;
    
    qd_arena_destroy(arena);
}

/* Test arena reset */
TEST(arena_reset)
{
    qd_arena_t *arena = qd_arena_create(4096);
    assert(arena != NULL);
    
    void *p1 = qd_arena_alloc(arena, 100);
    qd_arena_reset(arena);
    void *p2 = qd_arena_alloc(arena, 100);
    
    /* After reset, should allocate from beginning again */
    assert(p1 == p2);
    (void)p1; (void)p2;
    
    qd_arena_destroy(arena);
}

/* Test large allocation */
TEST(large_alloc)
{
    size_t size = 1024 * 1024; /* 1MB */
    void *p = qd_malloc(size);
    assert(p != NULL);
    memset(p, 0, size);
    qd_free(p);
}

/* Test many small allocations */
TEST(many_small)
{
    void *ptrs[1000];
    for (int i = 0; i < 1000; i++) {
        ptrs[i] = qd_malloc(16);
        assert(ptrs[i] != NULL);
    }
    for (int i = 0; i < 1000; i++) {
        qd_free(ptrs[i]);
    }
}

int main(void)
{
    printf("\n=== Memory Allocator Tests ===\n\n");
    
    RUN_TEST(basic_alloc);
    RUN_TEST(calloc_zeroes);
    RUN_TEST(realloc_basic);
    RUN_TEST(realloc_null);
    RUN_TEST(strdup_basic);
    RUN_TEST(slab_basic);
    RUN_TEST(slab_reuse);
    RUN_TEST(arena_basic);
    RUN_TEST(arena_reset);
    RUN_TEST(large_alloc);
    RUN_TEST(many_small);
    
    printf("\nAll memory tests passed!\n\n");
    return 0;
}
