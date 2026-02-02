/*
 * QDaemon Event Loop Benchmark
 * Measures throughput and latency of the epoll-based event loop using eventfd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <stdint.h>

#include "qdaemon/qd_event.h"

/* Number of iterations for the benchmark */
#define ITERATIONS 10000000
#define WARMUP     100000

static uint64_t g_count = 0;
static uint64_t g_max_count = 0;
static qd_event_loop_t *g_loop = NULL;
static qd_event_source_t *g_source = NULL;
static int g_efd = -1;

/* Get current time in seconds with high precision */
static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Event callback function */
static void on_event(int fd, uint32_t events, void *arg)
{
    (void)events;
    (void)arg;

    /* Read the eventfd to clear the signal */
    uint64_t val;
    if (read(fd, &val, sizeof(val)) != sizeof(val)) {
        perror("read");
        exit(1);
    }

    g_count++;

    if (g_count < g_max_count) {
        /* Signal the eventfd again to trigger the next event */
        val = 1;
        if (write(fd, &val, sizeof(val)) != sizeof(val)) {
            perror("write");
            exit(1);
        }
    } else {
        /* Benchmark complete, stop the loop */
        qd_event_loop_stop(g_loop);
    }
}

static void run_benchmark(const char *name, int count)
{
    g_count = 0;
    g_max_count = count;

    printf("Benchmarking %s (%d iterations)...\n", name, count);

    /* Kickstart the ping-pong */
    uint64_t val = 1;
    if (write(g_efd, &val, sizeof(val)) != sizeof(val)) {
        perror("write kickstart");
        exit(1);
    }

    double start = get_time_sec();

    /* Run the loop - blocks until qd_event_loop_stop() is called */
    qd_event_loop_run(g_loop);

    double end = get_time_sec();

    double duration = end - start;
    double ops_sec = count / duration;
    double ns_per_op = (duration * 1e9) / count;

    printf("  Duration:   %.4f s\n", duration);
    printf("  Throughput: %.2f M events/s\n", ops_sec / 1e6);
    printf("  Latency:    %.2f ns/event\n", ns_per_op);
    printf("\n");
}

int main(void)
{
    /* Create event loop */
    g_loop = qd_event_loop_create();
    if (!g_loop) {
        fprintf(stderr, "Failed to create event loop\n");
        return 1;
    }

    /* Create eventfd for ping-pong */
    g_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (g_efd < 0) {
        perror("eventfd");
        return 1;
    }

    /* Register event source */
    g_source = qd_event_add(g_loop, g_efd, QD_EVENT_READ, on_event, NULL);
    if (!g_source) {
        fprintf(stderr, "Failed to add event source\n");
        return 1;
    }

    printf("=== QDaemon Event Loop Benchmark ===\n");
    printf("Backend: epoll (via qd_event)\n\n");

    /* Warmup run */
    run_benchmark("Warmup", WARMUP);

    /* Actual benchmark run */
    run_benchmark("Eventfd Ping-Pong", ITERATIONS);

    /* Cleanup */
    qd_event_del(g_loop, g_source);
    close(g_efd);
    qd_event_loop_destroy(g_loop);

    return 0;
}
