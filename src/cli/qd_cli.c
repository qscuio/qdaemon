/*
 * QDaemon - Professional Modal CLI Shell Implementation
 * 
 * Implements:
 * - Mode stack management
 * - Readline integration with auto-completion
 * - Command dispatching
 * - CINT-like function execution
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <qdaemon/qd_cli.h>
#include "qd_cint.h"

/*
 * Internal Structures
 */

struct qd_cli_shell {
    qd_cli_config_t config;
    
    /* Modes */
    qd_cli_mode_t *modes[QD_CLI_MAX_MODES];
    int num_modes;
    
    /* Mode Stack */
    qd_cli_mode_t *mode_stack[16];
    int mode_depth;
    
    /* CINT Functions */
    qd_cint_func_t *cint_funcs[QD_CLI_MAX_CINT_FUNCS];
    int num_cint_funcs;

    /* CINT Interpreter */
    qd_cint_t *cint;
    
    /* Global Commands */
    qd_cli_cmd_t *global_cmds[QD_CLI_MAX_CMDS];
    int num_global_cmds;
    
    /* State */
    bool running;
    char prompt_buf[QD_CLI_PROMPT_SIZE];
};

static qd_cli_shell_t *g_current_shell = NULL;

/*
 * Helper Functions
 */

static void format_prompt(qd_cli_shell_t *shell)
{
    qd_cli_mode_t *current = qd_cli_get_current_mode(shell);
    
    if (shell->config.show_mode_in_prompt && current && current->parent) {
        snprintf(shell->prompt_buf, sizeof(shell->prompt_buf), 
                 "%s(%s)> ", shell->config.base_prompt, current->prompt);
    } else {
        snprintf(shell->prompt_buf, sizeof(shell->prompt_buf), 
                 "%s> ", shell->config.base_prompt);
    }
}

static qd_cli_cmd_t *find_command(qd_cli_mode_t *mode, const char *name)
{
    if (!mode) return NULL;
    
    /* Search mode commands */
    if (mode->commands) {
        for (int i = 0; i < mode->num_commands; i++) {
            if (strcmp(mode->commands[i].name, name) == 0)
                return &mode->commands[i];
        }
    }
    
    return NULL;
}

static qd_cli_cmd_t *find_global_command(qd_cli_shell_t *shell, const char *name)
{
    for (int i = 0; i < shell->num_global_cmds; i++) {
        if (strcmp(shell->global_cmds[i]->name, name) == 0)
            return shell->global_cmds[i];
    }
    return NULL;
}

/*
 * Readline Completion
 */

static char *command_generator(const char *text, int state)
{
    static int list_index;
    static int global_index;
    static int len;
    
    if (!g_current_shell) return NULL;
    
    qd_cli_mode_t *mode = qd_cli_get_current_mode(g_current_shell);
    
    if (!state) {
        list_index = 0;
        global_index = 0;
        len = strlen(text);
    }
    
    /* Check mode commands */
    if (mode && mode->commands) {
        while (list_index < mode->num_commands) {
            qd_cli_cmd_t *cmd = &mode->commands[list_index++];
            if (!cmd->hidden && strncmp(cmd->name, text, len) == 0) {
                return strdup(cmd->name);
            }
        }
    }
    
    /* Check global commands */
    while (global_index < g_current_shell->num_global_cmds) {
        qd_cli_cmd_t *cmd = g_current_shell->global_cmds[global_index++];
        if (!cmd->hidden && strncmp(cmd->name, text, len) == 0) {
            return strdup(cmd->name);
        }
    }
    
    return NULL;
}

static char **cli_completion(const char *text, int start, int end)
{
    (void)end;
    char **matches = NULL;
    
    if (start == 0) {
        matches = rl_completion_matches(text, command_generator);
    }
    
    return matches;
}

/*
 * Built-in Commands
 */

static qd_cli_result_t cmd_exit(qd_cli_ctx_t *ctx)
{
    if (qd_cli_exit_mode(ctx->shell) != 0) {
        /* Already at root, exit shell */
        return QD_CLI_EXIT;
    }
    return QD_CLI_MODE_CHANGE;
}

static qd_cli_result_t cmd_help(qd_cli_ctx_t *ctx)
{
    if (ctx->argc > 0) {
        /* Specific command help */
        // TODO: Implement specific help
    } else {
        /* List commands in current mode */
        qd_cli_print_mode_help(ctx->mode);
    }
    return QD_CLI_OK;
}

static qd_cli_result_t cmd_history(qd_cli_ctx_t *ctx)
{
    HIST_ENTRY **list = history_list();
    if (list) {
        for (int i = 0; list[i]; i++) {
            ctx->print("%4d  %s\n", i + history_base, list[i]->line);
        }
    }
    return QD_CLI_OK;
}

/*
 * CINT Implementation
 */

static qd_cint_func_t *find_cint_func(qd_cli_shell_t *shell, const char *name)
{
    for (int i = 0; i < shell->num_cint_funcs; i++) {
        if (strcmp(shell->cint_funcs[i]->name, name) == 0)
            return shell->cint_funcs[i];
    }
    return NULL;
}

/* Simple parser: func(arg1, arg2) */
static int parse_cint_call(char *line, char **func_name, int *argc, char **argv)
{
    char *p = line;
    while (isspace(*p)) p++;
    
    *func_name = p;
    char *paren = strchr(p, '(');
    if (!paren) return -1;
    
    *paren = '\0';
    p = paren + 1;
    
    /* Parse args */
    *argc = 0;
    while (*p && *p != ')') {
        while (isspace(*p)) p++;
        if (*p == ')') break;
        
        argv[(*argc)++] = p;
        if (*argc >= 16) break;
        
        char *comma = strchr(p, ',');
        char *close = strchr(p, ')');
        
        if (comma && (!close || comma < close)) {
            *comma = '\0';
            p = comma + 1;
        } else if (close) {
            *close = '\0';
            p = close + 1;
        } else {
            break; 
        }
    }
    
    return 0;
}

static qd_cli_result_t cmd_cint(qd_cli_ctx_t *ctx)
{
    /* If args provided, execute single line */
    if (ctx->argc > 0) {
        /* Check if first arg is a registered CINT function */
        qd_cint_func_t *func = find_cint_func(ctx->shell, ctx->argv[0]);
        if (func) {
            return qd_cint_call(ctx->shell, ctx->argv[0], ctx->argc - 1, ctx->argv + 1, NULL);
        }
    }
    
    ctx->print("Starting QD-CINT Interactive Shell (clean-room)\n");
    ctx->print("Type 'exit' to return to CLI\n");
    
    qd_cint_repl(ctx->shell->cint, "cint> ");
    
    return QD_CLI_OK;
}

/*
 * Shell API Implementation
 */

qd_cli_shell_t *qd_cli_create(const qd_cli_config_t *config)
{
    qd_cli_shell_t *shell = calloc(1, sizeof(qd_cli_shell_t));
    if (!shell) return NULL;
    
    if (config)
        memcpy(&shell->config, config, sizeof(qd_cli_config_t));
    else {
        qd_cli_config_t def = QD_CLI_CONFIG_DEFAULT;
        memcpy(&shell->config, &def, sizeof(def));
    }
    
    /* Create root mode */
    qd_cli_mode_t *root = qd_cli_mode_create(shell, "root", "", "Root mode");
    shell->mode_stack[0] = root;
    shell->mode_depth = 1;
    
    /* Create CINT interpreter */
    shell->cint = qd_cint_create();

    /* Register built-ins */
    qd_cli_register_global_cmd(shell, "exit", cmd_exit, "Exit current mode");
    qd_cli_register_global_cmd(shell, "quit", cmd_exit, "Exit current mode");
    qd_cli_register_global_cmd(shell, "help", cmd_help, "Show help");
    qd_cli_register_global_cmd(shell, "?", cmd_help, "Show help");
    qd_cli_register_global_cmd(shell, "history", cmd_history, "Show command history");
    qd_cli_register_global_cmd(shell, "cint", cmd_cint, "Execute CINT function");
    
    return shell;
}

void qd_cli_destroy(qd_cli_shell_t *shell)
{
    if (!shell) return;
    
    for (int i = 0; i < shell->num_modes; i++) {
        if (shell->modes[i]->commands)
            free(shell->modes[i]->commands);
        free(shell->modes[i]);
    }
    
    for (int i = 0; i < shell->num_cint_funcs; i++) {
        free(shell->cint_funcs[i]);
    }

    if (shell->cint)
        qd_cint_destroy(shell->cint);
    
    for (int i = 0; i < shell->num_global_cmds; i++) {
        free(shell->global_cmds[i]);
    }
    
    free(shell);
}

int qd_cli_run(qd_cli_shell_t *shell)
{
    shell->running = true;
    g_current_shell = shell;
    
    /* Init readline */
    rl_attempted_completion_function = cli_completion;
    if (shell->config.history_file)
        read_history(shell->config.history_file);
        
    printf("%s v%s\n", shell->config.name, shell->config.version);
    
    while (shell->running) {
        format_prompt(shell);
        char *line = readline(shell->prompt_buf);
        
        if (!line) {
            printf("\n");
            break;
        }
        
        if (line[0]) {
            add_history(line);
            qd_cli_execute(shell, line);
        }
        
        free(line);
    }
    
    if (shell->config.history_file)
        write_history(shell->config.history_file);
        
    return 0;
}

qd_cli_result_t qd_cli_execute(qd_cli_shell_t *shell, const char *cmdline)
{
    /* Tokenize */
    char *buf = strdup(cmdline);
    char *argv[QD_CLI_MAX_ARGS];
    int argc = 0;
    
    char *token = strtok(buf, " \t");
    while (token && argc < QD_CLI_MAX_ARGS) {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }
    
    if (argc == 0) {
        free(buf);
        return QD_CLI_OK;
    }
    
    /* Check CINT features first if enabled */
    /* Check for variable assignment: var = val */
    /* Check for function call: func(...) */
    if (strchr(cmdline, '(') && strchr(cmdline, ')')) {
        char *func_buf = strdup(cmdline);
        char *c_func;
        char *c_argv[16];
        int c_argc;
        
        if (parse_cint_call(func_buf, &c_func, &c_argc, c_argv) == 0) {
             qd_cint_func_t *f = find_cint_func(shell, c_func);
             if (f) {
                 int ret = qd_cint_call(shell, c_func, c_argc, (const char**)c_argv, NULL);
                 free(func_buf);
                 free(buf);
                 return ret;
             }
        }
        free(func_buf);
    }

    /* Find command */
    qd_cli_mode_t *current = qd_cli_get_current_mode(shell);
    qd_cli_cmd_t *cmd = find_command(current, argv[0]);
    
    if (!cmd) {
        /* Helper: Maybe global command? */
        cmd = find_global_command(shell, argv[0]);
    }
    
    if (!cmd) {
         /* Check if it's a CINT function directly called as command */
        qd_cint_func_t *f = find_cint_func(shell, argv[0]);
        if (f) {
            int ret = qd_cint_call(shell, argv[0], argc-1, (const char**)(argv+1), NULL);
            free(buf);
            return ret;
        }
        
        printf("Unknown command: %s\n", argv[0]);
        free(buf);
        return QD_CLI_ERROR;
    }
    
    /* Execute */
    qd_cli_ctx_t ctx = {
        .shell = shell,
        .mode = current,
        .cmd = cmd,
        .argc = argc - 1,
        .argv = (const char **)(argv + 1),
        .user_data = shell->config.user_data,
        .print = printf,
        .error = (int(*)(const char*,...))printf
    };
    
    qd_cli_result_t res = cmd->handler(&ctx);
    
    if (res == QD_CLI_EXIT) {
        shell->running = false;
    }
    
    free(buf);
    return res;
}

/*
 * Mode API Implementation
 */

qd_cli_mode_t *qd_cli_mode_create(qd_cli_shell_t *shell, const char *name,
                                   const char *prompt, const char *help)
{
    if (shell->num_modes >= QD_CLI_MAX_MODES) return NULL;
    
    qd_cli_mode_t *mode = calloc(1, sizeof(qd_cli_mode_t));
    mode->name = strdup(name);
    mode->prompt = strdup(prompt);
    mode->help = strdup(help);
    mode->max_commands = QD_CLI_MAX_CMDS;
    mode->commands = calloc(mode->max_commands, sizeof(qd_cli_cmd_t));
    
    shell->modes[shell->num_modes++] = mode;
    return mode;
}

int qd_cli_mode_set_parent(qd_cli_mode_t *mode, qd_cli_mode_t *parent)
{
    mode->parent = parent;
    return 0;
}

qd_cli_mode_t *qd_cli_get_root_mode(qd_cli_shell_t *shell)
{
    if (shell->mode_depth == 0) return NULL;
    return shell->mode_stack[0];
}

qd_cli_mode_t *qd_cli_get_current_mode(qd_cli_shell_t *shell)
{
    if (shell->mode_depth == 0) return NULL;
    return shell->mode_stack[shell->mode_depth - 1];
}

int qd_cli_enter_mode(qd_cli_shell_t *shell, qd_cli_mode_t *mode)
{
    if (shell->mode_depth >= 16) return -1;
    shell->mode_stack[shell->mode_depth++] = mode;
    return 0;
}

int qd_cli_exit_mode(qd_cli_shell_t *shell)
{
    if (shell->mode_depth <= 1) return -1; /* Can't exit root */
    shell->mode_depth--;
    return 0;
}

/*
 * Command API Implementation
 */

int qd_cli_register_cmd(qd_cli_mode_t *mode, const char *name,
                        qd_cli_handler_t handler, const char *help)
{
    if (mode->num_commands >= mode->max_commands) return -1;
    
    qd_cli_cmd_t *cmd = &mode->commands[mode->num_commands++];
    cmd->name = strdup(name);
    cmd->handler = handler;
    cmd->help = strdup(help);
    
    return 0;
}

int qd_cli_register_global_cmd(qd_cli_shell_t *shell, const char *name,
                               qd_cli_handler_t handler, const char *help)
{
    if (shell->num_global_cmds >= QD_CLI_MAX_CMDS) return -1;
    
    qd_cli_cmd_t *cmd = calloc(1, sizeof(qd_cli_cmd_t));
    cmd->name = strdup(name);
    cmd->handler = handler;
    cmd->help = strdup(help);
    cmd->mode_global = true;
    
    shell->global_cmds[shell->num_global_cmds++] = cmd;
    return 0;
}

/*
 * CINT API Implementation
 */
 
int qd_cint_register(qd_cli_shell_t *shell, const char *name,
                     void *func_ptr, qd_cint_type_t ret_type,
                     int num_args, ...)
{
    if (shell->num_cint_funcs >= QD_CLI_MAX_CINT_FUNCS) return -1;
    
    qd_cint_func_t *f = calloc(1, sizeof(qd_cint_func_t));
    f->name = strdup(name);
    f->func_ptr = func_ptr;
    f->ret_type = ret_type;
    f->num_args = num_args;
    
    va_list args;
    va_start(args, num_args);
    for (int i = 0; i < num_args; i++) {
        f->arg_types[i] = va_arg(args, qd_cint_type_t);
    }
    va_end(args);
    
    shell->cint_funcs[shell->num_cint_funcs++] = f;
    
    /* Register with interpreter */
    qd_cint_register_native(shell->cint, name, func_ptr, num_args);
    
    return 0;
}

int qd_cint_call(qd_cli_shell_t *shell, const char *funcname,
                 int argc, const char **argv, void *ret)
{
    (void)ret; /* TODO: Return value support */
    qd_cint_func_t *f = find_cint_func(shell, funcname);
    if (!f) {
        printf("CINT: Function not found: %s\n", funcname);
        return -1;
    }
    
    if (argc < f->num_args) {
        printf("CINT: Too few arguments for %s (expected %d, got %d)\n", 
               funcname, f->num_args, argc);
        return -1;
    }
    
    /* Use CINT interpreter */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s(", funcname);
    
    for (int i = 0; i < argc; i++) {
        strncat(buf, argv[i], sizeof(buf) - strlen(buf) - 2);
        if (i < argc - 1)
            strcat(buf, ", ");
    }
    strcat(buf, ")");
    
    return qd_cint_eval(shell->cint, buf);
}

/*
 * Output Helpers
 */

void qd_cli_print_mode_help(qd_cli_mode_t *mode)
{
    if (!mode) return;
    
    printf("\nCommands in %s mode:\n", mode->name);
    
    int max_len = 0;
    for (int i = 0; i < mode->num_commands; i++) {
        int len = strlen(mode->commands[i].name);
        if (len > max_len) max_len = len;
    }
    
    for (int i = 0; i < mode->num_commands; i++) {
        printf("  %-*s  %s\n", max_len, mode->commands[i].name, 
               mode->commands[i].help ? mode->commands[i].help : "");
    }
    printf("\n");
}

int qd_cli_printf(qd_cli_ctx_t *ctx, const char *fmt, ...)
{
    (void)ctx;
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}
