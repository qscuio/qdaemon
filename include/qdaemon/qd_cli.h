/*
 * QDaemon - Professional Modal CLI Shell
 * 
 * Features:
 * - Multiple modes with hierarchical structure
 * - User-defined custom modes
 * - Command auto-completion (readline)
 * - Built-in help system
 * - CINT-like internal function execution
 * - Mode-specific command sets
 * 
 * Example:
 *   qd_shell> enable
 *   qd_shell(enable)> config
 *   qd_shell(config)> interface eth0
 *   qd_shell(config-if)> exit
 *   qd_shell(config)>
 */

#ifndef QD_CLI_H
#define QD_CLI_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forward declarations
 */
typedef struct qd_cli_shell qd_cli_shell_t;
typedef struct qd_cli_mode qd_cli_mode_t;
typedef struct qd_cli_cmd qd_cli_cmd_t;
typedef struct qd_cli_ctx qd_cli_ctx_t;
typedef struct qd_cint_func qd_cint_func_t;

/*
 * Limits
 */
#define QD_CLI_MAX_MODES        32
#define QD_CLI_MAX_CMDS         128
#define QD_CLI_MAX_ARGS         32
#define QD_CLI_MAX_HISTORY      100
#define QD_CLI_LINE_SIZE        1024
#define QD_CLI_PROMPT_SIZE      64
#define QD_CLI_MAX_CINT_FUNCS   256

/*
 * Return codes
 */
typedef enum {
    QD_CLI_OK = 0,
    QD_CLI_ERROR = -1,
    QD_CLI_EXIT = 1,
    QD_CLI_MODE_CHANGE = 2,
    QD_CLI_SHOW_HELP = 3,
} qd_cli_result_t;

/*
 * Argument types for CINT
 */
typedef enum {
    QD_CINT_VOID = 0,
    QD_CINT_INT,
    QD_CINT_UINT,
    QD_CINT_INT64,
    QD_CINT_UINT64,
    QD_CINT_DOUBLE,
    QD_CINT_STRING,
    QD_CINT_PTR,
    QD_CINT_BOOL,
} qd_cint_type_t;

/*
 * Command context - passed to all command handlers
 */
struct qd_cli_ctx {
    qd_cli_shell_t *shell;          /* Shell instance */
    qd_cli_mode_t *mode;            /* Current mode */
    qd_cli_cmd_t *cmd;              /* Command being executed */
    
    int argc;                        /* Argument count */
    const char **argv;               /* Argument values */
    
    void *user_data;                 /* User-provided context */
    
    /* Output functions */
    int (*print)(const char *fmt, ...);
    int (*error)(const char *fmt, ...);
};

/*
 * Command handler type
 */
typedef qd_cli_result_t (*qd_cli_handler_t)(qd_cli_ctx_t *ctx);

/*
 * Auto-completion generator type
 * Returns number of completions added
 * completions: array to fill with completion strings
 * max: maximum completions to return
 * word: current word being completed
 * line: full command line
 * pos: cursor position in line
 */
typedef int (*qd_cli_completer_t)(qd_cli_ctx_t *ctx, const char *word,
                                   const char *line, int pos,
                                   char **completions, int max);

/*
 * Command definition
 */
struct qd_cli_cmd {
    const char *name;               /* Command name */
    const char *help;               /* Short help text */
    const char *usage;              /* Usage string */
    qd_cli_handler_t handler;       /* Command handler */
    qd_cli_completer_t completer;   /* Custom completer (optional) */
    
    /* Subcommands */
    qd_cli_cmd_t *subcmds;
    int num_subcmds;
    
    /* Flags */
    bool hidden;                    /* Hide from help */
    bool mode_global;               /* Available in all modes */
};

/*
 * Mode definition
 */
struct qd_cli_mode {
    const char *name;               /* Mode name (config, enable, etc) */
    const char *prompt;             /* Prompt suffix for this mode */
    const char *help;               /* Mode description */
    
    qd_cli_mode_t *parent;          /* Parent mode (NULL for root) */
    
    qd_cli_cmd_t *commands;         /* Commands in this mode */
    int num_commands;
    int max_commands;
    
    void *user_data;                /* Mode-specific context */
};

/*
 * CINT function registration for internal function calling
 */
struct qd_cint_func {
    const char *name;               /* Function name */
    const char *help;               /* Description */
    void *func_ptr;                 /* Function pointer */
    qd_cint_type_t ret_type;        /* Return type */
    qd_cint_type_t arg_types[16];   /* Argument types */
    int num_args;                   /* Number of arguments */
};

/*
 * Shell configuration
 */
typedef struct {
    const char *name;               /* Application name */
    const char *version;            /* Version string */
    const char *base_prompt;        /* Base prompt (before mode) */
    const char *history_file;       /* History file path */
    void *user_data;                /* User context */
    bool show_mode_in_prompt;       /* Show mode name in prompt */
} qd_cli_config_t;

#define QD_CLI_CONFIG_DEFAULT { \
    .name = "qd_shell", \
    .version = "1.0", \
    .base_prompt = "qd_shell", \
    .history_file = NULL, \
    .user_data = NULL, \
    .show_mode_in_prompt = true \
}

/*
 * ====================
 * Shell API
 * ====================
 */

/* Create shell instance */
qd_cli_shell_t *qd_cli_create(const qd_cli_config_t *config);

/* Destroy shell */
void qd_cli_destroy(qd_cli_shell_t *shell);

/* Run interactive shell (REPL with readline) */
int qd_cli_run(qd_cli_shell_t *shell);

/* Execute single command */
qd_cli_result_t qd_cli_execute(qd_cli_shell_t *shell, const char *cmdline);

/* Execute script file */
int qd_cli_exec_file(qd_cli_shell_t *shell, const char *filename);

/*
 * ====================
 * Mode API
 * ====================
 */

/* Create new mode */
qd_cli_mode_t *qd_cli_mode_create(qd_cli_shell_t *shell, const char *name,
                                   const char *prompt, const char *help);

/* Set parent mode (for mode hierarchy) */
int qd_cli_mode_set_parent(qd_cli_mode_t *mode, qd_cli_mode_t *parent);

/* Get root mode */
qd_cli_mode_t *qd_cli_get_root_mode(qd_cli_shell_t *shell);

/* Get current mode */
qd_cli_mode_t *qd_cli_get_current_mode(qd_cli_shell_t *shell);

/* Enter mode */
int qd_cli_enter_mode(qd_cli_shell_t *shell, qd_cli_mode_t *mode);

/* Exit current mode (return to parent) */
int qd_cli_exit_mode(qd_cli_shell_t *shell);

/* Find mode by name */
qd_cli_mode_t *qd_cli_find_mode(qd_cli_shell_t *shell, const char *name);

/*
 * ====================
 * Command API
 * ====================
 */

/* Register command in mode */
int qd_cli_register_cmd(qd_cli_mode_t *mode, const char *name,
                        qd_cli_handler_t handler, const char *help);

/* Register command with full options */
int qd_cli_register_cmd_full(qd_cli_mode_t *mode, const qd_cli_cmd_t *cmd);

/* Register multiple commands */
int qd_cli_register_cmds(qd_cli_mode_t *mode, const qd_cli_cmd_t *cmds, int count);

/* Register global command (available in all modes) */
int qd_cli_register_global_cmd(qd_cli_shell_t *shell, const char *name,
                               qd_cli_handler_t handler, const char *help);

/* Unregister command */
int qd_cli_unregister_cmd(qd_cli_mode_t *mode, const char *name);

/*
 * ====================
 * CINT API - Internal Function Calling
 * ====================
 */

/* Register internal function for CLI access */
int qd_cint_register(qd_cli_shell_t *shell, const char *name,
                     void *func_ptr, qd_cint_type_t ret_type,
                     int num_args, ...);  /* varargs: qd_cint_type_t for each arg */

/* Register with help */
int qd_cint_register_help(qd_cli_shell_t *shell, const char *name,
                          void *func_ptr, const char *help,
                          qd_cint_type_t ret_type, int num_args, ...);

/* Execute CINT function by name */
int qd_cint_call(qd_cli_shell_t *shell, const char *funcname,
                 int argc, const char **argv, void *ret);

/* List all registered CINT functions */
void qd_cint_list(qd_cli_shell_t *shell);

/*
 * ====================
 * Completion Helpers
 * ====================
 */

/* Add completion to list */
int qd_cli_add_completion(char **completions, int *count, int max, const char *text);

/* Complete from word list */
int qd_cli_complete_words(const char *word, const char **words, int num_words,
                          char **completions, int max);

/* Complete filename */
int qd_cli_complete_file(const char *word, char **completions, int max);

/* Complete from command names in mode */
int qd_cli_complete_commands(qd_cli_mode_t *mode, const char *word,
                             char **completions, int max);

/*
 * ====================
 * Output Helpers
 * ====================
 */

/* Colors */
#define QD_CLI_RESET    "\033[0m"
#define QD_CLI_BOLD     "\033[1m"
#define QD_CLI_RED      "\033[31m"
#define QD_CLI_GREEN    "\033[32m"
#define QD_CLI_YELLOW   "\033[33m"
#define QD_CLI_BLUE     "\033[34m"
#define QD_CLI_CYAN     "\033[36m"

/* Print with format */
int qd_cli_printf(qd_cli_ctx_t *ctx, const char *fmt, ...);

/* Print table */
void qd_cli_print_table_header(int cols, const char **headers, int *widths);
void qd_cli_print_table_row(int cols, const char **values, int *widths);
void qd_cli_print_table_sep(int cols, int *widths);

/* Print aligned list */
void qd_cli_print_list(int count, const char **items, const char **descriptions, int indent);

/*
 * ====================
 * Built-in Commands
 * ====================
 */

/* These are automatically registered */
/* help [command]      - Show help */
/* ?                   - Same as help */
/* exit                - Exit current mode or shell */
/* quit                - Same as exit */
/* history             - Show command history */
/* !<n>                - Execute history item n */
/* modes               - List all modes */
/* commands            - List commands in current mode */
/* cint <func> [args]  - Call CINT function */

/*
 * ====================
 * Command Definition Macros
 * ====================
 */

/* Simple command */
#define QD_CLI_CMD(cmd_name, cmd_handler, cmd_help) \
    { .name = cmd_name, .handler = cmd_handler, .help = cmd_help }

/* Command with usage */
#define QD_CLI_CMD_USAGE(cmd_name, cmd_handler, cmd_help, cmd_usage) \
    { .name = cmd_name, .handler = cmd_handler, .help = cmd_help, .usage = cmd_usage }

/* Command with completer */
#define QD_CLI_CMD_COMPLETE(cmd_name, cmd_handler, cmd_help, cmd_comp) \
    { .name = cmd_name, .handler = cmd_handler, .help = cmd_help, .completer = cmd_comp }

/* Hidden command */
#define QD_CLI_CMD_HIDDEN(cmd_name, cmd_handler) \
    { .name = cmd_name, .handler = cmd_handler, .hidden = true }

/* Global command (available in all modes) */
#define QD_CLI_CMD_GLOBAL(cmd_name, cmd_handler, cmd_help) \
    { .name = cmd_name, .handler = cmd_handler, .help = cmd_help, .mode_global = true }

/* End of command array */
#define QD_CLI_CMD_END() { .name = NULL }

/*
 * ====================
 * Context Helpers
 * ====================
 */

/* Get argument by index */
static inline const char *qd_cli_arg(qd_cli_ctx_t *ctx, int index)
{
    return (index >= 0 && index < ctx->argc) ? ctx->argv[index] : NULL;
}

/* Get argument as int */
static inline int qd_cli_arg_int(qd_cli_ctx_t *ctx, int index, int def)
{
    const char *arg = qd_cli_arg(ctx, index);
    return arg ? atoi(arg) : def;
}

/* Get argument as long long */
static inline long long qd_cli_arg_ll(qd_cli_ctx_t *ctx, int index, long long def)
{
    const char *arg = qd_cli_arg(ctx, index);
    return arg ? atoll(arg) : def;
}

/* Check if argument exists */
static inline bool qd_cli_has_arg(qd_cli_ctx_t *ctx, int index)
{
    return index >= 0 && index < ctx->argc;
}

/* Print help for current command */
void qd_cli_print_cmd_help(qd_cli_ctx_t *ctx);

/* Print help for mode */
void qd_cli_print_mode_help(qd_cli_mode_t *mode);

/* Print help for all modes */
void qd_cli_print_all_help(qd_cli_shell_t *shell);

#ifdef __cplusplus
}
#endif

#endif /* QD_CLI_H */
