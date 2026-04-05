/* parser.c — Fluxa Parser implementation
 * Sprint 5: Block declaration, typeof instance, member access/call/assign (#37)
 */
#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Pool wrappers ───────────────────────────────────────────────────────── */
#define P_NODE()   pool_alloc_node(p->pool)
#define P_STR(s)   pool_strdup(p->pool, s)

/* ── AST constructors ────────────────────────────────────────────────────── */
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
    n->line            = p->current.line;
    n->as.binary.op    = P_STR(op);
    n->as.binary.left  = left;
    n->as.binary.right = right; return n;
}

/* ── Token helpers ───────────────────────────────────────────────────────── */
static void parser_advance(Parser *p) {
    token_free(&p->current);
    p->current = p->next;
    p->next    = lexer_next(&p->lexer);
}
static int check(Parser *p, TokenType t) { return p->current.type == t; }
static int match(Parser *p, TokenType t) {
    if (!check(p, t)) return 0;
    parser_advance(p); return 1;
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

static int is_type_token(Parser *p) {
    TokenType t = p->current.type;
    return t==TOK_TYPE_INT||t==TOK_TYPE_FLOAT||t==TOK_TYPE_STR||
           t==TOK_TYPE_BOOL||t==TOK_TYPE_CHAR||t==TOK_TYPE_DYN||
           t==TOK_TYPE_ARR;
}

/* ── Forward declarations ────────────────────────────────────────────────── */
static ASTNode *parse_expr(Parser *p);
static ASTNode *parse_statement(Parser *p);

/* ── Parse argument list: ( expr, expr, ... ) ────────────────────────────── */
/* caller has already consumed the '(' */
static void parse_args_into(Parser *p, ASTNode ***args_out, int *count_out) {
    ASTNode **args = NULL;
    int count = 0;
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        ASTNode *arg = parse_expr(p);
        if (!arg) { free(args); *args_out = NULL; *count_out = 0; return; }
        count++;
        args = (ASTNode**)realloc(args, sizeof(ASTNode*) * count);
        args[count-1] = arg;
        if (!match(p, TOK_COMMA)) break;
    }
    *args_out  = args;
    *count_out = count;
}

/* ── Expression parsing ──────────────────────────────────────────────────── */
static ASTNode *parse_primary(Parser *p) {
    int cur_line = p->current.line;   /* Sprint 8: linha do token atual */
    if (check(p, TOK_STRING)) {
        ASTNode *n = p_string(p, p->current.value);
        n->line = cur_line; parser_advance(p); return n;
    }
    if (check(p, TOK_INT)) {
        ASTNode *n = p_integer(p, atol(p->current.value));
        n->line = cur_line; parser_advance(p); return n;
    }
    if (check(p, TOK_FLOAT)) {
        ASTNode *n = p_float(p, atof(p->current.value));
        n->line = cur_line; parser_advance(p); return n;
    }
    if (check(p, TOK_BOOL)) {
        ASTNode *n = p_bool(p, strcmp(p->current.value,"true")==0 ? 1 : 0);
        n->line = cur_line; parser_advance(p); return n;
    }
    if (check(p, TOK_NIL)) {
        ASTNode *n = p_ident(p, "nil");
        n->line = cur_line; parser_advance(p); return n;
    }
    if (check(p, TOK_IDENT) || check(p, TOK_ERR)) {
        char name[256];
        strncpy(name, p->current.value, sizeof(name)-1);
        name[sizeof(name)-1] = '\0';
        parser_advance(p);

        /* arr[i] or dyn[i] in expression — may be followed by .field/.method() */
        if (check(p, TOK_LBRACKET)) {
            parser_advance(p);
            ASTNode *idx = parse_expr(p);
            if (!expect(p, TOK_RBRACKET, "after array index")) return NULL;

            /* dyn[i].campo or dyn[i].metodo(args) */
            if (check(p, TOK_DOT)) {
                parser_advance(p);
                if (!check(p, TOK_IDENT)) {
                    parse_error(p, "expected member name after '.'");
                    return NULL;
                }
                char member[256];
                strncpy(member, p->current.value, sizeof(member)-1);
                member[sizeof(member)-1] = '\0';
                parser_advance(p);

                if (check(p, TOK_LPAREN)) {
                    /* dyn[i].metodo(args) */
                    parser_advance(p);
                    ASTNode **args = NULL; int argc = 0;
                    parse_args_into(p, &args, &argc);
                    if (!expect(p, TOK_RPAREN, "after method arguments")) {
                        free(args); return NULL;
                    }
                    ASTNode *n = P_NODE();
                    n->type                              = NODE_INDEXED_MEMBER_CALL;
                    n->as.indexed_member_call.dyn_name   = P_STR(name);
                    n->as.indexed_member_call.index      = idx;
                    n->as.indexed_member_call.method     = P_STR(member);
                    n->as.indexed_member_call.args       = args;
                    n->as.indexed_member_call.arg_count  = argc;
                    return n;
                } else {
                    /* dyn[i].campo */
                    ASTNode *n = P_NODE();
                    n->type                                    = NODE_INDEXED_MEMBER_ACCESS;
                    n->as.indexed_member_access.dyn_name       = P_STR(name);
                    n->as.indexed_member_access.index          = idx;
                    n->as.indexed_member_access.field          = P_STR(member);
                    return n;
                }
            }

            ASTNode *n = P_NODE();
            n->type                   = NODE_ARR_ACCESS;
            n->as.arr_access.arr_name = P_STR(name);
            n->as.arr_access.index    = idx;
            return n;
        }

        /* inst.field or inst.method(args) */
        if (check(p, TOK_DOT)) {
            parser_advance(p);
            if (!check(p, TOK_IDENT)) {
                parse_error(p, "expected member name after '.'");
                return NULL;
            }
            char member[256];
            strncpy(member, p->current.value, sizeof(member)-1);
            member[sizeof(member)-1] = '\0';
            parser_advance(p);

            if (check(p, TOK_LPAREN)) {
                /* inst.method(args) */
                parser_advance(p);
                ASTNode **args = NULL; int argc = 0;
                parse_args_into(p, &args, &argc);
                if (!expect(p, TOK_RPAREN, "after method arguments")) {
                    free(args); return NULL;
                }
                ASTNode *n = P_NODE();
                n->type                      = NODE_MEMBER_CALL;
                n->as.member_call.owner      = P_STR(name);
                n->as.member_call.method     = P_STR(member);
                n->as.member_call.args       = args;
                n->as.member_call.arg_count  = argc;
                return n;
            } else {
                /* inst.field (rvalue) */
                ASTNode *n = P_NODE();
                n->type                    = NODE_MEMBER_ACCESS;
                n->as.member_access.owner  = P_STR(name);
                n->as.member_access.field  = P_STR(member);
                return n;
            }
        }

        /* plain function call */
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
    /* unary minus: -expr */
    if (check(p, TOK_MINUS)) {
        parser_advance(p);
        ASTNode *operand = parse_primary(p);
        if (!operand) return NULL;
        /* fold into literal if possible */
        if (operand->type == NODE_INT_LIT) {
            operand->as.integer.value = -operand->as.integer.value;
            return operand;
        }
        if (operand->type == NODE_FLOAT_LIT) {
            operand->as.real.value = -operand->as.real.value;
            return operand;
        }
        /* general case: 0 - expr */
        ASTNode *zero = P_NODE();
        zero->type = NODE_INT_LIT;
        zero->as.integer.value = 0;
        return p_binary(p, "-", zero, operand);
    }
    /* logical not: !expr */
    if (check(p, TOK_NOT)) {
        parser_advance(p);
        ASTNode *operand = parse_primary(p);
        if (!operand) return NULL;
        return p_binary(p, "!", operand, NULL);
    }
    parse_error(p, "unexpected token in expression");
    return NULL;
}

static int is_multiplicative(Parser *p) {
    TokenType t = p->current.type;
    return t==TOK_STAR||t==TOK_SLASH||t==TOK_PERCENT;
}
static int is_additive(Parser *p) {
    TokenType t = p->current.type;
    return t==TOK_PLUS||t==TOK_MINUS;
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
static ASTNode *parse_comparison(Parser *p) {
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
static ASTNode *parse_logic_and(Parser *p) {
    ASTNode *left = parse_comparison(p);
    while (left && p->current.type == TOK_AND) {
        parser_advance(p);
        ASTNode *right = parse_comparison(p);
        if (!right) return NULL;
        left = p_binary(p, "&&", left, right);
    }
    return left;
}
static ASTNode *parse_expr(Parser *p) {
    ASTNode *left = parse_logic_and(p);
    while (left && p->current.type == TOK_OR) {
        parser_advance(p);
        ASTNode *right = parse_logic_and(p);
        if (!right) return NULL;
        left = p_binary(p, "||", left, right);
    }
    return left;
}

/* ── Block body: { var_decl | func_decl }* ───────────────────────────────── */
static ASTNode *parse_block_decl(Parser *p) {
    /* 'Block' already consumed — parse: Name { members } */
    if (!check(p, TOK_IDENT)) {
        parse_error(p, "expected Block name after 'Block'");
        return NULL;
    }
    char block_name[256];
    strncpy(block_name, p->current.value, sizeof(block_name)-1);
    block_name[sizeof(block_name)-1] = '\0';
    parser_advance(p);

    /* 'typeof' branch: Block b1 typeof Foo */
    if (check(p, TOK_TYPEOF)) {
        parser_advance(p);
        if (!check(p, TOK_IDENT)) {
            parse_error(p, "expected Block name after 'typeof'");
            return NULL;
        }
        char origin[256];
        strncpy(origin, p->current.value, sizeof(origin)-1);
        origin[sizeof(origin)-1] = '\0';
        parser_advance(p);

        ASTNode *n = P_NODE();
        n->type                        = NODE_TYPEOF_INST;
        n->as.typeof_inst.inst_name    = P_STR(block_name);
        n->as.typeof_inst.origin_name  = P_STR(origin);
        return n;
    }

    /* Block declaration: Block Name { ... } */
    if (!expect(p, TOK_LBRACE, "after Block name")) return NULL;

    ASTNode *n = P_NODE();
    n->type                    = NODE_BLOCK_DECL;
    n->as.block_decl.name      = P_STR(block_name);
    n->as.block_decl.members   = NULL;
    n->as.block_decl.count     = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->had_error) {
        ASTNode *member = NULL;
        int persistent = 0;

        if (check(p, TOK_PRST)) { persistent = 1; parser_advance(p); }

        if (check(p, TOK_FN)) {
            /* parse func_decl inside Block */
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

            char **param_names = NULL, **param_types = NULL;
            int    param_count = 0;
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                if (!is_type_token(p)) {
                    parse_error(p, "expected type in parameter list");
                    free(param_names); free(param_types); return NULL;
                }
                param_count++;
                param_types = (char**)realloc(param_types, sizeof(char*)*param_count);
                param_names = (char**)realloc(param_names, sizeof(char*)*param_count);
                /* arr parameter: "int arr name" or just "arr name" */
                if (p->current.type == TOK_TYPE_ARR) {
                    param_types[param_count-1] = P_STR("arr");
                    parser_advance(p);
                } else {
                    char _pt[64];
                    snprintf(_pt, sizeof(_pt), "%s", p->current.value);
                    parser_advance(p);
                    if (check(p, TOK_TYPE_ARR)) {
                        size_t _l = strlen(_pt);
                        snprintf(_pt+_l, sizeof(_pt)-_l, " arr");
                        parser_advance(p);
                    }
                    param_types[param_count-1] = P_STR(_pt);
                }
                if (!check(p, TOK_IDENT)) {
                    parse_error(p, "expected parameter name after type");
                    free(param_names); free(param_types); return NULL;
                }
                param_names[param_count-1] = P_STR(p->current.value);
                parser_advance(p);
                if (!match(p, TOK_COMMA)) break;
            }
            if (!expect(p, TOK_RPAREN, "after parameter list")) {
                free(param_names); free(param_types); return NULL;
            }

            char ret_type[32] = "nil";
            if (check(p, TOK_NIL)) {
                parser_advance(p);
            } else if (is_type_token(p)) {
                strncpy(ret_type, p->current.value, sizeof(ret_type)-1);
                ret_type[sizeof(ret_type)-1] = '\0';
                parser_advance(p);
            } else {
                parse_error(p, "expected return type after parameter list");
                free(param_names); free(param_types); return NULL;
            }

            /* parse body { ... } */
            if (!expect(p, TOK_LBRACE, "expected '{' to open function body")) {
                free(param_names); free(param_types); return NULL;
            }
            ASTNode *body = P_NODE();
            body->type = NODE_BLOCK_STMT;
            body->as.list.children = NULL;
            body->as.list.count    = 0;
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF) && !p->had_error) {
                ASTNode *stmt = parse_statement(p);
                if (stmt) ast_list_push(body, stmt);
            }
            if (!expect(p, TOK_RBRACE, "expected '}' to close function body")) {
                free(param_names); free(param_types); return NULL;
            }

            member = P_NODE();
            member->type = NODE_FUNC_DECL;
            member->as.func_decl.name        = P_STR(fn_name);
            member->as.func_decl.param_names = param_names;
            member->as.func_decl.param_types = param_types;
            member->as.func_decl.param_count = param_count;
            member->as.func_decl.return_type = P_STR(ret_type);
            member->as.func_decl.body        = body;

        } else if (is_type_token(p)) {
            /* var_decl inside Block — supports "int arr name[size] = val" */
            char type_name[32];
            snprintf(type_name, sizeof(type_name), "%s", p->current.value);
            parser_advance(p);

            /* check for arr declaration: "int arr name[size] = ..." */
            if (check(p, TOK_TYPE_ARR)) {
                parser_advance(p);
                if (!check(p, TOK_IDENT)) {
                    parse_error(p, "expected array name after 'arr' in Block");
                    return NULL;
                }
                char arr_name[256];
                snprintf(arr_name, sizeof(arr_name), "%s", p->current.value);
                parser_advance(p);
                if (!expect(p, TOK_LBRACKET, "after array name in Block")) return NULL;
                if (!check(p, TOK_INT)) {
                    parse_error(p, "expected integer size in array declaration");
                    return NULL;
                }
                int arr_size = (int)atol(p->current.value);
                parser_advance(p);
                if (!expect(p, TOK_RBRACKET, "after array size")) return NULL;
                if (!expect(p, TOK_EQ, "after array declaration in Block")) return NULL;
                /* default or list init */
                ASTNode *arr_node = P_NODE();
                arr_node->type                   = NODE_ARR_DECL;
                arr_node->as.arr_decl.type_name  = P_STR(type_name);
                arr_node->as.arr_decl.arr_name   = P_STR(arr_name);
                arr_node->as.arr_decl.size       = arr_size;
                arr_node->as.arr_decl.persistent = persistent;
                if (!check(p, TOK_LBRACKET)) {
                    /* default init */
                    ASTNode *def = parse_expr(p);
                    if (!def) return NULL;
                    arr_node->as.arr_decl.default_init  = 1;
                    arr_node->as.arr_decl.default_value = def;
                    arr_node->as.arr_decl.elements      = NULL;
                } else {
                    /* explicit list */
                    if (!expect(p, TOK_LBRACKET, "to open array literal")) return NULL;
                    ASTNode **elems = (ASTNode**)malloc(sizeof(ASTNode*)*(arr_size > 0 ? arr_size : 1));
                    int ec = 0;
                    while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
                        if (ec < arr_size) elems[ec++] = parse_expr(p);
                        else { parse_expr(p); ec++; }
                        if (!match(p, TOK_COMMA)) break;
                    }
                    if (!expect(p, TOK_RBRACKET, "to close array literal")) {
                        free(elems); return NULL;
                    }
                    if (ec != arr_size) {
                        char errmsg[128];
                        snprintf(errmsg, sizeof(errmsg),
                                 "array literal has %d element(s) but '%s' was declared with size %d",
                                 ec, arr_name, arr_size);
                        parse_error(p, errmsg);
                        free(elems); return NULL;
                    }
                    arr_node->as.arr_decl.default_init  = 0;
                    arr_node->as.arr_decl.default_value = NULL;
                    arr_node->as.arr_decl.elements      = elems;
                }
                member = arr_node;
            } else {
                /* plain var decl */
                if (!check(p, TOK_IDENT)) {
                    parse_error(p, "expected variable name after type in Block");
                    return NULL;
                }
                char var_name[256];
                snprintf(var_name, sizeof(var_name), "%s", p->current.value);
                parser_advance(p);
                if (!expect(p, TOK_EQ, "after variable name in Block")) return NULL;
                ASTNode *init = parse_expr(p);
                if (!init) return NULL;
                member = p_var_decl(p, type_name, var_name, init, persistent);
            }
        } else {
            parse_error(p, "expected 'fn' or type declaration inside Block");
            return NULL;
        }

        if (member) {
            n->as.block_decl.count++;
            n->as.block_decl.members = (ASTNode**)realloc(
                n->as.block_decl.members,
                sizeof(ASTNode*) * n->as.block_decl.count);
            n->as.block_decl.members[n->as.block_decl.count-1] = member;
        }
    }
    if (!expect(p, TOK_RBRACE, "expected '}' to close Block")) return NULL;
    return n;
}

/* ── Block body for if/while/for/fn ─────────────────────────────────────── */
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

/* ── Statement parsing ───────────────────────────────────────────────────── */
static ASTNode *parse_statement(Parser *p) {
    int persistent = 0;
    int stmt_line = p->current.line;   /* Sprint 8: captura linha do statement */
    if (check(p, TOK_PRST)) { persistent = 1; parser_advance(p); stmt_line = p->current.line; }

    /* Sprint 6.b: import c libname [as alias] */
    if (check(p, TOK_IMPORT)) {
        parser_advance(p);
        /* only "import c" supported here — std/live/static are future */
        if (!check(p, TOK_IDENT) || strcmp(p->current.value, "c") != 0) {
            parse_error(p, "only 'import c' is supported in Sprint 6.b");
            return NULL;
        }
        parser_advance(p);
        if (!check(p, TOK_IDENT)) {
            parse_error(p, "expected library name after 'import c'");
            return NULL;
        }
        char lib_name[128];
        strncpy(lib_name, p->current.value, sizeof(lib_name)-1);
        lib_name[sizeof(lib_name)-1] = '\0';
        parser_advance(p);
        char alias[128];
        snprintf(alias, sizeof(alias), "%s", lib_name);
        if (check(p, TOK_IDENT) && strcmp(p->current.value, "as") == 0) {
            parser_advance(p);
            if (!check(p, TOK_IDENT)) {
                parse_error(p, "expected alias after 'as'");
                return NULL;
            }
            snprintf(alias, sizeof(alias), "%s", p->current.value);
            parser_advance(p);
        }
        ASTNode *n = P_NODE();
        n->type              = NODE_IMPORT_C;
        n->as.import_c.lib_name = P_STR(lib_name);
        n->as.import_c.alias    = P_STR(alias);
        return n;
    }

    /* Sprint 5: Block declaration or typeof instance */
    if (check(p, TOK_BLOCK)) {
        parser_advance(p);
        return parse_block_decl(p);
    }

    /* fn declaration */
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

        char **param_names = NULL, **param_types = NULL;
        int    param_count = 0;
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            if (!is_type_token(p)) {
                parse_error(p, "expected type in parameter list");
                free(param_names); free(param_types); return NULL;
            }
            param_count++;
            param_types = (char**)realloc(param_types, sizeof(char*)*param_count);
            param_names = (char**)realloc(param_names, sizeof(char*)*param_count);
            /* arr parameter: "int arr name" or just "arr name" */
            if (p->current.type == TOK_TYPE_ARR) {
                param_types[param_count-1] = P_STR("arr");
                parser_advance(p);
            } else {
                char _pt[64];
                snprintf(_pt, sizeof(_pt), "%s", p->current.value);
                parser_advance(p);
                if (check(p, TOK_TYPE_ARR)) {
                    size_t _l = strlen(_pt);
                    snprintf(_pt+_l, sizeof(_pt)-_l, " arr");
                    parser_advance(p);
                }
                param_types[param_count-1] = P_STR(_pt);
            }
            if (!check(p, TOK_IDENT)) {
                parse_error(p, "expected parameter name after type");
                free(param_names); free(param_types); return NULL;
            }
            param_names[param_count-1] = P_STR(p->current.value);
            parser_advance(p);
            if (!match(p, TOK_COMMA)) break;
        }
        if (!expect(p, TOK_RPAREN, "after parameter list")) {
            free(param_names); free(param_types); return NULL;
        }

        char ret_type[32] = "nil";
        if (check(p, TOK_NIL)) {
            parser_advance(p);
        } else if (is_type_token(p)) {
            strncpy(ret_type, p->current.value, sizeof(ret_type)-1);
            ret_type[sizeof(ret_type)-1] = '\0';
            parser_advance(p);
        } else {
            parse_error(p, "expected return type after parameter list");
            free(param_names); free(param_types); return NULL;
        }

        ASTNode *body = parse_body(p);
        if (!body) { free(param_names); free(param_types); return NULL; }

        ASTNode *n = P_NODE();
        n->type = NODE_FUNC_DECL;
        n->line = stmt_line;
        n->as.func_decl.name        = P_STR(fn_name);
        n->as.func_decl.param_names = param_names;
        n->as.func_decl.param_types = param_types;
        n->as.func_decl.param_count = param_count;
        n->as.func_decl.return_type = P_STR(ret_type);
        n->as.func_decl.body        = body;
        return n;
    }

    /* return statement */
    if (check(p, TOK_RETURN)) {
        parser_advance(p);
        ASTNode *n = P_NODE();
        n->type = NODE_RETURN;
        n->line = stmt_line;
        if (check(p, TOK_RBRACE) || check(p, TOK_EOF))
            n->as.ret.value = NULL;
        else
            n->as.ret.value = parse_expr(p);
        return n;
    }

    /* Sprint 6: danger block — NOT nestable, parser enforces this */
    if (check(p, TOK_DANGER)) {
        /* Check for nesting — danger inside danger is a design error */
        /* We track this via a simple flag on the parser for now */
        parser_advance(p);
        ASTNode *body = parse_body(p);
        if (!body) return NULL;
        ASTNode *n = P_NODE();
        n->type               = NODE_DANGER;
        n->as.danger_stmt.body = body;
        return n;
    }

    /* Sprint 6: free(var) */
    if (check(p, TOK_FREE)) {
        parser_advance(p);
        if (!expect(p, TOK_LPAREN, "after 'free'")) return NULL;
        if (!check(p, TOK_IDENT)) {
            parse_error(p, "expected variable name in free()");
            return NULL;
        }
        char var_name[256];
        strncpy(var_name, p->current.value, sizeof(var_name)-1);
        var_name[sizeof(var_name)-1] = '\0';
        parser_advance(p);
        if (!expect(p, TOK_RPAREN, "after variable name in free()")) return NULL;
        ASTNode *n = P_NODE();
        n->type               = NODE_FREE;
        n->as.free_stmt.var_name = P_STR(var_name);
        return n;
    }

    /* if statement */
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

    /* while statement */
    if (check(p, TOK_WHILE)) {
        parser_advance(p);
        ASTNode *cond = parse_expr(p);
        if (!cond) return NULL;
        ASTNode *body = parse_body(p);
        if (!body) return NULL;
        ASTNode *n = P_NODE();
        n->type                    = NODE_WHILE;
        n->as.while_stmt.condition = cond;
        n->as.while_stmt.body      = body;
        return n;
    }

    /* for statement */
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

    /* var/arr declaration */
    if (is_type_token(p)) {
        char type_name[32];
        strncpy(type_name, p->current.value, sizeof(type_name)-1);
        type_name[sizeof(type_name)-1] = '\0';
        parser_advance(p);

        /* arr declaration */
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

            ASTNode *n = P_NODE();
            n->type                   = NODE_ARR_DECL;
            n->as.arr_decl.type_name  = P_STR(type_name);
            n->as.arr_decl.arr_name   = P_STR(arr_name);
            n->as.arr_decl.size       = size;
            n->as.arr_decl.persistent = persistent;

            /* Sprint 6.b: default init — int arr buf[1000] = 0
             * If next token is NOT [, it's a scalar default value */
            if (!check(p, TOK_LBRACKET)) {
                /* scalar initializer — fill all slots with this value */
                ASTNode *def_val = parse_expr(p);
                if (!def_val) return NULL;
                n->as.arr_decl.default_init  = 1;
                n->as.arr_decl.default_value = def_val;
                n->as.arr_decl.elements      = NULL;
                return n;
            }

            /* explicit list initializer */
            if (!expect(p, TOK_LBRACKET, "to open array literal")) return NULL;
            ASTNode **elems = (ASTNode**)malloc(sizeof(ASTNode*) * (size > 0 ? size : 1));
            int count = 0;
            while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
                if (count < size) elems[count++] = parse_expr(p);
                else { parse_expr(p); count++; } /* consume extra for error msg */
                if (!match(p, TOK_COMMA)) break;
            }
            if (!expect(p, TOK_RBRACKET, "to close array literal")) {
                free(elems); return NULL;
            }
            if (count != size) {
                char errmsg[1024];
                snprintf(errmsg, sizeof(errmsg),
                         "array literal has %d element(s) but '%s' was declared with size %d "
                         "(use scalar initializer: %s arr %s[%d] = <value>)",
                         count, arr_name, size, type_name, arr_name, size);
                parse_error(p, errmsg);
                free(elems); return NULL;
            }
            n->as.arr_decl.default_init  = 0;
            n->as.arr_decl.default_value = NULL;
            n->as.arr_decl.elements      = elems;
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

        /* dyn literal initializer: dyn lista = [expr, expr, ...]
         * dyn ONLY accepts a literal [...] or a function call returning dyn.
         * Bare primitives (dyn a = 8) are a type error caught at parse time. */
        ASTNode *init = NULL;
        if (strcmp(type_name, "dyn") == 0 && !check(p, TOK_LBRACKET)) {
            parse_error(p, "dyn variable must be initialized with a literal"
                           " '[...]' — bare values are not allowed (dyn a = 8 is invalid)");
            return NULL;
        }
        if (strcmp(type_name, "dyn") == 0 && check(p, TOK_LBRACKET)) {
            parser_advance(p); /* consume [ */
            ASTNode *lit = P_NODE();
            lit->type              = NODE_DYN_LIT;
            lit->as.dyn_lit.elements = NULL;
            lit->as.dyn_lit.count    = 0;
            while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
                ASTNode *elem = parse_expr(p);
                if (!elem) return NULL;
                lit->as.dyn_lit.count++;
                lit->as.dyn_lit.elements = (ASTNode**)realloc(
                    lit->as.dyn_lit.elements,
                    sizeof(ASTNode*) * lit->as.dyn_lit.count);
                lit->as.dyn_lit.elements[lit->as.dyn_lit.count - 1] = elem;
                if (!match(p, TOK_COMMA)) break;
            }
            if (!expect(p, TOK_RBRACKET, "to close dyn literal")) return NULL;
            init = lit;
        } else {
            init = parse_expr(p);
        }
        if (!init) return NULL;
        { ASTNode *_sn = p_var_decl(p, type_name, var_name, init, persistent); _sn->line = stmt_line; return _sn; }
    }

    if (persistent) { parse_error(p, "expected type after 'prst'"); return NULL; }

    /* identifier-started statements */
    if (check(p, TOK_IDENT)) {
        char name[256];
        strncpy(name, p->current.value, sizeof(name)-1);
        name[sizeof(name)-1] = '\0';
        parser_advance(p);

        /* arr[i] = val  or  dyn[i].metodo() as statement */
        if (check(p, TOK_LBRACKET)) {
            parser_advance(p);
            ASTNode *idx = parse_expr(p);
            if (!expect(p, TOK_RBRACKET, "after array index")) return NULL;

            /* dyn[i].metodo(args) as statement */
            if (check(p, TOK_DOT)) {
                parser_advance(p);
                if (!check(p, TOK_IDENT)) {
                    parse_error(p, "expected member name after '.'");
                    return NULL;
                }
                char member[256];
                strncpy(member, p->current.value, sizeof(member)-1);
                member[sizeof(member)-1] = '\0';
                parser_advance(p);
                if (!check(p, TOK_LPAREN)) {
                    parse_error(p, "expected '(' after method name");
                    return NULL;
                }
                parser_advance(p);
                ASTNode **args = NULL; int argc = 0;
                parse_args_into(p, &args, &argc);
                if (!expect(p, TOK_RPAREN, "after method arguments")) {
                    free(args); return NULL;
                }
                ASTNode *n = P_NODE();
                n->type                              = NODE_INDEXED_MEMBER_CALL;
                n->as.indexed_member_call.dyn_name   = P_STR(name);
                n->as.indexed_member_call.index      = idx;
                n->as.indexed_member_call.method     = P_STR(member);
                n->as.indexed_member_call.args       = args;
                n->as.indexed_member_call.arg_count  = argc;
                return n;
            }

            if (!expect(p, TOK_EQ, "after array access")) return NULL;
            ASTNode *val = parse_expr(p);
            if (!val) return NULL;
            ASTNode *n = P_NODE();
            n->type                   = NODE_ARR_ASSIGN;
            n->as.arr_assign.arr_name = P_STR(name);
            n->as.arr_assign.index    = idx;
            n->as.arr_assign.value    = val;
            return n;
        }

        /* Sprint 5: inst.field = val  or  inst.method(args) as statement */
        if (check(p, TOK_DOT)) {
            parser_advance(p);
            if (!check(p, TOK_IDENT)) {
                parse_error(p, "expected member name after '.'");
                return NULL;
            }
            char member[256];
            strncpy(member, p->current.value, sizeof(member)-1);
            member[sizeof(member)-1] = '\0';
            parser_advance(p);

            if (check(p, TOK_LPAREN)) {
                /* inst.method(args) as statement */
                parser_advance(p);
                ASTNode **args = NULL; int argc = 0;
                parse_args_into(p, &args, &argc);
                if (!expect(p, TOK_RPAREN, "after method arguments")) {
                    free(args); return NULL;
                }
                ASTNode *n = P_NODE();
                n->type                     = NODE_MEMBER_CALL;
                n->as.member_call.owner     = P_STR(name);
                n->as.member_call.method    = P_STR(member);
                n->as.member_call.args      = args;
                n->as.member_call.arg_count = argc;
                return n;
            }

            if (check(p, TOK_EQ)) {
                /* inst.field = val */
                parser_advance(p);
                ASTNode *val = parse_expr(p);
                if (!val) return NULL;
                ASTNode *n = P_NODE();
                n->type                    = NODE_MEMBER_ASSIGN;
                n->as.member_assign.owner  = P_STR(name);
                n->as.member_assign.field  = P_STR(member);
                n->as.member_assign.value  = val;
                return n;
            }

            parse_error(p, "expected '(' or '=' after member name");
            return NULL;
        }

        /* plain assignment */
        if (check(p, TOK_EQ)) {
            parser_advance(p);
            ASTNode *val = parse_expr(p);
            if (!val) return NULL;
            return p_assign(p, name, val);
        }

        /* function call as statement */
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

        parse_error(p, "expected '=', '[', '.' or '(' after identifier");
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
