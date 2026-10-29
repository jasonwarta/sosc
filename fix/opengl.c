// OpenGL presentation layer for the game's -o2 renderer.
//
// The game draws everything in a fixed 640x480 coordinate space: 3D polygons through
// gluOrtho2D(0, 640, 480, 0) and 2D overlays (dashboard, gauges, mirror frame) through
// glRasterPos2i + glDrawPixels. Its window, however, covers the whole screen. This layer
//  * maps the 640x480 space onto a centered, aspect-correct area of the real window, so
//    3D is rasterized at native resolution instead of being stretched afterwards,
//  * scales glDrawPixels images with glPixelZoom to match,
//  * keeps clears in bounds: black outside the game area and in the overlay gaps, the
//    game's sky colour only inside the 3D view rectangle.
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "opengl.h"
#include "screenshot.h"
#include "hdtex.h"
#include "widescreen.h"
#include <stdio.h>

#define GL_SCISSOR_TEST      0x0C11
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_COLOR_BUFFER_BIT  0x4000
#define GL_SCISSOR_BOX       0x0C10
#define GL_FOG               0x0B60
#define GL_BLEND             0x0BE2
#define GL_ALPHA_TEST        0x0BC0
#define GL_ALPHA_TEST_FUNC   0x0BC1
#define GL_ALPHA_TEST_REF    0x0BC2
#define GL_GREATER           0x0204

#define GAME_W 640  // the 2D layout; the coordinate space is ws_canvas_w wide (widescreen.c)
#define GAME_H 480

// Current 3D view rectangle in game coordinates, as loaded by FUN_004a9ba5.
#define VIEW_X (*(int *)0x0062830c)
#define VIEW_Y (*(int *)0x00628310)
#define VIEW_W (*(int *)0x00628314)
#define VIEW_H (*(int *)0x00628318)

typedef void(WINAPI *glViewport_t)(int, int, int, int);
typedef void(WINAPI *glClear_t)(unsigned);
typedef void(WINAPI *glRasterPos2i_t)(int, int);
typedef void(WINAPI *glDrawPixels_t)(int, int, unsigned, unsigned, const void *);
typedef void(WINAPI *glPixelZoom_t)(float, float);
typedef void(WINAPI *glScissor_t)(int, int, int, int);
typedef void(WINAPI *glEnable_t)(unsigned);
typedef void(WINAPI *glClearColor_t)(float, float, float, float);
typedef void(WINAPI *glGetFloatv_t)(unsigned, float *);
typedef void(WINAPI *glGetIntegerv_t)(unsigned, int *);
typedef HDC(WINAPI *wglGetCurrentDC_t)(void);
typedef unsigned char(WINAPI *glIsEnabled_t)(unsigned);
typedef void(WINAPI *glAlphaFunc_t)(unsigned, float);

static glViewport_t real_glViewport;
static glClear_t real_glClear;
static glRasterPos2i_t real_glRasterPos2i;
static glDrawPixels_t real_glDrawPixels;
static glPixelZoom_t p_glPixelZoom;
static glScissor_t p_glScissor;
static glEnable_t p_glEnable, p_glDisable;
static glClearColor_t p_glClearColor;
static glGetFloatv_t p_glGetFloatv;
static glGetIntegerv_t p_glGetIntegerv;
static wglGetCurrentDC_t p_wglGetCurrentDC;
static glIsEnabled_t p_glIsEnabled;
static glAlphaFunc_t p_glAlphaFunc;

// Mapping from game coordinates (origin top-left, y down) to window pixels (origin
// bottom-left, y up): window_x = ox + game_x * scale, window_y = oy + (GAME_H - game_y) * scale.
static float scale = 1.0f;
static int ox, oy, win_w, win_h;
static LONG overlay_calls_logged;

static void update_mapping(void) {
    // cnc-ddraw hooks GetClientRect to report the game's 640x480 mode; GetWindowInfo
    // gives the real size of the window OpenGL is drawing into.
    HWND w = p_wglGetCurrentDC ? WindowFromDC(p_wglGetCurrentDC()) : NULL;
    WINDOWINFO wi = {sizeof wi};
    if (!w || !GetWindowInfo(w, &wi)) return;
    int cw = wi.rcClient.right - wi.rcClient.left, ch = wi.rcClient.bottom - wi.rcClient.top;
    if (cw <= 0 || ch <= 0 || (cw == win_w && ch == win_h)) return;
    win_w = cw;
    win_h = ch;
    float sx = (float)win_w / ws_canvas_w, sy = (float)win_h / GAME_H;
    scale = sx < sy ? sx : sy;
    ox = (int)((win_w - ws_canvas_w * scale) / 2);
    oy = (int)((win_h - GAME_H * scale) / 2);
    logmsg("opengl: window %dx%d, game area %dx%d at (%d,%d), scale %.3f\n", win_w, win_h,
           (int)(ws_canvas_w * scale), (int)(GAME_H * scale), ox, oy, scale);
}

// Window-space rectangle for a rectangle in game coordinates.
static void game_rect_to_window(int x, int y, int w, int h, int *wx, int *wy, int *ww, int *wh) {
    *wx = ox + (int)(x * scale);
    *wy = oy + (int)((GAME_H - (y + h)) * scale);
    *ww = (int)(w * scale + 0.5f);
    *wh = (int)(h * scale + 0.5f);
}

static void census_frame_start(void);
static void depth_frame_start(void);

static void WINAPI hook_glViewport(int x, int y, int w, int h) {
    census_frame_start();
    depth_frame_start();
    update_mapping();
    // The game always asks for the full 640x480 space; keep its rectangle but map it.
    if (x == 0 && w == GAME_W) w = ws_canvas_w;
    int wx, wy, ww, wh;
    game_rect_to_window(x, GAME_H - (y + h), w, h, &wx, &wy, &ww, &wh);
    real_glViewport(wx, wy, ww, wh);
    p_glPixelZoom(scale, scale);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(wx, wy, ww, wh);
}

static void WINAPI hook_glClear(unsigned mask) {
    if (!(mask & GL_COLOR_BUFFER_BIT)) {
        real_glClear(mask);
        return;
    }
    float game_colour[4];
    p_glGetFloatv(GL_COLOR_CLEAR_VALUE, game_colour);
    // Everything (window borders and the game area) to black...
    p_glDisable(GL_SCISSOR_TEST);
    p_glClearColor(0, 0, 0, 1);
    real_glClear(GL_COLOR_BUFFER_BIT);
    // ...then the game's own clear (sky colour, depth) only inside the 3D view.
    p_glClearColor(game_colour[0], game_colour[1], game_colour[2], game_colour[3]);
    int wx, wy, ww, wh;
    int vw = VIEW_W > 0 && VIEW_W <= ws_canvas_w ? VIEW_W : ws_canvas_w;
    int vh = VIEW_H > 0 && VIEW_H <= GAME_H ? VIEW_H : GAME_H;
    game_rect_to_window(VIEW_X, VIEW_Y, vw, vh, &wx, &wy, &ww, &wh);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(wx, wy, ww, wh);
    real_glClear(mask);
    // Later drawing stays inside the game area.
    game_rect_to_window(0, 0, ws_canvas_w, GAME_H, &wx, &wy, &ww, &wh);
    p_glScissor(wx, wy, ww, wh);
}

// The game sets gluOrtho2D(0, 640, 480, 0) once when 3D starts; widescreen widens it.
typedef void(WINAPI *gluOrtho2D_t)(double, double, double, double);
static gluOrtho2D_t real_gluOrtho2D;

static void WINAPI hook_gluOrtho2D(double left, double right, double bottom, double top) {
    if (left == 0 && right == GAME_W) right = ws_canvas_w;
    real_gluOrtho2D(left, right, bottom, top);
}

static int raster_x = -1, raster_y;  // the game's last raster position (640 space)

static void WINAPI hook_glRasterPos2i(int x, int y) {
    if (InterlockedIncrement(&overlay_calls_logged) <= 40) logmsg("opengl: glRasterPos2i(%d, %d)\n", x, y);
    raster_x = x, raster_y = y;
    real_glRasterPos2i(x + ws_hud_off, y);  // overlays are laid out for 640: centre them
}

// The game uploads its 256-colour palette as pixel maps (glPixelMapfv I_TO_R/G/B/A) and draws
// overlays as GL_COLOR_INDEX images. Modern drivers handle that legacy path badly (wrong
// colours), so convert to RGBA here using the game's own map tables.
#define GL_COLOR_INDEX    0x1900
#define GL_RGBA           0x1908
#define GL_UNSIGNED_BYTE  0x1401
#define GL_PIXEL_MAP_I_TO_R 0x0C72  // ..._G 0x0C73, ..._B 0x0C74, ..._A 0x0C75

typedef void(WINAPI *glPixelMapfv_t)(unsigned, int, const float *);
typedef void(WINAPI *glTexImage2D_t)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static glPixelMapfv_t real_glPixelMapfv;
static glTexImage2D_t real_glTexImage2D;

// Our copy of the four index->colour maps the game uploads, and the RGBA table built from them.
static float maps[4][256];
static int map_sizes[4];
static LONG map_uploads_logged, textures_logged;
// Uploads per GL context, logged when the next context is created: the game makes a new
// context at every menu round trip, and a new context starts with no textures.
static int ctx_uploads, ctx_world_uploads;
#define WORLD_UPLOAD_RET 0x004b6da1  // 32x32 world textures, uploaded when a city loads

static void palettes_changed(void);

static void WINAPI hook_glPixelMapfv(unsigned map, int size, const float *values) {
    int c = (int)map - GL_PIXEL_MAP_I_TO_R;
    if (c >= 0 && c < 4 && values && size > 0) {
        int n = size < 256 ? size : 256;
        memcpy(maps[c], values, n * sizeof(float));
        map_sizes[c] = n;
        palettes_changed();
        if (InterlockedIncrement(&map_uploads_logged) <= 24)
            logmsg("opengl: glPixelMapfv(%c, %d) [0]=%.3f [1]=%.3f [255]=%.3f from %08lx\n", "RGBA"[c], size, values[0],
                   n > 1 ? values[1] : 0.0f, n > 255 ? values[255] : 0.0f, (DWORD)(UINT_PTR)__builtin_return_address(0));
    }
    real_glPixelMapfv(map, size, values);
}

// The live 256-colour palette, as the game sets it on its DirectDraw palette. The OpenGL
// path only ever uploads the default palette from the exe's resources, but the game changes
// entries at runtime (per-city colours, blinking radar blips, flashes); in software mode
// everything is drawn with the live palette. Two copies are kept:
//   current - every update, fades and flashes included; used for what the game re-uploads
//             every frame (overlays, radar, font), so those fade and flash like the original
//   base    - updates outside the fade routine; used for textures uploaded once (world), so
//             a fade in progress never gets baked into them
enum { PAL_CURRENT, PAL_BASE };
static PALETTEENTRY palettes[2][256];
static int palette_known;
static DWORD luts[2][256];
static int luts_dirty[2] = {1, 1};

static void palettes_changed(void) {
    luts_dirty[PAL_CURRENT] = luts_dirty[PAL_BASE] = 1;
}

static void rebuild_lut(int which) {
    if (!luts_dirty[which]) return;
    for (int i = 0; i < 256; i++) {
        BYTE ch[4];
        for (int c = 0; c < 4; c++) {
            // GL indexes a map of size n with (i mod n); an unset map is 1.0 for alpha, 0 otherwise.
            float v = map_sizes[c] ? maps[c][i % map_sizes[c]] : (c == 3 ? 1.0f : 0.0f);
            ch[c] = (BYTE)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
        }
        if (palette_known) {
            ch[0] = palettes[which][i].peRed;
            ch[1] = palettes[which][i].peGreen;
            ch[2] = palettes[which][i].peBlue;
        }
        luts[which][i] = (DWORD)ch[0] | (DWORD)ch[1] << 8 | (DWORD)ch[2] << 16 | (DWORD)ch[3] << 24;
    }
    luts_dirty[which] = 0;
}

// IDirectDrawPalette::GetEntries (vtable slot 4) and SetEntries (slot 6) of the game's palette
// object, whose pointer the game keeps at 0x690714. Hooked as soon as it exists.
#define GAME_PALETTE  (*(void **)0x00690714)
// Temporary palette changes (kept out of the base palette): the two fade routines
// FUN_00525b00 and FUN_00525fc0 (0x525b00-0x5262d0) and the Apocalypse white flash
// FUN_0046e310. FUN_005262d0 (0x52632f) sets the real palette.
#define FADE_FN_START  0x00525b00
#define FADE_FN_END    0x005262d0
#define FLASH_FN_START 0x0046e310
#define FLASH_FN_END   0x0046e600

typedef HRESULT(WINAPI *GetEntries_t)(void *, DWORD, DWORD, DWORD, PALETTEENTRY *);
typedef HRESULT(WINAPI *SetEntries_t)(void *, DWORD, DWORD, DWORD, PALETTEENTRY *);
static SetEntries_t real_SetEntries;
static LONG set_entries_logged;

static void *patch_vtable(void *object, int slot, void *hook) {
    void **vtable = *(void ***)object;
    if (vtable[slot] == hook) return NULL;
    DWORD old;
    void *orig = vtable[slot];
    VirtualProtect(&vtable[slot], sizeof(void *), PAGE_READWRITE, &old);
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void *), old, &old);
    return orig;
}

static HRESULT WINAPI hook_SetEntries(void *pal, DWORD flags, DWORD start, DWORD count, PALETTEENTRY *entries) {
    DWORD ret = (DWORD)(UINT_PTR)__builtin_return_address(0);
    int fade = (ret >= FADE_FN_START && ret < FADE_FN_END) || (ret >= FLASH_FN_START && ret < FLASH_FN_END);
    static DWORD last_ret;
    static LONG repeats;
    // Log each new caller, and the first few calls overall (blinks and flashes repeat a lot).
    if (InterlockedIncrement(&set_entries_logged) <= 30 || ret != last_ret)
        logmsg("palette: SetEntries(start %lu, count %lu) from %08lx%s first=(%d,%d,%d) [%ld repeats of previous]\n",
               start, count, ret, fade ? " (fade)" : "", entries ? entries[0].peRed : -1,
               entries ? entries[0].peGreen : -1, entries ? entries[0].peBlue : -1, repeats);
    repeats = ret == last_ret ? repeats + 1 : 0;
    last_ret = ret;
    if (entries && start < 256) {
        DWORD n = start + count > 256 ? 256 - start : count;
        memcpy(&palettes[PAL_CURRENT][start], entries, n * sizeof *entries);
        // Before every fade-in FUN_005262d0 (0x52632f) sets the whole palette to black from
        // outside the fade routines. Taken as the base palette, it turned every world texture
        // uploaded afterwards (menu round trips, graphics dialog) black. A palette that is
        // black throughout is never a real one: keep it out of the base.
        int all_black = 1;
        for (DWORD i = 0; i < n && all_black; i++)
            all_black = !entries[i].peRed && !entries[i].peGreen && !entries[i].peBlue;
        if (!fade && all_black && n > 16) {
            static LONG black_skips;
            if (InterlockedIncrement(&black_skips) <= 5)
                logmsg("palette: all-black SetEntries from %08lx kept out of the texture palette\n", ret);
            fade = 1;
        }
        if (!fade) memcpy(&palettes[PAL_BASE][start], entries, n * sizeof *entries);
        palette_known = 1;
        palettes_changed();
    }
    return real_SetEntries(pal, flags, start, count, entries);
}

// Called every frame: hook the game's palette object once it exists.
void opengl_track_palette(void) {
    static void *hooked;
    void *pal = GAME_PALETTE;
    if (!pal || pal == hooked) return;
    void *orig = patch_vtable(pal, 6, (void *)hook_SetEntries);
    if (orig) real_SetEntries = (SetEntries_t)orig;
    hooked = pal;
    GetEntries_t get = (GetEntries_t)(*(void ***)pal)[4];
    if (get(pal, 0, 0, 256, palettes[PAL_CURRENT]) == 0) {
        memcpy(palettes[PAL_BASE], palettes[PAL_CURRENT], sizeof palettes[PAL_BASE]);
        palette_known = 1;
        palettes_changed();
    }
    logmsg("palette: tracking game palette %p (%s), seeded [10]=(%d,%d,%d)\n", pal, real_SetEntries ? "ok" : "FAILED",
           palettes[PAL_CURRENT][10].peRed, palettes[PAL_CURRENT][10].peGreen, palettes[PAL_CURRENT][10].peBlue);
}

static DWORD *rgba_buffer;
static int rgba_capacity;

// Convert w*h colour indices to RGBA through palette `which`; returns NULL on failure.
static const DWORD *indices_to_rgba(const BYTE *src, int w, int h, int which) {
    if (w * h > rgba_capacity) {
        free(rgba_buffer);
        rgba_capacity = w * h;
        rgba_buffer = malloc(rgba_capacity * sizeof *rgba_buffer);
        if (!rgba_buffer) {
            rgba_capacity = 0;
            return NULL;
        }
    }
    rebuild_lut(which);
    const DWORD *lut = luts[which];
    for (int i = 0; i < w * h; i++) rgba_buffer[i] = lut[src[i]];
    return rgba_buffer;
}

// Most common colour indices in an image, for the log.
static void log_index_usage(const char *what, const BYTE *src, int n) {
    int counts[256] = {0};
    for (int i = 0; i < n; i++) counts[src[i]]++;
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < 256; i++)
        for (int k = 0; k < 3; k++)
            if (top[k] < 0 || counts[i] > counts[top[k]]) {
                for (int j = 2; j > k; j--) top[j] = top[j - 1];
                top[k] = i;
                break;
            }
    rebuild_lut(PAL_CURRENT);
    const DWORD *lut = luts[PAL_CURRENT];
    logmsg("opengl:   %s top indices: %d (%d px, rgba %08lx), %d (%d px, rgba %08lx), %d (%d px, rgba %08lx)\n", what,
           top[0], counts[top[0]], lut[top[0]], top[1], counts[top[1]], lut[top[1]], top[2], counts[top[2]], lut[top[2]]);
}

// The game's image blitter (FUN_0051a900) keeps the image object in ebx when it calls
// glDrawPixels. Each image has its own transparent colour: flag at +0x30, key index at +0x48.
// The software renderer skips key pixels and Glide uses chroma keying, but the OpenGL path
// relied on the global alpha map, which only knows index 254, so text and radar backgrounds
// came out solid. The thunk below records ebx so the hook can apply the image's own key.
#define BLIT_RET_FULL  0x0051aaca  // whole-image glDrawPixels in FUN_0051a900
#define BLIT_RET_ROWS  0x0051ab35  // row-by-row glDrawPixels for partial images
#define IMAGE_HAS_KEY(img) (*(int *)((BYTE *)(img) + 0x30) != 0)
#define IMAGE_KEY(img)     (*((BYTE *)(img) + 0x48))

static void *blit_image;  // ebx at the time of the latest glDrawPixels call

// Diagnostics (dumpimages=1): save the first draw of each overlay and the first textures as
// PNGs, plus an index map for short strips, under logs\dumps.
static char dump_dir[MAX_PATH];

void opengl_set_dump_dir(const char *dir) {
    lstrcpynA(dump_dir, dir, sizeof dump_dir);
    CreateDirectoryA(dump_dir, NULL);
}

static void dump_index_map(const BYTE *src, int w, int h, int key, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    int counts[256] = {0};
    for (int i = 0; i < w * h; i++) counts[src[i]]++;
    fprintf(f, "%dx%d key %d; legend: ' ' = key, digits/letters = index rank by frequency\n", w, h, key);
    // Rank indices by frequency so the map uses a few readable symbols.
    int rank[256], order[256];
    for (int i = 0; i < 256; i++) order[i] = i;
    for (int i = 0; i < 256; i++)
        for (int j = i + 1; j < 256; j++)
            if (counts[order[j]] > counts[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    const char *symbols = "0123456789abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < 256; i++) rank[order[i]] = i;
    for (int i = 0; i < 36 && counts[order[i]]; i++)
        fprintf(f, "  %c = index %d (%d px)\n", symbols[i], order[i], counts[order[i]]);
    for (int y = h - 1; y >= 0; y--) {  // GL rows are bottom-up; print top row first
        for (int x = 0; x < w; x++) {
            int idx = src[y * w + x];
            fputc(idx == key ? ' ' : rank[idx] < 36 ? symbols[rank[idx]] : '?', f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

static void maybe_dump_overlay(const BYTE *src, const DWORD *rgba, int w, int h, int key) {
    static struct { int w, h, key; } seen[48];
    static int nseen;
    if (!dump_dir[0]) return;
    for (int i = 0; i < nseen; i++)
        if (seen[i].w == w && seen[i].h == h && seen[i].key == key) return;
    if (nseen >= 48) return;
    seen[nseen].w = w, seen[nseen].h = h, seen[nseen].key = key, nseen++;
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\overlay_%dx%d_key%d.png", dump_dir, w, h, key);
    logmsg("opengl: dumped overlay %s (%s)\n", path, save_rgba_png((const unsigned long *)rgba, w, h, path) ? "ok" : "FAILED");
    if (h <= 40) {
        snprintf(path, sizeof path, "%s\\overlay_%dx%d_key%d.txt", dump_dir, w, h, key);
        dump_index_map(src, w, h, key, path);
    }
}

static void WINAPI hook_glDrawPixels_body(int w, int h, unsigned fmt, unsigned type, const void *px, DWORD ret);

__attribute__((naked)) static void WINAPI hook_glDrawPixels(void) {
    // Entry: [esp] = return address, [esp+4..+20] = the 5 glDrawPixels arguments.
    __asm__ volatile(
        "movl %%ebx, %0\n\t"
        "pushl (%%esp)\n\t"       // copy of the return address = body's 6th argument
        "pushl 24(%%esp)\n\t"     // copy the 5 arguments (each is 24 bytes above esp
        "pushl 24(%%esp)\n\t"     // once the previous one has been pushed)
        "pushl 24(%%esp)\n\t"
        "pushl 24(%%esp)\n\t"
        "pushl 24(%%esp)\n\t"
        "call %P1\n\t"            // stdcall body pops its 6 arguments (the copies)
        "ret $20\n\t"             // back to the game, popping the original 5
        : "=m"(blit_image)
        : "i"(hook_glDrawPixels_body));
}

// Which game code draws each overlay: the first time an image size shows up with a new
// chain of callers, log the game return addresses found on the stack (up to 64 chains).
// Full-width cockpit images (the in-car dashboards, 640 wide) end at the edges of the 4:3
// area in widescreen, with the world showing beside them. Continue them to the screen edges
// with a strip of the image's own surface: EXTEND_BLOCK columns starting EXTEND_INSET in from
// each end (past the rounded corners), tiled outward mirrored so the grain has no seams.
// Each extension also covers the corner columns, so it joins the image cleanly. Gauges,
// needles and the radar screen stay where the game puts them.
#define EXTEND_INSET 12
#define EXTEND_BLOCK 32
static int cfg_extend_hud = 1;

void opengl_set_extend_hud(int on) { cfg_extend_hud = on; }

static void extend_full_width_overlay(const DWORD *rgba, int w, int h) {
    static DWORD *strip;
    static int strip_cap;
    if (!cfg_extend_hud) return;
    int sw = ws_hud_off + EXTEND_INSET;  // side width including the covered corner
    if (sw * h > strip_cap) {
        free(strip);
        strip = malloc((size_t)sw * h * sizeof *strip);
        strip_cap = strip ? sw * h : 0;
        if (!strip) return;
    }
    for (int side = 0; side < 2; side++) {
        for (int x = 0; x < sw; x++) {
            int d = side ? x : sw - 1 - x;  // distance from the inner end of the strip
            int pos = d % (2 * EXTEND_BLOCK);
            int off = pos < EXTEND_BLOCK ? pos : 2 * EXTEND_BLOCK - 1 - pos;
            int src = side ? w - 1 - EXTEND_INSET - off : EXTEND_INSET + off;
            for (int y = 0; y < h; y++) strip[(size_t)y * sw + x] = rgba[(size_t)y * w + src];
        }
        // canvas x: from 0 on the left; on the right from the image's last corner column
        real_glRasterPos2i(side ? ws_hud_off + w - EXTEND_INSET : 0, raster_y);
        real_glDrawPixels(sw, h, GL_RGBA, GL_UNSIGNED_BYTE, strip);
    }
    real_glRasterPos2i(raster_x + ws_hud_off, raster_y);
}

// Hand-made wide versions of full-width overlays: textures\wide\<hash>.png (that exact
// image) or textures\wide\<w>x<h>.png (any overlay of that size), with the original centred
// in a wider picture (e.g. 1712 wide, original at x 536). Drawn cropped to the canvas in
// place of the original.
// Other in-car pieces, by size: the roof corner (88x80, the Beetle) and the windshield pillar
// (100x289, wood-dash cars) are pinned to the left screen edge; the roof bars (560x17,
// 540x17) are stretched between the screen edges. Each position is logged once.
enum { COCKPIT_NONE, COCKPIT_LEFT, COCKPIT_BAR };

static int cockpit_piece(int w, int h) {
    int kind = COCKPIT_NONE;
    if ((w == 88 && h == 80) || (w == 100 && h == 289)) kind = raster_x == 0 ? COCKPIT_LEFT : COCKPIT_NONE;
    else if (h <= 24 && w >= 400 && w < GAME_W) kind = COCKPIT_BAR;
    static struct { int w, h, x, y; } seen[16];
    static int nseen;
    if (kind != COCKPIT_NONE || (w == 88 && h == 80) || (w == 100 && h == 289)) {
        int i;
        for (i = 0; i < nseen && !(seen[i].w == w && seen[i].h == h && seen[i].x == raster_x && seen[i].y == raster_y); i++) {}
        if (i == nseen && nseen < 16) {
            seen[nseen].w = w, seen[nseen].h = h, seen[nseen].x = raster_x, seen[nseen].y = raster_y, nseen++;
            logmsg("cockpit piece %dx%d at (%d, %d): %s\n", w, h, raster_x, raster_y,
                   kind == COCKPIT_LEFT ? "pinned left" : kind == COCKPIT_BAR ? "stretched" : "left alone");
        }
    }
    return kind;
}

static unsigned long long texture_hash(const BYTE *px, int w, int h);
static char wide_dir[MAX_PATH];
// One entry per file in the folder (indexed once), loaded on first use. key 0 = by size.
static struct { unsigned long long key; int w, h, tried; unsigned long *rgba; int iw, ih; } wide_files[32];
static int wide_nfiles, wide_have_keys;

void opengl_set_wide_dir(const char *dir) {
    snprintf(wide_dir, sizeof wide_dir, "%s", dir);
    char pattern[MAX_PATH + 8];
    snprintf(pattern, sizeof pattern, "%s\\*.png", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        unsigned long long key = 0;
        int w = 0, h = 0;
        char tail[8];
        if (wide_nfiles == 32) break;
        if (sscanf(fd.cFileName, "%dx%d%7s", &w, &h, tail) == 3 && !_stricmp(tail, ".png")) {
            wide_files[wide_nfiles].w = w, wide_files[wide_nfiles].h = h;
        } else if (strlen(fd.cFileName) == 20 && sscanf(fd.cFileName, "%16llx", &key) == 1 && key) {
            wide_files[wide_nfiles].key = key;
            wide_have_keys = 1;
        } else {
            continue;
        }
        wide_nfiles++;
        logmsg("wide overlay: found %s\n", fd.cFileName);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static int draw_wide_overlay(const BYTE *px, int w, int h) {
    if (!wide_nfiles || !cfg_extend_hud) return 0;
    unsigned long long key = wide_have_keys ? texture_hash(px, w, h) : 0;
    int i = -1;
    for (int j = 0; j < wide_nfiles && i < 0; j++)
        if (key && wide_files[j].key == key) i = j;
    for (int j = 0; j < wide_nfiles && i < 0; j++)
        if (!wide_files[j].key && wide_files[j].w == w && wide_files[j].h == h) i = j;
    if (i < 0) return 0;
    if (!wide_files[i].tried) {
        wide_files[i].tried = 1;
        char path[MAX_PATH + 64];
        if (wide_files[i].key) snprintf(path, sizeof path, "%s\\%016llx.png", wide_dir, wide_files[i].key);
        else snprintf(path, sizeof path, "%s\\%dx%d.png", wide_dir, w, h);
        int iw = 0, ih = 0;
        unsigned long *img = hdtex_load_file(path, &iw, &ih);
        if (img && (ih != h || iw < ws_canvas_w)) {
            logmsg("wide overlay: %s is %dx%d, needs height %d and width >= %d; ignored\n", path, iw, ih, h, ws_canvas_w);
            free(img);
            img = NULL;
        }
        logmsg("wide overlay: %dx%d -> %s (%s)\n", w, h, path, img ? "ok" : "FAILED");
        wide_files[i].rgba = img, wide_files[i].iw = iw, wide_files[i].ih = ih;
    }
    if (!wide_files[i].rgba) return 0;
    // the original sits centred in the wide image; take the canvas-wide window around it
    int iw = wide_files[i].iw, x0 = (iw - w) / 2 - ws_hud_off;
    if (x0 < 0) x0 = 0;
    if (x0 + ws_canvas_w > iw) x0 = iw - ws_canvas_w;
    static DWORD *crop;
    static int crop_cap;
    if (ws_canvas_w * h > crop_cap) {
        free(crop);
        crop = malloc((size_t)ws_canvas_w * h * sizeof *crop);
        crop_cap = crop ? ws_canvas_w * h : 0;
        if (!crop) return 0;
    }
    for (int y = 0; y < h; y++) memcpy(crop + (size_t)y * ws_canvas_w, wide_files[i].rgba + (size_t)y * iw + x0, ws_canvas_w * 4);
    real_glRasterPos2i(0, raster_y);
    real_glDrawPixels(ws_canvas_w, h, GL_RGBA, GL_UNSIGNED_BYTE, crop);
    real_glRasterPos2i(raster_x + ws_hud_off, raster_y);
    return 1;
}

static void log_overlay_callers(int w, int h) {
    static struct { int w, h; DWORD a, b; } seen[64];
    static int nseen;
    DWORD *sp = (DWORD *)__builtin_frame_address(0), chain[8];
    int n = 0;
    for (int i = 0; i < 160 && n < 8; i++)
        if (sp[i] >= 0x00401000 && sp[i] < 0x005c0000 && sp[i] != BLIT_RET_FULL && sp[i] != BLIT_RET_ROWS) chain[n++] = sp[i];
    DWORD a = n > 0 ? chain[0] : 0, b = n > 1 ? chain[1] : 0;
    for (int i = 0; i < nseen; i++)
        if (seen[i].w == w && seen[i].h == h && seen[i].a == a && seen[i].b == b) return;
    if (nseen == 64) return;
    seen[nseen].w = w, seen[nseen].h = h, seen[nseen].a = a, seen[nseen].b = b, nseen++;
    char buf[160];
    int len = 0;
    for (int i = 0; i < n; i++) len += snprintf(buf + len, sizeof buf - len, " %08lx", chain[i]);
    buf[len] = 0;
    logmsg("opengl: overlay %dx%d drawn by%s\n", w, h, buf);
}

static void WINAPI hook_glDrawPixels_body(int w, int h, unsigned fmt, unsigned type, const void *px, DWORD ret) {
    log_overlay_callers(w, h);
    int log_it = InterlockedIncrement(&overlay_calls_logged) <= 40;
    void *image = (ret == BLIT_RET_FULL || ret == BLIT_RET_ROWS) ? blit_image : NULL;
    int has_key = image && IMAGE_HAS_KEY(image);
    int key = has_key ? IMAGE_KEY(image) : -1;
    if (log_it)
        logmsg("opengl: glDrawPixels(%d x %d, fmt %04x, type %04x) from %08lx, image %p key %d\n", w, h, fmt, type, ret,
               image, key);
    if (fmt != GL_COLOR_INDEX || type != GL_UNSIGNED_BYTE || w <= 0 || h <= 0) {
        real_glDrawPixels(w, h, fmt, type, px);
        return;
    }
    if (log_it) log_index_usage("overlay", px, w * h);
    DWORD *rgba = (DWORD *)indices_to_rgba(px, w, h, PAL_CURRENT);
    if (!rgba) return;
    if (image) {
        // Same rule as the software blitter: opaque, except the image's key colour.
        const BYTE *src = px;
        for (int i = 0; i < w * h; i++)
            rgba[i] = src[i] == key ? rgba[i] & 0x00FFFFFF : rgba[i] | 0xFF000000;
    }
    maybe_dump_overlay(px, rgba, w, h, key);
    // Fog (enabled for the 3D world, coloured like the sky) also applies to glDrawPixels
    // fragments, which tinted the whole dashboard sky blue. Overlays are drawn unfogged.
    // Index 254 is the overlays' transparent colour (alpha 0 in the game's map). The game
    // draws with glBlendFunc(GL_SRC_ALPHA, GL_ZERO), which turns those pixels black, so
    // discard them with an alpha test and draw without blending.
    int fogged = p_glIsEnabled(GL_FOG), blended = p_glIsEnabled(GL_BLEND), tested = p_glIsEnabled(GL_ALPHA_TEST);
    float alpha_func, alpha_ref;
    p_glGetFloatv(GL_ALPHA_TEST_FUNC, &alpha_func);
    p_glGetFloatv(GL_ALPHA_TEST_REF, &alpha_ref);
    if (fogged) p_glDisable(GL_FOG);
    if (blended) p_glDisable(GL_BLEND);
    p_glEnable(GL_ALPHA_TEST);
    p_glAlphaFunc(GL_GREATER, 0.0f);
    int cockpit = cockpit_piece(w, h);
    if (ws_hud_off > 0 && w == GAME_W && raster_x == 0) {
        if (!draw_wide_overlay(px, w, h)) {
            real_glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            extend_full_width_overlay(rgba, w, h);
        }
    } else if (ws_hud_off > 0 && cfg_extend_hud && cockpit == COCKPIT_LEFT) {
        // the roof corner / windshield pillar belongs at the left edge of the screen
        real_glRasterPos2i(raster_x, raster_y);
        real_glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        real_glRasterPos2i(raster_x + ws_hud_off, raster_y);
    } else if (ws_hud_off > 0 && cfg_extend_hud && cockpit == COCKPIT_BAR) {
        // the roof bar keeps its distance from both screen edges and stretches between them
        real_glRasterPos2i(raster_x, raster_y);
        p_glPixelZoom(scale * (float)(w + 2 * ws_hud_off) / w, scale);
        real_glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        p_glPixelZoom(scale, scale);
        real_glRasterPos2i(raster_x + ws_hud_off, raster_y);
    } else {
        real_glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    p_glAlphaFunc((unsigned)alpha_func, alpha_ref);
    if (!tested) p_glDisable(GL_ALPHA_TEST);
    if (blended) p_glEnable(GL_BLEND);
    if (fogged) p_glEnable(GL_FOG);
}

// Texture uploads that need special handling, by return address.
#define FONT_UPLOAD_RET  0x004abb2d  // FUN_004abad0: 256x256 font sheet used by text glyphs
#define SPRITE_UPLOAD_RET 0x004aed7d // FUN_004aecd0: 128x128 textures, chroma-keyed in Glide too
#define CHROMA_KEYED(ret) ((ret) == FONT_UPLOAD_RET || (ret) == SPRITE_UPLOAD_RET)
#define RADAR_UPLOAD_RET 0x004ae848  // FUN_004ae7f0: 128x128 radar map, into texture object 0
#define TEXT_BEGIN_RET   0x004ae5fc  // FUN_004ae310: glBegin(GL_QUADS) for one text glyph

#define GL_TEXTURE_2D         0x0DE1
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_LINEAR             0x2601

typedef void(WINAPI *glTexParameteri_t)(unsigned, unsigned, int);
typedef void(WINAPI *glGetTexParameteriv_t)(unsigned, unsigned, int *);
static glTexParameteri_t p_glTexParameteri;
static glGetTexParameteriv_t p_glGetTexParameteriv;

static int WINAPI hook_glTexImage2D_impl(unsigned target, int level, int internal, int w, int h, int border,
                                         unsigned fmt, unsigned type, const void *px, DWORD ret);
static int texture_is_keyed(unsigned id);
static int key_black_rgb565(DWORD *rgba, int n);

static void WINAPI hook_glTexImage2D(unsigned target, int level, int internal, int w, int h, int border, unsigned fmt,
                                     unsigned type, const void *px) {
    if (hook_glTexImage2D_impl(target, level, internal, w, h, border, fmt, type, px,
                               (DWORD)(UINT_PTR)__builtin_return_address(0)))
        return;  // replaced by an HD texture, which has its own mipmaps
    // The game only ever uploads level 0. A texture object whose minification filter
    // expects mipmaps is then incomplete and draws untextured (the radar binds object 0,
    // whose default filter is GL_NEAREST_MIPMAP_LINEAR), so fall back to plain linear.
    int filter = 0;
    p_glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &filter);
    if (filter >= 0x2700 && filter <= 0x2703) p_glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

// Texture census (texturedump=1), the groundwork for high-resolution replacements: every
// distinct texture the game uploads is saved once as <dir>\<site>_<w>x<h>_<hash>.png, where
// the hash covers the raw 8-bit indices (so it doesn't depend on the palette), and per call
// site counts of uploads and distinct textures are logged every 30 s.
static char census_dir[MAX_PATH];
static struct { unsigned long long *keys; int cap, count; } tex_seen;
static struct { DWORD ret; int uploads, distinct, w, h, saved; } tex_sites[32];
static int tex_nsites;
static DWORD tex_census_logged_at;

static unsigned long long texture_hash(const BYTE *px, int w, int h) {
    unsigned long long x = 1469598103934665603ULL ^ ((unsigned long long)w << 32 | (unsigned)h);
    for (int i = 0; i < w * h; i++) x = (x ^ px[i]) * 1099511628211ULL;
    return x ? x : 1;
}

// Returns 1 the first time this hash is seen.
static int texture_first_seen(unsigned long long key) {
    if (tex_seen.count * 2 >= tex_seen.cap) {
        int cap = tex_seen.cap ? tex_seen.cap * 2 : 4096;
        unsigned long long *keys = calloc(cap, sizeof *keys);
        if (!keys) return 0;
        for (int i = 0; i < tex_seen.cap; i++) {
            if (!tex_seen.keys[i]) continue;
            int j = (int)(tex_seen.keys[i] & (cap - 1));
            while (keys[j]) j = (j + 1) & (cap - 1);
            keys[j] = tex_seen.keys[i];
        }
        free(tex_seen.keys);
        tex_seen.keys = keys, tex_seen.cap = cap;
    }
    int j = (int)(key & (tex_seen.cap - 1));
    while (tex_seen.keys[j]) {
        if (tex_seen.keys[j] == key) return 0;
        j = (j + 1) & (tex_seen.cap - 1);
    }
    tex_seen.keys[j] = key;
    tex_seen.count++;
    return 1;
}

static void texture_census(const BYTE *px, const DWORD *rgba, int w, int h, DWORD ret) {
    int s;
    for (s = 0; s < tex_nsites && tex_sites[s].ret != ret; s++) {}
    if (s == tex_nsites) {
        if (tex_nsites == 32) return;
        tex_sites[tex_nsites++].ret = ret;
    }
    tex_sites[s].uploads++;
    tex_sites[s].w = w, tex_sites[s].h = h;
    unsigned long long key = texture_hash(px, w, h);
    if (texture_first_seen(key)) {
        tex_sites[s].distinct++;
        // sites that upload something new every frame (radar) would fill the disk
        if (tex_sites[s].saved < 3000) {
            char path[MAX_PATH + 64];
            snprintf(path, sizeof path, "%s\\%08lx_%dx%d_%016llx.png", census_dir, ret, w, h, key);
            if (save_rgba_png((const unsigned long *)rgba, w, h, path)) tex_sites[s].saved++;
        }
    }
    DWORD now = GetTickCount();
    if (now - tex_census_logged_at >= 30000) {
        tex_census_logged_at = now;
        for (int i = 0; i < tex_nsites; i++)
            logmsg("texture census: site %08lx last %dx%d, %d uploads, %d distinct, %d saved\n", tex_sites[i].ret,
                   tex_sites[i].w, tex_sites[i].h, tex_sites[i].uploads, tex_sites[i].distinct, tex_sites[i].saved);
    }
}

// HD replacements. The game names its textures itself (fixed numbers, no glGenTextures) and
// uploads into whatever is bound. A replaced texture lives in our own object (numbered from
// HD_NAME_BASE, far from the game's), and the game's name is redirected to it in
// glBindTexture until the game uploads something without a replacement into that name.
// Our objects get mipmaps and trilinear filtering; the game's own filter calls (GL_LINEAR)
// don't reach them.
#define HD_NAME_BASE 0x40000000u
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_GENERATE_MIPMAP      0x8191
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
typedef void(WINAPI *glBindTexture_t)(unsigned, unsigned);
static glBindTexture_t real_glBindTexture;
static glTexParameteri_t real_glTexParameteri_iat;
static int hd_enabled;
static unsigned game_bound;  // the name the game thinks is bound
// Lookups run on every glBindTexture (thousands per frame) and every upload, so both are
// O(1): game names are small numbers (a direct table), objects are found by key (hash table).
#define HD_MAX_OBJECTS 4096
#define HD_REDIRECT_NAMES 65536  // game names beyond this aren't redirected (none seen)
static struct { unsigned long long key; unsigned name; } hd_objects[HD_MAX_OBJECTS];
static int hd_nobjects;
static unsigned short hd_object_slots[2 * HD_MAX_OBJECTS];  // key hash -> index + 1
static unsigned hd_redirect[HD_REDIRECT_NAMES];             // game name -> our object, 0 = none
static int hd_uploaded, hd_failed;
static int hd_soft_key;  // set around the particle uploads: fade dark texels out (soft_key)
static void soft_key(DWORD *rgba, int n);
#define PARTICLE_UPLOAD_RET 0x004b6b34  // glTexImage2D in FUN_004b69b0 (smoke, fire, spray, debris)

static unsigned hd_lookup_redirect(unsigned game) { return game < HD_REDIRECT_NAMES ? hd_redirect[game] : 0; }

static void hd_set_redirect(unsigned game, unsigned hd) {
    if (game < HD_REDIRECT_NAMES) hd_redirect[game] = hd;
}

static unsigned short *hd_object_slot(unsigned long long key) {
    unsigned j = (unsigned)(key ^ key >> 32) & (2 * HD_MAX_OBJECTS - 1);
    while (hd_object_slots[j] && hd_objects[hd_object_slots[j] - 1].key != key) j = (j + 1) & (2 * HD_MAX_OBJECTS - 1);
    return &hd_object_slots[j];
}

static void hd_forget_all(void) {
    hd_nobjects = 0;
    memset(hd_object_slots, 0, sizeof hd_object_slots);
    memset(hd_redirect, 0, sizeof hd_redirect);
}

static int is_hd_name(unsigned name) { return name >= HD_NAME_BASE && name < HD_NAME_BASE + HD_MAX_OBJECTS; }

// Animated-texture probe (diagnostics, texprobe=1): which texture names the game binds in
// each frame. Animation frames show up as names used in a regular fraction of frames even
// with the camera still. Every 5 s, names used in 5-95% of the last 64 frames are logged
// with their usage pattern (newest frame last) and the bind site.
static int cfg_tex_probe;
static unsigned long long probe_frames[2048];  // per name: bit i = used i frames ago
static DWORD probe_site[2048];
static DWORD probe_logged_at;

void opengl_set_tex_probe(int on) { cfg_tex_probe = on; }

static void tex_probe_frame(void);
static void water_frame(void);
void opengl_frame_tick(void) {
    tex_probe_frame();
    water_frame();
}

static int water_bound, water_vertex_pending, water_probe_frames;
// Whole-frame water totals for the probe: with the camera still, a total that changes
// between frames says what the game animates (texcoords or vertex positions).
static int water_drawing, water_nverts;  // water_drawing: 1 water, 2 other terrain
static double water_sum_s, water_sum_t, water_sum_x, water_sum_y, water_sum_w;
static int land_nverts;
static double land_sum_y, land_sum_w;
// per-vertex water (x, y, w, s, t) this frame and last, to find the vertices that move
#define WATER_PROBE_VERTS 4096
static float water_verts[2][WATER_PROBE_VERTS][5];
static int water_verts_n[2], water_verts_cur;
static float water_last_st[2];

static void water_compare_frames(void) {
    int c = water_verts_cur, p = c ^ 1, n = water_verts_n[c];
    if (n != water_verts_n[p] || !n) {
        logmsg("water verts: %d (last frame %d), not compared\n", n, water_verts_n[p]);
        return;
    }
    int moved = 0, st_moved = 0, worst = 0;
    float worst_dy = 0;
    for (int i = 0; i < n && i < WATER_PROBE_VERTS; i++) {
        float dy = water_verts[c][i][1] - water_verts[p][i][1];
        float dx = water_verts[c][i][0] - water_verts[p][i][0];
        if (dy > 0.5f || dy < -0.5f || dx > 0.5f || dx < -0.5f) moved++;
        float ds = water_verts[c][i][3] - water_verts[p][i][3], dt = water_verts[c][i][4] - water_verts[p][i][4];
        if (ds > 0.01f || ds < -0.01f || dt > 0.01f || dt < -0.01f) st_moved++;
        if ((dy < 0 ? -dy : dy) > (worst_dy < 0 ? -worst_dy : worst_dy)) worst_dy = dy, worst = i;
    }
    float *a = water_verts[p][worst], *b = water_verts[c][worst];
    logmsg("water verts: %d, %d moved >0.5 px, %d st moved; worst #%d dy %+.2f: (%.1f,%.1f w %.0f st %.3f,%.3f) -> "
           "(%.1f,%.1f w %.0f st %.3f,%.3f)\n", n, moved, st_moved, worst, worst_dy, a[0], a[1], a[2], a[3], a[4], b[0], b[1],
           b[2], b[3], b[4]);
}

// World grid vertices (texprobe): FUN_004bbd50 transforms *(int **)0xa0f788 ("gridvert",
// x y z ints, 0xa0bc38 of them) every frame. Log the vertices that change between frames
// (with the camera still, the water waves), then put a hardware write watchpoint on one to
// log the code that moves it.
#define GRIDVERT_PTR   0x00a0f788u
#define GRIDVERT_COUNT 0x00a0bc38u
static int *gridvert_prev;
static int gridvert_prev_n, gridvert_watch_armed, gridvert_watch_hits;
static DWORD main_thread_id;
static DWORD gridvert_watch_addr;

static LONG CALLBACK gridvert_watch_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    if (!(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    char line[300];
    int n = snprintf(line, sizeof line, "gridvert watch: write to %08lx (now %08lx) before eip %08lx, stack:",
                     gridvert_watch_addr, *(DWORD *)(UINT_PTR)gridvert_watch_addr, c->Eip);
    DWORD *sp = (DWORD *)c->Esp;
    for (int i = 0, found = 0; i < 256 && found < 10; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        if (sp[i] >= 0x00401000 && sp[i] < 0x005c3000) n += snprintf(line + n, sizeof line - n, " %08lx", sp[i]), found++;
    }
    logmsg("%s\n", line);
    c->Dr6 = 0;
    if (++gridvert_watch_hits >= 8) c->Dr7 = 0, c->Dr0 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI gridvert_arm_thread(void *arg) {
    (void)arg;
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, main_thread_id);
    if (!t) return logmsg("gridvert watch: OpenThread failed %lu\n", GetLastError()), 0;
    SuspendThread(t);
    CONTEXT c = {0};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    int ok = GetThreadContext(t, &c);
    c.Dr0 = gridvert_watch_addr;
    c.Dr7 = 1 | (1u << 16) | (3u << 18);  // L0, break on write, 4 bytes
    ok = ok && SetThreadContext(t, &c);
    ResumeThread(t);
    CloseHandle(t);
    logmsg("gridvert watch: %s on %08lx\n", ok ? "armed" : "FAILED", gridvert_watch_addr);
    return 0;
}

static void gridvert_frame(int log) {
    const int *v = *(int **)(UINT_PTR)GRIDVERT_PTR;
    int n = *(int *)(UINT_PTR)GRIDVERT_COUNT;
    if (!v || n <= 0 || n > 1 << 20 || IsBadReadPtr(v, (UINT_PTR)n * 12)) return;
    if (log && gridvert_prev && gridvert_prev_n == n) {
        int changed = 0, comp[3] = {0}, shown = 0, first = -1;
        char ex[256] = "";
        int len = 0;
        for (int i = 0; i < n; i++) {
            int d = 0;
            for (int k = 0; k < 3; k++)
                if (v[i * 3 + k] != gridvert_prev[i * 3 + k]) comp[k]++, d = 1;
            if (!d) continue;
            if (first < 0) first = i;
            changed++;
            if (shown++ < 3)
                len += snprintf(ex + len, sizeof ex - len, " #%d (%d,%d,%d)->(%d,%d,%d)", i, gridvert_prev[i * 3],
                                gridvert_prev[i * 3 + 1], gridvert_prev[i * 3 + 2], v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
        }
        logmsg("gridvert: %d of %d changed (x %d, y %d, z %d)%s\n", changed, n, comp[0], comp[1], comp[2], ex);
        if (first >= 0 && !gridvert_watch_armed) {
            gridvert_watch_armed = 1;
            int k = comp[2] ? 2 : comp[1] ? 1 : 0;
            for (int i = first; i < n; i++)
                if (v[i * 3 + k] != gridvert_prev[i * 3 + k]) {
                    gridvert_watch_addr = (DWORD)(UINT_PTR)&v[i * 3 + k];
                    break;
                }
            main_thread_id = GetCurrentThreadId();
            AddVectoredExceptionHandler(1, gridvert_watch_handler);
            CloseHandle(CreateThread(NULL, 0, gridvert_arm_thread, NULL, 0, NULL));
        }
    }
    if (gridvert_prev_n != n) {
        free(gridvert_prev);
        gridvert_prev = malloc((size_t)n * 12);
        gridvert_prev_n = gridvert_prev ? n : 0;
    }
    if (gridvert_prev) memcpy(gridvert_prev, v, (size_t)n * 12);
}

static void tex_probe_frame(void) {
    if (!cfg_tex_probe) return;
    gridvert_frame(water_probe_frames >= 18 && water_probe_frames < 23);
    DWORD now = GetTickCount();
    if (now - probe_logged_at >= 5000) {
        probe_logged_at = now;
        water_probe_frames = 24;
        int lines = 0;
        for (int n = 0; n < 2048 && lines < 40; n++) {
            int used = __builtin_popcountll(probe_frames[n]);
            if (used < 4 || used > 60) continue;
            char bits[65];
            for (int i = 0; i < 64; i++) bits[i] = (probe_frames[n] >> (63 - i)) & 1 ? '#' : '.';
            bits[64] = 0;
            logmsg("tex probe: name %4d bound in %2d/64 frames from %08lx  %s\n", n, used, probe_site[n], bits);
            lines++;
        }
    }
    for (int n = 0; n < 2048; n++) probe_frames[n] <<= 1;
    // one water vertex per frame while the probe runs
    if (water_probe_frames > 0) {
        if (water_probe_frames < 24)
            logmsg("water totals: %d verts  s %.3f  t %.3f  x %.2f  y %.2f  w %.1f | land %d verts y %.2f w %.1f\n",
                   water_nverts, water_sum_s, water_sum_t, water_sum_x, water_sum_y, water_sum_w, land_nverts,
                   land_sum_y, land_sum_w);
        if (water_probe_frames < 24) water_compare_frames();
        water_probe_frames--;
        water_vertex_pending = water_probe_frames > 0 ? 3 : 0;
    }
    water_nverts = land_nverts = 0;
    land_sum_y = land_sum_w = 0;
    water_verts_cur ^= 1;
    water_verts_n[water_verts_cur] = 0;
    water_sum_s = water_sum_t = water_sum_x = water_sum_y = water_sum_w = 0;
}

// Water animation runs on game frames: the terrain (FUN_004abc00, bind ret 0x4abc7a) steps
// through two groups of five texture names (2-6 and 7-11) one name every 2 game frames, so
// at 120 fps it runs 4x faster than at 30. Retime it: a bind of a frame in a group becomes
// the frame for the current time, one step every cfg_water_ms. A group is only retimed once
// it has been seen animating (exactly one of its names per frame, several of them over the
// last 64 frames), so a city that uses those names for plain textures is left alone.
#define TERRAIN_BIND_RET 0x004abc7a
#define WATER_FIRST 2
#define WATER_GROUPS 2
#define WATER_FRAMES 5
static int cfg_water_ms = 67;  // 2 frames at 30 fps
static unsigned water_seen_now[WATER_GROUPS];  // bitmask of the group's names bound this frame
static int water_clean[WATER_GROUPS], water_mixed[WATER_GROUPS];  // frames with 1 / 2+ names
static unsigned water_seen_any[WATER_GROUPS];
static int water_retimed[WATER_GROUPS];

void opengl_set_water_ms(int ms) { cfg_water_ms = ms; }

static void water_frame(void) {
    for (int g = 0; g < WATER_GROUPS; g++) {
        int n = __builtin_popcount(water_seen_now[g]);
        if (n == 1) water_clean[g]++;
        if (n > 1) water_mixed[g]++;
        water_seen_any[g] |= water_seen_now[g];
        water_seen_now[g] = 0;
        // Frames with two names happen now and then (the mirror redraws the terrain, the
        // game steps between the two passes); plain textures would do it most of the time.
        // Decide on recent history: counts are halved every 256 frames.
        int on = water_clean[g] >= 32 && water_mixed[g] * 4 <= water_clean[g] &&
                 __builtin_popcount(water_seen_any[g]) >= 3;
        if (water_clean[g] + water_mixed[g] >= 256) water_clean[g] /= 2, water_mixed[g] /= 2;
        if (on != water_retimed[g]) {
            static int switches;
            water_retimed[g] = on;
            if (++switches <= 20)
                logmsg("water: texture group %d-%d %s\n", WATER_FIRST + g * WATER_FRAMES,
                   WATER_FIRST + g * WATER_FRAMES + WATER_FRAMES - 1,
                   on ? "animates, retimed" : "is not an animation, left alone");
        }
    }
}

// HD water drifts instead. The five original frames are fresh dither noise each (with a slight
// leftward drift), and the upscaler turns each into a different picture (calm, choppy,
// scratched, flat), so flipping between them strobes at any speed. With hdwater=1 every frame
// of a group shows its HD frame WATER_HD_FRAME, scrolled along s through the texture matrix by
// cfg_water_drift texels (of the original 32) per second.
#define WATER_HD_FRAME 2
static int cfg_water_drift = 4;
static int cfg_hd_water;
static int water_scroll_set;
static float water_scroll;
static unsigned hd_lookup_redirect(unsigned game);
static void set_texture_scroll(float s);

void opengl_set_water_drift(int texels_per_s) { cfg_water_drift = texels_per_s; }

static unsigned water_retime(unsigned name, float *scroll) {
    if (cfg_water_ms <= 0 || name < WATER_FIRST || name >= WATER_FIRST + WATER_GROUPS * WATER_FRAMES) return name;
    int g = (name - WATER_FIRST) / WATER_FRAMES;
    water_seen_now[g] |= 1u << ((name - WATER_FIRST) % WATER_FRAMES);
    if (!water_retimed[g]) return name;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    long long ms = now.QuadPart * 1000 / freq.QuadPart;
    unsigned hd_frame = WATER_FIRST + g * WATER_FRAMES + WATER_HD_FRAME;
    if (cfg_hd_water && hd_enabled && hd_lookup_redirect(hd_frame)) {
        // one tile is 32 texels; wrap so the float keeps its precision
        long long period = cfg_water_drift > 0 ? 32000 / cfg_water_drift : 0;
        *scroll = period ? (float)(ms % period) / (float)period : 0;
        return hd_frame;
    }
    unsigned step = (unsigned)(ms / cfg_water_ms);
    return WATER_FIRST + g * WATER_FRAMES + step % WATER_FRAMES;
}

// Water motion probe (texprobe=1): the texture coordinate and position of the first vertex
// drawn with a water texture, once per frame for 24 frames every 5 s. With the camera
// still, anything that changes is the game animating the water by frame.
typedef void(WINAPI *glTexCoord2f_t)(float, float);
static glTexCoord2f_t real_glTexCoord2f;
static float water_tc[2];

static void WINAPI hook_glTexCoord2f(float s, float t) {
    if (water_drawing == 1) water_sum_s += s, water_sum_t += t, water_last_st[0] = s, water_last_st[1] = t;
    if (water_vertex_pending == 2) water_tc[0] = s, water_tc[1] = t, water_vertex_pending = 1;
    real_glTexCoord2f(s, t);
}

static void water_probe_vertex(float x, float y, float w) {
    if (water_vertex_pending != 1) return;
    water_vertex_pending = 0;
    logmsg("water probe: tex %d  st (%.4f, %.4f)  pos (%.2f, %.2f) w %.3f\n", water_bound, water_tc[0], water_tc[1],
           x / w, y / w, w);
}

static void WINAPI hook_glBindTexture(unsigned target, unsigned name) {
    float scroll = 0;
    if ((DWORD)(UINT_PTR)__builtin_return_address(0) == TERRAIN_BIND_RET) {
        if (cfg_tex_probe && water_probe_frames > 0 && water_vertex_pending == 3 && name >= WATER_FIRST &&
            name < WATER_FIRST + WATER_GROUPS * WATER_FRAMES)
            water_bound = (int)name, water_vertex_pending = 2;
        water_drawing = !cfg_tex_probe ? 0 : name >= WATER_FIRST && name < WATER_FIRST + WATER_GROUPS * WATER_FRAMES ? 1 : 2;
        name = water_retime(name, &scroll);
    } else water_drawing = 0;
    if (scroll != 0 || water_scroll_set) set_texture_scroll(scroll);
    if (cfg_tex_probe && name < 2048) {
        probe_frames[name] |= 1;
        probe_site[name] = (DWORD)(UINT_PTR)__builtin_return_address(0);
    }
    game_bound = name;
    unsigned hd = hd_enabled ? hd_lookup_redirect(name) : 0;
    real_glBindTexture(target, hd ? hd : name);
}

static void WINAPI hook_glTexParameteri(unsigned target, unsigned pname, int value) {
    int bound = 0;
    if (hd_enabled && (pname == GL_TEXTURE_MIN_FILTER)) {
        p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        if (is_hd_name((unsigned)bound)) return;  // keep our mipmapped filtering
    }
    real_glTexParameteri_iat(target, pname, value);
}

// Returns our object for this key, creating and uploading it on first use (0 on failure).
static unsigned hd_object(unsigned long long key) {
    unsigned short *slot = hd_object_slot(key);
    if (*slot) return hd_objects[*slot - 1].name;
    if (hd_nobjects >= HD_MAX_OBJECTS) return 0;
    int w = 0, h = 0;
    unsigned long *px = hdtex_load(key, &w, &h);
    if (!px) {
        if (hd_failed++ < 10) logmsg("hd textures: could not load %016llx.png\n", key);
        return 0;
    }
    if (hd_soft_key) soft_key((DWORD *)px, w * h);
    unsigned name = HD_NAME_BASE + (unsigned)hd_nobjects;
    real_glBindTexture(GL_TEXTURE_2D, name);
    p_glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, 1);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, 0x2800 /* MAG_FILTER */, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, 0x2802 /* WRAP_S */, 0x2901 /* REPEAT */);
    p_glTexParameteri(GL_TEXTURE_2D, 0x2803 /* WRAP_T */, 0x2901);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8);
    real_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    hd_objects[hd_nobjects].key = key, hd_objects[hd_nobjects].name = name;
    *slot = (unsigned short)++hd_nobjects;
    if (++hd_uploaded <= 5 || hd_uploaded % 100 == 0)
        logmsg("hd textures: %d loaded (latest %016llx, %dx%d)\n", hd_uploaded, key, w, h);
    return name;
}

// The water animation frames (texture names 2-11) are upscaled one by one, and the AI invents
// different detail in each, so the animation boils. Keep them original unless hdwater=1.

void opengl_set_hd_water(int on) { cfg_hd_water = on; }

// Called for each level-0 colour-index upload. Returns 1 when a replacement took its place.
static int hd_replace(const BYTE *px, int w, int h) {
    unsigned long long key = texture_hash(px, w, h);
    int water = game_bound >= WATER_FIRST && game_bound < WATER_FIRST + WATER_GROUPS * WATER_FRAMES;
    if (hdtex_has(key) && (cfg_hd_water || !water)) {
        unsigned hd = hd_object(key);
        if (hd) {
            hd_set_redirect(game_bound, hd);
            real_glBindTexture(GL_TEXTURE_2D, hd);
            return 1;
        }
    }
    if (hd_lookup_redirect(game_bound)) {
        // the game reuses this name for a texture without a replacement: undo the redirect
        hd_set_redirect(game_bound, 0);
        real_glBindTexture(GL_TEXTURE_2D, game_bound);
    }
    return 0;
}

void opengl_set_hd_dir(const char *dir) {
    if (hd_enabled < 0) {
        logmsg("hd textures: off (glBindTexture/glTexParameteri hooks missing)\n");
        return;
    }
    int n = hdtex_init(dir);
    hd_enabled = n > 0;
    if (hd_enabled) hdtex_preload_start();
    logmsg("hd textures: %d replacements in %s\n", n, dir);
}

void opengl_set_texture_census_dir(const char *dir) {
    snprintf(census_dir, sizeof census_dir, "%s", dir);
    CreateDirectoryA(dir, NULL);
}

static int WINAPI hook_glTexImage2D_impl(unsigned target, int level, int internal, int w, int h, int border,
                                         unsigned fmt, unsigned type, const void *px, DWORD ret) {
    static int font_logged, radar_logged;
    ctx_uploads++;
    if (ret == WORLD_UPLOAD_RET) ctx_world_uploads++;
    int special = CHROMA_KEYED(ret) || ret == RADAR_UPLOAD_RET;
    if (InterlockedIncrement(&textures_logged) <= 30 || (ret == FONT_UPLOAD_RET && !font_logged++) ||
        (ret == RADAR_UPLOAD_RET && !radar_logged++))
        logmsg("opengl: glTexImage2D(%dx%d level %d, internal %04x, fmt %04x, type %04x) from %08lx\n", w, h, level,
               internal, fmt, type, ret);
    if (fmt == GL_COLOR_INDEX && type == GL_UNSIGNED_BYTE && px && w > 0 && h > 0) {
        hd_soft_key = ret == PARTICLE_UPLOAD_RET && w <= 64;
        int replaced = hd_enabled && level == 0 && ret != RADAR_UPLOAD_RET && hd_replace(px, w, h);
        hd_soft_key = 0;
        if (replaced) return 1;
        DWORD *rgba = (DWORD *)indices_to_rgba(px, w, h, special ? PAL_CURRENT : PAL_BASE);
        if (rgba) {
            // Glide draws text with chroma keying against its default key, black, comparing
            // in its 16-bit RGB565 format, so anything that rounds to black there (r < 8,
            // g < 4, b < 8) vanishes. The font's glyph backgrounds are a mix of black and
            // near-black; glyph quads (16 wide, 9 apart) overlap, so unkeyed backgrounds
            // painted boxes over the previous letter. Key the same way Glide does.
            int bound = 0;
            p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            if (ret == PARTICLE_UPLOAD_RET && w <= 64) soft_key(rgba, w * h);
            else if (CHROMA_KEYED(ret) || texture_is_keyed((unsigned)bound)) key_black_rgb565(rgba, w * h);
            static LONG textures_dumped;
            static int font_dumped, radar_dumped;
            if (dump_dir[0] && (special ? (ret == FONT_UPLOAD_RET ? !font_dumped++ : !radar_dumped++)
                                        : InterlockedIncrement(&textures_dumped) <= 64)) {
                char path[MAX_PATH + 64];
                snprintf(path, sizeof path, "%s\\texture_%s_%dx%d_from_%08lx.png", dump_dir,
                         ret == FONT_UPLOAD_RET ? "font" : ret == RADAR_UPLOAD_RET ? "radar" : "other", w, h, ret);
                save_rgba_png((const unsigned long *)rgba, w, h, path);
            }
            if (census_dir[0]) texture_census(px, rgba, w, h, ret);
            real_glTexImage2D(target, level, GL_RGBA, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            return 0;
        }
    }
    real_glTexImage2D(target, level, internal, w, h, border, fmt, type, px);
    return 0;
}

// Primitive census (diagnostics): for a few frames a few seconds into 3D, log every
// glBegin/glEnd batch with its call site, primitive type, vertex count and the raw colour
// bytes passed to glColor3b/glColor4b.
typedef void(WINAPI *glBegin_t)(unsigned);
typedef void(WINAPI *glEnd_t)(void);
typedef void(WINAPI *glColor3b_t)(signed char, signed char, signed char);
typedef void(WINAPI *glColor4b_t)(signed char, signed char, signed char, signed char);
typedef void(WINAPI *glVertex2f_t)(float, float);
typedef void(WINAPI *glVertex4f_t)(float, float, float, float);
static glBegin_t real_glBegin;
static glEnd_t real_glEnd;
static glColor3b_t real_glColor3b;
static glColor4b_t real_glColor4b;
static glVertex2f_t real_glVertex2f;
static glVertex4f_t real_glVertex4f;

static int frame_number, census_on;
static DWORD batch_caller;
static unsigned batch_mode;
static int batch_vertices, batch_colors;
static int first_color[4], last_color[4];
static float first_xy[2], last_xy[2];

static void census_frame_start(void) {
    frame_number++;
    census_on = frame_number >= 150 && frame_number < 153;
    if (frame_number == 150) logmsg("opengl census: frames 150-152\n");
    if (census_on) logmsg("opengl census: frame %d\n", frame_number);
}

static void record_color(int r, int g, int b, int a) {
    int *dst = batch_colors++ == 0 ? first_color : last_color;
    dst[0] = r, dst[1] = g, dst[2] = b, dst[3] = a;
    if (batch_colors > 1) last_color[0] = r, last_color[1] = g, last_color[2] = b, last_color[3] = a;
}

static void record_vertex(float x, float y) {
    if (batch_vertices++ == 0) first_xy[0] = x, first_xy[1] = y;
    last_xy[0] = x, last_xy[1] = y;
}

// Text glyphs: discard the font texture's transparent (keyed) pixels with an alpha test
// instead of letting the game's blend mode turn them black. State can't change between
// glBegin and glEnd, so it is set before glBegin and restored after glEnd.
// The font texture holds white glyphs that Glide tints with the text colour (green clock,
// pink/red quest text) through its colour combiner; the OpenGL port left the texture
// environment on GL_REPLACE, so text came out white. Use GL_MODULATE for text quads.
#define GL_TEXTURE_ENV      0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE         0x2100
typedef void(WINAPI *glTexEnvi_t)(unsigned, unsigned, int);
typedef void(WINAPI *glGetTexEnviv_t)(unsigned, unsigned, int *);
static glTexEnvi_t p_glTexEnvi;
static glGetTexEnviv_t p_glGetTexEnviv;

static int text_batch, text_saved_test, text_saved_blend, text_saved_env;
static float text_saved_func, text_saved_ref;

// Diagnostics for text: the first few glyph draws log the bound texture, environment mode and
// current colour, and save the bound texture as read back from the GPU.
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#define GL_CURRENT_COLOR      0x0B00
#define GL_TEXTURE_WIDTH      0x1000
#define GL_TEXTURE_HEIGHT     0x1001
typedef void(WINAPI *glGetTexImage_t)(unsigned, int, unsigned, unsigned, void *);
typedef void(WINAPI *glGetTexLevelParameteriv_t)(unsigned, int, unsigned, int *);
static glGetTexImage_t p_glGetTexImage;
static glGetTexLevelParameteriv_t p_glGetTexLevelParameteriv;

static void probe_text_state(void) {
    static int probes;
    if (probes >= 3 || !p_glGetTexImage || !p_glGetTexLevelParameteriv) return;
    probes++;
    int bound = -1, env = -1, w = 0, h = 0, filter = -1;
    float colour[4] = {0};
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    p_glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
    p_glGetFloatv(GL_CURRENT_COLOR, colour);
    p_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    p_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    p_glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
    logmsg("text probe %d: bound texture %d (%dx%d, min filter %04x), env %04x, colour (%.2f %.2f %.2f %.2f), "
           "texturing %s\n", probes, bound, w, h, filter, env, colour[0], colour[1], colour[2], colour[3],
           p_glIsEnabled(GL_TEXTURE_2D) ? "on" : "OFF");
    if (dump_dir[0] && w > 0 && h > 0 && w * h <= 1024 * 1024) {
        DWORD *px = malloc((size_t)w * h * 4);
        if (px) {
            p_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            char path[MAX_PATH + 64];
            snprintf(path, sizeof path, "%s\\text_probe_%d_texture_%d.png", dump_dir, probes, bound);
            logmsg("text probe %d: saved %s (%s)\n", probes, path,
                   save_rgba_png((const unsigned long *)px, w, h, path) ? "ok" : "FAILED");
            free(px);
        }
    }
}

// Flat-polygon census (diagnostics, whole session): every distinct kind of untextured batch
// (call site, blend state, first colour) is logged once. Used to work out how shadows and
// other flat-shaded polygons are drawn, without needing to aim the camera at them.
#define GL_BLEND_DST 0x0BE0
#define GL_BLEND_SRC 0x0BE1
static int flat_batch, flat_colours;
static DWORD flat_caller;
static int flat_blend, flat_src, flat_dst;
static float flat_current[4];
static int flat_first[4];

static void flat_record_colour(int r, int g, int b, int a) {
    if (flat_batch && flat_colours++ == 0) flat_first[0] = r, flat_first[1] = g, flat_first[2] = b, flat_first[3] = a;
}

static void flat_batch_end(void) {
    static struct { DWORD caller; int blend, src, dst, r, g, b, a; } seen[96];
    static int nseen;
    int r = flat_colours ? flat_first[0] : (int)(flat_current[0] * 255), g = flat_colours ? flat_first[1] : (int)(flat_current[1] * 255);
    int b = flat_colours ? flat_first[2] : (int)(flat_current[2] * 255), a = flat_colours ? flat_first[3] : (int)(flat_current[3] * 255);
    for (int i = 0; i < nseen; i++)
        if (seen[i].caller == flat_caller && seen[i].blend == flat_blend && seen[i].src == flat_src &&
            seen[i].dst == flat_dst && seen[i].r == r && seen[i].g == g && seen[i].b == b && seen[i].a == a)
            return;
    if (nseen >= 96) return;
    seen[nseen].caller = flat_caller, seen[nseen].blend = flat_blend, seen[nseen].src = flat_src;
    seen[nseen].dst = flat_dst, seen[nseen].r = r, seen[nseen].g = g, seen[nseen].b = b, seen[nseen].a = a;
    nseen++;
    logmsg("flat polygon: from %08lx blend %s (src %04x dst %04x) colour (%d,%d,%d,%d)%s\n", flat_caller,
           flat_blend ? "on" : "off", flat_src, flat_dst, r, g, b, a, flat_colours ? "" : " [current colour]");
}

// Textures used for text get Glide's black chroma key. Which upload fills the font texture
// varies, so key it where it is used: the first glyph drawn with a texture reads it back,
// keys near-black (RGB565 black) to transparent and re-uploads it; later uploads into a
// keyed texture are keyed in the glTexImage2D hook.
static unsigned keyed_textures[16];
static int nkeyed;

static int texture_is_keyed(unsigned id) {
    for (int i = 0; i < nkeyed; i++)
        if (keyed_textures[i] == id) return 1;
    return 0;
}

static int key_black_rgb565(DWORD *rgba, int n) {
    int keyed = 0;
    for (int i = 0; i < n; i++) {
        DWORD p = rgba[i];
        if ((p & 0xFF) < 8 && (p >> 8 & 0xFF) < 4 && (p >> 16 & 0xFF) < 8 && p != 0) {
            rgba[i] = 0;
            keyed++;
        }
    }
    return keyed;
}

static void key_bound_text_texture(void) {
    int bound = 0, w = 0, h = 0;
    p_glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (texture_is_keyed((unsigned)bound) || nkeyed >= 16) return;
    if (is_hd_name((unsigned)bound)) {
        // A font sheet got an HD replacement; its soft edges can't be keyed like the original.
        keyed_textures[nkeyed++] = (unsigned)bound;
        logmsg("hd textures: %016llx is a font sheet, don't upscale it (add it to SKIP_KEYS)\n",
               hd_objects[bound - HD_NAME_BASE].key);
        return;
    }
    p_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    p_glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    keyed_textures[nkeyed++] = (unsigned)bound;
    if (w <= 0 || h <= 0 || w * h > 1024 * 1024) return;
    DWORD *px = malloc((size_t)w * h * 4);
    if (!px) return;
    p_glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int keyed = key_black_rgb565(px, w * h);
    real_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    logmsg("text: keyed texture %d (%dx%d): %d near-black pixels made transparent\n", bound, w, h, keyed);
}

// 2D primitives laid out for 640 (text glyphs, radar map, radar lines) are shifted by
// ws_hud_off for widescreen, like the glDrawPixels overlays.
#define RADAR_BEGIN_RET 0x004ae852  // FUN_004ae7f0: radar map quad
#define RADAR_LINES_RET 0x004aec63  // FUN_004aea80: radar heading lines
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW   0x1700
typedef void(WINAPI *glMatrixMode_t)(unsigned);
typedef void(WINAPI *glVoid_t)(void);
typedef void(WINAPI *glTranslatef_t)(float, float, float);
static glMatrixMode_t p_glMatrixMode;
static glVoid_t p_glPushMatrix, p_glPopMatrix;
static glTranslatef_t p_glTranslatef;
static int hud_shifted, hud_saved_mode;
#define GL_TEXTURE_MATRIX_MODE 0x1702
static glVoid_t p_glLoadIdentity;

// Water drift: translate the texture matrix along s; 0 puts it back to identity.
static void set_texture_scroll(float s) {
    if (s == water_scroll && water_scroll_set == (s != 0)) return;
    int mode = GL_MODELVIEW;
    p_glGetIntegerv(GL_MATRIX_MODE, &mode);
    p_glMatrixMode(GL_TEXTURE_MATRIX_MODE);
    p_glLoadIdentity();
    if (s != 0) p_glTranslatef(s, 0, 0);
    p_glMatrixMode((unsigned)mode);
    water_scroll = s, water_scroll_set = s != 0;
}

// Sprite depth. The game draws back to front without a depth buffer, sorting each polygon
// by one depth. Far away a tree sprite and the hill behind it sort almost equal and swap
// between frames, so distant trees flicker or show through hills (original game, more
// visible in widescreen). World polygons now write their depth (GL_ALWAYS, so the game's
// order still decides what is drawn) and sprites are depth-tested against it without
// writing. World vertices carry w = view depth (glVertex4f(x*w, y*w, 0, w), w = v[2] /
// 4096); a sprite's depth is its first vertex's v[2] (FUN_004ad140, list at 0x9bb2e8).
// Sprites are never drawn in the mirror (FUN_004ad140 returns when the view has a y offset).
//
// Two traps from the first attempt:
//  * fixed-function fog is computed from eye-space z. The game passes z = 0, so in OpenGL
//    its fog never showed. Fog now reads an explicit fog coordinate, held at 0, so it stays
//    exactly as before whatever z the vertices carry;
//  * the game makes a new GL context at every menu round trip. Cached state is reset when
//    a context is created and re-synced at the start of every frame.
#define WORLD_TEX_BEGIN_RET 0x004abc82  // FUN_004abc00: textured world polygons
#define WORLD_BEGIN_RET     0x004acf2a  // FUN_004ac030: world polygons
#define SPRITE_BEGIN_RET    0x004ad780  // FUN_004ad140: billboard sprites (trees, pedestrians)
#define SPRITE_VERTS        (*(int **)0x009bb2e8)
#define GL_DEPTH_TEST       0x0B71
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_DEPTH_BITS       0x0D56
#define GL_ALWAYS           0x0207
#define GL_LEQUAL           0x0203
#define GL_FOG_COORD_SRC    0x8450
#define GL_FOG_COORD        0x8451
#define DEPTH_NEAR   24.0f   // polygons are clipped at z 0x30000 (w 48), so w stays above this
#define SPRITE_BIAS  0.98f   // pull sprites slightly forward so their base isn't cut by the ground
typedef void(WINAPI *glDepthFunc_t)(unsigned);
typedef void(WINAPI *glDepthMask_t)(unsigned char);
typedef void(WINAPI *glVertex3f_t)(float, float, float);
typedef void(WINAPI *glFogi_t)(unsigned, int);
typedef void(WINAPI *glFogCoordf_t)(float);
typedef PROC(WINAPI *wglGetProcAddress_t)(LPCSTR);
static glDepthFunc_t p_glDepthFunc;
static glDepthMask_t p_glDepthMask;
static glVertex3f_t p_glVertex3f;
static glFogi_t p_glFogi;
static wglGetProcAddress_t p_wglGetProcAddress;
enum { DEPTH_OFF, DEPTH_WORLD, DEPTH_SPRITE };
static int depth_wanted = 1, depth_ready, depth_mode;  // depth_ready: 0 unchecked, 1 on, -1 off
static LONG depth_logged;
static float sprite_z;

void opengl_set_sprite_depth(int on) { depth_wanted = on; }
void opengl_context_created(int fresh) {
    if (fresh) hd_forget_all();  // our texture objects died with the old context
    depth_ready = 0;
    depth_mode = DEPTH_OFF;
    logmsg("opengl: previous context had %d texture uploads (%d world)\n", ctx_uploads, ctx_world_uploads);
    ctx_uploads = ctx_world_uploads = 0;
}

// The z to pass so that normalized depth is 1 - 2n/w: hyperbolic like a normal perspective
// depth buffer. gluOrtho2D(…, -1, 1) on the modelview negates z, hence the sign.
static float depth_z(float w) {
    float ndc = w > 0 ? 1.0f - 2.0f * DEPTH_NEAR / w : -1.0f;
    if (ndc < -1.0f) ndc = -1.0f;
    if (ndc > 1.0f) ndc = 1.0f;
    return -ndc * w;
}

static void set_depth_mode(DWORD ret) {
    int want = depth_ready <= 0 ? DEPTH_OFF
             : ret == WORLD_TEX_BEGIN_RET || ret == WORLD_BEGIN_RET ? DEPTH_WORLD
             : ret == SPRITE_BEGIN_RET && SPRITE_VERTS ? DEPTH_SPRITE : DEPTH_OFF;
    if (want == DEPTH_SPRITE) {
        float w = SPRITE_VERTS[2] / 4096.0f * SPRITE_BIAS;
        sprite_z = w > 0 ? depth_z(w) / w : 1.0f;  // drawn with glVertex3f, so w = 1
    }
    if (want == depth_mode) return;
    if (want == DEPTH_OFF) {
        p_glDisable(GL_DEPTH_TEST);
    } else {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(want == DEPTH_WORLD ? GL_ALWAYS : GL_LEQUAL);
        p_glDepthMask(want == DEPTH_WORLD);
    }
    depth_mode = want;
}

// Start of each frame (glViewport, with the game's context current).
static void depth_frame_start(void) {
    if (!depth_wanted || depth_ready < 0 || !p_glDepthFunc || !p_glDepthMask || !p_glVertex3f || !p_glFogi) return;
    if (!depth_ready) {
        int bits = 0;
        p_glGetIntegerv(GL_DEPTH_BITS, &bits);
        glFogCoordf_t fog_coord = p_wglGetProcAddress ? (glFogCoordf_t)p_wglGetProcAddress("glFogCoordf") : NULL;
        depth_ready = bits >= 16 && fog_coord ? 1 : -1;
        if (depth_ready > 0) {
            p_glFogi(GL_FOG_COORD_SRC, GL_FOG_COORD);
            fog_coord(0.0f);  // current fog coordinate: eye distance 0, as with the game's z = 0
        }
        if (InterlockedIncrement(&depth_logged) <= 3)
            logmsg("opengl: depth buffer %d bits, fog coordinate %s, sprite depth test %s\n", bits,
                   fog_coord ? "ok" : "MISSING", depth_ready > 0 ? "on" : "off");
        if (depth_ready < 0) return;
    }
    // Known state for the new frame, then clear last frame's depth.
    p_glDisable(GL_DEPTH_TEST);
    depth_mode = DEPTH_OFF;
    int scissor = p_glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) p_glDisable(GL_SCISSOR_TEST);
    p_glDepthMask(1);
    real_glClear(GL_DEPTH_BUFFER_BIT);
    if (scissor) p_glEnable(GL_SCISSOR_TEST);
}

static void WINAPI hook_glBegin(unsigned mode) {
    DWORD ret = (DWORD)(UINT_PTR)__builtin_return_address(0);
    set_depth_mode(ret);
    if (ws_hud_off && (ret == TEXT_BEGIN_RET || ret == RADAR_BEGIN_RET || ret == RADAR_LINES_RET)) {
        hud_shifted = 1;
        p_glGetIntegerv(GL_MATRIX_MODE, &hud_saved_mode);
        p_glMatrixMode(GL_MODELVIEW);
        p_glPushMatrix();
        p_glTranslatef((float)ws_hud_off, 0, 0);
    }
    if (ret == TEXT_BEGIN_RET) key_bound_text_texture();
    flat_batch = ret != TEXT_BEGIN_RET && !p_glIsEnabled(GL_TEXTURE_2D);
    if (flat_batch) {
        flat_caller = ret;
        flat_colours = 0;
        flat_blend = p_glIsEnabled(GL_BLEND);
        p_glGetIntegerv(GL_BLEND_SRC, &flat_src);
        p_glGetIntegerv(GL_BLEND_DST, &flat_dst);
        p_glGetFloatv(GL_CURRENT_COLOR, flat_current);
    }
    if (ret == TEXT_BEGIN_RET) {
        probe_text_state();
        text_batch = 1;
        text_saved_test = p_glIsEnabled(GL_ALPHA_TEST);
        text_saved_blend = p_glIsEnabled(GL_BLEND);
        p_glGetFloatv(GL_ALPHA_TEST_FUNC, &text_saved_func);
        p_glGetFloatv(GL_ALPHA_TEST_REF, &text_saved_ref);
        p_glEnable(GL_ALPHA_TEST);
        p_glAlphaFunc(GL_GREATER, 0.0f);
        if (text_saved_blend) p_glDisable(GL_BLEND);
        p_glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &text_saved_env);
        p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }
    if (census_on) {
        batch_caller = ret;
        batch_mode = mode;
        batch_vertices = batch_colors = 0;
    }
    real_glBegin(mode);
}

static void WINAPI hook_glEnd(void) {
    real_glEnd();
    if (hud_shifted) {
        hud_shifted = 0;
        p_glPopMatrix();
        p_glMatrixMode(hud_saved_mode);
    }
    if (flat_batch) {
        flat_batch = 0;
        flat_batch_end();
    }
    if (text_batch) {
        text_batch = 0;
        p_glAlphaFunc((unsigned)text_saved_func, text_saved_ref);
        if (!text_saved_test) p_glDisable(GL_ALPHA_TEST);
        if (text_saved_blend) p_glEnable(GL_BLEND);
        p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, text_saved_env);
    }
    if (census_on)
        logmsg("  begin(%u) from %08lx: %d verts (%.0f,%.0f)..(%.0f,%.0f), %d colors first=(%d,%d,%d,%d) last=(%d,%d,%d,%d)\n",
               batch_mode, batch_caller, batch_vertices, first_xy[0], first_xy[1], last_xy[0], last_xy[1], batch_colors,
               first_color[0], first_color[1], first_color[2], first_color[3], last_color[0], last_color[1],
               last_color[2], last_color[3]);
}

// The game passes 0-255 colour values to glColor3b/glColor4b, which take *signed* bytes:
// anything above 127 wrapped negative and clamped to 0 (text alpha 255 became 0, so glyphs
// drew as black boxes; bright radar colours went black). Forward them as unsigned bytes.
typedef void(WINAPI *glColor3ub_t)(unsigned char, unsigned char, unsigned char);
typedef void(WINAPI *glColor4ub_t)(unsigned char, unsigned char, unsigned char, unsigned char);
static glColor3ub_t p_glColor3ub;
static glColor4ub_t p_glColor4ub;

// 3D polygon colours in FUN_004ac030's OpenGL branch, by call site:
//  * per-vertex shaded colour from the lighting table (ret 0x4acf6f), immediately followed by
//    an overwrite with the stale material colour at 0x9bd440 (ret 0x4ad045). Glide uses the
//    shaded colour (it updates 0x9bd440); OpenGL painted shadows and dark ground patches with
//    whatever colour was left there (bright blue). Use the shaded colour for the overwrite.
//  * shadow / glow polygons (material type 0xb): glColor4b(0,0,0,0) (ret 0x4ad02b) and
//    (1,1,1,0) (ret 0x4ad021), alpha 0 = invisible. Glide: shadows multiply the ground by the
//    alpha of the constant at 0x62acbc; glows add colour 0x62acb8 with alpha
//    (0x538 - material[0x28] * 0x18). The blend mode is chosen in hook_glBlendFunc.
#define SHADED_COLOUR_RET   0x004acf6f
#define MATERIAL_COLOUR_RET 0x004ad045
#define SHADOW_COLOUR_RET   0x004ad02b
#define GLOW_COLOUR_RET     0x004ad021
#define SHADOW_BLEND_RET    0x004acf22  // glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA) before type-0xb polygons
#define CURRENT_MATERIAL    (*(BYTE **)0x009bd414)
#define GLIDE_SHADOW_COLOUR (*(DWORD *)0x0062acbc)
#define GLIDE_GLOW_COLOUR   (*(DWORD *)0x0062acb8)

static int shaded_pending;
static unsigned char shaded_rgb[3];

static void WINAPI hook_glColor3b(signed char r, signed char g, signed char b) {
    DWORD ret = (DWORD)(UINT_PTR)__builtin_return_address(0);
    if (census_on) record_color(r, g, b, -999);
    if (ret == SHADED_COLOUR_RET) {
        shaded_rgb[0] = (unsigned char)r, shaded_rgb[1] = (unsigned char)g, shaded_rgb[2] = (unsigned char)b;
        shaded_pending = 1;
    } else if (ret == MATERIAL_COLOUR_RET && shaded_pending) {
        r = (signed char)shaded_rgb[0], g = (signed char)shaded_rgb[1], b = (signed char)shaded_rgb[2];
    }
    flat_record_colour((unsigned char)r, (unsigned char)g, (unsigned char)b, -1);
    p_glColor3ub((unsigned char)r, (unsigned char)g, (unsigned char)b);
}

static int glow_alpha(void) {
    BYTE *material = CURRENT_MATERIAL;
    DWORD v = material && !IsBadReadPtr(material + 0x28, 4) ? *(DWORD *)(material + 0x28) : 0x30;
    return (int)((0x538 - v * 0x18) & 0xFF);
}

static void WINAPI hook_glColor4b(signed char r, signed char g, signed char b, signed char a) {
    DWORD ret = (DWORD)(UINT_PTR)__builtin_return_address(0);
    if (census_on) record_color(r, g, b, a);
    if (ret == SHADOW_COLOUR_RET) {
        // Glide: dst * A. OpenGL here: blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) with black,
        // i.e. dst * (1 - a), so a = 1 - A.
        unsigned char glide_a = (unsigned char)(GLIDE_SHADOW_COLOUR >> 24);
        p_glColor4ub(0, 0, 0, (unsigned char)(255 - glide_a));
        return;
    }
    if (ret == GLOW_COLOUR_RET) {
        DWORD c = GLIDE_GLOW_COLOUR;
        p_glColor4ub((unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c, (unsigned char)glow_alpha());
        return;
    }
    flat_record_colour((unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
    p_glColor4ub((unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a);
}

// Polygons with material flag 0x20 whose texture is switched off get no colour at all in the
// OpenGL branch, so they too showed whatever colour was set last (bright green strips along
// buildings). Glide colours them brightness x (0.95, 0.90, 0.85), with brightness (0-255)
// computed per polygon into 0x9bb2fc. glShadeModel(GL_FLAT) (ret 0x4acedc) runs just
// before their glBegin, so set that colour there.
#define FLAT_SHADE_RET   0x004acedc
#define GLIDE_BRIGHTNESS (*(float *)0x009bb2fc)

typedef void(WINAPI *glShadeModel_t)(unsigned);
static glShadeModel_t real_glShadeModel;

static void WINAPI hook_glShadeModel(unsigned mode) {
    real_glShadeModel(mode);
    if ((DWORD)(UINT_PTR)__builtin_return_address(0) != FLAT_SHADE_RET) return;
    BYTE *material = CURRENT_MATERIAL;
    if (!material || IsBadReadPtr(material, 0x14) || !(material[9] & 0x20) || *(int *)(material + 0x10) == 0xb ||
        p_glIsEnabled(GL_TEXTURE_2D))
        return;
    float b = GLIDE_BRIGHTNESS;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    p_glColor4ub((unsigned char)(b * 0.95f), (unsigned char)(b * 0.90f), (unsigned char)(b * 0.85f), 255);
}

typedef void(WINAPI *glBlendFunc_t)(unsigned, unsigned);
static glBlendFunc_t real_glBlendFunc;
static LONG shadow_kinds_logged;

static void WINAPI hook_glBlendFunc(unsigned src, unsigned dst) {
    if ((DWORD)(UINT_PTR)__builtin_return_address(0) == SHADOW_BLEND_RET) {
        BYTE *material = CURRENT_MATERIAL;
        DWORD v = material && !IsBadReadPtr(material + 0x28, 4) ? *(DWORD *)(material + 0x28) : 0;
        if (InterlockedIncrement(&shadow_kinds_logged) <= 10)
            logmsg("opengl: type-0xb polygon, material value %lu: %s\n", v, v >= 0x30 ? "glow (additive)" : "shadow");
        // Many of these polygons never set a per-vertex colour (Glide used a constant colour
        // instead), so they inherited whatever was set last (green radar lines, black, blue)
        // and flickered. Set the shadow/glow colour here, before glBegin.
        if (v >= 0x30) {
            DWORD c = GLIDE_GLOW_COLOUR;
            p_glColor4ub((unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c, (unsigned char)glow_alpha());
            real_glBlendFunc(0x0302 /* GL_SRC_ALPHA */, 1 /* GL_ONE */);
            return;
        }
        p_glColor4ub(0, 0, 0, (unsigned char)(255 - (GLIDE_SHADOW_COLOUR >> 24)));
    }
    real_glBlendFunc(src, dst);
}

static void WINAPI hook_glVertex2f(float x, float y) {
    shaded_pending = 0;
    if (census_on) record_vertex(x, y);
    if (depth_mode == DEPTH_SPRITE) p_glVertex3f(x, y, sprite_z);
    else real_glVertex2f(x, y);
}

static void WINAPI hook_glVertex4f(float x, float y, float z, float w) {
    shaded_pending = 0;
    if (water_vertex_pending == 1) water_probe_vertex(x, y, w);
    if (water_drawing == 1 && w) {
        water_nverts++, water_sum_x += x / w, water_sum_y += y / w, water_sum_w += w;
        int *n = &water_verts_n[water_verts_cur];
        if (*n < WATER_PROBE_VERTS) {
            float *v = water_verts[water_verts_cur][(*n)++];
            v[0] = x / w, v[1] = y / w, v[2] = w, v[3] = water_last_st[0], v[4] = water_last_st[1];
        }
    }
    if (water_drawing == 2 && w) land_nverts++, land_sum_y += y / w, land_sum_w += w;
    if (census_on) record_vertex(w ? x / w : x, w ? y / w : y);
    if (depth_mode == DEPTH_WORLD && z == 0) z = depth_z(w);
    real_glVertex4f(x, y, z, w);
}

// FUN_004aed80(color, x0, y0, x1, y1) fills a rectangle with a 0x00RRGGBB colour (used, for
// example, to paint the rear-view mirror's sky before drawing into it). Only its Glide branch
// was ever written, so in OpenGL the mirror kept whatever the main view left there. Detour it:
// fill with a scissored clear in OpenGL, otherwise run the original.
#define FILL_RECT_FN    0x004aed80
#define FILL_RECT_RESUME 0x004aed8a  // the jne after the two relocated instructions
#define RENDERER        (*(int *)0x0062ac88)

typedef void(__cdecl *fill_rect_t)(DWORD, int, int, int, int);
static fill_rect_t original_fill_rect;
static LONG fill_rects_logged;

static void __cdecl fill_rect(DWORD color, int x0, int y0, int x1, int y1) {
    if (RENDERER != 2) {
        original_fill_rect(color, x0, y0, x1, y1);
        return;
    }
    if (InterlockedIncrement(&fill_rects_logged) <= 10)
        logmsg("opengl: fill rect %06lx (%d,%d)-(%d,%d)\n", color & 0xFFFFFF, x0, y0, x1, y1);
    float saved_colour[4];
    int saved_box[4];
    p_glGetFloatv(GL_COLOR_CLEAR_VALUE, saved_colour);
    p_glGetIntegerv(GL_SCISSOR_BOX, saved_box);
    int saved_test = p_glIsEnabled(GL_SCISSOR_TEST);
    int wx, wy, ww, wh;
    game_rect_to_window(x0 + ws_hud_off, y0, x1 - x0 + 1, y1 - y0 + 1, &wx, &wy, &ww, &wh);
    p_glEnable(GL_SCISSOR_TEST);
    p_glScissor(wx, wy, ww, wh);
    p_glClearColor((color >> 16 & 0xFF) / 255.0f, (color >> 8 & 0xFF) / 255.0f, (color & 0xFF) / 255.0f, 1.0f);
    real_glClear(GL_COLOR_BUFFER_BIT);
    p_glClearColor(saved_colour[0], saved_colour[1], saved_colour[2], saved_colour[3]);
    p_glScissor(saved_box[0], saved_box[1], saved_box[2], saved_box[3]);
    if (!saved_test) p_glDisable(GL_SCISSOR_TEST);
}

static int install_fill_rect(void) {
    static const BYTE expected[] = {0x83, 0xEC, 0x10, 0x83, 0x3D, 0x88, 0xAC, 0x62, 0x00, 0x01};
    BYTE *site = (BYTE *)FILL_RECT_FN;
    if (IsBadReadPtr(site, sizeof expected) || memcmp(site, expected, sizeof expected) != 0) return 0;
    BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, expected, sizeof expected);  // sub esp,10h / cmp [renderer],1
    t[sizeof expected] = 0xE9;             // jmp to the original jne
    *(DWORD *)(t + sizeof expected + 1) = FILL_RECT_RESUME - ((DWORD)(UINT_PTR)t + sizeof expected + 5);
    original_fill_rect = (fill_rect_t)t;
    DWORD old;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE9;
    *(DWORD *)(site + 1) = (DWORD)(UINT_PTR)fill_rect - ((DWORD)FILL_RECT_FN + 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

// Colour byte order: the OpenGL branches of the text colour setter (FUN_004ae700) and the
// radar line drawer (FUN_004aea80) unpack a 0x00RRGGBB colour with shifts of 24/16/8, i.e.
// (0, R, G): blue is lost, red lands in green, green in blue (pink quest text drew green,
// the green clock blue). Patch the shift immediates to 16/8/0.
static int patch_byte(DWORD addr, BYTE expected, BYTE value) {
    BYTE *p = (BYTE *)addr;
    if (IsBadReadPtr(p, 1) || *p != expected) return 0;
    DWORD old;
    VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old);
    *p = value;
    VirtualProtect(p, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 1);
    return 1;
}

static int install_colour_shift_fix(void) {
    static const struct { DWORD addr; BYTE from, to; } fixes[] = {
        {0x004ae77d, 0x08, 0x00}, {0x004ae783, 0x10, 0x08}, {0x004ae786, 0x18, 0x10},  // text: shr 8/16/24
        {0x004aec49, 0x08, 0x00}, {0x004aec4f, 0x10, 0x08}, {0x004aec52, 0x18, 0x10},  // radar lines
    };
    int ok = 0;
    for (int i = 0; i < (int)(sizeof fixes / sizeof fixes[0]); i++) ok += patch_byte(fixes[i].addr, fixes[i].from, fixes[i].to);
    return ok;
}

// ---- Effects the OpenGL renderer never got: particles and the Apocalypse flash ----

#define FX_QUADS                 0x0007
#define FX_SRC_ALPHA             0x0302
#define FX_ONE_MINUS_SRC_ALPHA   0x0303
#define FX_ONE                   1
#define FX_REPLACE               0x1E01
#define FX_PROJECTION            0x1701
#define FX_TEXTURE               0x1702
#define FX_CULL_FACE             0x0B44
#define FX_LIGHTING              0x0B50
#define FX_MAP_COLOR             0x0D10
#define FX_FRONT                 0x0404
#define FX_BACK                  0x0405
#define FX_RGB                   0x1907
#define FX_CLAMP_TO_EDGE         0x812F
#define FX_ALL_ATTRIB_BITS       0x000FFFFF
typedef void(WINAPI *glUint_t)(unsigned);
typedef void(WINAPI *glPixelTransferi_t)(unsigned, int);
typedef void(WINAPI *glCopyTexImage2D_t)(unsigned, int, unsigned, int, int, int, int, int);
typedef void(WINAPI *glReadPixels_t)(int, int, int, int, unsigned, unsigned, void *);
typedef void(WINAPI *glColor4f_t)(float, float, float, float);
typedef HGLRC(WINAPI *wglGetCurrentContext_t)(void);
typedef BOOL(WINAPI *SwapBuffers_fn)(HDC);
static glUint_t p_glPushAttrib, p_glReadBuffer;
static glVoid_t p_glPopAttrib;
static glPixelTransferi_t p_glPixelTransferi;
static glCopyTexImage2D_t p_glCopyTexImage2D;
static glReadPixels_t p_glReadPixels;
static glColor4f_t p_glColor4f;
static wglGetCurrentContext_t p_wglGetCurrentContext;
static SwapBuffers_fn p_SwapBuffers;

static int fx_ready(void) {
    return p_glPushAttrib && p_glPopAttrib && p_glReadBuffer && p_glPixelTransferi && p_glCopyTexImage2D &&
           p_glReadPixels && p_glColor4f && p_wglGetCurrentContext && p_SwapBuffers && real_glBegin && real_glEnd &&
           real_glTexCoord2f && real_glVertex2f && real_glBindTexture;
}

// Write a 5-byte jmp (0xE9) or call (0xE8) at site.
static void put_branch(DWORD site, BYTE op, const void *target) {
    BYTE *p = (BYTE *)site;
    DWORD old;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
    p[0] = op;
    *(DWORD *)(p + 1) = (DWORD)(UINT_PTR)target - (site + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

// Particles: smoke, fire, spray and debris puffs. FUN_004ade20 draws each effect as up to
// 30 small textured squares scattered at random around the effect's screen position, sized
// by distance, every frame. It only has a Glide branch (it returns at once unless the
// renderer is 1), so in OpenGL burning cars and explosions had no smoke or fire at all.
// Same layout here. Textures (FUN_004b69b0, uploaded in OpenGL too, names stored at):
//   0x7c spray 0xb272c0 (types 0, 3)        0x7d smoke 0xb27260 (1, 4, 6, 7, 11)
//   0x7e debris 0xb27a30 (8, 9)             0x7f fire 0xb27a10 (2, 5, 10; fog off in Glide)
// Glide chroma-keyed black; here dark texels fade out instead (soft_key), and fire adds
// light. Positions re-roll at 30 Hz (the original rate), not every frame.
#define PARTICLE_FN         0x004ade20
#define PARTICLE_RESUME     0x004ade2a  // the jne after sub esp,34h / cmp [renderer],1
#define PARTICLE_FAR        0x9780000
#define SCREEN_OFF_X        (*(double *)0x00629410)
#define SCREEN_OFF_Y        (*(double *)0x00629418)
typedef void(__cdecl *particles_t)(int *);
static particles_t original_particles;
static LONG particles_logged;

// Dark texels transparent, ramping to opaque: Glide keyed black with hard edges.
static void soft_key(DWORD *rgba, int n) {
    for (int i = 0; i < n; i++) {
        DWORD p = rgba[i];
        int m = (int)(p & 0xFF), g = (int)(p >> 8 & 0xFF), b = (int)(p >> 16 & 0xFF);
        if (g > m) m = g;
        if (b > m) m = b;
        int a = m <= 10 ? 0 : m >= 50 ? 255 : (m - 10) * 255 / 40;
        if (a < (int)(p >> 24)) rgba[i] = (p & 0xFFFFFF) | (DWORD)a << 24;
    }
}

static unsigned particle_rand(unsigned *s) {
    *s = *s * 1103515245u + 12345u;
    return *s >> 16 & 0x7FFF;
}

static void __cdecl particles(int *e) {
    if (RENDERER != 2 || !fx_ready()) {
        original_particles(e);
        return;
    }
    const int *v = (const int *)e[1];
    unsigned type = (unsigned)e[3] & 0x7FFFFFFF;
    int d = PARTICLE_FAR - v[2];
    if (d <= 0 || type > 11 || SCREEN_OFF_Y != 0) return;  // no particles in the mirror, like sprites
    int f = (int)(((long long)d << 16) / PARTICLE_FAR);    // 1.0 up close, 0 at the far limit
    int lo, hi, count, spread;
    switch (type) {
    case 0:            lo = 2 * f, hi = 7 * f, count = 15 * f, spread = count; break;
    case 3:            lo = 2 * f, hi = 3 * f, count = 12 * f, spread = count; break;
    case 4: case 6: case 8: lo = 2 * f, hi = 5 * f, count = 10 * f, spread = count; break;
    case 5:            lo = f, hi = 3 * f, count = 12 * f, spread = count; break;
    case 10: case 11:  lo = f, hi = 10 * f, count = 15 * f, spread = count; break;
    default:           lo = f, hi = 40 * f, count = 9 * f, spread = 20 * f; break;  // 1, 2, 7, 9
    }
    lo >>= 16, hi >>= 16, count >>= 16, spread >>= 16;
    if (count > 30) count = 30;
    if (count <= 0 || spread <= 0) return;
    int fire = type == 2 || type == 5 || type == 10;
    DWORD tex = *(DWORD *)(type == 1 || type == 4 || type == 11 || type == 7 || type == 6 ? 0x00b27260
                           : type == 9 || type == 8                                       ? 0x00b27a30
                           : fire                                                         ? 0x00b27a10
                                                                                          : 0x00b272c0);
    if (InterlockedIncrement(&particles_logged) <= 10)
        logmsg("particles: type %u at (%d,%d) depth %d: %d squares of %d-%d px, texture %lu\n", type, v[0] >> 12,
               v[1] >> 12, v[2], count, lo * 2, hi * 2, tex);
    unsigned hd = hd_enabled ? hd_lookup_redirect(tex) : 0;

    p_glPushAttrib(FX_ALL_ATTRIB_BITS);
    p_glMatrixMode(FX_TEXTURE);  // the water drift may have left a texture offset
    p_glPushMatrix();
    p_glLoadIdentity();
    p_glDisable(GL_ALPHA_TEST);
    p_glDisable(FX_CULL_FACE);
    p_glEnable(GL_TEXTURE_2D);
    real_glBindTexture(GL_TEXTURE_2D, hd ? hd : tex);
    p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    p_glEnable(GL_BLEND);
    real_glBlendFunc(FX_SRC_ALPHA, fire ? FX_ONE : FX_ONE_MINUS_SRC_ALPHA);
    if (fire) p_glDisable(GL_FOG);
    float z = 0;
    int depth = depth_ready > 0 && v[2] > 0;
    if (depth) {
        float w = v[2] / 4096.0f * SPRITE_BIAS;
        z = depth_z(w) / w;
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(GL_LEQUAL);
        p_glDepthMask(0);
    } else {
        p_glDisable(GL_DEPTH_TEST);
    }
    p_glColor4ub(255, 255, 255, 255);

    // Seeded by the 30 Hz tick and the effect, so a puff holds still between ticks.
    unsigned seed = (unsigned)(GetTickCount() / 33) * 2654435761u ^ (unsigned)(UINT_PTR)v * 40503u ^ type;
    int cx = v[0] >> 12, cy = v[1] >> 12, range = hi - lo + 1;
    float offx = (float)SCREEN_OFF_X, offy = (float)SCREEN_OFF_Y;
    real_glBegin(FX_QUADS);
    for (int i = 0; i < count; i++) {
        int x = cx + (int)(particle_rand(&seed) % (unsigned)(2 * spread)) - spread;
        int y = cy + (int)(particle_rand(&seed) % (unsigned)(2 * spread)) - spread;
        int s = ((int)(particle_rand(&seed) % (unsigned)range) + lo) * 2;
        if (x < 0 || y < 0 || x + s >= ws_canvas_w - 1 || y + s >= GAME_H - 1) continue;
        float x0 = x + offx, y0 = y + offy, x1 = x0 + s, y1 = y0 + s;
        const float quad[4][4] = {{x0, y0, 0, 0}, {x1, y0, 1, 0}, {x1, y1, 1, 1}, {x0, y1, 0, 1}};
        for (int k = 0; k < 4; k++) {
            real_glTexCoord2f(quad[k][2], quad[k][3]);
            if (depth) p_glVertex3f(quad[k][0], quad[k][1], z);
            else real_glVertex2f(quad[k][0], quad[k][1]);
        }
    }
    real_glEnd();
    p_glPopMatrix();
    p_glMatrixMode(GL_MODELVIEW);
    p_glPopAttrib();  // also restores the texture binding
}

static int install_particles(void) {
    static const BYTE expected[] = {0x83, 0xEC, 0x34, 0x83, 0x3D, 0x88, 0xAC, 0x62, 0x00, 0x01};
    BYTE *site = (BYTE *)PARTICLE_FN;
    if (IsBadReadPtr(site, sizeof expected) || memcmp(site, expected, sizeof expected) != 0) return 0;
    BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, expected, sizeof expected);  // sub esp,34h / cmp [renderer],1
    t[sizeof expected] = 0xE9;
    *(DWORD *)(t + sizeof expected + 1) = PARTICLE_RESUME - ((DWORD)(UINT_PTR)t + sizeof expected + 5);
    original_particles = (particles_t)t;
    put_branch(PARTICLE_FN, 0xE9, (void *)particles);
    return 1;
}

// The Apocalypse flash (FUN_0046e310). The original steps the palette 64 times toward white
// (+4 per channel per step, synced to vertical blank), shaking the screen by 8 lines with
// each step, then redraws the city twice and fades the palette back from white over 4 s
// (FUN_00525fa0). Palette changes can't reach OpenGL's true-colour frame, and the shake
// blits go through cnc-ddraw, whose empty 8-bit screen then covered the window: black.
// In OpenGL both parts are drawn here from a copy of the frame:
//   whiteout: the loop at 0x46e479 calls flash_whiteout(palette) and skips the original
//             loop when it returns 1;
//   hold:     the two redraws are each copied and covered with white before their swap;
//   fade:     the call at 0x46e66f goes to flash_fade, which draws the fade, then runs the
//             original (palette only, capped by fademax) so the palette ends up right.
#define FLASH_LOOP      0x0046e479  // mov esi, 40h: the 64-step loop's counter
#define FLASH_LOOP_BODY 0x0046e47e
#define FLASH_LOOP_END  0x0046e5a8
#define FLASH_FADE_CALL 0x0046e66f  // call FUN_00525fa0 (thiscall: colour, ms, steps)
#define PALETTE_FADE    0x00525fa0
#define FLASH_TEX       0x50000001u  // our texture name, clear of the game's and HD names
static int flash_hold, flash_w, flash_h;
static LONGLONG fx_qpc_freq;

static LONGLONG fx_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!fx_qpc_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        fx_qpc_freq = f.QuadPart;
    }
    return t.QuadPart;
}

static void fx_wait_until(LONGLONG t) {
    while (fx_now() < t) {
        LONGLONG left_ms = (t - fx_now()) * 1000 / fx_qpc_freq;
        Sleep(left_ms > 2 ? (DWORD)(left_ms - 1) : 0);
    }
}

static HWND fx_window(int *w, int *h) {
    HDC dc = p_wglGetCurrentDC();
    HWND wnd = dc ? WindowFromDC(dc) : NULL;
    RECT r;
    if (!wnd || !GetClientRect(wnd, &r)) return NULL;
    *w = r.right, *h = r.bottom;
    return wnd;
}

// Copy the front or back buffer into FLASH_TEX; returns its average brightness (0-255) or -1.
static int flash_capture(int front) {
    if (!fx_window(&flash_w, &flash_h) || flash_w <= 0 || flash_h <= 0) return -1;
    p_glPushAttrib(FX_ALL_ATTRIB_BITS);
    p_glPixelTransferi(FX_MAP_COLOR, 0);  // the game's colour-index pixel maps must not touch this copy
    p_glReadBuffer(front ? FX_FRONT : FX_BACK);
    real_glBindTexture(GL_TEXTURE_2D, FLASH_TEX);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, 0x2800 /* MAG_FILTER */, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, 0x2802 /* WRAP_S */, FX_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, 0x2803 /* WRAP_T */, FX_CLAMP_TO_EDGE);
    p_glCopyTexImage2D(GL_TEXTURE_2D, 0, FX_RGB, 0, 0, flash_w, flash_h, 0);
    BYTE px[16 * 16 * 4];
    p_glReadPixels(flash_w / 2 - 8, flash_h / 2 - 8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
    p_glPopAttrib();
    int sum = 0;
    for (int i = 0; i < 16 * 16; i++) sum += px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
    return sum / (16 * 16 * 3);
}

// Draw the copied frame shifted down by shift_px, then mix in white: additive (the
// whiteout's palette steps add to every colour) or as a blend toward `colour`.
static void flash_draw(float white, int additive, DWORD colour, int shift_px) {
    p_glPushAttrib(FX_ALL_ATTRIB_BITS);
    for (unsigned m = GL_MODELVIEW; m <= FX_TEXTURE; m++) {
        p_glMatrixMode(m);
        p_glPushMatrix();
        p_glLoadIdentity();
    }
    real_glViewport(0, 0, flash_w, flash_h);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_DEPTH_TEST);
    p_glDisable(GL_FOG);
    p_glDisable(GL_ALPHA_TEST);
    p_glDisable(GL_BLEND);
    p_glDisable(FX_CULL_FACE);
    p_glDisable(FX_LIGHTING);
    p_glClearColor(0, 0, 0, 1);
    real_glClear(GL_COLOR_BUFFER_BIT);
    p_glEnable(GL_TEXTURE_2D);
    real_glBindTexture(GL_TEXTURE_2D, FLASH_TEX);
    p_glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, FX_REPLACE);
    float dy = -2.0f * shift_px / flash_h;  // down by shift_px
    real_glBegin(FX_QUADS);
    real_glTexCoord2f(0, 0), real_glVertex2f(-1, -1 + dy);
    real_glTexCoord2f(1, 0), real_glVertex2f(1, -1 + dy);
    real_glTexCoord2f(1, 1), real_glVertex2f(1, 1 + dy);
    real_glTexCoord2f(0, 1), real_glVertex2f(-1, 1 + dy);
    real_glEnd();
    if (white > 0) {
        p_glDisable(GL_TEXTURE_2D);
        p_glEnable(GL_BLEND);
        if (additive) {
            real_glBlendFunc(FX_ONE, FX_ONE);
            p_glColor4f(white, white, white, 1);
        } else {
            real_glBlendFunc(FX_SRC_ALPHA, FX_ONE_MINUS_SRC_ALPHA);
            p_glColor4f((colour >> 16 & 0xFF) / 255.0f, (colour >> 8 & 0xFF) / 255.0f, (colour & 0xFF) / 255.0f,
                        white > 1 ? 1 : white);
        }
        real_glBegin(FX_QUADS);
        real_glVertex2f(-1, -1), real_glVertex2f(1, -1), real_glVertex2f(1, 1), real_glVertex2f(-1, 1);
        real_glEnd();
    }
    for (unsigned m = FX_TEXTURE + 1; m-- > GL_MODELVIEW;) {
        p_glMatrixMode(m);
        p_glPopMatrix();
    }
    p_glPopAttrib();
    p_SwapBuffers(p_wglGetCurrentDC());
}

// Returns 1 when the whiteout was drawn here (the original loop is skipped).
static int __stdcall flash_whiteout(BYTE *palette) {
    (void)palette;  // the palette steps don't show in OpenGL; the fade sets the final palette
    if (RENDERER != 2 || !fx_ready() || !p_wglGetCurrentContext()) {
        logmsg("flash: renderer %d, OpenGL %s: original whiteout\n", RENDERER,
               fx_ready() && p_wglGetCurrentContext() ? "ready" : "not current");
        return 0;
    }
    int brightness = flash_capture(1);
    logmsg("flash: whiteout in OpenGL, %dx%d, captured frame brightness %d\n", flash_w, flash_h, brightness);
    if (brightness < 0) return 0;
    LONGLONG t0 = fx_now(), step_ticks = fx_qpc_freq / 60;  // the original waits one vertical blank (60 Hz) per step
    int shake = (8 * flash_h + GAME_H / 2) / GAME_H;
    for (int step = 1; step <= 64; step++) {
        flash_draw(step * 4 / 255.0f, 1, 0xFFFFFF, step & 1 ? shake : 0);
        fx_wait_until(t0 + step * step_ticks);
    }
    flash_hold = 1;
    logmsg("flash: whiteout done in %d ms\n", (int)((fx_now() - t0) * 1000 / fx_qpc_freq));
    return 1;
}

// Before each SwapBuffers of the game's: while the flash holds white, keep a copy of the
// frame for the fade and show white instead.
void opengl_before_swap(void) {
    if (!flash_hold || RENDERER != 2) return;
    if (flash_hold++ > 4) {  // the fade never came: don't leave the screen white
        flash_hold = 0;
        logmsg("flash: no fade after the whiteout, white released\n");
        return;
    }
    int brightness = flash_capture(0);
    logmsg("flash: holding white over a redraw (brightness %d)\n", brightness);
    float c[4];
    p_glGetFloatv(GL_COLOR_CLEAR_VALUE, c);
    int scissor = p_glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) p_glDisable(GL_SCISSOR_TEST);
    p_glClearColor(1, 1, 1, 1);
    real_glClear(GL_COLOR_BUFFER_BIT);
    p_glClearColor(c[0], c[1], c[2], c[3]);
    if (scissor) p_glEnable(GL_SCISSOR_TEST);
}

static void __fastcall flash_fade(void *fader, void *edx, DWORD colour, int ms, int steps) {
    typedef void(__fastcall * fade_t)(void *, void *, DWORD, int, int);
    if (flash_hold && RENDERER == 2 && p_wglGetCurrentContext() && ms > 0) {
        flash_hold = 0;
        logmsg("flash: fade from %06lx over %d ms in OpenGL\n", colour & 0xFFFFFF, ms);
        LONGLONG t0 = fx_now(), end = t0 + fx_qpc_freq * ms / 1000;
        for (LONGLONG t = t0; t < end; t = fx_now()) {
            flash_draw(1.0f - (float)(t - t0) / (float)(end - t0), 0, colour, 0);
            fx_wait_until(t + fx_qpc_freq / 120);
        }
        flash_draw(0, 0, colour, 0);
    }
    flash_hold = 0;
    ((fade_t)PALETTE_FADE)(fader, edx, colour, ms, steps);
}

static int install_flash(void) {
    static const BYTE loop[] = {0xBE, 0x40, 0x00, 0x00, 0x00};  // mov esi, 40h
    BYTE *site = (BYTE *)FLASH_LOOP, *call = (BYTE *)FLASH_FADE_CALL;
    if (IsBadReadPtr(site, 5) || memcmp(site, loop, 5) != 0 || IsBadReadPtr(call, 5) || call[0] != 0xE8 ||
        FLASH_FADE_CALL + 5 + *(int *)(call + 1) != PALETTE_FADE)
        return 0;
    BYTE *t = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    BYTE *p = t;
    *p++ = 0x8D, *p++ = 0x85, *(int *)p = -0x59c, p += 4;   // lea eax, [ebp-59Ch] (the palette)
    *p++ = 0x50;                                            // push eax
    *p++ = 0xE8, *(DWORD *)p = (DWORD)(UINT_PTR)flash_whiteout - ((DWORD)(UINT_PTR)p + 4), p += 4;
    *p++ = 0x85, *p++ = 0xC0;                               // test eax, eax
    *p++ = 0x74, *p++ = 0x05;                               // jz original
    *p++ = 0xE9, *(DWORD *)p = FLASH_LOOP_END - ((DWORD)(UINT_PTR)p + 4), p += 4;
    memcpy(p, loop, 5), p += 5;                             // original: mov esi, 40h
    *p++ = 0xE9, *(DWORD *)p = FLASH_LOOP_BODY - ((DWORD)(UINT_PTR)p + 4), p += 4;
    put_branch(FLASH_LOOP, 0xE9, t);
    put_branch(FLASH_FADE_CALL, 0xE8, (void *)flash_fade);
    return 1;
}

int opengl_install(patch_iat_fn patch_iat) {
    HMODULE gl = LoadLibraryA("opengl32.dll");
    if (!gl) return 0;
    p_glPushAttrib = (glUint_t)GetProcAddress(gl, "glPushAttrib");
    p_glPopAttrib = (glVoid_t)GetProcAddress(gl, "glPopAttrib");
    p_glReadBuffer = (glUint_t)GetProcAddress(gl, "glReadBuffer");
    p_glPixelTransferi = (glPixelTransferi_t)GetProcAddress(gl, "glPixelTransferi");
    p_glCopyTexImage2D = (glCopyTexImage2D_t)GetProcAddress(gl, "glCopyTexImage2D");
    p_glReadPixels = (glReadPixels_t)GetProcAddress(gl, "glReadPixels");
    p_glColor4f = (glColor4f_t)GetProcAddress(gl, "glColor4f");
    p_wglGetCurrentContext = (wglGetCurrentContext_t)GetProcAddress(gl, "wglGetCurrentContext");
    p_SwapBuffers = (SwapBuffers_fn)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SwapBuffers");
    p_glPixelZoom = (glPixelZoom_t)GetProcAddress(gl, "glPixelZoom");
    p_glScissor = (glScissor_t)GetProcAddress(gl, "glScissor");
    p_glEnable = (glEnable_t)GetProcAddress(gl, "glEnable");
    p_glDisable = (glEnable_t)GetProcAddress(gl, "glDisable");
    p_glClearColor = (glClearColor_t)GetProcAddress(gl, "glClearColor");
    p_glGetFloatv = (glGetFloatv_t)GetProcAddress(gl, "glGetFloatv");
    p_glGetIntegerv = (glGetIntegerv_t)GetProcAddress(gl, "glGetIntegerv");
    p_wglGetCurrentDC = (wglGetCurrentDC_t)GetProcAddress(gl, "wglGetCurrentDC");
    p_glIsEnabled = (glIsEnabled_t)GetProcAddress(gl, "glIsEnabled");
    p_glAlphaFunc = (glAlphaFunc_t)GetProcAddress(gl, "glAlphaFunc");
    p_glTexParameteri = (glTexParameteri_t)GetProcAddress(gl, "glTexParameteri");
    p_glColor3ub = (glColor3ub_t)GetProcAddress(gl, "glColor3ub");
    p_glTexEnvi = (glTexEnvi_t)GetProcAddress(gl, "glTexEnvi");
    p_glGetTexImage = (glGetTexImage_t)GetProcAddress(gl, "glGetTexImage");
    p_glGetTexLevelParameteriv = (glGetTexLevelParameteriv_t)GetProcAddress(gl, "glGetTexLevelParameteriv");
    p_glGetTexEnviv = (glGetTexEnviv_t)GetProcAddress(gl, "glGetTexEnviv");
    p_glColor4ub = (glColor4ub_t)GetProcAddress(gl, "glColor4ub");
    p_glGetTexParameteriv = (glGetTexParameteriv_t)GetProcAddress(gl, "glGetTexParameteriv");
    p_glMatrixMode = (glMatrixMode_t)GetProcAddress(gl, "glMatrixMode");
    p_glPushMatrix = (glVoid_t)GetProcAddress(gl, "glPushMatrix");
    p_glPopMatrix = (glVoid_t)GetProcAddress(gl, "glPopMatrix");
    p_glLoadIdentity = (glVoid_t)GetProcAddress(gl, "glLoadIdentity");
    p_glTranslatef = (glTranslatef_t)GetProcAddress(gl, "glTranslatef");
    p_glDepthFunc = (glDepthFunc_t)GetProcAddress(gl, "glDepthFunc");
    p_glDepthMask = (glDepthMask_t)GetProcAddress(gl, "glDepthMask");
    p_glVertex3f = (glVertex3f_t)GetProcAddress(gl, "glVertex3f");
    p_glFogi = (glFogi_t)GetProcAddress(gl, "glFogi");
    p_wglGetProcAddress = (wglGetProcAddress_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!p_glMatrixMode || !p_glPushMatrix || !p_glPopMatrix || !p_glTranslatef || !p_glLoadIdentity) return 0;
    if (!p_glPixelZoom || !p_glScissor || !p_glEnable || !p_glDisable || !p_glClearColor || !p_glGetFloatv ||
        !p_wglGetCurrentDC || !p_glIsEnabled || !p_glAlphaFunc || !p_glTexParameteri ||
        !p_glGetTexParameteriv || !p_glColor3ub || !p_glColor4ub || !p_glGetIntegerv || !p_glTexEnvi ||
        !p_glGetTexEnviv)
        return 0;
    real_glViewport = (glViewport_t)patch_iat("OPENGL32.dll", "glViewport", (void *)hook_glViewport);
    real_glClear = (glClear_t)patch_iat("OPENGL32.dll", "glClear", (void *)hook_glClear);
    real_glRasterPos2i = (glRasterPos2i_t)patch_iat("OPENGL32.dll", "glRasterPos2i", (void *)hook_glRasterPos2i);
    real_glDrawPixels = (glDrawPixels_t)patch_iat("OPENGL32.dll", "glDrawPixels", (void *)hook_glDrawPixels);
    real_glPixelMapfv = (glPixelMapfv_t)patch_iat("OPENGL32.dll", "glPixelMapfv", (void *)hook_glPixelMapfv);
    real_glTexImage2D = (glTexImage2D_t)patch_iat("OPENGL32.dll", "glTexImage2D", (void *)hook_glTexImage2D);
    real_glBindTexture = (glBindTexture_t)patch_iat("OPENGL32.dll", "glBindTexture", (void *)hook_glBindTexture);
    real_glTexCoord2f = (glTexCoord2f_t)patch_iat("OPENGL32.dll", "glTexCoord2f", (void *)hook_glTexCoord2f);
    real_glTexParameteri_iat = (glTexParameteri_t)patch_iat("OPENGL32.dll", "glTexParameteri", (void *)hook_glTexParameteri);
    if (!real_glBindTexture || !real_glTexParameteri_iat) hd_enabled = -1;  // never enable replacements
    real_glBegin = (glBegin_t)patch_iat("OPENGL32.dll", "glBegin", (void *)hook_glBegin);
    real_glEnd = (glEnd_t)patch_iat("OPENGL32.dll", "glEnd", (void *)hook_glEnd);
    real_glColor3b = (glColor3b_t)patch_iat("OPENGL32.dll", "glColor3b", (void *)hook_glColor3b);
    real_glColor4b = (glColor4b_t)patch_iat("OPENGL32.dll", "glColor4b", (void *)hook_glColor4b);
    real_glVertex2f = (glVertex2f_t)patch_iat("OPENGL32.dll", "glVertex2f", (void *)hook_glVertex2f);
    real_glVertex4f = (glVertex4f_t)patch_iat("OPENGL32.dll", "glVertex4f", (void *)hook_glVertex4f);
    real_glBlendFunc = (glBlendFunc_t)patch_iat("OPENGL32.dll", "glBlendFunc", (void *)hook_glBlendFunc);
    real_glShadeModel = (glShadeModel_t)patch_iat("OPENGL32.dll", "glShadeModel", (void *)hook_glShadeModel);
    real_gluOrtho2D = (gluOrtho2D_t)patch_iat("GLU32.dll", "gluOrtho2D", (void *)hook_gluOrtho2D);
    if (ws_canvas_w != GAME_W && !real_gluOrtho2D) logmsg("opengl: gluOrtho2D hook FAILED, widescreen will be squeezed\n");
    int fill = install_fill_rect();
    logmsg("opengl: particles %s, Apocalypse flash %s\n", install_particles() ? "ok" : "FAILED",
           install_flash() ? "ok" : "FAILED");
    int shifts = install_colour_shift_fix();
    logmsg("opengl: colour byte-order fix: %d of 6 patched\n", shifts);
    logmsg("opengl: fill rect detour %s\n", fill ? "ok" : "FAILED");
    return real_glViewport && real_glClear && real_glRasterPos2i && real_glDrawPixels && real_glPixelMapfv &&
           real_glTexImage2D && real_glBegin && real_glEnd && real_glColor3b && real_glColor4b && real_glVertex2f &&
           real_glVertex4f;
}
