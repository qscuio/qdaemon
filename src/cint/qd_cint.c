/*
 * QD-CINT Main Implementation
 * Clean-room C Interpreter
 */

#include "qd_cint.h"
#include "qd_cint_parser.h"
#include "qd_cint_eval.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>

/*
 * Internal interpreter structure
 */
struct qd_cint {
    qd_cint_eval_t *eval;
    char *prompt;
    int verbose;
    int interactive;
};

/*
 * Create interpreter
 */
qd_cint_t *qd_cint_create(void)
{
    qd_cint_t *cint = calloc(1, sizeof(qd_cint_t));
    cint->eval = qd_cint_eval_create();
    cint->prompt = strdup("cint> ");
    return cint;
}

/*
 * Destroy interpreter
 */
void qd_cint_destroy(qd_cint_t *cint)
{
    if (!cint) return;
    
    if (cint->eval)
        qd_cint_eval_destroy(cint->eval);
    if (cint->prompt)
        free(cint->prompt);
    free(cint);
}

/*
 * Evaluate code string
 */
int qd_cint_eval(qd_cint_t *cint, const char *code)
{
    if (!cint || !code) return -1;
    
    qd_cint_parser_t parser;
    qd_cint_parser_init(&parser, code, 0);
    
    /* Try to parse as statement */
    qd_cint_ast_t *ast = qd_cint_parse_statement(&parser);
    
    if (qd_cint_parser_had_error(&parser)) {
        fprintf(stderr, "Parse error: %s\n", qd_cint_parser_error(&parser));
        qd_cint_parser_free(&parser);
        return -1;
    }
    
    /* Evaluate */
    qd_cint_value_t result = qd_cint_eval_ast(cint->eval, ast);
    
    /* Print result if it's an expression (not void) */
    if (result.type != VAL_VOID && cint->verbose) {
        printf("= ");
        qd_cint_val_print(&result);
        printf("\n");
    }
    
    qd_cint_val_free(&result);
    qd_cint_ast_free(ast);
    qd_cint_parser_free(&parser);
    
    if (qd_cint_eval_error(cint->eval)) {
        fprintf(stderr, "%s\n", qd_cint_eval_error(cint->eval));
        qd_cint_eval_clear_error(cint->eval);
        return -1;
    }
    
    return 0;
}

/*
 * Execute file
 */
int qd_cint_exec_file(qd_cint_t *cint, const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open file: %s\n", filename);
        return -1;
    }
    
    /* Read file */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char *code = malloc(size + 1);
    size_t n = fread(code, 1, size, fp);
    if (n != (size_t)size) {
        fprintf(stderr, "Error reading file\n");
        free(code);
        fclose(fp);
        return -1;
    }
    code[size] = '\0';
    fclose(fp);
    
    /* Parse as program */
    qd_cint_parser_t parser;
    qd_cint_parser_init(&parser, code, size);
    
    qd_cint_ast_t *ast = qd_cint_parse_program(&parser);
    
    if (qd_cint_parser_had_error(&parser)) {
        fprintf(stderr, "Parse error: %s\n", qd_cint_parser_error(&parser));
        qd_cint_parser_free(&parser);
        free(code);
        return -1;
    }
    
    /* Evaluate */
    qd_cint_eval_ast(cint->eval, ast);
    
    qd_cint_ast_free(ast);
    qd_cint_parser_free(&parser);
    free(code);
    
    if (qd_cint_eval_error(cint->eval)) {
        fprintf(stderr, "%s\n", qd_cint_eval_error(cint->eval));
        qd_cint_eval_clear_error(cint->eval);
        return -1;
    }
    
    return 0;
}

/*
 * REPL
 */
int qd_cint_repl(qd_cint_t *cint, const char *prompt)
{
    if (prompt) {
        free(cint->prompt);
        cint->prompt = strdup(prompt);
    }
    
    cint->interactive = 1;
    cint->verbose = 1;
    
    printf("QD-CINT C Interpreter\n");
    printf("Type 'exit' to quit, 'help' for commands\n\n");
    
    char *line;
    while ((line = readline(cint->prompt)) != NULL) {
        if (line[0] == '\0') {
            free(line);
            continue;
        }
        
        add_history(line);
        
        /* Built-in commands */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            free(line);
            break;
        }
        
        if (strcmp(line, "help") == 0) {
            printf("Commands:\n");
            printf("  exit, quit    - Exit interpreter\n");
            printf("  help          - Show this help\n");
            printf("  vars          - Show variables\n");
            printf("\nC Syntax:\n");
            printf("  int x = 10;   - Declare variable\n");
            printf("  print(x);     - Print value\n");
            printf("  x + y * z;    - Expressions\n");
            printf("  func(a, b);   - Function calls\n");
            free(line);
            continue;
        }
        
        if (strcmp(line, "vars") == 0) {
            printf("(Variable listing not implemented)\n");
            free(line);
            continue;
        }
        
        /* Evaluate code */
        qd_cint_eval(cint, line);
        
        free(line);
    }
    
    printf("\n");
    return 0;
}

/*
 * Register native function
 */
int qd_cint_register_native(qd_cint_t *cint, const char *name, void *func_ptr,
                            int num_args)
{
    return qd_cint_eval_register_native(cint->eval, name, func_ptr, 
                                         num_args, VAL_INT);
}

/*
 * Get error
 */
const char *qd_cint_error(qd_cint_t *cint)
{
    return qd_cint_eval_error(cint->eval);
}

/*
 * Set verbose
 */
void qd_cint_set_verbose(qd_cint_t *cint, int verbose)
{
    cint->verbose = verbose;
}
