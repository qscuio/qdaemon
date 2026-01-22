/*
 * QD-CINT Evaluator Implementation
 * Tree-walking interpreter for C subset
 */

#include "qd_cint_eval.h"
#include "qd_cint_parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Scope management
 */

static qd_cint_scope_t *scope_create(qd_cint_scope_t *parent)
{
    qd_cint_scope_t *s = calloc(1, sizeof(qd_cint_scope_t));
    s->parent = parent;
    return s;
}

static void scope_destroy(qd_cint_scope_t *s)
{
    qd_cint_var_t *v = s->vars;
    while (v) {
        qd_cint_var_t *next = v->next;
        free(v->name);
        qd_cint_val_free(&v->value);
        if (v->type) qd_cint_typespec_free(v->type);
        free(v);
        v = next;
    }
    free(s);
}

static qd_cint_var_t *scope_find(qd_cint_scope_t *s, const char *name)
{
    while (s) {
        for (qd_cint_var_t *v = s->vars; v; v = v->next) {
            if (strcmp(v->name, name) == 0)
                return v;
        }
        s = s->parent;
    }
    return NULL;
}

static qd_cint_var_t *scope_define(qd_cint_scope_t *s, const char *name)
{
    qd_cint_var_t *v = calloc(1, sizeof(qd_cint_var_t));
    v->name = strdup(name);
    v->next = s->vars;
    s->vars = v;
    return v;
}

/*
 * Value constructors
 */

qd_cint_value_t qd_cint_val_void(void)
{
    return (qd_cint_value_t){ .type = VAL_VOID };
}

qd_cint_value_t qd_cint_val_int(int64_t v)
{
    return (qd_cint_value_t){ .type = VAL_INT, .u.i = v };
}

qd_cint_value_t qd_cint_val_float(double v)
{
    return (qd_cint_value_t){ .type = VAL_FLOAT, .u.f = v };
}

qd_cint_value_t qd_cint_val_string(const char *s)
{
    return (qd_cint_value_t){ .type = VAL_STRING, .u.s = strdup(s) };
}

qd_cint_value_t qd_cint_val_char(char c)
{
    return (qd_cint_value_t){ .type = VAL_CHAR, .u.c = c };
}

/*
 * Value conversions
 */

int64_t qd_cint_val_to_int(qd_cint_value_t *v)
{
    switch (v->type) {
        case VAL_INT: return v->u.i;
        case VAL_FLOAT: return (int64_t)v->u.f;
        case VAL_CHAR: return v->u.c;
        default: return 0;
    }
}

double qd_cint_val_to_float(qd_cint_value_t *v)
{
    switch (v->type) {
        case VAL_INT: return (double)v->u.i;
        case VAL_FLOAT: return v->u.f;
        case VAL_CHAR: return v->u.c;
        default: return 0.0;
    }
}

int qd_cint_val_to_bool(qd_cint_value_t *v)
{
    switch (v->type) {
        case VAL_INT: return v->u.i != 0;
        case VAL_FLOAT: return v->u.f != 0.0;
        case VAL_CHAR: return v->u.c != 0;
        case VAL_STRING: return v->u.s != NULL && v->u.s[0] != 0;
        case VAL_PTR: return v->u.ptr != NULL;
        default: return 0;
    }
}

void qd_cint_val_print(qd_cint_value_t *v)
{
    switch (v->type) {
        case VAL_VOID: printf("void"); break;
        case VAL_INT: printf("%lld", (long long)v->u.i); break;
        case VAL_FLOAT: printf("%g", v->u.f); break;
        case VAL_STRING: printf("%s", v->u.s); break;
        case VAL_CHAR: printf("%c", v->u.c); break;
        case VAL_PTR: printf("0x%p", v->u.ptr); break;
        default: printf("?"); break;
    }
}

void qd_cint_val_free(qd_cint_value_t *v)
{
    if (v->type == VAL_STRING && v->u.s) {
        free(v->u.s);
        v->u.s = NULL;
    }
}

/*
 * Error handling
 */

static void eval_error(qd_cint_eval_t *e, int line, const char *msg)
{
    if (e->error) free(e->error);
    char buf[256];
    snprintf(buf, sizeof(buf), "Runtime error at line %d: %s", line, msg);
    e->error = strdup(buf);
    e->error_line = line;
}

/*
 * Forward declaration
 */
static qd_cint_value_t eval_node(qd_cint_eval_t *e, qd_cint_ast_t *ast);

/*
 * Expression evaluation
 */

static qd_cint_value_t eval_binop(qd_cint_eval_t *e, qd_cint_ast_t *ast)
{
    qd_cint_value_t left = eval_node(e, ast->u.binop.left);
    qd_cint_value_t right = eval_node(e, ast->u.binop.right);
    
    /* Promote to float if either is float */
    int is_float = (left.type == VAL_FLOAT || right.type == VAL_FLOAT);
    
    if (is_float) {
        double l = qd_cint_val_to_float(&left);
        double r = qd_cint_val_to_float(&right);
        
        switch (ast->u.binop.op) {
            case OP_ADD: return qd_cint_val_float(l + r);
            case OP_SUB: return qd_cint_val_float(l - r);
            case OP_MUL: return qd_cint_val_float(l * r);
            case OP_DIV: return qd_cint_val_float(l / r);
            case OP_EQ: return qd_cint_val_int(l == r);
            case OP_NE: return qd_cint_val_int(l != r);
            case OP_LT: return qd_cint_val_int(l < r);
            case OP_GT: return qd_cint_val_int(l > r);
            case OP_LE: return qd_cint_val_int(l <= r);
            case OP_GE: return qd_cint_val_int(l >= r);
            default: break;
        }
    } else {
        int64_t l = qd_cint_val_to_int(&left);
        int64_t r = qd_cint_val_to_int(&right);
        
        switch (ast->u.binop.op) {
            case OP_ADD: return qd_cint_val_int(l + r);
            case OP_SUB: return qd_cint_val_int(l - r);
            case OP_MUL: return qd_cint_val_int(l * r);
            case OP_DIV: return qd_cint_val_int(r ? l / r : 0);
            case OP_MOD: return qd_cint_val_int(r ? l % r : 0);
            case OP_EQ: return qd_cint_val_int(l == r);
            case OP_NE: return qd_cint_val_int(l != r);
            case OP_LT: return qd_cint_val_int(l < r);
            case OP_GT: return qd_cint_val_int(l > r);
            case OP_LE: return qd_cint_val_int(l <= r);
            case OP_GE: return qd_cint_val_int(l >= r);
            case OP_AND: return qd_cint_val_int(l && r);
            case OP_OR: return qd_cint_val_int(l || r);
            case OP_BAND: return qd_cint_val_int(l & r);
            case OP_BOR: return qd_cint_val_int(l | r);
            case OP_BXOR: return qd_cint_val_int(l ^ r);
            case OP_LSHIFT: return qd_cint_val_int(l << r);
            case OP_RSHIFT: return qd_cint_val_int(l >> r);
            default: break;
        }
    }
    
    return qd_cint_val_void();
}

static qd_cint_value_t eval_unop(qd_cint_eval_t *e, qd_cint_ast_t *ast)
{
    qd_cint_value_t val = eval_node(e, ast->u.unop.operand);
    
    switch (ast->u.unop.op) {
        case UOP_NEG:
            if (val.type == VAL_FLOAT)
                return qd_cint_val_float(-val.u.f);
            return qd_cint_val_int(-qd_cint_val_to_int(&val));
            
        case UOP_NOT:
            return qd_cint_val_int(!qd_cint_val_to_bool(&val));
            
        case UOP_BNOT:
            return qd_cint_val_int(~qd_cint_val_to_int(&val));
            
        case UOP_PRE_INC:
        case UOP_POST_INC:
        case UOP_PRE_DEC:
        case UOP_POST_DEC:
            /* Handle increment/decrement on variables */
            if (ast->u.unop.operand->type == AST_IDENT) {
                qd_cint_var_t *var = scope_find(e->current_scope, 
                                                 ast->u.unop.operand->u.ident);
                if (var) {
                    int64_t old = qd_cint_val_to_int(&var->value);
                    int64_t delta = (ast->u.unop.op == UOP_PRE_INC || 
                                     ast->u.unop.op == UOP_POST_INC) ? 1 : -1;
                    var->value = qd_cint_val_int(old + delta);
                    
                    if (ast->u.unop.op == UOP_PRE_INC || 
                        ast->u.unop.op == UOP_PRE_DEC)
                        return var->value;
                    return qd_cint_val_int(old);
                }
            }
            break;
        default: break;
    }
    
    return val;
}

static qd_cint_value_t eval_call(qd_cint_eval_t *e, qd_cint_ast_t *ast)
{
    /* Get function name */
    if (ast->u.call.func->type != AST_IDENT) {
        eval_error(e, ast->line, "Function expression not supported");
        return qd_cint_val_void();
    }
    
    const char *name = ast->u.call.func->u.ident;
    
    /* Built-in: print */
    if (strcmp(name, "print") == 0) {
        for (int i = 0; i < ast->u.call.num_args; i++) {
            if (i > 0) printf(" ");
            qd_cint_value_t v = eval_node(e, ast->u.call.args[i]);
            qd_cint_val_print(&v);
            qd_cint_val_free(&v);
        }
        printf("\n");
        return qd_cint_val_void();
    }
    
    /* Built-in: printf (simplified) */
    if (strcmp(name, "printf") == 0 && ast->u.call.num_args > 0) {
        qd_cint_value_t fmt = eval_node(e, ast->u.call.args[0]);
        if (fmt.type == VAL_STRING) {
            /* Simple printf - just print the format string for now */
            printf("%s", fmt.u.s);
        }
        qd_cint_val_free(&fmt);
        return qd_cint_val_int(0);
    }
    
    /* Check for native function */
    for (int i = 0; i < e->num_natives; i++) {
        if (strcmp(e->natives[i].name, name) == 0) {
            /* Evaluate arguments */
            int64_t args[16] = {0};
            for (int j = 0; j < ast->u.call.num_args && j < 16; j++) {
                qd_cint_value_t v = eval_node(e, ast->u.call.args[j]);
                args[j] = qd_cint_val_to_int(&v);
                qd_cint_val_free(&v);
            }
            
            /* Call native function (up to 4 args for simplicity) */
            typedef int64_t (*native_fn_t)(int64_t, int64_t, int64_t, int64_t);
            native_fn_t fn = (native_fn_t)e->natives[i].fptr;
            int64_t result = fn(args[0], args[1], args[2], args[3]);
            
            if (e->natives[i].ret_type == VAL_VOID)
                return qd_cint_val_void();
            return qd_cint_val_int(result);
        }
    }
    
    /* Check for interpreted function */
    qd_cint_var_t *func_var = scope_find(e->current_scope, name);
    if (func_var && func_var->value.type == VAL_FUNC) {
        qd_cint_ast_t *decl = func_var->value.u.func.decl;
        
        /* Create new scope */
        qd_cint_scope_t *old_scope = e->current_scope;
        e->current_scope = scope_create(e->global_scope);
        
        /* Bind arguments to parameters */
        for (int i = 0; i < decl->u.funcdecl.num_params && 
             i < ast->u.call.num_args; i++) {
            qd_cint_ast_t *param = decl->u.funcdecl.params[i];
            qd_cint_value_t arg_val = eval_node(old_scope->parent ? 
                                                 (qd_cint_eval_t*)((char*)old_scope - 
                                                  offsetof(qd_cint_eval_t, current_scope)) : e,
                                                 ast->u.call.args[i]);
            
            /* Actually evaluate in old scope */
            arg_val = eval_node(e, ast->u.call.args[i]);
            
            qd_cint_var_t *pvar = scope_define(e->current_scope, param->u.param.name);
            pvar->value = arg_val;
        }
        
        /* Evaluate body */
        e->flow = FLOW_NORMAL;
        eval_node(e, decl->u.funcdecl.body);
        
        qd_cint_value_t result = e->return_value;
        e->return_value = qd_cint_val_void();
        e->flow = FLOW_NORMAL;
        
        /* Restore scope */
        scope_destroy(e->current_scope);
        e->current_scope = old_scope;
        
        return result;
    }
    
    eval_error(e, ast->line, "Unknown function");
    return qd_cint_val_void();
}

/*
 * Main evaluation dispatcher
 */

static qd_cint_value_t eval_node(qd_cint_eval_t *e, qd_cint_ast_t *ast)
{
    if (!ast) return qd_cint_val_void();
    
    switch (ast->type) {
        /* Literals */
        case AST_INT:
            return qd_cint_val_int(ast->u.int_val);
        case AST_FLOAT:
            return qd_cint_val_float(ast->u.float_val);
        case AST_STRING:
            return qd_cint_val_string(ast->u.str_val);
        case AST_CHAR:
            return qd_cint_val_char(ast->u.char_val);
            
        /* Identifier */
        case AST_IDENT: {
            qd_cint_var_t *var = scope_find(e->current_scope, ast->u.ident);
            if (var) return var->value;
            eval_error(e, ast->line, "Undefined variable");
            return qd_cint_val_void();
        }
        
        /* Expressions */
        case AST_BINOP:
            return eval_binop(e, ast);
            
        case AST_UNOP:
            return eval_unop(e, ast);
            
        case AST_CALL:
            return eval_call(e, ast);
            
        case AST_ASSIGN: {
            qd_cint_value_t val = eval_node(e, ast->u.assign.value);
            
            if (ast->u.assign.target->type == AST_IDENT) {
                qd_cint_var_t *var = scope_find(e->current_scope,
                                                 ast->u.assign.target->u.ident);
                if (var) {
                    /* Handle compound assignment */
                    if (ast->u.assign.compound_op != OP_ASSIGN) {
                        int64_t old = qd_cint_val_to_int(&var->value);
                        int64_t v = qd_cint_val_to_int(&val);
                        switch (ast->u.assign.compound_op) {
                            case OP_ADD_ASSIGN: v = old + v; break;
                            case OP_SUB_ASSIGN: v = old - v; break;
                            case OP_MUL_ASSIGN: v = old * v; break;
                            case OP_DIV_ASSIGN: v = v ? old / v : 0; break;
                            default: break;
                        }
                        val = qd_cint_val_int(v);
                    }
                    qd_cint_val_free(&var->value);
                    var->value = val;
                    return val;
                }
            }
            eval_error(e, ast->line, "Invalid assignment target");
            return val;
        }
        
        case AST_TERNARY: {
            qd_cint_value_t cond = eval_node(e, ast->u.ternary.cond);
            if (qd_cint_val_to_bool(&cond))
                return eval_node(e, ast->u.ternary.then_expr);
            return eval_node(e, ast->u.ternary.else_expr);
        }
        
        /* Statements */
        case AST_BLOCK:
            for (int i = 0; i < ast->u.block.num_stmts; i++) {
                eval_node(e, ast->u.block.stmts[i]);
                if (e->flow != FLOW_NORMAL) break;
            }
            return qd_cint_val_void();
            
        case AST_IF: {
            qd_cint_value_t cond = eval_node(e, ast->u.if_stmt.cond);
            if (qd_cint_val_to_bool(&cond)) {
                eval_node(e, ast->u.if_stmt.then_body);
            } else if (ast->u.if_stmt.else_body) {
                eval_node(e, ast->u.if_stmt.else_body);
            }
            return qd_cint_val_void();
        }
        
        case AST_WHILE: {
            while (1) {
                qd_cint_value_t cond = eval_node(e, ast->u.while_stmt.cond);
                if (!qd_cint_val_to_bool(&cond)) break;
                
                eval_node(e, ast->u.while_stmt.body);
                
                if (e->flow == FLOW_BREAK) {
                    e->flow = FLOW_NORMAL;
                    break;
                }
                if (e->flow == FLOW_CONTINUE) {
                    e->flow = FLOW_NORMAL;
                    continue;
                }
                if (e->flow == FLOW_RETURN) break;
            }
            return qd_cint_val_void();
        }
        
        case AST_FOR: {
            /* Init */
            if (ast->u.for_stmt.init)
                eval_node(e, ast->u.for_stmt.init);
            
            while (1) {
                /* Condition */
                if (ast->u.for_stmt.cond) {
                    qd_cint_value_t cond = eval_node(e, ast->u.for_stmt.cond);
                    if (!qd_cint_val_to_bool(&cond)) break;
                }
                
                /* Body */
                eval_node(e, ast->u.for_stmt.body);
                
                if (e->flow == FLOW_BREAK) {
                    e->flow = FLOW_NORMAL;
                    break;
                }
                if (e->flow == FLOW_CONTINUE)
                    e->flow = FLOW_NORMAL;
                if (e->flow == FLOW_RETURN) break;
                
                /* Increment */
                if (ast->u.for_stmt.incr)
                    eval_node(e, ast->u.for_stmt.incr);
            }
            return qd_cint_val_void();
        }
        
        case AST_RETURN:
            if (ast->u.ret.value)
                e->return_value = eval_node(e, ast->u.ret.value);
            e->flow = FLOW_RETURN;
            return qd_cint_val_void();
            
        case AST_BREAK:
            e->flow = FLOW_BREAK;
            return qd_cint_val_void();
            
        case AST_CONTINUE:
            e->flow = FLOW_CONTINUE;
            return qd_cint_val_void();
            
        case AST_EXPR_STMT:
            return eval_node(e, ast->u.expr_stmt.expr);
            
        /* Declarations */
        case AST_VARDECL: {
            qd_cint_var_t *var = scope_define(e->current_scope, ast->u.vardecl.name);
            if (ast->u.vardecl.init)
                var->value = eval_node(e, ast->u.vardecl.init);
            return qd_cint_val_void();
        }
        
        case AST_FUNCDECL: {
            qd_cint_var_t *var = scope_define(e->current_scope, ast->u.funcdecl.name);
            var->value.type = VAL_FUNC;
            var->value.u.func.decl = ast;
            return qd_cint_val_void();
        }
        
        default:
            eval_error(e, ast->line, "Unsupported AST node");
            return qd_cint_val_void();
    }
}

/*
 * Public API
 */

qd_cint_eval_t *qd_cint_eval_create(void)
{
    qd_cint_eval_t *e = calloc(1, sizeof(qd_cint_eval_t));
    e->global_scope = scope_create(NULL);
    e->current_scope = e->global_scope;
    return e;
}

void qd_cint_eval_destroy(qd_cint_eval_t *eval)
{
    if (!eval) return;
    
    /* Free scopes */
    while (eval->current_scope != eval->global_scope) {
        qd_cint_scope_t *parent = eval->current_scope->parent;
        scope_destroy(eval->current_scope);
        eval->current_scope = parent;
    }
    scope_destroy(eval->global_scope);
    
    /* Free natives */
    for (int i = 0; i < eval->num_natives; i++)
        free(eval->natives[i].name);
    
    if (eval->error)
        free(eval->error);
    
    free(eval);
}

qd_cint_value_t qd_cint_eval_ast(qd_cint_eval_t *eval, qd_cint_ast_t *ast)
{
    return eval_node(eval, ast);
}

qd_cint_value_t qd_cint_eval_string(qd_cint_eval_t *eval, const char *code)
{
    qd_cint_parser_t parser;
    qd_cint_parser_init(&parser, code, 0);
    
    qd_cint_ast_t *ast = qd_cint_parse_statement(&parser);
    
    if (qd_cint_parser_had_error(&parser)) {
        eval_error(eval, 1, qd_cint_parser_error(&parser));
        qd_cint_parser_free(&parser);
        return qd_cint_val_void();
    }
    
    qd_cint_value_t result = eval_node(eval, ast);
    
    qd_cint_ast_free(ast);
    qd_cint_parser_free(&parser);
    
    return result;
}

int qd_cint_eval_register_native(qd_cint_eval_t *eval,
                                  const char *name,
                                  void *fptr,
                                  int num_args,
                                  qd_cint_val_type_t ret_type)
{
    if (eval->num_natives >= 256) return -1;
    
    eval->natives[eval->num_natives].name = strdup(name);
    eval->natives[eval->num_natives].fptr = fptr;
    eval->natives[eval->num_natives].num_args = num_args;
    eval->natives[eval->num_natives].ret_type = ret_type;
    eval->num_natives++;
    
    return 0;
}

qd_cint_value_t *qd_cint_eval_get_var(qd_cint_eval_t *eval, const char *name)
{
    qd_cint_var_t *var = scope_find(eval->current_scope, name);
    return var ? &var->value : NULL;
}

int qd_cint_eval_set_var(qd_cint_eval_t *eval, const char *name,
                          qd_cint_value_t value)
{
    qd_cint_var_t *var = scope_find(eval->current_scope, name);
    if (!var)
        var = scope_define(eval->current_scope, name);
    
    qd_cint_val_free(&var->value);
    var->value = value;
    return 0;
}

const char *qd_cint_eval_error(qd_cint_eval_t *eval)
{
    return eval->error;
}

void qd_cint_eval_clear_error(qd_cint_eval_t *eval)
{
    if (eval->error) {
        free(eval->error);
        eval->error = NULL;
    }
}
