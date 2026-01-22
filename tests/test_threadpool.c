/*
 * QDaemon Thread Pool Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>

#include <qdaemon/qd_threadpool.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

static atomic_int counter;

/* Simple work function */
static void increment_counter(void *arg)
{
    (void)arg;
    atomic_fetch_add(&counter, 1);
}

/* Work with delay */
static void delayed_work(void *arg)
{
    int ms = (int)(intptr_t)arg;
    usleep(ms * 1000);
    atomic_fetch_add(&counter, 1);
}

/* Test basic creation */
TEST(create_destroy)
{
    qd_threadpool_t *pool = qd_threadpool_create(4);
    assert(pool != NULL);
    qd_threadpool_destroy(pool);
}

/* Test simple work submission */
TEST(simple_work)
{
    atomic_store(&counter, 0);
    
    qd_threadpool_t *pool = qd_threadpool_create(2);
    assert(pool != NULL);
    
    for (int i = 0; i < 10; i++) {
        int ret = qd_threadpool_submit(pool, increment_counter, NULL);
        if (ret != QD_OK) {
            fprintf(stderr, "Submit failed\n");
            exit(1);
        }
    }
    
    qd_threadpool_shutdown(pool, QD_SHUTDOWN_GRACEFUL);
    qd_threadpool_wait(pool, 5000);
    qd_threadpool_destroy(pool);
    
    assert(atomic_load(&counter) == 10);
}

/* Test concurrent execution */
TEST(concurrent_work)
{
    atomic_store(&counter, 0);
    
    qd_threadpool_t *pool = qd_threadpool_create(4);
    assert(pool != NULL);
    
    for (int i = 0; i < 100; i++) {
        qd_threadpool_submit(pool, increment_counter, NULL);
    }
    
    qd_threadpool_shutdown(pool, QD_SHUTDOWN_GRACEFUL);
    qd_threadpool_wait(pool, 5000);
    qd_threadpool_destroy(pool);
    
    assert(atomic_load(&counter) == 100);
}

/* Test delayed work */
TEST(delayed_work_execution)
{
    atomic_store(&counter, 0);
    
    qd_threadpool_t *pool = qd_threadpool_create(2);
    assert(pool != NULL);
    
    for (int i = 0; i < 5; i++) {
        qd_threadpool_submit(pool, delayed_work, (void*)(intptr_t)10);
    }
    
    qd_threadpool_shutdown(pool, QD_SHUTDOWN_GRACEFUL);
    qd_threadpool_wait(pool, 5000);
    qd_threadpool_destroy(pool);
    
    assert(atomic_load(&counter) == 5);
}

/* Test pool stats */
TEST(pool_stats)
{
    qd_threadpool_t *pool = qd_threadpool_create(4);
    assert(pool != NULL);
    
    qd_threadpool_stats_t stats;
    qd_threadpool_stats(pool, &stats);
    assert(stats.num_workers == 4);
    
    qd_threadpool_destroy(pool);
}

/* Test immediate shutdown */
TEST(immediate_shutdown)
{
    atomic_store(&counter, 0);
    
    qd_threadpool_t *pool = qd_threadpool_create(2);
    assert(pool != NULL);
    
    /* Submit slow work */
    for (int i = 0; i < 20; i++) {
        qd_threadpool_submit(pool, delayed_work, (void*)(intptr_t)100);
    }
    
    /* Immediate shutdown - may not complete all */
    qd_threadpool_shutdown(pool, QD_SHUTDOWN_IMMEDIATE);
    qd_threadpool_wait(pool, 1000);
    qd_threadpool_destroy(pool);
    
    /* Some work may not complete */
    int completed = atomic_load(&counter);
    printf("(%d/%d) ", completed, 20);
}

/* Test priority work */
TEST(priority_work)
{
    atomic_store(&counter, 0);
    
    qd_threadpool_t *pool = qd_threadpool_create(1);
    assert(pool != NULL);
    
    /* Pause to queue up work */
    qd_threadpool_pause(pool);
    
    /* Submit normal priority */
    qd_threadpool_submit(pool, increment_counter, NULL);
    qd_threadpool_submit(pool, increment_counter, NULL);
    
    /* Submit high priority */
    qd_threadpool_submit_priority(pool, increment_counter, NULL, QD_PRIORITY_HIGH);
    
    qd_threadpool_resume(pool);
    
    qd_threadpool_shutdown(pool, QD_SHUTDOWN_GRACEFUL);
    qd_threadpool_wait(pool, 5000);
    qd_threadpool_destroy(pool);
    
    assert(atomic_load(&counter) == 3);
}

int main(void)
{
    printf("\n=== Thread Pool Tests ===\n\n");
    
    RUN_TEST(create_destroy);
    RUN_TEST(simple_work);
    RUN_TEST(concurrent_work);
    RUN_TEST(delayed_work_execution);
    RUN_TEST(pool_stats);
    RUN_TEST(immediate_shutdown);
    RUN_TEST(priority_work);
    
    printf("\nAll thread pool tests passed!\n\n");
    return 0;
}
