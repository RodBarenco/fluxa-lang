/* fuzz_json.c — std.json streaming-style parser harness.
 *
 * std.json is "the JSON string is the data structure" — no DOM, just
 * key-value lookups over a flat object. The two interesting pure-string
 * primitives are json_read_string (handles \-escapes) and json_find_key
 * (walks a flat object). We exercise both off the same input, using a
 * 0xFF separator to split a key out of the corpus.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"
#include "../src/scope.h"
#include "../src/err.h"
#include "../src/toml_config.h"
#include "../src/std/json/fluxa_std_json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 1u << 16) return 0;

    size_t json_sz = size;
    size_t key_off = size;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == 0xFF) { json_sz = i; key_off = i + 1; break; }
    }

    char *json = fuzz_dup_input(data, json_sz);
    if (!json) return 0;

    /* Extract a key string. The key may contain embedded NULs; that is
     * intentional — json_find_key uses strcmp on it, so a NUL truncates
     * the comparison key but not the JSON walk. */
    size_t klen = (key_off < size) ? (size - key_off) : 0;
    if (klen > 64) klen = 64;
    char key[65];
    memcpy(key, data + key_off, klen);
    key[klen] = '\0';

    /* json_read_string: feed it our input directly (must start with '"'). */
    char buf[256];
    (void)json_read_string(json, buf, sizeof(buf));

    /* json_find_key over the full corpus. */
    const char *val = json_find_key(json, key);
    (void)val;

    free(json);
    return 0;
}
