/*
 * QDaemon Async I/O Benchmark
 * Measures throughput and latency of qd_aio using different backends.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <stdint.h>
#include <stdatomic.h>

#include "qdaemon/qd_aio.h"

/* Number of iterations for the benchmark */
#define ITERATIONS 10000000
#define WARMUP     100000
#define BATCH_SIZE 64

static uint64_t g_count = 0;
static uint64_t g_max_count = 0;
static qd_aio_loop_t *g_loop = NULL;
static int g_efd = -1;

/* Get current time in seconds with high precision */
static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Completion callback */
static void on_completion(qd_completion_t *comp, void *arg)
{
    (void)arg;

    if (comp->result < 0) {
        fprintf(stderr, "Async op failed: %s\n", strerror(-comp->result));
        qd_aio_stop(g_loop);
        return;
    }

    if (comp->op == QD_OP_READ) {
        g_count++;

        if (g_count < g_max_count) {
            /* Submit next read */
            uint64_t val = 1;
            /* In a real benchmark we might ping-pong with writes,
               but for raw loop throughput we just re-submit the read
               and assume the fd is ready or we're just testing submission overhead */

            /* To simulate eventfd ping-pong properly:
               1. Read completed (value consumed)
               2. Write to signal (make it ready again)
               3. Submit Read again
            */

            /* For pure overhead test, we'll just read again.
               Since eventfd is semaphore-like or counter, we need to write to it
               to make it readable again if we consumed the value. */

            /* Write to make it readable */
            if (write(g_efd, &val, sizeof(val)) != sizeof(val)) {
                perror("write");
                exit(1);
            }

            /* Submit read request */
            /* We reuse the buffer from the previous completion if we had one,
               but here we just use a static buffer for simplicity */
            static uint64_t read_buf;
            qd_aio_read(g_loop, g_efd, &read_buf, sizeof(read_buf), 0, NULL);
        } else {
            qd_aio_stop(g_loop);
        }
    }
}

static void run_benchmark(const char *name, int count)
{
    g_count = 0;
    g_max_count = count;

    printf("Benchmarking %s (%d iterations)...\n", name, count);

    /* Kickstart: write to eventfd so first read succeeds */
    uint64_t val = 1;
    if (write(g_efd, &val, sizeof(val)) != sizeof(val)) {
        perror("write kickstart");
        exit(1);
    }

    /* Submit initial read */
    static uint64_t read_buf;
    qd_aio_read(g_loop, g_efd, &read_buf, sizeof(read_buf), 0, NULL);

    double start = get_time_sec();

    /* Run the loop */
    qd_aio_run(g_loop);

    double end = get_time_sec();

    double duration = end - start;
    double ops_sec = count / duration;
    double ns_per_op = (duration * 1e9) / count;

    printf("  Duration:   %.4f s\n", duration);
    printf("  Throughput: %.2f M ops/s\n", ops_sec / 1e6);
    printf("  Latency:    %.2f ns/op\n", ns_per_op);
    printf("\n");
}

int main(void)
{
    /* Create eventfd */
    g_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_efd < 0) {
        perror("eventfd");
        return 1;
    }

    /* Configure loop */
    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.queue_depth = 128;
    config.batch_size = BATCH_SIZE;
    config.callback = on_completion;

    /* Create loop */
    g_loop = qd_aio_create_ex(&config);
    if (!g_loop) {
        fprintf(stderr, "Failed to create async loop\n");
        return 1;
    }

    printf("=== QDaemon Async I/O Benchmark ===\n");
    printf("Backend: %s\n\n", qd_aio_backend_name());

    /* Warmup run */
    run_benchmark("Warmup", WARMUP);

    /* Actual benchmark run */
    run_benchmark("Eventfd Ping-Pong", ITERATIONS);

    /* Cleanup */
    qd_aio_destroy(g_loop);
    close(g_efd);

    return 0;
}
