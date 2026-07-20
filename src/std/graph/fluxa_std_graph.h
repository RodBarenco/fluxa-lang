#ifndef FLUXA_STD_GRAPH_H
#define FLUXA_STD_GRAPH_H

/*
 * std.graph — 2D/3D graphics for Fluxa-lang
 *
 * Two backends:
 *
 *   FLUXA_GRAPH_RAYLIB=1   Raylib backend (requires raylib.h + libraylib)
 *     Full hardware-accelerated 2D/3D. Works on Linux, macOS, Windows,
 *     Raspberry Pi. Vendor raylib into vendor/raylib.h + vendor/libraylib.a,
 *     then: make FLUXA_GRAPH_RAYLIB=1 build
 *
 *   (default) stub backend
 *     API-complete, no-op rendering. Useful for testing game logic,
 *     state machines, and prst patterns without a display.
 *     Returns sensible values (ok=true, window=dyn cursor, etc.).
 *
 * API:
 *   graph.init(width, height, title)  → dyn window cursor
 *   graph.close(win)                  → nil
 *   graph.should_close(win)           → bool
 *   graph.begin_frame(win)            → nil
 *   graph.end_frame(win)              → nil
 *   graph.clear(win, r, g, b)         → nil   (RGB 0-255)
 *   graph.fps(win)                    → int
 *   graph.set_fps(win, fps)           → nil
 *   graph.fullscreen(win)             → bool  (alterna tela cheia; retorna o estado novo)
 *   graph.draw_rect(win, x, y, w, h, r, g, b)          → nil
 *   graph.draw_circle(win, x, y, radius, r, g, b)       → nil
 *   graph.draw_line(win, x1, y1, x2, y2, r, g, b)      → nil
 *   graph.draw_text(win, text, x, y, size, r, g, b)     → nil
 *   graph.load_font(win, path, size)  → dyn font cursor  (TTF/OTF, incl. Latin-1 accents)
 *   graph.draw_text_font(win, font, text, x, y, size, r, g, b) → nil
 *   graph.text_width(win, font, text, size)             → int  (pixels, for layout)
 *   graph.unload_font(win, font)      → nil
 *   graph.key_pressed(win, key)       → bool  (key: "SPACE", "A"-"Z", etc.)
 *   graph.key_down(win, key)          → bool
 *   graph.mouse_x(win)                → int
 *   graph.mouse_y(win)                → int
 *   graph.mouse_pressed(win)          → bool  (left button)
 *   graph.dt(win)                     → float (delta time seconds)
 *   graph.version()                   → str
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../scope.h"
#include "../../err.h"

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: Raylib (when FLUXA_GRAPH_RAYLIB=1)
 * ════════════════════════════════════════════════════════════════════ */
#ifdef FLUXA_GRAPH_RAYLIB

#include <raylib.h>

typedef struct {
    int width, height;       /* logical (design) resolution — game draws here */
    int fps_target;
    RenderTexture2D target;  /* fixed-size offscreen buffer for scaled output */
    int has_target;          /* 1 once the render texture is created */
} GraphWin;

static GraphWin *graph_new_win(int w, int h, const char *title) {
    GraphWin *win = (GraphWin *)calloc(1, sizeof(GraphWin));
    win->width = w; win->height = h; win->fps_target = 60;
    InitWindow(w, h, title);
    SetExitKey(KEY_NULL);   /* ESC must reach the program (quit-confirm UIs),
                             * not silently close the window (raylib default) */
    SetTargetFPS(60);
    /* offscreen buffer at the logical resolution; end_frame blits it scaled
     * and letterboxed to the real window, so fullscreen enlarges the game
     * proportionally instead of leaving it in the top-left corner. */
    win->target = LoadRenderTexture(w, h);
    SetTextureFilter(win->target.texture, TEXTURE_FILTER_BILINEAR);
    win->has_target = 1;
    return win;
}

typedef struct {
    Font font;
    int  base_size;   /* size the glyph atlas was rasterized at */
    int  loaded;      /* 1 = owns GPU texture; 0 = fell back to default font */
} GraphFont;

/* Load a TTF/OTF at base_size with ASCII 32-126 + Latin-1 160-255
 * (covers Portuguese and Western European accented characters).
 * Returns NULL if the file cannot be read. loaded=0 means raylib
 * fell back to its default font (unsupported/corrupt file). */
static GraphFont *graph_new_font(const char *path, int base_size) {
    FILE *probe = fopen(path, "rb");
    if (!probe) return NULL;
    fclose(probe);

    int codepoints[95 + 96 + 6];
    int n = 0;
    for (int c = 32;  c <= 126; c++) codepoints[n++] = c;   /* ASCII    */
    for (int c = 160; c <= 255; c++) codepoints[n++] = c;   /* Latin-1  */
    codepoints[n++] = 0x2014;                               /* — em dash */
    codepoints[n++] = 0x2018;                               /* '         */
    codepoints[n++] = 0x2190;                               /* ←         */
    codepoints[n++] = 0x2191;                               /* ↑         */
    codepoints[n++] = 0x2192;                               /* →         */
    codepoints[n++] = 0x2193;                               /* ↓         */

    GraphFont *f = (GraphFont *)calloc(1, sizeof(GraphFont));
    f->base_size = base_size;
    f->font      = LoadFontEx(path, base_size, codepoints, n);
    /* On failure LoadFontEx returns the default font — never unload that. */
    f->loaded    = (f->font.texture.id != GetFontDefault().texture.id);
    SetTextureFilter(f->font.texture, TEXTURE_FILTER_BILINEAR);
    return f;
}

static int graph_key_code(const char *key) {
    if (!key) return 0;
    if (!strcmp(key,"SPACE"))  return KEY_SPACE;
    if (!strcmp(key,"ENTER"))  return KEY_ENTER;
    if (!strcmp(key,"ESCAPE")) return KEY_ESCAPE;
    if (!strcmp(key,"UP"))     return KEY_UP;
    if (!strcmp(key,"DOWN"))   return KEY_DOWN;
    if (!strcmp(key,"LEFT"))   return KEY_LEFT;
    if (!strcmp(key,"RIGHT"))  return KEY_RIGHT;
    if (!strcmp(key,"F11"))       return KEY_F11;
    if (!strcmp(key,"BACKSPACE"))  return KEY_BACKSPACE;
    if (!strcmp(key,"TAB"))        return KEY_TAB;
    if (strlen(key) == 1 && key[0] >= 'A' && key[0] <= 'Z')
        return KEY_A + (key[0] - 'A');
    if (strlen(key) == 1 && key[0] >= '0' && key[0] <= '9')
        return KEY_ZERO + (key[0] - '0');
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * BACKEND: stub (default — no display, API-complete no-op)
 * ════════════════════════════════════════════════════════════════════ */
#else

typedef struct {
    int width, height;
    int fps_target;
    int frame_count;
    int should_close;
    int fullscreen;
} GraphWin;

static GraphWin *graph_new_win(int w, int h, const char *title) {
    GraphWin *win = (GraphWin *)calloc(1, sizeof(GraphWin));
    win->width = w; win->height = h; win->fps_target = 60;
    fprintf(stderr,
        "[fluxa] std.graph: stub backend — window '%s' (%dx%d) created.\n"
        "  For real rendering: vendor raylib into vendor/raylib.h + vendor/libraylib.a\n"
        "  then rebuild with: make FLUXA_GRAPH_RAYLIB=1 build\n",
        title, w, h);
    return win;
}

typedef struct {
    int base_size;    /* size requested at load — used for stub metrics */
    int loaded;       /* mirrors the raylib-backend field; always 1 here */
} GraphFont;

/* Stub: validates the file exists (same error contract as the raylib
 * backend) and records the size for deterministic text_width metrics. */
static GraphFont *graph_new_font(const char *path, int base_size) {
    FILE *probe = fopen(path, "rb");
    if (!probe) return NULL;
    fclose(probe);
    GraphFont *f = (GraphFont *)calloc(1, sizeof(GraphFont));
    f->base_size = base_size;
    f->loaded    = 1;
    return f;
}

static int graph_key_code(const char *key) { (void)key; return 0; }
/* suppress unused-function in stub mode */
static inline void graph_key_code_unused_(void) { (void)graph_key_code; }

#endif /* FLUXA_GRAPH_RAYLIB */

/* ── Value helpers ───────────────────────────────────────────────── */
static inline Value graph_nil(void)    { Value v; v.type=VAL_NIL;    return v; }
static inline Value graph_bool(int b)  { Value v; v.type=VAL_BOOL;   v.as.boolean=b; return v; }
static inline Value graph_int(long n)  { Value v; v.type=VAL_INT;    v.as.integer=n; return v; }
static inline Value graph_float(double d){ Value v; v.type=VAL_FLOAT; v.as.real=d; return v; }
static inline Value graph_str(const char *s) {
    Value v; v.type=VAL_STRING; v.as.string=fxstr_new(s?s:""); return v; }

static inline Value graph_wrap(GraphWin *win) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=win;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline GraphWin *graph_unwrap(const Value *v, ErrStack *err,
                                      int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),"graph.%s: invalid window cursor",fn);
        errstack_push(err,ERR_FLUXA,eb,"graph",line); *had_error=1; return NULL; }
    return (GraphWin *)v->as.dyn->items[0].as.ptr;
}

static inline Value graph_wrap_font(GraphFont *f) {
    FluxaDyn *d=(FluxaDyn *)malloc(sizeof(FluxaDyn)); memset(d,0,sizeof(*d));
    d->items=(Value *)malloc(sizeof(Value));
    d->items[0].type=VAL_PTR; d->items[0].as.ptr=f;
    d->count=1; d->cap=1;
    Value v; v.type=VAL_DYN; v.as.dyn=d; return v;
}
static inline GraphFont *graph_unwrap_font(const Value *v, ErrStack *err,
                                            int *had_error, int line, const char *fn) {
    char eb[280];
    if (v->type!=VAL_DYN||!v->as.dyn||v->as.dyn->count<1||
        v->as.dyn->items[0].type!=VAL_PTR||!v->as.dyn->items[0].as.ptr) {
        snprintf(eb,sizeof(eb),
            "graph.%s: invalid font cursor — use graph.load_font() to create one",fn);
        errstack_push(err,ERR_FLUXA,eb,"graph",line); *had_error=1; return NULL; }
    return (GraphFont *)v->as.dyn->items[0].as.ptr;
}

/* ── Dispatch ────────────────────────────────────────────────────── */
static inline Value fluxa_std_graph_call(const char *fn_name,
                                          const Value *args, int argc,
                                          ErrStack *err, int *had_error,
                                          int line) {
    char errbuf[280];

#define GRAPH_ERR(msg) do { \
    snprintf(errbuf,sizeof(errbuf),"graph.%s (line %d): %s",fn_name,line,(msg)); \
    errstack_push(err,ERR_FLUXA,errbuf,"graph",line); \
    *had_error=1; return graph_nil(); } while(0)

#define NEED(n) do { if(argc<(n)) { \
    snprintf(errbuf,sizeof(errbuf),"graph.%s: expected %d arg(s), got %d",fn_name,(n),argc); \
    errstack_push(err,ERR_FLUXA,errbuf,"graph",line); \
    *had_error=1; return graph_nil(); } } while(0)

#define GET_WIN(idx,var) \
    GraphWin *(var)=graph_unwrap(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return graph_nil();

#define GET_FONT(idx,var) \
    GraphFont *(var)=graph_unwrap_font(&args[(idx)],err,had_error,line,fn_name); \
    if(!(var)) return graph_nil();

#define GET_INT(idx,var) \
    if(args[(idx)].type!=VAL_INT) GRAPH_ERR("expected int"); \
    long (var)=args[(idx)].as.integer;

#define GET_STR(idx,var) \
    if(args[(idx)].type!=VAL_STRING||!args[(idx)].as.string) GRAPH_ERR("expected str"); \
    const char *(var)=args[(idx)].as.string;

    if (!strcmp(fn_name,"version")) {
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_str("raylib/" RAYLIB_VERSION);
#else
        return graph_str("fluxa-graph/1.0 (stub — no display)");
#endif
    }

    if (!strcmp(fn_name,"init")) {
        NEED(3); GET_INT(0,w); GET_INT(1,h); GET_STR(2,title);
        GraphWin *win = graph_new_win((int)w, (int)h, title);
        return graph_wrap(win);
    }

    if (!strcmp(fn_name,"close")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) UnloadRenderTexture(win->target);
        CloseWindow();
#endif
        free(win);
        if(args[0].type==VAL_DYN&&args[0].as.dyn)
            args[0].as.dyn->items[0].as.ptr=NULL;
        return graph_nil();
    }

    if (!strcmp(fn_name,"should_close")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(WindowShouldClose());
#else
        return graph_bool(win->should_close);
#endif
    }

    if (!strcmp(fn_name,"begin_frame")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            BeginTextureMode(win->target);   /* render the game at logical res */
        } else {
            BeginDrawing();
        }
#else
        win->frame_count++;
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"end_frame")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            EndTextureMode();                 /* finish the offscreen render */
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            float scale = (float)sw / (float)win->width;
            float sy = (float)sh / (float)win->height;
            if (sy < scale) scale = sy;        /* fit: the smaller ratio wins */
            float dw = win->width  * scale;
            float dh = win->height * scale;
            float ox = (sw - dw) * 0.5f;       /* center → letterbox/pillarbox */
            float oy = (sh - dh) * 0.5f;
            BeginDrawing();
            ClearBackground(BLACK);            /* bars around the game are black */
            Rectangle srcR = { 0, 0, (float)win->target.texture.width,
                                     -(float)win->target.texture.height };
            Rectangle dstR = { ox, oy, dw, dh };
            Vector2 origin = { 0, 0 };
            DrawTexturePro(win->target.texture, srcR, dstR, origin, 0.0f, WHITE);
            EndDrawing();
        } else {
            EndDrawing();
        }
#else
        (void)win;
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"clear")) {
        NEED(4); GET_WIN(0,win);
        GET_INT(1,r); GET_INT(2,g); GET_INT(3,b);
        (void)win; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        ClearBackground((Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,255});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"fps")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_int(GetFPS());
#else
        return graph_int(win->fps_target);
#endif
    }

    if (!strcmp(fn_name,"fullscreen")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        (void)win;
        ToggleFullscreen();
        return graph_bool(IsWindowFullscreen());
#else
        win->fullscreen = !win->fullscreen;
        return graph_bool(win->fullscreen);
#endif
    }

    if (!strcmp(fn_name,"set_fps")) {
        NEED(2); GET_WIN(0,win); GET_INT(1,fps);
        win->fps_target = (int)fps;
#ifdef FLUXA_GRAPH_RAYLIB
        SetTargetFPS((int)fps);
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_rect")) {
        NEED(8); GET_WIN(0,win);
        GET_INT(1,x); GET_INT(2,y); GET_INT(3,w); GET_INT(4,h);
        GET_INT(5,r); GET_INT(6,g); GET_INT(7,b);
        (void)win; (void)x; (void)y; (void)w; (void)h; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawRectangle((int)x,(int)y,(int)w,(int)h,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,255});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_circle")) {
        NEED(7); GET_WIN(0,win);
        GET_INT(1,x); GET_INT(2,y); GET_INT(3,radius);
        GET_INT(4,r); GET_INT(5,g); GET_INT(6,b);
        (void)win; (void)x; (void)y; (void)radius; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawCircle((int)x,(int)y,(float)radius,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,255});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_line")) {
        NEED(8); GET_WIN(0,win);
        GET_INT(1,x1); GET_INT(2,y1); GET_INT(3,x2); GET_INT(4,y2);
        GET_INT(5,r); GET_INT(6,g); GET_INT(7,b);
        (void)win; (void)x1; (void)y1; (void)x2; (void)y2; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawLine((int)x1,(int)y1,(int)x2,(int)y2,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,255});
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"draw_text")) {
        NEED(8); GET_WIN(0,win);
        GET_STR(1,text); GET_INT(2,x); GET_INT(3,y); GET_INT(4,size);
        GET_INT(5,r); GET_INT(6,g); GET_INT(7,b);
        (void)win; (void)text; (void)x; (void)y; (void)size; (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawText(text,(int)x,(int)y,(int)size,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,255});
#endif
        return graph_nil();
    }

    /* ── graph.load_font(win, path, size) → dyn font cursor ─────── */
    if (!strcmp(fn_name,"load_font")) {
        NEED(3); GET_WIN(0,win); GET_STR(1,path); GET_INT(2,size);
        (void)win;
        if (size < 1 || size > 512) GRAPH_ERR("font size must be 1-512");
        GraphFont *f = graph_new_font(path, (int)size);
        if (!f) GRAPH_ERR("cannot open font file");
#ifdef FLUXA_GRAPH_RAYLIB
        if (!f->loaded) {
            free(f);
            GRAPH_ERR("failed to load font (unsupported or corrupt file)");
        }
#endif
        return graph_wrap_font(f);
    }

    /* ── graph.draw_text_font(win, font, text, x, y, size, r, g, b) ─ */
    if (!strcmp(fn_name,"draw_text_font")) {
        NEED(9); GET_WIN(0,win); GET_FONT(1,fnt);
        GET_STR(2,text); GET_INT(3,x); GET_INT(4,y); GET_INT(5,size);
        GET_INT(6,r); GET_INT(7,g); GET_INT(8,b);
        (void)win; (void)fnt; (void)text; (void)x; (void)y; (void)size;
        (void)r; (void)g; (void)b;
#ifdef FLUXA_GRAPH_RAYLIB
        DrawTextEx(fnt->font, text,
            (Vector2){(float)x,(float)y}, (float)size,
            (float)size / 10.0f,
            (Color){(unsigned char)r,(unsigned char)g,(unsigned char)b,255});
#endif
        return graph_nil();
    }

    /* ── graph.text_width(win, font, text, size) → int (pixels) ──── */
    if (!strcmp(fn_name,"text_width")) {
        NEED(4); GET_WIN(0,win); GET_FONT(1,fnt);
        GET_STR(2,text); GET_INT(3,size);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        Vector2 m = MeasureTextEx(fnt->font, text, (float)size,
                                  (float)size / 10.0f);
        return graph_int((long)m.x);
#else
        /* deterministic stub metric: ~0.6em average advance per byte */
        (void)fnt;
        return graph_int((long)strlen(text) * size * 6 / 10);
#endif
    }

    /* ── graph.unload_font(win, font) → nil ──────────────────────── */
    if (!strcmp(fn_name,"unload_font")) {
        NEED(2); GET_WIN(0,win); GET_FONT(1,fnt);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        if (fnt->loaded) UnloadFont(fnt->font);
#endif
        free(fnt);
        if (args[1].type==VAL_DYN && args[1].as.dyn && args[1].as.dyn->count>=1)
            args[1].as.dyn->items[0].as.ptr=NULL;   /* prevent double-free */
        return graph_nil();
    }

    if (!strcmp(fn_name,"key_pressed")) {
        NEED(2); GET_WIN(0,win); GET_STR(1,key);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(IsKeyPressed(graph_key_code(key)));
#else
        (void)key; return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"key_down")) {
        NEED(2); GET_WIN(0,win); GET_STR(1,key);
        (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(IsKeyDown(graph_key_code(key)));
#else
        (void)key; return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_x")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            float scale = (float)sw / (float)win->width;
            float sy = (float)sh / (float)win->height;
            if (sy < scale) scale = sy;
            float ox = (sw - win->width * scale) * 0.5f;
            int lx = (int)(((float)GetMouseX() - ox) / scale);
            return graph_int(lx);
        }
        return graph_int(GetMouseX());
#else
        (void)win; return graph_int(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_y")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        if (win->has_target) {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            float scale = (float)sw / (float)win->width;
            float sx = (float)sh / (float)win->height;
            if (sx < scale) scale = sx;
            float oy = (sh - win->height * scale) * 0.5f;
            int ly = (int)(((float)GetMouseY() - oy) / scale);
            return graph_int(ly);
        }
        return graph_int(GetMouseY());
#else
        (void)win; return graph_int(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_pressed")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_bool(IsMouseButtonPressed(MOUSE_BUTTON_LEFT));
#else
        return graph_bool(0);
#endif
    }

    if (!strcmp(fn_name,"dt")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_float((double)GetFrameTime());
#else
        return graph_float(1.0 / (double)(win->fps_target > 0 ? win->fps_target : 60));
#endif
    }

#undef GRAPH_ERR
#undef NEED
#undef GET_WIN
#undef GET_FONT
#undef GET_INT
#undef GET_STR

    snprintf(errbuf,sizeof(errbuf),"graph.%s: unknown function",fn_name);
    errstack_push(err,ERR_FLUXA,errbuf,"graph",line);
    *had_error=1; return graph_nil();
}

FLUXA_LIB_EXPORT(
    name      = "graph",
    toml_key  = "std.graph",
    owner     = "graph",
    call      = fluxa_std_graph_call,
    rt_aware  = 0,
    cfg_aware = 0
)

#endif /* FLUXA_STD_GRAPH_H */
