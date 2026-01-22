/*
 * QD-CINT Parser Implementation
 * Clean-room recursive descent parser
 */

#include "qd_cint_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Helper macros
 */
#define CURRENT(p)  ((p)->current)
#define PREV(p)     ((p)->prev)
#define CHECK(p, t) ((p)->current.type == (t))
#define AT_END(p)   ((p)->current.type == TOK_EOF)

/*
 * Forward declarations
 */
static qd_cint_ast_t *parse_declaration(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_statement(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_expression(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_assignment(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_or(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_and(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_bitor(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_bitxor(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_bitand(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_equality(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_comparison(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_shift(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_term(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_factor(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_unary(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_postfix(qd_cint_parser_t *p);
static qd_cint_ast_t *parse_primary(qd_cint_parser_t *p);

/*
 * Token helpers
 */

static void advance(qd_cint_parser_t *p)
{
    p->prev = p->current;
    p->current = qd_cint_lexer_next(&p->lex);
}

static int match(qd_cint_parser_t *p, qd_cint_token_type_t type)
{
    if (CHECK(p, type)) {
        advance(p);
        return 1;
    }
    return 0;
}

static void error(qd_cint_parser_t *p, const char *msg)
{
    if (p->had_error) return;
    
    p->had_error = 1;
    p->error_line = p->current.line;
    p->error_col = p->current.col;
    
    char buf[256];
    snprintf(buf, sizeof(buf), "Line %d:%d: %s (got %s)",
             p->current.line, p->current.col, msg,
             qd_cint_token_name(p->current.type));
    p->error = strdup(buf);
}

static int expect(qd_cint_parser_t *p, qd_cint_token_type_t type, const char *msg)
{
    if (CHECK(p, type)) {
        advance(p);
        return 1;
    }
    error(p, msg);
    return 0;
}

/*
 * Type parsing
 */

static qd_cint_typespec_t *parse_typespec(qd_cint_parser_t *p)
{
    if (!qd_cint_token_is_type(CURRENT(p).type) &&
        CURRENT(p).type != TOK_IDENT) {
        error(p, "Expected type");
        return NULL;
    }
    
    /* Build type name */
    char name[128] = {0};
    int is_unsigned = 0;
    
    /* Handle unsigned/signed */
    if (match(p, TOK_UNSIGNED)) {
        is_unsigned = 1;
        strcpy(name, "unsigned ");
    } else if (match(p, TOK_SIGNED)) {
        strcpy(name, "signed ");
    }
    
    /* Base type */
    if (CHECK(p, TOK_VOID)) {
        strcat(name, "void");
        advance(p);
    } else if (CHECK(p, TOK_INT_KW)) {
        strcat(name, "int");
        advance(p);
    } else if (CHECK(p, TOK_CHAR_KW)) {
        strcat(name, "char");
        advance(p);
    } else if (CHECK(p, TOK_SHORT)) {
        strcat(name, "short");
        advance(p);
    } else if (CHECK(p, TOK_LONG)) {
        strcat(name, "long");
        advance(p);
    } else if (CHECK(p, TOK_FLOAT_KW)) {
        strcat(name, "float");
        advance(p);
    } else if (CHECK(p, TOK_DOUBLE)) {
        strcat(name, "double");
        advance(p);
    } else if (CHECK(p, TOK_IDENT)) {
        strcat(name, CURRENT(p).val.s);
        advance(p);
    } else if (is_unsigned) {
        /* "unsigned" alone means "unsigned int" */
        strcat(name, "int");
    } else {
        error(p, "Expected type name");
        return NULL;
    }
    
    qd_cint_typespec_t *ts = qd_cint_typespec_create(name);
    ts->is_unsigned = is_unsigned;
    
    /* Pointers */
    while (match(p, TOK_STAR)) {
        ts->pointer_depth++;
    }
    
    return ts;
}

/*
 * Expression parsing (precedence climbing)
 */

static qd_cint_ast_t *parse_primary(qd_cint_parser_t *p)
{
    int line = CURRENT(p).line;
    int col = CURRENT(p).col;
    
    /* Literals */
    if (match(p, TOK_INT)) {
        return qd_cint_ast_int(PREV(p).val.i, line, col);
    }
    if (match(p, TOK_FLOAT)) {
        return qd_cint_ast_float(PREV(p).val.f, line, col);
    }
    if (match(p, TOK_STRING)) {
        qd_cint_ast_t *ast = qd_cint_ast_string(PREV(p).val.s, line, col);
        free(PREV(p).val.s);
        return ast;
    }
    if (match(p, TOK_CHAR)) {
        return qd_cint_ast_char(PREV(p).val.c, line, col);
    }
    
    /* Identifier */
    if (match(p, TOK_IDENT)) {
        qd_cint_ast_t *ast = qd_cint_ast_ident(PREV(p).val.s, line, col);
        free(PREV(p).val.s);
        return ast;
    }
    
    /* Parenthesized expression */
    if (match(p, TOK_LPAREN)) {
        qd_cint_ast_t *expr = parse_expression(p);
        expect(p, TOK_RPAREN, "Expected ')' after expression");
        return expr;
    }
    
    /* sizeof */
    if (match(p, TOK_SIZEOF)) {
        expect(p, TOK_LPAREN, "Expected '(' after sizeof");
        /* For simplicity, assume sizeof(type) */
        qd_cint_typespec_t *ts = parse_typespec(p);
        expect(p, TOK_RPAREN, "Expected ')' after sizeof");
        
        qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
        ast->type = AST_SIZEOF;
        ast->line = line;
        ast->col = col;
        ast->u.size_of.type = ts;
        return ast;
    }
    
    error(p, "Expected expression");
    return NULL;
}

static qd_cint_ast_t *parse_postfix(qd_cint_parser_t *p)
{
    qd_cint_ast_t *expr = parse_primary(p);
    if (!expr) return NULL;
    
    while (1) {
        int line = CURRENT(p).line;
        int col = CURRENT(p).col;
        
        /* Function call */
        if (match(p, TOK_LPAREN)) {
            qd_cint_ast_t *args[64];
            int num_args = 0;
            
            if (!CHECK(p, TOK_RPAREN)) {
                do {
                    args[num_args++] = parse_assignment(p);
                } while (match(p, TOK_COMMA));
            }
            expect(p, TOK_RPAREN, "Expected ')' after arguments");
            
            expr = qd_cint_ast_call(expr, args, num_args, line, col);
        }
        /* Array index */
        else if (match(p, TOK_LBRACKET)) {
            qd_cint_ast_t *index = parse_expression(p);
            expect(p, TOK_RBRACKET, "Expected ']' after index");
            
            qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
            ast->type = AST_INDEX;
            ast->line = line;
            ast->col = col;
            ast->u.index.array = expr;
            ast->u.index.index = index;
            expr = ast;
        }
        /* Member access */
        else if (match(p, TOK_DOT)) {
            expect(p, TOK_IDENT, "Expected member name");
            qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
            ast->type = AST_MEMBER;
            ast->line = line;
            ast->col = col;
            ast->u.member.object = expr;
            ast->u.member.member = strdup(PREV(p).val.s);
            free(PREV(p).val.s);
            expr = ast;
        }
        /* Pointer member access */
        else if (match(p, TOK_ARROW)) {
            expect(p, TOK_IDENT, "Expected member name");
            qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
            ast->type = AST_PTRMEMBER;
            ast->line = line;
            ast->col = col;
            ast->u.member.object = expr;
            ast->u.member.member = strdup(PREV(p).val.s);
            free(PREV(p).val.s);
            expr = ast;
        }
        /* Post-increment */
        else if (match(p, TOK_INC)) {
            expr = qd_cint_ast_unop(UOP_POST_INC, expr, line, col);
        }
        /* Post-decrement */
        else if (match(p, TOK_DEC)) {
            expr = qd_cint_ast_unop(UOP_POST_DEC, expr, line, col);
        }
        else {
            break;
        }
    }
    
    return expr;
}

static qd_cint_ast_t *parse_unary(qd_cint_parser_t *p)
{
    int line = CURRENT(p).line;
    int col = CURRENT(p).col;
    
    if (match(p, TOK_MINUS)) {
        return qd_cint_ast_unop(UOP_NEG, parse_unary(p), line, col);
    }
    if (match(p, TOK_NOT)) {
        return qd_cint_ast_unop(UOP_NOT, parse_unary(p), line, col);
    }
    if (match(p, TOK_BNOT)) {
        return qd_cint_ast_unop(UOP_BNOT, parse_unary(p), line, col);
    }
    if (match(p, TOK_INC)) {
        return qd_cint_ast_unop(UOP_PRE_INC, parse_unary(p), line, col);
    }
    if (match(p, TOK_DEC)) {
        return qd_cint_ast_unop(UOP_PRE_DEC, parse_unary(p), line, col);
    }
    if (match(p, TOK_STAR)) {
        qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
        ast->type = AST_DEREF;
        ast->line = line;
        ast->col = col;
        ast->u.unop.operand = parse_unary(p);
        return ast;
    }
    if (match(p, TOK_BAND)) {
        qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
        ast->type = AST_ADDR;
        ast->line = line;
        ast->col = col;
        ast->u.unop.operand = parse_unary(p);
        return ast;
    }
    
    return parse_postfix(p);
}

static qd_cint_ast_t *parse_factor(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_unary(p);
    
    while (CHECK(p, TOK_STAR) || CHECK(p, TOK_SLASH) || CHECK(p, TOK_PERCENT)) {
        int line = CURRENT(p).line;
        int col = CURRENT(p).col;
        qd_cint_binop_t op;
        
        if (match(p, TOK_STAR)) op = OP_MUL;
        else if (match(p, TOK_SLASH)) op = OP_DIV;
        else { match(p, TOK_PERCENT); op = OP_MOD; }
        
        qd_cint_ast_t *right = parse_unary(p);
        left = qd_cint_ast_binop(op, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_term(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_factor(p);
    
    while (CHECK(p, TOK_PLUS) || CHECK(p, TOK_MINUS)) {
        int line = CURRENT(p).line;
        int col = CURRENT(p).col;
        qd_cint_binop_t op = match(p, TOK_PLUS) ? OP_ADD : (match(p, TOK_MINUS), OP_SUB);
        
        qd_cint_ast_t *right = parse_factor(p);
        left = qd_cint_ast_binop(op, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_shift(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_term(p);
    
    while (CHECK(p, TOK_LSHIFT) || CHECK(p, TOK_RSHIFT)) {
        int line = CURRENT(p).line;
        int col = CURRENT(p).col;
        qd_cint_binop_t op = match(p, TOK_LSHIFT) ? OP_LSHIFT : (match(p, TOK_RSHIFT), OP_RSHIFT);
        
        qd_cint_ast_t *right = parse_term(p);
        left = qd_cint_ast_binop(op, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_comparison(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_shift(p);
    
    while (CHECK(p, TOK_LT) || CHECK(p, TOK_GT) || CHECK(p, TOK_LE) || CHECK(p, TOK_GE)) {
        int line = CURRENT(p).line;
        int col = CURRENT(p).col;
        qd_cint_binop_t op;
        
        if (match(p, TOK_LT)) op = OP_LT;
        else if (match(p, TOK_GT)) op = OP_GT;
        else if (match(p, TOK_LE)) op = OP_LE;
        else { match(p, TOK_GE); op = OP_GE; }
        
        qd_cint_ast_t *right = parse_shift(p);
        left = qd_cint_ast_binop(op, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_equality(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_comparison(p);
    
    while (CHECK(p, TOK_EQ) || CHECK(p, TOK_NE)) {
        int line = CURRENT(p).line;
        int col = CURRENT(p).col;
        qd_cint_binop_t op = match(p, TOK_EQ) ? OP_EQ : (match(p, TOK_NE), OP_NE);
        
        qd_cint_ast_t *right = parse_comparison(p);
        left = qd_cint_ast_binop(op, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_bitand(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_equality(p);
    
    while (match(p, TOK_BAND)) {
        int line = PREV(p).line;
        int col = PREV(p).col;
        qd_cint_ast_t *right = parse_equality(p);
        left = qd_cint_ast_binop(OP_BAND, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_bitxor(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_bitand(p);
    
    while (match(p, TOK_BXOR)) {
        int line = PREV(p).line;
        int col = PREV(p).col;
        qd_cint_ast_t *right = parse_bitand(p);
        left = qd_cint_ast_binop(OP_BXOR, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_bitor(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_bitxor(p);
    
    while (match(p, TOK_BOR)) {
        int line = PREV(p).line;
        int col = PREV(p).col;
        qd_cint_ast_t *right = parse_bitxor(p);
        left = qd_cint_ast_binop(OP_BOR, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_and(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_bitor(p);
    
    while (match(p, TOK_AND)) {
        int line = PREV(p).line;
        int col = PREV(p).col;
        qd_cint_ast_t *right = parse_bitor(p);
        left = qd_cint_ast_binop(OP_AND, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_or(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_and(p);
    
    while (match(p, TOK_OR)) {
        int line = PREV(p).line;
        int col = PREV(p).col;
        qd_cint_ast_t *right = parse_and(p);
        left = qd_cint_ast_binop(OP_OR, left, right, line, col);
    }
    
    return left;
}

static qd_cint_ast_t *parse_ternary(qd_cint_parser_t *p)
{
    qd_cint_ast_t *cond = parse_or(p);
    
    if (match(p, TOK_QUESTION)) {
        int line = PREV(p).line;
        int col = PREV(p).col;
        qd_cint_ast_t *then_expr = parse_expression(p);
        expect(p, TOK_COLON, "Expected ':' in ternary expression");
        qd_cint_ast_t *else_expr = parse_ternary(p);
        
        qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
        ast->type = AST_TERNARY;
        ast->line = line;
        ast->col = col;
        ast->u.ternary.cond = cond;
        ast->u.ternary.then_expr = then_expr;
        ast->u.ternary.else_expr = else_expr;
        return ast;
    }
    
    return cond;
}

static qd_cint_ast_t *parse_assignment(qd_cint_parser_t *p)
{
    qd_cint_ast_t *left = parse_ternary(p);
    
    if (CHECK(p, TOK_ASSIGN) || CHECK(p, TOK_PLUS_EQ) || CHECK(p, TOK_MINUS_EQ) ||
        CHECK(p, TOK_STAR_EQ) || CHECK(p, TOK_SLASH_EQ)) {
        int line = CURRENT(p).line;
        int col = CURRENT(p).col;
        qd_cint_binop_t op = OP_ASSIGN;
        
        if (match(p, TOK_PLUS_EQ)) op = OP_ADD_ASSIGN;
        else if (match(p, TOK_MINUS_EQ)) op = OP_SUB_ASSIGN;
        else if (match(p, TOK_STAR_EQ)) op = OP_MUL_ASSIGN;
        else if (match(p, TOK_SLASH_EQ)) op = OP_DIV_ASSIGN;
        else advance(p);  /* TOK_ASSIGN */
        
        qd_cint_ast_t *right = parse_assignment(p);
        
        qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
        ast->type = AST_ASSIGN;
        ast->line = line;
        ast->col = col;
        ast->u.assign.target = left;
        ast->u.assign.value = right;
        ast->u.assign.compound_op = op;
        return ast;
    }
    
    return left;
}

static qd_cint_ast_t *parse_expression(qd_cint_parser_t *p)
{
    return parse_assignment(p);
}

/*
 * Statement parsing
 */

static qd_cint_ast_t *parse_block(qd_cint_parser_t *p)
{
    int line = CURRENT(p).line;
    int col = CURRENT(p).col;
    
    expect(p, TOK_LBRACE, "Expected '{'");
    
    qd_cint_ast_t *stmts[256];
    int num_stmts = 0;
    
    while (!CHECK(p, TOK_RBRACE) && !AT_END(p)) {
        stmts[num_stmts++] = parse_statement(p);
    }
    
    expect(p, TOK_RBRACE, "Expected '}'");
    
    return qd_cint_ast_block(stmts, num_stmts, line, col);
}

static qd_cint_ast_t *parse_statement(qd_cint_parser_t *p)
{
    int line = CURRENT(p).line;
    int col = CURRENT(p).col;
    
    /* Block */
    if (CHECK(p, TOK_LBRACE)) {
        return parse_block(p);
    }
    
    /* If statement */
    if (match(p, TOK_IF)) {
        expect(p, TOK_LPAREN, "Expected '(' after 'if'");
        qd_cint_ast_t *cond = parse_expression(p);
        expect(p, TOK_RPAREN, "Expected ')' after condition");
        
        qd_cint_ast_t *then_body = parse_statement(p);
        qd_cint_ast_t *else_body = NULL;
        
        if (match(p, TOK_ELSE)) {
            else_body = parse_statement(p);
        }
        
        return qd_cint_ast_if(cond, then_body, else_body, line, col);
    }
    
    /* While statement */
    if (match(p, TOK_WHILE)) {
        expect(p, TOK_LPAREN, "Expected '(' after 'while'");
        qd_cint_ast_t *cond = parse_expression(p);
        expect(p, TOK_RPAREN, "Expected ')' after condition");
        
        qd_cint_ast_t *body = parse_statement(p);
        return qd_cint_ast_while(cond, body, line, col);
    }
    
    /* For statement */
    if (match(p, TOK_FOR)) {
        expect(p, TOK_LPAREN, "Expected '(' after 'for'");
        
        qd_cint_ast_t *init = NULL;
        if (!CHECK(p, TOK_SEMICOLON)) {
            init = parse_expression(p);
        }
        expect(p, TOK_SEMICOLON, "Expected ';' after for init");
        
        qd_cint_ast_t *cond = NULL;
        if (!CHECK(p, TOK_SEMICOLON)) {
            cond = parse_expression(p);
        }
        expect(p, TOK_SEMICOLON, "Expected ';' after for condition");
        
        qd_cint_ast_t *incr = NULL;
        if (!CHECK(p, TOK_RPAREN)) {
            incr = parse_expression(p);
        }
        expect(p, TOK_RPAREN, "Expected ')' after for clauses");
        
        qd_cint_ast_t *body = parse_statement(p);
        return qd_cint_ast_for(init, cond, incr, body, line, col);
    }
    
    /* Return statement */
    if (match(p, TOK_RETURN)) {
        qd_cint_ast_t *value = NULL;
        if (!CHECK(p, TOK_SEMICOLON)) {
            value = parse_expression(p);
        }
        expect(p, TOK_SEMICOLON, "Expected ';' after return");
        return qd_cint_ast_return(value, line, col);
    }
    
    /* Break */
    if (match(p, TOK_BREAK)) {
        expect(p, TOK_SEMICOLON, "Expected ';' after break");
        qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
        ast->type = AST_BREAK;
        ast->line = line;
        ast->col = col;
        return ast;
    }
    
    /* Continue */
    if (match(p, TOK_CONTINUE)) {
        expect(p, TOK_SEMICOLON, "Expected ';' after continue");
        qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
        ast->type = AST_CONTINUE;
        ast->line = line;
        ast->col = col;
        return ast;
    }
    
    /* Variable declaration or expression statement */
    if (qd_cint_token_is_type(CURRENT(p).type)) {
        return parse_declaration(p);
    }
    
    /* Expression statement */
    qd_cint_ast_t *expr = parse_expression(p);
    expect(p, TOK_SEMICOLON, "Expected ';' after expression");
    
    qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
    ast->type = AST_EXPR_STMT;
    ast->line = line;
    ast->col = col;
    ast->u.expr_stmt.expr = expr;
    return ast;
}

/*
 * Declaration parsing
 */

static qd_cint_ast_t *parse_declaration(qd_cint_parser_t *p)
{
    int line = CURRENT(p).line;
    int col = CURRENT(p).col;
    
    qd_cint_typespec_t *type = parse_typespec(p);
    if (!type) return NULL;
    
    expect(p, TOK_IDENT, "Expected identifier");
    char *name = strdup(PREV(p).val.s);
    free(PREV(p).val.s);
    
    /* Function declaration */
    if (match(p, TOK_LPAREN)) {
        qd_cint_ast_t *params[16];
        int num_params = 0;
        
        if (!CHECK(p, TOK_RPAREN)) {
            do {
                qd_cint_typespec_t *param_type = parse_typespec(p);
                char *param_name = NULL;
                if (CHECK(p, TOK_IDENT)) {
                    param_name = strdup(CURRENT(p).val.s);
                    advance(p);
                }
                
                qd_cint_ast_t *param = calloc(1, sizeof(qd_cint_ast_t));
                param->type = AST_PARAM;
                param->u.param.type = param_type;
                param->u.param.name = param_name;
                params[num_params++] = param;
            } while (match(p, TOK_COMMA));
        }
        expect(p, TOK_RPAREN, "Expected ')' after parameters");
        
        qd_cint_ast_t *body = NULL;
        if (CHECK(p, TOK_LBRACE)) {
            body = parse_block(p);
        } else {
            expect(p, TOK_SEMICOLON, "Expected ';' or function body");
        }
        
        return qd_cint_ast_funcdecl(type, name, params, num_params, body, line, col);
    }
    
    /* Variable declaration */
    qd_cint_ast_t *init = NULL;
    if (match(p, TOK_ASSIGN)) {
        init = parse_expression(p);
    }
    expect(p, TOK_SEMICOLON, "Expected ';' after variable declaration");
    
    return qd_cint_ast_vardecl(type, name, init, line, col);
}

/*
 * Public API
 */

void qd_cint_parser_init(qd_cint_parser_t *parser, const char *src, size_t len)
{
    memset(parser, 0, sizeof(*parser));
    qd_cint_lexer_init(&parser->lex, src, len);
    parser->current = qd_cint_lexer_next(&parser->lex);
}

qd_cint_ast_t *qd_cint_parse_program(qd_cint_parser_t *parser)
{
    qd_cint_ast_t *stmts[256];
    int num_stmts = 0;
    
    while (!AT_END(parser) && !parser->had_error) {
        stmts[num_stmts++] = parse_declaration(parser);
    }
    
    if (parser->had_error) return NULL;
    
    return qd_cint_ast_block(stmts, num_stmts, 1, 1);
}

qd_cint_ast_t *qd_cint_parse_statement(qd_cint_parser_t *parser)
{
    return parse_statement(parser);
}

qd_cint_ast_t *qd_cint_parse_expression(qd_cint_parser_t *parser)
{
    return parse_expression(parser);
}

int qd_cint_parser_had_error(qd_cint_parser_t *parser)
{
    return parser->had_error;
}

const char *qd_cint_parser_error(qd_cint_parser_t *parser)
{
    return parser->error;
}

void qd_cint_parser_free(qd_cint_parser_t *parser)
{
    if (parser->error) {
        free(parser->error);
        parser->error = NULL;
    }
}
