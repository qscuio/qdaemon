/*
 * QDaemon - Channel Watch Test
 * Tests qd_aio_channel_watch() and qd_aio_channel_unwatch()
 */

#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

#include "qdaemon/qd_aio.h"
#include "qdaemon/qd_channel.h"
#include "qdaemon/qd_memory.h"
#include "qdaemon/qd_log.h"

/* Test result type */
typedef struct test_result {
    int value;
    char message[32];
} test_result_t;

/* Global test state */
static qd_aio_loop_t *g_loop = NULL;
static qd_channel_t *g_chan = NULL;
static volatile int g_completion_count = 0;

/* Completion callback for channel events */
static void channel_callback(qd_completion_t *comp, void *arg)
{
    (void)arg;

    if (comp->op == QD_OP_CHANNEL) {
        qd_channel_t *chan = comp->user_data;

        /* Drain all available items */
        test_result_t result;
        while (qd_channel_try_recv(chan, &result) == QD_OK) {
            g_completion_count++;
        }

        /* Acknowledge events to clear eventfd */
        qd_channel_ack(chan);
    }
}

/* Worker thread to send to channel */
static void *sender_thread(void *arg)
{
    int count = *(int *)arg;

    for (int i = 0; i < count; i++) {
        test_result_t result = {
            .value = i,
            .message = "test"
        };
        qd_channel_send(g_chan, &result);
        usleep(10000); /* 10ms delay */
    }

    return NULL;
}

int main(void)
{
    qd_log_set_level(QD_LOG_DEBUG);

    printf("=== Channel Watch Tests ===\n\n");

    /* Create async loop */
    qd_aio_config_t aio_config = QD_AIO_CONFIG_DEFAULT;
    aio_config.callback = channel_callback;
    aio_config.callback_arg = NULL;

    g_loop = qd_aio_create_ex(&aio_config);
    assert(g_loop != NULL);

    /* Create channel */
    qd_channel_config_t chan_config = {
        .capacity = 64,
        .item_size = sizeof(test_result_t),
        .flags = QD_CHAN_NONBLOCK
    };
    g_chan = qd_channel_create(&chan_config);
    assert(g_chan != NULL);

    /* Test 1: Watch channel */
    int ret = qd_aio_channel_watch(g_loop, g_chan, g_chan);
    if (ret != QD_OK) {
        printf("  watch_channel                           FAIL (error %d)\n", ret);
        return 1;
    }
    printf("  watch_channel                           PASS\n");

    /* Test 2: Send from background thread and receive in loop */
    int send_count = 5;
    g_completion_count = 0;

    pthread_t sender;
    pthread_create(&sender, NULL, sender_thread, &send_count);

    /* Run loop for a while to process completions */
    for (int i = 0; i < 50 && g_completion_count < send_count; i++) {
        qd_aio_run_once(g_loop, 100);
    }

    pthread_join(sender, NULL);

    if (g_completion_count != send_count) {
        printf("  receive_in_loop                        FAIL (got %d, expected %d)\n",
               g_completion_count, send_count);
        return 1;
    }
    printf("  receive_in_loop                        PASS\n");

    /* Test 3: Unwatch channel */
    ret = qd_aio_channel_unwatch(g_loop, g_chan);
    if (ret != QD_OK) {
        printf("  unwatch_channel                         FAIL (error %d)\n", ret);
        return 1;
    }
    printf("  unwatch_channel                         PASS\n");

    /* Cleanup */
    qd_channel_destroy(g_chan);
    qd_aio_destroy(g_loop);

    printf("\nAll channel watch tests passed!\n");
    return 0;
}
