/* fuzz_toml.c — fluxa.toml loader harness.
 *
 * fluxa_config_load() and fluxa_config_load_libs() both work on a file
 * path. Each invocation writes the fuzz input to a private tempfile and
 * calls both loaders, then unlinks. Output is buffered into FluxaConfig
 * (no heap allocation; just stack + memcpy), so the only thing the
 * sanitizer can catch here is a read/write OOB in the parser itself
 * (cfg_trim, cfg_unquote, cfg_parse_sig, section/key handling).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../src/toml_config.h"

/* Per-process tempfile. libFuzzer is single-threaded, so a fixed path is
 * fine and we avoid mkstemp churn on every iteration. */
static char g_path[64];

__attribute__((constructor))
static void fuzz_init_path(void) {
    snprintf(g_path, sizeof(g_path),
             "/tmp/fluxa-fuzz-toml-%d.toml", (int)getpid());
}

__attribute__((destructor))
static void fuzz_cleanup(void) {
    unlink(g_path);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The loader reads line-by-line with a 512-byte buffer, so very large
     * inputs add no extra coverage but make iterations slow. */
    if (size > 65536) return 0;

    FILE *f = fopen(g_path, "wb");
    if (!f) return 0;
    if (size > 0) fwrite(data, 1, size, f);
    fclose(f);

    FluxaConfig cfg = fluxa_config_load(g_path);
    fluxa_config_load_libs(&cfg, g_path);
    (void)cfg;  /* FluxaConfig holds no heap pointers — nothing to free. */
    return 0;
}
