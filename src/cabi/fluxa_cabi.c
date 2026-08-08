#define _POSIX_C_SOURCE 200809L
#define FLUXA_CABI_BUILD 1

#include "fluxa_cabi.h"
#include "fluxa_cabi_context.h"

#include "../parser.h"
#include "../pool.h"
#include "../resolver.h"
#include "../runtime.h"
#include "../block.h"
#include "../fluxa_ffi.h"
#include "../toml_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef CRITICAL_SECTION cabi_mutex;
static void cabi_mutex_init(cabi_mutex *m)    { InitializeCriticalSection(m); }
static void cabi_mutex_destroy(cabi_mutex *m) { DeleteCriticalSection(m); }
static void cabi_mutex_lock(cabi_mutex *m)    { EnterCriticalSection(m); }
static void cabi_mutex_unlock(cabi_mutex *m)  { LeaveCriticalSection(m); }
static SRWLOCK g_instance_lock = SRWLOCK_INIT;
static void cabi_global_lock(void)   { AcquireSRWLockExclusive(&g_instance_lock); }
static void cabi_global_unlock(void) { ReleaseSRWLockExclusive(&g_instance_lock); }
#else
#  include <pthread.h>
typedef pthread_mutex_t cabi_mutex;
static void cabi_mutex_init(cabi_mutex *m)    { (void)pthread_mutex_init(m, NULL); }
static void cabi_mutex_destroy(cabi_mutex *m) { (void)pthread_mutex_destroy(m); }
static void cabi_mutex_lock(cabi_mutex *m)    { (void)pthread_mutex_lock(m); }
static void cabi_mutex_unlock(cabi_mutex *m)  { (void)pthread_mutex_unlock(m); }
static pthread_mutex_t g_instance_lock = PTHREAD_MUTEX_INITIALIZER;
static void cabi_global_lock(void)   { (void)pthread_mutex_lock(&g_instance_lock); }
static void cabi_global_unlock(void) { (void)pthread_mutex_unlock(&g_instance_lock); }
#endif

/*
 * Important architectural rule:
 * This translation unit is the ONLY place where the stable ABI touches Fluxa
 * internals. Host applications see only fluxa_cabi.h.
 */

/*
 * The current Fluxa runtime still owns a process-global Block registry.
 * Until that registry becomes Runtime-owned, C ABI v1 deliberately allows
 * one live embedded runtime per process. This prevents cross-instance state
 * aliasing while preserving an opaque ABI that can support N runtimes later.
 */
static int g_instance_live = 0;

struct fluxa_cabi_runtime {
    Runtime rt;
    ASTPool ast_pool;
    ASTNode *program;
    FluxaConfig config;
    cabi_mutex lock;
    uint32_t max_payload;
    char dispatch_fn[128];
    unsigned char *response_buf;
    uint32_t response_cap;
};

/* ── error helpers ───────────────────────────────────────────────────────── */

static void cabi_error_clear(fluxa_cabi_error *e) {
    if (e) memset(e, 0, sizeof(*e));
}
static int cabi_fail(fluxa_cabi_error *e, int code, int line,
                     const char *context, const char *message) {
    if (e) {
        memset(e, 0, sizeof(*e));
        e->code = (uint32_t)code;
        e->line = line;
        snprintf(e->context, sizeof(e->context), "%s", context ? context : "cabi");
        snprintf(e->message, sizeof(e->message), "%s", message ? message : "");
    }
    return code;
}
static int cabi_fail_runtime(Runtime *rt, fluxa_cabi_error *e, const char *fallback) {
    const ErrEntry *ee = rt ? errstack_get(&rt->err_stack, 0) : NULL;
    if (ee)
        return cabi_fail(e, FLUXA_CABI_ERUNTIME, ee->line, ee->context, ee->message);
    return cabi_fail(e, FLUXA_CABI_ERUNTIME,
                     rt ? rt->current_line : 0, "runtime", fallback);
}


/* Project-root resolution for live/static modules. */
static void cabi_dirname(const char *path, char *out, size_t out_cap) {
    const char *a, *b, *cut;
    size_t n;
    if (!out || out_cap == 0) return;
    out[0] = '\0';
    if (!path || !path[0]) { snprintf(out, out_cap, "."); return; }
    a = strrchr(path, '/');
    b = strrchr(path, '\\');
    cut = (!a) ? b : (!b ? a : (a > b ? a : b));
    if (!cut) { snprintf(out, out_cap, "."); return; }
    n = (size_t)(cut - path);
    if (n == 0) n = 1;
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}
static int cabi_path_is_absolute(const char *p) {
    if (!p || !p[0]) return 0;
#if defined(_WIN32)
    if ((p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/') return 1;
#else
    if (p[0] == '/') return 1;
#endif
    return 0;
}
static void cabi_resolve_module_root(
    const char *entry_path, const char *configured, char *out, size_t out_cap)
{
    char project_dir[1024];
    cabi_dirname(entry_path, project_dir, sizeof(project_dir));
    if (!configured || !configured[0]) {
        snprintf(out, out_cap, "%s", project_dir);
    } else if (cabi_path_is_absolute(configured)) {
        snprintf(out, out_cap, "%s", configured);
    } else if (strcmp(project_dir, ".") == 0) {
        snprintf(out, out_cap, "%s", configured);
    } else {
#if defined(_WIN32)
        snprintf(out, out_cap, "%s\\%s", project_dir, configured);
#else
        snprintf(out, out_cap, "%s/%s", project_dir, configured);
#endif
    }
}

/* ── project parser (same module ordering contract as CLI parse_file_ex) ── */

static char *cabi_load_file(const char *path) {
    FILE *f;
    long size;
    size_t got;
    char *buf;
    if (!path) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)size + 1u);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static ASTNode *cabi_parse_file_ex(
    const char *path, ASTPool *pool,
    const char *mod_root, int module_cap)
{
    typedef struct { char ns[64]; char *src; } ModEntry;
    char *main_src = cabi_load_file(path);
    ModEntry *mods = NULL;
    int mod_count = 0;
    int mod_overflow = 0;
    Parser main_p;
    ASTNode *program;
    ASTNode *main_prog;

    if (!main_src) return NULL;
    pool_init(pool);

    if (module_cap < 1) module_cap = 32;
    mods = (ModEntry *)calloc((size_t)module_cap, sizeof(ModEntry));
    if (!mods) { free(main_src); pool_free(pool); return NULL; }

    {
        const char *scan = main_src;
        while (*scan) {
            while (*scan == ' ' || *scan == '\t') scan++;
            if (strncmp(scan, "import", 6) == 0) {
                const char *s = scan + 6;
                int is_live, is_static;
                while (*s == ' ' || *s == '\t') s++;
                is_live = (strncmp(s, "live", 4) == 0 && (s[4] == ' ' || s[4] == '\t'));
                is_static = (strncmp(s, "static", 6) == 0 && (s[6] == ' ' || s[6] == '\t'));
                if (is_live || is_static) {
                    char ns[64] = {0};
                    int ni = 0;
                    char fpath[1024];
                    const char *kind;
                    s += is_live ? 4 : 6;
                    while (*s == ' ' || *s == '\t') s++;
                    while (*s && *s != ' ' && *s != '\t' &&
                           *s != '\n' && *s != '\r' && ni < 63)
                        ns[ni++] = *s++;
                    if (ni > 0) {
                        if (mod_count >= module_cap) { mod_overflow = 1; break; }
                        kind = is_live ? "live" : "static";
                        if (mod_root && mod_root[0])
                            snprintf(fpath, sizeof(fpath), "%s/%s/%s.flx", mod_root, kind, ns);
                        else
                            snprintf(fpath, sizeof(fpath), "%s/%s.flx", kind, ns);
                        mods[mod_count].src = cabi_load_file(fpath);
                        if (!mods[mod_count].src) {
                            int i;
                            for (i = 0; i < mod_count; i++) free(mods[i].src);
                            free(mods); free(main_src); pool_free(pool);
                            return NULL;
                        }
                        snprintf(mods[mod_count].ns, sizeof(mods[mod_count].ns), "%.63s", ns);
                        mod_count++;
                    }
                }
            }
            while (*scan && *scan != '\n') scan++;
            if (*scan == '\n') scan++;
        }
    }

    if (mod_overflow) {
        int i;
        for (i = 0; i < mod_count; i++) free(mods[i].src);
        free(mods); free(main_src); pool_free(pool);
        return NULL;
    }

    main_p = parser_new(main_src, pool);
    program = pool_alloc_node(pool);
    if (!program) {
        parser_free(&main_p); free(mods); free(main_src); pool_free(pool); return NULL;
    }
    program->type = NODE_PROGRAM;
    program->as.list.children = NULL;
    program->as.list.count = 0;

    {
        int i;
        for (i = 0; i < mod_count; i++) {
            int rc = parser_parse_module(&main_p, program, mods[i].ns, mods[i].src);
            free(mods[i].src);
            if (rc != 0) {
                int j;
                for (j = i + 1; j < mod_count; j++) free(mods[j].src);
                free(mods); free(main_src); parser_free(&main_p); pool_free(pool);
                return NULL;
            }
        }
    }
    free(mods);

    main_prog = parser_parse(&main_p);
    free(main_src);
    if (!main_prog) {
        parser_free(&main_p); pool_free(pool); return NULL;
    }
    {
        int i;
        for (i = 0; i < main_prog->as.list.count; i++)
            ast_list_push(program, main_prog->as.list.children[i]);
    }
    parser_free(&main_p);
    return program;
}

/* ── Runtime lifecycle ───────────────────────────────────────────────────── */

static void cabi_runtime_zero(Runtime *rt, FluxaConfig config, FluxaMode mode) {
    int i;
    memset(rt, 0, sizeof(*rt));
    rt->scope            = scope_new();
    rt->global_table     = NULL;
    rt->stack_size       = 0;
    rt->had_error        = 0;
    rt->call_depth       = 0;
    rt->ret.active       = 0;
    rt->ret.tco_active   = 0;
    rt->ret.tco_fn       = NULL;
    rt->ret.tco_args     = NULL;
    rt->ret.value.type   = VAL_NIL;
    rt->current_instance = NULL;
    rt->current_thread   = NULL;
    rt->danger_depth     = 0;
    rt->cycle_count      = 0;
    rt->dry_run          = 0;
    rt->current_line     = 0;
    rt->cancel_flag      = NULL;
    rt->mode             = mode;
    rt->shared_prst_pool = NULL;
    rt->config           = config;
    errstack_clear(&rt->err_stack);
    gc_init(&rt->gc, config.gc_cap);
    ffi_registry_init(&rt->ffi);
    warm_profile_init(&rt->warm, config.warm_func_cap);
    rt->warm.enabled = 1;
    rt->current_fn = NULL;
    rt->current_wf = NULL;
    ffi_load_from_config(&rt->ffi, &rt->err_stack, &config);

    if (mode == FLUXA_MODE_PROJECT) {
        prst_pool_init(&rt->prst_pool);
        if (config.prst_cap != PRST_POOL_INIT_CAP && config.prst_cap > 0) {
            PrstEntry *ne = (PrstEntry *)realloc(
                rt->prst_pool.entries, sizeof(PrstEntry) * (size_t)config.prst_cap);
            if (ne) {
                rt->prst_pool.entries = ne;
                rt->prst_pool.cap = config.prst_cap;
            }
        }
        prst_graph_init_cap(&rt->prst_graph, config.prst_graph_cap);
    } else {
        rt->prst_pool.entries = NULL;
        rt->prst_pool.count = 0;
        rt->prst_pool.cap = 0;
        rt->prst_graph.deps = NULL;
        rt->prst_graph.count = 0;
        rt->prst_graph.cap = 0;
    }
    for (i = 0; i < FLUXA_STACK_SIZE; i++) rt->stack[i].type = VAL_NIL;
}

static void cabi_runtime_destroy(Runtime *rt) {
    if (!rt) return;
    scope_free(&rt->scope);
    scope_table_free(&rt->global_table);
    block_registry_free();
    gc_collect_all(&rt->gc, gc_dyn_free_fn);
    if (rt->mode == FLUXA_MODE_PROJECT) {
        prst_pool_free(&rt->prst_pool);
        prst_graph_free(&rt->prst_graph);
    }
    ffi_registry_free(&rt->ffi);
    warm_profile_free(&rt->warm);
    memset(rt, 0, sizeof(*rt));
}

/* Build a zero-argument function call AST node on the stack.
 * runtime_eval() only reads it for the duration of the call. */
static Value cabi_call_dispatch(Runtime *rt, const char *name) {
    ASTNode call;
    memset(&call, 0, sizeof(call));
    call.type = NODE_FUNC_CALL;
    call.resolved_offset = -1;
    call.line = 0;
    call.as.list.name = (char *)name;
    call.as.list.children = NULL;
    call.as.list.count = 0;
    return runtime_eval(rt, &call);
}

/* ── Public C ABI ────────────────────────────────────────────────────────── */

uint32_t fluxa_cabi_abi_version(void) { return FLUXA_CABI_ABI_VERSION; }
const char *fluxa_cabi_version_string(void) { return "fluxa-cabi/1.0.0"; }

int fluxa_cabi_open(const fluxa_cabi_config *config,
                    fluxa_cabi_runtime **out_runtime,
                    fluxa_cabi_error *error)
{
    fluxa_cabi_runtime *h;
    FluxaMode mode;
    char module_root[1024];
    int slots, rc;

    cabi_error_clear(error);
    if (!out_runtime) return cabi_fail(error, FLUXA_CABI_EINVAL, 0, "cabi", "out_runtime is NULL");
    *out_runtime = NULL;
    if (!config || config->struct_size < sizeof(fluxa_cabi_config))
        return cabi_fail(error, FLUXA_CABI_EABI, 0, "cabi", "invalid fluxa_cabi_config.struct_size");
    if ((config->abi_version >> 16) != FLUXA_CABI_ABI_MAJOR)
        return cabi_fail(error, FLUXA_CABI_EABI, 0, "cabi", "C ABI major version mismatch");
    if (!config->entry_path || !config->entry_path[0])
        return cabi_fail(error, FLUXA_CABI_EINVAL, 0, "cabi", "entry_path is required");
    if (config->flags != 0)
        return cabi_fail(error, FLUXA_CABI_EABI, 0, "cabi", "ABI v1 requires config.flags == 0");

    cabi_global_lock();
    if (g_instance_live) {
        cabi_global_unlock();
        return cabi_fail(error, FLUXA_CABI_EBUSY, 0, "cabi",
                         "C ABI v1 currently allows one embedded Fluxa runtime per process");
    }
    g_instance_live = 1;
    cabi_global_unlock();

    h = (fluxa_cabi_runtime *)calloc(1, sizeof(*h));
    if (!h) {
        cabi_global_lock(); g_instance_live = 0; cabi_global_unlock();
        return cabi_fail(error, FLUXA_CABI_ENOMEM, 0, "cabi", "out of memory allocating runtime");
    }

    if (config->config_path && config->config_path[0]) {
        h->config = fluxa_config_load(config->config_path);
        fluxa_config_load_libs(&h->config, config->config_path);
    } else {
        h->config = fluxa_config_defaults();
    }
    resolver_set_scope_cap(h->config.scope_cap);
    parser_set_module_cap(h->config.module_cap);
    pool_set_node_cap(h->config.ast_pool_cap);
    pool_set_str_cap(h->config.ast_str_pool_cap);

    cabi_resolve_module_root(config->entry_path,
        h->config.module_root[0] ? h->config.module_root : NULL,
        module_root, sizeof(module_root));

    h->program = cabi_parse_file_ex(config->entry_path, &h->ast_pool,
                                    module_root, h->config.module_cap);
    if (!h->program) {
        free(h); cabi_global_lock(); g_instance_live = 0; cabi_global_unlock();
        return cabi_fail(error, FLUXA_CABI_EPARSE, 0, "parser",
                         "failed to parse entry file or one of its modules");
    }
    slots = resolver_run(h->program);
    if (slots < 0) {
        pool_free(&h->ast_pool); free(h);
        cabi_global_lock(); g_instance_live = 0; cabi_global_unlock();
        return cabi_fail(error, FLUXA_CABI_ERESOLVE, 0, "resolver", "name resolution failed");
    }

    /* The bridge does not require prst. If the program itself contains prst on
     * a target that supports it, the ordinary runtime decides its mode. */
    mode = resolver_has_prst(h->program) ? FLUXA_MODE_PROJECT : FLUXA_MODE_SCRIPT;
    cabi_runtime_zero(&h->rt, h->config, mode);
    cabi_mutex_init(&h->lock);
    h->max_payload = config->max_frame_bytes ? config->max_frame_bytes : FLUXA_CABI_MAX_FRAME_DEFAULT;
    if (h->max_payload < 1024u) h->max_payload = 1024u;
    snprintf(h->dispatch_fn, sizeof(h->dispatch_fn), "%s",
             (config->dispatch_fn && config->dispatch_fn[0]) ? config->dispatch_fn : "cabi_dispatch");

    rc = runtime_exec_with_rt(&h->rt, h->program);
    if (rc != 0) {
        cabi_fail_runtime(&h->rt, error, "initial Fluxa execution failed");
        cabi_runtime_destroy(&h->rt); pool_free(&h->ast_pool);
        cabi_mutex_destroy(&h->lock); free(h);
        cabi_global_lock(); g_instance_live = 0; cabi_global_unlock();
        return FLUXA_CABI_ERUNTIME;
    }
    *out_runtime = h;
    return FLUXA_CABI_OK;
}

void fluxa_cabi_close(fluxa_cabi_runtime *runtime) {
    if (!runtime) return;
    cabi_mutex_lock(&runtime->lock);
    fluxa_cabi_ctx_unbind();
    free(runtime->response_buf);
    runtime->response_buf = NULL;
    runtime->response_cap = 0;
    cabi_runtime_destroy(&runtime->rt);
    pool_free(&runtime->ast_pool);
    cabi_mutex_unlock(&runtime->lock);
    cabi_mutex_destroy(&runtime->lock);
    free(runtime);
    cabi_global_lock(); g_instance_live = 0; cabi_global_unlock();
}

int fluxa_cabi_exchange(fluxa_cabi_runtime *runtime,
                        const fluxa_cabi_view *request,
                        fluxa_cabi_view *response,
                        fluxa_cabi_error *error)
{
    fluxa_cabi_exchange_ctx ctx;
    Value ret;

    cabi_error_clear(error);
    if (!runtime || !request || !response)
        return cabi_fail(error, FLUXA_CABI_EINVAL, 0, "cabi", "runtime, request and response are required");
    if (request->size > runtime->max_payload)
        return cabi_fail(error, FLUXA_CABI_EOVERSIZE, 0, "cabi", "request frame exceeds configured limit");
    if (!fluxa_cabi_view_validate(request))
        return cabi_fail(error, FLUXA_CABI_EWIRE, 0, "cabi", "request is not a valid deterministic FXCB frame");

    cabi_mutex_lock(&runtime->lock);
    memset(&ctx, 0, sizeof(ctx));
    ctx.request = *request;
    ctx.max_frame = runtime->max_payload;
    ctx.response.data = runtime->response_buf;
    ctx.response.capacity = runtime->response_cap;
    ctx.response.size = 0;

    runtime->rt.had_error = 0;
    errstack_clear(&runtime->rt.err_stack);
    fluxa_cabi_ctx_bind(&ctx);
    ret = cabi_call_dispatch(&runtime->rt, runtime->dispatch_fn);
    (void)ret;
    fluxa_cabi_ctx_unbind();

    runtime->response_buf = (unsigned char *)ctx.response.data;
    runtime->response_cap = ctx.response.capacity;

    if (runtime->rt.had_error) {
        int out = cabi_fail_runtime(&runtime->rt, error, "Fluxa dispatch failed");
        runtime->rt.had_error = 0;
        errstack_clear(&runtime->rt.err_stack);
        cabi_mutex_unlock(&runtime->lock);
        return out;
    }
    if (ctx.response.size == 0) {
        /* Empty response is represented by a valid zero-value frame. */
        fluxa_cabi_message_reset(&ctx.response);
        if (!fluxa_cabi_add_bool(&ctx.response, 1)) {
            cabi_mutex_unlock(&runtime->lock);
            return cabi_fail(error, FLUXA_CABI_ENOMEM, 0, "cabi", "could not materialize response frame");
        }
        /* Remove the placeholder item while retaining the initialized header. */
        ((unsigned char *)ctx.response.data)[8]=0;
        ((unsigned char *)ctx.response.data)[9]=0;
        ((unsigned char *)ctx.response.data)[10]=0;
        ((unsigned char *)ctx.response.data)[11]=0;
        ctx.response.size=12;
        runtime->response_buf=(unsigned char*)ctx.response.data;
        runtime->response_cap=ctx.response.capacity;
    }
    if (ctx.response.size > runtime->max_payload ||
        !fluxa_cabi_view_validate(&(fluxa_cabi_view){ctx.response.data,ctx.response.size})) {
        cabi_mutex_unlock(&runtime->lock);
        return cabi_fail(error, FLUXA_CABI_EWIRE, 0, "cabi", "Fluxa produced an invalid response frame");
    }
    response->data = runtime->response_buf;
    response->size = ctx.response.size;
    cabi_mutex_unlock(&runtime->lock);
    return FLUXA_CABI_OK;
}

int fluxa_cabi_exchange_sealed(fluxa_cabi_runtime *runtime,
                               const uint8_t key[FLUXA_CABI_KEY_BYTES],
                               const fluxa_cabi_view *sealed_request,
                               fluxa_cabi_message *sealed_response,
                               fluxa_cabi_error *error)
{
    fluxa_cabi_message clear_request = {0};
    fluxa_cabi_view clear_view, clear_response;
    int rc;
    if (!runtime || !key || !sealed_request || !sealed_response)
        return cabi_fail(error, FLUXA_CABI_EINVAL, 0, "cabi.security", "invalid secure exchange argument");
    fluxa_cabi_message_init(&clear_request);
    rc = fluxa_cabi_unseal(key, sealed_request, &clear_request, error);
    if (rc != FLUXA_CABI_OK) return rc;
    clear_view.data = clear_request.data;
    clear_view.size = clear_request.size;
    rc = fluxa_cabi_exchange(runtime, &clear_view, &clear_response, error);
    fluxa_cabi_message_free(&clear_request);
    if (rc != FLUXA_CABI_OK) return rc;
    return fluxa_cabi_seal(key, &clear_response, sealed_response, error);
}
