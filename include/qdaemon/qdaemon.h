/*
 * QDaemon - Main Include Header
 * Professional Linux Daemon Framework
 *
 * This is the main header that includes all QDaemon components.
 * For selective inclusion, include individual headers instead.
 */

#ifndef QDAEMON_H
#define QDAEMON_H

/* Version information */
#define QDAEMON_VERSION_MAJOR 1
#define QDAEMON_VERSION_MINOR 0
#define QDAEMON_VERSION_PATCH 0
#define QDAEMON_VERSION_STRING "1.0.0"

/* Common types and macros */
#include "qd_common.h"

/* Memory allocator */
#include "qd_memory.h"

/* Thread pool */
#include "qd_threadpool.h"

/* Event loop */
#include "qd_event.h"

/* Timer wheel */
#include "qd_timer.h"

/* IPC system */
#include "qd_ipc.h"

/* Kernel IPC (Netlink) */
#include "qd_netlink.h"

/* Daemon lifecycle */
#include "qd_daemon.h"

/* Logging */
#include "qd_log.h"

/* Configuration */
#include "qd_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Global Initialization
 */

/* Initialize all QDaemon subsystems */
int qdaemon_init(void);

/* Shutdown all QDaemon subsystems */
void qdaemon_shutdown(void);

/* Get QDaemon version string */
const char *qdaemon_version(void);

/* Get QDaemon version components */
void qdaemon_version_info(int *major, int *minor, int *patch);

#ifdef __cplusplus
}
#endif

#endif /* QDAEMON_H */
