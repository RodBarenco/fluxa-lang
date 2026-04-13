/* dis_main.c — fluxa dis: Fluxa Program Disassembler
 *
 * Standalone binary. Reads a .flx file, runs Lexer -> Parser -> Resolver
 * (no execution), writes a .dis report file with 5 sections:
 *
 *   1. AST structure     nodes, types, lines, warm_local, offsets
 *   2. Warm forecast     static prediction of tier promotion per function
 *   3. prst fork         persistent vars, dependency graph, what dies together
 *   4. Execution paths   tier forecast, bytes/read, VM eligibility, TCO
 *   5. Statistics        node counts, warm budget, prst count
 *
 * Usage:
 *   fluxa_dis <file.flx>
 *   fluxa_dis <file.flx> -o output.txt
 *   fluxa_dis <file.flx> -proj <dir>
 *
 * Sprint 12.a
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "pool.h"
#include "lexer.h"
#include "parser.h"
#include "resolver.h"
#include "warm_profile.h"
#include "bytecode.h"

#define DIS_VERSION "0.10"
#define DIS_SPRINT  "Sprint 11"

/* ── Output ────────────────────────────────────────────────────────────── */
static FILE *OUT = NULL;

static void dis(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(OUT, fmt, ap);
    va_end(ap);
}

static void dis_rule(int w) {
    for (int i = 0; i < w; i++) fputc('-', OUT);
    fputc('\n', OUT);
}

/* ── Node type name ────────────────────────────────────────────────────── */
static const char *nstr(NodeType t) {
    switch (t) {
        case NODE_PROGRAM:       return "PROGRAM";
        case NODE_FUNC_CALL:     return "CALL";
        case NODE_INT_LIT:       return "INT_LIT";
        case NODE_FLOAT_LIT:     return "FLOAT_LIT";
        case NODE_BOOL_LIT:      return "BOOL_LIT";
        case NODE_STRING_LIT:    return "STRING_LIT";
        case NODE_IDENTIFIER:    return "IDENT";
        case NODE_VAR_DECL:      return "VAR_DECL";
        case NODE_ASSIGN:        return "ASSIGN";
        case NODE_BINARY_EXPR:   return "BINARY";
        case NODE_IF:            return "IF";
        case NODE_WHILE:         return "WHILE";
        case NODE_FOR:           return "FOR";
        case NODE_BLOCK_STMT:    return "BLOCK_STMT";
        case NODE_RETURN:        return "RETURN";
        case NODE_ARR_DECL:      return "ARR_DECL";
        case NODE_ARR_ACCESS:    return "ARR_ACCESS";
        case NODE_ARR_ASSIGN:    return "ARR_ASSIGN";
        case NODE_FUNC_DECL:     return "FN_DECL";
        case NODE_BLOCK_DECL:    return "BLOCK_DECL";
        case NODE_TYPEOF_INST:   return "TYPEOF";
        case NODE_MEMBER_ACCESS: return "MEMBER";
        case NODE_MEMBER_CALL:   return "MEMBER_CALL";
        case NODE_MEMBER_ASSIGN: return "MEMBER_ASSIGN";
        case NODE_FREE:          return "FREE";
        case NODE_DANGER:        return "DANGER";
        case NODE_IMPORT_C:      return "IMPORT_C";
        case NODE_IMPORT_STD:    return "IMPORT_STD";
        default:                 return "NODE";
    }
}

/* ── Stats ─────────────────────────────────────────────────────────────── */
typedef struct {
    int total_nodes, fn_count, promotable, block_count, prst_count;
    int has_vm, has_tco;
} Stats;

/* ── AST print ─────────────────────────────────────────────────────────── */
static void pnode(ASTNode *n, int d, Stats *st);

static void ind(int d) { for (int i=0;i<d;i++) dis("  "); }

static void pnode(ASTNode *n, int d, Stats *st) {
    if (!n) return;
    st->total_nodes++;

    char warm[20]="", off[20]="";
    if (n->warm_local)           snprintf(warm,sizeof(warm),"  warm=1");
    if (n->resolved_offset >= 0) snprintf(off, sizeof(off), "  off=%d",n->resolved_offset);

    switch (n->type) {

    case NODE_FUNC_DECL: {
        st->fn_count++;
        ind(d);
        dis("fn %s(", n->as.func_decl.name);
        for (int i=0;i<n->as.func_decl.param_count;i++) {
            if (i) dis(", ");
            if (n->as.func_decl.param_types)
                dis("%s ", n->as.func_decl.param_types[i]);
            dis("%s", n->as.func_decl.param_names[i]);
        }
        dis(") %s   line %d\n",
            n->as.func_decl.return_type ? n->as.func_decl.return_type : "nil",
            n->line);
        for (int i=0;i<n->as.func_decl.param_count;i++) {
            ind(d+1);
            dis("PARAM   %-10s  type=%-6s  off=%-3d  warm=1\n",
                n->as.func_decl.param_names[i],
                n->as.func_decl.param_types ? n->as.func_decl.param_types[i] : "?",
                i);
        }
        if (n->as.func_decl.body)
            pnode(n->as.func_decl.body, d+1, st);
        break;
    }

    case NODE_BLOCK_DECL: {
        st->block_count++;
        ind(d);
        dis("Block %s   line %d\n", n->as.block_decl.name, n->line);
        for (int i=0;i<n->as.block_decl.count;i++)
            pnode(n->as.block_decl.members[i], d+1, st);
        break;
    }

    case NODE_VAR_DECL: {
        if (n->as.var_decl.persistent) st->prst_count++;
        ind(d);
        dis("%-9s  %-12s  type=%-6s  line=%-4d%s%s\n",
            n->as.var_decl.persistent ? "PRST_DECL" : "VAR_DECL",
            n->as.var_decl.var_name,
            n->as.var_decl.type_name ? n->as.var_decl.type_name : "?",
            n->line, off, warm);
        if (n->as.var_decl.initializer)
            pnode(n->as.var_decl.initializer, d+1, st);
        break;
    }

    case NODE_IDENTIFIER:
        ind(d);
        dis("IDENT      %-14s  line=%-4d%s%s\n",
            n->as.str.value, n->line, off, warm);
        break;

    case NODE_BINARY_EXPR:
        ind(d);
        dis("BINARY  %-4s  line=%-4d\n", n->as.binary.op, n->line);
        pnode(n->as.binary.left,  d+1, st);
        pnode(n->as.binary.right, d+1, st);
        break;

    case NODE_IF:
        ind(d);
        dis("IF   line=%-4d\n", n->line);
        pnode(n->as.if_stmt.condition, d+1, st);
        pnode(n->as.if_stmt.then_body,  d+1, st);
        if (n->as.if_stmt.else_body)
            pnode(n->as.if_stmt.else_body, d+1, st);
        break;

    case NODE_WHILE:
        st->has_vm = 1;
        ind(d);
        dis("WHILE [-> bytecode VM]   line=%-4d\n", n->line);
        pnode(n->as.while_stmt.condition, d+1, st);
        pnode(n->as.while_stmt.body,      d+1, st);
        break;

    case NODE_RETURN:
        ind(d);
        dis("RETURN   line=%-4d\n", n->line);
        if (n->as.ret.value) pnode(n->as.ret.value, d+1, st);
        break;

    case NODE_FUNC_CALL:
        ind(d);
        dis("CALL    %-14s  line=%-4d\n",
            n->as.list.name ? n->as.list.name : "?", n->line);
        for (int i=0;i<n->as.list.count;i++)
            pnode(n->as.list.children[i], d+1, st);
        break;

    case NODE_INT_LIT:
        ind(d);
        dis("INT_LIT   %-12ld  line=%-4d\n", n->as.integer.value, n->line);
        break;

    case NODE_FLOAT_LIT:
        ind(d);
        dis("FLOAT_LIT %-12g  line=%-4d\n", n->as.real.value, n->line);
        break;

    case NODE_BOOL_LIT:
        ind(d);
        dis("BOOL_LIT  %-12s  line=%-4d\n",
            n->as.boolean.value ? "true" : "false", n->line);
        break;

    case NODE_STRING_LIT:
        ind(d);
        dis("STRING_LIT \"%s\"  line=%-4d\n", n->as.str.value, n->line);
        break;

    case NODE_ASSIGN:
        ind(d);
        dis("ASSIGN  %-14s  line=%-4d%s\n",
            n->as.assign.var_name, n->line, off);
        if (n->as.assign.value) pnode(n->as.assign.value, d+1, st);
        break;

    case NODE_MEMBER_CALL:
        ind(d);
        dis("MEMBER_CALL  %s.%s()  line=%-4d\n",
            n->as.member_call.owner, n->as.member_call.method, n->line);
        for (int i=0;i<n->as.member_call.arg_count;i++)
            pnode(n->as.member_call.args[i], d+1, st);
        break;

    case NODE_MEMBER_ACCESS:
        ind(d);
        dis("MEMBER  %s.%s  line=%-4d\n",
            n->as.member_access.owner, n->as.member_access.field, n->line);
        break;

    case NODE_MEMBER_ASSIGN:
        ind(d);
        dis("MEMBER_ASSIGN  %s.%s  line=%-4d\n",
            n->as.member_assign.owner, n->as.member_assign.field, n->line);
        if (n->as.member_assign.value) pnode(n->as.member_assign.value, d+1, st);
        break;

    case NODE_TYPEOF_INST:
        ind(d);
        dis("TYPEOF  %s typeof %s  line=%-4d\n",
            n->as.typeof_inst.inst_name, n->as.typeof_inst.origin_name, n->line);
        break;

    case NODE_DANGER:
        ind(d);
        dis("DANGER   line=%-4d\n", n->line);
        if (n->as.danger_stmt.body) pnode(n->as.danger_stmt.body, d+1, st);
        break;

    case NODE_PROGRAM:
    case NODE_BLOCK_STMT:
        for (int i=0;i<n->as.list.count;i++)
            pnode(n->as.list.children[i], d, st);
        break;

    default:
        ind(d);
        dis("%-12s  line=%-4d%s%s\n", nstr(n->type), n->line, off, warm);
        break;
    }
}

/* ── Function analysis ─────────────────────────────────────────────────── */
typedef struct {
    int has_dyn, has_prst_read, has_block_method, is_recursive, has_tco;
    const char *fn_name;
} FA;

static void analyze(ASTNode *n, FA *a) {
    if (!n) return;
    switch (n->type) {
    case NODE_VAR_DECL:
        if (n->as.var_decl.persistent) a->has_prst_read = 1;
        if (n->as.var_decl.type_name &&
            strcmp(n->as.var_decl.type_name,"dyn")==0) a->has_dyn = 1;
        analyze(n->as.var_decl.initializer, a);
        break;
    case NODE_IDENTIFIER:
        if (!n->warm_local && n->resolved_offset >= 0) a->has_prst_read = 1;
        break;
    case NODE_FUNC_CALL:
        if (n->as.list.name && a->fn_name &&
            strcmp(n->as.list.name, a->fn_name)==0) a->is_recursive = 1;
        for (int i=0;i<n->as.list.count;i++) analyze(n->as.list.children[i],a);
        break;
    case NODE_MEMBER_CALL:
    case NODE_MEMBER_ACCESS:
        a->has_block_method = 1;
        break;
    case NODE_RETURN:
        if (n->as.ret.value && n->as.ret.value->type==NODE_FUNC_CALL)
            a->has_tco = 1;
        analyze(n->as.ret.value, a);
        break;
    case NODE_BINARY_EXPR:
        analyze(n->as.binary.left, a);
        analyze(n->as.binary.right, a);
        break;
    case NODE_IF:
        analyze(n->as.if_stmt.condition, a);
        analyze(n->as.if_stmt.then_body, a);
        analyze(n->as.if_stmt.else_body, a);
        break;
    case NODE_WHILE:
        analyze(n->as.while_stmt.condition, a);
        analyze(n->as.while_stmt.body, a);
        break;
    case NODE_BLOCK_STMT: case NODE_PROGRAM:
        for (int i=0;i<n->as.list.count;i++) analyze(n->as.list.children[i],a);
        break;
    default: break;
    }
}

static int count_locals(ASTNode *body) {
    if (!body) return 0;
    int n=0;
    for (int i=0;i<body->as.list.count;i++) {
        ASTNode *s=body->as.list.children[i];
        if (s && s->type==NODE_VAR_DECL && !s->as.var_decl.persistent) n++;
    }
    return n;
}

static int body_has_while(ASTNode *body) {
    if (!body) return 0;
    for (int i=0;i<body->as.list.count;i++) {
        ASTNode *s=body->as.list.children[i];
        if (s && s->type==NODE_WHILE) return 1;
    }
    return 0;
}

/* ── Bytecode disassembler ─────────────────────────────────────────────── */
static const char *op_name(Opcode op) {
    switch (op) {
        case OP_LOADK:         return "LOADK";
        case OP_MOVE:          return "MOVE";
        case OP_ADD:           return "ADD";
        case OP_SUB:           return "SUB";
        case OP_MUL:           return "MUL";
        case OP_DIV:           return "DIV";
        case OP_MOD:           return "MOD";
        case OP_EQ:            return "EQ";
        case OP_NEQ:           return "NEQ";
        case OP_LT:            return "LT";
        case OP_GT:            return "GT";
        case OP_LTE:           return "LTE";
        case OP_GTE:           return "GTE";
        case OP_JUMP_IF_FALSE: return "JUMP_IF_FALSE";
        case OP_JUMP:          return "JUMP";
        case OP_RETURN:        return "RETURN";
        default:               return "OP?";
    }
}

static void dis_chunk(Chunk *ch, int indent_depth) {
    char pad[64]; int p = indent_depth * 2;
    if (p >= 63) p = 62;
    memset(pad, ' ', p); pad[p] = '\0';

    for (int i = 0; i < ch->count; i++) {
        Instruction in = ch->code[i];
        dis("%s  %04d  %-16s", pad, i, op_name(in.op));
        switch (in.op) {
            case OP_LOADK: {
                /* b = const index */
                dis("  r%-3d  K%-3d  ", in.a, in.b);
                if (in.b < ch->const_count) {
                    Value v = ch->constants[in.b];
                    if      (v.type == VAL_INT)   dis("(%ld)", v.as.integer);
                    else if (v.type == VAL_FLOAT)  dis("(%g)",  v.as.real);
                    else if (v.type == VAL_BOOL)   dis("(%s)",  v.as.boolean?"true":"false");
                    else if (v.type == VAL_STRING) dis("(\"%s\")", v.as.string?v.as.string:"");
                }
                break;
            }
            case OP_MOVE:
                dis("  r%-3d  r%-3d", in.a, in.b);
                break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_EQ:  case OP_NEQ: case OP_LT:  case OP_GT:
            case OP_LTE: case OP_GTE:
                dis("  r%-3d  r%-3d  r%-3d", in.a, in.b, in.c);
                break;
            case OP_JUMP_IF_FALSE:
                dis("  r%-3d  -> %04d", in.b, in.offset);
                break;
            case OP_JUMP:
                dis("  -> %04d", in.offset);
                break;
            case OP_RETURN:
                dis("  r%-3d", in.a);
                break;
            default:
                dis("  a=%-3d b=%-3d c=%-3d", in.a, in.b, in.c);
                break;
        }
        dis("\n");
    }
    if (ch->const_count > 0) {
        dis("%s  constants: %d\n", pad, ch->const_count);
        for (int i = 0; i < ch->const_count; i++) {
            Value v = ch->constants[i];
            dis("%s    K%-3d  ", pad, i);
            if      (v.type == VAL_INT)    dis("int    %ld\n",   v.as.integer);
            else if (v.type == VAL_FLOAT)  dis("float  %g\n",    v.as.real);
            else if (v.type == VAL_BOOL)   dis("bool   %s\n",    v.as.boolean?"true":"false");
            else if (v.type == VAL_STRING) dis("string \"%s\"\n", v.as.string?v.as.string:"");
            else                           dis("?\n");
        }
    }
}



/* ── Call order / call graph ───────────────────────────────────────────── */
#define MAX_FNS 64
#define MAX_EDGES 256

typedef struct { char name[64]; int line; int is_tco; } CallEdge;
typedef struct {
    char      name[64];
    int       line;
    CallEdge  callees[MAX_EDGES];
    int       callee_count;
    int       is_recursive;   /* calls itself */
    int       tco_self;       /* tail-recursive */
} FnNode;

static FnNode g_fns[MAX_FNS];
static int    g_fn_count = 0;

static int find_fn(const char *name) {
    for (int i = 0; i < g_fn_count; i++)
        if (strcmp(g_fns[i].name, name) == 0) return i;
    return -1;
}

static void collect_calls(ASTNode *n, FnNode *fn) {
    if (!n) return;
    switch (n->type) {
    case NODE_FUNC_CALL:
        if (n->as.list.name && fn->callee_count < MAX_EDGES) {
            /* check if this is a tail call: handled by caller */
            CallEdge e; memset(&e, 0, sizeof(e));
            strncpy(e.name, n->as.list.name, 63);
            e.is_tco = 0;  /* refined by caller */
            fn->callees[fn->callee_count++] = e;
            if (strcmp(n->as.list.name, fn->name) == 0)
                fn->is_recursive = 1;
        }
        for (int i = 0; i < n->as.list.count; i++)
            collect_calls(n->as.list.children[i], fn);
        break;
    case NODE_RETURN:
        /* tag callee as TCO if it's a direct call in return position */
        if (n->as.ret.value && n->as.ret.value->type == NODE_FUNC_CALL) {
            const char *callee = n->as.ret.value->as.list.name;
            if (callee) {
                /* mark the last matching callee edge as TCO */
                for (int i = fn->callee_count - 1; i >= 0; i--) {
                    if (strcmp(fn->callees[i].name, callee) == 0) {
                        fn->callees[i].is_tco = 1;
                        if (strcmp(callee, fn->name) == 0) fn->tco_self = 1;
                        break;
                    }
                }
            }
        }
        collect_calls(n->as.ret.value, fn);
        break;
    case NODE_BINARY_EXPR:
        collect_calls(n->as.binary.left, fn);
        collect_calls(n->as.binary.right, fn);
        break;
    case NODE_IF:
        collect_calls(n->as.if_stmt.condition, fn);
        collect_calls(n->as.if_stmt.then_body, fn);
        collect_calls(n->as.if_stmt.else_body, fn);
        break;
    case NODE_WHILE:
        collect_calls(n->as.while_stmt.condition, fn);
        collect_calls(n->as.while_stmt.body, fn);
        break;
    case NODE_VAR_DECL:
        collect_calls(n->as.var_decl.initializer, fn);
        break;
    case NODE_ASSIGN:
        collect_calls(n->as.assign.value, fn);
        break;
    case NODE_BLOCK_STMT: case NODE_PROGRAM:
        for (int i = 0; i < n->as.list.count; i++)
            collect_calls(n->as.list.children[i], fn);
        break;
    default: break;
    }
}

/* Simple cycle detection: DFS returning 1 if a->b->...->a */
static int visited[MAX_FNS];
static int in_stack[MAX_FNS];

static int has_cycle_from(int idx) {
    visited[idx] = 1;
    in_stack[idx] = 1;
    FnNode *fn = &g_fns[idx];
    for (int e = 0; e < fn->callee_count; e++) {
        int nxt = find_fn(fn->callees[e].name);
        if (nxt < 0) continue;
        if (!visited[nxt] && has_cycle_from(nxt)) return 1;
        if (in_stack[nxt]) return 1;
    }
    in_stack[idx] = 0;
    return 0;
}

static void sec_callorder(ASTNode *prog) {
    dis("-- 3. Call Order ------------------------------------------------\n\n");

    /* Build fn nodes */
    g_fn_count = 0;
    for (int i = 0; i < prog->as.list.count; i++) {
        ASTNode *n = prog->as.list.children[i];
        if (!n || n->type != NODE_FUNC_DECL) continue;
        if (g_fn_count >= MAX_FNS) break;
        FnNode *fn = &g_fns[g_fn_count++];
        memset(fn, 0, sizeof(*fn));
        strncpy(fn->name, n->as.func_decl.name, 63);
        fn->line = n->line;
        if (n->as.func_decl.body)
            collect_calls(n->as.func_decl.body, fn);
    }

    if (g_fn_count == 0) {
        dis("  (no functions declared)\n\n");
        return;
    }

    /* Detect cycles (mutual recursion) */
    int has_any_cycle = 0;
    memset(visited, 0, sizeof(visited));
    memset(in_stack, 0, sizeof(in_stack));
    for (int i = 0; i < g_fn_count; i++)
        if (!visited[i] && has_cycle_from(i)) { has_any_cycle = 1; break; }

    /* Print call graph */
    dis("  Call graph:\n\n");
    for (int i = 0; i < g_fn_count; i++) {
        FnNode *fn = &g_fns[i];
        dis("    fn %s  (line %d)\n", fn->name, fn->line);
        if (fn->callee_count == 0) {
            dis("      calls: (none)\n");
        } else {
            /* deduplicate callees */
            for (int e = 0; e < fn->callee_count; e++) {
                int dup = 0;
                for (int d = 0; d < e; d++)
                    if (strcmp(fn->callees[d].name, fn->callees[e].name)==0)
                        { dup = 1; break; }
                if (dup) continue;
                int is_tco = 0;
                int tco_count = 0;
                int call_count = 0;
                for (int d = e; d < fn->callee_count; d++) {
                    if (strcmp(fn->callees[d].name, fn->callees[e].name)==0) {
                        call_count++;
                        if (fn->callees[d].is_tco) { is_tco = 1; tco_count++; }
                    }
                }
                int is_self = strcmp(fn->callees[e].name, fn->name)==0;
                int callee_exists = find_fn(fn->callees[e].name) >= 0;
                dis("      -> %s", fn->callees[e].name);
                if (call_count > 1) dis("  (x%d)", call_count);
                if (is_tco)     dis("  [TCO]");
                if (is_self)    dis("  [recursive]");
                if (!callee_exists) dis("  [external/builtin]");
                dis("\n");
            }
        }
        /* Classify */
        if (fn->tco_self)
            dis("      type: tail-recursive (TCO — O(1) stack)\n");
        else if (fn->is_recursive)
            dis("      type: recursive (non-tail — bounded by FLUXA_MAX_DEPTH=500)\n");
        else
            dis("      type: plain function\n");
        dis("\n");
    }

    /* Mutual recursion / cycle report */
    if (has_any_cycle) {
        dis("  Mutual recursion detected:\n");
        for (int i = 0; i < g_fn_count; i++) {
            FnNode *fn = &g_fns[i];
            for (int e = 0; e < fn->callee_count; e++) {
                int j = find_fn(fn->callees[e].name);
                if (j < 0 || j == i) continue;
                /* check if j calls back to i — only print once (i < j) */
                if (j <= i) continue;
                FnNode *fj = &g_fns[j];
                for (int f = 0; f < fj->callee_count; f++) {
                    if (strcmp(fj->callees[f].name, fn->name)==0) {
                        int tco_ab = fn->callees[e].is_tco;
                        int tco_ba = fj->callees[f].is_tco;
                        dis("    %s <-> %s", fn->name, fj->name);
                        if (tco_ab && tco_ba) dis("  [mutual TCO — O(1) stack]");
                        else                  dis("  [mutual recursion — uses call stack]");
                        dis("\n");
                    }
                }
            }
        }
        dis("\n");
    }

    /* Topological call order (only when no cycles) */
    if (!has_any_cycle && g_fn_count > 1) {
        dis("  Call order (topological):\n");
        /* Simple: functions that call nobody first, then who calls them */
        int printed[MAX_FNS] = {0};
        int order = 1;
        /* Leaves first */
        for (int i = 0; i < g_fn_count; i++) {
            int has_fn_call = 0;
            for (int e = 0; e < g_fns[i].callee_count; e++)
                if (find_fn(g_fns[i].callees[e].name) >= 0)
                    { has_fn_call = 1; break; }
            if (!has_fn_call) {
                dis("    %d. %s  (leaf — calls no other Fluxa fn)\n",
                    order++, g_fns[i].name);
                printed[i] = 1;
            }
        }
        /* Then callers */
        for (int i = 0; i < g_fn_count; i++) {
            if (!printed[i]) {
                dis("    %d. %s  (calls: ", order++, g_fns[i].name);
                int first = 1;
                for (int e = 0; e < g_fns[i].callee_count; e++) {
                    if (find_fn(g_fns[i].callees[e].name) < 0) continue;
                    int dup = 0;
                    for (int d = 0; d < e; d++)
                        if (strcmp(g_fns[i].callees[d].name,
                                   g_fns[i].callees[e].name)==0) { dup=1; break; }
                    if (dup) continue;
                    if (!first) dis(", ");
                    dis("%s", g_fns[i].callees[e].name);
                    first = 0;
                }
                dis(")\n");
            }
        }
        dis("\n");
    }
}

/* ── Section 2: Warm Forecast ──────────────────────────────────────────── */
static void sec_warm(ASTNode *prog, Stats *st, int prst_n) {
    dis("\n-- 2. Warm Path Forecast ----------------------------------------\n\n");
    int fi=0;
    for (int i=0;i<prog->as.list.count;i++) {
        ASTNode *n=prog->as.list.children[i];
        if (!n || n->type!=NODE_FUNC_DECL) continue;
        fi++;

        FA a={0}; a.fn_name=n->as.func_decl.name;
        if (n->as.func_decl.body) analyze(n->as.func_decl.body,&a);

        int params=n->as.func_decl.param_count;
        int locals=count_locals(n->as.func_decl.body);
        int slots =params+locals;

        const char *forecast, *reason="";
        if (fi > WARM_FUNC_CAP) {
            forecast="COLD-FALLBACK";
            reason=" (> WARM_FUNC_CAP=32: no slot; warm_local direct read)";
        } else if (slots > WARM_SLOTS_MAX) {
            forecast="COLD-LOCKED";
            reason=" (> 256 slots: wrap may cause QJL collisions)";
        } else if (a.has_dyn) {
            forecast="COLD-LOCKED";
            reason=" (dyn local: type not stable)";
        } else if (a.has_prst_read) {
            forecast="COLD-LOCKED";
            reason=" (reads prst: must pass through prst_pool_has)";
        } else {
            forecast="PROMOTABLE";
            st->promotable++;
        }
        if (a.has_tco) st->has_tco=1;

        int cold_B=18+20*prst_n;

        dis("  fn %s%s\n", n->as.func_decl.name, reason);
        dis("    params:    %d", params);
        for (int p=0;p<params;p++) dis("  %s", n->as.func_decl.param_names[p]);
        dis("\n");
        dis("    locals:    %d\n", locals);
        dis("    slots:     %d\n", slots);
        dis("    forecast:  %s\n", forecast);

        if (strcmp(forecast,"PROMOTABLE")==0) {
            dis("    tier:      1 (warm) after %d stable calls\n", WARM_STABLE_RUNS);
            dis("    cold read: %dB  (%d prst vars x ~20B scan)\n", cold_B, prst_n);
            dis("    warm read: 9B   (WarmSlot 1B + stack[off] 8B)\n");
            dis("    savings:   %dB per local var read after promotion\n",
                cold_B - 9);
        } else {
            dis("    tier:      0 (cold) — warm_local still skips prst_pool_has\n");
            dis("    cold read: 18B  (direct stack read, no pool scan)\n");
        }
        if (a.is_recursive)
            dis("    recursive: yes — same fn_node* key across all frames\n");
        if (a.has_tco)
            dis("    TCO:       yes — tail call detected in return position\n");
        if (a.has_block_method)
            dis("    note:      calls Block method (callee uses scope, cold)\n");
        dis("\n");
    }
    if (fi==0) dis("  (no functions declared)\n\n");
}

/* ── Section 3: prst Fork ──────────────────────────────────────────────── */
static void sec_prst(ASTNode *prog, int is_proj) {
    dis("-- 5. prst Fork -------------------------------------------------\n\n");
    if (!is_proj) {
        dis("  mode:      SCRIPT  (no prst declarations)\n");
        dis("  prst vars: 0\n");
        dis("  note:      PrstPool not instantiated — zero overhead\n\n");
        return;
    }
    dis("  mode:      PROJECT\n\n");

    typedef struct { const char *name,*type,*owner; int line; } PV;
    PV vars[256]; int nv=0;

    for (int i=0;i<prog->as.list.count&&nv<256;i++) {
        ASTNode *n=prog->as.list.children[i];
        if (!n) continue;
        if (n->type==NODE_VAR_DECL && n->as.var_decl.persistent)
            vars[nv++]=(PV){n->as.var_decl.var_name,
                n->as.var_decl.type_name?n->as.var_decl.type_name:"?",
                "[top-level]", n->line};
        if (n->type==NODE_BLOCK_DECL)
            for (int m=0;m<n->as.block_decl.count&&nv<256;m++) {
                ASTNode *mb=n->as.block_decl.members[m];
                if (mb&&mb->type==NODE_VAR_DECL&&mb->as.var_decl.persistent)
                    vars[nv++]=(PV){mb->as.var_decl.var_name,
                        mb->as.var_decl.type_name?mb->as.var_decl.type_name:"?",
                        n->as.block_decl.name, mb->line};
            }
    }

    if (nv==0) { dis("  prst vars: 0\n\n"); return; }

    dis("  prst vars: %d\n\n", nv);
    for (int v=0;v<nv;v++)
        dis("  prst %-6s  %-16s  owner=%-16s  line=%d\n",
            vars[v].type, vars[v].name, vars[v].owner, vars[v].line);

    dis("\n  Fork — removing any prst var atomically invalidates:\n\n");
    for (int v=0;v<nv;v++) {
        dis("    remove %s:\n", vars[v].name);
        dis("      -> %s dies immediately\n", vars[v].name);
        dis("      -> every fn that read %s is invalidated in PrstGraph\n",vars[v].name);
        dis("      -> prst vars that depended on %s die recursively\n",vars[v].name);
        dis("      -> interruption is total — no grace period, no tombstone\n");
        if (v<nv-1) dis("\n");
    }
    dis("\n  note: static analysis — use `fluxa explain` for live PrstGraph.\n\n");
}

/* ── Section 4: Execution Paths ────────────────────────────────────────── */
static void sec_paths(ASTNode *prog, int prst_n, Stats *st) {
    dis("-- 6. Execution Path Summary ------------------------------------\n\n");
    int fi=0;
    for (int i=0;i<prog->as.list.count;i++) {
        ASTNode *n=prog->as.list.children[i];
        if (!n||n->type!=NODE_FUNC_DECL) continue;
        fi++;

        FA a={0}; a.fn_name=n->as.func_decl.name;
        if (n->as.func_decl.body) analyze(n->as.func_decl.body,&a);

        int params=n->as.func_decl.param_count;
        int slots =params+count_locals(n->as.func_decl.body);
        int warm_ok=(fi<=WARM_FUNC_CAP&&!a.has_dyn&&!a.has_prst_read&&
                     slots<=WARM_SLOTS_MAX);
        int vm_ok=body_has_while(n->as.func_decl.body);
        if (vm_ok) st->has_vm=1;

        dis("  fn %s\n", n->as.func_decl.name);
        dis("    tier 0 (cold):   calls 1-%d    %dB/read  (%d prst vars)\n",
            WARM_OBS_LIMIT, 18+20*prst_n, prst_n);
        if (warm_ok)
            dis("    tier 1 (warm):   calls %d+      9B/read   after promotion\n",
                WARM_OBS_LIMIT+1);
        else
            dis("    tier 1 (warm):   cold-locked  18B/read  (warm_local direct)\n");
        dis("    tier 2 (hot VM): %s\n",
            vm_ok
            ? "eligible     (while compiled to 3-address bytecode)"
            : "not eligible (no while in fn body)");
        dis("    TCO:             %s\n",
            a.has_tco?"yes — tail call in return position":"no");
        dis("\n");
    }

    dis("  [top-level script body]\n");
    dis("    warm_local:  no   (in_func_depth=0, not inside fn scope)\n");
    dis("    execution:   cold path for all reads\n");
    dis("    note:        Fluxa has no global variables; the script body\n");
    dis("                 is the top-level execution context, not a fn scope\n\n");
}

/* ── Section 5: Statistics ─────────────────────────────────────────────── */
static void sec_stats(Stats *st, int lines) {
    dis("-- 7. Statistics ------------------------------------------------\n\n");
    dis("  source lines:    %d\n", lines);
    dis("  AST nodes:       %d\n", st->total_nodes);
    dis("  functions:       %d", st->fn_count);
    if (st->fn_count>0) {
        int ov=st->fn_count>WARM_FUNC_CAP ? st->fn_count-WARM_FUNC_CAP : 0;
        int cl=st->fn_count-st->promotable-ov;
        dis("   (%d promotable", st->promotable);
        if (cl>0) dis(", %d cold-locked", cl);
        if (ov>0) dis(", %d beyond cap -> fallback", ov);
        dis(")");
    }
    dis("\n");
    dis("  Blocks:          %d\n", st->block_count);
    dis("  prst vars:       %d\n", st->prst_count);
    int wfns=st->fn_count<WARM_FUNC_CAP?st->fn_count:WARM_FUNC_CAP;
    dis("  warm candidates: %d / %d\n", st->promotable, st->fn_count);
    dis("  WarmProfile:     %d fn x %dB = %dB  (max %dB)\n",
        wfns, (int)sizeof(WarmFunc),
        wfns*(int)sizeof(WarmFunc),
        WARM_FUNC_CAP*(int)sizeof(WarmFunc));
    dis("  bytecode VM:     %s\n", st->has_vm  ? "yes" : "no");
    dis("  TCO:             %s\n\n", st->has_tco ? "yes" : "no");
}

/* ── has_prst walk ─────────────────────────────────────────────────────── */
static int has_prst(ASTNode *n) {
    if (!n) return 0;
    if (n->type==NODE_VAR_DECL && n->as.var_decl.persistent) return 1;
    if (n->type==NODE_PROGRAM||n->type==NODE_BLOCK_STMT)
        for (int i=0;i<n->as.list.count;i++)
            if (has_prst(n->as.list.children[i])) return 1;
    if (n->type==NODE_BLOCK_DECL)
        for (int i=0;i<n->as.block_decl.count;i++)
            if (has_prst(n->as.block_decl.members[i])) return 1;
    return 0;
}

static int count_src_lines(const char *s) {
    int n=0; for (;*s;s++) if (*s=='\n') n++; return n+1;
}

/* ── main ──────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc<2) {
        fprintf(stderr,"usage: fluxa_dis <file.flx> [-o out.txt] [-proj <dir>]\n");
        return 1;
    }
    const char *inpath=argv[1], *outpath=NULL;
    for (int i=2;i<argc;i++)
        if (strcmp(argv[i],"-o")==0 && i+1<argc) outpath=argv[++i];

    char defout[512];
    if (!outpath) {
        strncpy(defout, inpath, sizeof(defout)-5);
        defout[sizeof(defout)-5]='\0';
        char *dot=strrchr(defout,'.');
        if (dot) strcpy(dot,".dis"); else strcat(defout,".dis");
        outpath=defout;
    }

    FILE *fp=fopen(inpath,"r");
    if (!fp) { fprintf(stderr,"fluxa_dis: cannot open '%s'\n",inpath); return 1; }
    fseek(fp,0,SEEK_END); long sz=ftell(fp); rewind(fp);
    char *src=malloc(sz+1);
    if (!src) { fclose(fp); return 1; }
    size_t rd=fread(src,1,sz,fp); src[rd]='\0'; fclose(fp);
    int lines=count_src_lines(src);

    ASTPool pool; pool_init(&pool);
    Parser  parser=parser_new(src, &pool);
    ASTNode *prog =parser_parse(&parser);
    if (parser.had_error||!prog) {
        fprintf(stderr,"fluxa_dis: parse error in '%s'\n",inpath);
        pool_free(&pool); free(src); return 1;
    }
    resolver_run(prog);

    int is_proj=has_prst(prog);
    int prst_n=0;
    for (int i=0;i<prog->as.list.count;i++) {
        ASTNode *n=prog->as.list.children[i];
        if (!n) continue;
        if (n->type==NODE_VAR_DECL&&n->as.var_decl.persistent) prst_n++;
        if (n->type==NODE_BLOCK_DECL)
            for (int m=0;m<n->as.block_decl.count;m++) {
                ASTNode *mb=n->as.block_decl.members[m];
                if (mb&&mb->type==NODE_VAR_DECL&&mb->as.var_decl.persistent)
                    prst_n++;
            }
    }

    OUT=fopen(outpath,"w");
    if (!OUT) {
        fprintf(stderr,"fluxa_dis: cannot write '%s'\n",outpath);
        pool_free(&pool); free(src); return 1;
    }

    dis_rule(68);
    dis("  fluxa dis -- %s\n", inpath);
    dis("  %s | v%s | mode: %s\n",
        DIS_SPRINT, DIS_VERSION, is_proj?"PROJECT":"SCRIPT");
    dis_rule(68);
    dis("\n-- 1. AST -------------------------------------------------------\n\n");

    Stats st={0};
    pnode(prog, 0, &st);
    dis("\n");

    sec_warm(prog, &st, prst_n);
    /* Hot path section — compile and disassemble VM bytecode */
    {
        int has_hot = 0;
        for (int i = 0; i < prog->as.list.count; i++) {
            ASTNode *n = prog->as.list.children[i];
            if (!n) continue;
            /* top-level while/if */
            if (n->type == NODE_WHILE || n->type == NODE_IF) {
                if (!has_hot) {
                    dis("-- 3. Hot Path — Bytecode VM -----------------------------------\n\n");
                    has_hot = 1;
                }
                Chunk ch; chunk_init(&ch);
                int ok = chunk_compile_loop(&ch, n);
                if (ok && ch.count > 0) {
                    dis("  %s @ line %d  (%d instructions)\n",
                        n->type==NODE_WHILE?"while":"if", n->line, ch.count);
                    dis_chunk(&ch, 2);
                    dis("\n");
                }
                chunk_free(&ch);
            }
            /* inside fn bodies */
            if (n->type == NODE_FUNC_DECL && n->as.func_decl.body) {
                ASTNode *body = n->as.func_decl.body;
                int fn_has = 0;
                for (int j = 0; j < body->as.list.count; j++) {
                    ASTNode *s = body->as.list.children[j];
                    if (s && (s->type==NODE_WHILE||s->type==NODE_IF)) {
                                Chunk ch; chunk_init(&ch);
                        int ok = chunk_compile_loop(&ch, s);
                        if (ok && ch.count > 0) {
                            if (!has_hot)
                                dis("-- 3. Hot Path — Bytecode VM -----------------------------------\n\n");
                            has_hot = 1;
                            if (!fn_has) {
                                dis("  fn %s:\n", n->as.func_decl.name);
                                fn_has = 1;
                            }
                            dis("    %s @ line %d  (%d instructions)\n",
                                s->type==NODE_WHILE?"while":"if", s->line, ch.count);
                            dis_chunk(&ch, 3);
                            dis("\n");
                        }
                        chunk_free(&ch);
                    }
                }
            }
        }
        if (!has_hot) {
            dis("-- 3. Hot Path — Bytecode VM -----------------------------------\n\n");
            dis("  (no while/if loops compiled to VM bytecode)\n\n");
        }
    }

    sec_callorder(prog);
    sec_prst(prog, is_proj);
    sec_paths(prog, prst_n, &st);
    sec_stats(&st, lines);

    dis_rule(68);
    fclose(OUT);
    pool_free(&pool);
    free(src);
    fprintf(stdout,"fluxa_dis: wrote %s\n", outpath);
    return 0;
}
