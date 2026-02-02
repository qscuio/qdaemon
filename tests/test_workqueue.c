/*
 * QDaemon Work Queue Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <qdaemon/qd_workqueue.h>
#include <qdaemon/qd_aio.h>
#include <qdaemon/qd_threadpool.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    fflush(stdout); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

static int completion_count = 0;
static void *last_result = NULL;
static int last_status = 0;

static void on_work_complete(void *result, int status, void *arg)
{
    (void)arg;
    last_result = result;
    last_status = status;
    completion_count++;
}

static void *simple_work(void *arg)
{
    (void)arg;
    return (void*)(uintptr_t)42;
}

TEST(create_destroy)
{
    qd_aio_loop_t *loop = qd_aio_create();
    assert(loop != NULL);

    qd_threadpool_t *pool = qd_threadpool_create(2);
    assert(pool != NULL);

    qd_workqueue_t *wq = qd_workqueue_create(loop, pool,
                                                  on_work_complete, NULL);
    assert(wq != NULL);

    qd_workqueue_destroy(wq);
    qd_threadpool_destroy(pool);
    qd_aio_destroy(loop);
}

TEST(immediate_work)
{
    qd_aio_loop_t *loop = qd_aio_create();
    qd_threadpool_t *pool = qd_threadpool_create(2);
    qd_workqueue_t *wq = qd_workqueue_create(loop, pool,
                                                  on_work_complete, NULL);

    completion_count = 0;
    assert(qd_workqueue_submit(wq, simple_work, NULL) == QD_OK);

    /* Run loop once to process completion */
    qd_aio_run_once(loop, 100);
    assert(completion_count == 1);
    assert(last_status == QD_WORK_OK);
    assert((uintptr_t)last_result == 42);

    qd_workqueue_destroy(wq);
    qd_threadpool_destroy(pool);
    qd_aio_destroy(loop);
}

TEST(pending_count)
{
    qd_aio_loop_t *loop = qd_aio_create();
    qd_threadpool_t *pool = qd_threadpool_create(2);
    qd_workqueue_t *wq = qd_workqueue_create(loop, pool,
                                                  on_work_complete, NULL);

    completion_count = 0;

    qd_workqueue_submit(wq, simple_work, NULL);
    qd_workqueue_submit(wq, simple_work, NULL);
    qd_workqueue_submit(wq, simple_work, NULL);

    assert(qd_workqueue_pending(wq) == 3);

    qd_workqueue_destroy(wq);
    qd_threadpool_destroy(pool);
    qd_aio_destroy(loop);
}

int main(void)
{
    printf("\n=== Work Queue Tests ===\n\n");

    RUN_TEST(create_destroy);
    RUN_TEST(immediate_work);
    RUN_TEST(pending_count);

    printf("\nAll work queue tests passed!\n\n");
    return 0;
}
