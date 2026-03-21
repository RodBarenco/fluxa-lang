/* lexer.h — Fluxa Lexer
 * Tokenizes .flx source files.
 * Sprint 1: covers all tokens needed for print("hello world")
 */
#ifndef FLUXA_LEXER_H
#define FLUXA_LEXER_H

/* ── Token types ─────────────────────────────────────────────────────────── */
typedef enum {
    /* literals */
    TOK_INT,        /* 42           */
    TOK_FLOAT,      /* 3.14         */
    TOK_STRING,     /* "hello"      */
    TOK_BOOL,       /* true / false */

    /* identifiers & keywords */
    TOK_IDENT,      /* any name     */
    TOK_FN,
    TOK_RETURN,
    TOK_NIL,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_IMPORT,
    TOK_BLOCK,
    TOK_TYPEOF,
    TOK_PRST,
    TOK_FREE,
    TOK_DANGER,
    TOK_ERR,

    /* types */
    TOK_TYPE_INT,
    TOK_TYPE_FLOAT,
    TOK_TYPE_STR,
    TOK_TYPE_BOOL,
    TOK_TYPE_CHAR,
    TOK_TYPE_ARR,
    TOK_TYPE_DYN,

    /* symbols */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_COMMA,      /* , */
    TOK_DOT,        /* . */
    TOK_EQ,         /* = */
    TOK_NEQ,        /* != */
    TOK_EQEQ,       /* == */
    TOK_LT,         /* < */
    TOK_GT,         /* > */
    TOK_LTE,        /* <= */
    TOK_GTE,        /* >= */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_STAR,       /* * */
    TOK_SLASH,      /* / */
    TOK_PERCENT,    /* % */

    TOK_EOF,
    TOK_ERROR,
} TokenType;

/* ── Token ───────────────────────────────────────────────────────────────── */
typedef struct {
    TokenType   type;
    char       *value;   /* heap-allocated string representation */
    int         line;
} Token;

/* ── Lexer state ─────────────────────────────────────────────────────────── */
typedef struct {
    const char *src;
    int         pos;
    int         line;
    int         len;
} Lexer;

/* ── Public API ──────────────────────────────────────────────────────────── */
Lexer  lexer_new(const char *source);
Token  lexer_next(Lexer *l);
void   token_free(Token *t);
const char *token_type_name(TokenType t);

#endif /* FLUXA_LEXER_H */
