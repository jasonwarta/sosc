// High-resolution texture replacements: game\textures\hd\<hash>.png, where <hash> is the
// 16-hex-digit key texturedump=1 gives the original (a hash of its 8-bit indices). The
// directory is indexed once at startup; images are decoded on first use with GDI+ (part of
// Windows, loaded dynamically so nothing else needs it).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "hdtex.h"
#include "log.h"

static unsigned long long *keys;
static int cap, count;

// Background preload: a worker thread decodes every replacement at startup, so the game's
// first use of a texture (a car or explosion appearing mid-drive) doesn't stall the frame on
// a PNG decode. Per slot (parallel to keys): PRE_PENDING, PRE_DECODING (worker busy),
// PRE_READY (pixels waiting), PRE_TAKEN (handed out, or decoded by the main thread itself).
enum { PRE_PENDING, PRE_DECODING, PRE_READY, PRE_TAKEN };
static volatile LONG *pre_state;
static unsigned long **pre_px;
static int *pre_w, *pre_h;
static volatile LONG pre_done, pre_bytes;

static void add_key(unsigned long long key) {
    if (count * 2 >= cap) {
        int ncap = cap ? cap * 2 : 1024;
        unsigned long long *nk = calloc(ncap, sizeof *nk);
        if (!nk) return;
        for (int i = 0; i < cap; i++) {
            if (!keys[i]) continue;
            int j = (int)(keys[i] & (ncap - 1));
            while (nk[j]) j = (j + 1) & (ncap - 1);
            nk[j] = keys[i];
        }
        free(keys);
        keys = nk, cap = ncap;
    }
    int j = (int)(key & (cap - 1));
    while (keys[j]) {
        if (keys[j] == key) return;
        j = (j + 1) & (cap - 1);
    }
    keys[j] = key;
    count++;
}

static int slot_of(unsigned long long key) {
    if (!count || !key) return -1;
    int j = (int)(key & (cap - 1));
    while (keys[j]) {
        if (keys[j] == key) return j;
        j = (j + 1) & (cap - 1);
    }
    return -1;
}

int hdtex_has(unsigned long long key) { return slot_of(key) >= 0; }

// GDI+ flat API, resolved at runtime.
typedef struct { UINT32 version; void *callback; BOOL no_thread, no_codecs; } GdiplusStartupInput;
typedef struct { UINT width, height; INT stride; INT format; void *scan0; UINT_PTR reserved; } GdipBitmapData;
typedef struct { INT x, y, w, h; } GdipRect;
#define PixelFormat32bppARGB 0x26200A
#define ImageLockModeRead    1
static int(WINAPI *GdiplusStartup)(ULONG_PTR *, const GdiplusStartupInput *, void *);
static int(WINAPI *GdipCreateBitmapFromFile)(const WCHAR *, void **);
static int(WINAPI *GdipGetImageWidth)(void *, UINT *);
static int(WINAPI *GdipGetImageHeight)(void *, UINT *);
static int(WINAPI *GdipBitmapLockBits)(void *, const GdipRect *, UINT, INT, GdipBitmapData *);
static int(WINAPI *GdipBitmapUnlockBits)(void *, GdipBitmapData *);
static int(WINAPI *GdipDisposeImage)(void *);
static char hd_dir[MAX_PATH];

static int load_gdiplus(void) {
    HMODULE m = LoadLibraryA("gdiplus.dll");
    if (!m) return 0;
    GdiplusStartup = (void *)GetProcAddress(m, "GdiplusStartup");
    GdipCreateBitmapFromFile = (void *)GetProcAddress(m, "GdipCreateBitmapFromFile");
    GdipGetImageWidth = (void *)GetProcAddress(m, "GdipGetImageWidth");
    GdipGetImageHeight = (void *)GetProcAddress(m, "GdipGetImageHeight");
    GdipBitmapLockBits = (void *)GetProcAddress(m, "GdipBitmapLockBits");
    GdipBitmapUnlockBits = (void *)GetProcAddress(m, "GdipBitmapUnlockBits");
    GdipDisposeImage = (void *)GetProcAddress(m, "GdipDisposeImage");
    if (!GdiplusStartup || !GdipCreateBitmapFromFile || !GdipGetImageWidth || !GdipGetImageHeight ||
        !GdipBitmapLockBits || !GdipBitmapUnlockBits || !GdipDisposeImage)
        return 0;
    ULONG_PTR token;
    GdiplusStartupInput in = {1, NULL, FALSE, FALSE};
    return GdiplusStartup(&token, &in, NULL) == 0;
}

int hdtex_init(const char *dir) {
    snprintf(hd_dir, sizeof hd_dir, "%s", dir);
    char pattern[MAX_PATH + 8];
    snprintf(pattern, sizeof pattern, "%s\\*.png", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            unsigned long long key = 0;
            char tail[8];
            if (sscanf(fd.cFileName, "%16llx%7s", &key, tail) == 2 && !_stricmp(tail, ".png") && key) add_key(key);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (count && !load_gdiplus()) {
        logmsg("hd textures: GDI+ unavailable, %d replacements ignored\n", count);
        count = 0;
    }
    return count;
}

// Decode <key>.png as RGBA (0xAABBGGRR) rows bottom-up, the way the dump wrote them, so row 0
// lines up with the game's first row. Caller frees.
static unsigned long *load_now(unsigned long long key, int *w, int *h) {
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\%016llx.png", hd_dir, key);
    return hdtex_load_file(path, w, h);
}

unsigned long *hdtex_load(unsigned long long key, int *w, int *h) {
    int j = slot_of(key);
    if (j >= 0 && pre_state) {
        if (InterlockedCompareExchange(&pre_state[j], PRE_TAKEN, PRE_READY) == PRE_READY) {
            *w = pre_w[j], *h = pre_h[j];
            InterlockedExchangeAdd(&pre_bytes, -(LONG)(pre_w[j] * pre_h[j] * 4));
            unsigned long *px = pre_px[j];
            pre_px[j] = NULL;
            return px;
        }
        // not decoded yet (or already handed out once): decode here; a worker still busy with
        // it sees PRE_TAKEN when it finishes and drops its copy
        InterlockedExchange(&pre_state[j], PRE_TAKEN);
    }
    return load_now(key, w, h);
}

static DWORD WINAPI preload_thread(void *arg) {
    (void)arg;
    DWORD start = GetTickCount();
    int decoded = 0;
    for (int j = 0; j < cap; j++) {
        if (!keys[j] || InterlockedCompareExchange(&pre_state[j], PRE_DECODING, PRE_PENDING) != PRE_PENDING) continue;
        int w = 0, h = 0;
        unsigned long *px = load_now(keys[j], &w, &h);
        if (!px) {
            InterlockedExchange(&pre_state[j], PRE_TAKEN);
            continue;
        }
        pre_px[j] = px, pre_w[j] = w, pre_h[j] = h;
        if (InterlockedCompareExchange(&pre_state[j], PRE_READY, PRE_DECODING) != PRE_DECODING) {
            pre_px[j] = NULL;  // the main thread decoded it meanwhile
            free(px);
            continue;
        }
        InterlockedExchangeAdd(&pre_bytes, w * h * 4);
        decoded++;
    }
    pre_done = 1;
    logmsg("hd textures: preloaded %d in %lu ms (%ld MB decoded, waiting for upload)\n", decoded,
           GetTickCount() - start, pre_bytes >> 20);
    return 0;
}

void hdtex_preload_start(void) {
    if (!count || pre_state) return;
    pre_state = calloc(cap, sizeof *pre_state);
    pre_px = calloc(cap, sizeof *pre_px);
    pre_w = calloc(cap, sizeof *pre_w);
    pre_h = calloc(cap, sizeof *pre_h);
    if (!pre_state || !pre_px || !pre_w || !pre_h) {
        free((void *)pre_state), free(pre_px), free(pre_w), free(pre_h);
        pre_state = NULL;
        return;
    }
    HANDLE t = CreateThread(NULL, 0, preload_thread, NULL, 0, NULL);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
}

unsigned long *hdtex_load_file(const char *file, int *w, int *h) {
    static int gdiplus_ready = -1;
    if (gdiplus_ready < 0) gdiplus_ready = GdipCreateBitmapFromFile ? 1 : load_gdiplus();
    if (!gdiplus_ready || GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES) return NULL;
    WCHAR path[MAX_PATH + 32];
    swprintf(path, sizeof path / sizeof path[0], L"%hs", file);
    void *bmp = NULL;
    if (GdipCreateBitmapFromFile(path, &bmp) != 0 || !bmp) return NULL;
    UINT bw = 0, bh = 0;
    GdipGetImageWidth(bmp, &bw);
    GdipGetImageHeight(bmp, &bh);
    unsigned long *out = NULL;
    GdipRect rect = {0, 0, (INT)bw, (INT)bh};
    GdipBitmapData data;
    if (bw && bh && bw <= 4096 && bh <= 4096 &&
        GdipBitmapLockBits(bmp, &rect, ImageLockModeRead, PixelFormat32bppARGB, &data) == 0) {
        out = malloc((size_t)bw * bh * 4);
        if (out) {
            for (UINT y = 0; y < bh; y++) {
                const BYTE *src = (const BYTE *)data.scan0 + (size_t)y * data.stride;  // B G R A
                unsigned long *dst = out + (size_t)(bh - 1 - y) * bw;
                for (UINT x = 0; x < bw; x++, src += 4)
                    dst[x] = (unsigned long)src[2] | (unsigned long)src[1] << 8 | (unsigned long)src[0] << 16 |
                             (unsigned long)src[3] << 24;
            }
        }
        GdipBitmapUnlockBits(bmp, &data);
    }
    GdipDisposeImage(bmp);
    if (out) *w = (int)bw, *h = (int)bh;
    return out;
}
