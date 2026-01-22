/*
 * QD-CINT Lexer Implementation
 * Clean-room C tokenizer
 */

#include "qd_cint_lexer.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/*
 * Keyword table
 */
static struct {
    const char *name;
    qd_cint_token_type_t type;
} keywords[] = {
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"while",    TOK_WHILE},
    {"for",      TOK_FOR},
    {"do",       TOK_DO},
    {"switch",   TOK_SWITCH},
    {"case",     TOK_CASE},
    {"default",  TOK_DEFAULT},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"return",   TOK_RETURN},
    {"goto",     TOK_GOTO},
    {"void",     TOK_VOID},
    {"int",      TOK_INT_KW},
    {"char",     TOK_CHAR_KW},
    {"short",    TOK_SHORT},
    {"long",     TOK_LONG},
    {"float",    TOK_FLOAT_KW},
    {"double",   TOK_DOUBLE},
    {"signed",   TOK_SIGNED},
    {"unsigned", TOK_UNSIGNED},
    {"struct",   TOK_STRUCT},
    {"union",    TOK_UNION},
    {"enum",     TOK_ENUM},
    {"typedef",  TOK_TYPEDEF},
    {"const",    TOK_CONST},
    {"static",   TOK_STATIC},
    {"extern",   TOK_EXTERN},
    {"sizeof",   TOK_SIZEOF},
    {NULL, 0}
};

static const char *token_names[] = {
    [TOK_EOF] = "EOF",
    [TOK_ERROR] = "ERROR",
    [TOK_INT] = "INT",
    [TOK_FLOAT] = "FLOAT",
    [TOK_STRING] = "STRING",
    [TOK_CHAR] = "CHAR",
    [TOK_IDENT] = "IDENT",
    [TOK_IF] = "if",
    [TOK_ELSE] = "else",
    [TOK_WHILE] = "while",
    [TOK_FOR] = "for",
    [TOK_RETURN] = "return",
    [TOK_PLUS] = "+",
    [TOK_MINUS] = "-",
    [TOK_STAR] = "*",
    [TOK_SLASH] = "/",
    [TOK_EQ] = "==",
    [TOK_ASSIGN] = "=",
    [TOK_LPAREN] = "(",
    [TOK_RPAREN] = ")",
    [TOK_LBRACE] = "{",
    [TOK_RBRACE] = "}",
    [TOK_SEMICOLON] = ";",
    [TOK_COMMA] = ",",
};

/*
 * Helper functions
 */

static inline int is_ident_start(char c)
{
    return isalpha(c) || c == '_';
}

static inline int is_ident_char(char c)
{
    return isalnum(c) || c == '_';
}

static inline char peek(qd_cint_lexer_t *lex)
{
    return (lex->pos < lex->end) ? *lex->pos : '\0';
}

static inline char peek_next(qd_cint_lexer_t *lex)
{
    return (lex->pos + 1 < lex->end) ? *(lex->pos + 1) : '\0';
}

static inline char advance(qd_cint_lexer_t *lex)
{
    char c = peek(lex);
    if (c) {
        lex->pos++;
        if (c == '\n') {
            lex->line++;
            lex->col = 1;
        } else {
            lex->col++;
        }
    }
    return c;
}

static void skip_whitespace(qd_cint_lexer_t *lex)
{
    while (lex->pos < lex->end) {
        char c = peek(lex);
        if (isspace(c)) {
            advance(lex);
        } else if (c == '/' && peek_next(lex) == '/') {
            /* Line comment */
            while (peek(lex) && peek(lex) != '\n')
                advance(lex);
        } else if (c == '/' && peek_next(lex) == '*') {
            /* Block comment */
            advance(lex);
            advance(lex);
            while (lex->pos < lex->end) {
                if (peek(lex) == '*' && peek_next(lex) == '/') {
                    advance(lex);
                    advance(lex);
                    break;
                }
                advance(lex);
            }
        } else {
            break;
        }
    }
}

static qd_cint_token_t make_token(qd_cint_lexer_t *lex, qd_cint_token_type_t type)
{
    qd_cint_token_t tok = {
        .type = type,
        .val = {0},
        .line = lex->line,
        .col = lex->col
    };
    return tok;
}

static qd_cint_token_t scan_number(qd_cint_lexer_t *lex)
{
    qd_cint_token_t tok = make_token(lex, TOK_INT);
    const char *start = lex->pos;
    int is_float = 0;
    int is_hex = 0;
    
    /* Check for hex */
    if (peek(lex) == '0' && (peek_next(lex) == 'x' || peek_next(lex) == 'X')) {
        advance(lex);
        advance(lex);
        is_hex = 1;
        while (isxdigit(peek(lex)))
            advance(lex);
    } else {
        /* Decimal or octal */
        while (isdigit(peek(lex)))
            advance(lex);
        
        /* Check for float */
        if (peek(lex) == '.') {
            is_float = 1;
            advance(lex);
            while (isdigit(peek(lex)))
                advance(lex);
        }
        
        /* Exponent */
        if (peek(lex) == 'e' || peek(lex) == 'E') {
            is_float = 1;
            advance(lex);
            if (peek(lex) == '+' || peek(lex) == '-')
                advance(lex);
            while (isdigit(peek(lex)))
                advance(lex);
        }
    }
    
    /* Parse value */
    size_t len = lex->pos - start;
    char *buf = malloc(len + 1);
    memcpy(buf, start, len);
    buf[len] = '\0';
    
    if (is_float) {
        tok.type = TOK_FLOAT;
        tok.val.f = strtod(buf, NULL);
    } else if (is_hex) {
        tok.val.i = strtoll(buf, NULL, 16);
    } else if (buf[0] == '0' && len > 1) {
        tok.val.i = strtoll(buf, NULL, 8);
    } else {
        tok.val.i = strtoll(buf, NULL, 10);
    }
    
    free(buf);
    return tok;
}

static qd_cint_token_t scan_string(qd_cint_lexer_t *lex)
{
    qd_cint_token_t tok = make_token(lex, TOK_STRING);
    advance(lex);  /* Skip opening quote */
    
    size_t cap = 64;
    size_t len = 0;
    char *buf = malloc(cap);
    
    while (peek(lex) && peek(lex) != '"') {
        char c = advance(lex);
        
        if (c == '\\') {
            c = advance(lex);
            switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                case '0': c = '\0'; break;
                default: break;
            }
        }
        
        if (len + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        buf[len++] = c;
    }
    
    advance(lex);  /* Skip closing quote */
    buf[len] = '\0';
    tok.val.s = buf;
    
    return tok;
}

static qd_cint_token_t scan_char(qd_cint_lexer_t *lex)
{
    qd_cint_token_t tok = make_token(lex, TOK_CHAR);
    advance(lex);  /* Skip opening quote */
    
    char c = advance(lex);
    if (c == '\\') {
        c = advance(lex);
        switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            case '\'': c = '\''; break;
            case '0': c = '\0'; break;
            default: break;
        }
    }
    
    tok.val.c = c;
    advance(lex);  /* Skip closing quote */
    
    return tok;
}

static qd_cint_token_t scan_ident(qd_cint_lexer_t *lex)
{
    qd_cint_token_t tok = make_token(lex, TOK_IDENT);
    const char *start = lex->pos;
    
    while (is_ident_char(peek(lex)))
        advance(lex);
    
    size_t len = lex->pos - start;
    
    /* Check keywords */
    for (int i = 0; keywords[i].name; i++) {
        if (strlen(keywords[i].name) == len &&
            strncmp(keywords[i].name, start, len) == 0) {
            tok.type = keywords[i].type;
            return tok;
        }
    }
    
    /* Identifier */
    char *buf = malloc(len + 1);
    memcpy(buf, start, len);
    buf[len] = '\0';
    tok.val.s = buf;
    
    return tok;
}

/*
 * Public API
 */

void qd_cint_lexer_init(qd_cint_lexer_t *lex, const char *src, size_t len)
{
    lex->src = src;
    lex->pos = src;
    lex->end = src + (len ? len : strlen(src));
    lex->line = 1;
    lex->col = 1;
    lex->error = NULL;
}

void qd_cint_lexer_reset(qd_cint_lexer_t *lex)
{
    lex->pos = lex->src;
    lex->line = 1;
    lex->col = 1;
    if (lex->error) {
        free(lex->error);
        lex->error = NULL;
    }
}

qd_cint_token_t qd_cint_lexer_next(qd_cint_lexer_t *lex)
{
    skip_whitespace(lex);
    
    if (lex->pos >= lex->end)
        return make_token(lex, TOK_EOF);
    
    char c = peek(lex);
    
    /* Numbers */
    if (isdigit(c))
        return scan_number(lex);
    
    /* Strings */
    if (c == '"')
        return scan_string(lex);
    
    /* Characters */
    if (c == '\'')
        return scan_char(lex);
    
    /* Identifiers/Keywords */
    if (is_ident_start(c))
        return scan_ident(lex);
    
    /* Operators and punctuation */
    qd_cint_token_t tok = make_token(lex, TOK_ERROR);
    
    advance(lex);
    
    switch (c) {
        case '+':
            if (peek(lex) == '+') { advance(lex); tok.type = TOK_INC; }
            else if (peek(lex) == '=') { advance(lex); tok.type = TOK_PLUS_EQ; }
            else tok.type = TOK_PLUS;
            break;
        case '-':
            if (peek(lex) == '-') { advance(lex); tok.type = TOK_DEC; }
            else if (peek(lex) == '=') { advance(lex); tok.type = TOK_MINUS_EQ; }
            else if (peek(lex) == '>') { advance(lex); tok.type = TOK_ARROW; }
            else tok.type = TOK_MINUS;
            break;
        case '*':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_STAR_EQ; }
            else tok.type = TOK_STAR;
            break;
        case '/':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_SLASH_EQ; }
            else tok.type = TOK_SLASH;
            break;
        case '%':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_PERCENT_EQ; }
            else tok.type = TOK_PERCENT;
            break;
        case '=':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_EQ; }
            else tok.type = TOK_ASSIGN;
            break;
        case '!':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_NE; }
            else tok.type = TOK_NOT;
            break;
        case '<':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_LE; }
            else if (peek(lex) == '<') {
                advance(lex);
                if (peek(lex) == '=') { advance(lex); tok.type = TOK_LSHIFT_EQ; }
                else tok.type = TOK_LSHIFT;
            }
            else tok.type = TOK_LT;
            break;
        case '>':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_GE; }
            else if (peek(lex) == '>') {
                advance(lex);
                if (peek(lex) == '=') { advance(lex); tok.type = TOK_RSHIFT_EQ; }
                else tok.type = TOK_RSHIFT;
            }
            else tok.type = TOK_GT;
            break;
        case '&':
            if (peek(lex) == '&') { advance(lex); tok.type = TOK_AND; }
            else if (peek(lex) == '=') { advance(lex); tok.type = TOK_BAND_EQ; }
            else tok.type = TOK_BAND;
            break;
        case '|':
            if (peek(lex) == '|') { advance(lex); tok.type = TOK_OR; }
            else if (peek(lex) == '=') { advance(lex); tok.type = TOK_BOR_EQ; }
            else tok.type = TOK_BOR;
            break;
        case '^':
            if (peek(lex) == '=') { advance(lex); tok.type = TOK_BXOR_EQ; }
            else tok.type = TOK_BXOR;
            break;
        case '~': tok.type = TOK_BNOT; break;
        case '?': tok.type = TOK_QUESTION; break;
        case ':': tok.type = TOK_COLON; break;
        case '.': tok.type = TOK_DOT; break;
        case '(': tok.type = TOK_LPAREN; break;
        case ')': tok.type = TOK_RPAREN; break;
        case '{': tok.type = TOK_LBRACE; break;
        case '}': tok.type = TOK_RBRACE; break;
        case '[': tok.type = TOK_LBRACKET; break;
        case ']': tok.type = TOK_RBRACKET; break;
        case ';': tok.type = TOK_SEMICOLON; break;
        case ',': tok.type = TOK_COMMA; break;
        default:
            tok.type = TOK_ERROR;
            break;
    }
    
    return tok;
}

qd_cint_token_t qd_cint_lexer_peek(qd_cint_lexer_t *lex)
{
    /* Save state */
    const char *pos = lex->pos;
    int line = lex->line;
    int col = lex->col;
    
    qd_cint_token_t tok = qd_cint_lexer_next(lex);
    
    /* Restore state */
    lex->pos = pos;
    lex->line = line;
    lex->col = col;
    
    return tok;
}

void qd_cint_token_free(qd_cint_token_t *tok)
{
    if (tok->type == TOK_STRING || tok->type == TOK_IDENT) {
        free(tok->val.s);
        tok->val.s = NULL;
    }
}

const char *qd_cint_token_name(qd_cint_token_type_t type)
{
    if (type < TOK_COUNT && token_names[type])
        return token_names[type];
    return "UNKNOWN";
}

int qd_cint_token_is_type(qd_cint_token_type_t type)
{
    return type >= TOK_VOID && type <= TOK_EXTERN;
}
