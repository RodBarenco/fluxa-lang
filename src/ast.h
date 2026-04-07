/* ast.h — Abstract Syntax Tree node definitions for Fluxa
 * Sprint 1: print()
 * Sprint 2: NODE_VAR_DECL, NODE_ASSIGN, NODE_BINARY_EXPR
 * Sprint 3: NODE_IF, NODE_WHILE, NODE_FOR, NODE_ARR_*
 * Sprint 4: NODE_FUNC_DECL, NODE_RETURN
 * Sprint 5: NODE_BLOCK_DECL, NODE_TYPEOF_INST, NODE_MEMBER_CALL, NODE_MEMBER_ACCESS
 */
#ifndef FLUXA_AST_H
#define FLUXA_AST_H

#include <stdlib.h>

typedef enum {
    NODE_PROGRAM,
    NODE_FUNC_CALL,
    NODE_STRING_LIT,
    NODE_INT_LIT,
    NODE_FLOAT_LIT,
    NODE_BOOL_LIT,
    NODE_IDENTIFIER,
    NODE_VAR_DECL,
    NODE_ASSIGN,
    NODE_BINARY_EXPR,
    /* Sprint 3 */
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_BLOCK_STMT,
    NODE_ARR_DECL,
    NODE_ARR_ACCESS,
    NODE_ARR_ASSIGN,
    /* Sprint 4 */
    NODE_FUNC_DECL,
    NODE_RETURN,
    /* Sprint 5 — #36 */
    NODE_BLOCK_DECL,      /* Block Foo { ... }          */
    NODE_TYPEOF_INST,     /* Block b1 typeof Foo        */
    NODE_MEMBER_CALL,     /* inst.metodo(args)          */
    NODE_MEMBER_ACCESS,   /* inst.campo (lvalue/rvalue) */
    NODE_MEMBER_ASSIGN,   /* inst.campo = val           */
    /* Sprint 6 */
    NODE_DANGER,        /* danger { }      */
    NODE_FREE,          /* free(var)       */
    /* Sprint 6.b */
    NODE_IMPORT_C,      /* import c libm   */
    NODE_IMPORT_STD,    /* import std math */
    NODE_FFI_CALL,      /* libm.sqrt(x)    */
    /* Sprint 9.c */
    NODE_DYN_LIT,       /* [1, "x", true]  — heterogeneous literal  */
    NODE_DYN_ACCESS,    /* lista[i]        — dyn read               */
    NODE_DYN_ASSIGN,    /* lista[i] = v    — dyn write (auto-grow)  */
    /* Sprint 9.c bugfix: dyn[i].campo / dyn[i].metodo(args) */
    NODE_INDEXED_MEMBER_ACCESS, /* petshop[1].olhos                 */
    NODE_INDEXED_MEMBER_CALL,   /* petshop[1].latir()               */
} NodeType;

#ifndef FLUXA_AST_NODE_DECLARED
#define FLUXA_AST_NODE_DECLARED
typedef struct ASTNode ASTNode;
#endif

struct ASTNode {
    NodeType type;
    int      resolved_offset;  /* set by resolver — -1 = unresolved */
    int      line;             /* source line — set by parser (Sprint 8) */

    union {
        /* NODE_PROGRAM / NODE_FUNC_CALL / NODE_BLOCK_STMT */
        struct {
            ASTNode **children;
            int       count;
            char     *name;
        } list;

        /* NODE_STRING_LIT / NODE_IDENTIFIER */
        struct { char *value; } str;

        /* NODE_INT_LIT */
        struct { long value; } integer;

        /* NODE_FLOAT_LIT */
        struct { double value; } real;

        /* NODE_BOOL_LIT */
        struct { int value; } boolean;

        /* NODE_VAR_DECL */
        struct {
            char    *type_name;
            char    *var_name;
            ASTNode *initializer;
            int      persistent;
        } var_decl;

        /* NODE_ASSIGN */
        struct {
            char    *var_name;
            ASTNode *value;
        } assign;

        /* NODE_BINARY_EXPR */
        struct {
            char    *op;
            ASTNode *left;
            ASTNode *right;
        } binary;

        /* NODE_IF */
        struct {
            ASTNode *condition;
            ASTNode *then_body;
            ASTNode *else_body;
        } if_stmt;

        /* NODE_WHILE */
        struct {
            ASTNode *condition;
            ASTNode *body;
        } while_stmt;

        /* NODE_FOR */
        struct {
            char    *var_name;
            char    *arr_name;
            ASTNode *body;
            int      arr_resolved_offset; /* stack slot of the iterable */
        } for_stmt;

        /* NODE_ARR_DECL */
        struct {
            char    *type_name;
            char    *arr_name;
            int      size;
            ASTNode **elements;    /* NULL if default_init */
            int      persistent;
            int      default_init; /* 1 = fill with default_value */
            ASTNode *default_value;/* scalar literal for default  */
        } arr_decl;

        /* NODE_ARR_ACCESS */
        struct {
            char    *arr_name;
            ASTNode *index;
        } arr_access;

        /* NODE_ARR_ASSIGN */
        struct {
            char    *arr_name;
            ASTNode *index;
            ASTNode *value;
        } arr_assign;

        /* NODE_FUNC_DECL */
        struct {
            char     *name;
            char    **param_names;
            char    **param_types;
            int       param_count;
            char     *return_type;
            ASTNode  *body;
        } func_decl;

        /* NODE_RETURN */
        struct { ASTNode *value; } ret;

        /* NODE_BLOCK_DECL — Sprint 5 */
        struct {
            char     *name;
            ASTNode **members;   /* var_decl and func_decl nodes */
            int       count;
        } block_decl;

        /* NODE_TYPEOF_INST — Sprint 5 */
        struct {
            char *inst_name;    /* left: Block b1 */
            char *origin_name;  /* right: typeof Foo — must be BlockDef */
        } typeof_inst;

        /* NODE_MEMBER_CALL — Sprint 5: inst.method(args) */
        struct {
            char     *owner;        /* instance or Block name */
            char     *method;
            ASTNode **args;
            int       arg_count;
        } member_call;

        /* NODE_MEMBER_ACCESS — Sprint 5: inst.field */
        struct {
            char *owner;
            char *field;
        } member_access;

        /* NODE_MEMBER_ASSIGN — Sprint 5: inst.field = val */
        struct {
            char    *owner;
            char    *field;
            ASTNode *value;
        } member_assign;


        /* NODE_DANGER (Sprint 6) */
        struct { ASTNode *body; } danger_stmt;

        /* NODE_FREE (Sprint 6) */
        struct { char *var_name; } free_stmt;

        /* NODE_IMPORT_C (Sprint 6.b) */
        struct {
            char *lib_name;   /* "libm", "libc", etc. */
            char *alias;      /* "as fm" — same as lib_name if no alias */
        } import_c;

        /* NODE_IMPORT_STD — import std math/csv/json/vec */
        struct {
            char *lib_name;   /* "math", "csv", "json", "vec" */
        } import_std;

        /* NODE_FFI_CALL (Sprint 6.b): alias.symbol(args) inside danger */
        struct {
            char     *lib_alias;    /* library alias */
            char     *sym_name;     /* function name */
            char     *ret_type;     /* "int","float","str","bool","nil" */
            ASTNode **args;
            int       arg_count;
        } ffi_call;

        /* NODE_DYN_LIT (Sprint 9.c): [expr, expr, ...] heterogeneous */
        struct {
            ASTNode **elements;
            int       count;
        } dyn_lit;

        /* NODE_DYN_ACCESS (Sprint 9.c): lista[i] */
        struct {
            char    *dyn_name;
            ASTNode *index;
        } dyn_access;

        /* NODE_DYN_ASSIGN (Sprint 9.c): lista[i] = v  (auto-grow) */
        struct {
            char    *dyn_name;
            ASTNode *index;
            ASTNode *value;
        } dyn_assign;

        /* NODE_INDEXED_MEMBER_ACCESS: dyn[i].campo */
        struct {
            char    *dyn_name;
            ASTNode *index;
            char    *field;
        } indexed_member_access;

        /* NODE_INDEXED_MEMBER_CALL: dyn[i].metodo(args) */
        struct {
            char     *dyn_name;
            ASTNode  *index;
            char     *method;
            ASTNode **args;
            int       arg_count;
        } indexed_member_call;

    } as;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static inline ASTNode *ast_new(NodeType type) {
    ASTNode *n = (ASTNode*)calloc(1, sizeof(ASTNode));
    n->type = type;
    n->resolved_offset = -1;
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

#endif /* FLUXA_AST_H */
