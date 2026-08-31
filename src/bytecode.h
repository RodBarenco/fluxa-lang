/* bytecode.h — Fluxa Bytecode VM (Sprint 4 performance)
 * Issue #32: implementation moved to bytecode.c
 * Issue #33: next_reg is now uint16_t (was uint8_t — silent overflow at 128)
 * v0.14: OP_CALL_METHOD and OP_CALL_FUNC — Block methods and fns in the VM
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
    OP_RETURN,
    /* v0.14: call opcodes — bridge VM back to runtime C via callback ───── */
    /* OP_CALL_METHOD: inst.method(args)
     *   a      = dest register for return value
     *   b      = constants[] index → VAL_STRING owner name
     *   c      = constants[] index → VAL_STRING method name
     *   offset = (first_arg_reg << 8) | arg_count  (each fits in 8 bits) */
    OP_CALL_METHOD,
    /* OP_CALL_FUNC: fn(args)
     *   a      = dest register for return value
     *   b      = constants[] index → VAL_STRING function name
     *   c      = first arg register
     *   offset = arg_count */
    OP_CALL_FUNC,
    /* OP_GET_FIELD: R[a] = inst.field  (Block field read)
     *   a = dest register
     *   b = constants[] index → VAL_STRING owner name
     *   c = constants[] index → VAL_STRING field name   */
    OP_GET_FIELD,
    /* OP_SET_FIELD: inst.field = R[a]  (Block field write)
     *   a = src register (value to write)
     *   b = constants[] index → VAL_STRING owner name
     *   c = constants[] index → VAL_STRING field name   */
    OP_SET_FIELD,
    /* Fixed numeric/bool array indexing. Array payloads remain on the heap;
     * the VM caches only the address of their official Value descriptor. */
    OP_GET_INDEX,
    OP_PREP_INDEX,
    OP_SET_INDEX,
    /* OP_RETURN_VAL: return a value from a compiled function body.
     *   a = register holding return value.
     * OP_RETURN_NIL: return nil (void functions).
     * Both terminate vm_run_fn execution.                              */
    OP_RETURN_VAL,
    OP_RETURN_NIL,
    /* Ownership-aware variants. Numeric LOADK/MOVE stay branch-free. */
    OP_LOADK_STR,
    OP_MOVE_STR,
    OP_DROP_STR,
    /* Field read proven to be int/float/bool at compile time: the destination
     * never holds or receives a string, so it stays off the string band and
     * needs neither a release before the write nor a drop after it. */
    OP_GET_FIELD_V,
    /* Logical operators.  Two opcodes because the evaluator uses two
     * different truthiness rules: `!` treats a 0.0 float as false, while
     * `&&`/`||` treat every non-nil non-bool non-int value as true.  The VM
     * has to reproduce each one exactly, not unify them.
     *   OP_NOT     a = dst, b = src   — the `!` rule, negated
     *   OP_TRUTHY  a = dst, b = src   — the `&&`/`||` rule                */
    OP_NOT,
    OP_TRUTHY
} Opcode;

/* ── Call callback — passed to vm_run; bridges back to runtime C ─────────── */
/* owner_kv: NULL for plain function (OP_CALL_FUNC).
 *           For OP_CALL_METHOD: pointer to c->constants[b] — mutable so
 *           callback can patch VAL_STRING→VAL_PTR(BlockInstance*) inline cache.
 * args: pointer directly into R[first_arg] — NO copy, NO malloc in VM.
 *       Callback must NOT store this pointer past its return. */
typedef Value (*vm_call_cb_t)(void       *rt_opaque,
                               Value      *owner_kv,
                               const char *method_or_func,
                               Value      *args,
                               int         argc);

typedef int (*vm_indexable_cb_t)(void *rt, const char *name,
                                 int resolved_offset);

/* Compile-time probe for a bare identifier the resolver could not map to a
 * stack slot.  Returns the Block instance name to bake into OP_GET_FIELD /
 * OP_SET_FIELD when the identifier is a primitive scalar field of the current
 * instance, or NULL when it is not eligible and the caller must demote.
 * Restricting this to int/float/bool fields keeps the existing field opcodes
 * and their callbacks unchanged: no string, arr or dyn ownership crosses the
 * VM boundary through this path. */
/* Compile-time probe into the runtime.  Answers are read from the Block
 * declaration's AST — the declared type, never the current value — so a chunk
 * compiled once stays valid for the whole run.  All of it happens while
 * building the chunk; nothing here is on the per-iteration path.
 *
 *   VM_PROBE_BARE_FIELD  name is a bare identifier; owner is ignored.
 *                        Returns the current instance name to bake into the
 *                        field opcode, or NULL to demote as before.
 *   VM_PROBE_FIELD       owner.name; returns owner when the field exists.
 *   VM_PROBE_RET         owner.name method (owner NULL = plain function);
 *                        returns non-NULL when the declaration was found.
 *
 * *flag is set to 1 when the declared type can never be a string — an
 * int/float/bool field for the field probes, or a non-str return type for
 * VM_PROBE_RET.  Unknown owners (stdlib, FFI) report 0 and keep the
 * ownership-aware encoding. */
enum { VM_PROBE_BARE_FIELD = 0, VM_PROBE_FIELD, VM_PROBE_RET };

typedef const char *(*vm_probe_cb_t)(void *rt, int kind, const char *owner,
                                     const char *name, int *flag);

/* ── Instruction (3-address register-based) ──────────────────────────────── */
typedef struct {
    Opcode   op;
    uint16_t a;       /* dest register    — Issue #33: uint16_t */
    uint16_t b;       /* src1 / const idx — Issue #33: uint16_t */
    uint16_t c;       /* src2 register    — Issue #33: uint16_t */
    int      offset;  /* jump target / arg encoding */
} Instruction;

/* ── Chunk — compiled bytecode ───────────────────────────────────────────── */
#define CHUNK_INIT_CAP  64
#define CHUNK_MAX_CONST 128
#define CHUNK_MAX_REG   512

typedef struct {
    Instruction *code;
    int          count;
    int          cap;
    Value        constants[CHUNK_MAX_CONST];
    /* Strings synthesized for internal method keys and array-name operands.
     * Ordinary constants retain their historical ownership rules; these bits
     * let chunk_free release only the new allocations introduced by the
     * namespace encoding. */
    unsigned char owned_strings[(CHUNK_MAX_CONST + 7) / 8];
    int          const_count;
    int          ok;
    uint16_t     next_reg;   /* Issue #33: uint16_t — starts at 128 */
    uint16_t     peak_reg;   /* highest temporary register end, across resets */
    unsigned char string_regs[CHUNK_MAX_REG / 8];
    Value        return_value; /* vm_run loop-to-evaluator return channel */
    int          did_return;
    vm_indexable_cb_t indexable_cb;
    vm_probe_cb_t     probe_cb;
    void             *indexable_rt;
} Chunk;

/* ── Chunk lifecycle (inline — trivial) ──────────────────────────────────── */
static inline void chunk_init(Chunk *c) {
    c->code  = (Instruction*)malloc(sizeof(Instruction) * CHUNK_INIT_CAP);
    c->count = 0;
    c->cap   = CHUNK_INIT_CAP;
    c->const_count = 0;
    memset(c->owned_strings, 0, sizeof(c->owned_strings));
    c->ok    = 1;
    c->next_reg = 128;
    c->peak_reg = 128;
    memset(c->string_regs, 0, sizeof(c->string_regs));
    c->return_value = val_nil();
    c->did_return = 0;
    c->indexable_cb = NULL;
    c->probe_cb = NULL;
    c->indexable_rt = NULL;
}

static inline void chunk_free(Chunk *c) {
    for (int i = 0; i < c->const_count; i++) {
        if ((c->owned_strings[i >> 3] & (1u << (i & 7))) &&
            c->constants[i].type == VAL_STRING) {
            fxstr_release(c->constants[i].as.string);
            c->constants[i].as.string = NULL;
        }
    }
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
    int index = c->const_count++;
    c->constants[index] = val_string(sval);
    c->owned_strings[index >> 3] |= (unsigned char)(1u << (index & 7));
    return index;
}

/* ── Public API (implemented in bytecode.c) ──────────────────────────────── */
enum { VM_INDEX_GET = 0, VM_INDEX_PREP, VM_INDEX_SET };

typedef struct {
    Value *slot;
    int    prst_index; /* -2 unresolved, -1 ordinary array, >=0 pool entry */
    int    failed;     /* callback raised a runtime error: stop this VM chunk */
} VMIndexCache;

typedef Value (*vm_index_cb_t)(void *rt, int action,
                               VMIndexCache *cache, const char *name,
                               Value *registers, int register_count,
                               int resolved_offset, Value index,
                               Value incoming);

int chunk_compile_loop(Chunk *c, ASTNode *loop_node,
                       vm_indexable_cb_t indexable_cb,
                       vm_probe_cb_t probe_cb, void *indexable_rt);
/* Compile a function body — uses OP_RETURN_VAL / OP_RETURN_NIL.
 * Params are at resolved_offset 0..param_count-1 in the register file. */
int chunk_compile_fn(Chunk *c, ASTNode *fn_node,
                     vm_indexable_cb_t indexable_cb,
                     vm_probe_cb_t probe_cb, void *indexable_rt);

/* cancel_flag: NULL for normal; set *cancel_flag=1 to abort (used by -dev).
 * call_cb / rt_opaque: dispatch OP_CALL_METHOD / OP_CALL_FUNC to runtime C.
 * tick_cb: called at every OP_JUMP back-edge alongside cancel_flag check.
 *   Used by runtime for GC sweep and mailbox processing. NULL = no tick.
 *   Signature: void tick_cb(void *rt_opaque)                              */
typedef void (*vm_tick_cb_t)(void *rt_opaque);

/* Field access callbacks for OP_GET_FIELD / OP_SET_FIELD.
 * owner_kv: pointer to c->constants[b] — mutable so the callback can patch
 * it from VAL_STRING("c1") to VAL_PTR(BlockInstance*) on first call.
 * Subsequent calls skip resolve_instance entirely (O(1) pointer deref).
 * NULL = no field access support (loop has no Block field ops).           */
typedef Value (*vm_get_field_cb_t)(void *rt, Value *owner_kv, const char *field);
typedef void  (*vm_set_field_cb_t)(void *rt, Value *owner_kv, const char *field, Value val);

/* vm_run_fn: execute a compiled function body chunk.
 * fn_stack: pre-allocated register file with params at [0..param_count-1].
 * Returns the value from OP_RETURN_VAL, or VAL_NIL for OP_RETURN_NIL/end.
 * Separate from vm_run: no Scope*, no tick_cb, no field callbacks needed. */
Value vm_run_fn(Chunk *c, Value *fn_stack, int fn_stack_size,
                vm_call_cb_t      call_cb,
                vm_get_field_cb_t get_field_cb,
                vm_set_field_cb_t set_field_cb,
                vm_index_cb_t     index_cb,
                void             *rt_opaque);

/* Called by OP_MOVE when writing to a variable register (dst < 128, the
 * param/local band) and either the old slot value or the incoming value is
 * a VAL_DYN. Lets the runtime keep the GC slot=>pin invariant (rt_set
 * semantics: unpin the overwritten dyn, pin the stored one) for stores
 * performed by the VM. Without it, a dyn assigned inside a compiled loop
 * has pin_count 0 and the back-edge gc_sweep frees it while the variable
 * still references it (use-after-free). May be NULL (no dyn tracking). */
typedef void (*vm_store_cb_t)(void *rt_opaque, Value *slot, Value *incoming);

int vm_run(Chunk *c, Scope *scope, Value *stack_ptr, int stack_size,
           volatile int *cancel_flag,
           vm_call_cb_t      call_cb,
           void             *rt_opaque,
           vm_tick_cb_t      tick_cb,
           vm_get_field_cb_t get_field_cb,
           vm_set_field_cb_t set_field_cb,
           vm_store_cb_t     store_cb,
           vm_index_cb_t     index_cb);

#endif /* FLUXA_BYTECODE_H */
