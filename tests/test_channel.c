/*
 * QDaemon Channel Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>

#include <qdaemon/qd_channel.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    fflush(stdout); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

TEST(create_destroy)
{
    qd_channel_config_t config = {
        .capacity = 16,
        .item_size = sizeof(int),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);
    assert(chan != NULL);
    assert(qd_channel_capacity(chan) >= 16);
    assert(qd_channel_fd(chan) >= 0);
    qd_channel_destroy(chan);
}

TEST(send_recv)
{
    qd_channel_config_t config = {
        .capacity = 16,
        .item_size = sizeof(int),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);

    int value = 42;
    assert(qd_channel_send(chan, &value) == QD_OK);

    int received;
    assert(qd_channel_recv(chan, &received) == QD_OK);
    assert(received == 42);
    (void)value; (void)received;

    qd_channel_destroy(chan);
}

TEST(multiple_values)
{
    qd_channel_config_t config = {
        .capacity = 16,
        .item_size = sizeof(int),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);

    for (int i = 0; i < 10; i++) {
        assert(qd_channel_send(chan, &i) == QD_OK);
    }

    for (int i = 0; i < 10; i++) {
        int received;
        assert(qd_channel_recv(chan, &received) == QD_OK);
        assert(received == i);
        (void)received;
    }

    qd_channel_destroy(chan);
}

TEST(blocking_send)
{
    qd_channel_config_t config = {
        .capacity = 2,
        .item_size = sizeof(int),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);

    assert(qd_channel_send(chan, &(int){1}) == QD_OK);
    assert(qd_channel_send(chan, &(int){2}) == QD_OK);
    assert(qd_channel_size(chan) == 2);
    assert(qd_channel_try_send(chan, &(int){3}) != QD_OK);

    qd_channel_destroy(chan);
}

TEST(non_blocking_recv)
{
    qd_channel_config_t config = QD_CHANNEL_CONFIG_DEFAULT;
    config.flags = QD_CHAN_NONBLOCK;
    qd_channel_t *chan = qd_channel_create(&config);

    int received;
    assert(qd_channel_try_recv(chan, &received) == QD_ERR_AGAIN);
    (void)received;

    qd_channel_destroy(chan);
}

TEST(pointer_items)
{
    qd_channel_config_t config = {
        .capacity = 16,
        .item_size = sizeof(void*),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);

    char *str = strdup("hello world");
    assert(str != NULL);
    assert(qd_channel_send(chan, &str) == QD_OK);
    (void)str;

    char *received = NULL;
    assert(qd_channel_recv(chan, &received) == QD_OK);
    assert(strcmp(received, "hello world") == 0);
    assert(received == str);
    free(received);

    qd_channel_destroy(chan);
}

TEST(struct_items)
{
    typedef struct {
        int x;
        int y;
        const char *label;
    } point_t;

    qd_channel_config_t config = {
        .capacity = 16,
        .item_size = sizeof(point_t),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);

    assert(qd_channel_send(chan, &(point_t){10, 20, "point1"}) == QD_OK);

    point_t received;
    assert(qd_channel_recv(chan, &received) == QD_OK);
    assert(received.x == 10);
    assert(received.y == 20);
    assert(strcmp(received.label, "point1") == 0);
    (void)received;

    qd_channel_destroy(chan);
}

TEST(eventfd_integration)
{
    qd_channel_config_t config = {
        .capacity = 16,
        .item_size = sizeof(int),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    assert(epfd >= 0);

    assert(epoll_ctl(epfd, EPOLL_CTL_ADD, qd_channel_fd(chan),
                  &(struct epoll_event){.events = EPOLLIN, .data.u32 = 1}) == 0);

    int value = 123;
    assert(qd_channel_send(chan, &value) == QD_OK);
    (void)value;

    struct epoll_event out;
    assert(epoll_wait(epfd, &out, 1, 100) == 1);
    assert(out.data.u32 == 1);

    int received;
    assert(qd_channel_recv(chan, &received) == QD_OK);
    assert(received == 123);
    (void)out; (void)received;

    close(epfd);
    qd_channel_destroy(chan);
}

TEST(fill_and_drain)
{
    qd_channel_config_t config = {
        .capacity = 100,
        .item_size = sizeof(int),
        .flags = 0,
    };
    qd_channel_t *chan = qd_channel_create(&config);

    for (int i = 0; i < 100; i++) {
        assert(qd_channel_send(chan, &i) == QD_OK);
    }
    assert(qd_channel_size(chan) == 100);

    for (int i = 0; i < 100; i++) {
        int received;
        assert(qd_channel_recv(chan, &received) == QD_OK);
        assert(received == i);
        (void)received;
    }
    assert(qd_channel_size(chan) == 0);

    qd_channel_destroy(chan);
}

int main(void)
{
    printf("\n=== Channel Tests ===\n\n");

    RUN_TEST(create_destroy);
    RUN_TEST(send_recv);
    RUN_TEST(multiple_values);
    RUN_TEST(blocking_send);
    RUN_TEST(non_blocking_recv);
    RUN_TEST(pointer_items);
    RUN_TEST(struct_items);
    RUN_TEST(eventfd_integration);
    RUN_TEST(fill_and_drain);

    printf("\nAll channel tests passed!\n\n");
    return 0;
}
