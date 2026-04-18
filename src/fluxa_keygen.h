/* fluxa_keygen.h — Fluxa key management for FLUXA_SECURE builds
 *
 * Provides:
 *   fluxa_keygen_ed25519(dir)      generate Ed25519 keypair, save to dir/
 *   fluxa_key_load_signing(path)   load Ed25519 private key from file
 *   fluxa_key_load_hmac(path)      load or generate HMAC-SHA512 secret
 *   fluxa_ipc_hmac_sign(key, msg, len, out)   sign IPC request
 *   fluxa_ipc_hmac_verify(key, msg, len, sig) verify IPC HMAC
 *
 * Key file format:
 *   Ed25519 private key  : 64 raw bytes, mode 0400
 *   Ed25519 public key   : 32 raw bytes, mode 0444
 *   HMAC secret          : 32 raw bytes, mode 0400
 *
 * NEVER store key material in fluxa.toml. The toml [security] section
 * contains only the file PATH to the key, not the key itself.
 *
 * Only compiled when FLUXA_SECURE=1.
 */
#ifndef FLUXA_KEYGEN_H
#define FLUXA_KEYGEN_H

#ifdef FLUXA_SECURE

#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

/* ── Constants ──────────────────────────────────────────────────────────── */
#define FLUXA_SK_BYTES  crypto_sign_SECRETKEYBYTES  /* 64 */
#define FLUXA_PK_BYTES  crypto_sign_PUBLICKEYBYTES  /* 32 */
#define FLUXA_HMAC_BYTES crypto_auth_BYTES          /* 32 */
#define FLUXA_HMAC_KEY_BYTES crypto_auth_KEYBYTES   /* 32 */

/* ── fluxa_keygen_ed25519 ────────────────────────────────────────────────── *
 * Generates an Ed25519 keypair and writes:                                  *
 *   <dir>/signing.key  — private key, 64 bytes, 0400                       *
 *   <dir>/signing.pub  — public key,  32 bytes, 0444                       *
 * Returns 0 on success, -1 on error.                                       */
static inline int fluxa_keygen_ed25519(const char *dir) {
    if (sodium_init() < 0) {
        fprintf(stderr, "[fluxa] keygen: sodium_init() failed\n");
        return -1;
    }

    unsigned char pk[FLUXA_PK_BYTES];
    unsigned char sk[FLUXA_SK_BYTES];
    crypto_sign_keypair(pk, sk);

    /* Build paths */
    char sk_path[512], pk_path[512], fp_path[512];
    snprintf(sk_path, sizeof(sk_path), "%s/signing.key", dir);
    snprintf(pk_path, sizeof(pk_path), "%s/signing.pub", dir);
    snprintf(fp_path, sizeof(fp_path), "%s/signing.fingerprint", dir);

    /* Write private key — 0400 */
    FILE *f = fopen(sk_path, "wb");
    if (!f) {
        fprintf(stderr, "[fluxa] keygen: cannot write %s: %s\n",
                sk_path, strerror(errno));
        return -1;
    }
    fwrite(sk, 1, FLUXA_SK_BYTES, f);
    fclose(f);
    chmod(sk_path, 0400);

    /* Write public key — 0444 */
    f = fopen(pk_path, "wb");
    if (!f) {
        fprintf(stderr, "[fluxa] keygen: cannot write %s: %s\n",
                pk_path, strerror(errno));
        return -1;
    }
    fwrite(pk, 1, FLUXA_PK_BYTES, f);
    fclose(f);
    chmod(pk_path, 0444);

    /* Write hex fingerprint for easy identification */
    char hex[FLUXA_PK_BYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), pk, FLUXA_PK_BYTES);
    f = fopen(fp_path, "w");
    if (f) { fprintf(f, "%s\n", hex); fclose(f); chmod(fp_path, 0444); }

    /* Zero sk from stack */
    sodium_memzero(sk, sizeof(sk));

    printf("[fluxa] keygen: Ed25519 keypair generated\n");
    printf("  private key : %s  (0400 — keep secret)\n", sk_path);
    printf("  public key  : %s  (0444 — share freely)\n", pk_path);
    printf("  fingerprint : %s\n", hex);
    printf("\n");
    printf("  Add to fluxa.toml:\n");
    printf("    [security]\n");
    printf("    signing_key = \"%s\"\n", sk_path);
    printf("    mode        = \"strict\"\n");
    return 0;
}

/* ── fluxa_keygen_hmac ───────────────────────────────────────────────────── *
 * Generates a random HMAC-SHA512 secret key and writes:                    *
 *   <dir>/ipc_hmac.key  — 32 random bytes, 0400                            *
 * Returns 0 on success, -1 on error.                                       */
static inline int fluxa_keygen_hmac(const char *dir) {
    if (sodium_init() < 0) return -1;

    unsigned char key[FLUXA_HMAC_KEY_BYTES];
    randombytes_buf(key, sizeof(key));

    char key_path[512];
    snprintf(key_path, sizeof(key_path), "%s/ipc_hmac.key", dir);

    FILE *f = fopen(key_path, "wb");
    if (!f) {
        fprintf(stderr, "[fluxa] keygen: cannot write %s: %s\n",
                key_path, strerror(errno));
        sodium_memzero(key, sizeof(key));
        return -1;
    }
    fwrite(key, 1, FLUXA_HMAC_KEY_BYTES, f);
    fclose(f);
    chmod(key_path, 0400);
    sodium_memzero(key, sizeof(key));

    printf("[fluxa] keygen: HMAC key generated\n");
    printf("  ipc hmac key: %s  (0400 — keep secret)\n", key_path);
    printf("\n");
    printf("  Add to fluxa.toml:\n");
    printf("    [security]\n");
    printf("    ipc_hmac_key = \"%s\"\n", key_path);
    return 0;
}

/* ── fluxa_key_load ──────────────────────────────────────────────────────── *
 * Load raw bytes from a key file. Returns heap-allocated buffer (caller    *
 * must sodium_memzero + free), or NULL on error.                           */
static inline unsigned char *fluxa_key_load(const char *path,
                                             size_t expected_bytes) {
    /* Check permissions first — warn if key is world-readable */
    struct stat st;
    if (stat(path, &st) == 0) {
        if (st.st_mode & (S_IRGRP | S_IROTH)) {
            fprintf(stderr,
                "[fluxa] security WARNING: key file %s has loose permissions "
                "(mode %04o) — should be 0400\n",
                path, (unsigned)(st.st_mode & 0777));
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[fluxa] security: cannot read key %s: %s\n",
                path, strerror(errno));
        return NULL;
    }

    unsigned char *buf = (unsigned char *)sodium_malloc(expected_bytes);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, expected_bytes, f);
    fclose(f);

    if (n != expected_bytes) {
        fprintf(stderr,
            "[fluxa] security: key file %s: expected %zu bytes, got %zu\n",
            path, expected_bytes, n);
        sodium_free(buf);
        return NULL;
    }
    return buf;
}

/* ── HMAC-SHA512 sign / verify (for IPC request authentication) ──────────── */

/* Sign len bytes of msg with key, write 32-byte MAC to out[32] */
static inline int fluxa_hmac_sign(const unsigned char *key,  /* 32 bytes */
                                   const void *msg, size_t len,
                                   unsigned char out[FLUXA_HMAC_BYTES]) {
    return crypto_auth(out, (const unsigned char *)msg, len, key);
}

/* Verify MAC over msg. Returns 0 if valid, -1 if tampered. */
static inline int fluxa_hmac_verify(const unsigned char *key,
                                     const void *msg, size_t len,
                                     const unsigned char mac[FLUXA_HMAC_BYTES]) {
    return crypto_auth_verify(mac, (const unsigned char *)msg, len, key);
}

/* ── fluxa_security_check ────────────────────────────────────────────────── *
 * Called at runtime start when FLUXA_SECURE=1 and mode != off.            *
 * Validates that configured key files exist and are readable.              *
 * Returns 0 if ok, -1 if a required key is missing/unreadable.            */
static inline int fluxa_security_check(const char *signing_key_path,
                                        const char *hmac_key_path,
                                        int mode) {
    if (mode == FLUXA_SEC_MODE_OFF) return 0;

    int ok = 1;

    /* Helper: check file exists, readable, and warn on loose permissions */
    #define CHECK_KEY(path, required) do { \
        if ((path) && (path)[0]) { \
            if (access((path), R_OK) != 0) { \
                fprintf(stderr, \
                    "[fluxa] security: key file '%s' not readable: %s\n" \
                    "  Generate with: fluxa keygen --dir <dir>\n", \
                    (path), strerror(errno)); \
                if (required) ok = 0; \
            } else { \
                struct stat _st; \
                if (stat((path), &_st) == 0 && \
                    (_st.st_mode & (S_IRGRP | S_IROTH))) { \
                    fprintf(stderr, \
                        "[fluxa] security WARNING: key file '%s' has loose " \
                        "permissions (mode %04o) — should be 0400\n", \
                        (path), (unsigned)(_st.st_mode & 0777)); \
                } \
            } \
        } else if (required && mode == FLUXA_SEC_MODE_STRICT) { \
            fprintf(stderr, \
                "[fluxa] security: mode=strict but no %s configured.\n" \
                "  Add to [security] in fluxa.toml and run: fluxa keygen\n", \
                #path); \
            ok = 0; \
        } \
    } while(0)

    CHECK_KEY(signing_key_path, 1);
    CHECK_KEY(hmac_key_path, 0);    /* hmac key optional — warn only */
    #undef CHECK_KEY

    return ok ? 0 : -1;
}

#endif /* FLUXA_SECURE */
#endif /* FLUXA_KEYGEN_H */
