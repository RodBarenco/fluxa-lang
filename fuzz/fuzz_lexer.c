/* fuzz_lexer.c — Fluxa lexer harness.
 *
 * Drives lexer_next() to EOF or TOK_ERROR, frees each token. Catches any
 * OOB read past l->len, leaks from make_tok's strdup, and UB in the
 * escape-sequence handling.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../src/lexer.h"
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *src = fuzz_dup_input(data, size);
    if (!src) return 0;

    Lexer l = lexer_new(src);
    for (int n = 0; n < 100000; n++) {
        Token t = lexer_next(&l);
        TokenType ty = t.type;
        token_free(&t);
        if (ty == TOK_EOF || ty == TOK_ERROR) break;
    }
    free(src);
    return 0;
}
