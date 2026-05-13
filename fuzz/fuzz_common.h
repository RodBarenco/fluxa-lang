/* fuzz_common.h — shared helpers for libFuzzer drivers.
 *
 * Every Fluxa parser entry point takes a null-terminated `const char *`.
 * libFuzzer hands us `(const uint8_t *, size_t)`, which is neither
 * null-terminated nor guaranteed to lack embedded NULs. We copy into a
 * fresh malloc'd buffer that ends exactly at `size + 1`: this lets ASan
 * catch any read past the logical end of the input.
 *
 * Embedded NULs are preserved — the parser is responsible for handling
 * them. Some Fluxa entry points use strlen(), which means a NUL byte
 * inside the input effectively truncates the visible string. That is
 * the parser's contract; we don't pre-filter.
 */
#ifndef FLUXA_FUZZ_COMMON_H
#define FLUXA_FUZZ_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline char *fuzz_dup_input(const uint8_t *data, size_t size) {
    char *buf = (char *)malloc(size + 1);
    if (!buf) return NULL;
    if (size > 0) memcpy(buf, data, size);
    buf[size] = '\0';
    return buf;
}

#endif
