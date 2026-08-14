/* fluxa_video_dec.c — h264bsd decoder, built as a single translation unit.
 *
 * The 26 upstream .c files are included here rather than compiled separately
 * for one reason: warning isolation. Fluxa builds with -Wall -Wextra -pedantic
 * and treats a warning-free build as a hard gate, but third-party code is not
 * ours to rewrite. Pulling it into one unit lets the pragma below silence it
 * here and nowhere else, so the gate keeps meaning what it says for our own
 * sources. It also cuts the per-file compile overhead.
 *
 * Upstream: https://github.com/oneam/h264bsd — Apache-2.0, from AOSP.
 * See LICENSE.h264bsd in this directory.
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

#include "h264bsd_byte_stream.c"
#include "h264bsd_cavlc.c"
#include "h264bsd_conceal.c"
#include "h264bsd_deblocking.c"
#include "h264bsd_decoder.c"
#include "h264bsd_dpb.c"
#include "h264bsd_image.c"
#include "h264bsd_inter_prediction.c"
#include "h264bsd_intra_prediction.c"
#include "h264bsd_macroblock_layer.c"
#include "h264bsd_nal_unit.c"
#include "h264bsd_neighbour.c"
#include "h264bsd_pic_order_cnt.c"
#include "h264bsd_pic_param_set.c"
#include "h264bsd_reconstruct.c"
#include "h264bsd_sei.c"
#include "h264bsd_seq_param_set.c"
#include "h264bsd_slice_data.c"
#include "h264bsd_slice_group_map.c"
#include "h264bsd_slice_header.c"
#include "h264bsd_storage.c"
#include "h264bsd_stream.c"
#include "h264bsd_transform.c"
#include "h264bsd_util.c"
#include "h264bsd_vlc.c"
#include "h264bsd_vui.c"
