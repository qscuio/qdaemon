/*
 * QD-CINT AST Implementation
 * Clean-room AST construction and manipulation
 */

#include "qd_cint_ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Helper for allocation
 */
static qd_cint_ast_t *ast_alloc(qd_cint_ast_type_t type, int line, int col)
{
    qd_cint_ast_t *ast = calloc(1, sizeof(qd_cint_ast_t));
    ast->type = type;
    ast->line = line;
    ast->col = col;
    return ast;
}

/*
 * Literal constructors
 */

qd_cint_ast_t *qd_cint_ast_int(int64_t val, int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_INT, line, col);
    ast->u.int_val = val;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_float(double val, int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_FLOAT, line, col);
    ast->u.float_val = val;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_string(const char *val, int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_STRING, line, col);
    ast->u.str_val = strdup(val);
    return ast;
}

qd_cint_ast_t *qd_cint_ast_char(char val, int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_CHAR, line, col);
    ast->u.char_val = val;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_ident(const char *name, int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_IDENT, line, col);
    ast->u.ident = strdup(name);
    return ast;
}

/*
 * Expression constructors
 */

qd_cint_ast_t *qd_cint_ast_binop(qd_cint_binop_t op,
                                  qd_cint_ast_t *left,
                                  qd_cint_ast_t *right,
                                  int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_BINOP, line, col);
    ast->u.binop.op = op;
    ast->u.binop.left = left;
    ast->u.binop.right = right;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_unop(qd_cint_unop_t op,
                                 qd_cint_ast_t *operand,
                                 int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_UNOP, line, col);
    ast->u.unop.op = op;
    ast->u.unop.operand = operand;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_call(qd_cint_ast_t *func,
                                 qd_cint_ast_t **args, int num_args,
                                 int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_CALL, line, col);
    ast->u.call.func = func;
    ast->u.call.num_args = num_args;
    if (num_args > 0) {
        ast->u.call.args = malloc(sizeof(qd_cint_ast_t *) * num_args);
        memcpy(ast->u.call.args, args, sizeof(qd_cint_ast_t *) * num_args);
    }
    return ast;
}

/*
 * Statement constructors
 */

qd_cint_ast_t *qd_cint_ast_if(qd_cint_ast_t *cond,
                               qd_cint_ast_t *then_body,
                               qd_cint_ast_t *else_body,
                               int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_IF, line, col);
    ast->u.if_stmt.cond = cond;
    ast->u.if_stmt.then_body = then_body;
    ast->u.if_stmt.else_body = else_body;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_while(qd_cint_ast_t *cond,
                                  qd_cint_ast_t *body,
                                  int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_WHILE, line, col);
    ast->u.while_stmt.cond = cond;
    ast->u.while_stmt.body = body;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_for(qd_cint_ast_t *init,
                                qd_cint_ast_t *cond,
                                qd_cint_ast_t *incr,
                                qd_cint_ast_t *body,
                                int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_FOR, line, col);
    ast->u.for_stmt.init = init;
    ast->u.for_stmt.cond = cond;
    ast->u.for_stmt.incr = incr;
    ast->u.for_stmt.body = body;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_block(qd_cint_ast_t **stmts, int num_stmts,
                                  int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_BLOCK, line, col);
    ast->u.block.num_stmts = num_stmts;
    if (num_stmts > 0) {
        ast->u.block.stmts = malloc(sizeof(qd_cint_ast_t *) * num_stmts);
        memcpy(ast->u.block.stmts, stmts, sizeof(qd_cint_ast_t *) * num_stmts);
    }
    return ast;
}

qd_cint_ast_t *qd_cint_ast_return(qd_cint_ast_t *value, int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_RETURN, line, col);
    ast->u.ret.value = value;
    return ast;
}

/*
 * Declaration constructors
 */

qd_cint_ast_t *qd_cint_ast_vardecl(qd_cint_typespec_t *type,
                                    const char *name,
                                    qd_cint_ast_t *init,
                                    int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_VARDECL, line, col);
    ast->u.vardecl.type = type;
    ast->u.vardecl.name = strdup(name);
    ast->u.vardecl.init = init;
    return ast;
}

qd_cint_ast_t *qd_cint_ast_funcdecl(qd_cint_typespec_t *ret_type,
                                     const char *name,
                                     qd_cint_ast_t **params, int num_params,
                                     qd_cint_ast_t *body,
                                     int line, int col)
{
    qd_cint_ast_t *ast = ast_alloc(AST_FUNCDECL, line, col);
    ast->u.funcdecl.return_type = ret_type;
    ast->u.funcdecl.name = strdup(name);
    ast->u.funcdecl.num_params = num_params;
    if (num_params > 0) {
        ast->u.funcdecl.params = malloc(sizeof(qd_cint_ast_t *) * num_params);
        memcpy(ast->u.funcdecl.params, params, sizeof(qd_cint_ast_t *) * num_params);
    }
    ast->u.funcdecl.body = body;
    return ast;
}

/*
 * Type specifier
 */

qd_cint_typespec_t *qd_cint_typespec_create(const char *name)
{
    qd_cint_typespec_t *ts = calloc(1, sizeof(qd_cint_typespec_t));
    ts->name = strdup(name);
    ts->array_size = -1;  /* Not an array by default */
    return ts;
}

void qd_cint_typespec_free(qd_cint_typespec_t *ts)
{
    if (ts) {
        free(ts->name);
        free(ts);
    }
}

/*
 * Free AST
 */

void qd_cint_ast_free(qd_cint_ast_t *ast)
{
    if (!ast) return;
    
    switch (ast->type) {
        case AST_STRING:
            free(ast->u.str_val);
            break;
        case AST_IDENT:
            free(ast->u.ident);
            break;
        case AST_BINOP:
            qd_cint_ast_free(ast->u.binop.left);
            qd_cint_ast_free(ast->u.binop.right);
            break;
        case AST_UNOP:
            qd_cint_ast_free(ast->u.unop.operand);
            break;
        case AST_CALL:
            qd_cint_ast_free(ast->u.call.func);
            for (int i = 0; i < ast->u.call.num_args; i++)
                qd_cint_ast_free(ast->u.call.args[i]);
            free(ast->u.call.args);
            break;
        case AST_BLOCK:
            for (int i = 0; i < ast->u.block.num_stmts; i++)
                qd_cint_ast_free(ast->u.block.stmts[i]);
            free(ast->u.block.stmts);
            break;
        case AST_IF:
            qd_cint_ast_free(ast->u.if_stmt.cond);
            qd_cint_ast_free(ast->u.if_stmt.then_body);
            qd_cint_ast_free(ast->u.if_stmt.else_body);
            break;
        case AST_WHILE:
        case AST_DOWHILE:
            qd_cint_ast_free(ast->u.while_stmt.cond);
            qd_cint_ast_free(ast->u.while_stmt.body);
            break;
        case AST_FOR:
            qd_cint_ast_free(ast->u.for_stmt.init);
            qd_cint_ast_free(ast->u.for_stmt.cond);
            qd_cint_ast_free(ast->u.for_stmt.incr);
            qd_cint_ast_free(ast->u.for_stmt.body);
            break;
        case AST_RETURN:
            qd_cint_ast_free(ast->u.ret.value);
            break;
        case AST_VARDECL:
            qd_cint_typespec_free(ast->u.vardecl.type);
            free(ast->u.vardecl.name);
            qd_cint_ast_free(ast->u.vardecl.init);
            break;
        case AST_FUNCDECL:
            qd_cint_typespec_free(ast->u.funcdecl.return_type);
            free(ast->u.funcdecl.name);
            for (int i = 0; i < ast->u.funcdecl.num_params; i++)
                qd_cint_ast_free(ast->u.funcdecl.params[i]);
            free(ast->u.funcdecl.params);
            qd_cint_ast_free(ast->u.funcdecl.body);
            break;
        default:
            break;
    }
    
    free(ast);
}

/*
 * Operator names
 */

static const char *binop_names[] = {
    [OP_ADD] = "+", [OP_SUB] = "-", [OP_MUL] = "*", [OP_DIV] = "/", [OP_MOD] = "%",
    [OP_EQ] = "==", [OP_NE] = "!=", [OP_LT] = "<", [OP_GT] = ">",
    [OP_LE] = "<=", [OP_GE] = ">=",
    [OP_AND] = "&&", [OP_OR] = "||",
    [OP_BAND] = "&", [OP_BOR] = "|", [OP_BXOR] = "^",
    [OP_LSHIFT] = "<<", [OP_RSHIFT] = ">>",
    [OP_ASSIGN] = "=", [OP_COMMA] = ",",
};

static const char *unop_names[] = {
    [UOP_NEG] = "-", [UOP_NOT] = "!", [UOP_BNOT] = "~",
    [UOP_PRE_INC] = "++", [UOP_PRE_DEC] = "--",
    [UOP_POST_INC] = "++", [UOP_POST_DEC] = "--",
};

const char *qd_cint_binop_name(qd_cint_binop_t op)
{
    return binop_names[op];
}

const char *qd_cint_unop_name(qd_cint_unop_t op)
{
    return unop_names[op];
}

/*
 * Debug print
 */

static void print_indent(int indent)
{
    for (int i = 0; i < indent; i++)
        printf("  ");
}

void qd_cint_ast_print(qd_cint_ast_t *ast, int indent)
{
    if (!ast) {
        print_indent(indent);
        printf("(null)\n");
        return;
    }
    
    print_indent(indent);
    
    switch (ast->type) {
        case AST_INT:
            printf("Int(%lld)\n", (long long)ast->u.int_val);
            break;
        case AST_FLOAT:
            printf("Float(%g)\n", ast->u.float_val);
            break;
        case AST_STRING:
            printf("String(\"%s\")\n", ast->u.str_val);
            break;
        case AST_CHAR:
            printf("Char('%c')\n", ast->u.char_val);
            break;
        case AST_IDENT:
            printf("Ident(%s)\n", ast->u.ident);
            break;
        case AST_BINOP:
            printf("BinOp(%s)\n", binop_names[ast->u.binop.op]);
            qd_cint_ast_print(ast->u.binop.left, indent + 1);
            qd_cint_ast_print(ast->u.binop.right, indent + 1);
            break;
        case AST_UNOP:
            printf("UnOp(%s)\n", unop_names[ast->u.unop.op]);
            qd_cint_ast_print(ast->u.unop.operand, indent + 1);
            break;
        case AST_CALL:
            printf("Call\n");
            qd_cint_ast_print(ast->u.call.func, indent + 1);
            for (int i = 0; i < ast->u.call.num_args; i++)
                qd_cint_ast_print(ast->u.call.args[i], indent + 1);
            break;
        case AST_BLOCK:
            printf("Block\n");
            for (int i = 0; i < ast->u.block.num_stmts; i++)
                qd_cint_ast_print(ast->u.block.stmts[i], indent + 1);
            break;
        case AST_IF:
            printf("If\n");
            print_indent(indent + 1); printf("cond:\n");
            qd_cint_ast_print(ast->u.if_stmt.cond, indent + 2);
            print_indent(indent + 1); printf("then:\n");
            qd_cint_ast_print(ast->u.if_stmt.then_body, indent + 2);
            if (ast->u.if_stmt.else_body) {
                print_indent(indent + 1); printf("else:\n");
                qd_cint_ast_print(ast->u.if_stmt.else_body, indent + 2);
            }
            break;
        case AST_WHILE:
            printf("While\n");
            qd_cint_ast_print(ast->u.while_stmt.cond, indent + 1);
            qd_cint_ast_print(ast->u.while_stmt.body, indent + 1);
            break;
        case AST_FOR:
            printf("For\n");
            qd_cint_ast_print(ast->u.for_stmt.init, indent + 1);
            qd_cint_ast_print(ast->u.for_stmt.cond, indent + 1);
            qd_cint_ast_print(ast->u.for_stmt.incr, indent + 1);
            qd_cint_ast_print(ast->u.for_stmt.body, indent + 1);
            break;
        case AST_RETURN:
            printf("Return\n");
            if (ast->u.ret.value)
                qd_cint_ast_print(ast->u.ret.value, indent + 1);
            break;
        case AST_VARDECL:
            printf("VarDecl(%s %s)\n", 
                   ast->u.vardecl.type->name, ast->u.vardecl.name);
            if (ast->u.vardecl.init)
                qd_cint_ast_print(ast->u.vardecl.init, indent + 1);
            break;
        case AST_FUNCDECL:
            printf("FuncDecl(%s %s)\n",
                   ast->u.funcdecl.return_type->name, ast->u.funcdecl.name);
            if (ast->u.funcdecl.body)
                qd_cint_ast_print(ast->u.funcdecl.body, indent + 1);
            break;
        default:
            printf("AST(%d)\n", ast->type);
            break;
    }
}
