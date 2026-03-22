/* bytecode.h — Fluxa Bytecode VM for hot paths
 * Sprint 4 Performance — Issue backlog
 *
 * Instead of walking the AST on every loop iteration, the NODE_WHILE
 * body is compiled once to a flat array of instructions. The VM then
 * executes those instructions directly — no tree traversal, no function
 * call overhead per node, no branch on node type per iteration.
 *
 * Scope integration: variables are accessed by NAME (string key) via
 * the existing uthash scope. Name Resolution (offset-based stack) is a
 * later step that will replace this with O(1) array access.
 */
#ifndef FLUXA_BYTECODE_H
#define FLUXA_BYTECODE_H

#define _POSIX_C_SOURCE 200809L
#include "scope.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
typedef enum {
    /* literals */
    OP_PUSH_INT,      /* operand: long                */
    OP_PUSH_FLOAT,    /* operand: double              */
    OP_PUSH_BOOL,     /* operand: int                 */
    OP_PUSH_STR,      /* operand: char*               */
    OP_PUSH_NIL,

    /* variables */
    OP_LOAD,          /* operand: char* name          */
    OP_STORE,         /* operand: char* name          */

    /* arithmetic */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,

    /* comparison */
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_GT,
    OP_LTE,
    OP_GTE,

    /* control */
    OP_JUMP_IF_FALSE, /* operand: int offset (relative) */
    OP_JUMP,          /* operand: int offset (relative) */

    OP_RETURN,        /* signals end of chunk         */
} Opcode;

/* ── Instruction ─────────────────────────────────────────────────────────── */
typedef struct {
    Opcode       op;
    int          stack_offset;  /* Issue #23: -1 = use sval/scope, >=0 = direct stack */
    ScopeEntry  *cached_entry;  /* inline cache — NULL until first access */
    union {
        long    ival;
        double  fval;
        char   *sval;   /* interned — not owned by instruction */
        int     offset; /* jump target */
    } arg;
} Instruction;

/* ── Chunk — compiled bytecode ───────────────────────────────────────────── */
#define CHUNK_INIT_CAP 64

typedef struct {
    Instruction *code;
    int          count;
    int          cap;
    int          ok;    /* 0 = compile error */
} Chunk;

static inline void chunk_init(Chunk *c) {
    c->code  = (Instruction*)malloc(sizeof(Instruction) * CHUNK_INIT_CAP);
    c->count = 0;
    c->cap   = CHUNK_INIT_CAP;
    c->ok    = 1;
}

static inline void chunk_free(Chunk *c) {
    free(c->code);
    c->code  = NULL;
    c->count = 0;
}

static inline int chunk_emit(Chunk *c, Instruction instr) {
    if (c->count >= c->cap) {
        c->cap  *= 2;
        c->code  = (Instruction*)realloc(c->code,
                       sizeof(Instruction) * c->cap);
    }
    instr.cached_entry = NULL;   /* inline cache starts empty */
    instr.stack_offset = -1;     /* -1 = use scope/sval, set by resolver */
    c->code[c->count++] = instr;
    return c->count - 1;
}

static inline void chunk_patch(Chunk *c, int idx, int offset) {
    c->code[idx].arg.offset = offset;
}

/* ── Value stack for VM ──────────────────────────────────────────────────── */
#define VM_STACK_MAX 256

typedef struct {
    Value stack[VM_STACK_MAX];
    int   top;
} VMStack;

static inline void vs_push(VMStack *vs, Value v) {
    if (vs->top < VM_STACK_MAX) vs->stack[vs->top++] = v;
}
static inline Value vs_pop(VMStack *vs) {
    if (vs->top > 0) return vs->stack[--vs->top];
    Value n; n.type = VAL_NIL; return n;
}

/* ── Compiler: ASTNode → Chunk ───────────────────────────────────────────── */
#include "ast.h"

static void compile_node(Chunk *c, ASTNode *node);

static void compile_expr(Chunk *c, ASTNode *node) {
    if (!node || !c->ok) return;
    switch (node->type) {
        case NODE_INT_LIT: {
            Instruction i; i.op = OP_PUSH_INT; i.arg.ival = node->as.integer.value;
            chunk_emit(c, i); break;
        }
        case NODE_FLOAT_LIT: {
            Instruction i; i.op = OP_PUSH_FLOAT; i.arg.fval = node->as.real.value;
            chunk_emit(c, i); break;
        }
        case NODE_BOOL_LIT: {
            Instruction i; i.op = OP_PUSH_BOOL; i.arg.ival = node->as.boolean.value;
            chunk_emit(c, i); break;
        }
        case NODE_STRING_LIT: {
            Instruction i; i.op = OP_PUSH_STR; i.arg.sval = node->as.str.value;
            chunk_emit(c, i); break;
        }
        case NODE_IDENTIFIER: {
            Instruction i; i.op = OP_LOAD; i.arg.sval = node->as.str.value;
            i.stack_offset = node->resolved_offset;  /* -1 if unresolved */
            chunk_emit(c, i); break;
        }
        case NODE_BINARY_EXPR: {
            compile_expr(c, node->as.binary.left);
            compile_expr(c, node->as.binary.right);
            const char *op = node->as.binary.op;
            Instruction i;
            if      (!strcmp(op,"+"))  i.op = OP_ADD;
            else if (!strcmp(op,"-"))  i.op = OP_SUB;
            else if (!strcmp(op,"*"))  i.op = OP_MUL;
            else if (!strcmp(op,"/"))  i.op = OP_DIV;
            else if (!strcmp(op,"%"))  i.op = OP_MOD;
            else if (!strcmp(op,"==")) i.op = OP_EQ;
            else if (!strcmp(op,"!=")) i.op = OP_NEQ;
            else if (!strcmp(op,"<"))  i.op = OP_LT;
            else if (!strcmp(op,">"))  i.op = OP_GT;
            else if (!strcmp(op,"<=")) i.op = OP_LTE;
            else if (!strcmp(op,">=")) i.op = OP_GTE;
            else { c->ok = 0; return; }
            chunk_emit(c, i);
            break;
        }
        default:
            /* complex expr — not compilable to bytecode yet */
            c->ok = 0;
            break;
    }
}

static void compile_node(Chunk *c, ASTNode *node) {
    if (!node || !c->ok) return;
    switch (node->type) {
        case NODE_VAR_DECL:
        case NODE_ASSIGN: {
            ASTNode *val  = (node->type == NODE_VAR_DECL)
                          ? node->as.var_decl.initializer
                          : node->as.assign.value;
            const char *name = (node->type == NODE_VAR_DECL)
                          ? node->as.var_decl.var_name
                          : node->as.assign.var_name;
            compile_expr(c, val);
            Instruction i; i.op = OP_STORE; i.arg.sval = (char*)name;
            i.stack_offset = node->resolved_offset;  /* set by resolver */
            chunk_emit(c, i);
            break;
        }
        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++)
                compile_node(c, node->as.list.children[i]);
            break;
        case NODE_IF: {
            compile_expr(c, node->as.if_stmt.condition);
            Instruction jf; jf.op = OP_JUMP_IF_FALSE; jf.arg.offset = 0;
            int jf_idx = chunk_emit(c, jf);
            compile_node(c, node->as.if_stmt.then_body);
            if (node->as.if_stmt.else_body) {
                Instruction jmp; jmp.op = OP_JUMP; jmp.arg.offset = 0;
                int jmp_idx = chunk_emit(c, jmp);
                chunk_patch(c, jf_idx, c->count);
                compile_node(c, node->as.if_stmt.else_body);
                chunk_patch(c, jmp_idx, c->count);
            } else {
                chunk_patch(c, jf_idx, c->count);
            }
            break;
        }
        case NODE_FUNC_CALL:
            /* function calls inside while body — fall back to AST walk */
            c->ok = 0;
            break;
        default:
            c->ok = 0;
            break;
    }
}

/* compile a while body — returns 0 if not fully compilable */
static inline int chunk_compile_body(Chunk *c, ASTNode *body) {
    chunk_init(c);
    compile_node(c, body);
    if (!c->ok) { chunk_free(c); return 0; }
    Instruction ret; ret.op = OP_RETURN;
    chunk_emit(c, ret);
    return 1;
}

/* ── VM execution ────────────────────────────────────────────────────────── */
static inline int vm_truthy(Value v) {
    if (v.type == VAL_BOOL)  return v.as.boolean;
    if (v.type == VAL_INT)   return v.as.integer != 0;
    if (v.type == VAL_FLOAT) return v.as.real != 0.0;
    return 0;
}

static inline Value vm_arith(Value l, Value r, Opcode op) {
    int both_int = (l.type == VAL_INT && r.type == VAL_INT);
    double lv = (l.type == VAL_INT) ? (double)l.as.integer : l.as.real;
    double rv = (r.type == VAL_INT) ? (double)r.as.integer : r.as.real;
    double res = 0;
    switch (op) {
        case OP_ADD: res = lv + rv; break;
        case OP_SUB: res = lv - rv; break;
        case OP_MUL: res = lv * rv; break;
        case OP_DIV: res = (rv != 0) ? lv / rv : 0; break;
        case OP_MOD:
            if (both_int && (long)rv != 0)
                return val_int((long)lv % (long)rv);
            return val_int(0);
        default: break;
    }
    return both_int ? val_int((long)res) : val_float(res);
}

static inline Value vm_compare(Value l, Value r, Opcode op) {
    if (l.type == VAL_INT && r.type == VAL_INT) {
        long lv = l.as.integer, rv = r.as.integer;
        switch (op) {
            case OP_EQ:  return val_bool(lv == rv);
            case OP_NEQ: return val_bool(lv != rv);
            case OP_LT:  return val_bool(lv <  rv);
            case OP_GT:  return val_bool(lv >  rv);
            case OP_LTE: return val_bool(lv <= rv);
            case OP_GTE: return val_bool(lv >= rv);
            default: break;
        }
    }
    double lv = (l.type==VAL_INT)?(double)l.as.integer:l.as.real;
    double rv = (r.type==VAL_INT)?(double)r.as.integer:r.as.real;
    switch (op) {
        case OP_EQ:  return val_bool(lv == rv);
        case OP_NEQ: return val_bool(lv != rv);
        case OP_LT:  return val_bool(lv <  rv);
        case OP_GT:  return val_bool(lv >  rv);
        case OP_LTE: return val_bool(lv <= rv);
        case OP_GTE: return val_bool(lv >= rv);
        default: break;
    }
    return val_bool(0);
}

/* run compiled chunk with direct stack access ─────────────────────────────
 * Issue #23: stack_ptr/stack_size enable O(1) variable access by offset.
 * Falls back to uthash inline cache when stack_offset == -1.            */
static inline int vm_run(Chunk *c, Scope *scope, Value *stack_ptr, int stack_size) {
    VMStack vs; vs.top = 0;
    int ip = 0;

#ifdef __GNUC__
    static const void *dispatch[] = {
        &&L_PUSH_INT, &&L_PUSH_FLOAT, &&L_PUSH_BOOL, &&L_PUSH_STR, &&L_PUSH_NIL,
        &&L_LOAD, &&L_STORE,
        &&L_ADD, &&L_SUB, &&L_MUL, &&L_DIV, &&L_MOD,
        &&L_EQ, &&L_NEQ, &&L_LT, &&L_GT, &&L_LTE, &&L_GTE,
        &&L_JUMP_IF_FALSE, &&L_JUMP, &&L_RETURN
    };
    #define NEXT() do { if (ip >= c->count) goto L_RETURN; \
                        goto *dispatch[c->code[ip++].op]; } while(0)
    #define CUR()  (c->code[ip-1])

    NEXT();

    L_PUSH_INT:   { Value v; v.type=VAL_INT;   v.as.integer=CUR().arg.ival; vs_push(&vs,v); NEXT(); }
    L_PUSH_FLOAT: { Value v; v.type=VAL_FLOAT; v.as.real=CUR().arg.fval;    vs_push(&vs,v); NEXT(); }
    L_PUSH_BOOL:  { Value v; v.type=VAL_BOOL;  v.as.boolean=(int)CUR().arg.ival; vs_push(&vs,v); NEXT(); }
    L_PUSH_STR:   { Value v=val_string(CUR().arg.sval); vs_push(&vs,v); NEXT(); }
    L_PUSH_NIL:   { Value v; v.type=VAL_NIL; vs_push(&vs,v); NEXT(); }

    L_LOAD: {
        Instruction *instr = &c->code[ip-1];
        Value v;
        /* Issue #23 — direct stack access */
        if (instr->stack_offset >= 0 && instr->stack_offset < stack_size) {
            v = stack_ptr[instr->stack_offset];
        } else {
            if (!instr->cached_entry)
                HASH_FIND_STR(scope->table, instr->arg.sval, instr->cached_entry);
            v = instr->cached_entry ? instr->cached_entry->value : val_nil();
        }
        vs_push(&vs, v);
        NEXT();
    }

    L_STORE: {
        Instruction *instr = &c->code[ip-1];
        Value v = vs_pop(&vs);
        /* Issue #23 — direct stack access */
        if (instr->stack_offset >= 0 && instr->stack_offset < stack_size) {
            stack_ptr[instr->stack_offset] = v;
            /* also update scope entry if cached — keeps scope in sync */
            if (instr->cached_entry) instr->cached_entry->value = v;
        } else {
            if (!instr->cached_entry)
                HASH_FIND_STR(scope->table, instr->arg.sval, instr->cached_entry);
            if (instr->cached_entry) instr->cached_entry->value = v;
            else { scope_set(scope, instr->arg.sval, v);
                   HASH_FIND_STR(scope->table, instr->arg.sval, instr->cached_entry); }
        }
        NEXT();
    }

    L_ADD: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_arith(l,r,OP_ADD)); NEXT(); }
    L_SUB: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_arith(l,r,OP_SUB)); NEXT(); }
    L_MUL: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_arith(l,r,OP_MUL)); NEXT(); }
    L_DIV: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_arith(l,r,OP_DIV)); NEXT(); }
    L_MOD: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_arith(l,r,OP_MOD)); NEXT(); }
    L_EQ:  { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_compare(l,r,OP_EQ));  NEXT(); }
    L_NEQ: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_compare(l,r,OP_NEQ)); NEXT(); }
    L_LT:  { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_compare(l,r,OP_LT));  NEXT(); }
    L_GT:  { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_compare(l,r,OP_GT));  NEXT(); }
    L_LTE: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_compare(l,r,OP_LTE)); NEXT(); }
    L_GTE: { Value r=vs_pop(&vs); Value l=vs_pop(&vs); vs_push(&vs,vm_compare(l,r,OP_GTE)); NEXT(); }
    L_JUMP_IF_FALSE: {
        Value cond = vs_pop(&vs);
        if (!vm_truthy(cond)) ip = c->code[ip-1].arg.offset;
        NEXT();
    }
    L_JUMP: ip = c->code[ip-1].arg.offset; NEXT();
    L_RETURN: return 1;

    #undef NEXT
    #undef CUR

#else
    while (ip < c->count) {
        Instruction *instr = &c->code[ip++];
        switch (instr->op) {
            case OP_PUSH_INT:   { Value v; v.type=VAL_INT;   v.as.integer=instr->arg.ival; vs_push(&vs,v); break; }
            case OP_PUSH_FLOAT: { Value v; v.type=VAL_FLOAT; v.as.real=instr->arg.fval;    vs_push(&vs,v); break; }
            case OP_PUSH_BOOL:  { Value v; v.type=VAL_BOOL;  v.as.boolean=(int)instr->arg.ival; vs_push(&vs,v); break; }
            case OP_PUSH_STR:   { Value v=val_string(instr->arg.sval); vs_push(&vs,v); break; }
            case OP_PUSH_NIL:   { Value v; v.type=VAL_NIL; vs_push(&vs,v); break; }
            case OP_LOAD: {
                Value v;
                if (instr->stack_offset >= 0 && instr->stack_offset < stack_size)
                    v = stack_ptr[instr->stack_offset];
                else {
                    if (!instr->cached_entry)
                        HASH_FIND_STR(scope->table, instr->arg.sval, instr->cached_entry);
                    v = instr->cached_entry ? instr->cached_entry->value : val_nil();
                }
                vs_push(&vs, v); break;
            }
            case OP_STORE: {
                Value v = vs_pop(&vs);
                if (instr->stack_offset >= 0 && instr->stack_offset < stack_size) {
                    stack_ptr[instr->stack_offset] = v;
                    if (instr->cached_entry) instr->cached_entry->value = v;
                } else {
                    if (!instr->cached_entry)
                        HASH_FIND_STR(scope->table, instr->arg.sval, instr->cached_entry);
                    if (instr->cached_entry) instr->cached_entry->value = v;
                    else { scope_set(scope, instr->arg.sval, v);
                           HASH_FIND_STR(scope->table, instr->arg.sval, instr->cached_entry); }
                }
                break;
            }
            case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_DIV: case OP_MOD: {
                Value r=vs_pop(&vs); Value l=vs_pop(&vs);
                vs_push(&vs, vm_arith(l,r,instr->op)); break;
            }
            case OP_EQ: case OP_NEQ: case OP_LT:
            case OP_GT: case OP_LTE: case OP_GTE: {
                Value r=vs_pop(&vs); Value l=vs_pop(&vs);
                vs_push(&vs, vm_compare(l,r,instr->op)); break;
            }
            case OP_JUMP_IF_FALSE: {
                Value cond=vs_pop(&vs);
                if (!vm_truthy(cond)) ip=instr->arg.offset;
                break;
            }
            case OP_JUMP: ip=instr->arg.offset; break;
            case OP_RETURN: return 1;
        }
    }
    return 1;
#endif
}

#endif /* FLUXA_BYTECODE_H */
