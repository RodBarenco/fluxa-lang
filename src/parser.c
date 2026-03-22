/* parser.c — Fluxa Parser implementation */
#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Allocation wrappers (use pool) ──────────────────────────────────────── */
#define P_NODE()       pool_alloc_node(p->pool)
#define P_STR(s)       pool_strdup(p->pool, s)

/* ── AST constructors using pool ─────────────────────────────────────────── */
static ASTNode *p_string(Parser *p, const char *v) {
    ASTNode *n = P_NODE(); n->type = NODE_STRING_LIT;
    n->as.str.value = P_STR(v); return n;
}
static ASTNode *p_integer(Parser *p, long v) {
    ASTNode *n = P_NODE(); n->type = NODE_INT_LIT;
    n->as.integer.value = v; return n;
}
static ASTNode *p_float(Parser *p, double v) {
    ASTNode *n = P_NODE(); n->type = NODE_FLOAT_LIT;
    n->as.real.value = v; return n;
}
static ASTNode *p_bool(Parser *p, int v) {
    ASTNode *n = P_NODE(); n->type = NODE_BOOL_LIT;
    n->as.boolean.value = v; return n;
}
static ASTNode *p_ident(Parser *p, const char *name) {
    ASTNode *n = P_NODE(); n->type = NODE_IDENTIFIER;
    n->as.str.value = P_STR(name); return n;
}
static ASTNode *p_func_call(Parser *p, const char *name) {
    ASTNode *n = P_NODE(); n->type = NODE_FUNC_CALL;
    n->as.list.name = P_STR(name);
    n->as.list.children = NULL; n->as.list.count = 0; return n;
}
static ASTNode *p_program(Parser *p) {
    ASTNode *n = P_NODE(); n->type = NODE_PROGRAM;
    n->as.list.children = NULL; n->as.list.count = 0; return n;
}
static ASTNode *p_var_decl(Parser *p, const char *type_name,
                            const char *var_name, ASTNode *init, int prst) {
    ASTNode *n = P_NODE(); n->type = NODE_VAR_DECL;
    n->as.var_decl.type_name   = P_STR(type_name);
    n->as.var_decl.var_name    = P_STR(var_name);
    n->as.var_decl.initializer = init;
    n->as.var_decl.persistent  = prst; return n;
}
static ASTNode *p_assign(Parser *p, const char *name, ASTNode *val) {
    ASTNode *n = P_NODE(); n->type = NODE_ASSIGN;
    n->as.assign.var_name = P_STR(name);
    n->as.assign.value    = val; return n;
}
static ASTNode *p_binary(Parser *p, const char *op,
                          ASTNode *left, ASTNode *right) {
    ASTNode *n = P_NODE(); n->type = NODE_BINARY_EXPR;
    n->as.binary.op    = P_STR(op);
    n->as.binary.left  = left;
    n->as.binary.right = right; return n;
}

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
    parse_error(p, buf); return 0;
}

/* ── Expression parsing ──────────────────────────────────────────────────── */
static ASTNode *parse_expr(Parser *p);

static ASTNode *parse_primary(Parser *p) {
    if (check(p, TOK_STRING)) {
        ASTNode *n = p_string(p, p->current.value);
        parser_advance(p); return n;
    }
    if (check(p, TOK_INT)) {
        ASTNode *n = p_integer(p, atol(p->current.value));
        parser_advance(p); return n;
    }
    if (check(p, TOK_FLOAT)) {
        ASTNode *n = p_float(p, atof(p->current.value));
        parser_advance(p); return n;
    }
    if (check(p, TOK_BOOL)) {
        ASTNode *n = p_bool(p, strcmp(p->current.value,"true")==0 ? 1 : 0);
        parser_advance(p); return n;
    }
    if (check(p, TOK_NIL)) {
        ASTNode *n = p_ident(p, "nil");
        parser_advance(p); return n;
    }
    if (check(p, TOK_IDENT) || check(p, TOK_ERR)) {
        char name[256];
        strncpy(name, p->current.value, sizeof(name)-1);
        name[sizeof(name)-1] = '\0';
        parser_advance(p);

        /* Issue #16 — array access in expression: name[i] */
        if (check(p, TOK_LBRACKET)) {
            parser_advance(p);
            ASTNode *idx = parse_expr(p);
            if (!expect(p, TOK_RBRACKET, "after array index")) return NULL;
            ASTNode *n = P_NODE();
            n->type                   = NODE_ARR_ACCESS;
            n->as.arr_access.arr_name = P_STR(name);
            n->as.arr_access.index    = idx;
            return n;
        }

        if (check(p, TOK_LPAREN)) {
            parser_advance(p);
            ASTNode *call = p_func_call(p, name);
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ASTNode *arg = parse_expr(p);
                if (!arg) return NULL;
                ast_list_push(call, arg);
                if (!match(p, TOK_COMMA)) break;
            }
            if (!expect(p, TOK_RPAREN, "after function arguments")) return NULL;
            return call;
        }
        return p_ident(p, name);
    }
    if (check(p, TOK_LPAREN)) {
        parser_advance(p);
        ASTNode *inner = parse_expr(p);
        if (!expect(p, TOK_RPAREN, "after expression")) return NULL;
        return inner;
    }
    parse_error(p, "unexpected token in expression");
    return NULL;
}

static int is_multiplicative(Parser *p) {
    TokenType t = p->current.type;
    return t == TOK_STAR || t == TOK_SLASH || t == TOK_PERCENT;
}
static int is_additive(Parser *p) {
    TokenType t = p->current.type;
    return t == TOK_PLUS || t == TOK_MINUS;
}
static int is_comparison(Parser *p) {
    TokenType t = p->current.type;
    return t==TOK_EQEQ||t==TOK_NEQ||t==TOK_LT||t==TOK_GT||t==TOK_LTE||t==TOK_GTE;
}

static ASTNode *parse_term(Parser *p) {
    ASTNode *left = parse_primary(p);
    while (left && is_multiplicative(p)) {
        char op[4]; strncpy(op, p->current.value, 3); op[3]='\0';
        parser_advance(p);
        ASTNode *right = parse_primary(p);
        if (!right) return NULL;
        left = p_binary(p, op, left, right);
    }
    return left;
}
static ASTNode *parse_arith(Parser *p) {
    ASTNode *left = parse_term(p);
    while (left && is_additive(p)) {
        char op[4]; strncpy(op, p->current.value, 3); op[3]='\0';
        parser_advance(p);
        ASTNode *right = parse_term(p);
        if (!right) return NULL;
        left = p_binary(p, op, left, right);
    }
    return left;
}
static ASTNode *parse_expr(Parser *p) {
    ASTNode *left = parse_arith(p);
    if (left && is_comparison(p)) {
        char op[4]; strncpy(op, p->current.value, 3); op[3]='\0';
        parser_advance(p);
        ASTNode *right = parse_arith(p);
        if (!right) return NULL;
        left = p_binary(p, op, left, right);
    }
    return left;
}

/* ── Statement parsing ───────────────────────────────────────────────────── */
static int is_type_token(Parser *p) {
    TokenType t = p->current.type;
    return t==TOK_TYPE_INT||t==TOK_TYPE_FLOAT||t==TOK_TYPE_STR||
           t==TOK_TYPE_BOOL||t==TOK_TYPE_CHAR||t==TOK_TYPE_DYN;
}

static ASTNode *parse_statement(Parser *p);

/* Issue #16 — parse { stmt* } into NODE_BLOCK_STMT */
static ASTNode *parse_body(Parser *p) {
    if (!expect(p, TOK_LBRACE, "expected '{' to open block")) return NULL;
    ASTNode *block = P_NODE();
    block->type = NODE_BLOCK_STMT;
    block->as.list.children = NULL;
    block->as.list.count    = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->had_error) {
        ASTNode *stmt = parse_statement(p);
        if (stmt) ast_list_push(block, stmt);
    }
    if (!expect(p, TOK_RBRACE, "expected '}' to close block")) return NULL;
    return block;
}

static ASTNode *parse_statement(Parser *p) {
    int persistent = 0;
    if (check(p, TOK_PRST)) { persistent = 1; parser_advance(p); }

    /* Issue #20 — fn declaration */
    if (check(p, TOK_FN)) {
        parser_advance(p);

        if (!check(p, TOK_IDENT)) {
            parse_error(p, "expected function name after 'fn'");
            return NULL;
        }
        char fn_name[256];
        strncpy(fn_name, p->current.value, sizeof(fn_name)-1);
        fn_name[sizeof(fn_name)-1] = '\0';
        parser_advance(p);

        if (!expect(p, TOK_LPAREN, "after function name")) return NULL;

        /* parse parameters */
        char **param_names = NULL;
        char **param_types = NULL;
        int    param_count = 0;

        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            if (!is_type_token(p)) {
                parse_error(p, "expected type in parameter list");
                free(param_names); free(param_types);
                return NULL;
            }
            param_count++;
            param_types = (char**)realloc(param_types,
                sizeof(char*) * param_count);
            param_names = (char**)realloc(param_names,
                sizeof(char*) * param_count);
            param_types[param_count-1] = P_STR(p->current.value);
            parser_advance(p);

            if (!check(p, TOK_IDENT)) {
                parse_error(p, "expected parameter name after type");
                free(param_names); free(param_types);
                return NULL;
            }
            param_names[param_count-1] = P_STR(p->current.value);
            parser_advance(p);

            if (!match(p, TOK_COMMA)) break;
        }
        if (!expect(p, TOK_RPAREN, "after parameter list")) {
            free(param_names); free(param_types);
            return NULL;
        }

        /* return type */
        char ret_type[32] = "nil";
        if (check(p, TOK_NIL)) {
            parser_advance(p);
        } else if (is_type_token(p)) {
            strncpy(ret_type, p->current.value, sizeof(ret_type)-1);
            ret_type[sizeof(ret_type)-1] = '\0';
            parser_advance(p);
        } else {
            parse_error(p, "expected return type after parameter list");
            free(param_names); free(param_types);
            return NULL;
        }

        ASTNode *body = parse_body(p);
        if (!body) { free(param_names); free(param_types); return NULL; }

        ASTNode *n = P_NODE();
        n->type = NODE_FUNC_DECL;
        n->as.func_decl.name         = P_STR(fn_name);
        n->as.func_decl.param_names  = param_names;
        n->as.func_decl.param_types  = param_types;
        n->as.func_decl.param_count  = param_count;
        n->as.func_decl.return_type  = P_STR(ret_type);
        n->as.func_decl.body         = body;
        return n;
    }

    /* Issue #20 — return statement */
    if (check(p, TOK_RETURN)) {
        parser_advance(p);
        ASTNode *n = P_NODE();
        n->type = NODE_RETURN;
        /* bare return (nil functions) vs return <expr> */
        if (check(p, TOK_RBRACE) || check(p, TOK_EOF))
            n->as.ret.value = NULL;
        else
            n->as.ret.value = parse_expr(p);
        return n;
    }

    /* Issue #16 — if statement */
    if (check(p, TOK_IF)) {
        parser_advance(p);
        ASTNode *cond = parse_expr(p);
        if (!cond) return NULL;
        ASTNode *then_body = parse_body(p);
        if (!then_body) return NULL;
        ASTNode *else_body = NULL;
        if (check(p, TOK_ELSE)) {
            parser_advance(p);
            else_body = parse_body(p);
            if (!else_body) return NULL;
        }
        ASTNode *n = P_NODE();
        n->type                 = NODE_IF;
        n->as.if_stmt.condition = cond;
        n->as.if_stmt.then_body = then_body;
        n->as.if_stmt.else_body = else_body;
        return n;
    }

    /* Issue #16 — while statement */
    if (check(p, TOK_WHILE)) {
        parser_advance(p);
        ASTNode *cond = parse_expr(p);
        if (!cond) return NULL;
        ASTNode *body = parse_body(p);
        if (!body) return NULL;
        ASTNode *n = P_NODE();
        n->type                   = NODE_WHILE;
        n->as.while_stmt.condition = cond;
        n->as.while_stmt.body      = body;
        return n;
    }

    /* Issue #16 — for statement: for <ident> in <ident> { } */
    if (check(p, TOK_FOR)) {
        parser_advance(p);
        if (!check(p, TOK_IDENT)) {
            parse_error(p, "expected loop variable after 'for'");
            return NULL;
        }
        char var_name[256];
        strncpy(var_name, p->current.value, sizeof(var_name)-1);
        var_name[sizeof(var_name)-1] = '\0';
        parser_advance(p);

        /* 'in' is not a keyword — parsed as identifier */
        if (!check(p, TOK_IDENT) || strcmp(p->current.value, "in") != 0) {
            parse_error(p, "expected 'in' after loop variable");
            return NULL;
        }
        parser_advance(p);

        if (!check(p, TOK_IDENT)) {
            parse_error(p, "expected array name after 'in'");
            return NULL;
        }
        char arr_name[256];
        strncpy(arr_name, p->current.value, sizeof(arr_name)-1);
        arr_name[sizeof(arr_name)-1] = '\0';
        parser_advance(p);

        ASTNode *body = parse_body(p);
        if (!body) return NULL;

        ASTNode *n = P_NODE();
        n->type                = NODE_FOR;
        n->as.for_stmt.var_name = P_STR(var_name);
        n->as.for_stmt.arr_name = P_STR(arr_name);
        n->as.for_stmt.body     = body;
        return n;
    }

    /* var/arr declaration: [prst] <type> [arr] <name> ... */
    if (is_type_token(p)) {
        char type_name[32];
        strncpy(type_name, p->current.value, sizeof(type_name)-1);
        type_name[sizeof(type_name)-1] = '\0';
        parser_advance(p);

        /* Issue #16 — arr declaration: <type> arr <name>[<size>] = [elems] */
        if (check(p, TOK_TYPE_ARR)) {
            parser_advance(p);
            if (!check(p, TOK_IDENT)) {
                parse_error(p, "expected array name after 'arr'");
                return NULL;
            }
            char arr_name[256];
            strncpy(arr_name, p->current.value, sizeof(arr_name)-1);
            arr_name[sizeof(arr_name)-1] = '\0';
            parser_advance(p);

            if (!expect(p, TOK_LBRACKET, "after array name")) return NULL;
            if (!check(p, TOK_INT)) {
                parse_error(p, "expected integer size in array declaration");
                return NULL;
            }
            int size = (int)atol(p->current.value);
            parser_advance(p);
            if (!expect(p, TOK_RBRACKET, "after array size")) return NULL;
            if (!expect(p, TOK_EQ, "after array declaration")) return NULL;
            if (!expect(p, TOK_LBRACKET, "to open array literal")) return NULL;

            ASTNode **elems = (ASTNode**)malloc(sizeof(ASTNode*) * size);
            int count = 0;
            while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
                if (count < size) elems[count++] = parse_expr(p);
                else { parse_expr(p); } /* consume extra */
                if (!match(p, TOK_COMMA)) break;
            }
            if (!expect(p, TOK_RBRACKET, "to close array literal")) {
                free(elems); return NULL;
            }

            ASTNode *n = P_NODE();
            n->type                  = NODE_ARR_DECL;
            n->as.arr_decl.type_name = P_STR(type_name);
            n->as.arr_decl.arr_name  = P_STR(arr_name);
            n->as.arr_decl.size      = size;
            n->as.arr_decl.elements  = elems;
            n->as.arr_decl.persistent = persistent;
            return n;
        }

        /* plain var declaration */
        if (!check(p, TOK_IDENT)) {
            parse_error(p, "expected variable name after type");
            return NULL;
        }
        char var_name[256];
        strncpy(var_name, p->current.value, sizeof(var_name)-1);
        var_name[sizeof(var_name)-1] = '\0';
        parser_advance(p);

        if (!expect(p, TOK_EQ, "after variable name")) return NULL;
        ASTNode *init = parse_expr(p);
        if (!init) return NULL;
        return p_var_decl(p, type_name, var_name, init, persistent);
    }

    if (persistent) { parse_error(p, "expected type after 'prst'"); return NULL; }

    if (check(p, TOK_IDENT)) {
        char name[256];
        strncpy(name, p->current.value, sizeof(name)-1);
        name[sizeof(name)-1] = '\0';
        parser_advance(p);

        /* Issue #16 — arr[i] = val */
        if (check(p, TOK_LBRACKET)) {
            parser_advance(p);
            ASTNode *idx = parse_expr(p);
            if (!expect(p, TOK_RBRACKET, "after array index")) return NULL;
            if (!expect(p, TOK_EQ, "after array access")) return NULL;
            ASTNode *val = parse_expr(p);
            if (!val) return NULL;
            ASTNode *n = P_NODE();
            n->type               = NODE_ARR_ASSIGN;
            n->as.arr_assign.arr_name = P_STR(name);
            n->as.arr_assign.index    = idx;
            n->as.arr_assign.value    = val;
            return n;
        }

        if (check(p, TOK_EQ)) {
            parser_advance(p);
            ASTNode *val = parse_expr(p);
            if (!val) return NULL;
            return p_assign(p, name, val);
        }
        if (check(p, TOK_LPAREN)) {
            parser_advance(p);
            ASTNode *call = p_func_call(p, name);
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ASTNode *arg = parse_expr(p);
                if (!arg) return NULL;
                ast_list_push(call, arg);
                if (!match(p, TOK_COMMA)) break;
            }
            if (!expect(p, TOK_RPAREN, "after function arguments")) return NULL;
            return call;
        }
        parse_error(p, "expected '=', '[' or '(' after identifier");
        return NULL;
    }

    parse_error(p, "expected statement");
    parser_advance(p);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
Parser parser_new(const char *source, ASTPool *pool) {
    Parser p;
    p.lexer     = lexer_new(source);
    p.had_error = 0;
    p.pool      = pool;
    p.current   = lexer_next(&p.lexer);
    p.next      = lexer_next(&p.lexer);
    return p;
}

ASTNode *parser_parse(Parser *p) {
    ASTNode *program = p_program(p);
    while (!check(p, TOK_EOF) && !p->had_error) {
        ASTNode *stmt = parse_statement(p);
        if (stmt) ast_list_push(program, stmt);
    }
    if (p->had_error) return NULL;
    return program;
}

void parser_free(Parser *p) {
    token_free(&p->current);
    token_free(&p->next);
}
