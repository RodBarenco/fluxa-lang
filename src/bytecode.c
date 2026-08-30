/* bytecode.c — Fluxa Bytecode VM implementation
 * Issue #32: extracted from bytecode.h (was header-only)
 * Issue #33: next_reg / register fields are uint16_t
 * v0.14: OP_CALL_METHOD (Block methods) and OP_CALL_FUNC (plain fns) added.
 *        vm_run now accepts call_cb + rt_opaque for bridging back to runtime.
 */
#define _POSIX_C_SOURCE 200809L
#include "bytecode.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Compiler: ASTNode → Chunk ───────────────────────────────────────────── */
static void compile_node(Chunk *c, ASTNode *node);

static int chunk_add_method_key(Chunk *c, const char *owner,
                                const char *method) {
    BlockInstance *inst = block_inst_find(owner);
    if (!inst && !block_def_find(owner))
        return chunk_add_const_str(c, method); /* stdlib / FFI */
    char key[256];
    if (!block_method_key(method, key, sizeof(key))) {
        c->ok = 0;
        return 0;
    }
    int index = chunk_add_const_str(c, key);
    if (c->ok)
        c->owned_method_keys[index >> 3] |= (unsigned char)(1u << (index & 7));
    return index;
}

static uint16_t compile_expr(Chunk *c, ASTNode *node) {
    if (!node || !c->ok) return 0;
    switch (node->type) {
        case NODE_INT_LIT: {
            int k = chunk_add_const_int(c, node->as.integer.value);
            uint16_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint16_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_FLOAT_LIT: {
            int k = chunk_add_const_float(c, node->as.real.value);
            uint16_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint16_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_BOOL_LIT: {
            int k = chunk_add_const_bool(c, node->as.boolean.value);
            uint16_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint16_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_STRING_LIT: {
            int k = chunk_add_const_str(c, node->as.str.value);
            uint16_t dst = c->next_reg++;
            Instruction i; i.op=OP_LOADK; i.a=dst; i.b=(uint16_t)k; i.c=0; i.offset=0;
            chunk_emit(c, i); return dst;
        }
        case NODE_IDENTIFIER: {
            if (node->resolved_offset < 0) { c->ok = 0; return 0; }
            return (uint16_t)node->resolved_offset;
        }
        case NODE_BINARY_EXPR: {
            uint16_t r1 = compile_expr(c, node->as.binary.left);
            uint16_t r2 = compile_expr(c, node->as.binary.right);
            uint16_t dst = c->next_reg++;
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
        case NODE_MEMBER_ACCESS: {
            /* inst.field as expression → OP_GET_FIELD */
            int owner_k = chunk_add_const_str(c, node->as.member_access.owner);
            int field_k = chunk_add_const_str(c, node->as.member_access.field);
            if (!c->ok) return 0;
            uint16_t dst = c->next_reg++;
            Instruction ins; ins.op=OP_GET_FIELD; ins.a=dst;
            ins.b=(uint16_t)owner_k; ins.c=(uint16_t)field_k; ins.offset=0;
            chunk_emit(c, ins);
            return dst;
        }
        case NODE_MEMBER_CALL: {
            /* inst.method(args) as expression — compile to OP_CALL_METHOD */
            int owner_k  = chunk_add_const_str(c, node->as.member_call.owner);
            int method_k = chunk_add_method_key(c, node->as.member_call.owner,
                                                node->as.member_call.method);
            if (!c->ok) return 0;
            /* Evaluate args into consecutive temp registers */
            int argc = node->as.member_call.arg_count;
            uint16_t first_arg = c->next_reg;
            for (int i = 0; i < argc; i++) {
                uint16_t ar = compile_expr(c, node->as.member_call.args[i]);
                if (!c->ok) return 0;
                /* Move into consecutive slots if not already there */
                if (ar != first_arg + (uint16_t)i) {
                    Instruction mv;
                    mv.op=(Opcode)OP_MOVE; mv.a=first_arg+(uint16_t)i;
                    mv.b=ar; mv.c=0; mv.offset=0;
                    chunk_emit(c, mv);
                }
                c->next_reg = first_arg + (uint16_t)i + 1;
            }
            uint16_t dst = c->next_reg++;
            /* Pack first_arg_reg and argc into offset field.
             * Both fit in 16 bits each; combined in a 32-bit int. */
            Instruction ins;
            ins.op = OP_CALL_METHOD;
            ins.a  = dst;
            ins.b  = (uint16_t)owner_k;
            ins.c  = (uint16_t)method_k;
            ins.offset = ((int)first_arg << 8) | (argc & 0xFF);
            chunk_emit(c, ins);
            return dst;
        }
        case NODE_FUNC_CALL: {
            /* fn(args) as expression — compile to OP_CALL_FUNC */
            int fn_k = chunk_add_const_str(c, node->as.list.name);
            if (!c->ok) return 0;
            int argc = node->as.list.count;
            uint16_t first_arg = c->next_reg;
            for (int i = 0; i < argc; i++) {
                uint16_t ar = compile_expr(c, node->as.list.children[i]);
                if (!c->ok) return 0;
                if (ar != first_arg + (uint16_t)i) {
                    Instruction mv;
                    mv.op=OP_MOVE; mv.a=first_arg+(uint16_t)i;
                    mv.b=ar; mv.c=0; mv.offset=0;
                    chunk_emit(c, mv);
                }
                c->next_reg = first_arg + (uint16_t)i + 1;
            }
            uint16_t dst = c->next_reg++;
            Instruction ins;
            ins.op = OP_CALL_FUNC;
            ins.a  = dst;
            ins.b  = (uint16_t)fn_k;
            ins.c  = first_arg;
            ins.offset = argc;
            chunk_emit(c, ins);
            return dst;
        }
        default:
            c->ok = 0;
            return 0;
    }
}

static void compile_node(Chunk *c, ASTNode *node) {
    if (!node || !c->ok) return;
    uint16_t start_reg = c->next_reg;
    switch (node->type) {
        case NODE_VAR_DECL:
        case NODE_ASSIGN: {
            ASTNode *val = (node->type == NODE_VAR_DECL)
                         ? node->as.var_decl.initializer
                         : node->as.assign.value;
            uint16_t src = compile_expr(c, val);
            if (!c->ok) break;
            if (node->resolved_offset >= 0) {
                Instruction i; i.op=OP_MOVE; i.a=(uint16_t)node->resolved_offset;
                i.b=src; i.c=0; i.offset=0;
                chunk_emit(c, i);
            } else {
                c->ok = 0;
            }
            break;
        }
        case NODE_BLOCK_STMT:
            for (int i = 0; i < node->as.list.count; i++) {
                compile_node(c, node->as.list.children[i]);
                if (!c->ok) return;
                c->next_reg = start_reg;
            }
            break;
        case NODE_IF: {
            uint16_t cond_reg = compile_expr(c, node->as.if_stmt.condition);
            if (!c->ok) return;
            Instruction jf; jf.op=OP_JUMP_IF_FALSE; jf.a=cond_reg; jf.b=0; jf.c=0; jf.offset=0;
            int jf_idx = chunk_emit(c, jf);
            c->next_reg = start_reg;
            compile_node(c, node->as.if_stmt.then_body);
            if (!c->ok) return;
            if (node->as.if_stmt.else_body) {
                Instruction jmp; jmp.op=OP_JUMP; jmp.a=0; jmp.b=0; jmp.c=0; jmp.offset=0;
                int jmp_idx = chunk_emit(c, jmp);
                chunk_patch(c, jf_idx, c->count);
                c->next_reg = start_reg;
                compile_node(c, node->as.if_stmt.else_body);
                if (!c->ok) return;
                chunk_patch(c, jmp_idx, c->count);
            } else {
                chunk_patch(c, jf_idx, c->count);
            }
            break;
        }
        case NODE_WHILE: {
            int start_ip = c->count;
            uint16_t cond_reg = compile_expr(c, node->as.while_stmt.condition);
            if (!c->ok) return;
            Instruction jf; jf.op=OP_JUMP_IF_FALSE; jf.a=cond_reg; jf.b=0; jf.c=0; jf.offset=0;
            int jf_idx = chunk_emit(c, jf);
            c->next_reg = start_reg;
            compile_node(c, node->as.while_stmt.body);
            if (!c->ok) return;
            Instruction jmp; jmp.op=OP_JUMP; jmp.a=0; jmp.b=0; jmp.c=0; jmp.offset=start_ip;
            chunk_emit(c, jmp);
            chunk_patch(c, jf_idx, c->count);
            break;
        }
        case NODE_MEMBER_CALL: {
            /* inst.method(args) as statement — compile to OP_CALL_METHOD,
             * result (if any) discarded */
            int owner_k  = chunk_add_const_str(c, node->as.member_call.owner);
            int method_k = chunk_add_method_key(c, node->as.member_call.owner,
                                                node->as.member_call.method);
            if (!c->ok) break;
            int argc = node->as.member_call.arg_count;
            uint16_t first_arg = c->next_reg;
            for (int i = 0; i < argc; i++) {
                uint16_t ar = compile_expr(c, node->as.member_call.args[i]);
                if (!c->ok) return;
                if (ar != first_arg + (uint16_t)i) {
                    Instruction mv;
                    mv.op=OP_MOVE; mv.a=first_arg+(uint16_t)i;
                    mv.b=ar; mv.c=0; mv.offset=0;
                    chunk_emit(c, mv);
                }
                c->next_reg = first_arg + (uint16_t)i + 1;
            }
            uint16_t dst = c->next_reg++;
            Instruction ins;
            ins.op = OP_CALL_METHOD;
            ins.a  = dst;
            ins.b  = (uint16_t)owner_k;
            ins.c  = (uint16_t)method_k;
            ins.offset = ((int)first_arg << 8) | (argc & 0xFF);
            chunk_emit(c, ins);
            break;
        }
        case NODE_MEMBER_ASSIGN: {
            /* inst.field = expr → compile expr + OP_SET_FIELD */
            int owner_k = chunk_add_const_str(c, node->as.member_assign.owner);
            int field_k = chunk_add_const_str(c, node->as.member_assign.field);
            if (!c->ok) break;
            uint16_t src = compile_expr(c, node->as.member_assign.value);
            if (!c->ok) break;
            Instruction ins; ins.op=OP_SET_FIELD; ins.a=src;
            ins.b=(uint16_t)owner_k; ins.c=(uint16_t)field_k; ins.offset=0;
            chunk_emit(c, ins);
            break;
        }
        case NODE_FUNC_CALL: {
            /* fn(args) as statement */
            int fn_k = chunk_add_const_str(c, node->as.list.name);
            if (!c->ok) break;
            int argc = node->as.list.count;
            uint16_t first_arg = c->next_reg;
            for (int i = 0; i < argc; i++) {
                uint16_t ar = compile_expr(c, node->as.list.children[i]);
                if (!c->ok) return;
                if (ar != first_arg + (uint16_t)i) {
                    Instruction mv;
                    mv.op=OP_MOVE; mv.a=first_arg+(uint16_t)i;
                    mv.b=ar; mv.c=0; mv.offset=0;
                    chunk_emit(c, mv);
                }
                c->next_reg = first_arg + (uint16_t)i + 1;
            }
            uint16_t dst = c->next_reg++;
            Instruction ins;
            ins.op = OP_CALL_FUNC;
            ins.a  = dst;
            ins.b  = (uint16_t)fn_k;
            ins.c  = first_arg;
            ins.offset = argc;
            chunk_emit(c, ins);
            break;
        }
        case NODE_RETURN: {
            if (node->as.ret.value) {
                uint16_t r = compile_expr(c, node->as.ret.value);
                if (!c->ok) break;
                Instruction ins; ins.op=OP_RETURN_VAL; ins.a=r;
                ins.b=0; ins.c=0; ins.offset=0;
                chunk_emit(c, ins);
            } else {
                Instruction ins; ins.op=OP_RETURN_NIL; ins.a=0;
                ins.b=0; ins.c=0; ins.offset=0;
                chunk_emit(c, ins);
            }
            break;
        }
        default:
            c->ok = 0;
            break;
    }
    c->next_reg = start_reg;
}

int chunk_compile_loop(Chunk *c, ASTNode *loop_node) {
    chunk_init(c);
    compile_node(c, loop_node);
    if (!c->ok) { chunk_free(c); return 0; }
    Instruction ret; ret.op=OP_RETURN; ret.a=0; ret.b=0; ret.c=0; ret.offset=0;
    chunk_emit(c, ret);
    return 1;
}

int chunk_compile_fn(Chunk *c, ASTNode *fn_node) {
    if (!fn_node || fn_node->type != NODE_FUNC_DECL) return 0;
    chunk_init(c);
    ASTNode *body = fn_node->as.func_decl.body;
    /* Params are pre-bound at resolved_offset 0..param_count-1 by caller.
     * next_reg starts at 128 to avoid collision with param/local slots. */
    uint16_t peak_reg = 128;
    for (int i = 0; i < body->as.list.count; i++) {
        compile_node(c, body->as.list.children[i]);
        if (!c->ok) { chunk_free(c); return 0; }
        /* Track peak before reset — fn_regs must be large enough */
        if (c->next_reg > peak_reg) peak_reg = c->next_reg;
        c->next_reg = 128;  /* reset temps after each statement */
    }
    /* Store peak so caller can allocate fn_regs correctly */
    c->next_reg = peak_reg;
    /* Implicit nil return if no explicit OP_RETURN_VAL/NIL emitted */
    Instruction ret; ret.op=OP_RETURN_NIL; ret.a=0; ret.b=0; ret.c=0; ret.offset=0;
    chunk_emit(c, ret);
    return 1;
}

/* ── VM helpers ──────────────────────────────────────────────────────────── */
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
    /* String equality in the tree-walker is content-based. Keep the VM
     * identical: treating the Value union as a double here compared pointer
     * bits by accident, so separately allocated equal strings failed only
     * after a while/function had been compiled to bytecode. */
    if (l.type == VAL_STRING && r.type == VAL_STRING) {
        const char *ls = l.as.string ? l.as.string : "";
        const char *rs = r.as.string ? r.as.string : "";
        if (op == OP_EQ)  return val_bool(strcmp(ls, rs) == 0);
        if (op == OP_NEQ) return val_bool(strcmp(ls, rs) != 0);
        return val_bool(0);
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

/* ── VM execution ────────────────────────────────────────────────────────── */
/* ── vm_run_fn — execute a compiled function body ───────────────────────── */
/* Returns the Value from OP_RETURN_VAL, or VAL_NIL for OP_RETURN_NIL/end.
 * fn_stack: isolated register file (not rt->stack) — no frame save/restore.
 * Params must be pre-loaded at fn_stack[0..param_count-1] by caller.      */
Value vm_run_fn(Chunk *c, Value *fn_stack, int fn_stack_size,
                vm_call_cb_t      call_cb,
                vm_get_field_cb_t get_field_cb,
                vm_set_field_cb_t set_field_cb,
                void             *rt_opaque) {
    (void)fn_stack_size;
    Value retval; retval.type = VAL_NIL;

    Instruction *ip  = c->code;
    Instruction *end = c->code + c->count;
    Value       *R   = fn_stack;

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    static const void *fn_dispatch[] = {
        &&FN_LOADK, &&FN_MOVE,
        &&FN_ADD, &&FN_SUB, &&FN_MUL, &&FN_DIV, &&FN_MOD,
        &&FN_EQ, &&FN_NEQ, &&FN_LT, &&FN_GT, &&FN_LTE, &&FN_GTE,
        &&FN_JUMP_IF_FALSE, &&FN_JUMP, &&FN_RETURN,
        &&FN_CALL_METHOD, &&FN_CALL_FUNC,
        &&FN_GET_FIELD, &&FN_SET_FIELD,
        &&FN_RETURN_VAL, &&FN_RETURN_NIL
    };

    #define FNEXT() do { if (ip >= end) goto FN_RETURN_NIL;                          goto *fn_dispatch[(ip++)->op]; } while(0)
    #define fn_a   ((ip-1)->a)
    #define fn_b   ((ip-1)->b)
    #define fn_c   ((ip-1)->c)
    #define fn_off ((ip-1)->offset)

    FNEXT();

    FN_LOADK: { R[fn_a] = c->constants[fn_b]; FNEXT(); }
    FN_MOVE:  { R[fn_a] = R[fn_b]; FNEXT(); }

    FN_ADD: {
        Value *l = &R[fn_b], *r = &R[fn_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[fn_a].type = VAL_INT;
            R[fn_a].as.integer = l->as.integer + r->as.integer;
        } else { R[fn_a] = vm_arith(*l, *r, OP_ADD); }
        FNEXT();
    }
    FN_SUB: {
        Value *l = &R[fn_b], *r = &R[fn_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[fn_a].type = VAL_INT;
            R[fn_a].as.integer = l->as.integer - r->as.integer;
        } else { R[fn_a] = vm_arith(*l, *r, OP_SUB); }
        FNEXT();
    }
    FN_MUL: { R[fn_a] = vm_arith(R[fn_b], R[fn_c], OP_MUL); FNEXT(); }
    FN_DIV: { R[fn_a] = vm_arith(R[fn_b], R[fn_c], OP_DIV); FNEXT(); }
    FN_MOD: { R[fn_a] = vm_arith(R[fn_b], R[fn_c], OP_MOD); FNEXT(); }

    FN_LT: {
        Value *l = &R[fn_b], *r = &R[fn_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[fn_a].type = VAL_BOOL;
            R[fn_a].as.boolean = l->as.integer < r->as.integer;
        } else { R[fn_a] = vm_compare(*l, *r, OP_LT); }
        FNEXT();
    }
    FN_EQ:  { R[fn_a] = vm_compare(R[fn_b], R[fn_c], OP_EQ);  FNEXT(); }
    FN_NEQ: { R[fn_a] = vm_compare(R[fn_b], R[fn_c], OP_NEQ); FNEXT(); }
    FN_GT:  { R[fn_a] = vm_compare(R[fn_b], R[fn_c], OP_GT);  FNEXT(); }
    FN_LTE: { R[fn_a] = vm_compare(R[fn_b], R[fn_c], OP_LTE); FNEXT(); }
    FN_GTE: { R[fn_a] = vm_compare(R[fn_b], R[fn_c], OP_GTE); FNEXT(); }

    FN_JUMP_IF_FALSE: {
        Value *cond = &R[fn_a];
        int truthy = (cond->type == VAL_BOOL) ? cond->as.boolean :
                     (cond->type == VAL_INT)  ? (cond->as.integer != 0) :
                     vm_truthy(*cond);
        if (!truthy) ip = c->code + fn_off;
        FNEXT();
    }
    FN_JUMP: { ip = c->code + fn_off; FNEXT(); }

    FN_CALL_METHOD: {
        if (call_cb) {
            const char *method = c->constants[fn_c].as.string;
            int first_arg = (fn_off >> 8) & 0xFF;
            int argc      =  fn_off       & 0xFF;
            R[fn_a] = call_cb(rt_opaque, &c->constants[fn_b], method,
                              &R[first_arg], argc);
        } else { R[fn_a].type = VAL_NIL; }
        FNEXT();
    }
    FN_CALL_FUNC: {
        if (call_cb) {
            const char *fn_name = c->constants[fn_b].as.string;
            R[fn_a] = call_cb(rt_opaque, NULL, fn_name, &R[fn_c], fn_off);
        } else { R[fn_a].type = VAL_NIL; }
        FNEXT();
    }
    FN_GET_FIELD: {
        if (get_field_cb)
            R[fn_a] = get_field_cb(rt_opaque, &c->constants[fn_b],
                                   c->constants[fn_c].as.string);
        else R[fn_a].type = VAL_NIL;
        FNEXT();
    }
    FN_SET_FIELD: {
        if (set_field_cb)
            set_field_cb(rt_opaque, &c->constants[fn_b],
                         c->constants[fn_c].as.string, R[fn_a]);
        FNEXT();
    }
    FN_RETURN_VAL: { retval = R[fn_a]; goto fn_done; }
    FN_RETURN_NIL: { goto fn_done; }
    FN_RETURN:     { goto fn_done; }  /* OP_RETURN = loop sentinel, treat as nil */

fn_done:
    #pragma GCC diagnostic pop
    #undef FNEXT
    #undef fn_a
    #undef fn_b
    #undef fn_c
    #undef fn_off
    return retval;
#else
    /* Fallback */
    while (ip < end) {
        Instruction *ins = ip++;
        switch (ins->op) {
            case OP_LOADK: R[ins->a] = c->constants[ins->b]; break;
            case OP_MOVE:  R[ins->a] = R[ins->b]; break;
            case OP_ADD: {
                Value *l=&R[ins->b],*r=&R[ins->c];
                if(l->type==VAL_INT&&r->type==VAL_INT){R[ins->a].type=VAL_INT;R[ins->a].as.integer=l->as.integer+r->as.integer;}
                else R[ins->a]=vm_arith(*l,*r,OP_ADD); break;
            }
            case OP_SUB:   R[ins->a]=vm_arith(R[ins->b],R[ins->c],OP_SUB); break;
            case OP_MUL:   R[ins->a]=vm_arith(R[ins->b],R[ins->c],OP_MUL); break;
            case OP_DIV:   R[ins->a]=vm_arith(R[ins->b],R[ins->c],OP_DIV); break;
            case OP_MOD:   R[ins->a]=vm_arith(R[ins->b],R[ins->c],OP_MOD); break;
            case OP_EQ:    R[ins->a]=vm_compare(R[ins->b],R[ins->c],OP_EQ); break;
            case OP_NEQ:   R[ins->a]=vm_compare(R[ins->b],R[ins->c],OP_NEQ); break;
            case OP_LT:    R[ins->a]=vm_compare(R[ins->b],R[ins->c],OP_LT); break;
            case OP_GT:    R[ins->a]=vm_compare(R[ins->b],R[ins->c],OP_GT); break;
            case OP_LTE:   R[ins->a]=vm_compare(R[ins->b],R[ins->c],OP_LTE); break;
            case OP_GTE:   R[ins->a]=vm_compare(R[ins->b],R[ins->c],OP_GTE); break;
            case OP_JUMP_IF_FALSE: {
                int t=(R[ins->a].type==VAL_BOOL)?R[ins->a].as.boolean:vm_truthy(R[ins->a]);
                if(!t) ip=c->code+ins->offset; break;
            }
            case OP_JUMP: ip=c->code+ins->offset; break;
            case OP_CALL_METHOD:
                if(call_cb){const char *m=c->constants[ins->c].as.string;int fa=(ins->offset>>8)&0xFF,argc=ins->offset&0xFF;R[ins->a]=call_cb(rt_opaque,&c->constants[ins->b],m,&R[fa],argc);}else R[ins->a].type=VAL_NIL; break;
            case OP_CALL_FUNC:
                if(call_cb){R[ins->a]=call_cb(rt_opaque,NULL,c->constants[ins->b].as.string,&R[ins->c],ins->offset);}else R[ins->a].type=VAL_NIL; break;
            case OP_GET_FIELD:
                if(get_field_cb)R[ins->a]=get_field_cb(rt_opaque,&c->constants[ins->b],c->constants[ins->c].as.string);else R[ins->a].type=VAL_NIL; break;
            case OP_SET_FIELD:
                if(set_field_cb)set_field_cb(rt_opaque,&c->constants[ins->b],c->constants[ins->c].as.string,R[ins->a]); break;
            case OP_RETURN_VAL: return R[ins->a];
            case OP_RETURN_NIL: return retval;
            case OP_RETURN:     return retval;
        }
    }
    return retval;
#endif
}

int vm_run(Chunk *c, Scope *scope, Value *stack_ptr, int stack_size,
           volatile int *cancel_flag,
           vm_call_cb_t      call_cb,
           void             *rt_opaque,
           vm_tick_cb_t      tick_cb,
           vm_get_field_cb_t get_field_cb,
           vm_set_field_cb_t set_field_cb,
           vm_store_cb_t     store_cb) {
    (void)scope;

    Instruction *ip  = c->code;
    Instruction *end = c->code + c->count;
    Value       *R   = stack_ptr;

    (void)stack_size;

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    static const void *dispatch[] = {
        &&L_LOADK, &&L_MOVE,
        &&L_ADD, &&L_SUB, &&L_MUL, &&L_DIV, &&L_MOD,
        &&L_EQ, &&L_NEQ, &&L_LT, &&L_GT, &&L_LTE, &&L_GTE,
        &&L_JUMP_IF_FALSE, &&L_JUMP, &&L_RETURN,
        &&L_CALL_METHOD, &&L_CALL_FUNC,
        &&L_GET_FIELD,   &&L_SET_FIELD,
        &&L_RETURN_VAL,  &&L_RETURN_NIL
    };

    #define NEXT() do { if (ip >= end) goto L_RETURN; \
                        goto *dispatch[(ip++)->op]; } while(0)
    #define i_a   ((ip-1)->a)
    #define i_b   ((ip-1)->b)
    #define i_c   ((ip-1)->c)
    #define i_off ((ip-1)->offset)

    NEXT();

    L_LOADK: { R[i_a] = c->constants[i_b]; NEXT(); }
    L_MOVE:  {
        /* Variable-register store (dst < 128 = param/local band; temps live
         * at >= 128). When a VAL_DYN is written over or written in, delegate
         * to store_cb so the runtime keeps the slot=>pin GC invariant, same
         * as rt_set does on the evaluator path. Two type compares for the
         * common int/float/str case — no callback, no extra cost. */
        if (store_cb && i_a < 128 &&
            (R[i_a].type == VAL_DYN || R[i_b].type == VAL_DYN))
            store_cb(rt_opaque, &R[i_a], &R[i_b]);
        R[i_a] = R[i_b]; NEXT();
    }

    L_ADD: {
        Value *l = &R[i_b], *r = &R[i_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[i_a].type = VAL_INT;
            R[i_a].as.integer = l->as.integer + r->as.integer;
        } else { R[i_a] = vm_arith(*l, *r, OP_ADD); }
        NEXT();
    }
    L_SUB: {
        Value *l = &R[i_b], *r = &R[i_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[i_a].type = VAL_INT;
            R[i_a].as.integer = l->as.integer - r->as.integer;
        } else { R[i_a] = vm_arith(*l, *r, OP_SUB); }
        NEXT();
    }
    L_MUL: { R[i_a] = vm_arith(R[i_b], R[i_c], OP_MUL); NEXT(); }
    L_DIV: { R[i_a] = vm_arith(R[i_b], R[i_c], OP_DIV); NEXT(); }
    L_MOD: { R[i_a] = vm_arith(R[i_b], R[i_c], OP_MOD); NEXT(); }

    L_LT: {
        Value *l = &R[i_b], *r = &R[i_c];
        if (l->type == VAL_INT && r->type == VAL_INT) {
            R[i_a].type = VAL_BOOL;
            R[i_a].as.boolean = l->as.integer < r->as.integer;
        } else { R[i_a] = vm_compare(*l, *r, OP_LT); }
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
        if      (cond->type == VAL_BOOL) truthy = cond->as.boolean;
        else if (cond->type == VAL_INT)  truthy = cond->as.integer != 0;
        else truthy = vm_truthy(*cond);
        if (!truthy) ip = c->code + i_off;
        NEXT();
    }
    L_JUMP: {
        if (cancel_flag && *cancel_flag) goto L_RETURN;
        if (tick_cb) tick_cb(rt_opaque);  /* GC sweep + mailbox at back-edge */
        ip = c->code + i_off; NEXT();
    }

    L_CALL_METHOD: {
        /* Pass &constants[i_b] — callback patches string→ptr inline cache */
        if (!call_cb) { R[i_a].type = VAL_NIL; NEXT(); }
        const char *method = c->constants[i_c].as.string;
        int first_arg = (i_off >> 8) & 0xFF;
        int argc      =  i_off       & 0xFF;
        R[i_a] = call_cb(rt_opaque, &c->constants[i_b], method, &R[first_arg], argc);
        NEXT();
    }

    L_CALL_FUNC: {
        /* OP_CALL_FUNC: owner_kv=NULL signals plain function */
        if (!call_cb) { R[i_a].type = VAL_NIL; NEXT(); }
        const char *fn_name = c->constants[i_b].as.string;
        R[i_a] = call_cb(rt_opaque, NULL, fn_name, &R[i_c], i_off);
        NEXT();
    }

    L_GET_FIELD: {
        /* Pass &constants[b] so callback can patch string→ptr (inline cache) */
        if (get_field_cb) {
            R[i_a] = get_field_cb(rt_opaque,
                                  &c->constants[i_b],
                                  c->constants[i_c].as.string);
        } else { R[i_a].type = VAL_NIL; }
        NEXT();
    }
    L_SET_FIELD: {
        if (set_field_cb) {
            set_field_cb(rt_opaque,
                         &c->constants[i_b],
                         c->constants[i_c].as.string,
                         R[i_a]);
        }
        NEXT();
    }
    L_RETURN_VAL: return 1;   /* handled by vm_run_fn; in vm_run = stop */
    L_RETURN_NIL: return 1;   /* same */
    L_RETURN: return 1;

    #pragma GCC diagnostic pop
    #undef NEXT
    #undef i_a
    #undef i_b
    #undef i_c
    #undef i_off

#else
    /* Fallback for non-GCC compilers */
    while (ip < end) {
        Instruction *instr = ip++;
        switch (instr->op) {
            case OP_LOADK: R[instr->a] = c->constants[instr->b]; break;
            case OP_MOVE:
                /* Same slot=>pin GC invariant as the computed-goto L_MOVE. */
                if (store_cb && instr->a < 128 &&
                    (R[instr->a].type == VAL_DYN || R[instr->b].type == VAL_DYN))
                    store_cb(rt_opaque, &R[instr->a], &R[instr->b]);
                R[instr->a] = R[instr->b]; break;
            case OP_ADD: {
                Value *l = &R[instr->b], *r = &R[instr->c];
                if (l->type==VAL_INT && r->type==VAL_INT) {
                    R[instr->a].type=VAL_INT;
                    R[instr->a].as.integer=l->as.integer+r->as.integer;
                } else R[instr->a] = vm_arith(*l, *r, OP_ADD);
                break;
            }
            case OP_SUB:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_SUB); break;
            case OP_MUL:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_MUL); break;
            case OP_DIV:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_DIV); break;
            case OP_MOD:   R[instr->a] = vm_arith(R[instr->b], R[instr->c], OP_MOD); break;
            case OP_EQ:    R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_EQ); break;
            case OP_NEQ:   R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_NEQ); break;
            case OP_LT: {
                Value *l=&R[instr->b], *r=&R[instr->c];
                if (l->type==VAL_INT && r->type==VAL_INT) {
                    R[instr->a].type=VAL_BOOL;
                    R[instr->a].as.boolean=l->as.integer < r->as.integer;
                } else R[instr->a] = vm_compare(*l, *r, OP_LT);
                break;
            }
            case OP_GT:    R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_GT); break;
            case OP_LTE:   R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_LTE); break;
            case OP_GTE:   R[instr->a] = vm_compare(R[instr->b], R[instr->c], OP_GTE); break;
            case OP_JUMP_IF_FALSE: {
                Value *cond = &R[instr->a];
                int truthy = 0;
                if      (cond->type==VAL_BOOL) truthy=cond->as.boolean;
                else if (cond->type==VAL_INT)  truthy=cond->as.integer!=0;
                else truthy = vm_truthy(*cond);
                if (!truthy) ip = c->code + instr->offset;
                break;
            }
            case OP_JUMP:
                if (cancel_flag && *cancel_flag) return 1;
                if (tick_cb) tick_cb(rt_opaque);
                ip = c->code + instr->offset; break;
            case OP_CALL_METHOD:
                if (call_cb) {
                    const char *method = c->constants[instr->c].as.string;
                    int first_arg = (instr->offset >> 8) & 0xFF;
                    int argc      =  instr->offset       & 0xFF;
                    R[instr->a] = call_cb(rt_opaque, &c->constants[instr->b],
                                          method, &R[first_arg], argc);
                } else { R[instr->a].type = VAL_NIL; }
                break;
            case OP_CALL_FUNC:
                if (call_cb) {
                    const char *fn = c->constants[instr->b].as.string;
                    R[instr->a] = call_cb(rt_opaque, NULL, fn,
                                          &R[instr->c], instr->offset);
                } else { R[instr->a].type = VAL_NIL; }
                break;
            case OP_GET_FIELD:
                if (get_field_cb)
                    R[instr->a] = get_field_cb(rt_opaque,
                                               &c->constants[instr->b],
                                               c->constants[instr->c].as.string);
                else R[instr->a].type = VAL_NIL;
                break;
            case OP_SET_FIELD:
                if (set_field_cb)
                    set_field_cb(rt_opaque,
                                 &c->constants[instr->b],
                                 c->constants[instr->c].as.string,
                                 R[instr->a]);
                break;
            case OP_RETURN_VAL: return 1;
            case OP_RETURN_NIL:  return 1;
            case OP_RETURN: return 1;
        }
    }
    return 1;
#endif
}
