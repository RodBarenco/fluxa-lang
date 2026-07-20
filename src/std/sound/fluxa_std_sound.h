#ifndef FLUXA_STD_SOUND_H
#define FLUXA_STD_SOUND_H

/*
 * std.sound — audio playback and tone generation for Fluxa-lang
 *
 * Two backends:
 *
 *   FLUXA_SOUND_MINIAUDIO=1   miniaudio backend (vendor/miniaudio.h)
 *     Real audio output. miniaudio abstracts the OS audio subsystem:
 *     ALSA/PulseAudio/JACK (Linux), WASAPI (Windows), CoreAudio (macOS),
 *     OSS/sndio (BSD), AAudio/OpenSL (Android). Decodes wav/mp3/flac.
 *     Vendor the single header into vendor/miniaudio.h, then:
 *       make FLUXA_SOUND_MINIAUDIO=1 build
 *
 *   (default) stub backend
 *     API-complete, no audio device needed. Tracks engine/sound state
 *     (loaded, playing, paused, volume) so program logic — state
 *     machines, Block methods, prst patterns — is fully testable
 *     headless. load() verifies the file is readable; play/stop/pause/
 *     resume/is_playing behave consistently; tone() returns immediately
 *     (the miniaudio backend blocks for the tone duration).
 *
 * Design: opaque int handles (wserver pattern). No dyn cursors, so
 * Block methods can receive engine/sound handles as plain int params.
 *
 * API:
 *   sound.init()                       → int   engine handle
 *   sound.close(int eng)               → nil   (frees all sounds)
 *   sound.load(int eng, str path)      → int   sound handle (wav/mp3/flac)
 *   sound.unload(int eng, int h)       → nil
 *   sound.play(int eng, int h)         → bool  (always from the start)
 *   sound.stop(int eng, int h)         → nil   (rewinds)
 *   sound.pause(int eng, int h)        → nil   (keeps position)
 *   sound.resume(int eng, int h)       → nil
 *   sound.is_playing(int eng, int h)   → bool
 *   sound.volume(int eng, int h, v)    → nil   (v: float|int 0.0..1.0)
 *   sound.tone(int eng, int freq_hz, int ms) → bool  (sine beep;
 *                                        blocking on miniaudio backend)
 *   sound.version()                    → str
 *
 * All file IO (load) belongs inside danger {} in Fluxa code.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include "../../scope.h"
#include "../../err.h"

#ifdef FLUXA_SOUND_MINIAUDIO
#include <miniaudio.h>
#endif

/* ── Limits ──────────────────────────────────────────────────────── */
#define SND_MAX_ENGINES     4
#define SND_MAX_SOUNDS      64
#define SND_TONE_MAX_MS     10000

/* ── Engine state ────────────────────────────────────────────────── */
typedef struct {
#ifdef FLUXA_SOUND_MINIAUDIO
    ma_engine  engine;
    ma_sound  *sounds[SND_MAX_SOUNDS];   /* NULL = free slot */
#else
    int        loaded[SND_MAX_SOUNDS];   /* 1 = slot in use  */
    int        playing[SND_MAX_SOUNDS];
    int        paused[SND_MAX_SOUNDS];
    float      volume[SND_MAX_SOUNDS];
#endif
} SndEngine;

static SndEngine       *snd_engines[SND_MAX_ENGINES];
static pthread_mutex_t  snd_mu = PTHREAD_MUTEX_INITIALIZER;

/* ── Value constructors ──────────────────────────────────────────── */
static inline Value snd_int(long n)     { Value v; v.type = VAL_INT;   v.as.integer = n; return v; }
static inline Value snd_bool(int b)     { Value v; v.type = VAL_BOOL;  v.as.boolean = b; return v; }
static inline Value snd_nil(void)       { Value v; v.type = VAL_NIL;                     return v; }
static inline Value snd_str(const char *s) {
    Value v; v.type = VAL_STRING;
    v.as.string = fxstr_new(s ? s : "");
    return v;
}

/* ── Main dispatch function ──────────────────────────────────────── */
static inline Value fluxa_std_sound_call(const char *fn_name,
                                         const Value *args, int argc,
                                         ErrStack *err, int *had_error,
                                         int line) {
    char errbuf[280];

#define SND_ERR(...) do { \
    char _m[200]; snprintf(_m, sizeof(_m), __VA_ARGS__); \
    snprintf(errbuf, sizeof(errbuf), "sound.%s (line %d): %s", \
             fn_name, line, _m); \
    errstack_push(err, ERR_FLUXA, errbuf, "sound", line); \
    *had_error = 1; return snd_nil(); \
} while(0)

#define SND_ERR_UNLOCK(...) do { \
    pthread_mutex_unlock(&snd_mu); \
    SND_ERR(__VA_ARGS__); \
} while(0)

#define NEED(n) do { \
    if (argc < (n)) SND_ERR("expected %d argument(s), got %d", (n), argc); \
} while(0)

#define GET_INT(idx, var) \
    if (args[(idx)].type != VAL_INT) SND_ERR("expected int argument"); \
    long var = args[(idx)].as.integer;

#define GET_STR(idx, var) \
    if (args[(idx)].type != VAL_STRING || !args[(idx)].as.string) \
        SND_ERR("expected str argument"); \
    const char *var = args[(idx)].as.string;

#define GET_NUM(idx, var) \
    double var; \
    if (args[(idx)].type == VAL_FLOAT)    var = args[(idx)].as.real; \
    else if (args[(idx)].type == VAL_INT) var = (double)args[(idx)].as.integer; \
    else SND_ERR("expected float or int argument");

/* Lookup an engine while holding snd_mu. On failure: unlock + error. */
#define GET_ENGINE(hvar, evar) \
    SndEngine *evar; \
    pthread_mutex_lock(&snd_mu); \
    if ((hvar) < 1 || (hvar) > SND_MAX_ENGINES || !snd_engines[(hvar)-1]) \
        SND_ERR_UNLOCK("invalid or closed engine handle %ld", (hvar)); \
    evar = snd_engines[(hvar)-1];

/* Validate a sound slot on an already-locked engine. */
#ifdef FLUXA_SOUND_MINIAUDIO
#define SND_SLOT_OK(e, h) ((h) >= 1 && (h) <= SND_MAX_SOUNDS && (e)->sounds[(h)-1])
#else
#define SND_SLOT_OK(e, h) ((h) >= 1 && (h) <= SND_MAX_SOUNDS && (e)->loaded[(h)-1])
#endif

#define GET_SOUND_SLOT(evar, hvar) \
    if (!SND_SLOT_OK(evar, hvar)) \
        SND_ERR_UNLOCK("invalid or unloaded sound handle %ld", (hvar));

    /* ── sound.version() → str ──────────────────────────────────── */
    if (strcmp(fn_name, "version") == 0) {
#ifdef FLUXA_SOUND_MINIAUDIO
        return snd_str("miniaudio/" MA_VERSION_STRING);
#else
        return snd_str("fluxa-sound/1.0 (stub — no audio device)");
#endif
    }

    /* ── sound.init() → int engine handle ───────────────────────── */
    if (strcmp(fn_name, "init") == 0) {
        pthread_mutex_lock(&snd_mu);
        int slot = -1;
        for (int i = 0; i < SND_MAX_ENGINES; i++)
            if (!snd_engines[i]) { slot = i; break; }
        if (slot < 0)
            SND_ERR_UNLOCK("too many engines (max %d)", SND_MAX_ENGINES);

        SndEngine *e = (SndEngine *)calloc(1, sizeof(SndEngine));
        if (!e) SND_ERR_UNLOCK("out of memory");
#ifdef FLUXA_SOUND_MINIAUDIO
        if (ma_engine_init(NULL, &e->engine) != MA_SUCCESS) {
            free(e);
            SND_ERR_UNLOCK("failed to initialize audio engine");
        }
#endif
        snd_engines[slot] = e;
        pthread_mutex_unlock(&snd_mu);
        return snd_int((long)slot + 1);
    }

    /* ── sound.close(eng) → nil ─────────────────────────────────── */
    if (strcmp(fn_name, "close") == 0) {
        NEED(1); GET_INT(0, eh); GET_ENGINE(eh, e);
        for (int i = 0; i < SND_MAX_SOUNDS; i++) {
#ifdef FLUXA_SOUND_MINIAUDIO
            if (e->sounds[i]) {
                ma_sound_uninit(e->sounds[i]);
                free(e->sounds[i]);
                e->sounds[i] = NULL;
            }
#else
            e->loaded[i] = 0;
#endif
        }
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_engine_uninit(&e->engine);
#endif
        snd_engines[eh-1] = NULL;
        free(e);
        pthread_mutex_unlock(&snd_mu);
        return snd_nil();
    }

    /* ── sound.load(eng, path) → int sound handle ───────────────── */
    if (strcmp(fn_name, "load") == 0) {
        NEED(2); GET_INT(0, eh); GET_STR(1, path); GET_ENGINE(eh, e);
        int slot = -1;
        for (int i = 0; i < SND_MAX_SOUNDS; i++) {
#ifdef FLUXA_SOUND_MINIAUDIO
            if (!e->sounds[i]) { slot = i; break; }
#else
            if (!e->loaded[i]) { slot = i; break; }
#endif
        }
        if (slot < 0)
            SND_ERR_UNLOCK("too many sounds loaded (max %d)", SND_MAX_SOUNDS);

#ifdef FLUXA_SOUND_MINIAUDIO
        ma_sound *s = (ma_sound *)calloc(1, sizeof(ma_sound));
        if (!s) SND_ERR_UNLOCK("out of memory");
        if (ma_sound_init_from_file(&e->engine, path, 0, NULL, NULL, s)
                != MA_SUCCESS) {
            free(s);
            SND_ERR_UNLOCK("cannot load '%s' (missing or unsupported format)",
                           path);
        }
        e->sounds[slot] = s;
#else
        if (access(path, R_OK) != 0)
            SND_ERR_UNLOCK("cannot load '%s' (file not readable)", path);
        e->loaded[slot]  = 1;
        e->playing[slot] = 0;
        e->paused[slot]  = 0;
        e->volume[slot]  = 1.0f;
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_int((long)slot + 1);
    }

    /* ── sound.unload(eng, h) → nil ─────────────────────────────── */
    if (strcmp(fn_name, "unload") == 0) {
        NEED(2); GET_INT(0, eh); GET_INT(1, sh);
        GET_ENGINE(eh, e); GET_SOUND_SLOT(e, sh);
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_sound_uninit(e->sounds[sh-1]);
        free(e->sounds[sh-1]);
        e->sounds[sh-1] = NULL;
#else
        e->loaded[sh-1] = 0;
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_nil();
    }

    /* ── sound.play(eng, h) → bool (from the start) ─────────────── */
    if (strcmp(fn_name, "play") == 0) {
        NEED(2); GET_INT(0, eh); GET_INT(1, sh);
        GET_ENGINE(eh, e); GET_SOUND_SLOT(e, sh);
        int ok = 1;
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_sound_seek_to_pcm_frame(e->sounds[sh-1], 0);
        ok = (ma_sound_start(e->sounds[sh-1]) == MA_SUCCESS);
#else
        e->playing[sh-1] = 1;
        e->paused[sh-1]  = 0;
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_bool(ok);
    }

    /* ── sound.stop(eng, h) → nil (rewinds) ─────────────────────── */
    if (strcmp(fn_name, "stop") == 0) {
        NEED(2); GET_INT(0, eh); GET_INT(1, sh);
        GET_ENGINE(eh, e); GET_SOUND_SLOT(e, sh);
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_sound_stop(e->sounds[sh-1]);
        ma_sound_seek_to_pcm_frame(e->sounds[sh-1], 0);
#else
        e->playing[sh-1] = 0;
        e->paused[sh-1]  = 0;
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_nil();
    }

    /* ── sound.pause(eng, h) → nil (keeps position) ─────────────── */
    if (strcmp(fn_name, "pause") == 0) {
        NEED(2); GET_INT(0, eh); GET_INT(1, sh);
        GET_ENGINE(eh, e); GET_SOUND_SLOT(e, sh);
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_sound_stop(e->sounds[sh-1]);
#else
        if (e->playing[sh-1]) { e->playing[sh-1] = 0; e->paused[sh-1] = 1; }
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_nil();
    }

    /* ── sound.resume(eng, h) → nil ─────────────────────────────── */
    if (strcmp(fn_name, "resume") == 0) {
        NEED(2); GET_INT(0, eh); GET_INT(1, sh);
        GET_ENGINE(eh, e); GET_SOUND_SLOT(e, sh);
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_sound_start(e->sounds[sh-1]);
#else
        if (e->paused[sh-1]) { e->paused[sh-1] = 0; e->playing[sh-1] = 1; }
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_nil();
    }

    /* ── sound.is_playing(eng, h) → bool ────────────────────────── */
    if (strcmp(fn_name, "is_playing") == 0) {
        NEED(2); GET_INT(0, eh); GET_INT(1, sh);
        GET_ENGINE(eh, e); GET_SOUND_SLOT(e, sh);
        int p;
#ifdef FLUXA_SOUND_MINIAUDIO
        p = ma_sound_is_playing(e->sounds[sh-1]) ? 1 : 0;
#else
        p = e->playing[sh-1];
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_bool(p);
    }

    /* ── sound.volume(eng, h, v) → nil (v in 0.0..1.0) ──────────── */
    if (strcmp(fn_name, "volume") == 0) {
        NEED(3); GET_INT(0, eh); GET_INT(1, sh); GET_NUM(2, vol);
        if (vol < 0.0 || vol > 1.0)
            SND_ERR("volume %.2f out of range 0.0..1.0", vol);
        GET_ENGINE(eh, e); GET_SOUND_SLOT(e, sh);
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_sound_set_volume(e->sounds[sh-1], (float)vol);
#else
        e->volume[sh-1] = (float)vol;
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_nil();
    }

    /* ── sound.tone(eng, freq_hz, ms) → bool ────────────────────── *
     * Sine beep. Blocking on the miniaudio backend (returns after   *
     * the tone finishes); immediate no-op on the stub.              */
    if (strcmp(fn_name, "tone") == 0) {
        NEED(3); GET_INT(0, eh); GET_INT(1, freq); GET_INT(2, ms);
        if (freq < 1 || freq > 20000)
            SND_ERR("frequency %ld out of range 1..20000 Hz", freq);
        if (ms < 0 || ms > SND_TONE_MAX_MS)
            SND_ERR("duration %ld out of range 0..%d ms", ms, SND_TONE_MAX_MS);
        GET_ENGINE(eh, e);
#ifdef FLUXA_SOUND_MINIAUDIO
        ma_waveform_config wc = ma_waveform_config_init(
            ma_format_f32, 2,
            ma_engine_get_sample_rate(&e->engine),
            ma_waveform_type_sine, 0.4, (double)freq);
        ma_waveform wave;
        if (ma_waveform_init(&wc, &wave) != MA_SUCCESS)
            SND_ERR_UNLOCK("failed to create waveform");
        ma_sound tone_snd;
        if (ma_sound_init_from_data_source(&e->engine, &wave, 0, NULL,
                                           &tone_snd) != MA_SUCCESS) {
            ma_waveform_uninit(&wave);
            SND_ERR_UNLOCK("failed to create tone");
        }
        ma_sound_start(&tone_snd);
        struct timespec ts;
        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        ma_sound_stop(&tone_snd);
        ma_sound_uninit(&tone_snd);
        ma_waveform_uninit(&wave);
#else
        (void)e;
#endif
        pthread_mutex_unlock(&snd_mu);
        return snd_bool(1);
    }

#undef SND_ERR
#undef SND_ERR_UNLOCK
#undef NEED
#undef GET_INT
#undef GET_STR
#undef GET_NUM
#undef GET_ENGINE
#undef SND_SLOT_OK
#undef GET_SOUND_SLOT

    snprintf(errbuf, sizeof(errbuf), "sound.%s: unknown function", fn_name);
    errstack_push(err, ERR_FLUXA, errbuf, "sound", line);
    *had_error = 1;
    return snd_nil();
}

/* ── Lib descriptor — read by scripts/gen_lib_registry.py ────────── */
FLUXA_LIB_EXPORT(
    name      = "sound",
    toml_key  = "std.sound",
    owner     = "sound",
    call      = fluxa_std_sound_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_SOUND_H */
