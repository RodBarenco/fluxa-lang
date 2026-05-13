/* fuzz_snapshot.c — PrstPool / PrstGraph snapshot deserialization harness.
 *
 * The snapshot bytes are an attacker-controlled surface in three places:
 *
 *   1. Stage 3 (`fluxa update`): the new binary reads $FLUXA_RESTART_SNAPSHOT
 *      from disk. The path is set by the old binary, but the file's contents
 *      cross an execve boundary and the new binary's deserialization is the
 *      first thing it does on a fresh stack.
 *
 *   2. IPC_OP_UPDATE payload: the runtime accepts a path to a new binary;
 *      the snapshot it writes is what the new binary reads.
 *
 *   3. HANDOVER_MODE_FLASH on RP2040: the snapshot lives in a reserved Flash
 *      sector and is read after a reboot.
 *
 * The wire format (see prst_pool.h and prst_graph.h):
 *
 *   HandoverSnapshotHeader { magic, version, pool_checksum, graph_checksum,
 *                            pool_size, graph_size, pool_count, graph_count,
 *                            cycle_count_a, _pad[4] }
 *   PrstPool wire bytes ([int32 count] + per-entry: name[256] + dt + so +
 *                        value_block + init_value_block)
 *   PrstGraph wire bytes ([int32 count] + per-entry: name[256] + ctx[256])
 *
 * The harness dispatches off the first byte:
 *
 *   0  → fuzz prst_pool_deserialize directly (raw bytes minus selector)
 *   1  → fuzz prst_graph_deserialize directly
 *   2+ → fuzz the full snapshot path: header parse + both deser + both
 *        checksums (the checksum walks `while (*p)` over fixed-size name
 *        fields and OOB-reads if deserialize accepts unterminated names).
 *
 * The post-deser checksum and an explicit walk of each entry are what
 * surface the OOB-read bugs that pure pool/graph fuzzing would miss —
 * the deserializer can accept malformed names but the consumers crash.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/scope.h"
#include "../src/err.h"
#include "../src/prst_pool.h"
#include "../src/prst_graph.h"

/* Mirror of HandoverSnapshotHeader from src/handover.h. We don't include
 * handover.h directly because it pulls in runtime.h → fluxa_ffi.h →
 * toml_config.h, dragging in dependencies the deserializer itself does
 * not need. The on-wire layout is the only thing we care about. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t pool_checksum;
    uint32_t graph_checksum;
    uint32_t pool_size;
    uint32_t graph_size;
    int32_t  pool_count;
    int32_t  graph_count;
    int32_t  cycle_count_a;
    uint8_t  _pad[4];
} HandoverSnapshotHeader;

static void touch_pool(const PrstPool *p) {
    /* Walk every entry's strings the way prst_pool_checksum would —
     * forces ASan to see any OOB read past a malformed name field. */
    (void)prst_pool_checksum(p);
    for (int i = 0; i < p->count; i++) {
        /* Read every byte of the runtime value's string field. The
         * deserializer null-terminates malloc'd strings, so this should
         * always be safe; if it isn't, that's a finding. */
        const Value *v = &p->entries[i].value;
        if (v->type == VAL_STRING && v->as.string)
            (void)strlen(v->as.string);
        v = &p->entries[i].init_value;
        if (v->type == VAL_STRING && v->as.string)
            (void)strlen(v->as.string);
    }
}

static void touch_graph(const PrstGraph *g) {
    /* Same idea: checksum walks unterminated name/ctx fields if deser
     * accepted them. Also brute-force strlen each one. */
    (void)prst_graph_checksum(g);
    for (int i = 0; i < g->count; i++) {
        (void)strlen(g->deps[i].prst_name);
        (void)strlen(g->deps[i].reader_ctx);
    }
}

/* prst_pool_free now recursively reclaims value and init_value for
 * every entry (see src/prst_pool.h). The harness just calls it. */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Cap at a generous size — recursion depth in VAL_ARR can hit the
     * stack hard, and huge inputs don't improve coverage. */
    if (size < 1 || size > 65536) return 0;

    uint8_t mode = data[0] % 3;
    const uint8_t *body  = data + 1;
    size_t         blen  = size - 1;

    if (mode == 0) {
        PrstPool p = {0};
        if (prst_pool_deserialize(&p, body, blen)) {
            touch_pool(&p);
        }
        prst_pool_free(&p);
        return 0;
    }

    if (mode == 1) {
        PrstGraph g = {0};
        if (prst_graph_deserialize(&g, body, blen)) {
            touch_graph(&g);
        }
        prst_graph_free(&g);
        return 0;
    }

    /* Mode 2: full snapshot — synthesize a header and let the
     * deserialize helpers chew on the body. We hand-roll the header
     * parse here so we don't have to drag in handover.c (which pulls in
     * runtime + ipc + ffi + warm_profile). The validation we do is the
     * same as handover_deserialize_state's. */
    if (blen < sizeof(HandoverSnapshotHeader)) return 0;
    HandoverSnapshotHeader hdr;
    memcpy(&hdr, body, sizeof(hdr));

    /* Accept any magic/version — we want to exercise the deser code even
     * with attacker-controlled headers. The real path bails on bad magic;
     * we let it through to fuzz what comes after. */
    const uint8_t *rest = body + sizeof(HandoverSnapshotHeader);
    size_t         rlen = blen   - sizeof(HandoverSnapshotHeader);

    /* Clamp pool_size / graph_size to what we actually have. The
     * deserializer trusts these sizes; that's part of what we want to
     * see ASan/UBSan complain about. */
    uint32_t ps = hdr.pool_size;
    uint32_t gs = hdr.graph_size;
    if (ps > rlen) ps = (uint32_t)rlen;
    size_t remaining = rlen - ps;
    if (gs > remaining) gs = (uint32_t)remaining;

    PrstPool  p = {0};
    PrstGraph g = {0};

    if (ps > 0 && prst_pool_deserialize(&p, rest, ps)) {
        touch_pool(&p);
    }
    if (gs > 0 && prst_graph_deserialize(&g, rest + ps, gs)) {
        touch_graph(&g);
    }

    prst_pool_free(&p);
    prst_graph_free(&g);
    return 0;
}
