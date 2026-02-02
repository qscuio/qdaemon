/*
 * QDaemon Work Queue Benchmark
 * Measures throughput and latency of offloading work to threads.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "qdaemon/qd_workqueue.h"
#include "qdaemon/qd_aio.h"
#include "qdaemon/qd_threadpool.h"

#define DEFAULT_ITERATIONS 1000000
#define DEFAULT_WARMUP 10000
#define DEFAULT_CONCURRENCY 8
#define DEFAULT_WORK_SIZE 1000
#define DEFAULT_IO_FILE_SIZE (16 * 1024 * 1024)
#define MAX_IO_SIZE (64 * 1024)

typedef enum {
    WORK_NOOP = 0,
    WORK_CPU,
    WORK_IO,
} work_mode_t;

static int g_iterations = DEFAULT_ITERATIONS;
static int g_warmup = DEFAULT_WARMUP;
static int g_concurrency = DEFAULT_CONCURRENCY;
static size_t g_work_size = DEFAULT_WORK_SIZE;
static size_t g_io_file_size = DEFAULT_IO_FILE_SIZE;
static const char *g_io_path = "/tmp/qd_bench_io.bin";
static int g_io_fd = -1;
static _Atomic size_t g_io_offset;

static int g_threads[8] = {1, 2, 4};
static int g_num_threads = 3;
static work_mode_t g_mode = WORK_NOOP;

static int g_completed;
static int g_submitted;
static int g_target;
static qd_aio_loop_t *g_loop = NULL;
static qd_threadpool_t *g_pool = NULL;
static qd_workqueue_t *g_wq = NULL;

static qd_work_fn_t g_work_fn = NULL;

/* Get current time in seconds */
static double get_time_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  --iterations N      Total iterations (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  --warmup N          Warmup iterations (default: %d)\n", DEFAULT_WARMUP);
    printf("  --concurrency N     In-flight tasks (default: %d)\n", DEFAULT_CONCURRENCY);
    printf("  --threads LIST      Thread counts (e.g. 1,2,4)\n");
    printf("  --work MODE         Work mode: noop|cpu|io (default: noop)\n");
    printf("  --work-size N       Work size (cpu iters or io bytes, default: %d)\n", DEFAULT_WORK_SIZE);
    printf("  --io-file-size N    IO file size in bytes (default: %zu)\n", (size_t)DEFAULT_IO_FILE_SIZE);
    printf("  --io-path PATH      IO file path (default: %s)\n", g_io_path);
    printf("  --help              Show this help\n");
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!s || *s == '\0' || (end && *end != '\0'))
        return -1;
    if (v <= 0 || v > INT32_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_size(const char *s, size_t *out)
{
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!s || *s == '\0' || (end && *end != '\0'))
        return -1;
    if (v == 0)
        return -1;
    *out = (size_t)v;
    return 0;
}

static int parse_threads(const char *s)
{
    char *tmp = strdup(s);
    if (!tmp)
        return -1;

    int count = 0;
    char *tok = strtok(tmp, ",");
    while (tok && count < (int)(sizeof(g_threads) / sizeof(g_threads[0]))) {
        int v = 0;
        if (parse_int(tok, &v) != 0) {
            free(tmp);
            return -1;
        }
        g_threads[count++] = v;
        tok = strtok(NULL, ",");
    }

    free(tmp);
    if (count == 0)
        return -1;

    g_num_threads = count;
    return 0;
}

/* Trivial work function */
static void *noop_work(void *arg)
{
    return arg;
}

static void *cpu_work(void *arg)
{
    (void)arg;
    static __thread uint64_t state = 0x9e3779b97f4a7c15ULL;
    size_t iters = g_work_size;

    for (size_t i = 0; i < iters; i++) {
        state = state * 2862933555777941757ULL + 3037000493ULL;
        state ^= (state >> 33);
    }

    return (void *)(uintptr_t)state;
}

static void *io_work(void *arg)
{
    (void)arg;
    if (g_io_fd < 0)
        return NULL;

    size_t size = g_work_size;
    if (size > MAX_IO_SIZE)
        size = MAX_IO_SIZE;

    static __thread unsigned char buf[MAX_IO_SIZE];

    size_t off = atomic_fetch_add(&g_io_offset, size);
    if (g_io_file_size > size) {
        off %= (g_io_file_size - size);
    } else {
        off = 0;
    }

    ssize_t ret = pread(g_io_fd, buf, size, (off_t)off);
    (void)ret;

    return buf;
}

/* Completion callback */
static void on_work_complete(void *result, int status, void *arg)
{
    (void)result;
    (void)status;
    (void)arg;

    int c = ++g_completed;

    /* Submit more work if needed to maintain concurrency */
    if (g_submitted < g_target) {
        g_submitted++;
        qd_workqueue_submit(g_wq, g_work_fn, NULL);
    }

    /* Stop loop when all completions are received */
    if (c + 1 >= g_target) {
        qd_aio_stop(g_loop);
    }
}

/* Global loop callback for channel processing */
static void loop_callback(qd_completion_t *comp, void *arg)
{
    (void)arg;
    qd_workqueue_handle_completion(comp);
}

static void run_benchmark(const char *name, int count)
{
    g_completed = 0;
    g_submitted = 0;
    g_target = count;

    printf("Benchmarking %s (%d iterations, %d concurrent)...\n", name, count, g_concurrency);

    double start = get_time_sec();

    /* Initial submission burst */
    int initial = count < g_concurrency ? count : g_concurrency;
    printf("Submitting %d initial tasks...\n", initial);
    for (int i = 0; i < initial; i++) {
        g_submitted++;
        qd_workqueue_submit(g_wq, g_work_fn, NULL);
    }

    printf("Running loop...\n");
    /* Run loop */
    qd_aio_run(g_loop);
    printf("Loop finished.\n");

    double end = get_time_sec();

    double duration = end - start;
    double ops_sec = count / duration;
    double ns_per_op = (duration * 1e9) / count;

    printf("  Duration:   %.4f s\n", duration);
    printf("  Throughput: %.2f M ops/s\n", ops_sec / 1e6);
    printf("  Latency:    %.2f ns/op (avg)\n", ns_per_op);
    printf("\n");
}

static void run_with_threads(int num_threads)
{
    qd_aio_config_t config = QD_AIO_CONFIG_DEFAULT;
    config.callback = loop_callback;

    g_loop = qd_aio_create_ex(&config);
    if (!g_loop) return;

    qd_threadpool_config_t pool_config = QD_THREADPOOL_CONFIG_DEFAULT;
    pool_config.num_threads = num_threads;
    pool_config.enable_stealing = (num_threads > 1);

    g_pool = qd_threadpool_create_ex(&pool_config);
    if (!g_pool) {
        qd_aio_destroy(g_loop);
        return;
    }

    g_wq = qd_workqueue_create(g_loop, g_pool, on_work_complete, NULL);
    if (!g_wq) {
        qd_threadpool_destroy(g_pool);
        qd_aio_destroy(g_loop);
        return;
    }

    printf("--- %d thread(s) ---\n", num_threads);
    run_benchmark("Warmup", g_warmup);
    run_benchmark("Offload", g_iterations);

    qd_workqueue_destroy(g_wq);
    qd_threadpool_destroy(g_pool);
    qd_aio_destroy(g_loop);
}

static int prepare_io_file(void)
{
    int fd = open(g_io_path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "Failed to create IO file: %s\n", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, (off_t)g_io_file_size) != 0) {
        fprintf(stderr, "Failed to size IO file: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);

    g_io_fd = open(g_io_path, O_RDONLY | O_CLOEXEC);
    if (g_io_fd < 0) {
        fprintf(stderr, "Failed to open IO file: %s\n", strerror(errno));
        return -1;
    }

    atomic_store(&g_io_offset, 0);
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &g_iterations) != 0)
                return 1;
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &g_warmup) != 0)
                return 1;
        } else if (strcmp(argv[i], "--concurrency") == 0 && i + 1 < argc) {
            if (parse_int(argv[++i], &g_concurrency) != 0)
                return 1;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            if (parse_threads(argv[++i]) != 0)
                return 1;
        } else if (strcmp(argv[i], "--work") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "noop") == 0) {
                g_mode = WORK_NOOP;
            } else if (strcmp(mode, "cpu") == 0) {
                g_mode = WORK_CPU;
            } else if (strcmp(mode, "io") == 0) {
                g_mode = WORK_IO;
            } else {
                return 1;
            }
        } else if (strcmp(argv[i], "--work-size") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &g_work_size) != 0)
                return 1;
        } else if (strcmp(argv[i], "--io-file-size") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &g_io_file_size) != 0)
                return 1;
        } else if (strcmp(argv[i], "--io-path") == 0 && i + 1 < argc) {
            g_io_path = argv[++i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    switch (g_mode) {
    case WORK_CPU:
        g_work_fn = cpu_work;
        break;
    case WORK_IO:
        g_work_fn = io_work;
        break;
    case WORK_NOOP:
    default:
        g_work_fn = noop_work;
        break;
    }

    if (g_mode == WORK_IO) {
        if (g_work_size > MAX_IO_SIZE) {
            fprintf(stderr, "work-size too large, clamping to %d\n", MAX_IO_SIZE);
            g_work_size = MAX_IO_SIZE;
        }
        if (g_io_file_size < g_work_size)
            g_io_file_size = g_work_size;

        if (prepare_io_file() != 0)
            return 1;
    }

    printf("=== QDaemon Work Queue Benchmark ===\n");
    printf("Backend: %s\n", qd_aio_backend_name());
    printf("Mode: %s, work-size=%zu, concurrency=%d\n\n",
           g_mode == WORK_CPU ? "cpu" : (g_mode == WORK_IO ? "io" : "noop"),
           g_work_size, g_concurrency);

    for (int i = 0; i < g_num_threads; i++) {
        run_with_threads(g_threads[i]);
    }

    if (g_io_fd >= 0) {
        close(g_io_fd);
        g_io_fd = -1;
    }

    return 0;
}
