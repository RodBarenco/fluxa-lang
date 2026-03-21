/* parser.c — Fluxa Parser implementation */
#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void parser_advance(Parser *p) {
    token_free(&p->current);
    p->current = p->next;
    p->next    = lexer_next(&p->lexer);
}

static int check(Parser *p, TokenType t) { return p->current.type == t; }

static int match(Parser *p, TokenType t) {
    if (!check(p, t)) return 0;
    parser_advance(p);
    return 1;
}

static void parse_error(Parser *p, const char *msg) {
    fprintf(stderr, "[fluxa] Parse error (line %d): %s (got '%s')\n",
            p->current.line, msg, p->current.value);
    p->had_error = 1;
}

static int expect(Parser *p, TokenType t, const char *ctx) {
    if (check(p, t)) { parser_advance(p); return 1; }
    char buf[128];
    snprintf(buf, sizeof(buf), "expected '%s' %s", token_type_name(t), ctx);
    parse_error(p, buf);
    return 0;
}

/* ── Expression parsing ──────────────────────────────────────────────────── */

static ASTNode *parse_expr(Parser *p);

/* literal or identifier */
static ASTNode *parse_primary(Parser *p) {
    if (check(p, TOK_STRING)) {
        ASTNode *n = ast_string(strdup(p->current.value));
        parser_advance(p);
        return n;
    }
    if (check(p, TOK_INT)) {
        ASTNode *n = ast_integer(atol(p->current.value));
        parser_advance(p);
        return n;
    }
    if (check(p, TOK_FLOAT)) {
        ASTNode *n = ast_float(atof(p->current.value));
        parser_advance(p);
        return n;
    }
    if (check(p, TOK_BOOL)) {
        ASTNode *n = ast_bool(strcmp(p->current.value, "true") == 0 ? 1 : 0);
        parser_advance(p);
        return n;
    }
    if (check(p, TOK_NIL)) {
        ASTNode *n = ast_new(NODE_IDENTIFIER);
        n->as.str.value = strdup("nil");
        parser_advance(p);
        return n;
    }
    /* identifier — may be a function call */
    if (check(p, TOK_IDENT) || check(p, TOK_ERR)) {
        char *name = strdup(p->current.value);
        parser_advance(p);

        /* function call: name(...) */
        if (check(p, TOK_LPAREN)) {
            parser_advance(p); /* consume '(' */
            ASTNode *call = ast_func_call(name);

            /* arguments */
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ASTNode *arg = parse_expr(p);
                if (!arg) { ast_free(call); free(name); return NULL; }
                ast_list_push(call, arg);
                if (!match(p, TOK_COMMA)) break;
            }
            if (!expect(p, TOK_RPAREN, "after function arguments")) {
                ast_free(call);
                free(name);
                return NULL;
            }
            return call;
        }

        /* plain identifier */
        ASTNode *n = ast_new(NODE_IDENTIFIER);
        n->as.str.value = name;
        return n;
    }

    parse_error(p, "unexpected token in expression");
    return NULL;
}

static ASTNode *parse_expr(Parser *p) {
    return parse_primary(p);
}

/* ── Statement parsing ───────────────────────────────────────────────────── */

static ASTNode *parse_statement(Parser *p) {
    /* Sprint 1: top-level function calls only */
    if (check(p, TOK_IDENT)) {
        return parse_expr(p);
    }
    /* built-in print / len */
    if (check(p, TOK_IDENT)) {
        return parse_expr(p);
    }
    parse_error(p, "expected statement");
    parser_advance(p); /* skip bad token to avoid infinite loop */
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

Parser parser_new(const char *source) {
    Parser p;
    p.lexer     = lexer_new(source);
    p.had_error = 0;
    /* prime the two-token lookahead */
    p.current   = lexer_next(&p.lexer);
    p.next      = lexer_next(&p.lexer);
    return p;
}

ASTNode *parser_parse(Parser *p) {
    ASTNode *program = ast_program();

    while (!check(p, TOK_EOF) && !p->had_error) {
        ASTNode *stmt = parse_statement(p);
        if (stmt) ast_list_push(program, stmt);
    }

    if (p->had_error) {
        ast_free(program);
        return NULL;
    }
    return program;
}

void parser_free(Parser *p) {
    token_free(&p->current);
    token_free(&p->next);
}
