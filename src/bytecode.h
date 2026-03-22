/* bytecode.h — Fluxa Bytecode VM (Sprint 4 performance)
 * Issue #32: implementation moved to bytecode.c
 * Issue #33: next_reg is now uint16_t (was uint8_t — silent overflow at 128)
 */
#ifndef FLUXA_BYTECODE_H
#define FLUXA_BYTECODE_H

#define _POSIX_C_SOURCE 200809L
#include "scope.h"
#include "ast.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Opcodes ─────────────────────────────────────────────────────────────── */
typedef enum {
    OP_LOADK,
    OP_MOVE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE,
    OP_JUMP_IF_FALSE,
    OP_JUMP,
    OP_RETURN
} Opcode;

/* ── Instruction (3-address register-based) ──────────────────────────────── */
typedef struct {
    Opcode   op;
    uint16_t a;       /* dest register    — Issue #33: uint16_t */
    uint16_t b;       /* src1 / const idx — Issue #33: uint16_t */
    uint16_t c;       /* src2 register    — Issue #33: uint16_t */
    int      offset;  /* jump target */
} Instruction;

/* ── Chunk — compiled bytecode ───────────────────────────────────────────── */
#define CHUNK_INIT_CAP  64
#define CHUNK_MAX_CONST 128

typedef struct {
    Instruction *code;
    int          count;
    int          cap;
    Value        constants[CHUNK_MAX_CONST];
    int          const_count;
    int          ok;
    uint16_t     next_reg;   /* Issue #33: uint16_t — starts at 128 */
} Chunk;

/* ── Chunk lifecycle (inline — trivial) ──────────────────────────────────── */
static inline void chunk_init(Chunk *c) {
    c->code  = (Instruction*)malloc(sizeof(Instruction) * CHUNK_INIT_CAP);
    c->count = 0;
    c->cap   = CHUNK_INIT_CAP;
    c->const_count = 0;
    c->ok    = 1;
    c->next_reg = 128;
}

static inline void chunk_free(Chunk *c) {
    free(c->code);
    c->code  = NULL;
    c->count = 0;
}

static inline int chunk_emit(Chunk *c, Instruction instr) {
    if (c->count >= c->cap) {
        c->cap *= 2;
        c->code = (Instruction*)realloc(c->code,
                      sizeof(Instruction) * c->cap);
    }
    c->code[c->count++] = instr;
    return c->count - 1;
}

static inline void chunk_patch(Chunk *c, int idx, int offset) {
    c->code[idx].offset = offset;
}

static inline int chunk_add_const_int(Chunk *c, long ival) {
    if (c->const_count >= CHUNK_MAX_CONST) { c->ok = 0; return 0; }
    Value v; v.type = VAL_INT; v.as.integer = ival;
    c->constants[c->const_count] = v;
    return c->const_count++;
}
static inline int chunk_add_const_float(Chunk *c, double fval) {
    if (c->const_count >= CHUNK_MAX_CONST) { c->ok = 0; return 0; }
    Value v; v.type = VAL_FLOAT; v.as.real = fval;
    c->constants[c->const_count] = v;
    return c->const_count++;
}
static inline int chunk_add_const_bool(Chunk *c, int bval) {
    if (c->const_count >= CHUNK_MAX_CONST) { c->ok = 0; return 0; }
    Value v; v.type = VAL_BOOL; v.as.boolean = bval;
    c->constants[c->const_count] = v;
    return c->const_count++;
}
static inline int chunk_add_const_str(Chunk *c, const char *sval) {
    if (c->const_count >= CHUNK_MAX_CONST) { c->ok = 0; return 0; }
    c->constants[c->const_count] = val_string(sval);
    return c->const_count++;
}

/* ── Public API (implemented in bytecode.c) ──────────────────────────────── */
int chunk_compile_loop(Chunk *c, ASTNode *loop_node);
int vm_run(Chunk *c, Scope *scope, Value *stack_ptr, int stack_size);

#endif /* FLUXA_BYTECODE_H */
