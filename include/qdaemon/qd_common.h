/*
 * QDaemon - Common Types and Macros
 */

#ifndef QD_COMMON_H
#define QD_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

/* Version information */
#define QD_VERSION_MAJOR 1
#define QD_VERSION_MINOR 0
#define QD_VERSION_PATCH 0
#define QD_VERSION_STRING "1.0.0"

/* Error codes */
#define QD_OK           0
#define QD_ERR         -1
#define QD_ERR_NOMEM   -2
#define QD_ERR_INVAL   -3
#define QD_ERR_BUSY    -4
#define QD_ERR_TIMEOUT -5
#define QD_ERR_IO      -6
#define QD_ERR_NOENT   -7
#define QD_ERR_EXIST   -8
#define QD_ERR_PERM    -9
#define QD_ERR_PROTO   -10
#define QD_ERR_CONN    -11
#define QD_ERR_AGAIN   -12
#define QD_ERR_NOSYS   -13
#define QD_ERR_SYSTEM  -14

/* Utility macros */
#define QD_UNUSED(x) (void)(x)

#define QD_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define QD_MIN(a, b) ((a) < (b) ? (a) : (b))
#define QD_MAX(a, b) ((a) > (b) ? (a) : (b))
#define QD_CLAMP(x, lo, hi) QD_MIN(QD_MAX(x, lo), hi)

#define QD_ALIGN(x, a)      (((x) + ((a) - 1)) & ~((a) - 1))
#define QD_ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define QD_IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

#define QD_ALIGN_PTR(p, a)  ((void *)QD_ALIGN((uintptr_t)(p), (a)))

#define QD_IS_POWER_OF_2(n) ((n) && !((n) & ((n) - 1)))

/* Round up to next power of 2 */
static inline uint32_t qd_next_pow2_32(uint32_t x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

static inline uint64_t qd_next_pow2_64(uint64_t x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

/* Likely/unlikely branch hints */
#define qd_likely(x)   __builtin_expect(!!(x), 1)
#define qd_unlikely(x) __builtin_expect(!!(x), 0)

/* Prefetch hints */
#define qd_prefetch(addr)       __builtin_prefetch(addr)
#define qd_prefetch_write(addr) __builtin_prefetch(addr, 1)

/* Cache line size (common default) */
#ifndef QD_CACHE_LINE_SIZE
#define QD_CACHE_LINE_SIZE 64
#endif

/* Alignment attributes */
#define QD_ALIGNED(n)     __attribute__((aligned(n)))
#define QD_CACHE_ALIGNED  QD_ALIGNED(QD_CACHE_LINE_SIZE)

/* Packing */
#define QD_PACKED __attribute__((packed))

/* Unused function/variable */
#define QD_MAYBE_UNUSED __attribute__((unused))

/* No return */
#define QD_NORETURN __attribute__((noreturn))

/* Printf-like format checking */
#define QD_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))

/* Constructor/destructor */
#define QD_CONSTRUCTOR __attribute__((constructor))
#define QD_DESTRUCTOR  __attribute__((destructor))

/* Thread-local storage */
#define QD_THREAD_LOCAL __thread

/* Container-of macro */
#define qd_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Offsetof for non-constant expressions */
#define qd_offsetof(type, member) ((size_t)&((type *)0)->member)

/* Stringify */
#define QD_STRINGIFY(x)  QD_STRINGIFY2(x)
#define QD_STRINGIFY2(x) #x

/* Concatenate */
#define QD_CONCAT(a, b)  QD_CONCAT2(a, b)
#define QD_CONCAT2(a, b) a##b

/* Unique identifier */
#define QD_UNIQUE_ID(prefix) QD_CONCAT(prefix, __LINE__)

/* Static assertion */
#define QD_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* Build-time assertion (zero-sized array trick for pre-C11) */
#define QD_BUILD_BUG_ON(cond) \
    ((void)sizeof(char[1 - 2 * !!(cond)]))

/* Bit manipulation */
#define QD_BIT(n)           (1UL << (n))
#define QD_BIT_SET(x, n)    ((x) |= QD_BIT(n))
#define QD_BIT_CLEAR(x, n)  ((x) &= ~QD_BIT(n))
#define QD_BIT_TOGGLE(x, n) ((x) ^= QD_BIT(n))
#define QD_BIT_TEST(x, n)   (((x) >> (n)) & 1)

#define QD_MASK(n)          (QD_BIT(n) - 1)

/* Count leading/trailing zeros */
#define qd_clz32(x) __builtin_clz(x)
#define qd_clz64(x) __builtin_clzll(x)
#define qd_ctz32(x) __builtin_ctz(x)
#define qd_ctz64(x) __builtin_ctzll(x)

/* Population count */
#define qd_popcount32(x) __builtin_popcount(x)
#define qd_popcount64(x) __builtin_popcountll(x)

/* Find first set bit (1-indexed, 0 if none) */
#define qd_ffs32(x) __builtin_ffs(x)
#define qd_ffs64(x) __builtin_ffsll(x)

/* Log2 (floor) */
static inline unsigned int qd_log2_32(uint32_t x)
{
    return x ? 31 - qd_clz32(x) : 0;
}

static inline unsigned int qd_log2_64(uint64_t x)
{
    return x ? 63 - qd_clz64(x) : 0;
}

/* Time types */
typedef uint64_t qd_time_t;    /* Absolute time in milliseconds */
typedef int64_t  qd_msec_t;    /* Duration in milliseconds */
typedef int64_t  qd_usec_t;    /* Duration in microseconds */
typedef int64_t  qd_nsec_t;    /* Duration in nanoseconds */

/* Get current time in milliseconds */
#include <time.h>
static inline qd_time_t qd_time_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (qd_time_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline qd_time_t qd_time_now_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (qd_time_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static inline qd_time_t qd_time_now_nsec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (qd_time_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Callback types */
typedef void (*qd_callback_t)(void *arg);
typedef int (*qd_compare_t)(const void *a, const void *b);
typedef uint32_t (*qd_hash_fn_t)(const void *key);
typedef void (*qd_free_fn_t)(void *ptr);

/* Result type for operations that can fail */
typedef struct qd_result {
    int error;
    union {
        void *ptr;
        int64_t i64;
        uint64_t u64;
        double f64;
    } value;
} qd_result_t;

#define QD_RESULT_OK(val) ((qd_result_t){ .error = 0, .value.ptr = (val) })
#define QD_RESULT_ERR(err) ((qd_result_t){ .error = (err), .value.ptr = NULL })
#define QD_RESULT_IS_OK(r) ((r).error == 0)
#define QD_RESULT_IS_ERR(r) ((r).error != 0)

/* Handle type */
typedef int qd_handle_t;
#define QD_INVALID_HANDLE (-1)

/* Flags for various operations */
typedef uint32_t qd_flags_t;

/* Common buffer structure */
typedef struct qd_buffer {
    void *data;
    size_t len;
    size_t cap;
} qd_buffer_t;

static inline void qd_buffer_init(qd_buffer_t *buf)
{
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/* String view (non-owning) */
typedef struct qd_strview {
    const char *data;
    size_t len;
} qd_strview_t;

#define QD_STRVIEW(s) ((qd_strview_t){ .data = (s), .len = sizeof(s) - 1 })
#define QD_STRVIEW_NULL ((qd_strview_t){ .data = NULL, .len = 0 })

static inline qd_strview_t qd_strview_from_cstr(const char *s)
{
    return (qd_strview_t){ .data = s, .len = s ? strlen(s) : 0 };
}

/* Byte order conversions */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define qd_htobe16(x) __builtin_bswap16(x)
#define qd_htobe32(x) __builtin_bswap32(x)
#define qd_htobe64(x) __builtin_bswap64(x)
#define qd_be16toh(x) __builtin_bswap16(x)
#define qd_be32toh(x) __builtin_bswap32(x)
#define qd_be64toh(x) __builtin_bswap64(x)
#define qd_htole16(x) (x)
#define qd_htole32(x) (x)
#define qd_htole64(x) (x)
#define qd_le16toh(x) (x)
#define qd_le32toh(x) (x)
#define qd_le64toh(x) (x)
#else
#define qd_htobe16(x) (x)
#define qd_htobe32(x) (x)
#define qd_htobe64(x) (x)
#define qd_be16toh(x) (x)
#define qd_be32toh(x) (x)
#define qd_be64toh(x) (x)
#define qd_htole16(x) __builtin_bswap16(x)
#define qd_htole32(x) __builtin_bswap32(x)
#define qd_htole64(x) __builtin_bswap64(x)
#define qd_le16toh(x) __builtin_bswap16(x)
#define qd_le32toh(x) __builtin_bswap32(x)
#define qd_le64toh(x) __builtin_bswap64(x)
#endif

#endif /* QD_COMMON_H */
