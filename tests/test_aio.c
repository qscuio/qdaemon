/*
 * QDaemon Async I/O Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdatomic.h>

#include <qdaemon/qd_aio.h>

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    fflush(stdout); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

static atomic_int completion_count;
static int last_result;
static qd_op_type_t last_op;

static void test_callback(qd_completion_t *comp, void *arg)
{
    (void)arg;
    atomic_fetch_add(&completion_count, 1);
    last_result = comp->result;
    last_op = comp->op;
}

/* Test basic creation */
TEST(create_destroy)
{
    qd_aio_loop_t *loop = qd_aio_create();
    assert(loop != NULL);
    qd_aio_destroy(loop);
}

/* Test creation with config */
TEST(create_with_config)
{
    qd_aio_config_t config = {
        .queue_depth = 128,
        .batch_size = 32,
        .callback = test_callback,
        .callback_arg = NULL,
    };

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);
    qd_aio_destroy(loop);
}

/* Test backend name */
TEST(backend_info)
{
    const char *name = qd_aio_backend_name();
    assert(name != NULL);
    assert(strlen(name) > 0);

    assert(qd_aio_backend_features() & QD_FEAT_ASYNC_IO);

    printf("(%s) ", name);
}

/* Test pipe read/write */
TEST(pipe_read_write)
{
    atomic_store(&completion_count, 0);

    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = test_callback;

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        abort();
    }

    /* Set non-blocking */
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    /* Write some data */
    const char *msg = "hello async";
    if (write(pipefd[1], msg, strlen(msg)) < 0) {
        perror("write");
        abort();
    }

    /* Submit async read */
    char buf[64] = {0};
    if (qd_aio_read(loop, pipefd[0], buf, sizeof(buf), 0, NULL) != QD_OK) {
        fprintf(stderr, "qd_aio_read failed\n");
        abort();
    }

    /* Process */
    int ret = qd_aio_run_once(loop, 100);
    if (ret < 0) {
        fprintf(stderr, "qd_aio_run_once failed: %d\n", ret);
        abort();
    }
    assert(atomic_load(&completion_count) == 1);
    assert(last_result > 0);
    assert(strncmp(buf, "hello async", 11) == 0);

    close(pipefd[0]);
    close(pipefd[1]);
    qd_aio_destroy(loop);
}

/* Test multiple operations */
TEST(multiple_ops)
{
    atomic_store(&completion_count, 0);

    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = test_callback;

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);

    int pipes[3][2];
    char bufs[3][64] = {{0}};

    for (int i = 0; i < 3; i++) {
        if (pipe(pipes[i]) != 0) {
            perror("pipe");
            abort();
        }
        fcntl(pipes[i][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[i][1], F_SETFL, O_NONBLOCK);

        /* Write data */
        char msg[32];
        snprintf(msg, sizeof(msg), "pipe%d", i);
        if (write(pipes[i][1], msg, strlen(msg)) < 0) {
            perror("write");
            abort();
        }

        /* Submit read */
        qd_aio_read(loop, pipes[i][0], bufs[i], sizeof(bufs[i]), 0, NULL);
    }

    /* Process all */
    int total = 0;
    for (int i = 0; i < 10 && total < 3; i++) {
        int ret = qd_aio_run_once(loop, 100);
        if (ret > 0)
            total += ret;
    }

    assert(atomic_load(&completion_count) == 3);

    for (int i = 0; i < 3; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    qd_aio_destroy(loop);
}

/* Test timeout operation */
TEST(timeout)
{
    atomic_store(&completion_count, 0);

    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = test_callback;

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);

    assert(qd_aio_timeout(loop, 10, NULL) == QD_OK);

    for (int i = 0; i < 10 && atomic_load(&completion_count) == 0; i++) {
        qd_aio_run_once(loop, 50);
    }

    assert(atomic_load(&completion_count) == 1);
    assert(last_op == QD_OP_TIMEOUT);

    qd_aio_destroy(loop);
}

/* Test statistics */
TEST(statistics)
{
    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = test_callback;

    qd_aio_loop_t *loop = qd_aio_create_ex(&config);
    assert(loop != NULL);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        abort();
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    if (write(pipefd[1], "x", 1) != 1) {
        perror("write");
        abort();
    }

    char buf[8];
    qd_aio_read(loop, pipefd[0], buf, sizeof(buf), 0, NULL);

    qd_aio_stats_t stats;
    qd_aio_stats(loop, &stats);
    assert(stats.submit_count == 1);
    assert(stats.pending >= 0);

    qd_aio_run_once(loop, 100);

    qd_aio_stats(loop, &stats);
    assert(stats.complete_count >= 1);

    close(pipefd[0]);
    close(pipefd[1]);
    qd_aio_destroy(loop);
}

/* Test stop */
TEST(run_stop)
{
    qd_aio_loop_t *loop = qd_aio_create();
    assert(loop != NULL);

    assert(qd_aio_is_running(loop) == 0);

    /* Stop before running should be safe */
    qd_aio_stop(loop);

    qd_aio_destroy(loop);
}

int main(void)
{
    printf("\n=== Async I/O Tests ===\n\n");
    printf("Backend: %s\n\n", qd_aio_backend_name());

    RUN_TEST(create_destroy);
    RUN_TEST(create_with_config);
    RUN_TEST(backend_info);
    RUN_TEST(pipe_read_write);
    RUN_TEST(multiple_ops);
    RUN_TEST(timeout);
    RUN_TEST(statistics);
    RUN_TEST(run_stop);

    printf("\nAll async I/O tests passed!\n\n");
    return 0;
}
