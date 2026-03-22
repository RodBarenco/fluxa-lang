/* ast.h — Abstract Syntax Tree node definitions for Fluxa
 * Sprint 1: nodes sufficient to execute print("hello world")
 * Sprint 2: NODE_VAR_DECL, NODE_ASSIGN, NODE_BINARY_EXPR
 * Sprint 3 (Issue #15): NODE_IF, NODE_WHILE, NODE_FOR, NODE_ARR_DECL,
 *                       NODE_ARR_ACCESS, NODE_ARR_ASSIGN, NODE_BLOCK_STMT
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
    NODE_VAR_DECL,      /* int a = 10                              */
    NODE_ASSIGN,        /* a = 99                                  */
    NODE_BINARY_EXPR,   /* a + b                                   */
    /* Sprint 3 — Issue #15 */
    NODE_IF,            /* if cond { } else { }                    */
    NODE_WHILE,         /* while cond { }                          */
    NODE_FOR,           /* for x in arr { }                        */
    NODE_BLOCK_STMT,    /* { stmt* }  — body of if/while/for/fn    */
    NODE_ARR_DECL,      /* int arr nums[3] = [1,2,3]               */
    NODE_ARR_ACCESS,    /* nums[i]                                  */
    NODE_ARR_ASSIGN,    /* nums[i] = val                           */
    /* Sprint 4 — Issue #19 */
    NODE_FUNC_DECL,     /* fn name(params) type { body }           */
    NODE_RETURN,        /* return expr?                             */
} NodeType;

/* ── Forward declaration ─────────────────────────────────────────────────── */
#ifndef FLUXA_AST_NODE_DECLARED
#define FLUXA_AST_NODE_DECLARED
typedef struct ASTNode ASTNode;
#endif

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

        /* NODE_VAR_DECL */
        struct {
            char    *type_name;   /* "int", "float", "str", "bool", "char" */
            char    *var_name;
            ASTNode *initializer;
            int      persistent;  /* prst flag — Sprint 6 */
        } var_decl;

        /* NODE_ASSIGN */
        struct {
            char    *var_name;
            ASTNode *value;
        } assign;

        /* NODE_BINARY_EXPR */
        struct {
            char    *op;    /* "+", "-", "*", "/", "%", "==", "!=", "<", ">", "<=", ">=" */
            ASTNode *left;
            ASTNode *right;
        } binary;

        /* NODE_IF (Issue #15) */
        struct {
            ASTNode *condition;
            ASTNode *then_body;  /* NODE_BLOCK_STMT */
            ASTNode *else_body;  /* NODE_BLOCK_STMT or NULL */
        } if_stmt;

        /* NODE_WHILE (Issue #15) */
        struct {
            ASTNode *condition;
            ASTNode *body;       /* NODE_BLOCK_STMT */
        } while_stmt;

        /* NODE_FOR (Issue #15) */
        struct {
            char    *var_name;   /* loop variable */
            char    *arr_name;   /* array to iterate */
            ASTNode *body;       /* NODE_BLOCK_STMT */
        } for_stmt;

        /* NODE_ARR_DECL (Issue #15) */
        struct {
            char    *type_name;
            char    *arr_name;
            int      size;
            ASTNode **elements;  /* heap-allocated array of ASTNode* */
            int      persistent;
        } arr_decl;

        /* NODE_ARR_ACCESS (Issue #15) */
        struct {
            char    *arr_name;
            ASTNode *index;
        } arr_access;

        /* NODE_ARR_ASSIGN (Issue #15) */
        struct {
            char    *arr_name;
            ASTNode *index;
            ASTNode *value;
        } arr_assign;

        /* NODE_FUNC_DECL (Issue #19) */
        struct {
            char     *name;
            char    **param_names;   /* heap-allocated — parallel arrays */
            char    **param_types;   /* heap-allocated */
            int       param_count;
            char     *return_type;   /* "int", "float", "str", "bool", "nil" */
            ASTNode  *body;          /* NODE_BLOCK_STMT */
        } func_decl;

        /* NODE_RETURN (Issue #19) */
        struct {
            ASTNode *value;          /* NULL for bare return */
        } ret;
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

static inline ASTNode *ast_var_decl(char *type_name, char *var_name,
                                     ASTNode *initializer, int persistent) {
    ASTNode *n = ast_new(NODE_VAR_DECL);
    n->as.var_decl.type_name   = type_name;
    n->as.var_decl.var_name    = var_name;
    n->as.var_decl.initializer = initializer;
    n->as.var_decl.persistent  = persistent;
    return n;
}

static inline ASTNode *ast_assign(char *var_name, ASTNode *value) {
    ASTNode *n = ast_new(NODE_ASSIGN);
    n->as.assign.var_name = var_name;
    n->as.assign.value    = value;
    return n;
}

static inline ASTNode *ast_binary(char *op, ASTNode *left, ASTNode *right) {
    ASTNode *n = ast_new(NODE_BINARY_EXPR);
    n->as.binary.op    = op;
    n->as.binary.left  = left;
    n->as.binary.right = right;
    return n;
}

/* ── Free ────────────────────────────────────────────────────────────────── */
/* NOTE: When using ASTPool, node memory is owned by the pool — do NOT call
 * free(n) on individual nodes. ast_free() only releases heap-allocated
 * children arrays (from ast_list_push / realloc). Node structs themselves
 * are left in place; pool_free() discards the entire arena at once.        */
static inline void ast_free(ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case NODE_PROGRAM:
        case NODE_FUNC_CALL:
            for (int i = 0; i < n->as.list.count; i++)
                ast_free(n->as.list.children[i]);
            free(n->as.list.children);
            break;
        case NODE_BLOCK_STMT:
            for (int i = 0; i < n->as.list.count; i++)
                ast_free(n->as.list.children[i]);
            free(n->as.list.children);
            break;
        case NODE_VAR_DECL:
            ast_free(n->as.var_decl.initializer);
            break;
        case NODE_ASSIGN:
            ast_free(n->as.assign.value);
            break;
        case NODE_BINARY_EXPR:
            ast_free(n->as.binary.left);
            ast_free(n->as.binary.right);
            break;
        case NODE_IF:
            ast_free(n->as.if_stmt.condition);
            ast_free(n->as.if_stmt.then_body);
            ast_free(n->as.if_stmt.else_body);
            break;
        case NODE_WHILE:
            ast_free(n->as.while_stmt.condition);
            ast_free(n->as.while_stmt.body);
            break;
        case NODE_FOR:
            ast_free(n->as.for_stmt.body);
            break;
        case NODE_ARR_DECL:
            if (n->as.arr_decl.elements) {
                for (int i = 0; i < n->as.arr_decl.size; i++)
                    ast_free(n->as.arr_decl.elements[i]);
                free(n->as.arr_decl.elements);
            }
            break;
        case NODE_ARR_ACCESS:
            ast_free(n->as.arr_access.index);
            break;
        case NODE_ARR_ASSIGN:
            ast_free(n->as.arr_assign.index);
            ast_free(n->as.arr_assign.value);
            break;
        /* Issue #19 */
        case NODE_FUNC_DECL:
            if (n->as.func_decl.param_names) free(n->as.func_decl.param_names);
            if (n->as.func_decl.param_types) free(n->as.func_decl.param_types);
            ast_free(n->as.func_decl.body);
            break;
        case NODE_RETURN:
            ast_free(n->as.ret.value);
            break;
        default:
            break;
    }
    /* do NOT free(n) — node lives in the pool arena */
}

#endif /* FLUXA_AST_H */
