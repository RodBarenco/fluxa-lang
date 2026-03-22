/* bytecode.h — Fluxa Bytecode VM for hot paths
 * Sprint 4 Performance — Issue backlog
 *
 * Emits register-based instructions. The VMStack has been completely
 * replaced by direct access to local frame slots (registers).
 * Execution is perfectly linear with zero indirections or pushes/pops.
 */
#ifndef FLUXA_BYTECODE_H
#define FLUXA_BYTECODE_H

#define _POSIX_C_SOURCE 200809L
#include "scope.h"
#include <stdint.h> /* Correção: para reconhecer uint8_t */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Opcodes (Register-based) ────────────────────────────────────────────── */
typedef enum {
    OP_LOADK,         /* R[a] = constants[b]          */
    OP_MOVE,          /* R[a] = R[b]                  */
    OP_ADD,           /* R[a] = R[b] + R[c]           */
    OP_SUB,           /* R[a] = R[b] - R[c]           */
    OP_MUL,           /* R[a] = R[b] * R[c]           */
    OP_DIV,           /* R[a] = R[b] / R[c]           */
    OP_MOD,           /* R[a] = R[b] % R[c]           */
    OP_EQ,            /* R[a] = R[b] == R[c]          */
    OP_NEQ,           /* R[a] = R[b] != R[c]          */
    OP_LT,            /* R[a] = R[b] < R[c]           */
    OP_GT,            /* R[a] = R[b] > R[c]           */
    OP_LTE,           /* R[a] = R[b] <= R[c]          */
    OP_GTE,           /* R[a] = R[b] >= R[c]          */
    OP_JUMP_IF_FALSE, /* if (!R[a]) ip = offset       */
    OP_JUMP,          /* ip = offset                  */
    OP_RETURN         /* signals end of chunk         */
} Opcode;

/* ── Instruction (3 Addresses) ───────────────────────────────────────────── */
typedef struct {
    Opcode  op;
    uint8_t a;        /* dest register */
    uint8_t b;        /* src1 register or const index */
    uint8_t c;        /* src2 register */
    int     offset;   /* jump target */
} Instruction;

/* ── Chunk — compiled bytecode ───────────────────────────────────────────── */
#define CHUNK_INIT_CAP 64
#define CHUNK_MAX_CONST 128

typedef struct {
    Instruction *code;
    int          count;
    int          cap;
    Value        constants[CHUNK_MAX_CONST];
    int          const_count;
    int          ok;       /* 0 = compile error */
    uint8_t      next_reg; /* register allocator state */
} Chunk;

static inline void chunk_init(Chunk *c) {
    c->code  = (Instruction*)malloc(sizeof(Instruction) * CHUNK_INIT_CAP);
    c->count = 0;
    c->cap   = CHUNK_INIT_CAP;
    c->const_count = 0;
    c->ok    = 1;
    c->next_reg = 128;     /* temporaries start above 127 */
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
    c->code[c->count++] = instr;
    return c->count - 1;
}

static inline void chunk_patch(Chunk *c, int idx, int offset) {
    c->code[idx].offset = offset;
}

/* ── Constants Helpers ───────────────────────────────────────────────────── */
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

/* ── Compiler: ASTNode → Chunk ───────────────────────────────────────────── */
#include "ast.h"

extern Value val_string(const char*); /* ensure available for constants */

static inline int chunk_add_const_str(Chunk *c, const char *sval) {
    if (c->const_count >= CHUNK_MAX_CONST) { c->ok = 0; return 0; }
    c->constants[c->const_count] = val_string(sval);
    return c->const_count++;
}

static void compile_node(Chunk *c, ASTNode *node);

static uint8_t compile_expr(Chunk *c, ASTNode *node) {
    if (!node || !c->ok) return 0;
    switch (node->type) {
        case NODE_INT_LIT: {
            int k = chunk_add_const_int(c, node->as.integer.value);
            uint8_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint8_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_FLOAT_LIT: {
            int k = chunk_add_const_float(c, node->as.real.value);
            uint8_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint8_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_BOOL_LIT: {
            int k = chunk_add_const_bool(c, node->as.boolean.value);
            uint8_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint8_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_STRING_LIT: {
            int k = chunk_add_const_str(c, node->as.str.value);
            uint8_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint8_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_IDENTIFIER: {
            if (node->resolved_offset < 0) { c->ok = 0; return 0; }
            return (uint8_t)node->resolved_offset;
        }
        case NODE_BINARY_EXPR: {
            uint8_t r1 = compile_expr(c, node->as.binary.left);
            uint8_t r2 = compile_expr(c, node->as.binary.right);
            uint8_t dst = c->next_reg++;
            Instruction i; i.a=dst; i.b=r1; i.c=r2; i.offset=0;
            const char *op = node->as.binary.op;
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
            else { c->ok = 0; return 0; }
            chunk_emit(c, i);
            return dst;
        }
        default:
            c->ok = 0;
            return 0;
    }
}

static void compile_node(Chunk *c, ASTNode *node) {
    if (!node || !c->ok) return;
    uint8_t start_reg = c->next_reg;
    switch (node->type) {
        case NODE_VAR_DECL:
        case NODE_ASSIGN: {
            ASTNode *val  = (node->type == NODE_VAR_DECL)
                          ? node->as.var_decl.initializer
                          : node->as.assign.value;
            uint8_t src = compile_expr(c, val);
            if (node->resolved_offset >= 0) {
                Instruction i; i.op=OP_MOVE; i.a=(uint8_t)node->resolved_offset; i.b=src; i.c=0; i.offset=0;
                chunk_emit(c, i);
            } else {
                c->ok = 0; /* Fallback required for unresolved vars */
            }
            break;
        }
        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++) {
                compile_node(c, node->as.list.children[i]);
                c->next_reg = start_reg; /* reset temp vars */
            }
            break;
        case NODE_IF: {
            uint8_t cond_reg = compile_expr(c, node->as.if_stmt.condition);
            Instruction jf; jf.op=OP_JUMP_IF_FALSE; jf.a=cond_reg; jf.b=0; jf.c=0; jf.offset=0;
            int jf_idx = chunk_emit(c, jf);
            
            c->next_reg = start_reg;
            compile_node(c, node->as.if_stmt.then_body);
            
            if (node->as.if_stmt.else_body) {
                Instruction jmp; jmp.op=OP_JUMP; jmp.a=0; jmp.b=0; jmp.c=0; jmp.offset=0;
                int jmp_idx = chunk_emit(c, jmp);
                chunk_patch(c, jf_idx, c->count);
                
                c->next_reg = start_reg;
                compile_node(c, node->as.if_stmt.else_body);
                
                chunk_patch(c, jmp_idx, c->count);
            } else {
                chunk_patch(c, jf_idx, c->count);
            }
            break;
        }
        /* Correção: Compilação do loop integrada */
        case NODE_WHILE: {
            int start_ip = c->count;
            
            uint8_t cond_reg = compile_expr(c, node->as.while_stmt.condition);
            if (!c->ok) return;

            Instruction jf; jf.op = OP_JUMP_IF_FALSE; jf.a = cond_reg; jf.b = 0; jf.c = 0; jf.offset = 0;
            int jf_idx = chunk_emit(c, jf);

            c->next_reg = start_reg;
            compile_node(c, node->as.while_stmt.body);
            if (!c->ok) return;

            Instruction jmp; jmp.op = OP_JUMP; jmp.a = 0; jmp.b = 0; jmp.c = 0; jmp.offset = start_ip;
            chunk_emit(c, jmp);

            chunk_patch(c, jf_idx, c->count);
            break;
        }
        case NODE_FUNC_CALL:
            c->ok = 0;
            break;
        default:
            c->ok = 0;
            break;
    }
    c->next_reg = start_reg;
}

/* Correção: Função que compila o loop inteiro e assinala o retorno */
static inline int chunk_compile_loop(Chunk *c, ASTNode *loop_node) {
    chunk_init(c);
    compile_node(c, loop_node);
    if (!c->ok) { chunk_free(c); return 0; }
    Instruction ret; ret.op = OP_RETURN; ret.a = 0; ret.b = 0; ret.c = 0; ret.offset = 0;
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

/* run compiled chunk with strict linear register model ───────────────────── */
/* run compiled chunk with strict linear register model ───────────────────── */
static inline int vm_run(Chunk *c, Scope *scope, Value *stack_ptr, int stack_size) {
    (void)scope;
    (void)stack_size;
    
    /* Usar ponteiros em vez de índices de array economiza ciclos de CPU */
    Instruction *ip = c->code;
    Instruction *end = c->code + c->count;
    Value *R = stack_ptr; 

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    static const void *dispatch[] = {
        &&L_LOADK, &&L_MOVE,
        &&L_ADD, &&L_SUB, &&L_MUL, &&L_DIV, &&L_MOD,
        &&L_EQ, &&L_NEQ, &&L_LT, &&L_GT, &&L_LTE, &&L_GTE,
        &&L_JUMP_IF_FALSE, &&L_JUMP, &&L_RETURN
    };
    
    #define NEXT() do { if (ip >= end) goto L_RETURN; \
                        goto *dispatch[(ip++)->op]; } while(0)

    /* Acessa a instrução atual (que já foi incrementada no NEXT) */
    #define i_a   ((ip-1)->a)
    #define i_b   ((ip-1)->b)
    #define i_c   ((ip-1)->c)
    #define i_off ((ip-1)->offset)

    NEXT();

    L_LOADK: { R[i_a] = c->constants[i_b]; NEXT(); }
    L_MOVE:  { R[i_a] = R[i_b]; NEXT(); }

    /* Fast paths usando ponteiros (sem cópia de structs) */
    L_ADD: { 
        Value *l = &R[i_b];
        Value *r = &R[i_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[i_a].type = VAL_INT; 
            R[i_a].as.integer = l->as.integer + r->as.integer;
        } else {
            R[i_a] = vm_arith(*l, *r, OP_ADD); 
        }
        NEXT(); 
    }
    L_SUB: { 
        Value *l = &R[i_b];
        Value *r = &R[i_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[i_a].type = VAL_INT; 
            R[i_a].as.integer = l->as.integer - r->as.integer;
        } else {
            R[i_a] = vm_arith(*l, *r, OP_SUB); 
        }
        NEXT(); 
    }
    
    L_MUL: { R[i_a] = vm_arith(R[i_b], R[i_c], OP_MUL); NEXT(); }
    L_DIV: { R[i_a] = vm_arith(R[i_b], R[i_c], OP_DIV); NEXT(); }
    L_MOD: { R[i_a] = vm_arith(R[i_b], R[i_c], OP_MOD); NEXT(); }

    L_LT:  { 
        Value *l = &R[i_b];
        Value *r = &R[i_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[i_a].type = VAL_BOOL; 
            R[i_a].as.boolean = l->as.integer < r->as.integer;
        } else {
            R[i_a] = vm_compare(*l, *r, OP_LT);  
        }
        NEXT(); 
    }
    
    L_EQ:  { R[i_a] = vm_compare(R[i_b], R[i_c], OP_EQ);  NEXT(); }
    L_NEQ: { R[i_a] = vm_compare(R[i_b], R[i_c], OP_NEQ); NEXT(); }
    L_GT:  { R[i_a] = vm_compare(R[i_b], R[i_c], OP_GT);  NEXT(); }
    L_LTE: { R[i_a] = vm_compare(R[i_b], R[i_c], OP_LTE); NEXT(); }
    L_GTE: { R[i_a] = vm_compare(R[i_b], R[i_c], OP_GTE); NEXT(); }

    L_JUMP_IF_FALSE: {
        Value *cond = &R[i_a];
        int truthy = 0;
        if (cond->type == VAL_BOOL) truthy = cond->as.boolean;
        else if (cond->type == VAL_INT) truthy = cond->as.integer != 0;
        else truthy = vm_truthy(*cond);

        if (!truthy) ip = c->code + i_off;
        NEXT();
    }
    
    L_JUMP: { ip = c->code + i_off; NEXT(); }
    L_RETURN: return 1;

    #pragma GCC diagnostic pop
    #undef NEXT
    #undef i_a
    #undef i_b
    #undef i_c
    #undef i_off

#else
    /* Fallback pointer-based simplificado */
    Instruction *ip = c->code;
    Instruction *end = c->code + c->count;
    Value *R = stack_ptr;

    while (ip < end) {
        Instruction *instr = ip++;
        switch (instr->op) {
            case OP_LOADK: R[instr->a] = c->constants[instr->b]; break;
            case OP_MOVE:  R[instr->a] = R[instr->b]; break;
            case OP_ADD:   {
                Value *l = &R[instr->b], *r = &R[instr->c];
                if (l->type == VAL_INT && r->type == VAL_INT) {
                    R[instr->a].type = VAL_INT; R[instr->a].as.integer = l->as.integer + r->as.integer;
                } else R[instr->a] = vm_arith(*l, *r, OP_ADD);
                break;
            }
            case OP_SUB:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_SUB); break;
            case OP_MUL:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_MUL); break;
            case OP_DIV:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_DIV); break;
            case OP_MOD:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_MOD); break;
            case OP_EQ:    R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_EQ); break;
            case OP_NEQ:   R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_NEQ); break;
            case OP_LT:    {
                Value *l = &R[instr->b], *r = &R[instr->c];
                if (l->type == VAL_INT && r->type == VAL_INT) {
                    R[instr->a].type = VAL_BOOL; R[instr->a].as.boolean = l->as.integer < r->as.integer;
                } else R[instr->a] = vm_compare(*l, *r, OP_LT);
                break;
            }
            case OP_GT:    R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_GT); break;
            case OP_LTE:   R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_LTE); break;
            case OP_GTE:   R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_GTE); break;
            case OP_JUMP_IF_FALSE: {
                Value *cond = &R[instr->a];
                int truthy = 0;
                if (cond->type == VAL_BOOL) truthy = cond->as.boolean;
                else if (cond->type == VAL_INT) truthy = cond->as.integer != 0;
                else truthy = vm_truthy(*cond);
                if (!truthy) ip = c->code + instr->offset;
                break;
            }
            case OP_JUMP:  ip = c->code + instr->offset; break;
            case OP_RETURN: return 1;
        }
    }
    return 1;
#endif
}

#endif /* FLUXA_BYTECODE_H */