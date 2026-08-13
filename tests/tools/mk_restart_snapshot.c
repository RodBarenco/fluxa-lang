/* tests/tools/mk_restart_snapshot.c — build a restart snapshot by hand.
 *
 * Reproduces what ipc_server writes before execve: one prst dyn (a window
 * handle) and one prst int (a measurement counter, declared 0 and mutated to
 * 41). Used by prst_reload_resources.sh to exercise the runtime-swap half of
 * prst semantics without needing a second binary to swap to.
 *
 *   cc -std=c99 -D_POSIX_C_SOURCE=200809L -Isrc \
 *      tests/tools/mk_restart_snapshot.c src/scope.c -o mk_restart_snapshot
 *   ./mk_restart_snapshot snap.bin
 *
 * The point of the dyn entry: the wire format has no VAL_DYN case, so it comes
 * back as VAL_NIL while declared_type still reads VAL_DYN. That headstone is
 * what tells the runtime to rebuild the resource instead of restoring it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scope.h"
#include "prst_pool.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mk_restart_snapshot <out.bin>\n"); return 2; }

    PrstPool p;
    prst_pool_init(&p);

    /* An external resource handle: a pointer that cannot survive execve. */
    Value dynv; memset(&dynv, 0, sizeof(dynv));
    dynv.type   = VAL_DYN;
    dynv.as.dyn = (FluxaDyn *)calloc(1, sizeof(FluxaDyn));
    prst_pool_set(&p, "win", dynv, NULL);

    /* A measurement: declared 0, mutated to 41 by the previous run. */
    Value iv; memset(&iv, 0, sizeof(iv));
    iv.type = VAL_INT; iv.as.integer = 41;
    prst_pool_set(&p, "readings", iv, NULL);
    {
        int k = prst_pool_find(&p, "readings");
        Value z; memset(&z, 0, sizeof(z));
        z.type = VAL_INT; z.as.integer = 0;
        prst_value_free_clone(&p.entries[k].init_value);
        p.entries[k].init_value = z;   /* baseline = the declared value */
    }

    void  *buf = NULL;
    size_t sz  = 0;
    if (!prst_pool_serialize(&p, &buf, &sz)) return 1;

    FILE *f = fopen(argv[1], "wb");
    if (!f) { free(buf); return 1; }
    fwrite(buf, 1, sz, f);
    fclose(f);
    free(buf);

    printf("snapshot: %lu bytes, %d entries\n", (unsigned long)sz, p.count);
    return 0;
}
