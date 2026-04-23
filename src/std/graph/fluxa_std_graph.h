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
 *   graph.draw_rect(win, x, y, w, h, r, g, b)          → nil
 *   graph.draw_circle(win, x, y, radius, r, g, b)       → nil
 *   graph.draw_line(win, x1, y1, x2, y2, r, g, b)      → nil
 *   graph.draw_text(win, text, x, y, size, r, g, b)     → nil
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
    int width, height;
    int fps_target;
} GraphWin;

static GraphWin *graph_new_win(int w, int h, const char *title) {
    GraphWin *win = (GraphWin *)calloc(1, sizeof(GraphWin));
    win->width = w; win->height = h; win->fps_target = 60;
    InitWindow(w, h, title);
    SetTargetFPS(60);
    return win;
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
    Value v; v.type=VAL_STRING; v.as.string=strdup(s?s:""); return v; }

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
        BeginDrawing();
#else
        win->frame_count++;
#endif
        return graph_nil();
    }

    if (!strcmp(fn_name,"end_frame")) {
        NEED(1); GET_WIN(0,win);
#ifdef FLUXA_GRAPH_RAYLIB
        EndDrawing();
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
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_int(GetMouseX());
#else
        return graph_int(0);
#endif
    }

    if (!strcmp(fn_name,"mouse_y")) {
        NEED(1); GET_WIN(0,win); (void)win;
#ifdef FLUXA_GRAPH_RAYLIB
        return graph_int(GetMouseY());
#else
        return graph_int(0);
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
