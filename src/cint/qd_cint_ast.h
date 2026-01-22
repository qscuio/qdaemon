/*
 * QD-CINT AST - Abstract Syntax Tree
 * Clean-room implementation
 */

#ifndef QD_CINT_AST_H
#define QD_CINT_AST_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AST Node Types
 */
typedef enum {
    /* Literals */
    AST_INT,            /* Integer literal */
    AST_FLOAT,          /* Float literal */
    AST_STRING,         /* String literal */
    AST_CHAR,           /* Char literal */
    
    /* Identifiers */
    AST_IDENT,          /* Variable/function reference */
    
    /* Expressions */
    AST_BINOP,          /* Binary operation: a + b */
    AST_UNOP,           /* Unary operation: -a, !a */
    AST_ASSIGN,         /* Assignment: a = b */
    AST_CALL,           /* Function call: foo(a, b) */
    AST_INDEX,          /* Array index: a[i] */
    AST_MEMBER,         /* Struct member: a.x */
    AST_PTRMEMBER,      /* Pointer member: a->x */
    AST_TERNARY,        /* Ternary: a ? b : c */
    AST_CAST,           /* Type cast: (int)a */
    AST_SIZEOF,         /* sizeof(type) or sizeof(expr) */
    AST_ADDR,           /* Address-of: &a */
    AST_DEREF,          /* Dereference: *a */
    
    /* Statements */
    AST_BLOCK,          /* Block: { ... } */
    AST_IF,             /* If statement */
    AST_WHILE,          /* While loop */
    AST_FOR,            /* For loop */
    AST_DOWHILE,        /* Do-while loop */
    AST_SWITCH,         /* Switch statement */
    AST_CASE,           /* Case label */
    AST_DEFAULT,        /* Default label */
    AST_BREAK,          /* Break */
    AST_CONTINUE,       /* Continue */
    AST_RETURN,         /* Return */
    AST_EXPR_STMT,      /* Expression statement */
    
    /* Declarations */
    AST_VARDECL,        /* Variable declaration */
    AST_FUNCDECL,       /* Function declaration */
    AST_STRUCTDECL,     /* Struct declaration */
    AST_TYPEDEF,        /* Typedef */
    AST_PARAM,          /* Function parameter */
    
    /* Types */
    AST_TYPE,           /* Type specifier */
    
    /* Lists */
    AST_LIST,           /* Generic list of nodes */
    
    AST_COUNT
} qd_cint_ast_type_t;

/*
 * Binary operators
 */
typedef enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_AND, OP_OR,
    OP_BAND, OP_BOR, OP_BXOR, OP_LSHIFT, OP_RSHIFT,
    OP_ASSIGN, OP_ADD_ASSIGN, OP_SUB_ASSIGN,
    OP_MUL_ASSIGN, OP_DIV_ASSIGN, OP_MOD_ASSIGN,
    OP_COMMA
} qd_cint_binop_t;

/*
 * Unary operators
 */
typedef enum {
    UOP_NEG,        /* -x */
    UOP_NOT,        /* !x */
    UOP_BNOT,       /* ~x */
    UOP_PRE_INC,    /* ++x */
    UOP_PRE_DEC,    /* --x */
    UOP_POST_INC,   /* x++ */
    UOP_POST_DEC,   /* x-- */
} qd_cint_unop_t;

/*
 * Type specifier
 */
typedef struct qd_cint_typespec {
    char *name;             /* Type name (int, char, struct X, etc.) */
    int pointer_depth;      /* Number of * */
    int array_size;         /* -1 if not array, 0 if [] (unsized), >0 for [N] */
    bool is_const;
    bool is_unsigned;
} qd_cint_typespec_t;

/*
 * Forward declaration
 */
typedef struct qd_cint_ast qd_cint_ast_t;

/*
 * AST Node
 */
struct qd_cint_ast {
    qd_cint_ast_type_t type;
    int line;
    int col;
    
    union {
        /* Literals */
        int64_t int_val;
        double float_val;
        char *str_val;
        char char_val;
        
        /* Identifier */
        char *ident;
        
        /* Binary operation */
        struct {
            qd_cint_binop_t op;
            qd_cint_ast_t *left;
            qd_cint_ast_t *right;
        } binop;
        
        /* Unary operation */
        struct {
            qd_cint_unop_t op;
            qd_cint_ast_t *operand;
        } unop;
        
        /* Assignment */
        struct {
            qd_cint_ast_t *target;
            qd_cint_ast_t *value;
            qd_cint_binop_t compound_op;  /* For +=, -=, etc. OP_ASSIGN for plain = */
        } assign;
        
        /* Function call */
        struct {
            qd_cint_ast_t *func;          /* Function name or expression */
            qd_cint_ast_t **args;         /* Array of arguments */
            int num_args;
        } call;
        
        /* Array index */
        struct {
            qd_cint_ast_t *array;
            qd_cint_ast_t *index;
        } index;
        
        /* Member access */
        struct {
            qd_cint_ast_t *object;
            char *member;
        } member;
        
        /* Ternary */
        struct {
            qd_cint_ast_t *cond;
            qd_cint_ast_t *then_expr;
            qd_cint_ast_t *else_expr;
        } ternary;
        
        /* Cast */
        struct {
            qd_cint_typespec_t *target_type;
            qd_cint_ast_t *expr;
        } cast;
        
        /* Sizeof */
        struct {
            qd_cint_typespec_t *type;     /* If sizeof(type) */
            qd_cint_ast_t *expr;          /* If sizeof(expr) */
        } size_of;
        
        /* Block */
        struct {
            qd_cint_ast_t **stmts;
            int num_stmts;
        } block;
        
        /* If statement */
        struct {
            qd_cint_ast_t *cond;
            qd_cint_ast_t *then_body;
            qd_cint_ast_t *else_body;     /* NULL if no else */
        } if_stmt;
        
        /* While/Do-While */
        struct {
            qd_cint_ast_t *cond;
            qd_cint_ast_t *body;
        } while_stmt;
        
        /* For statement */
        struct {
            qd_cint_ast_t *init;          /* Can be decl or expr */
            qd_cint_ast_t *cond;
            qd_cint_ast_t *incr;
            qd_cint_ast_t *body;
        } for_stmt;
        
        /* Switch statement */
        struct {
            qd_cint_ast_t *expr;
            qd_cint_ast_t **cases;
            int num_cases;
        } switch_stmt;
        
        /* Case/Default */
        struct {
            qd_cint_ast_t *value;         /* NULL for default */
            qd_cint_ast_t **stmts;
            int num_stmts;
        } case_stmt;
        
        /* Return */
        struct {
            qd_cint_ast_t *value;         /* NULL for void return */
        } ret;
        
        /* Expression statement */
        struct {
            qd_cint_ast_t *expr;
        } expr_stmt;
        
        /* Variable declaration */
        struct {
            qd_cint_typespec_t *type;
            char *name;
            qd_cint_ast_t *init;          /* NULL if no initializer */
        } vardecl;
        
        /* Function declaration */
        struct {
            qd_cint_typespec_t *return_type;
            char *name;
            qd_cint_ast_t **params;       /* Array of AST_PARAM */
            int num_params;
            qd_cint_ast_t *body;          /* NULL for declaration only */
        } funcdecl;
        
        /* Function parameter */
        struct {
            qd_cint_typespec_t *type;
            char *name;
        } param;
        
        /* Type specifier node */
        qd_cint_typespec_t *typespec;
        
        /* Generic list */
        struct {
            qd_cint_ast_t **items;
            int num_items;
        } list;
        
    } u;
};

/*
 * AST API
 */

/* Create nodes */
qd_cint_ast_t *qd_cint_ast_int(int64_t val, int line, int col);
qd_cint_ast_t *qd_cint_ast_float(double val, int line, int col);
qd_cint_ast_t *qd_cint_ast_string(const char *val, int line, int col);
qd_cint_ast_t *qd_cint_ast_char(char val, int line, int col);
qd_cint_ast_t *qd_cint_ast_ident(const char *name, int line, int col);

qd_cint_ast_t *qd_cint_ast_binop(qd_cint_binop_t op, 
                                  qd_cint_ast_t *left, 
                                  qd_cint_ast_t *right,
                                  int line, int col);

qd_cint_ast_t *qd_cint_ast_unop(qd_cint_unop_t op,
                                 qd_cint_ast_t *operand,
                                 int line, int col);

qd_cint_ast_t *qd_cint_ast_call(qd_cint_ast_t *func,
                                 qd_cint_ast_t **args, int num_args,
                                 int line, int col);

qd_cint_ast_t *qd_cint_ast_if(qd_cint_ast_t *cond,
                               qd_cint_ast_t *then_body,
                               qd_cint_ast_t *else_body,
                               int line, int col);

qd_cint_ast_t *qd_cint_ast_while(qd_cint_ast_t *cond,
                                  qd_cint_ast_t *body,
                                  int line, int col);

qd_cint_ast_t *qd_cint_ast_for(qd_cint_ast_t *init,
                                qd_cint_ast_t *cond,
                                qd_cint_ast_t *incr,
                                qd_cint_ast_t *body,
                                int line, int col);

qd_cint_ast_t *qd_cint_ast_block(qd_cint_ast_t **stmts, int num_stmts,
                                  int line, int col);

qd_cint_ast_t *qd_cint_ast_return(qd_cint_ast_t *value, int line, int col);

qd_cint_ast_t *qd_cint_ast_vardecl(qd_cint_typespec_t *type,
                                    const char *name,
                                    qd_cint_ast_t *init,
                                    int line, int col);

qd_cint_ast_t *qd_cint_ast_funcdecl(qd_cint_typespec_t *ret_type,
                                     const char *name,
                                     qd_cint_ast_t **params, int num_params,
                                     qd_cint_ast_t *body,
                                     int line, int col);

/* Type specifier */
qd_cint_typespec_t *qd_cint_typespec_create(const char *name);
void qd_cint_typespec_free(qd_cint_typespec_t *ts);

/* Free AST node (recursive) */
void qd_cint_ast_free(qd_cint_ast_t *ast);

/* Debug: print AST */
void qd_cint_ast_print(qd_cint_ast_t *ast, int indent);

/* Get operator name */
const char *qd_cint_binop_name(qd_cint_binop_t op);
const char *qd_cint_unop_name(qd_cint_unop_t op);

#ifdef __cplusplus
}
#endif

#endif /* QD_CINT_AST_H */
