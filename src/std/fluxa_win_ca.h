#ifndef FLUXA_WIN_CA_H
#define FLUXA_WIN_CA_H

/*
 * Windows CA trust for the libcurl-backed libs (std.httpc, std.https).
 *
 * The standalone Windows runtime links MSYS2's libcurl, which is built against
 * OpenSSL with its CA bundle path compiled in as
 * "/mingw64/etc/ssl/certs/ca-bundle.crt". That is an MSYS2 build path: on a
 * machine that merely runs the distributed executable it does not exist, so
 * peer verification failed for every HTTPS request:
 *
 *     https: SSL peer certificate or SSH remote key was not OK
 *
 * CURLSSLOPT_NATIVE_CA alone does not rescue it. curl_easy_setopt accepts the
 * bit and returns CURLE_OK, but the OpenSSL backend only acts on it when curl
 * was built with native-CA support — MSYS2's is not — so it is silently
 * ignored, which is why the failure looked like a code bug rather than a build
 * one.
 *
 * So the runtime reads the trust anchors of the machine it is running on:
 * enumerate the Windows ROOT store through crypt32 (already an allowed system
 * DLL for the standalone dependency gate), convert each certificate to PEM, and
 * hand the concatenation to curl as an in-memory bundle. Nothing ships beside
 * the executable and nothing goes stale.
 *
 * Precedence:
 *   1. CURL_CA_BUNDLE  — the documented operator override (private CA, proxy);
 *   2. Schannel builds — left to CURLSSLOPT_NATIVE_CA, which is genuinely
 *      native there and keeps Windows' automatic root updates working;
 *   3. otherwise       — the ROOT store, passed as CURLOPT_CAINFO_BLOB.
 *
 * Verification is never disabled. If no anchors can be found the handle is left
 * untouched and the request fails closed.
 *
 * std.mcpc and std.mcps use libcurl the same way and should call
 * fluxa_win_ca_apply() too whenever they are enabled in a Windows profile;
 * today they are not built on Windows, so they are deliberately left alone
 * rather than carrying an untested path.
 */

#if defined(_WIN32)

#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#ifndef _WINDOWS_
#  include <windows.h>
#endif
/* WIN32_LEAN_AND_MEAN (set by the Windows profiles) leaves cryptography out of
 * windows.h, so the certificate store API has to be pulled in explicitly. */
#include <wincrypt.h>

/* Concatenate every certificate in the Windows ROOT store as PEM. Returns NULL
 * when the store cannot be read or holds nothing usable. */
static inline char *fluxa_win_ca_build(size_t *out_len) {
    HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
    if (!store) return NULL;

    char  *pem = NULL;
    size_t len = 0, cap = 0;
    PCCERT_CONTEXT ctx = NULL;

    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != NULL) {
        DWORD need = 0;
        /* Sizing call: `need` comes back counting the NUL terminator. */
        if (!CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded,
                                  CRYPT_STRING_BASE64HEADER, NULL, &need))
            continue;
        if (len + need + 1 > cap) {
            size_t ncap = (len + need + 1) * 2;
            char *grown = (char *)realloc(pem, ncap);
            if (!grown) { free(pem); CertCloseStore(store, 0); return NULL; }
            pem = grown; cap = ncap;
        }
        DWORD got = need;
        /* Write call: `got` comes back NOT counting the NUL, so the next
         * certificate appends directly after this one's END line. */
        if (!CryptBinaryToStringA(ctx->pbCertEncoded, ctx->cbCertEncoded,
                                  CRYPT_STRING_BASE64HEADER, pem + len, &got))
            continue;
        len += got;
    }
    CertCloseStore(store, 0);

    if (!pem || len == 0) { free(pem); return NULL; }
    pem[len] = '\0';
    *out_len = len;
    return pem;
}

/* True when curl's TLS backend consults the Windows store itself. */
static inline int fluxa_win_ca_backend_is_native(void) {
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    return v && v->ssl_version && !strncmp(v->ssl_version, "Schannel", 8);
}

/* Give `curl` trust anchors that exist on the machine running this build. */
static inline void fluxa_win_ca_apply(CURL *curl) {
    const char *env = getenv("CURL_CA_BUNDLE");
    if (env && env[0]) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, env);
        return;
    }

#if defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    /* Schannel honours that and keeps Windows' automatic root updates in play;
     * overriding it with a snapshot of the store would only make trust worse. */
    if (fluxa_win_ca_backend_is_native()) return;
#endif

#if LIBCURL_VERSION_NUM >= 0x074700   /* CURLOPT_CAINFO_BLOB — libcurl 7.71 */
    {
        /* Built once per process; the store does not change under us often
         * enough to be worth rebuilding per request. */
        static char  *cached     = NULL;
        static size_t cached_len = 0;
        static int    attempted  = 0;
        if (!attempted) { attempted = 1; cached = fluxa_win_ca_build(&cached_len); }
        if (cached) {
            struct curl_blob blob;
            blob.data = cached;
            blob.len  = cached_len;
            /* The buffer outlives every request, so curl can borrow it rather
             * than copy ~40 KB per handle. */
            blob.flags = CURL_BLOB_NOCOPY;
            curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
        }
    }
#endif
}

#endif /* _WIN32 */
#endif /* FLUXA_WIN_CA_H */
