/* fuzz_json2.c — std.json2 full-DOM parser harness.
 *
 * Drives j2_parse → j2_navigate → j2_stringify_node → j2_free_node.
 * The lexer is byte-oriented (j2_peek/j2_next bound by l->len), and the
 * recursive parser allocates a calloc'd J2Node per value, a 4 KiB buf
 * per string, and grows the string builder via realloc — every alloc is
 * tracked by ASan's leak detector.
 *
 * To exercise j2_navigate, half the input is fed to the parser and the
 * second half (split at the first NUL or '\xFF' separator, falling back
 * to size/2) is used as the navigation path.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"

/* Pull in the full json2 implementation. json2 only needs scope.h's Value
 * type and err.h — we provide both via the header chain. */
#include "../src/scope.h"
#include "../src/err.h"
#include "../src/std/json2/fluxa_std_json2.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 1u << 16) return 0;

    /* Find a separator so we can drive both parse and navigate from one
     * libFuzzer mutation. We split on the first 0xFF byte — common
     * mutators add/flip arbitrary bytes, so this is reachable. */
    size_t json_sz = size;
    size_t path_off = size;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == 0xFF) { json_sz = i; path_off = i + 1; break; }
    }

    char *json = fuzz_dup_input(data, json_sz);
    if (!json) return 0;

    Json2Doc *doc = j2_parse(json);
    if (doc) {
        if (doc->root) {
            /* Navigation path */
            size_t plen = (path_off < size) ? (size - path_off) : 0;
            if (plen > 256) plen = 256;
            char path[257];
            for (size_t i = 0; i < plen; i++) {
                uint8_t b = data[path_off + i];
                /* Restrict to printable ASCII; the navigator only looks
                 * at '.', '[', ']', digits, and key chars. */
                path[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
            }
            path[plen] = '\0';
            J2Node *target = j2_navigate(doc->root, path);
            (void)target;

            /* Stringify always re-serializes from the root. */
            J2SB sb = {0, 0, 0};
            j2_stringify_node(doc->root, &sb);
            free(sb.buf);
        }
        j2_free_node(doc->root);
        free(doc);
    }
    free(json);
    return 0;
}
