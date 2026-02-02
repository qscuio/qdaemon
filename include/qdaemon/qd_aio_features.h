/*
 * QDaemon - Async I/O Feature Detection
 * Compile-time feature flags for backend selection
 */

#ifndef QD_AIO_FEATURES_H
#define QD_AIO_FEATURES_H

/* Backend selection - set by Makefile */
#if defined(QD_BACKEND_URING)
    #define QD_AIO_BACKEND_NAME "io_uring"
#elif defined(QD_BACKEND_EPOLL)
    #define QD_AIO_BACKEND_NAME "epoll"
#else
    /* Default to epoll if nothing specified */
    #define QD_BACKEND_EPOLL
    #define QD_AIO_BACKEND_NAME "epoll"
#endif

/* Feature capability flags */
#define QD_FEAT_ASYNC_IO          (1 << 0)  /* Basic async I/O */
#define QD_FEAT_MULTISHOT_ACCEPT  (1 << 1)  /* Multishot accept */
#define QD_FEAT_MULTISHOT_RECV    (1 << 2)  /* Multishot recv */
#define QD_FEAT_ZERO_COPY         (1 << 3)  /* Zero-copy operations */
#define QD_FEAT_SQPOLL            (1 << 4)  /* Kernel-side polling */
#define QD_FEAT_FIXED_BUFFERS     (1 << 5)  /* Registered buffers */

/* io_uring feature detection (compile-time via liburing headers) */
#if defined(QD_BACKEND_URING)
    #include <liburing.h>

    /* Check liburing version for multishot support (>= 2.2) */
    #if defined(IORING_ACCEPT_MULTISHOT)
        #define QD_URING_HAS_MULTISHOT_ACCEPT 1
    #endif

    #if defined(IORING_RECV_MULTISHOT)
        #define QD_URING_HAS_MULTISHOT_RECV 1
    #endif

    #if defined(IORING_SETUP_SQPOLL)
        #define QD_URING_HAS_SQPOLL 1
    #endif
#endif

#endif /* QD_AIO_FEATURES_H */
