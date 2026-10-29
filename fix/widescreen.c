// Widescreen for the OpenGL renderer (-o2).
//
// The game works in a 640x480 coordinate space. For a wider screen that space becomes
// ws_canvas_w x 480 (ws_canvas_w = 480 x aspect, rounded down to a multiple of 8):
//  * the main 3D view record (0x5e7878, and the half-resolution one at 0x5e7858) is widened
//    whenever FUN_004a9ba5 loads it, so the game projects and clips for the full width;
//  * FUN_004ab326 derives the focal length from the view's half-width (x 1.732, 60 degrees
//    horizontal), so a wider view would zoom in and crop top and bottom. For widened views it
//    is derived from the half-height instead (x 2.309, the same value at 4:3): the vertical
//    field stays and more is visible at the sides;
//  * 2D overlays are laid out for 640 and are drawn shifted by ws_hud_off (opengl.c). The
//    rear-view mirror's 3D image is placed by an x offset that is an immediate in
//    FUN_004adc60 (631.0 as a double); it is moved by the same amount.
// The software and Glide renderers are untouched: everything checks the active renderer.
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "log.h"
#include "widescreen.h"
#include "drawdist.h"

int ws_canvas_w = 640;
int ws_hud_off;

#define RENDERER       (*(int *)0x0062ac88)
#define MAIN_VIEW      ((DWORD *)0x005e7878)  // x, y, width, height, half-res flag, ...
#define HALF_VIEW      ((DWORD *)0x005e7858)
#define VIEW_WIDTH     (*(DWORD *)0x00628434)  // current view, after the half-res adjustment
#define VIEW_HEIGHT    (*(DWORD *)0x00628438)
#define HALF_HEIGHT_FX (*(DWORD *)0x00628410)  // 20.12 fixed point
#define FOCAL          (*(DWORD *)0x00628414)  // 20.12 fixed point

#define LOAD_VIEW_FN     0x004a9ba5  // FUN_004a9ba5(record), cdecl
#define LOAD_VIEW_RESUME 0x004a9bab  // the pushal after the three relocated instructions
#define FOCAL_SITE       0x004ab379  // mov edx,[0x628430], right after the focal is stored
#define MIRROR_X_SITE    0x004adcac  // mov dword [0x629414], 0x4083b800 (high half of 631.0)

static LONG widened_logged, focal_logged;

static void __stdcall on_load_view(DWORD *rec) {
    if (RENDERER != 2) return;
    DWORD old_w = rec[2];
    if (rec == MAIN_VIEW && rec[2] == 640 && rec[3] == 480) rec[2] = ws_canvas_w;
    else if (rec == HALF_VIEW && rec[2] == 320 && rec[3] == 240) rec[2] = ws_canvas_w / 2;
    else return;
    if (InterlockedIncrement(&widened_logged) <= 4)
        logmsg("widescreen: view record %p widened %lu -> %lu\n", (void *)rec, old_w, rec[2]);
}

// Replaces the first three instructions of FUN_004a9ba5 (push ebp / mov ebp,esp /
// mov eax,[ebp+8]) and passes the record to on_load_view before the game reads it.
__attribute__((naked)) static void load_view_thunk(void) {
    __asm__ volatile(
        "pushl %%ebp\n\t"
        "movl %%esp, %%ebp\n\t"
        "movl 8(%%ebp), %%eax\n\t"
        "pushal\n\t"
        "pushl %%eax\n\t"
        "call %P0\n\t"            // stdcall, pops the record
        "popal\n\t"
        "pushl %1\n\t"
        "ret\n\t"                 // continue at the original pushal
        :
        : "i"(on_load_view), "i"(LOAD_VIEW_RESUME));
}

static void __cdecl fix_focal(void) {
    if (RENDERER != 2) return;
    DWORD w = VIEW_WIDTH, h = VIEW_HEIGHT;
    if (w != (DWORD)ws_canvas_w && w != (DWORD)ws_canvas_w / 2) return;  // the mirror keeps its own
    DWORD old = FOCAL;
    FOCAL = (DWORD)(((uint64_t)HALF_HEIGHT_FX * 9459) >> 12);  // 2.3093 = 1.7319 x 4/3
    if (InterlockedIncrement(&focal_logged) <= 4)
        logmsg("widescreen: view %lux%lu, focal %.1f -> %.1f\n", w, h, old / 4096.0, FOCAL / 4096.0);
}

// Called in place of `mov edx,[0x628430]`: eax holds the focal just stored; the code after
// multiplies it by the zoom in edx to get the vertical scale.
__attribute__((naked)) static void focal_thunk(void) {
    __asm__ volatile(
        "pushal\n\t"
        "call %P0\n\t"
        "popal\n\t"
        "movl 0x00628414, %%eax\n\t"
        "movl 0x00628430, %%edx\n\t"
        "ret\n\t"
        :
        : "i"(fix_focal));
}

// Per-frame 3D lists, filled by FUN_004ab410 and friends: polygons (0x6f8610, capacity
// 15000, flushed when full) and their vertices (0x896348, capacity 60000, never checked).
// A wider view puts more on screen, so the peaks are logged to see how close they get.
#define FRAME_POLYS    (*(int *)0x00b27a50)
#define FRAME_VERTICES (*(int *)0x009bb2d4)
static int peak_polys, peak_vertices;
static DWORD peak_logged_at;
extern int ws_edge_clamps;  // grid guard, below

void widescreen_frame(void) {
    if (ws_canvas_w <= 640 || RENDERER != 2) return;
    int p = FRAME_POLYS, v = FRAME_VERTICES;
    if (p > peak_polys) peak_polys = p;
    if (v > peak_vertices) peak_vertices = v;
    int terrain_a, terrain_b;
    drawdist_frame(&terrain_a, &terrain_b);
    DWORD now = GetTickCount();
    if (now - peak_logged_at >= 30000) {
        peak_logged_at = now;
        logmsg("widescreen: peak per-frame polygons %d / %d, vertices %d / %d, terrain buffers %d / %d bytes, "
               "draw grid %d tiles\n",
               peak_polys, dd_poly_cap, peak_vertices, dd_vertex_cap, terrain_a, terrain_b, *(int *)0x00a0f7c0);
        if (ws_edge_clamps) logmsg("grid guard: %d edge positions clamped so far\n", ws_edge_clamps);
    }
}

// FUN_004ad140 (OpenGL sprites: trees, pedestrians) clamps a sprite's right edge to 639.0
// and shortens its texture range to match. A sprite entirely right of 639 then becomes a
// quad stretched back to x=639 with the texture coordinate running negative, so the image
// repeats across it. The comparison and subtraction read the double at 0x5c4aa8 (shared
// with the radar), and the clamped value is stored as a float immediate: point the two
// references at our own copy and replace the immediate.
#define SPRITE_FCOM_SITE  0x004ad87d  // fcom qword [0x5c4aa8]
#define SPRITE_FSUB_SITE  0x004ad892  // fsub qword [0x5c4aa8]
#define SPRITE_STORE_SITE 0x004ad898  // mov dword [0x9bd398], 639.0f
static double sprite_right_edge = 639.0;

static int patch_code(DWORD addr, const BYTE *expected, int n, const BYTE *bytes) {
    BYTE *p = (BYTE *)addr;
    if (IsBadReadPtr(p, n) || memcmp(p, expected, n) != 0) return 0;
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

static void put_rel32(BYTE *b, BYTE opcode, DWORD from, void *to) {
    b[0] = opcode;
    *(DWORD *)(b + 1) = (DWORD)(UINT_PTR)to - (from + 5);
}

// FUN_004b3b00 (culling) keeps three screen-coverage grids of 8x8-pixel tiles, column-major,
// sized for 640x480: 0x9bea10 and 0x9bfd70 (61 rows x 81 columns) and 0x9bd618 (62 rows x
// 82 columns). It never clamps x; it relies on the view width. A wider view writes past the
// last column into the function's own variables (0x9c10c0.., 0x9be9f4..) and the loops then
// run off into read-only memory. Move the grids to buffers sized for the canvas: every code
// reference to them carries a relocation (21 in 0x4b3ad0..0x4b4750, some as base-1 or
// base+62), and the clear loop at 0x4b3ad0 has the column count 81 as an immediate.
static const struct { DWORD addr; int rows, cols; } grids[] = {
    {0x009bea10, 61, 81},
    {0x009bfd70, 61, 81},
    {0x009bd618, 62, 82},
};
#define NGRIDS (int)(sizeof grids / sizeof grids[0])
#define GRID_CODE_START 0x004b3a00
#define GRID_CODE_END   0x004b4800
#define GRID_REFS   21
#define GRID_CLEAR_COLS 0x004b3ad8  // mov ecx, 0x51
#define GRID_PAD    256             // bytes kept free before and after each new grid

static int relocate_grids(BYTE *const *moved) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.VirtualAddress || !dir.Size) return -1;
    int found = 0;
    BYTE *p = base + dir.VirtualAddress, *end = p + dir.Size;
    while (p < end) {
        IMAGE_BASE_RELOCATION *block = (IMAGE_BASE_RELOCATION *)p;
        if (block->SizeOfBlock < sizeof *block) break;
        WORD *entries = (WORD *)(block + 1);
        int n = (block->SizeOfBlock - sizeof *block) / 2;
        for (int i = 0; i < n; i++) {
            if (entries[i] >> 12 != IMAGE_REL_BASED_HIGHLOW) continue;
            DWORD site = (DWORD)(UINT_PTR)base + block->VirtualAddress + (entries[i] & 0xfff);
            if (site < GRID_CODE_START || site >= GRID_CODE_END) continue;
            DWORD v = *(DWORD *)site;
            int g;
            for (g = 0; g < NGRIDS; g++)
                if (v >= grids[g].addr - 1 && v < grids[g].addr + (DWORD)(grids[g].rows * grids[g].cols)) break;
            if (g == NGRIDS) continue;
            found++;
            if (moved) {
                DWORD nv = (DWORD)(UINT_PTR)moved[g] + (v - grids[g].addr), old;
                VirtualProtect((void *)site, 4, PAGE_EXECUTE_READWRITE, &old);
                *(DWORD *)site = nv;
                VirtualProtect((void *)site, 4, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void *)site, 4);
            }
        }
        p += block->SizeOfBlock;
    }
    return found;
}

// FUN_004b3b00 is handed a projected polygon ({count, vertices}, 5 ints per vertex, x and y
// in 20.12 screen pixels) and marks its 8x8 tiles in the grids without any bounds check.
// Now and then a vertex comes out absurd (x around 157000 pixels, likely a point at the
// camera plane), and the tile index lands a megabyte past the grid: a crash the game
// swallows when that memory is unmapped, silent corruption when it isn't. Its only caller
// (FUN_004ab7b0) stores the result as the polygon's draw flag, so reject such a polygon
// there: don't touch the grids and don't draw it.
#define COVERAGE_FN   0x004b3b00
#define COVERAGE_CALL 0x004ab91d  // call FUN_004b3b00 (cdecl: polygon, flag)
typedef int(__cdecl *coverage_fn_t)(int *, int);
static LONG grid_rejects;

static int __cdecl coverage_guard(int *poly, int flag) {
    int n = poly[0];
    const int *v = (const int *)(UINT_PTR)poly[1];
    // tile columns 0..w/8 and rows 0..60 exist in every grid (they have one spare of each);
    // the tile is (pixel + 7) >> 3 below zero, so pixels -7..-1 still map to tile 0
    int min_xy = -(7 << 12), max_x = ((ws_canvas_w + 8) << 12) - 1, max_y = ((480 + 8) << 12) - 1;
    for (int i = 0; i < n; i++, v += 5) {
        if (v[0] >= min_xy && v[0] <= max_x && v[1] >= min_xy && v[1] <= max_y) continue;
        if (InterlockedIncrement(&grid_rejects) <= 10)
            logmsg("grid guard: polygon with vertex %d of %d at (%.1f, %.1f) skipped\n", i, n, v[0] / 4096.0,
                   v[1] / 4096.0);
        return 0;
    }
    return ((coverage_fn_t)COVERAGE_FN)(poly, flag);
}

// Even with every vertex on screen the function can still run off the grid: for each tile
// row it interpolates the polygon's left and right edge x (tables 0x9c10f8 and 0x9bd508),
// and an edge that is nearly horizontal gets a huge step (0x8000 / dy), so the x for the
// next row lands far outside the polygon. Clamp each stored edge x to the polygon's own x
// range (0x9c10e4 min, 0x9c10d4 max, set by the function's first loop). The four stores are
// 7-byte `mov [index*4 + table], value`; each becomes a call to a thunk that clamps a copy
// (all registers and the running value are kept) and does the store.
int ws_edge_clamps;
#define EDGE_THUNK(name, value, index, table)                                               \
    void name(void);                                                                        \
    __asm__(".globl _" #name "\n_" #name ":\n"                                              \
            "  push %eax\n"                                                                 \
            "  mov %" #value ", %eax\n"                                                     \
            "  cmp 0x9c10e4, %eax\n"                                                        \
            "  jge 1f\n"                                                                    \
            "  mov 0x9c10e4, %eax\n"                                                        \
            "  incl _ws_edge_clamps\n"                                                      \
            "1: cmp 0x9c10d4, %eax\n"                                                       \
            "  jle 2f\n"                                                                    \
            "  mov 0x9c10d4, %eax\n"                                                        \
            "  incl _ws_edge_clamps\n"                                                      \
            "2: mov %eax, " #table "(,%" #index ",4)\n"                                     \
            "  pop %eax\n"                                                                  \
            "  ret\n");
EDGE_THUNK(edge_store_left_a, edx, ecx, 0x9c10f8)
EDGE_THUNK(edge_store_left_b, ecx, edx, 0x9c10f8)
EDGE_THUNK(edge_store_right_a, edx, ecx, 0x9bd508)
EDGE_THUNK(edge_store_right_b, ecx, edx, 0x9bd508)

static const struct { DWORD site; BYTE orig[7]; void (*thunk)(void); } edge_stores[] = {
    {0x004b3f0e, {0x89, 0x14, 0x8D, 0xF8, 0x10, 0x9C, 0x00}, edge_store_left_a},   // mov [ecx*4+0x9c10f8], edx
    {0x004b3fd8, {0x89, 0x0C, 0x95, 0xF8, 0x10, 0x9C, 0x00}, edge_store_left_b},   // mov [edx*4+0x9c10f8], ecx
    {0x004b40fd, {0x89, 0x14, 0x8D, 0x08, 0xD5, 0x9B, 0x00}, edge_store_right_a},  // mov [ecx*4+0x9bd508], edx
    {0x004b41d0, {0x89, 0x0C, 0x95, 0x08, 0xD5, 0x9B, 0x00}, edge_store_right_b},  // mov [edx*4+0x9bd508], ecx
};

int grid_guard_install(void) {
    BYTE *site = (BYTE *)COVERAGE_CALL;
    if (site[0] != 0xE8 || COVERAGE_CALL + 5 + *(int *)(site + 1) != COVERAGE_FN) return 0;
    for (int i = 0; i < 4; i++)
        if (memcmp((void *)edge_stores[i].site, edge_stores[i].orig, 7) != 0) return 0;
    BYTE call[7];
    for (int i = 0; i < 4; i++) {
        put_rel32(call, 0xE8, edge_stores[i].site, (void *)edge_stores[i].thunk);
        call[5] = call[6] = 0x90;
        patch_code(edge_stores[i].site, edge_stores[i].orig, 7, call);
    }
    put_rel32(call, 0xE8, COVERAGE_CALL, (void *)coverage_guard);
    return patch_code(COVERAGE_CALL, site, 5, call);
}

int widescreen_install(double aspect) {
    int w = (int)(480 * aspect) & ~7;
    if (w > 2048) w = 2048;
    if (w <= 640) {
        logmsg("widescreen: off (aspect %.3f)\n", aspect);
        return 0;
    }

    static const BYTE load_orig[6] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08};
    BYTE load_new[6];
    put_rel32(load_new, 0xE9, LOAD_VIEW_FN, (void *)load_view_thunk);
    load_new[5] = 0x90;

    static const BYTE focal_orig[6] = {0x8B, 0x15, 0x30, 0x84, 0x62, 0x00};
    BYTE focal_new[6];
    put_rel32(focal_new, 0xE8, FOCAL_SITE, (void *)focal_thunk);
    focal_new[5] = 0x90;

    static const BYTE mirror_orig[10] = {0xC7, 0x05, 0x14, 0x94, 0x62, 0x00, 0x00, 0xB8, 0x83, 0x40};
    BYTE mirror_new[10];
    memcpy(mirror_new, mirror_orig, 6);
    union { double d; DWORD dw[2]; } x = {631.0 + (w - 640) / 2};
    memcpy(mirror_new + 6, &x.dw[1], 4);

    int cols = w / 8;  // tile columns across the canvas (80 at 640)
    static const BYTE clear_orig[5] = {0xB9, 81, 0x00, 0x00, 0x00};
    BYTE clear_new[5] = {0xB9};
    *(DWORD *)(clear_new + 1) = (DWORD)(cols + 1);

    sprite_right_edge = w - 1;
    static const BYTE fcom_orig[6] = {0xDC, 0x15, 0xA8, 0x4A, 0x5C, 0x00};
    static const BYTE fsub_orig[6] = {0xDC, 0x25, 0xA8, 0x4A, 0x5C, 0x00};
    static const BYTE store_orig[10] = {0xC7, 0x05, 0x98, 0xD3, 0x9B, 0x00, 0x00, 0xC0, 0x1F, 0x44};
    BYTE fcom_new[6], fsub_new[6], store_new[10];
    memcpy(fcom_new, fcom_orig, 2);
    memcpy(fsub_new, fsub_orig, 2);
    *(DWORD *)(fcom_new + 2) = *(DWORD *)(fsub_new + 2) = (DWORD)(UINT_PTR)&sprite_right_edge;
    memcpy(store_new, store_orig, 6);
    float right_f = (float)(w - 1);
    memcpy(store_new + 6, &right_f, 4);

    // All or nothing: a half-applied patch would distort the view or overrun the grids.
    if (IsBadReadPtr((void *)LOAD_VIEW_FN, 6) || memcmp((void *)LOAD_VIEW_FN, load_orig, 6) ||
        memcmp((void *)FOCAL_SITE, focal_orig, 6) || memcmp((void *)MIRROR_X_SITE, mirror_orig, 10) ||
        memcmp((void *)GRID_CLEAR_COLS, clear_orig, 5) ||
        memcmp((void *)SPRITE_FCOM_SITE, fcom_orig, 6) || memcmp((void *)SPRITE_FSUB_SITE, fsub_orig, 6) ||
        memcmp((void *)SPRITE_STORE_SITE, store_orig, 10)) {
        logmsg("widescreen: game code not as expected, staying 4:3\n");
        return 0;
    }
    int refs = relocate_grids(NULL);
    if (refs != GRID_REFS) {
        logmsg("widescreen: found %d coverage-grid references (expected %d), staying 4:3\n", refs, GRID_REFS);
        return 0;
    }
    BYTE *moved[NGRIDS];
    for (int g = 0; g < NGRIDS; g++) {
        // Same rows; columns for the canvas plus the original's spare ones.
        int size = grids[g].rows * (cols + grids[g].cols - 80 + 2);  // + 2 spare columns
        BYTE *mem = VirtualAlloc(NULL, size + 2 * GRID_PAD, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) {
            logmsg("widescreen: grid allocation failed, staying 4:3\n");
            return 0;
        }
        moved[g] = mem + GRID_PAD;
        logmsg("widescreen: coverage grid %08lx moved to %p (%d bytes)\n", grids[g].addr, (void *)moved[g], size);
    }
    relocate_grids(moved);
    patch_code(GRID_CLEAR_COLS, clear_orig, 5, clear_new);
    patch_code(LOAD_VIEW_FN, load_orig, 6, load_new);
    patch_code(FOCAL_SITE, focal_orig, 6, focal_new);
    patch_code(MIRROR_X_SITE, mirror_orig, 10, mirror_new);
    patch_code(SPRITE_FCOM_SITE, fcom_orig, 6, fcom_new);
    patch_code(SPRITE_FSUB_SITE, fsub_orig, 6, fsub_new);
    patch_code(SPRITE_STORE_SITE, store_orig, 10, store_new);
    ws_canvas_w = w;
    ws_hud_off = (w - 640) / 2;
    logmsg("widescreen: aspect %.3f, game space %dx480, overlays shifted %d\n", aspect, w, ws_hud_off);
    return 1;
}
