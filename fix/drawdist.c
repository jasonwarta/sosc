// Draw distance beyond the game's maximum. The world is drawn from a square window of map
// tiles around the camera, n tiles a side (odd, 13..61; the options slider maps 0..24 to
// n = 2s + 13). Everything sized for n lives in a pool FUN_004c5bf0 preallocates for 61,
// except:
//   - a static [61][61] table of gridfaces row pointers at 0xa0bca0 (filled by FUN_004c56d0,
//     read by FUN_004d6730 with the row stride 61 hard-coded as lea arithmetic), with other
//     globals right after it, so it moves to the heap;
//   - the per-frame polygon list (0x6f8610, 0x71 bytes, cap 15000, flushed when full) and
//     vertex list (0x896348, 0x14 bytes, never checked). A larger window fills them sooner,
//     so both move to bigger heap buffers.
//   - two static 560000-byte terrain buffers the terrain pass (FUN_004b21c5 and friends) fills
//     through write pointers 0x62afcc / 0x62afd0 with no capacity check: 0xa9e3b0 (16-byte
//     records) and 0xa15820. At 91 tiles they overflowed and the ground vanished.
// The game's "max" (61) then means drawdistance tiles. The far clip and the fog scale with n
// by themselves (FUN_004d16a0((n/2 - 3) tiles)).
#include <windows.h>
#include <string.h>
#include "drawdist.h"
#include "log.h"

#define GAME_MAX_TILES   61
#define TILE_LIMIT       125  // the window must stay smaller than the 128-tile map
#define GRID_RESIZE      0x004c56d0  // FUN_004c56d0(n): sets the window size, carves the pool
#define POLY_LIST        0x006f8610u
#define VERTEX_LIST      0x00896348u
#define POLY_SIZE        0x71
#define VERTEX_SIZE      0x14

int dd_tiles = GAME_MAX_TILES;
static int dd_start = GAME_MAX_TILES;  // the size the game's "max" maps to (worldstart)
int dd_poly_cap = 15000;
int dd_vertex_cap = 60000;
void **face_rows;  // replaces the [61][61] table at 0xa0bca0, stride dd_tiles (read from asm)
static BYTE *poly_list, *vertex_list, *terrain_a, *terrain_b;
static DWORD terrain_size;
#define TERRAIN_A        0x00a9e3b0u  // write pointer 0x62afcc
#define TERRAIN_B        0x00a15820u  // write pointer 0x62afd0
#define TERRAIN_SIZE     560000u
static int dry_run;  // check every site's bytes first, so nothing is half-patched

static int patch_bytes(DWORD addr, const BYTE *expected, int n, const BYTE *bytes) {
    BYTE *p = (BYTE *)addr;
    if (IsBadReadPtr(p, n) || memcmp(p, expected, n) != 0) {
        logmsg("draw distance: unexpected bytes at %08lx\n", addr);
        return 0;
    }
    if (dry_run) return 1;
    DWORD old;
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

// Replace the 32-bit value old_value inside the instruction at addr (within its first 11 bytes).
static int patch_dword_in(DWORD addr, DWORD old_value, DWORD new_value) {
    BYTE *p = (BYTE *)addr;
    for (int i = 1; i <= 7; i++)
        if (*(DWORD *)(p + i) == old_value) return patch_bytes(addr + i, (BYTE *)&old_value, 4, (BYTE *)&new_value);
    logmsg("draw distance: %08lx not found at %08lx\n", old_value, addr);
    return 0;
}

static int redirect_call(DWORD site, DWORD target, void *hook) {
    BYTE *p = (BYTE *)site;
    if (p[0] != 0xE8 || site + 5 + *(int *)(p + 1) != target) return 0;
    BYTE call[5] = {0xE8};
    *(int *)(call + 1) = (int)((DWORD)(UINT_PTR)hook - (site + 5));
    return patch_bytes(site, p, 5, call);
}

// FUN_004c56d0 wrapper: the game's maximum means our starting size, when growing to it.
//
// Every caller keeps the window centred on the camera by moving its origin (0xb42fa0/4, in
// tiles) by half the change in size, but only when the size came out as asked:
// FUN_004dd240 (the draw distance setting, also the "max" applied at launch) checks that the
// new size equals the one it asked for, and the , / . keys (FUN_004dcf10) step by 2 and move
// the origin by 1 only if the size changed by exactly 2. A 61 that became a larger size fails
// both checks, so the window kept its old origin: the car sat 12 tiles from a corner of the 63
// window (the origin of the 25-tile startup window) and the ground around it was outside.
// So recentre here whenever the size was mapped. FUN_004d74c0 recentres from the camera after
// the call anyway. Stepping down from above 61 is a real 61 (the , key from dd_tiles): that's
// a change of 2, which the key handler recentres itself.
#define WINDOW_SIZE     0x00a0f7c0
#define WINDOW_ORIGIN_X 0x00b42fa0
#define WINDOW_ORIGIN_Z 0x00b42fa4
static int grid_resize(int n) {
    int old = *(int *)WINDOW_SIZE;
    int mapped = n == GAME_MAX_TILES && old < GAME_MAX_TILES && dd_start != GAME_MAX_TILES;
    int r = ((int (*)(int))GRID_RESIZE)(mapped ? dd_start : n);
    int now = *(int *)WINDOW_SIZE;
    if (mapped && now != old) {
        int shift = (old - now) / 2;  // the game's own rounding (cdq; sub; sar)
        *(int *)WINDOW_ORIGIN_X += shift;
        *(int *)WINDOW_ORIGIN_Z += shift;
    }
    static int last;
    if (now != last) {
        last = now;
        logmsg("draw distance: world window %d x %d map tiles, origin (%d,%d)\n", now, now,
               *(int *)WINDOW_ORIGIN_X, *(int *)WINDOW_ORIGIN_Z);
    }
    return r;
}

// FUN_004d6730's two table reads, row ebp and column [esp+0x14] (+4 inside the call).
// Site 1 keeps eax's store and the counter increment, then loads eax = table[row][col].
void face_row_eax(void);
__asm__(".globl _face_row_eax\n_face_row_eax:\n"
        "  mov %ebp, %eax\n"
        "  imul _dd_tiles, %eax\n"
        "  add 0x18(%esp), %eax\n"
        "  push %ecx\n"
        "  mov _face_rows, %ecx\n"
        "  mov (%ecx,%eax,4), %eax\n"
        "  pop %ecx\n"
        "  ret\n");
// Site 2 loads ecx = table[row][col] and keeps eax and edx.
void face_row_ecx(void);
__asm__(".globl _face_row_ecx\n_face_row_ecx:\n"
        "  mov %ebp, %ecx\n"
        "  imul _dd_tiles, %ecx\n"
        "  add 0x18(%esp), %ecx\n"
        "  push %eax\n"
        "  mov _face_rows, %eax\n"
        "  mov (%eax,%ecx,4), %ecx\n"
        "  pop %eax\n"
        "  ret\n");

static void put_call(BYTE *b, DWORD from, void *to) {
    b[0] = 0xE8;
    *(int *)(b + 1) = (int)((DWORD)(UINT_PTR)to - (from + 5));
}

static int move_face_table(void) {
    DWORD table = (DWORD)(UINT_PTR)face_rows, stride = dd_tiles * 4;
    // FUN_004c56d0 fill loop: mov esi, 0xa0bca0 ... add esi, 0xf4
    static const BYTE fill_base[] = {0xBE, 0xA0, 0xBC, 0xA0, 0x00};
    static const BYTE fill_step[] = {0x81, 0xC6, 0xF4, 0x00, 0x00, 0x00};
    BYTE base[5] = {0xBE}, step[6] = {0x81, 0xC6};
    memcpy(base + 1, &table, 4);
    memcpy(step + 2, &stride, 4);
    // 0x4d682d: lea ecx,[ebp+ebp*4]; mov [esi-4],eax; lea eax,[ecx+ecx*2]; inc [0xa0f7b0];
    //           lea eax,[ebp+eax*4]; add eax,[esp+0x14]; mov eax,[eax*4+0xa0bca0]
    static const BYTE read1[31] = {0x8D, 0x4C, 0xAD, 0x00, 0x89, 0x46, 0xFC, 0x8D, 0x04, 0x49, 0xFF,
                                   0x05, 0xB0, 0xF7, 0xA0, 0x00, 0x8D, 0x44, 0x85, 0x00, 0x03, 0x44,
                                   0x24, 0x14, 0x8B, 0x04, 0x85, 0xA0, 0xBC, 0xA0, 0x00};
    BYTE new1[31];
    memset(new1, 0x90, sizeof new1);
    memcpy(new1, "\x89\x46\xFC\xFF\x05\xB0\xF7\xA0\x00", 9);  // mov [esi-4],eax; inc [0xa0f7b0]
    put_call(new1 + 9, 0x004d682d + 9, face_row_eax);
    // 0x4d68e5: lea ecx,[ebp+ebp*4]; add ebx,4; add ebx,4; lea ecx,[ecx+ecx*2];
    //           lea ecx,[ebp+ecx*4]; add ecx,[esp+0x14]; mov ecx,[ecx*4+0xa0bca0]
    static const BYTE read2[28] = {0x8D, 0x4C, 0xAD, 0x00, 0x83, 0xC3, 0x04, 0x83, 0xC3, 0x04,
                                   0x8D, 0x0C, 0x49, 0x8D, 0x4C, 0x8D, 0x00, 0x03, 0x4C, 0x24,
                                   0x14, 0x8B, 0x0C, 0x8D, 0xA0, 0xBC, 0xA0, 0x00};
    BYTE new2[28];
    memset(new2, 0x90, sizeof new2);
    memcpy(new2, "\x83\xC3\x08", 3);  // add ebx,8
    put_call(new2 + 3, 0x004d68e5 + 3, face_row_ecx);
    return patch_bytes(0x004c5968, fill_base, 5, base) & patch_bytes(0x004c5994, fill_step, 6, step) &
           patch_bytes(0x004d682d, read1, 31, new1) & patch_bytes(0x004d68e5, read2, 28, new2);
}

// FUN_004c5bf0 presets the sizes for 61 and allocates the pool; FUN_004c56d0 rejects n > 61.
static int grow_grid_pool(void) {
    int n = dd_tiles, v = (n + 1) * (n + 1);
    DWORD pool = (DWORD)(33 * v + 252 * n * n);  // gridrot+gridvert+whereisit+isrotated, gridproj+faces+sfaces+cells
    BYTE cap_old = 0x3D, cap_new = (BYTE)n;
    return patch_dword_in(0x004c5bf0, 0x3d, (DWORD)n) & patch_dword_in(0x004c5bfa, 0x3e, (DWORD)(n + 1)) &
           patch_dword_in(0x004c5c04, 0x7a, (DWORD)(2 * n)) & patch_dword_in(0x004c5c0e, 0xf04, (DWORD)v) &
           patch_dword_in(0x004c5c18, 0x1d12, (DWORD)(2 * n * n)) & patch_dword_in(0x004c5c2c, 0x103e60, pool) &
           patch_bytes(0x004c570a, &cap_old, 1, &cap_new);  // cmp ecx, 0x3d
}

static int grow_draw_lists(void) {
    static const DWORD poly_refs[] = {0x004ab47e, 0x004ab5bd, 0x004ab6f9, 0x004ab813, 0x004ab948};
    static const DWORD vertex_refs[] = {0x004ab4bb, 0x004ab4e5, 0x004ab60c, 0x004ab63b, 0x004ab74a, 0x004ab76c};
    static const DWORD cap_refs[] = {0x004ab453, 0x004ab593, 0x004ab6d2};  // cmp [0xb27a50], 15000
    int ok = 1;
    for (int i = 0; i < 5; i++) ok &= patch_dword_in(poly_refs[i], POLY_LIST, (DWORD)(UINT_PTR)poly_list);
    for (int i = 0; i < 6; i++) ok &= patch_dword_in(vertex_refs[i], VERTEX_LIST, (DWORD)(UINT_PTR)vertex_list);
    DWORD old_cap = 15000, new_cap = (DWORD)dd_poly_cap;
    for (int i = 0; i < 3; i++) ok &= patch_bytes(cap_refs[i], (BYTE *)&old_cap, 4, (BYTE *)&new_cap);
    return ok;
}

static int move_terrain_buffers(void) {
    static const DWORD a_refs[] = {0x004b229d, 0x004b26aa, 0x004b2734, 0x004b2835, 0x004b285d,
                                   0x004b2c2b, 0x004b2c53, 0x004b2dd9, 0x004bd0b7};
    static const DWORD b_refs[] = {0x004b22a2, 0x004b26af, 0x004b2739, 0x004b29aa, 0x004b29ba,
                                   0x004b2afc, 0x004b2b0c};
    int ok = 1;
    for (int i = 0; i < 9; i++) ok &= patch_dword_in(a_refs[i], TERRAIN_A, (DWORD)(UINT_PTR)terrain_a);
    for (int i = 0; i < 7; i++) ok &= patch_dword_in(b_refs[i], TERRAIN_B, (DWORD)(UINT_PTR)terrain_b);
    return ok;
}

// Terrain projection overflow. The transform/project/clip functions (FUN_004bc2d4,
// FUN_004bc547, FUN_004bc8bf) divide 16.16 values by depth or by the near-plane distance
// (0x68786c / 0x687868), e.g. x * focal / near at 0x4bc6ab. A triangle that crosses the
// camera plane far off to the side has a huge x, and the quotient doesn't fit: integer
// overflow (seen at 0x4bc6ab and 0x4bc61f). With a wider world window there are many more such
// triangles. Saturate the quotient (the point is off screen anyway) and carry on.
enum { DIV_ECX, DIV_ESI, DIV_ESI4, DIV_NEAR, DIV_NEAR2 };
static const struct { DWORD eip; BYTE len, kind; } projection_divides[] = {
    {0x004bc465, 3, DIV_ESI4}, {0x004bc47e, 2, DIV_ESI},   {0x004bc61f, 3, DIV_ESI4}, {0x004bc638, 2, DIV_ESI},
    {0x004bc695, 2, DIV_ECX},  {0x004bc6ab, 6, DIV_NEAR},  {0x004bc6d0, 6, DIV_NEAR}, {0x004bc76e, 2, DIV_ECX},
    {0x004bc784, 6, DIV_NEAR}, {0x004bc7a9, 6, DIV_NEAR},  {0x004bc87a, 3, DIV_ESI4}, {0x004bc893, 2, DIV_ESI},
    {0x004bcb89, 2, DIV_ECX},  {0x004bcbc4, 6, DIV_NEAR2}, {0x004bcc5f, 2, DIV_ECX},  {0x004bcc9a, 6, DIV_NEAR2},
};
static LONG projection_overflows;

static LONG CALLBACK projection_guard(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    for (int i = 0; i < (int)(sizeof projection_divides / sizeof projection_divides[0]); i++) {
        if (c->Eip != projection_divides[i].eip) continue;
        LONG d;
        switch (projection_divides[i].kind) {
        case DIV_ECX: d = (LONG)c->Ecx; break;
        case DIV_ESI: d = *(LONG *)(UINT_PTR)c->Esi; break;
        case DIV_ESI4: d = *(LONG *)(UINT_PTR)(c->Esi + 4); break;
        case DIV_NEAR: d = *(LONG *)0x0068786c; break;
        default: d = *(LONG *)0x00687868; break;
        }
        // saturate well inside 32 bits: callers add a screen centre to the result
        LONG q = (((LONG)c->Edx < 0) != (d < 0)) ? -0x08000000 : 0x08000000;
        if (InterlockedIncrement(&projection_overflows) <= 5)
            logmsg("projection guard: overflow at %08lx (%08lx:%08lx / %08lx) -> %08lx\n", c->Eip, c->Edx, c->Eax,
                   (DWORD)d, (DWORD)q);
        c->Eax = (DWORD)q;
        c->Edx = 0;
        c->Eip += projection_divides[i].len;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Ctrl+Shift+F9: cycle the window through the game's 61, a middle size and dd_tiles (61 -> 77
// -> 91 at worldtiles=91). The buffers are sized for dd_tiles, so any size up to it is safe.
// Done between frames on the game thread, with the same steps as the game's own resize
// (FUN_004dd240): resize, far clip at (n/2 - 3) tiles, move the origin by half the change,
// then FUN_004dd4e0.
#define FAR_CLIP_FN  0x004d16a0
#define AFTER_RESIZE 0x004dd4e0
static volatile LONG toggle_pending;

void drawdist_toggle(void) {
    InterlockedExchange(&toggle_pending, 1);
}

static void apply_toggle(void) {
    if (!InterlockedExchange(&toggle_pending, 0)) return;
    int old = *(int *)WINDOW_SIZE;
    if (dd_tiles <= GAME_MAX_TILES || old < GAME_MAX_TILES) {  // not extended, or below max
        logmsg("draw distance: Ctrl+Shift+F9 ignored (window %d, extended size %d)\n", old, dd_tiles);
        return;
    }
    int middle = (GAME_MAX_TILES + dd_tiles) / 2 | 1;  // window sizes are odd
    int want = old < middle ? middle : old < dd_tiles ? dd_tiles : GAME_MAX_TILES;
    ((int (*)(int))GRID_RESIZE)(want);
    int now = *(int *)WINDOW_SIZE;
    ((void (*)(int))FAR_CLIP_FN)((now / 2 - 3) * 0x400000);
    int shift = (old - now) / 2;
    *(int *)WINDOW_ORIGIN_X += shift;
    *(int *)WINDOW_ORIGIN_Z += shift;
    ((void (*)(void))AFTER_RESIZE)();
    logmsg("draw distance: Ctrl+Shift+F9, window %d -> %d tiles, origin (%d,%d)\n", old, now,
           *(int *)WINDOW_ORIGIN_X, *(int *)WINDOW_ORIGIN_Z);
}

// Peak terrain buffer use, logged with the other per-frame peaks.
void drawdist_frame(int *used_a, int *used_b) {
    apply_toggle();
    static int peak_a, peak_b;
    if (terrain_a) {
        int a = (int)(*(BYTE **)0x0062afcc - terrain_a), b = (int)(*(BYTE **)0x0062afd0 - terrain_b);
        if (a > peak_a && a <= (int)terrain_size) peak_a = a;
        if (b > peak_b && b <= (int)terrain_size) peak_b = b;
    }
    *used_a = peak_a, *used_b = peak_b;
    // Ground pipeline counters: once a second for 8 s after the window changes size, then every
    // 10 s. Visible cells (FUN_004d5b30's list at 0x9c5050..[0x9c4f68]), ground faces built
    // (0xa0bc90), cells culled (0x5e7834), cells with objects (0xa0f7b0), polygons queued
    // (0xb27a50), the window origin (0xb42fa0/4) and the camera tile (0xc42fe8/ec).
    static int last_n, burst;
    static DWORD stats_at;
    int n = *(int *)0x00a0f7c0;
    DWORD t = GetTickCount();
    if (n != last_n) last_n = n, burst = 8, stats_at = 0;
    if (t - stats_at >= (burst > 0 ? 1000u : 10000u)) {
        stats_at = t;
        if (burst > 0) burst--;
        BYTE *end = *(BYTE **)0x009c4f68;
        int cells = end >= (BYTE *)0x009c5050 ? (int)((end - (BYTE *)0x009c5050) / 8 + 1) : 0;
        logmsg("ground: window %d at (%d,%d), camera tile (%d,%d): %d visible cells, %d with objects, %d culled, "
               "%d faces, %d polygons\n",
               n, *(int *)0x00b42fa0, *(int *)0x00b42fa4, *(int *)0x00c42fe8, *(int *)0x00c42fec, cells,
               *(int *)0x00a0f7b0, *(int *)0x005e7834, *(int *)0x00a0bc90, *(int *)0x00b27a50);
    }
    static DWORD logged_at;
    static LONG logged_count;
    DWORD now = GetTickCount();
    if (now - logged_at >= 30000) {
        logged_at = now;
        if (projection_overflows != logged_count)
            logmsg("projection guard: %ld overflows saturated so far\n", projection_overflows);
        logged_count = projection_overflows;
    }
}

// The relocated arrays sat between other globals, and the game reads a little outside them
// (the coverage pass reads 16 bytes before a list's start: an access violation at 0x4b3f3c
// on a bare allocation, which aborted the pass and left polygons undrawn). Give each one a
// zeroed page of slack on both sides.
#define SLACK 4096
static void *alloc_padded(SIZE_T size) {
    BYTE *p = VirtualAlloc(NULL, size + 2 * SLACK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p ? p + SLACK : NULL;
}

int drawdist_install(int tiles, int start) {
    int guard = 1;
    for (int i = 0; i < (int)(sizeof projection_divides / sizeof projection_divides[0]); i++)
        guard &= ((BYTE *)projection_divides[i].eip)[0] == 0xF7;
    if (guard) AddVectoredExceptionHandler(1, projection_guard);
    else logmsg("projection guard: unexpected code, not installed\n");
    // A negative value is a diagnostic: install every relocation even at 61 or less.
    int force = tiles < 0;
    if (force) tiles = -tiles;
    if (tiles < GAME_MAX_TILES) tiles = GAME_MAX_TILES;
    if (tiles == GAME_MAX_TILES && !force) return -1;
    if (tiles > TILE_LIMIT) tiles = TILE_LIMIT;
    dd_tiles = tiles | 1;
    // start at worldstart, or by default at the middle step of Ctrl+Shift+F9's cycle
    if (start <= 0) start = (GAME_MAX_TILES + dd_tiles) / 2;
    dd_start = start < GAME_MAX_TILES ? GAME_MAX_TILES : start > dd_tiles ? dd_tiles : start | 1;
    // the polygon count grows with the window's area (peak 14899 at 61)
    double area = (double)dd_tiles * dd_tiles / (GAME_MAX_TILES * GAME_MAX_TILES);
    dd_poly_cap = (int)(15000 * area * 1.5);
    dd_vertex_cap = dd_poly_cap * 6;
    static const DWORD resize_calls[] = {0x004d762d, 0x004db2df, 0x004dd0f5, 0x004dd150,
                                         0x004dd1b3, 0x004dd1fa, 0x004dd268};
    face_rows = alloc_padded((SIZE_T)dd_tiles * dd_tiles * 4);
    poly_list = alloc_padded((SIZE_T)dd_poly_cap * POLY_SIZE);
    vertex_list = alloc_padded((SIZE_T)dd_vertex_cap * VERTEX_SIZE);
    terrain_size = (DWORD)(TERRAIN_SIZE * area * 1.5);
    terrain_a = alloc_padded(terrain_size);
    terrain_b = alloc_padded(terrain_size);
    int ok = face_rows && poly_list && vertex_list && terrain_a && terrain_b;
    for (dry_run = 1; ok && dry_run >= 0; dry_run--) {
        ok = grow_grid_pool() & move_face_table() & grow_draw_lists() & move_terrain_buffers();
        for (int i = 0; i < 7; i++) ok &= redirect_call(resize_calls[i], GRID_RESIZE, grid_resize);
    }
    dry_run = 0;
    if (!ok) dd_tiles = dd_start = GAME_MAX_TILES, dd_poly_cap = 15000, dd_vertex_cap = 60000, terrain_a = terrain_b = NULL;
    logmsg("draw distance: %s, max window %d tiles (game max %d), polygon list %d, vertex list %d, terrain buffers %lu bytes\n",
           ok ? "extended" : "PATCH FAILED", dd_tiles, GAME_MAX_TILES, dd_poly_cap, dd_vertex_cap,
           (unsigned long)terrain_size);
    return ok;
}
