/* lexer.c — Fluxa Lexer implementation */
#define _POSIX_C_SOURCE 200809L
#include "lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Keyword table ───────────────────────────────────────────────────────── */
typedef struct { const char *word; TokenType type; } Keyword;

static const Keyword KEYWORDS[] = {
    {"fn",      TOK_FN},
    {"return",  TOK_RETURN},
    {"nil",     TOK_NIL},
    {"if",      TOK_IF},
    {"else",    TOK_ELSE},
    {"while",   TOK_WHILE},
    {"for",     TOK_FOR},
    {"import",  TOK_IMPORT},
    {"Block",   TOK_BLOCK},
    {"typeof",  TOK_TYPEOF},
    {"prst",    TOK_PRST},
    {"free",    TOK_FREE},
    {"danger",  TOK_DANGER},
    {"err",     TOK_ERR},
    {"true",    TOK_BOOL},
    {"false",   TOK_BOOL},
    /* types */
    {"int",     TOK_TYPE_INT},
    {"float",   TOK_TYPE_FLOAT},
    {"str",     TOK_TYPE_STR},
    {"bool",    TOK_TYPE_BOOL},
    {"char",    TOK_TYPE_CHAR},
    {"arr",     TOK_TYPE_ARR},
    {"dyn",     TOK_TYPE_DYN},
    {NULL,      TOK_ERROR},
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static char peek(Lexer *l)      { return l->pos < l->len ? l->src[l->pos] : '\0'; }
static char advance(Lexer *l)   { char c = l->src[l->pos++]; if (c=='\n') l->line++; return c; }

static Token make_tok(TokenType type, const char *val, int line) {
    Token t;
    t.type  = type;
    t.value = val ? strdup(val) : strdup("");
    t.line  = line;
    return t;
}

static Token make_err(const char *msg, int line) {
    fprintf(stderr, "[fluxa] Lexer error (line %d): %s\n", line, msg);
    return make_tok(TOK_ERROR, msg, line);
}

/* ── Public API ──────────────────────────────────────────────────────────── */
Lexer lexer_new(const char *source) {
    Lexer l;
    l.src  = source;
    l.pos  = 0;
    l.line = 1;
    l.len  = (int)strlen(source);
    return l;
}

Token lexer_next(Lexer *l) {
    /* skip whitespace */
    while (l->pos < l->len && isspace((unsigned char)peek(l)))
        advance(l);

    if (l->pos >= l->len)
        return make_tok(TOK_EOF, "", l->line);

    int    line = l->line;
    char   c    = advance(l);

    /* ── comments ──────────────────────────────────────────────────────── */
    if (c == '/' && peek(l) == '/') {
        while (l->pos < l->len && peek(l) != '\n') advance(l);
        return lexer_next(l); /* recurse to next real token */
    }

    /* ── string literal ────────────────────────────────────────────────── */
    if (c == '"') {
        char buf[4096];
        int  i = 0;
        while (l->pos < l->len && peek(l) != '"') {
            char ch = advance(l);
            if (ch == '\\') {
                char esc = advance(l);
                switch (esc) {
                    case 'n':  ch = '\n'; break;
                    case 't':  ch = '\t'; break;
                    case '"':  ch = '"';  break;
                    case '\\': ch = '\\'; break;
                    default:   ch = esc;  break;
                }
            }
            if (i < (int)sizeof(buf)-1) buf[i++] = ch;
        }
        if (peek(l) == '"') advance(l); /* consume closing " */
        else return make_err("unterminated string literal", line);
        buf[i] = '\0';
        return make_tok(TOK_STRING, buf, line);
    }

    /* ── number ────────────────────────────────────────────────────────── */
    if (isdigit((unsigned char)c)) {
        char buf[64];
        int  i = 0;
        buf[i++] = c;
        int is_float = 0;
        while (l->pos < l->len && (isdigit((unsigned char)peek(l)) || peek(l)=='.')) {
            char ch = advance(l);
            if (ch == '.') is_float = 1;
            if (i < 63) buf[i++] = ch;
        }
        buf[i] = '\0';
        return make_tok(is_float ? TOK_FLOAT : TOK_INT, buf, line);
    }

    /* ── identifier / keyword ──────────────────────────────────────────── */
    if (isalpha((unsigned char)c) || c == '_') {
        char buf[256];
        int  i = 0;
        buf[i++] = c;
        while (l->pos < l->len &&
               (isalnum((unsigned char)peek(l)) || peek(l)=='_')) {
            if (i < 255) buf[i++] = advance(l);
            else advance(l);
        }
        buf[i] = '\0';

        /* keyword lookup */
        for (int k = 0; KEYWORDS[k].word != NULL; k++) {
            if (strcmp(buf, KEYWORDS[k].word) == 0)
                return make_tok(KEYWORDS[k].type, buf, line);
        }
        return make_tok(TOK_IDENT, buf, line);
    }

    /* ── two-character symbols ─────────────────────────────────────────── */
    if (c == '!' && peek(l) == '=') { advance(l); return make_tok(TOK_NEQ,  "!=", line); }
    if (c == '=' && peek(l) == '=') { advance(l); return make_tok(TOK_EQEQ, "==", line); }
    if (c == '<' && peek(l) == '=') { advance(l); return make_tok(TOK_LTE,  "<=", line); }
    if (c == '>' && peek(l) == '=') { advance(l); return make_tok(TOK_GTE,  ">=", line); }

    /* ── single-character symbols ──────────────────────────────────────── */
    switch (c) {
        case '(': return make_tok(TOK_LPAREN,   "(", line);
        case ')': return make_tok(TOK_RPAREN,   ")", line);
        case '{': return make_tok(TOK_LBRACE,   "{", line);
        case '}': return make_tok(TOK_RBRACE,   "}", line);
        case '[': return make_tok(TOK_LBRACKET, "[", line);
        case ']': return make_tok(TOK_RBRACKET, "]", line);
        case ',': return make_tok(TOK_COMMA,    ",", line);
        case '.': return make_tok(TOK_DOT,      ".", line);
        case '=': return make_tok(TOK_EQ,       "=", line);
        case '<': return make_tok(TOK_LT,       "<", line);
        case '>': return make_tok(TOK_GT,       ">", line);
        case '+': return make_tok(TOK_PLUS,     "+", line);
        case '-': return make_tok(TOK_MINUS,    "-", line);
        case '*': return make_tok(TOK_STAR,     "*", line);
        case '/': return make_tok(TOK_SLASH,    "/", line);
        case '%': return make_tok(TOK_PERCENT,  "%", line);
    }

    char err[32];
    snprintf(err, sizeof(err), "unexpected char '%c'", c);
    return make_err(err, line);
}

void token_free(Token *t) {
    if (t && t->value) { free(t->value); t->value = NULL; }
}

const char *token_type_name(TokenType t) {
    switch(t) {
        case TOK_INT:       return "INT";
        case TOK_FLOAT:     return "FLOAT";
        case TOK_STRING:    return "STRING";
        case TOK_BOOL:      return "BOOL";
        case TOK_IDENT:     return "IDENT";
        case TOK_FN:        return "fn";
        case TOK_RETURN:    return "return";
        case TOK_NIL:       return "nil";
        case TOK_IF:        return "if";
        case TOK_ELSE:      return "else";
        case TOK_WHILE:     return "while";
        case TOK_FOR:       return "for";
        case TOK_IMPORT:    return "import";
        case TOK_BLOCK:     return "Block";
        case TOK_TYPEOF:    return "typeof";
        case TOK_PRST:      return "prst";
        case TOK_FREE:      return "free";
        case TOK_DANGER:    return "danger";
        case TOK_ERR:       return "err";
        case TOK_TYPE_INT:  return "int";
        case TOK_TYPE_FLOAT:return "float";
        case TOK_TYPE_STR:  return "str";
        case TOK_TYPE_BOOL: return "bool";
        case TOK_TYPE_CHAR: return "char";
        case TOK_TYPE_ARR:  return "arr";
        case TOK_TYPE_DYN:  return "dyn";
        case TOK_LPAREN:    return "(";
        case TOK_RPAREN:    return ")";
        case TOK_LBRACE:    return "{";
        case TOK_RBRACE:    return "}";
        case TOK_LBRACKET:  return "[";
        case TOK_RBRACKET:  return "]";
        case TOK_COMMA:     return ",";
        case TOK_DOT:       return ".";
        case TOK_EQ:        return "=";
        case TOK_NEQ:       return "!=";
        case TOK_EQEQ:      return "==";
        case TOK_LT:        return "<";
        case TOK_GT:        return ">";
        case TOK_LTE:       return "<=";
        case TOK_GTE:       return ">=";
        case TOK_PLUS:      return "+";
        case TOK_MINUS:     return "-";
        case TOK_STAR:      return "*";
        case TOK_SLASH:     return "/";
        case TOK_PERCENT:   return "%";
        case TOK_EOF:       return "EOF";
        case TOK_ERROR:     return "ERROR";
        default:            return "?";
    }
}
