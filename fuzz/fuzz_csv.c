/* fuzz_csv.c — std.csv field-parser harness.
 *
 * csv.field walks a single CSV row character-by-character handling quoted
 * fields and "" escapes. We drive it via the dispatch function so the
 * argument-validation paths get coverage too. The delimiter and target
 * field index are pulled from the start of the input to maximise
 * variation; the remainder is the row payload.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include "fuzz_common.h"
#include "../src/scope.h"
#include "../src/err.h"
#include "../src/std/csv/fluxa_std_csv.h"

/* Increase stack limit to 64 MB at startup to prevent libFuzzer's own
 * signal/crash-save machinery from overflowing on deep corpora. */
__attribute__((constructor))
static void fuzz_csv_init(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_STACK, &rl) == 0) {
        rl.rlim_cur = 64 * 1024 * 1024;  /* 64 MB */
        setrlimit(RLIMIT_STACK, &rl);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096) return 0;

    /* Reserve 2 bytes from the front: 1 = field index, 1 = delimiter. */
    int idx = 0;
    char delim_str[2] = { ',', 0 };
    size_t row_off = 0;
    if (size >= 1) { idx = data[0] & 0x3f; row_off = 1; }     /* 0..63 */
    if (size >= 2) { delim_str[0] = (char)data[1]; row_off = 2; }
    if (delim_str[0] == '\0' || delim_str[0] == '"') delim_str[0] = ',';

    char *row = fuzz_dup_input(data + row_off, size - row_off);
    if (!row) return 0;
    /* heap-copy the delim so ASan can properly poison boundaries */
    char *delim_heap = strdup(delim_str);
    if (!delim_heap) { free(row); return 0; }

    Value args[3];
    args[0].type = VAL_STRING; args[0].as.string = row;
    args[1].type = VAL_INT;    args[1].as.integer = idx;
    args[2].type = VAL_STRING; args[2].as.string = delim_heap;

    ErrStack err;
    errstack_clear(&err);
    int had_error = 0;
    Value got = fluxa_std_csv_call("field", args, 3, &err, &had_error, 1);
    value_free_data(&got);

    /* field_count: args[1] = delim (VAL_STRING), args[0] = row */
    Value args_fc[2];
    args_fc[0].type = VAL_STRING; args_fc[0].as.string = row;
    args_fc[1].type = VAL_STRING; args_fc[1].as.string = delim_heap;
    errstack_clear(&err);
    had_error = 0;
    got = fluxa_std_csv_call("field_count", args_fc, 2, &err, &had_error, 1);
    value_free_data(&got);

    /* Also fuzz with wrong types to exercise guards */
    Value args_bad[2];
    args_bad[0].type = VAL_INT; args_bad[0].as.integer = 42;  /* wrong type for row */
    args_bad[1].type = VAL_INT; args_bad[1].as.integer = 0;
    errstack_clear(&err); had_error = 0;
    got = fluxa_std_csv_call("field", args_bad, 2, &err, &had_error, 1);
    value_free_data(&got);

    free(delim_heap);
    free(row);
    return 0;
}
