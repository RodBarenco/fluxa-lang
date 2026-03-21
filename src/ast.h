/* ast.h — Abstract Syntax Tree node definitions for Fluxa
 * Sprint 1: nodes sufficient to execute print("hello world")
 */
#ifndef FLUXA_AST_H
#define FLUXA_AST_H

#include <stdlib.h>

/* ── Node types ─────────────────────────────────────────────────────────── */
typedef enum {
    NODE_PROGRAM,       /* root: list of statements                */
    NODE_FUNC_CALL,     /* print("hello")                          */
    NODE_STRING_LIT,    /* "hello world"                           */
    NODE_INT_LIT,       /* 42                                      */
    NODE_FLOAT_LIT,     /* 3.14                                    */
    NODE_BOOL_LIT,      /* true / false                            */
    NODE_IDENTIFIER,    /* variable or function name               */
} NodeType;

/* ── Forward declaration ─────────────────────────────────────────────────── */
typedef struct ASTNode ASTNode;

/* ── Node structure ──────────────────────────────────────────────────────── */
struct ASTNode {
    NodeType type;

    union {
        /* NODE_PROGRAM / NODE_FUNC_CALL: list of child nodes */
        struct {
            ASTNode **children;
            int       count;
            char     *name;     /* function name for FUNC_CALL */
        } list;

        /* NODE_STRING_LIT / NODE_IDENTIFIER */
        struct {
            char *value;
        } str;

        /* NODE_INT_LIT */
        struct {
            long value;
        } integer;

        /* NODE_FLOAT_LIT */
        struct {
            double value;
        } real;

        /* NODE_BOOL_LIT */
        struct {
            int value; /* 1 = true, 0 = false */
        } boolean;
    } as;
};

/* ── Constructors ────────────────────────────────────────────────────────── */

static inline ASTNode *ast_new(NodeType type) {
    ASTNode *n = (ASTNode*)calloc(1, sizeof(ASTNode));
    n->type = type;
    return n;
}

static inline ASTNode *ast_program(void) {
    ASTNode *n = ast_new(NODE_PROGRAM);
    n->as.list.children = NULL;
    n->as.list.count    = 0;
    return n;
}

static inline void ast_list_push(ASTNode *parent, ASTNode *child) {
    parent->as.list.count++;
    parent->as.list.children = (ASTNode**)realloc(
        parent->as.list.children,
        sizeof(ASTNode*) * parent->as.list.count
    );
    parent->as.list.children[parent->as.list.count - 1] = child;
}

static inline ASTNode *ast_func_call(char *name) {
    ASTNode *n  = ast_new(NODE_FUNC_CALL);
    n->as.list.name     = name;
    n->as.list.children = NULL;
    n->as.list.count    = 0;
    return n;
}

static inline ASTNode *ast_string(char *value) {
    ASTNode *n = ast_new(NODE_STRING_LIT);
    n->as.str.value = value;
    return n;
}

static inline ASTNode *ast_integer(long value) {
    ASTNode *n = ast_new(NODE_INT_LIT);
    n->as.integer.value = value;
    return n;
}

static inline ASTNode *ast_float(double value) {
    ASTNode *n = ast_new(NODE_FLOAT_LIT);
    n->as.real.value = value;
    return n;
}

static inline ASTNode *ast_bool(int value) {
    ASTNode *n = ast_new(NODE_BOOL_LIT);
    n->as.boolean.value = value;
    return n;
}

/* ── Free ────────────────────────────────────────────────────────────────── */
static inline void ast_free(ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_PROGRAM:
        case NODE_FUNC_CALL:
            for (int i = 0; i < n->as.list.count; i++)
                ast_free(n->as.list.children[i]);
            free(n->as.list.children);
            /* name is owned by the lexer token — do not free here */
            break;
        case NODE_STRING_LIT:
        case NODE_IDENTIFIER:
            /* value is owned by the lexer token — do not free here */
            break;
        default:
            break;
    }
    free(n);
}

#endif /* FLUXA_AST_H */
