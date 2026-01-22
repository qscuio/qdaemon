/*
 * QDaemon - Atomic Operations
 * Portable atomic operations using C11 atomics and compiler builtins
 */

#ifndef QD_ATOMIC_H
#define QD_ATOMIC_H

#include <stdint.h>
#include <stdatomic.h>

/* Memory ordering shortcuts */
#define QD_MEMORY_ORDER_RELAXED memory_order_relaxed
#define QD_MEMORY_ORDER_CONSUME memory_order_consume
#define QD_MEMORY_ORDER_ACQUIRE memory_order_acquire
#define QD_MEMORY_ORDER_RELEASE memory_order_release
#define QD_MEMORY_ORDER_ACQ_REL memory_order_acq_rel
#define QD_MEMORY_ORDER_SEQ_CST memory_order_seq_cst

/* Atomic types */
typedef _Atomic int qd_atomic_int;
typedef _Atomic unsigned int qd_atomic_uint;
typedef _Atomic long qd_atomic_long;
typedef _Atomic unsigned long qd_atomic_ulong;
typedef _Atomic int32_t qd_atomic_int32;
typedef _Atomic uint32_t qd_atomic_uint32;
typedef _Atomic int64_t qd_atomic_int64;
typedef _Atomic uint64_t qd_atomic_uint64;
typedef _Atomic void* qd_atomic_ptr;
typedef _Atomic size_t qd_atomic_size;

/* Initialize atomic variable */
#define qd_atomic_init(obj, val) atomic_init(obj, val)

/* Load operations */
#define qd_atomic_load(obj) atomic_load(obj)
#define qd_atomic_load_explicit(obj, order) atomic_load_explicit(obj, order)

/* Store operations */
#define qd_atomic_store(obj, val) atomic_store(obj, val)
#define qd_atomic_store_explicit(obj, val, order) atomic_store_explicit(obj, val, order)

/* Exchange operations */
#define qd_atomic_exchange(obj, val) atomic_exchange(obj, val)
#define qd_atomic_exchange_explicit(obj, val, order) atomic_exchange_explicit(obj, val, order)

/* Compare-and-swap operations */
#define qd_atomic_compare_exchange_strong(obj, expected, desired) \
    atomic_compare_exchange_strong(obj, expected, desired)
#define qd_atomic_compare_exchange_weak(obj, expected, desired) \
    atomic_compare_exchange_weak(obj, expected, desired)
#define qd_atomic_compare_exchange_strong_explicit(obj, expected, desired, succ, fail) \
    atomic_compare_exchange_strong_explicit(obj, expected, desired, succ, fail)
#define qd_atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail) \
    atomic_compare_exchange_weak_explicit(obj, expected, desired, succ, fail)

/* Fetch-and-modify operations */
#define qd_atomic_fetch_add(obj, arg) atomic_fetch_add(obj, arg)
#define qd_atomic_fetch_sub(obj, arg) atomic_fetch_sub(obj, arg)
#define qd_atomic_fetch_or(obj, arg) atomic_fetch_or(obj, arg)
#define qd_atomic_fetch_xor(obj, arg) atomic_fetch_xor(obj, arg)
#define qd_atomic_fetch_and(obj, arg) atomic_fetch_and(obj, arg)

#define qd_atomic_fetch_add_explicit(obj, arg, order) atomic_fetch_add_explicit(obj, arg, order)
#define qd_atomic_fetch_sub_explicit(obj, arg, order) atomic_fetch_sub_explicit(obj, arg, order)
#define qd_atomic_fetch_or_explicit(obj, arg, order) atomic_fetch_or_explicit(obj, arg, order)
#define qd_atomic_fetch_xor_explicit(obj, arg, order) atomic_fetch_xor_explicit(obj, arg, order)
#define qd_atomic_fetch_and_explicit(obj, arg, order) atomic_fetch_and_explicit(obj, arg, order)

/* Add-and-fetch (returns new value) */
static inline int qd_atomic_add_fetch_int(qd_atomic_int *obj, int arg)
{
    return atomic_fetch_add(obj, arg) + arg;
}

static inline long qd_atomic_add_fetch_long(qd_atomic_long *obj, long arg)
{
    return atomic_fetch_add(obj, arg) + arg;
}

static inline uint64_t qd_atomic_add_fetch_u64(qd_atomic_uint64 *obj, uint64_t arg)
{
    return atomic_fetch_add(obj, arg) + arg;
}

/* Increment/Decrement helpers */
static inline int qd_atomic_inc(qd_atomic_int *obj)
{
    return atomic_fetch_add(obj, 1) + 1;
}

static inline int qd_atomic_dec(qd_atomic_int *obj)
{
    return atomic_fetch_sub(obj, 1) - 1;
}

static inline int qd_atomic_inc_and_test(qd_atomic_int *obj)
{
    return qd_atomic_inc(obj) == 0;
}

static inline int qd_atomic_dec_and_test(qd_atomic_int *obj)
{
    return qd_atomic_dec(obj) == 0;
}

/* Memory barriers */
#define qd_memory_barrier() atomic_thread_fence(memory_order_seq_cst)
#define qd_read_barrier() atomic_thread_fence(memory_order_acquire)
#define qd_write_barrier() atomic_thread_fence(memory_order_release)

/* Compiler barriers */
#define qd_compiler_barrier() __asm__ __volatile__("" ::: "memory")

/* Spin-wait hint for busy loops */
#if defined(__x86_64__) || defined(__i386__)
#define qd_cpu_relax() __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__)
#define qd_cpu_relax() __asm__ __volatile__("yield" ::: "memory")
#else
#define qd_cpu_relax() qd_compiler_barrier()
#endif

/* Spinlock implementation using atomics */
typedef struct qd_spinlock {
    qd_atomic_int locked;
} qd_spinlock_t;

#define QD_SPINLOCK_INIT { 0 }

static inline void qd_spinlock_init(qd_spinlock_t *lock)
{
    qd_atomic_store(&lock->locked, 0);
}

static inline void qd_spin_lock(qd_spinlock_t *lock)
{
    while (1) {
        /* Test-and-test-and-set for better cache behavior */
        while (qd_atomic_load_explicit(&lock->locked, QD_MEMORY_ORDER_RELAXED)) {
            qd_cpu_relax();
        }

        int expected = 0;
        if (qd_atomic_compare_exchange_weak_explicit(&lock->locked, &expected, 1,
                                                      QD_MEMORY_ORDER_ACQUIRE,
                                                      QD_MEMORY_ORDER_RELAXED)) {
            break;
        }
    }
}

static inline int qd_spin_trylock(qd_spinlock_t *lock)
{
    int expected = 0;
    return qd_atomic_compare_exchange_strong_explicit(&lock->locked, &expected, 1,
                                                       QD_MEMORY_ORDER_ACQUIRE,
                                                       QD_MEMORY_ORDER_RELAXED);
}

static inline void qd_spin_unlock(qd_spinlock_t *lock)
{
    qd_atomic_store_explicit(&lock->locked, 0, QD_MEMORY_ORDER_RELEASE);
}

/* Read-write spinlock */
typedef struct qd_rwspinlock {
    qd_atomic_int readers;
    qd_atomic_int writer;
} qd_rwspinlock_t;

#define QD_RWSPINLOCK_INIT { 0, 0 }

static inline void qd_rwspinlock_init(qd_rwspinlock_t *lock)
{
    qd_atomic_store(&lock->readers, 0);
    qd_atomic_store(&lock->writer, 0);
}

static inline void qd_read_lock(qd_rwspinlock_t *lock)
{
    while (1) {
        /* Wait for writer to finish */
        while (qd_atomic_load_explicit(&lock->writer, QD_MEMORY_ORDER_RELAXED)) {
            qd_cpu_relax();
        }

        qd_atomic_fetch_add(&lock->readers, 1);

        /* Check if writer came in */
        if (!qd_atomic_load(&lock->writer)) {
            break;
        }

        /* Writer came in, back off */
        qd_atomic_fetch_sub(&lock->readers, 1);
    }
}

static inline void qd_read_unlock(qd_rwspinlock_t *lock)
{
    qd_atomic_fetch_sub_explicit(&lock->readers, 1, QD_MEMORY_ORDER_RELEASE);
}

static inline void qd_write_lock(qd_rwspinlock_t *lock)
{
    /* Acquire writer lock */
    while (1) {
        int expected = 0;
        if (qd_atomic_compare_exchange_weak_explicit(&lock->writer, &expected, 1,
                                                      QD_MEMORY_ORDER_ACQUIRE,
                                                      QD_MEMORY_ORDER_RELAXED)) {
            break;
        }
        qd_cpu_relax();
    }

    /* Wait for readers to finish */
    while (qd_atomic_load(&lock->readers) > 0) {
        qd_cpu_relax();
    }
}

static inline void qd_write_unlock(qd_rwspinlock_t *lock)
{
    qd_atomic_store_explicit(&lock->writer, 0, QD_MEMORY_ORDER_RELEASE);
}

/* Sequence lock for read-mostly data */
typedef struct qd_seqlock {
    qd_atomic_uint sequence;
} qd_seqlock_t;

#define QD_SEQLOCK_INIT { 0 }

static inline void qd_seqlock_init(qd_seqlock_t *lock)
{
    qd_atomic_store(&lock->sequence, 0);
}

static inline unsigned int qd_read_seqbegin(qd_seqlock_t *lock)
{
    unsigned int seq;
    do {
        seq = qd_atomic_load_explicit(&lock->sequence, QD_MEMORY_ORDER_ACQUIRE);
    } while (seq & 1);
    return seq;
}

static inline int qd_read_seqretry(qd_seqlock_t *lock, unsigned int seq)
{
    qd_read_barrier();
    return seq != qd_atomic_load_explicit(&lock->sequence, QD_MEMORY_ORDER_RELAXED);
}

static inline void qd_write_seqlock(qd_seqlock_t *lock)
{
    qd_atomic_fetch_add_explicit(&lock->sequence, 1, QD_MEMORY_ORDER_ACQUIRE);
    qd_write_barrier();
}

static inline void qd_write_sequnlock(qd_seqlock_t *lock)
{
    qd_write_barrier();
    qd_atomic_fetch_add_explicit(&lock->sequence, 1, QD_MEMORY_ORDER_RELEASE);
}

#endif /* QD_ATOMIC_H */
