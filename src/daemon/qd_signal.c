/*
 * QDaemon - Signal Handling Implementation
 * Thread-safe signal handling using signalfd
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <pthread.h>

#include "qdaemon/qd_daemon.h"
#include "qdaemon/qd_event.h"
#include "qdaemon/qd_log.h"

/* Local signal handler entry */
typedef struct qd_sig_handler_entry {
    int signo;
    void (*handler)(int signo, void *arg);
    void *arg;
} qd_sig_handler_entry_t;

static struct {
    int signalfd;
    sigset_t mask;
    qd_sig_handler_entry_t handlers[32];
    int num_handlers;
    pthread_mutex_t lock;
    int initialized;
} g_signal = {
    .signalfd = -1,
    .num_handlers = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = 0,
};

/* Default signal handlers */
static qd_daemon_t *g_daemon = NULL;

static void default_sigterm_handler(int signo, void *arg)
{
    (void)signo;
    qd_daemon_t *daemon = arg;
    if (daemon) {
        qd_log_info("Received SIGTERM, initiating shutdown");
        qd_daemon_shutdown(daemon);
    }
}

static void default_sigint_handler(int signo, void *arg)
{
    (void)signo;
    qd_daemon_t *daemon = arg;
    if (daemon) {
        qd_log_info("Received SIGINT, initiating shutdown");
        qd_daemon_shutdown(daemon);
    }
}

static void default_sighup_handler(int signo, void *arg)
{
    (void)signo;
    qd_daemon_t *daemon = arg;
    if (daemon) {
        qd_log_info("Received SIGHUP, reloading configuration");
        qd_daemon_reload(daemon);
    }
}

static void default_sigchld_handler(int signo, void *arg)
{
    (void)signo;
    (void)arg;
    /* Reap zombie children */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

int qd_signal_init(qd_daemon_t *daemon)
{
    pthread_mutex_lock(&g_signal.lock);

    if (g_signal.initialized) {
        pthread_mutex_unlock(&g_signal.lock);
        return QD_OK;
    }

    sigemptyset(&g_signal.mask);
    sigaddset(&g_signal.mask, SIGTERM);
    sigaddset(&g_signal.mask, SIGINT);
    sigaddset(&g_signal.mask, SIGHUP);
    sigaddset(&g_signal.mask, SIGCHLD);
    sigaddset(&g_signal.mask, SIGUSR1);
    sigaddset(&g_signal.mask, SIGUSR2);

    /* Block signals so they're delivered via signalfd */
    if (sigprocmask(SIG_BLOCK, &g_signal.mask, NULL) < 0) {
        pthread_mutex_unlock(&g_signal.lock);
        return QD_ERR_SYSTEM;
    }

    g_signal.signalfd = signalfd(-1, &g_signal.mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_signal.signalfd < 0) {
        sigprocmask(SIG_UNBLOCK, &g_signal.mask, NULL);
        pthread_mutex_unlock(&g_signal.lock);
        return QD_ERR_SYSTEM;
    }

    g_daemon = daemon;

    /* Register default handlers */
    g_signal.handlers[g_signal.num_handlers++] = (qd_sig_handler_entry_t){
        .signo = SIGTERM, .handler = default_sigterm_handler, .arg = daemon
    };
    g_signal.handlers[g_signal.num_handlers++] = (qd_sig_handler_entry_t){
        .signo = SIGINT, .handler = default_sigint_handler, .arg = daemon
    };
    g_signal.handlers[g_signal.num_handlers++] = (qd_sig_handler_entry_t){
        .signo = SIGHUP, .handler = default_sighup_handler, .arg = daemon
    };
    g_signal.handlers[g_signal.num_handlers++] = (qd_sig_handler_entry_t){
        .signo = SIGCHLD, .handler = default_sigchld_handler, .arg = daemon
    };

    g_signal.initialized = 1;
    pthread_mutex_unlock(&g_signal.lock);

    return QD_OK;
}

void qd_signal_shutdown(void)
{
    pthread_mutex_lock(&g_signal.lock);

    if (!g_signal.initialized) {
        pthread_mutex_unlock(&g_signal.lock);
        return;
    }

    if (g_signal.signalfd >= 0) {
        close(g_signal.signalfd);
        g_signal.signalfd = -1;
    }

    sigprocmask(SIG_UNBLOCK, &g_signal.mask, NULL);

    g_signal.num_handlers = 0;
    g_signal.initialized = 0;
    g_daemon = NULL;

    pthread_mutex_unlock(&g_signal.lock);
}

int qd_signal_get_fd(void)
{
    return g_signal.signalfd;
}

int qd_signal_process(void)
{
    if (g_signal.signalfd < 0)
        return QD_ERR_INVAL;

    struct signalfd_siginfo info;
    ssize_t n;

    while ((n = read(g_signal.signalfd, &info, sizeof(info))) == sizeof(info)) {
        int signo = info.ssi_signo;

        pthread_mutex_lock(&g_signal.lock);
        for (int i = 0; i < g_signal.num_handlers; i++) {
            if (g_signal.handlers[i].signo == signo && g_signal.handlers[i].handler) {
                g_signal.handlers[i].handler(signo, g_signal.handlers[i].arg);
            }
        }
        pthread_mutex_unlock(&g_signal.lock);
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return QD_ERR_IO;
    }

    return QD_OK;
}

int qd_signal_register(int signo, void (*handler)(int, void*), void *arg)
{
    pthread_mutex_lock(&g_signal.lock);

    if (g_signal.num_handlers >= 32) {
        pthread_mutex_unlock(&g_signal.lock);
        return QD_ERR_NOMEM;
    }

    /* Check for existing handler and replace */
    for (int i = 0; i < g_signal.num_handlers; i++) {
        if (g_signal.handlers[i].signo == signo) {
            g_signal.handlers[i].handler = handler;
            g_signal.handlers[i].arg = arg;
            pthread_mutex_unlock(&g_signal.lock);
            return QD_OK;
        }
    }

    g_signal.handlers[g_signal.num_handlers++] = (qd_sig_handler_entry_t){
        .signo = signo, .handler = handler, .arg = arg
    };

    pthread_mutex_unlock(&g_signal.lock);
    return QD_OK;
}

int qd_signal_unregister(int signo)
{
    pthread_mutex_lock(&g_signal.lock);

    for (int i = 0; i < g_signal.num_handlers; i++) {
        if (g_signal.handlers[i].signo == signo) {
            memmove(&g_signal.handlers[i], &g_signal.handlers[i + 1],
                    (g_signal.num_handlers - i - 1) * sizeof(qd_sig_handler_entry_t));
            g_signal.num_handlers--;
            pthread_mutex_unlock(&g_signal.lock);
            return QD_OK;
        }
    }

    pthread_mutex_unlock(&g_signal.lock);
    return QD_ERR_NOENT;
}

/* Block all signals in current thread */
int qd_signal_block_all(void)
{
    sigset_t mask;
    sigfillset(&mask);
    return pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0 ? QD_OK : QD_ERR_SYSTEM;
}

/* Unblock signals in current thread */
int qd_signal_unblock_all(void)
{
    sigset_t mask;
    sigfillset(&mask);
    return pthread_sigmask(SIG_UNBLOCK, &mask, NULL) == 0 ? QD_OK : QD_ERR_SYSTEM;
}

/* Ignore a signal */
int qd_signal_ignore(int signo)
{
    struct sigaction sa = { .sa_handler = SIG_IGN };
    return sigaction(signo, &sa, NULL) == 0 ? QD_OK : QD_ERR_SYSTEM;
}

/* Restore default signal handler */
int qd_signal_default(int signo)
{
    struct sigaction sa = { .sa_handler = SIG_DFL };
    return sigaction(signo, &sa, NULL) == 0 ? QD_OK : QD_ERR_SYSTEM;
}
