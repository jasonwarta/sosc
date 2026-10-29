// Screenshot of the game window as it appears on screen (works for every renderer and
// wrapper, since it copies from the desktop), saved as PNG through GDI+.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "log.h"
#include "screenshot.h"

typedef struct { UINT32 version; void *callback; BOOL no_thread; BOOL no_codecs; } GdiplusStartupInput;
typedef int(WINAPI *GdiplusStartup_t)(ULONG_PTR *, const GdiplusStartupInput *, void *);
typedef int(WINAPI *GdipCreateBitmapFromHBITMAP_t)(HBITMAP, HPALETTE, void **);
typedef int(WINAPI *GdipSaveImageToFile_t)(void *, const WCHAR *, const CLSID *, const void *);
typedef int(WINAPI *GdipDisposeImage_t)(void *);

static const CLSID png_encoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

static int save_png(HBITMAP bmp, const char *path) {
    static ULONG_PTR token;
    static GdipCreateBitmapFromHBITMAP_t create;
    static GdipSaveImageToFile_t save;
    static GdipDisposeImage_t dispose;
    if (!token) {
        HMODULE g = LoadLibraryA("gdiplus.dll");
        GdiplusStartup_t startup = g ? (GdiplusStartup_t)GetProcAddress(g, "GdiplusStartup") : NULL;
        create = g ? (GdipCreateBitmapFromHBITMAP_t)GetProcAddress(g, "GdipCreateBitmapFromHBITMAP") : NULL;
        save = g ? (GdipSaveImageToFile_t)GetProcAddress(g, "GdipSaveImageToFile") : NULL;
        dispose = g ? (GdipDisposeImage_t)GetProcAddress(g, "GdipDisposeImage") : NULL;
        GdiplusStartupInput in = {1, NULL, FALSE, FALSE};
        if (!startup || !create || !save || !dispose || startup(&token, &in, NULL) != 0) return 0;
    }
    void *image = NULL;
    if (create(bmp, NULL, &image) != 0 || !image) return 0;
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    int ok = save(image, wpath, &png_encoder, NULL) == 0;
    dispose(image);
    return ok;
}

typedef int(WINAPI *GdipCreateBitmapFromScan0_t)(int, int, int, int, BYTE *, void **);

// rgba: rows bottom-up (OpenGL order), each pixel 0xAABBGGRR.
int save_rgba_png(const unsigned long *rgba, int w, int h, const char *path) {
    // Make sure GDI+ is started (save_png does it on first use).
    HMODULE g = LoadLibraryA("gdiplus.dll");
    GdipCreateBitmapFromScan0_t from_scan0 = g ? (GdipCreateBitmapFromScan0_t)GetProcAddress(g, "GdipCreateBitmapFromScan0") : NULL;
    if (!from_scan0) return 0;
    DWORD *bgra = malloc((size_t)w * h * 4);
    if (!bgra) return 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            DWORD p = rgba[(size_t)(h - 1 - y) * w + x];
            bgra[(size_t)y * w + x] = (p & 0xFF00FF00) | (p & 0xFF) << 16 | (p >> 16 & 0xFF);
        }
    // GDI+ needs to be running before the bitmap call; a 1x1 save_png primes it.
    static int primed;
    if (!primed) {
        HDC dc = GetDC(NULL);
        HBITMAP tmp = CreateCompatibleBitmap(dc, 1, 1);
        char tmp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp_path);
        lstrcatA(tmp_path, "streets_fix_prime.png");
        save_png(tmp, tmp_path);
        DeleteFileA(tmp_path);
        DeleteObject(tmp);
        ReleaseDC(NULL, dc);
        primed = 1;
    }
    void *image = NULL;
    int ok = from_scan0(w, h, w * 4, 0x26200A /* PixelFormat32bppARGB */, (BYTE *)bgra, &image) == 0 && image;
    if (ok) {
        WCHAR wpath[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
        HMODULE gp = GetModuleHandleA("gdiplus.dll");
        GdipSaveImageToFile_t save = (GdipSaveImageToFile_t)GetProcAddress(gp, "GdipSaveImageToFile");
        GdipDisposeImage_t dispose = (GdipDisposeImage_t)GetProcAddress(gp, "GdipDisposeImage");
        ok = save(image, wpath, &png_encoder, NULL) == 0;
        dispose(image);
    }
    free(bgra);
    return ok;
}

void take_screenshot(HWND window, const char *game_dir) {
    // GetWindowInfo: cnc-ddraw hooks GetClientRect/GetWindowRect to report 640x480.
    WINDOWINFO wi = {sizeof wi};
    if (!window || !GetWindowInfo(window, &wi)) {
        logmsg("screenshot: no game window\n");
        return;
    }
    RECT r = wi.rcClient;
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);

    char dir[MAX_PATH], path[MAX_PATH + 64];
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(dir, sizeof dir, "%s\\screenshots", game_dir);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\shot_%04d%02d%02d_%02d%02d%02d_%03d.png", dir, t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond, t.wMilliseconds);
    if (save_png(bmp, path))
        logmsg("screenshot saved (%dx%d): %s\n", w, h, path);
    else
        logmsg("screenshot FAILED (%dx%d): %s\n", w, h, path);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}
