/*
 * QDaemon - Event Loop Implementation
 * High-performance epoll-based event loop
 */

#ifndef _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <pthread.h>

#include "qdaemon/qd_event.h"
#include "qdaemon/qd_timer.h"
#include "qdaemon/qd_memory.h"
#include "../util/qd_atomic.h"

/* Thread-local current event loop */
static __thread qd_event_loop_t *tls_current_loop = NULL;

/* Convert our event flags to epoll flags */
static uint32_t to_epoll_events(uint32_t events)
{
    uint32_t epoll_events = 0;

    if (events & QD_EVENT_READ)
        epoll_events |= EPOLLIN;
    if (events & QD_EVENT_WRITE)
        epoll_events |= EPOLLOUT;
    if (events & QD_EVENT_ERROR)
        epoll_events |= EPOLLERR;
    if (events & QD_EVENT_HANGUP)
        epoll_events |= EPOLLHUP;
    if (events & QD_EVENT_RDHUP)
        epoll_events |= EPOLLRDHUP;
    if (events & QD_EVENT_EDGE)
        epoll_events |= EPOLLET;
    if (events & QD_EVENT_ONESHOT)
        epoll_events |= EPOLLONESHOT;

    return epoll_events;
}

/* Convert epoll flags to our event flags */
static uint32_t from_epoll_events(uint32_t epoll_events)
{
    uint32_t events = 0;

    if (epoll_events & EPOLLIN)
        events |= QD_EVENT_READ;
    if (epoll_events & EPOLLOUT)
        events |= QD_EVENT_WRITE;
    if (epoll_events & EPOLLERR)
        events |= QD_EVENT_ERROR;
    if (epoll_events & EPOLLHUP)
        events |= QD_EVENT_HANGUP;
    if (epoll_events & EPOLLRDHUP)
        events |= QD_EVENT_RDHUP;

    return events;
}

int qd_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int qd_set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

int qd_socketpair(int fds[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) == -1)
        return -1;
    return 0;
}

/* Handle wakeup event */
static void wakeup_callback(int fd, uint32_t events, void *arg)
{
    QD_UNUSED(events);
    QD_UNUSED(arg);

    uint64_t val;
    ssize_t n = read(fd, &val, sizeof(val));
    QD_UNUSED(n);
}

/* Handle signal event */
static void signal_callback(int fd, uint32_t events, void *arg)
{
    QD_UNUSED(events);
    qd_event_loop_t *loop = arg;

    struct signalfd_siginfo siginfo;
    ssize_t n = read(fd, &siginfo, sizeof(siginfo));
    if (n != sizeof(siginfo))
        return;

    int signo = siginfo.ssi_signo;

    /* Find and call handler */
    qd_signal_handler_t *handler = loop->signal_handlers;
    while (handler) {
        if (handler->signo == signo && handler->callback) {
            handler->callback(signo, handler->arg);
        }
        handler = handler->next;
    }
}

/* Handle timer event */
static void timer_callback(int fd, uint32_t events, void *arg)
{
    QD_UNUSED(events);
    qd_event_loop_t *loop = arg;

    uint64_t expirations;
    ssize_t n = read(fd, &expirations, sizeof(expirations));
    QD_UNUSED(n);

    if (loop->timer_wheel) {
        qd_timer_wheel_tick(loop->timer_wheel);
    }
}

qd_event_loop_t *qd_event_loop_create(void)
{
    qd_event_loop_config_t config = QD_EVENT_LOOP_CONFIG_DEFAULT;
    return qd_event_loop_create_ex(&config);
}

qd_event_loop_t *qd_event_loop_create_ex(const qd_event_loop_config_t *config)
{
    if (!config)
        return NULL;

    qd_event_loop_t *loop = calloc(1, sizeof(qd_event_loop_t));
    if (!loop)
        return NULL;

    /* Create epoll instance */
    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd == -1) {
        free(loop);
        return NULL;
    }

    /* Initialize state */
    qd_atomic_init(&loop->state, QD_LOOP_STOPPED);
    qd_atomic_init(&loop->iteration, 0);

    /* Configuration */
    loop->max_events = config->max_events > 0 ? config->max_events : 1024;
    loop->default_timeout = config->timeout_ms;

    if (config->name)
        snprintf(loop->name, sizeof(loop->name), "%s", config->name);
    else
        snprintf(loop->name, sizeof(loop->name), "event_loop");

    /* Allocate epoll events array */
    loop->epoll_events = calloc(loop->max_events, sizeof(struct epoll_event));
    if (!loop->epoll_events) {
        close(loop->epoll_fd);
        free(loop);
        return NULL;
    }

    /* Create wakeup eventfd */
    loop->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wakeup_fd == -1) {
        free(loop->epoll_events);
        close(loop->epoll_fd);
        free(loop);
        return NULL;
    }

    /* Initialize lock and pending work queue */
    pthread_mutex_init(&loop->lock, NULL);
    pthread_mutex_init(&loop->pending_lock, NULL);

    /* Create timer wheel */
    loop->timer_wheel = qd_timer_wheel_create(1);  /* 1ms tick */

    /* Create timerfd for timer wheel */
    loop->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (loop->timer_fd != -1) {
        /* Set up 1ms periodic timer */
        struct itimerspec its = {
            .it_interval = { .tv_sec = 0, .tv_nsec = 1000000 },  /* 1ms */
            .it_value = { .tv_sec = 0, .tv_nsec = 1000000 }
        };
        timerfd_settime(loop->timer_fd, 0, &its, NULL);
    }

    /* Set up signal handling if enabled */
    if (config->enable_signals) {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGHUP);
        sigaddset(&mask, SIGUSR1);
        sigaddset(&mask, SIGUSR2);

        /* Block signals so they're delivered to signalfd */
        pthread_sigmask(SIG_BLOCK, &mask, NULL);

        loop->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    } else {
        loop->signal_fd = -1;
    }

    /* Add internal fds to epoll */
    qd_event_add(loop, loop->wakeup_fd, QD_EVENT_READ, wakeup_callback, loop);

    if (loop->timer_fd != -1) {
        qd_event_add(loop, loop->timer_fd, QD_EVENT_READ, timer_callback, loop);
    }

    if (loop->signal_fd != -1) {
        qd_event_add(loop, loop->signal_fd, QD_EVENT_READ, signal_callback, loop);
    }

    loop->owner_thread = pthread_self();
    loop->thread_bound = 1;

    return loop;
}

void qd_event_loop_destroy(qd_event_loop_t *loop)
{
    if (!loop)
        return;

    /* Stop if running */
    qd_event_loop_stop(loop);

    /* Free event sources */
    qd_event_source_t *source = loop->sources;
    while (source) {
        qd_event_source_t *next = source->next;
        free(source);
        source = next;
    }

    /* Free signal handlers */
    qd_signal_handler_t *handler = loop->signal_handlers;
    while (handler) {
        qd_signal_handler_t *next = handler->next;
        free(handler);
        handler = next;
    }

    /* Free pending work */
    qd_pending_work_t *work = loop->pending_head;
    while (work) {
        qd_pending_work_t *next = work->next;
        free(work);
        work = next;
    }

    /* Destroy timer wheel */
    if (loop->timer_wheel)
        qd_timer_wheel_destroy(loop->timer_wheel);

    /* Close file descriptors */
    if (loop->wakeup_fd != -1)
        close(loop->wakeup_fd);
    if (loop->timer_fd != -1)
        close(loop->timer_fd);
    if (loop->signal_fd != -1)
        close(loop->signal_fd);
    if (loop->epoll_fd != -1)
        close(loop->epoll_fd);

    pthread_mutex_destroy(&loop->lock);
    pthread_mutex_destroy(&loop->pending_lock);

    free(loop->epoll_events);
    free(loop);
}

qd_event_source_t *qd_event_add(qd_event_loop_t *loop, int fd, uint32_t events,
                                 qd_event_cb_t callback, void *arg)
{
    if (!loop || fd < 0)
        return NULL;

    qd_event_source_t *source = calloc(1, sizeof(qd_event_source_t));
    if (!source)
        return NULL;

    source->fd = fd;
    source->type = QD_SOURCE_FD;
    source->events = events;
    source->callback = callback;
    source->arg = arg;
    source->loop = loop;
    source->enabled = 1;

    /* Add to epoll */
    struct epoll_event ev = {
        .events = to_epoll_events(events),
        .data.ptr = source
    };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        free(source);
        return NULL;
    }

    source->registered = 1;

    /* Add to source list */
    pthread_mutex_lock(&loop->lock);
    source->next = loop->sources;
    if (loop->sources)
        loop->sources->prev = source;
    loop->sources = source;
    loop->num_sources++;
    pthread_mutex_unlock(&loop->lock);

    return source;
}

int qd_event_mod(qd_event_loop_t *loop, qd_event_source_t *source, uint32_t events)
{
    if (!loop || !source)
        return QD_ERR_INVAL;

    source->events = events;

    struct epoll_event ev = {
        .events = to_epoll_events(events),
        .data.ptr = source
    };

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, source->fd, &ev) == -1)
        return QD_ERR_IO;

    return QD_OK;
}

int qd_event_del(qd_event_loop_t *loop, qd_event_source_t *source)
{
    if (!loop || !source)
        return QD_ERR_INVAL;

    /* Remove from epoll */
    if (source->registered) {
        epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, source->fd, NULL);
        source->registered = 0;
    }

    /* Remove from source list */
    pthread_mutex_lock(&loop->lock);
    if (source->prev)
        source->prev->next = source->next;
    else
        loop->sources = source->next;
    if (source->next)
        source->next->prev = source->prev;
    loop->num_sources--;
    pthread_mutex_unlock(&loop->lock);

    free(source);
    return QD_OK;
}

int qd_event_enable(qd_event_source_t *source)
{
    if (!source || !source->loop)
        return QD_ERR_INVAL;

    if (source->enabled)
        return QD_OK;

    source->enabled = 1;

    struct epoll_event ev = {
        .events = to_epoll_events(source->events),
        .data.ptr = source
    };

    if (epoll_ctl(source->loop->epoll_fd, EPOLL_CTL_MOD, source->fd, &ev) == -1)
        return QD_ERR_IO;

    return QD_OK;
}

int qd_event_disable(qd_event_source_t *source)
{
    if (!source || !source->loop)
        return QD_ERR_INVAL;

    if (!source->enabled)
        return QD_OK;

    source->enabled = 0;

    /* Set empty events to effectively disable */
    struct epoll_event ev = {
        .events = 0,
        .data.ptr = source
    };

    if (epoll_ctl(source->loop->epoll_fd, EPOLL_CTL_MOD, source->fd, &ev) == -1)
        return QD_ERR_IO;

    return QD_OK;
}

int qd_event_del_fd(qd_event_loop_t *loop, int fd)
{
    if (!loop || fd < 0)
        return QD_ERR_INVAL;

    pthread_mutex_lock(&loop->lock);
    qd_event_source_t *source = loop->sources;
    while (source) {
        if (source->fd == fd) {
            pthread_mutex_unlock(&loop->lock);
            return qd_event_del(loop, source);
        }
        source = source->next;
    }
    pthread_mutex_unlock(&loop->lock);

    return QD_ERR_NOENT;
}

int qd_event_add_signal(qd_event_loop_t *loop, int signo, qd_signal_cb_t callback, void *arg)
{
    if (!loop || !callback)
        return QD_ERR_INVAL;

    qd_signal_handler_t *handler = calloc(1, sizeof(qd_signal_handler_t));
    if (!handler)
        return QD_ERR_NOMEM;

    handler->signo = signo;
    handler->callback = callback;
    handler->arg = arg;

    pthread_mutex_lock(&loop->lock);
    handler->next = loop->signal_handlers;
    loop->signal_handlers = handler;
    pthread_mutex_unlock(&loop->lock);

    return QD_OK;
}

int qd_event_del_signal(qd_event_loop_t *loop, int signo)
{
    if (!loop)
        return QD_ERR_INVAL;

    pthread_mutex_lock(&loop->lock);
    qd_signal_handler_t **pp = &loop->signal_handlers;
    while (*pp) {
        if ((*pp)->signo == signo) {
            qd_signal_handler_t *handler = *pp;
            *pp = handler->next;
            free(handler);
            pthread_mutex_unlock(&loop->lock);
            return QD_OK;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&loop->lock);

    return QD_ERR_NOENT;
}

int qd_event_defer(qd_event_loop_t *loop, void (*func)(void *arg), void *arg)
{
    if (!loop || !func)
        return QD_ERR_INVAL;

    qd_pending_work_t *work = malloc(sizeof(qd_pending_work_t));
    if (!work)
        return QD_ERR_NOMEM;

    work->func = func;
    work->arg = arg;
    work->next = NULL;

    pthread_mutex_lock(&loop->pending_lock);
    if (loop->pending_tail) {
        loop->pending_tail->next = work;
        loop->pending_tail = work;
    } else {
        loop->pending_head = work;
        loop->pending_tail = work;
    }
    pthread_mutex_unlock(&loop->pending_lock);

    qd_event_loop_wakeup(loop);

    return QD_OK;
}

/* Process pending work */
static void process_pending_work(qd_event_loop_t *loop)
{
    qd_pending_work_t *work;

    pthread_mutex_lock(&loop->pending_lock);
    work = loop->pending_head;
    loop->pending_head = NULL;
    loop->pending_tail = NULL;
    pthread_mutex_unlock(&loop->pending_lock);

    while (work) {
        qd_pending_work_t *next = work->next;
        if (work->func)
            work->func(work->arg);
        free(work);
        work = next;
    }
}

int qd_event_loop_run_once(qd_event_loop_t *loop, int timeout_ms)
{
    if (!loop)
        return QD_ERR_INVAL;

    tls_current_loop = loop;

    /* Process pending work first */
    process_pending_work(loop);

    /* Call pre-poll callback */
    if (loop->pre_poll_callback)
        loop->pre_poll_callback(loop, loop->pre_poll_arg);

    /* Calculate timeout based on timer wheel */
    if (loop->timer_wheel) {
        int64_t next = qd_timer_wheel_next_timeout(loop->timer_wheel);
        if (next >= 0) {
            if (timeout_ms < 0 || next < timeout_ms)
                timeout_ms = (int)next;
        }
    }

    /* Wait for events */
    struct epoll_event *events = loop->epoll_events;
    int nfds = epoll_wait(loop->epoll_fd, events, loop->max_events, timeout_ms);

    loop->poll_count++;

    if (nfds < 0) {
        if (errno == EINTR)
            return QD_OK;
        return QD_ERR_IO;
    }

    if (nfds == 0) {
        loop->timeout_count++;

        /* Call idle callback */
        if (loop->idle_callback)
            loop->idle_callback(loop, loop->idle_arg);

        return QD_OK;
    }

    /* Process events */
    for (int i = 0; i < nfds; i++) {
        qd_event_source_t *source = events[i].data.ptr;
        if (!source || !source->enabled)
            continue;

        uint32_t revents = from_epoll_events(events[i].events);
        source->active_events = revents;

        if (source->callback)
            source->callback(source->fd, revents, source->arg);

        loop->event_count++;
    }

    /* Call post-poll callback */
    if (loop->post_poll_callback)
        loop->post_poll_callback(loop, loop->post_poll_arg);

    qd_atomic_fetch_add(&loop->iteration, 1);

    return QD_OK;
}

int qd_event_loop_run(qd_event_loop_t *loop)
{
    if (!loop)
        return QD_ERR_INVAL;

    qd_atomic_store(&loop->state, QD_LOOP_RUNNING);

    while (qd_atomic_load(&loop->state) == QD_LOOP_RUNNING) {
        int ret = qd_event_loop_run_once(loop, loop->default_timeout);
        if (ret != QD_OK && ret != QD_ERR_TIMEOUT)
            break;
    }

    qd_atomic_store(&loop->state, QD_LOOP_STOPPED);
    tls_current_loop = NULL;

    return QD_OK;
}

void qd_event_loop_stop(qd_event_loop_t *loop)
{
    if (!loop)
        return;

    qd_atomic_store(&loop->state, QD_LOOP_STOPPING);
    qd_event_loop_wakeup(loop);
}

int qd_event_loop_is_running(qd_event_loop_t *loop)
{
    if (!loop)
        return 0;
    return qd_atomic_load(&loop->state) == QD_LOOP_RUNNING;
}

void qd_event_loop_wakeup(qd_event_loop_t *loop)
{
    if (!loop || loop->wakeup_fd == -1)
        return;

    uint64_t val = 1;
    ssize_t n = write(loop->wakeup_fd, &val, sizeof(val));
    QD_UNUSED(n);
}

void qd_event_set_idle_callback(qd_event_loop_t *loop, qd_idle_cb_t callback, void *arg)
{
    if (!loop)
        return;
    loop->idle_callback = callback;
    loop->idle_arg = arg;
}

void qd_event_set_pre_poll_callback(qd_event_loop_t *loop, qd_poll_cb_t callback, void *arg)
{
    if (!loop)
        return;
    loop->pre_poll_callback = callback;
    loop->pre_poll_arg = arg;
}

void qd_event_set_post_poll_callback(qd_event_loop_t *loop, qd_poll_cb_t callback, void *arg)
{
    if (!loop)
        return;
    loop->post_poll_callback = callback;
    loop->post_poll_arg = arg;
}

void qd_event_loop_stats(qd_event_loop_t *loop, qd_event_loop_stats_t *stats)
{
    if (!loop || !stats)
        return;

    stats->poll_count = loop->poll_count;
    stats->event_count = loop->event_count;
    stats->timeout_count = loop->timeout_count;
    stats->num_sources = loop->num_sources;
    stats->state = qd_atomic_load(&loop->state);
    stats->iteration = qd_atomic_load(&loop->iteration);
}

qd_event_loop_t *qd_event_loop_current(void)
{
    return tls_current_loop;
}

/*
 * Async notification
 */

/* Wrapper callback for async events */
static void async_event_wrapper(int fd, uint32_t events, void *arg)
{
    qd_async_t *async = arg;
    QD_UNUSED(events);
    
    /* Read the eventfd to reset it */
    uint64_t val;
    ssize_t n = read(fd, &val, sizeof(val));
    QD_UNUSED(n);
    
    if (async->callback)
        async->callback(async->arg);
}

qd_async_t *qd_async_create(qd_event_loop_t *loop, void (*callback)(void *arg), void *arg)
{
    if (!loop)
        return NULL;

    qd_async_t *async = calloc(1, sizeof(qd_async_t));
    if (!async)
        return NULL;

    async->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (async->fd == -1) {
        free(async);
        return NULL;
    }

    async->callback = callback;
    async->arg = arg;

    async->source = qd_event_add(loop, async->fd, QD_EVENT_READ,
                                  async_event_wrapper, async);
    if (!async->source) {
        close(async->fd);
        free(async);
        return NULL;
    }

    return async;
}

void qd_async_destroy(qd_async_t *async)
{
    if (!async)
        return;

    if (async->source)
        qd_event_del(async->source->loop, async->source);

    if (async->fd != -1)
        close(async->fd);

    free(async);
}

int qd_async_send(qd_async_t *async)
{
    if (!async || async->fd == -1)
        return QD_ERR_INVAL;

    uint64_t val = 1;
    ssize_t n = write(async->fd, &val, sizeof(val));
    if (n != sizeof(val))
        return QD_ERR_IO;

    return QD_OK;
}
