/* fluxa_video_enc.c — minih264e implementation, in its own translation unit.
 *
 * Separate from fluxa_video_mux.c because minimp4 and minih264e both define
 * `bs_t` and `nal_put_esc`.
 *
 * H264E_MAX_THREADS = 0 keeps the encoder single-threaded: std.video is called
 * from Fluxa code that may itself already be inside a worker, and an encoder
 * spawning its own pool underneath would make the thread count unpredictable.
 * Encoding a frame is fast enough that the caller can drive it directly.
 *
 * Upstream: https://github.com/lieff/minih264 — CC0 (public domain).
 * See LICENSE.minih264 in this directory.
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

#define H264E_MAX_THREADS 0
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"
