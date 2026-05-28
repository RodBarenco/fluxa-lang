/* fluxa_std_wserver.c — non-static entry point for std.wserver.
 *
 * The entire implementation lives in fluxa_std_wserver.h (static functions).
 * This TU provides the single non-static fluxa_std_wserver_call that the
 * linker sees exactly once — matching the extern declaration in lib_registry_gen.h.
 */
#include "fluxa_std_wserver.h"

#ifdef FLUXA_WSERVER_MHD
Value fluxa_std_wserver_call(const char *fn_name,
                              const Value *args, int argc,
                              ErrStack *err, int *had_error,
                              int line, void *rt_ptr) {
    return ws_dispatch(fn_name, args, argc, err, had_error, line, rt_ptr);
}
#else
Value fluxa_std_wserver_call(const char *fn_name,
                              const Value *args, int argc,
                              ErrStack *err, int *had_error,
                              int line, void *rt_ptr) {
    return ws_stub_dispatch(fn_name, args, argc, err, had_error, line, rt_ptr);
}
#endif
