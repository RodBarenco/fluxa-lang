/* fluxa_video_mux.c — minimp4 implementation, in its own translation unit.
 *
 * minimp4 and minih264e both define `bs_t` and `nal_put_esc`. Putting the two
 * implementations in the same translation unit is a compile error, so each gets
 * its own .c and std.video only ever sees the headers' declarations.
 *
 * Upstream: https://github.com/lieff/minimp4 — CC0 (public domain).
 * See LICENSE.minimp4 in this directory.
 */
#if defined(__GNUC__)
#  pragma GCC diagnostic ignored "-Wall"
#  pragma GCC diagnostic ignored "-Wextra"
#  pragma GCC diagnostic ignored "-Wpedantic"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#  pragma GCC diagnostic ignored "-Wchar-subscripts"
#  pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#  pragma GCC diagnostic ignored "-Wmisleading-indentation"
#  pragma GCC diagnostic ignored "-Wtype-limits"
#  pragma GCC diagnostic ignored "-Woverflow"
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"
