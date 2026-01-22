/*
 * QD-CINT Lexer - Token types and lexer API
 * Clean-room implementation of C tokenizer
 */

#ifndef QD_CINT_LEXER_H
#define QD_CINT_LEXER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Token Types
 */
typedef enum {
    /* End/Error */
    TOK_EOF = 0,
    TOK_ERROR,
    
    /* Literals */
    TOK_INT,            /* 123, 0x1F, 0777 */
    TOK_FLOAT,          /* 1.5, 3.14e10 */
    TOK_STRING,         /* "hello" */
    TOK_CHAR,           /* 'a' */
    
    /* Identifiers */
    TOK_IDENT,          /* variable/function names */
    
    /* Keywords */
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_DO,
    TOK_SWITCH,
    TOK_CASE,
    TOK_DEFAULT,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_RETURN,
    TOK_GOTO,
    
    /* Type keywords */
    TOK_VOID,
    TOK_INT_KW,
    TOK_CHAR_KW,
    TOK_SHORT,
    TOK_LONG,
    TOK_FLOAT_KW,
    TOK_DOUBLE,
    TOK_SIGNED,
    TOK_UNSIGNED,
    TOK_STRUCT,
    TOK_UNION,
    TOK_ENUM,
    TOK_TYPEDEF,
    TOK_CONST,
    TOK_STATIC,
    TOK_EXTERN,
    TOK_SIZEOF,
    
    /* Operators - Arithmetic */
    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_STAR,           /* * */
    TOK_SLASH,          /* / */
    TOK_PERCENT,        /* % */
    
    /* Operators - Comparison */
    TOK_EQ,             /* == */
    TOK_NE,             /* != */
    TOK_LT,             /* < */
    TOK_GT,             /* > */
    TOK_LE,             /* <= */
    TOK_GE,             /* >= */
    
    /* Operators - Logical */
    TOK_AND,            /* && */
    TOK_OR,             /* || */
    TOK_NOT,            /* ! */
    
    /* Operators - Bitwise */
    TOK_BAND,           /* & */
    TOK_BOR,            /* | */
    TOK_BXOR,           /* ^ */
    TOK_BNOT,           /* ~ */
    TOK_LSHIFT,         /* << */
    TOK_RSHIFT,         /* >> */
    
    /* Operators - Assignment */
    TOK_ASSIGN,         /* = */
    TOK_PLUS_EQ,        /* += */
    TOK_MINUS_EQ,       /* -= */
    TOK_STAR_EQ,        /* *= */
    TOK_SLASH_EQ,       /* /= */
    TOK_PERCENT_EQ,     /* %= */
    TOK_BAND_EQ,        /* &= */
    TOK_BOR_EQ,         /* |= */
    TOK_BXOR_EQ,        /* ^= */
    TOK_LSHIFT_EQ,      /* <<= */
    TOK_RSHIFT_EQ,      /* >>= */
    
    /* Operators - Increment/Decrement */
    TOK_INC,            /* ++ */
    TOK_DEC,            /* -- */
    
    /* Operators - Other */
    TOK_ARROW,          /* -> */
    TOK_DOT,            /* . */
    TOK_QUESTION,       /* ? */
    TOK_COLON,          /* : */
    
    /* Punctuation */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_LBRACE,         /* { */
    TOK_RBRACE,         /* } */
    TOK_LBRACKET,       /* [ */
    TOK_RBRACKET,       /* ] */
    TOK_SEMICOLON,      /* ; */
    TOK_COMMA,          /* , */
    
    TOK_COUNT
} qd_cint_token_type_t;

/*
 * Token Value Union
 */
typedef union {
    int64_t i;          /* Integer value */
    double f;           /* Float value */
    char *s;            /* String/identifier (owned, must free) */
    char c;             /* Char value */
} qd_cint_token_val_t;

/*
 * Token Structure
 */
typedef struct {
    qd_cint_token_type_t type;
    qd_cint_token_val_t val;
    int line;
    int col;
} qd_cint_token_t;

/*
 * Lexer State
 */
typedef struct qd_cint_lexer {
    const char *src;        /* Source code */
    const char *pos;        /* Current position */
    const char *end;        /* End of source */
    int line;               /* Current line (1-based) */
    int col;                /* Current column (1-based) */
    char *error;            /* Error message (if any) */
} qd_cint_lexer_t;

/*
 * Lexer API
 */

/* Initialize lexer with source code */
void qd_cint_lexer_init(qd_cint_lexer_t *lex, const char *src, size_t len);

/* Reset lexer to beginning */
void qd_cint_lexer_reset(qd_cint_lexer_t *lex);

/* Get next token */
qd_cint_token_t qd_cint_lexer_next(qd_cint_lexer_t *lex);

/* Peek at next token without consuming */
qd_cint_token_t qd_cint_lexer_peek(qd_cint_lexer_t *lex);

/* Free token resources (for string tokens) */
void qd_cint_token_free(qd_cint_token_t *tok);

/* Get token type name (for debugging) */
const char *qd_cint_token_name(qd_cint_token_type_t type);

/* Check if token is a type keyword */
int qd_cint_token_is_type(qd_cint_token_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* QD_CINT_LEXER_H */
