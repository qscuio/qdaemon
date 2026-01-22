/*
 * QD-CINT Parser - Recursive Descent Parser
 * Clean-room implementation
 */

#ifndef QD_CINT_PARSER_H
#define QD_CINT_PARSER_H

#include "qd_cint_lexer.h"
#include "qd_cint_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parser state
 */
typedef struct qd_cint_parser {
    qd_cint_lexer_t lex;
    qd_cint_token_t current;
    qd_cint_token_t prev;
    char *error;
    int error_line;
    int error_col;
    int had_error;
} qd_cint_parser_t;

/*
 * Parser API
 */

/* Initialize parser with source code */
void qd_cint_parser_init(qd_cint_parser_t *parser, const char *src, size_t len);

/* Parse a complete program (list of top-level declarations) */
qd_cint_ast_t *qd_cint_parse_program(qd_cint_parser_t *parser);

/* Parse a single statement (for REPL) */
qd_cint_ast_t *qd_cint_parse_statement(qd_cint_parser_t *parser);

/* Parse a single expression (for evaluation) */
qd_cint_ast_t *qd_cint_parse_expression(qd_cint_parser_t *parser);

/* Check if parser had an error */
int qd_cint_parser_had_error(qd_cint_parser_t *parser);

/* Get error message */
const char *qd_cint_parser_error(qd_cint_parser_t *parser);

/* Free parser resources */
void qd_cint_parser_free(qd_cint_parser_t *parser);

#ifdef __cplusplus
}
#endif

#endif /* QD_CINT_PARSER_H */
