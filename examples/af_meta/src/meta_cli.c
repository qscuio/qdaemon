/*
 * Meta CLI - Professional Shell Example
 * Demonstrates:
 * - Multiple modes (root, enable, config)
 * - Navigation between modes
 * - Command verification
 * - CINT function calls
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <qdaemon/qd_cli.h>
#include "qd_meta_sock.h"

/* Shell instance */
static qd_cli_shell_t *g_shell;

/* Modes */
static qd_cli_mode_t *g_mode_root;
static qd_cli_mode_t *g_mode_enable;
static qd_cli_mode_t *g_mode_config;

/* Server connection */
static int g_sockfd = -1;

#define META_SERVER_CTRL_PORT 5000

static int connect_server(void)
{
    if (g_sockfd >= 0) return 0;
    
    g_sockfd = qdmeta_socket(SOCK_DGRAM, 1);
    if (g_sockfd < 0) {
        printf("Error: Cannot create socket. Is qd_af_meta.ko loaded?\n");
        return -1;
    }
    
    if (qdmeta_bind(g_sockfd, 0, 0, 1) < 0) {
        printf("Error: Cannot bind socket\n");
        close(g_sockfd);
        g_sockfd = -1;
        return -1;
    }
    return 0;
}

/* ====================== Commands ====================== */

static qd_cli_result_t cmd_enable(qd_cli_ctx_t *ctx)
{
    qd_cli_enter_mode(ctx->shell, g_mode_enable);
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_disable(qd_cli_ctx_t *ctx)
{
    qd_cli_exit_mode(ctx->shell);
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_configure(qd_cli_ctx_t *ctx)
{
    qd_cli_enter_mode(ctx->shell, g_mode_config);
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_status(qd_cli_ctx_t *ctx)
{
    if (connect_server() < 0) return QD_CLI_ERROR;
    
    ctx->print("Server Status:\n");
    ctx->print("  State:   Running\n");
    ctx->print("  Socket:  Connected\n");
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_ping(qd_cli_ctx_t *ctx)
{
    int count = 3;
    if (ctx->argc > 0)
        count = atoi(ctx->argv[0]);
    
    if (connect_server() < 0) return QD_CLI_ERROR;
    
    ctx->print("Pinging server (count=%d)...\n", count);
    for (int i=0; i<count; i++) {
        /* Simulate ping */
        usleep(50000);
        ctx->print("PING seq=%d time=0.1ms\n", i+1);
    }
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_hostname(qd_cli_ctx_t *ctx)
{
    if (ctx->argc < 1) {
        ctx->error("Usage: hostname <name>");
        return QD_CLI_ERROR;
    }
    ctx->print("Hostname set to %s\n", ctx->argv[0]);
    return QD_CLI_OK;
}

/* ====================== CINT Functions ====================== */

/* API wrapper for CINT */
long long bcm_port_enable(long long port, long long enable)
{
    printf("[API] bcm_port_enable(port=%lld, enable=%lld)\n", port, enable);
    /* In real app, call actual BCM API */
    return 0;
}

long long bcm_port_stat_get(long long port)
{
    printf("[API] bcm_port_stat_get(port=%lld)\n", port);
    return 123456789; /* Simulated stat */
}

/* ====================== Main ====================== */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    
    qd_cli_config_t config = QD_CLI_CONFIG_DEFAULT;
    config.name = "MetaShell";
    config.version = "2.0";
    config.base_prompt = "meta";
    config.history_file = ".meta_history";
    
    g_shell = qd_cli_create(&config);
    if (!g_shell) return 1;
    
    /* Create Modes */
    g_mode_root = qd_cli_get_root_mode(g_shell);
    
    g_mode_enable = qd_cli_mode_create(g_shell, "enable", "#", "Privileged Exec");
    qd_cli_mode_set_parent(g_mode_enable, g_mode_root);
    
    g_mode_config = qd_cli_mode_create(g_shell, "config", "(config)", "Configuration");
    qd_cli_mode_set_parent(g_mode_config, g_mode_enable);
    
    /* Register Commands: Root */
    qd_cli_register_cmd(g_mode_root, "enable", cmd_enable, "Enter privileged mode");
    qd_cli_register_cmd(g_mode_root, "status", cmd_status, "Show server status");
    qd_cli_register_cmd(g_mode_root, "ping", cmd_ping, "Ping server");
    
    /* Register Commands: Enable */
    qd_cli_register_cmd(g_mode_enable, "disable", cmd_disable, "Exit privileged mode");
    qd_cli_register_cmd(g_mode_enable, "configure", cmd_configure, "Enter configuration mode");
    qd_cli_register_cmd(g_mode_enable, "ping", cmd_ping, "Ping server (privileged)");
    
    /* Register Commands: Config */
    qd_cli_register_cmd(g_mode_config, "hostname", cmd_hostname, "Set system hostname");
    
    /* Register CINT Functions */
    qd_cint_register(g_shell, "bcm_port_enable", bcm_port_enable, QD_CINT_INT, 2, QD_CINT_INT, QD_CINT_INT);
    qd_cint_register(g_shell, "bcm_port_stat_get", bcm_port_stat_get, QD_CINT_INT64, 1, QD_CINT_INT);
    
    printf("Starting Meta CLI Shell...\n");
    printf("Try 'enable' then 'configure' to switch modes.\n");
    printf("Try CINT: bcm_port_enable(1, 1)\n\n");
    
    qd_cli_run(g_shell);
    
    qd_cli_destroy(g_shell);
    if (g_sockfd >= 0) close(g_sockfd);
    
    return 0;
}
