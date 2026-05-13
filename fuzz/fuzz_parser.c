/* fuzz_parser.c — Fluxa parser harness.
 *
 * Runs the full lexer + recursive-descent parser. The ASTPool is a
 * stack-sized arena (4096 nodes, 64 KB strings); on overflow it falls
 * back to calloc'd nodes and strdup'd strings. pool_free() does not track
 * those overflow allocations — that is a leak the fuzzer will report
 * verbatim under ASan. Fixing the leak is part of the work.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../src/lexer.h"
#include "../src/parser.h"
#include "../src/pool.h"
#include "../src/ast.h"
#include "fuzz_common.h"

/* The pool is large (~half a MB with strings) — keep one static so libFuzzer
 * doesn't blow its default 2 GB rss_limit on a many-iteration run. */
static ASTPool g_pool;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 1u << 16) return 0;  /* skip pathologically large inputs */

    char *src = fuzz_dup_input(data, size);
    if (!src) return 0;

    pool_init(&g_pool);
    Parser p = parser_new(src, &g_pool);
    (void)parser_parse(&p);
    parser_free(&p);
    pool_free(&g_pool);

    free(src);
    return 0;
}
