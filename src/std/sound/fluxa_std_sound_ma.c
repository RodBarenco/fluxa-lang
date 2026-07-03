/* fluxa_std_sound_ma.c — miniaudio implementation translation unit.
 *
 * miniaudio is a single-header library: exactly one TU must define
 * MINIAUDIO_IMPLEMENTATION. The lib header (fluxa_std_sound.h) is
 * included from lib_registry_gen.h into multiple TUs (parser.c,
 * runtime.c), so the implementation lives here, added to the build
 * by src/std/sound/lib.mk only when FLUXA_SOUND_MINIAUDIO=1.
 *
 * Vendored third-party code: diagnostics relaxed for this TU only —
 * the zero-warning policy applies to Fluxa's own sources.
 */
#ifdef FLUXA_SOUND_MINIAUDIO

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING          /* playback only — no wav writing */
#include <miniaudio.h>

#pragma GCC diagnostic pop

#else
/* ISO C forbids an empty translation unit */
typedef int fluxa_std_sound_ma_unused;
#endif
