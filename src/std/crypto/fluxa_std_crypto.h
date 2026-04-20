/* fluxa_std_crypto.h — Fluxa Standard Library: crypto
 *
 * Wraps libsodium (1.0.18+) following the CREATING_LIBS.md pattern exactly.
 * All primitives are modern and misuse-resistant (libsodium high-level API).
 *
 * Functions:
 *   crypto.hash(data)                  BLAKE2b-256 -> int arr[32]
 *   crypto.to_hex(arr)                 int arr -> hex str
 *   crypto.from_hex(str)               hex str -> int arr
 *   crypto.keygen()                    random 32-byte key -> int arr[32]
 *   crypto.nonce()                     random 24-byte nonce -> int arr[24]
 *   crypto.encrypt(msg, key, nonce)    XSalsa20-Poly1305 -> int arr
 *   crypto.decrypt(cipher, key, nonce) -> str (or err inside danger)
 *   crypto.sign_keygen(pk, sk)         Ed25519 keypair (writes into arr args)
 *   crypto.sign(msg, sk)               sign -> int arr (sig+msg)
 *   crypto.sign_open(signed, pk)       verify + extract -> str (or err)
 *   crypto.kx_keygen(pk, sk)           Curve25519 keypair
 *   crypto.kx_client(rx, tx, cpk, csk, spk)  client session keys
 *   crypto.kx_server(rx, tx, spk, ssk, cpk)  server session keys
 *   crypto.compare(a, b)               constant-time compare -> bool
 *   crypto.wipe(arr)                   zero arr in place -> nil
 *   crypto.version()                   libsodium version -> str
 *
 * Key material in int arr: each element is VAL_INT holding one byte [0..255].
 * prst int arr key survives hot reloads naturally.
 *
 * Sprint 12 -- std.crypto
 */
#ifndef FLUXA_STD_CRYPTO_H
#define FLUXA_STD_CRYPTO_H

#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

/* ------------------------------------------------------------------ */
/* Value constructors                                                  */
/* ------------------------------------------------------------------ */
static inline Value fluxa_crypto_nil(void) {
    Value v; v.type = VAL_NIL; return v;
}
static inline Value fluxa_crypto_bool(int b) {
    Value v; v.type = VAL_BOOL; v.as.boolean = b; return v;
}
static inline Value fluxa_crypto_str(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = strdup(s ? s : "");
    return v;
}

/* Build a Fluxa int arr from raw bytes.
 * Each byte -> VAL_INT element; arr.owned=1 (runtime frees on scope exit). */
static inline Value fluxa_crypto_bytes_to_arr(const unsigned char *buf, int len) {
    Value *data = (Value *)malloc(sizeof(Value) * (size_t)(len < 1 ? 1 : len));
    if (!data) return fluxa_crypto_nil();
    for (int i = 0; i < len; i++) {
        data[i].type       = VAL_INT;
        data[i].as.integer = (long)(unsigned char)buf[i];
    }
    Value v;
    v.type         = VAL_ARR;
    v.as.arr.data  = data;
    v.as.arr.size  = len;
    v.as.arr.owned = 1;
    return v;
}

/* Extract raw bytes from a Fluxa int arr.
 * Returns malloc'd buffer (caller frees), NULL on error (writes errbuf). */
static inline unsigned char *fluxa_crypto_arr_to_bytes(const Value *v,
                                                         int *out_len,
                                                         char *errbuf,
                                                         int eblen) {
    if (!v || v->type != VAL_ARR || !v->as.arr.data) {
        snprintf(errbuf, eblen, "expected int arr");
        return NULL;
    }
    int n = v->as.arr.size;
    unsigned char *buf = (unsigned char *)malloc((size_t)(n + 1));
    if (!buf) { snprintf(errbuf, eblen, "out of memory"); return NULL; }
    for (int i = 0; i < n; i++) {
        Value elem = v->as.arr.data[i];
        if (elem.type != VAL_INT) {
            snprintf(errbuf, eblen, "arr[%d] is not int", i);
            free(buf); return NULL;
        }
        long val = elem.as.integer;
        if (val < 0 || val > 255) {
            snprintf(errbuf, eblen, "arr[%d] out of byte range: %ld", i, val);
            free(buf); return NULL;
        }
        buf[i] = (unsigned char)val;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

/* Accept str or int arr as message/data input. */
static inline unsigned char *fluxa_crypto_any_to_bytes(const Value *v,
                                                         int *out_len,
                                                         char *errbuf,
                                                         int eblen) {
    if (v->type == VAL_STRING) {
        const char *s = v->as.string ? v->as.string : "";
        int n = (int)strlen(s);
        unsigned char *buf = (unsigned char *)malloc((size_t)(n + 1));
        if (!buf) { snprintf(errbuf, eblen, "out of memory"); return NULL; }
        memcpy(buf, s, (size_t)n); buf[n] = '\0';
        if (out_len) *out_len = n;
        return buf;
    }
    return fluxa_crypto_arr_to_bytes(v, out_len, errbuf, eblen);
}

/* Write bytes into an existing Fluxa int arr (for keygen-style functions). */
static inline void fluxa_crypto_write_arr(Value *arr_val,
                                           const unsigned char *buf, int len) {
    for (int i = 0; i < len && i < arr_val->as.arr.size; i++) {
        arr_val->as.arr.data[i].type       = VAL_INT;
        arr_val->as.arr.data[i].as.integer = (long)buf[i];
    }
}

/* ------------------------------------------------------------------ */
/* Main dispatch function                                              */
/* ------------------------------------------------------------------ */
static inline Value fluxa_std_crypto_call(const char *fn_name,
                                           const Value *args, int argc,
                                           ErrStack *err, int *had_error,
                                           int line) {
    char errbuf[280];

#define CERR(msg) do { \
    snprintf(errbuf, sizeof(errbuf), "crypto.%s (line %d): %s", \
             fn_name, line, (msg)); \
    errstack_push(err, ERR_FLUXA, errbuf, "std.crypto", line); \
    *had_error = 1; return fluxa_crypto_nil(); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) { \
        snprintf(errbuf, sizeof(errbuf), \
            "crypto.%s: expected %d arg(s), got %d", fn_name, (n), argc); \
        errstack_push(err, ERR_FLUXA, errbuf, "std.crypto", line); \
        *had_error = 1; return fluxa_crypto_nil(); \
    } \
} while(0)

    if (sodium_init() < 0) CERR("sodium_init() failed");

    /* crypto.version() -> str */
    if (strcmp(fn_name, "version") == 0)
        return fluxa_crypto_str(sodium_version_string());

    /* crypto.hash(data) -> int arr[32]  BLAKE2b-256 */
    if (strcmp(fn_name, "hash") == 0) {
        NEED(1);
        char eb[128]={0}; int dlen=0;
        unsigned char *data = fluxa_crypto_any_to_bytes(&args[0], &dlen, eb, sizeof(eb));
        if (!data) CERR(eb);
        unsigned char out[crypto_generichash_BYTES];
        crypto_generichash(out, sizeof(out), data, (unsigned long long)dlen, NULL, 0);
        free(data);
        return fluxa_crypto_bytes_to_arr(out, (int)crypto_generichash_BYTES);
    }

    /* crypto.to_hex(arr) -> str */
    if (strcmp(fn_name, "to_hex") == 0) {
        NEED(1);
        char eb[128]={0}; int dlen=0;
        unsigned char *data = fluxa_crypto_arr_to_bytes(&args[0], &dlen, eb, sizeof(eb));
        if (!data) CERR(eb);
        char *hex = (char *)malloc((size_t)(dlen*2+1));
        if (!hex) { free(data); CERR("out of memory"); }
        sodium_bin2hex(hex, (size_t)(dlen*2+1), data, (size_t)dlen);
        free(data);
        Value r = fluxa_crypto_str(hex); free(hex); return r;
    }

    /* crypto.from_hex(str) -> int arr */
    if (strcmp(fn_name, "from_hex") == 0) {
        NEED(1);
        if (args[0].type != VAL_STRING) CERR("expected str");
        const char *hex = args[0].as.string ? args[0].as.string : "";
        size_t hexlen = strlen(hex);
        if (hexlen % 2 != 0) CERR("hex length must be even");
        unsigned char *bin = (unsigned char *)malloc(hexlen/2 + 1);
        if (!bin) CERR("out of memory");
        size_t actual = 0;
        if (sodium_hex2bin(bin, hexlen/2, hex, hexlen, NULL, &actual, NULL) != 0) {
            free(bin); CERR("invalid hex string");
        }
        Value r = fluxa_crypto_bytes_to_arr(bin, (int)actual); free(bin); return r;
    }

    /* crypto.keygen() -> int arr[32]  random XSalsa20-Poly1305 key */
    if (strcmp(fn_name, "keygen") == 0) {
        unsigned char key[crypto_secretbox_KEYBYTES];
        crypto_secretbox_keygen(key);
        return fluxa_crypto_bytes_to_arr(key, (int)crypto_secretbox_KEYBYTES);
    }

    /* crypto.nonce() -> int arr[24]  random nonce */
    if (strcmp(fn_name, "nonce") == 0) {
        unsigned char n[crypto_secretbox_NONCEBYTES];
        randombytes_buf(n, sizeof(n));
        return fluxa_crypto_bytes_to_arr(n, (int)crypto_secretbox_NONCEBYTES);
    }

    /* crypto.encrypt(msg, key, nonce) -> int arr  XSalsa20-Poly1305 */
    if (strcmp(fn_name, "encrypt") == 0) {
        NEED(3);
        char eb[128]={0}; int mlen=0,klen=0,nlen=0;
        unsigned char *msg   = fluxa_crypto_any_to_bytes(&args[0], &mlen, eb, sizeof(eb));
        if (!msg) CERR(eb);
        unsigned char *key   = fluxa_crypto_arr_to_bytes(&args[1], &klen, eb, sizeof(eb));
        if (!key) { free(msg); CERR(eb); }
        if (klen != (int)crypto_secretbox_KEYBYTES) {
            free(msg); free(key); CERR("key must be 32 bytes (use crypto.keygen())");
        }
        unsigned char *nonce = fluxa_crypto_arr_to_bytes(&args[2], &nlen, eb, sizeof(eb));
        if (!nonce) { free(msg); free(key); CERR(eb); }
        if (nlen != (int)crypto_secretbox_NONCEBYTES) {
            free(msg); free(key); free(nonce);
            CERR("nonce must be 24 bytes (use crypto.nonce())");
        }
        unsigned long long clen = (unsigned long long)mlen + crypto_secretbox_MACBYTES;
        unsigned char *cipher = (unsigned char *)malloc((size_t)clen);
        if (!cipher) { free(msg); free(key); free(nonce); CERR("out of memory"); }
        crypto_secretbox_easy(cipher, msg, (unsigned long long)mlen, nonce, key);
        free(msg); free(key); free(nonce);
        Value r = fluxa_crypto_bytes_to_arr(cipher, (int)clen); free(cipher); return r;
    }

    /* crypto.decrypt(cipher, key, nonce) -> str */
    if (strcmp(fn_name, "decrypt") == 0) {
        NEED(3);
        char eb[128]={0}; int clen=0,klen=0,nlen=0;
        unsigned char *cipher = fluxa_crypto_arr_to_bytes(&args[0], &clen, eb, sizeof(eb));
        if (!cipher) CERR(eb);
        if (clen < (int)crypto_secretbox_MACBYTES) { free(cipher); CERR("ciphertext too short"); }
        unsigned char *key   = fluxa_crypto_arr_to_bytes(&args[1], &klen, eb, sizeof(eb));
        if (!key) { free(cipher); CERR(eb); }
        if (klen != (int)crypto_secretbox_KEYBYTES) { free(cipher); free(key); CERR("key must be 32 bytes"); }
        unsigned char *nonce = fluxa_crypto_arr_to_bytes(&args[2], &nlen, eb, sizeof(eb));
        if (!nonce) { free(cipher); free(key); CERR(eb); }
        if (nlen != (int)crypto_secretbox_NONCEBYTES) {
            free(cipher); free(key); free(nonce); CERR("nonce must be 24 bytes");
        }
        unsigned long long plen = (unsigned long long)clen - crypto_secretbox_MACBYTES;
        unsigned char *plain = (unsigned char *)malloc((size_t)(plen+1));
        if (!plain) { free(cipher); free(key); free(nonce); CERR("out of memory"); }
        int rc = crypto_secretbox_open_easy(plain, cipher, (unsigned long long)clen, nonce, key);
        free(cipher); free(key); free(nonce);
        if (rc != 0) { free(plain); CERR("authentication failed -- wrong key or corrupted"); }
        plain[plen] = '\0';
        Value r = fluxa_crypto_str((char *)plain); free(plain); return r;
    }

    /* crypto.sign_keygen(pk, sk) -> nil  Ed25519 keypair into existing arrs */
    if (strcmp(fn_name, "sign_keygen") == 0) {
        NEED(2);
        if (args[0].type != VAL_ARR || !args[0].as.arr.data ||
            args[0].as.arr.size < (int)crypto_sign_PUBLICKEYBYTES)
            CERR("pk must be int arr[>=32]");
        if (args[1].type != VAL_ARR || !args[1].as.arr.data ||
            args[1].as.arr.size < (int)crypto_sign_SECRETKEYBYTES)
            CERR("sk must be int arr[>=64]");
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sk[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(pk, sk);
        /* write via non-const cast -- keygen modifies the caller's arr */
        Value *pkv = (Value *)&args[0];
        Value *skv = (Value *)&args[1];
        fluxa_crypto_write_arr(pkv, pk, (int)crypto_sign_PUBLICKEYBYTES);
        fluxa_crypto_write_arr(skv, sk, (int)crypto_sign_SECRETKEYBYTES);
        return fluxa_crypto_nil();
    }

    /* crypto.sign(msg, sk) -> int arr  Ed25519 detached sign+msg */
    if (strcmp(fn_name, "sign") == 0) {
        NEED(2);
        char eb[128]={0}; int mlen=0,sklen=0;
        unsigned char *msg = fluxa_crypto_any_to_bytes(&args[0], &mlen, eb, sizeof(eb));
        if (!msg) CERR(eb);
        unsigned char *sk  = fluxa_crypto_arr_to_bytes(&args[1], &sklen, eb, sizeof(eb));
        if (!sk) { free(msg); CERR(eb); }
        if (sklen != (int)crypto_sign_SECRETKEYBYTES) {
            free(msg); free(sk); CERR("sk must be 64 bytes (use crypto.sign_keygen())");
        }
        unsigned long long outlen = (unsigned long long)mlen + crypto_sign_BYTES;
        unsigned char *out = (unsigned char *)malloc((size_t)outlen);
        if (!out) { free(msg); free(sk); CERR("out of memory"); }
        unsigned long long siglen = 0;
        crypto_sign_ed25519(out, &siglen, msg, (unsigned long long)mlen, sk);
        free(msg); free(sk);
        Value r = fluxa_crypto_bytes_to_arr(out, (int)siglen); free(out); return r;
    }

    /* crypto.sign_open(signed_msg, pk) -> str  verify + extract */
    if (strcmp(fn_name, "sign_open") == 0) {
        NEED(2);
        char eb[128]={0}; int smlen=0,pklen=0;
        unsigned char *sm = fluxa_crypto_arr_to_bytes(&args[0], &smlen, eb, sizeof(eb));
        if (!sm) CERR(eb);
        if (smlen < (int)crypto_sign_BYTES) { free(sm); CERR("signed message too short"); }
        unsigned char *pk = fluxa_crypto_arr_to_bytes(&args[1], &pklen, eb, sizeof(eb));
        if (!pk) { free(sm); CERR(eb); }
        if (pklen != (int)crypto_sign_PUBLICKEYBYTES) {
            free(sm); free(pk); CERR("pk must be 32 bytes");
        }
        unsigned char *plain = (unsigned char *)malloc((size_t)smlen);
        if (!plain) { free(sm); free(pk); CERR("out of memory"); }
        unsigned long long plen = 0;
        int rc = crypto_sign_ed25519_open(plain, &plen,
                                           sm, (unsigned long long)smlen, pk);
        free(sm); free(pk);
        if (rc != 0) { free(plain); CERR("invalid signature -- tampered or wrong key"); }
        plain[plen] = '\0';
        Value r = fluxa_crypto_str((char *)plain); free(plain); return r;
    }

    /* crypto.kx_keygen(pk, sk) -> nil  Curve25519 keypair into arrs */
    if (strcmp(fn_name, "kx_keygen") == 0) {
        NEED(2);
        if (args[0].type != VAL_ARR || !args[0].as.arr.data ||
            args[0].as.arr.size < (int)crypto_kx_PUBLICKEYBYTES)
            CERR("pk must be int arr[>=32]");
        if (args[1].type != VAL_ARR || !args[1].as.arr.data ||
            args[1].as.arr.size < (int)crypto_kx_SECRETKEYBYTES)
            CERR("sk must be int arr[>=32]");
        unsigned char pk[crypto_kx_PUBLICKEYBYTES];
        unsigned char sk[crypto_kx_SECRETKEYBYTES];
        crypto_kx_keypair(pk, sk);
        Value *pkv = (Value *)&args[0]; Value *skv = (Value *)&args[1];
        fluxa_crypto_write_arr(pkv, pk, (int)crypto_kx_PUBLICKEYBYTES);
        fluxa_crypto_write_arr(skv, sk, (int)crypto_kx_SECRETKEYBYTES);
        return fluxa_crypto_nil();
    }

    /* crypto.kx_client(rx, tx, cpk, csk, spk) -> nil */
    if (strcmp(fn_name, "kx_client") == 0) {
        NEED(5);
        if (args[0].type != VAL_ARR || !args[0].as.arr.data ||
            args[0].as.arr.size < (int)crypto_kx_SESSIONKEYBYTES)
            CERR("rx must be int arr[>=32]");
        if (args[1].type != VAL_ARR || !args[1].as.arr.data ||
            args[1].as.arr.size < (int)crypto_kx_SESSIONKEYBYTES)
            CERR("tx must be int arr[>=32]");
        char eb[128]={0}; int len=0;
        unsigned char *cpk = fluxa_crypto_arr_to_bytes(&args[2], &len, eb, sizeof(eb));
        if (!cpk || len != (int)crypto_kx_PUBLICKEYBYTES) { free(cpk); CERR("cpk must be 32 bytes"); }
        unsigned char *csk = fluxa_crypto_arr_to_bytes(&args[3], &len, eb, sizeof(eb));
        if (!csk || len != (int)crypto_kx_SECRETKEYBYTES) { free(cpk); free(csk); CERR("csk must be 32 bytes"); }
        unsigned char *spk = fluxa_crypto_arr_to_bytes(&args[4], &len, eb, sizeof(eb));
        if (!spk || len != (int)crypto_kx_PUBLICKEYBYTES) {
            free(cpk); free(csk); free(spk); CERR("spk must be 32 bytes");
        }
        unsigned char rx[crypto_kx_SESSIONKEYBYTES], tx[crypto_kx_SESSIONKEYBYTES];
        int rc = crypto_kx_client_session_keys(rx, tx, cpk, csk, spk);
        free(cpk); free(csk); free(spk);
        if (rc != 0) CERR("kx_client failed -- invalid server public key");
        Value *rxv = (Value *)&args[0]; Value *txv = (Value *)&args[1];
        fluxa_crypto_write_arr(rxv, rx, (int)crypto_kx_SESSIONKEYBYTES);
        fluxa_crypto_write_arr(txv, tx, (int)crypto_kx_SESSIONKEYBYTES);
        return fluxa_crypto_nil();
    }

    /* crypto.kx_server(rx, tx, spk, ssk, cpk) -> nil */
    if (strcmp(fn_name, "kx_server") == 0) {
        NEED(5);
        if (args[0].type != VAL_ARR || !args[0].as.arr.data ||
            args[0].as.arr.size < (int)crypto_kx_SESSIONKEYBYTES)
            CERR("rx must be int arr[>=32]");
        if (args[1].type != VAL_ARR || !args[1].as.arr.data ||
            args[1].as.arr.size < (int)crypto_kx_SESSIONKEYBYTES)
            CERR("tx must be int arr[>=32]");
        char eb[128]={0}; int len=0;
        unsigned char *spk = fluxa_crypto_arr_to_bytes(&args[2], &len, eb, sizeof(eb));
        if (!spk || len != (int)crypto_kx_PUBLICKEYBYTES) { free(spk); CERR("spk must be 32 bytes"); }
        unsigned char *ssk = fluxa_crypto_arr_to_bytes(&args[3], &len, eb, sizeof(eb));
        if (!ssk || len != (int)crypto_kx_SECRETKEYBYTES) { free(spk); free(ssk); CERR("ssk must be 32 bytes"); }
        unsigned char *cpk = fluxa_crypto_arr_to_bytes(&args[4], &len, eb, sizeof(eb));
        if (!cpk || len != (int)crypto_kx_PUBLICKEYBYTES) {
            free(spk); free(ssk); free(cpk); CERR("cpk must be 32 bytes");
        }
        unsigned char rx[crypto_kx_SESSIONKEYBYTES], tx[crypto_kx_SESSIONKEYBYTES];
        int rc = crypto_kx_server_session_keys(rx, tx, spk, ssk, cpk);
        free(spk); free(ssk); free(cpk);
        if (rc != 0) CERR("kx_server failed -- invalid client public key");
        Value *rxv = (Value *)&args[0]; Value *txv = (Value *)&args[1];
        fluxa_crypto_write_arr(rxv, rx, (int)crypto_kx_SESSIONKEYBYTES);
        fluxa_crypto_write_arr(txv, tx, (int)crypto_kx_SESSIONKEYBYTES);
        return fluxa_crypto_nil();
    }

    /* crypto.compare(a, b) -> bool  constant-time comparison */
    if (strcmp(fn_name, "compare") == 0) {
        NEED(2);
        if (args[0].type != VAL_ARR || args[1].type != VAL_ARR)
            CERR("both arguments must be int arr");
        if (args[0].as.arr.size != args[1].as.arr.size)
            return fluxa_crypto_bool(0);
        char eb[64]={0}; int la=0,lb=0;
        unsigned char *a = fluxa_crypto_arr_to_bytes(&args[0], &la, eb, sizeof(eb));
        unsigned char *b = fluxa_crypto_arr_to_bytes(&args[1], &lb, eb, sizeof(eb));
        if (!a || !b) { free(a); free(b); CERR(eb); }
        int eq = (sodium_memcmp(a, b, (size_t)la) == 0);
        free(a); free(b);
        return fluxa_crypto_bool(eq);
    }

    /* crypto.wipe(arr) -> nil  secure zero */
    if (strcmp(fn_name, "wipe") == 0) {
        NEED(1);
        if (args[0].type != VAL_ARR) CERR("expected int arr");
        if (args[0].as.arr.data && args[0].as.arr.size > 0) {
            Value *d = args[0].as.arr.data;
            int n = args[0].as.arr.size;
            /* Zero only the integer field — preserve type=VAL_INT so arr
             * remains readable after wipe. sodium_memzero on the whole Value
             * array would corrupt the type tag and make elements read as nil. */
            for (int i = 0; i < n; i++) {
                d[i].type = VAL_INT; d[i].as.integer = 0;
            }
            /* Compiler barrier — prevent optimization of the loop above */
            __asm__ __volatile__("" ::: "memory");
        }
        return fluxa_crypto_nil();
    }

#undef CERR
#undef NEED

    snprintf(errbuf, sizeof(errbuf),
        "crypto.%s: unknown function. Available: hash, to_hex, from_hex, "
        "keygen, nonce, encrypt, decrypt, sign_keygen, sign, sign_open, "
        "kx_keygen, kx_client, kx_server, compare, wipe, version", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "std.crypto", line);
    *had_error = 1;
    return fluxa_crypto_nil();
}


/* ── Fluxa lib descriptor — read by scripts/gen_lib_registry.py ───────── *
 * This block is the only integration point needed for the lib linker.    *
 * Do NOT edit lib_registry_gen.h manually — run 'make build' instead.   */
FLUXA_LIB_EXPORT(
    name     = "crypto",
    toml_key = "std.crypto",
    owner    = "crypto",
    call     = fluxa_std_crypto_call,
    rt_aware = 0
)

#endif /* FLUXA_STD_CRYPTO_H */
