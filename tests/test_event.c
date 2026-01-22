/*
 * QDaemon Event Loop Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/eventfd.h>

#include <qdaemon/qd_event.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

static atomic_int event_count;

/* Event callback */
static void on_event(int fd, uint32_t events, void *arg)
{
    (void)arg;
    
    if (events & QD_EVENT_READ) {
        uint64_t val;
        ssize_t n = read(fd, &val, sizeof(val));
        (void)n;
        atomic_fetch_add(&event_count, 1);
    }
}



/* Test basic creation */
TEST(create_destroy)
{
    qd_event_loop_t *loop = qd_event_loop_create();
    assert(loop != NULL);
    qd_event_loop_destroy(loop);
}

/* Test event registration */
TEST(register_event)
{
    atomic_store(&event_count, 0);
    
    qd_event_loop_t *loop = qd_event_loop_create();
    assert(loop != NULL);
    
    int efd = eventfd(0, EFD_NONBLOCK);
    assert(efd >= 0);
    
    qd_event_source_t *source = qd_event_add(loop, efd, QD_EVENT_READ, on_event, NULL);
    assert(source != NULL);
    
    /* Trigger event */
    uint64_t val = 1;
    ssize_t n = write(efd, &val, sizeof(val));
    (void)n;
    
    /* Process once */
    qd_event_loop_run_once(loop, 100);
    
    assert(atomic_load(&event_count) == 1);
    
    qd_event_del(loop, source);
    close(efd);
    qd_event_loop_destroy(loop);
}

/* Test multiple events */
TEST(multiple_events)
{
    atomic_store(&event_count, 0);
    
    qd_event_loop_t *loop = qd_event_loop_create();
    assert(loop != NULL);
    
    int efds[5];
    qd_event_source_t *sources[5];
    
    for (int i = 0; i < 5; i++) {
        efds[i] = eventfd(0, EFD_NONBLOCK);
        assert(efds[i] >= 0);
        sources[i] = qd_event_add(loop, efds[i], QD_EVENT_READ, on_event, NULL);
        assert(sources[i] != NULL);
    }
    
    /* Trigger all events */
    for (int i = 0; i < 5; i++) {
        uint64_t val = 1;
        ssize_t n = write(efds[i], &val, sizeof(val));
        (void)n;
    }
    
    /* Process events */
    qd_event_loop_run_once(loop, 100);
    
    assert(atomic_load(&event_count) == 5);
    
    for (int i = 0; i < 5; i++) {
        qd_event_del(loop, sources[i]);
        close(efds[i]);
    }
    qd_event_loop_destroy(loop);
}

/* Scheduled timer callback */
static void on_timer_timeout(void *arg)
{
    int *count = arg;
    (*count)++;
}

/* Test timer */
TEST(timer_basic)
{
    qd_event_loop_t *loop = qd_event_loop_create();
    assert(loop != NULL);
    
    int count = 0;
    
    /* Schedule timer - using defer timeout as loop_add_timer generic is not exposed */
    /* Note: original test expected periodic timer? defer_timeout is one-shot. */
    /* We'll just test one-shot for simplicity or re-schedule in callback */
    int ret = qd_event_defer_timeout(loop, on_timer_timeout, &count, 10);
    assert(ret == QD_OK);
    (void)ret;
    
    /* Run */
    qd_event_loop_run_once(loop, 50);
    
    assert(count == 1);
    
    qd_event_loop_destroy(loop);
}

/* Test event modification */
TEST(modify_event)
{
    atomic_store(&event_count, 0);
    
    qd_event_loop_t *loop = qd_event_loop_create();
    assert(loop != NULL);
    
    int efd = eventfd(0, EFD_NONBLOCK);
    assert(efd >= 0);
    
    /* Add with read only */
    qd_event_source_t *src = qd_event_add(loop, efd, QD_EVENT_READ, on_event, NULL);
    assert(src != NULL);
    
    /* Modify to read+write */
    int ret = qd_event_mod(loop, src, QD_EVENT_READ | QD_EVENT_WRITE);
    assert(ret == QD_OK);
    (void)ret;
    
    qd_event_del(loop, src);
    close(efd);
    qd_event_loop_destroy(loop);
}

/* Test stop from callback */
static void stop_callback(int fd, uint32_t events, void *arg)
{
    (void)fd;
    (void)events;
    qd_event_loop_t *loop = arg;
    qd_event_loop_stop(loop);
}

TEST(stop_from_callback)
{
    qd_event_loop_t *loop = qd_event_loop_create();
    assert(loop != NULL);
    
    int efd = eventfd(0, EFD_NONBLOCK);
    assert(efd >= 0);
    
    /* Pass loop as arg since callback signature doesn't include loop directly in used API? */
    /* qd_event_cb_t is (int fd, uint32_t events, void *arg) */
    qd_event_add(loop, efd, QD_EVENT_READ, stop_callback, loop);
    
    /* Trigger event to stop loop */
    uint64_t val = 1;
    ssize_t n = write(efd, &val, sizeof(val));
    (void)n;
    
    /* This should return quickly */
    qd_event_loop_run(loop);
    
    close(efd);
    qd_event_loop_destroy(loop);
}

int main(void)
{
    printf("\n=== Event Loop Tests ===\n\n");
    
    RUN_TEST(create_destroy);
    RUN_TEST(register_event);
    RUN_TEST(multiple_events);
    RUN_TEST(timer_basic);
    RUN_TEST(modify_event);
    RUN_TEST(stop_from_callback);
    
    printf("\nAll event loop tests passed!\n\n");
    return 0;
}
