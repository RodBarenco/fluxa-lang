#ifndef FLUXA_STD_INFER_H
#define FLUXA_STD_INFER_H

/*
 * std.infer — local LLM inference for Fluxa-lang
 *
 * Two backends:
 *
 *   FLUXA_INFER_LLAMA=1   llama.cpp backend (requires llama.h + libllama)
 *     Runs quantized GGUF models locally. Zero network, works on CPU.
 *     Target: desktop Linux/macOS with 4-16GB RAM.
 *     Vendor llama.cpp into vendor/llama.h + vendor/libllama.a,
 *     then: make FLUXA_INFER_LLAMA=1 build
 *
 *   (default) stub backend
 *     API-complete. load() succeeds, generate() returns a placeholder.
 *     Useful for testing prompt pipelines and prst patterns without
 *     a real model. prst dyn cursor survives hot reloads.
 *
 * API:
 *   infer.load(model_path)            → dyn model cursor
 *   infer.generate(model, prompt)     → str  (generated text)
 *   infer.generate_n(model, prompt, n)→ str  (max n tokens)
 *   infer.unload(model)               → nil
 *   infer.loaded(model)               → bool
 *   infer.ctx_size(model)             → int  (context window tokens)
 *   infer.model_name(model)           → str
 *   infer.version()                   → str
 *
 * prst pattern for hot-reload survival:
 *   prst dyn model = infer.load("/models/mistral.gguf")
 *   // model cursor survives fluxa apply — no reload needed
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: llama.cpp (when FLUXA_INFER_LLAMA=1)
 * ════════════════════════════════════════════════════════════════════ */
#ifdef FLUXA_INFER_LLAMA

#include <llama.h>

typedef struct {
    struct llama_model   *model;
    struct llama_context *ctx;
    char  model_path[512];
    int   ctx_size;
    int   loaded;
} InferModel;

static InferModel *infer_do_load(const char *path) {
    struct llama_model_params mparams = llama_model_default_params();
    struct llama_model *m = llama_load_model_from_file(path, mparams);
    if (!m) return NULL;

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx    = 2048;
    cparams.n_threads = 4;
    struct llama_context *ctx = llama_new_context_with_model(m, cparams);
    if (!ctx) { llama_free_model(m); return NULL; }

    InferModel *im = (InferModel *)calloc(1, sizeof(InferModel));
    im->model = m; im->ctx = ctx;
    strncpy(im->model_path, path, sizeof(im->model_path)-1);
    im->ctx_size = (int)cparams.n_ctx;
    im->loaded   = 1;
    return im;
}

static char *infer_do_generate(InferModel *im, const char *prompt, int max_tokens) {
    if (max_tokens <= 0) max_tokens = 256;

    /* Tokenize prompt */
    int n_prompt = -llama_tokenize(im->model, prompt, (int)strlen(prompt),
                                    NULL, 0, 1, 1);
    llama_token *tokens = (llama_token *)malloc((size_t)n_prompt * sizeof(llama_token));
    llama_tokenize(im->model, prompt, (int)strlen(prompt), tokens, n_prompt, 1, 1);

    llama_batch batch = llama_batch_get_one(tokens, n_prompt);
    llama_decode(im->ctx, batch);
    free(tokens);

    /* Sample tokens */
    char *out = (char *)malloc((size_t)(max_tokens * 8 + 1));
    int   out_len = 0;
    out[0] = '\0';

    struct llama_sampler *sampler = llama_sampler_chain_init(
        llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

    for (int i = 0; i < max_tokens; i++) {
        llama_token tok = llama_sampler_sample(sampler, im->ctx, -1);
        if (llama_token_is_eog(im->model, tok)) break;

        char piece[32];
        int  plen = llama_token_to_piece(im->model, tok, piece, sizeof(piece), 0, 1);
        if (plen < 0) break;
        piece[plen] = '\0';

        if (out_len + plen < max_tokens * 8) {
            memcpy(out + out_len, piece, (size_t)plen);
            out_len += plen;
        }

        llama_batch next = llama_batch_get_one(&tok, 1);
        llama_decode(im->ctx, next);
    }
    out[out_len] = '\0';
    llama_sampler_free(sampler);
    return out;
}

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: stub (default — no model required)
 * ════════════════════════════════════════════════════════════════════ */
#else

typedef struct {
    char model_path[512];
    int  loaded;
    int  ctx_size;
} InferModel;

static InferModel *infer_do_load(const char *path) {
    InferModel *im = (InferModel *)calloc(1, sizeof(InferModel));
    strncpy(im->model_path, path, sizeof(im->model_path)-1);
    im->ctx_size = 2048;
    im->loaded   = 1;
    fprintf(stderr,
        "[fluxa] std.infer: stub backend — model '%s' loaded (no-op).\n"
        "  For real inference: vendor llama.cpp into vendor/llama.h + vendor/libllama.a\n"
        "  then rebuild with: make FLUXA_INFER_LLAMA=1 build\n", path);
    return im;
}

static char *infer_do_generate(InferModel *im, const char *prompt, int max_tokens) {
    (void)im; (void)max_tokens;
    /* Return a stub that makes it clear this is the no-op backend */
    char *out = (char *)malloc(256);
    snprintf(out, 256,
        "[stub] prompt received (%d chars). "
        "Build with FLUXA_INFER_LLAMA=1 for real inference.",
        (int)strlen(prompt));
    return out;
}

#endif /* FLUXA_INFER_LLAMA */

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value infer_nil(void)   { Value v; v.type=VAL_NIL;  return v; }
static inline Value infer_bool(int b) { Value v; v.type=VAL_BOOL; v.as.boolean=b; return v; }
static inline Value infer_int(long n) { Value v; v.type=VAL_INT;  v.as.integer=n; return v; }
static inline Value infer_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=strdup(s?s:""); return v; }

static inline Value infer_wrap(InferModel *im) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=im;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline InferModel *infer_unwrap(const Value *v, ErrStack *err,
                                        int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),"infer.%s: invalid model cursor",fn);
        errstack_push(err,ERR_FLUXA,eb,"infer",line); *had_error=1; return NULL; }
    return (InferModel *)v->as.dyn->items[0].as.ptr;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_infer_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define INFER_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"infer.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"infer",line); \
    *had_error=1; return infer_nil(); } while(0)

#define NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf),"infer.%s: expected %d arg(s), got %d",fn_name,(n),argc); \
    errstack_push(err,ERR_FLUXA,errbuf,"infer",line); \
    *had_error=1; return infer_nil(); } } while(0)

#define GET_STR(idx,var) \
    if(args[(idx)].type!=VAL_STRING||!args[(idx)].as.string) INFER_ERR("expected str"); \
    const char *(var)=args[(idx)].as.string;

#define GET_INT(idx,var) \
    if(args[(idx)].type!=VAL_INT) INFER_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

#define GET_MODEL(idx,var) \
    InferModel *(var)=infer_unwrap(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return infer_nil();

    if (!strcmp(fn_name,"version")) {
#ifdef FLUXA_INFER_LLAMA
        return infer_str("llama.cpp/" LLAMA_VERSION_STRING);
#else
        return infer_str("fluxa-infer/1.0 (stub — no model)");
#endif
    }

    if (!strcmp(fn_name,"load")) {
        NEED(1); GET_STR(0,path);
        InferModel *im = infer_do_load(path);
        if (!im) INFER_ERR("load: failed to load model");
        return infer_wrap(im);
    }

    if (!strcmp(fn_name,"loaded")) {
        NEED(1); GET_MODEL(0,im);
        return infer_bool(im->loaded);
    }

    if (!strcmp(fn_name,"model_name")) {
        NEED(1); GET_MODEL(0,im);
        /* Extract filename from path */
        const char *slash = strrchr(im->model_path, '/');
        return infer_str(slash ? slash+1 : im->model_path);
    }

    if (!strcmp(fn_name,"ctx_size")) {
        NEED(1); GET_MODEL(0,im);
        return infer_int(im->ctx_size);
    }

    if (!strcmp(fn_name,"generate")) {
        NEED(2); GET_MODEL(0,im); GET_STR(1,prompt);
        if (!im->loaded) INFER_ERR("generate: model not loaded");
        char *out = infer_do_generate(im, prompt, 256);
        if (!out) INFER_ERR("generate: generation failed");
        Value v; v.type=VAL_STRING; v.as.string=out;
        return v;
    }

    if (!strcmp(fn_name,"generate_n")) {
        NEED(3); GET_MODEL(0,im); GET_STR(1,prompt); GET_INT(2,n);
        if (!im->loaded) INFER_ERR("generate_n: model not loaded");
        if (n <= 0 || n > 4096) INFER_ERR("generate_n: n must be 1-4096");
        char *out = infer_do_generate(im, prompt, (int)n);
        if (!out) INFER_ERR("generate_n: generation failed");
        Value v; v.type=VAL_STRING; v.as.string=out;
        return v;
    }

    if (!strcmp(fn_name,"unload")) {
        NEED(1); GET_MODEL(0,im);
#ifdef FLUXA_INFER_LLAMA
        if (im->ctx)   { llama_free(im->ctx);        im->ctx   = NULL; }
        if (im->model) { llama_free_model(im->model); im->model = NULL; }
#endif
        im->loaded = 0;
        free(im);
        if(args[0].type==VAL_DYN&&args[0].as.dyn)
            args[0].as.dyn->items[0].as.ptr=NULL;
        return infer_nil();
    }

#undef INFER_ERR
#undef NEED
#undef GET_STR
#undef GET_INT
#undef GET_MODEL

    snprintf(errbuf,sizeof(errbuf),"infer.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"infer",line);
    *had_error=1; return infer_nil();
}

FLUXA_LIB_EXPORT(
    name      = "infer",
    toml_key  = "std.infer",
    owner     = "infer",
    call      = fluxa_std_infer_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_INFER_H */
