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
static qd_event_loop_t *test_loop;

/* Event callback */
static void on_event(qd_event_loop_t *loop, int fd, uint32_t events, void *arg)
{
    (void)loop;
    (void)arg;
    
    if (events & QD_EVENT_READ) {
        uint64_t val;
        read(fd, &val, sizeof(val));
        atomic_fetch_add(&event_count, 1);
    }
}

/* Timer callback */
static void on_timer(qd_event_loop_t *loop, void *arg)
{
    (void)loop;
    int *count = arg;
    (*count)++;
    
    if (*count >= 3) {
        qd_event_loop_stop(loop);
    }
}

/* Test basic creation */
TEST(create_destroy)
{
    qd_event_loop_t *loop = qd_event_loop_create(100);
    assert(loop != NULL);
    qd_event_loop_destroy(loop);
}

/* Test event registration */
TEST(register_event)
{
    atomic_store(&event_count, 0);
    
    qd_event_loop_t *loop = qd_event_loop_create(10);
    assert(loop != NULL);
    
    int efd = eventfd(0, EFD_NONBLOCK);
    assert(efd >= 0);
    
    int ret = qd_event_loop_add(loop, efd, QD_EVENT_READ, on_event, NULL);
    assert(ret == QD_OK);
    
    /* Trigger event */
    uint64_t val = 1;
    write(efd, &val, sizeof(val));
    
    /* Process once */
    qd_event_loop_run_once(loop, 100);
    
    assert(atomic_load(&event_count) == 1);
    
    qd_event_loop_remove(loop, efd);
    close(efd);
    qd_event_loop_destroy(loop);
}

/* Test multiple events */
TEST(multiple_events)
{
    atomic_store(&event_count, 0);
    
    qd_event_loop_t *loop = qd_event_loop_create(10);
    assert(loop != NULL);
    
    int efds[5];
    for (int i = 0; i < 5; i++) {
        efds[i] = eventfd(0, EFD_NONBLOCK);
        assert(efds[i] >= 0);
        qd_event_loop_add(loop, efds[i], QD_EVENT_READ, on_event, NULL);
    }
    
    /* Trigger all events */
    for (int i = 0; i < 5; i++) {
        uint64_t val = 1;
        write(efds[i], &val, sizeof(val));
    }
    
    /* Process events */
    qd_event_loop_run_once(loop, 100);
    
    assert(atomic_load(&event_count) == 5);
    
    for (int i = 0; i < 5; i++) {
        qd_event_loop_remove(loop, efds[i]);
        close(efds[i]);
    }
    qd_event_loop_destroy(loop);
}

/* Test timer */
TEST(timer_basic)
{
    qd_event_loop_t *loop = qd_event_loop_create(10);
    assert(loop != NULL);
    
    int count = 0;
    
    qd_timer_handle_t timer = qd_event_loop_add_timer(loop, 10, 10, on_timer, &count);
    assert(timer != QD_INVALID_TIMER);
    
    /* Run until timer stops us */
    qd_event_loop_run(loop);
    
    assert(count >= 3);
    
    qd_event_loop_destroy(loop);
}

/* Test event modification */
TEST(modify_event)
{
    atomic_store(&event_count, 0);
    
    qd_event_loop_t *loop = qd_event_loop_create(10);
    assert(loop != NULL);
    
    int efd = eventfd(0, EFD_NONBLOCK);
    assert(efd >= 0);
    
    /* Add with read only */
    qd_event_loop_add(loop, efd, QD_EVENT_READ, on_event, NULL);
    
    /* Modify to read+write */
    int ret = qd_event_loop_modify(loop, efd, QD_EVENT_READ | QD_EVENT_WRITE);
    assert(ret == QD_OK);
    
    qd_event_loop_remove(loop, efd);
    close(efd);
    qd_event_loop_destroy(loop);
}

/* Test stop from callback */
static void stop_callback(qd_event_loop_t *loop, int fd, uint32_t events, void *arg)
{
    (void)fd;
    (void)events;
    (void)arg;
    qd_event_loop_stop(loop);
}

TEST(stop_from_callback)
{
    qd_event_loop_t *loop = qd_event_loop_create(10);
    assert(loop != NULL);
    
    int efd = eventfd(0, EFD_NONBLOCK);
    assert(efd >= 0);
    
    qd_event_loop_add(loop, efd, QD_EVENT_READ, stop_callback, NULL);
    
    /* Trigger event to stop loop */
    uint64_t val = 1;
    write(efd, &val, sizeof(val));
    
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
