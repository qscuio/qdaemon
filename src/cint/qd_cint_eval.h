/*
 * QD-CINT Evaluator - Tree-walking interpreter
 * Clean-room implementation
 */

#ifndef QD_CINT_EVAL_H
#define QD_CINT_EVAL_H

#include "qd_cint_ast.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Value types
 */
typedef enum {
    VAL_VOID,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_CHAR,
    VAL_PTR,
    VAL_FUNC,       /* Interpreted function */
    VAL_NATIVE,     /* Native C function */
} qd_cint_val_type_t;

/*
 * Runtime value
 */
typedef struct qd_cint_value {
    qd_cint_val_type_t type;
    union {
        int64_t i;
        double f;
        char *s;
        char c;
        void *ptr;
        struct {
            qd_cint_ast_t *decl;    /* Function AST */
        } func;
        struct {
            void *fptr;             /* Native function pointer */
            int num_args;
            qd_cint_val_type_t ret_type;
        } native;
    } u;
} qd_cint_value_t;

/*
 * Variable entry
 */
typedef struct qd_cint_var {
    char *name;
    qd_cint_value_t value;
    qd_cint_typespec_t *type;
    struct qd_cint_var *next;
} qd_cint_var_t;

/*
 * Scope (linked list of variables)
 */
typedef struct qd_cint_scope {
    qd_cint_var_t *vars;
    struct qd_cint_scope *parent;
} qd_cint_scope_t;

/*
 * Control flow signals
 */
typedef enum {
    FLOW_NORMAL,
    FLOW_RETURN,
    FLOW_BREAK,
    FLOW_CONTINUE,
} qd_cint_flow_t;

/*
 * Evaluator state
 */
typedef struct qd_cint_eval {
    qd_cint_scope_t *global_scope;
    qd_cint_scope_t *current_scope;
    
    /* Return value from function */
    qd_cint_value_t return_value;
    qd_cint_flow_t flow;
    
    /* Error handling */
    char *error;
    int error_line;
    
    /* Native function registry */
    struct {
        char *name;
        void *fptr;
        int num_args;
        qd_cint_val_type_t ret_type;
    } natives[256];
    int num_natives;
    
    /* Builtin print buffer */
    char print_buf[4096];
    
} qd_cint_eval_t;

/*
 * Evaluator API
 */

/* Create evaluator */
qd_cint_eval_t *qd_cint_eval_create(void);

/* Destroy evaluator */
void qd_cint_eval_destroy(qd_cint_eval_t *eval);

/* Evaluate AST, return result value */
qd_cint_value_t qd_cint_eval_ast(qd_cint_eval_t *eval, qd_cint_ast_t *ast);

/* Evaluate string of code */
qd_cint_value_t qd_cint_eval_string(qd_cint_eval_t *eval, const char *code);

/* Register native function */
int qd_cint_eval_register_native(qd_cint_eval_t *eval, 
                                  const char *name,
                                  void *fptr,
                                  int num_args,
                                  qd_cint_val_type_t ret_type);

/* Get variable value */
qd_cint_value_t *qd_cint_eval_get_var(qd_cint_eval_t *eval, const char *name);

/* Set variable value */
int qd_cint_eval_set_var(qd_cint_eval_t *eval, const char *name, 
                          qd_cint_value_t value);

/* Get error message */
const char *qd_cint_eval_error(qd_cint_eval_t *eval);

/* Clear error */
void qd_cint_eval_clear_error(qd_cint_eval_t *eval);

/*
 * Value helpers
 */

qd_cint_value_t qd_cint_val_void(void);
qd_cint_value_t qd_cint_val_int(int64_t v);
qd_cint_value_t qd_cint_val_float(double v);
qd_cint_value_t qd_cint_val_string(const char *s);
qd_cint_value_t qd_cint_val_char(char c);

/* Convert value to int */
int64_t qd_cint_val_to_int(qd_cint_value_t *v);

/* Convert value to float */
double qd_cint_val_to_float(qd_cint_value_t *v);

/* Convert value to bool */
int qd_cint_val_to_bool(qd_cint_value_t *v);

/* Print value */
void qd_cint_val_print(qd_cint_value_t *v);

/* Free string value */
void qd_cint_val_free(qd_cint_value_t *v);

#ifdef __cplusplus
}
#endif

#endif /* QD_CINT_EVAL_H */
