// streets_fix.dll - runtime fixes for Streets of SimCity (1997) on modern Windows.
// Loaded into Streets.exe by streets_launch.exe before the game's entry point runs.
//
//  * frame limiter: paces the main loop (FUN_005169b0) so the per-frame simulation
//    runs at a 1997-ish rate instead of hundreds of fps
//  * WASD: rewrites W/A/S/D key messages into arrow keys
//  * teardown guard: avoids a null-pointer crash when a game session ends
//  * no-CD: lets the game folder (a full copy of the disc) stand in for the CD
//  * dialogs: centers Open/Save dialogs and message boxes so the mouse can reach them
//
// Settings live in streets_fix.ini next to Streets.exe.
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "log.h"
#include "opengl.h"
#include "screenshot.h"
#include "widescreen.h"
#include "drawdist.h"
#include "mods.h"

// The main loop (FUN_005169b0) loads PeekMessageA from its import slot into edi at
// 0x516ac0 and calls it through edi. cnc-ddraw also hooks that import slot, so instead of
// competing for the slot, that load is patched to point edi at our hook, which calls
// whatever the slot holds (cnc-ddraw's hook or the real function).
#define MAINLOOP_PEEK_LOAD 0x00516ac0  // mov edi, [PEEK_IAT_SLOT]
#define PEEK_IAT_SLOT      0x00c53a2c

#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_ 0x00000002

typedef BOOL(WINAPI *PeekMessageA_t)(LPMSG, HWND, UINT, UINT, UINT);

static int cfg_fps = 30;
static int cfg_wasd = 1;
static int cfg_logfiles;
static LARGE_INTEGER qpc_freq;
static LONGLONG next_frame;
static LONGLONG frame_ticks;
static HANDLE frame_timer;
static char ini_path[MAX_PATH + 32];
static char game_dir[MAX_PATH];
static HWND game_window;  // first window the game creates (class Sparkal)

static LONGLONG now_ticks(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// Sleep until the next frame deadline: coarse wait on a high-resolution timer, then spin.
enum { PROF_SIM, PROF_RENDER, PROF_POST, PROF_SWAP, PROF_COUNT };
static LONGLONG prof_ticks[PROF_COUNT];  // frame profile buckets (install_frame_profile)
static LONGLONG work_ticks, wait_done;  // time the game spends on its frames, between our waits

static void wait_for_next_frame(void) {
    LONGLONG now = now_ticks();
    if (wait_done) work_ticks += now - wait_done;
    if (next_frame == 0 || now - next_frame > frame_ticks) {
        // first frame, or we fell behind by more than a frame: don't try to catch up
        next_frame = now + frame_ticks;
        wait_done = now_ticks();
        return;
    }
    LONGLONG remaining = next_frame - now;
    LONGLONG spin_ticks = qpc_freq.QuadPart / 1000;  // spin the final ~1 ms
    if (remaining > spin_ticks && frame_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - spin_ticks) * 10000000 / qpc_freq.QuadPart);  // relative, 100 ns units
        if (SetWaitableTimer(frame_timer, &due, 0, NULL, NULL, FALSE))
            WaitForSingleObject(frame_timer, INFINITE);
    }
    while (now_ticks() < next_frame)
        YieldProcessor();
    next_frame += frame_ticks;
    wait_done = now_ticks();
}

static DWORD main_thread_id;
static volatile LONGLONG last_frame_seen;

// Stall watchdog: when the main loop hasn't produced a frame for 500 ms, sample where the
// main thread is (instruction pointer plus return addresses on its stack) every 100 ms.
// The thread is only suspended long enough to copy its registers and stack; everything
// that might take a lock (formatting, module lookup) happens after it is resumed.
static void log_code_address(char *out, size_t size, DWORD addr) {
    HMODULE mod;
    char name[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(UINT_PTR)addr, &mod) && GetModuleFileNameA(mod, name, sizeof name)) {
        const char *base = strrchr(name, '\\');
        snprintf(out, size, "%08lx (%s+%lx)", addr, base ? base + 1 : name, addr - (DWORD)(UINT_PTR)mod);
    } else {
        snprintf(out, size, "%08lx", addr);
    }
}

static DWORD WINAPI stall_watchdog(LPVOID arg) {
    (void)arg;
    HANDLE main_thread = NULL;
    int samples = 0;
    for (;;) {
        Sleep(100);
        if (!main_thread_id || !last_frame_seen) continue;
        if (!main_thread) main_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, main_thread_id);
        if (!main_thread) return 0;
        LONGLONG stalled = now_ticks() - last_frame_seen;
        if (stalled < qpc_freq.QuadPart / 2) {
            samples = 0;
            continue;
        }
        if (++samples > 100) continue;  // cap per stall

        CONTEXT c = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
        DWORD stack[512];
        int nstack = 0;
        if (SuspendThread(main_thread) == (DWORD)-1) continue;
        BOOL ok = GetThreadContext(main_thread, &c);
        if (ok) {
            DWORD *sp = (DWORD *)c.Esp;
            for (; nstack < 512 && !IsBadReadPtr(sp + nstack, 4); nstack++) stack[nstack] = sp[nstack];
        }
        ResumeThread(main_thread);
        if (!ok) continue;

        char eip[96], line[640];
        log_code_address(eip, sizeof eip, c.Eip);
        int n = snprintf(line, sizeof line, "stall %.0f ms: eip=%s stack:", stalled * 1000.0 / qpc_freq.QuadPart, eip);
        for (int i = 0, found = 0; i < nstack && found < 16; i++) {
            if (stack[i] >= 0x00401000 && stack[i] < 0x005c3000) {
                n += snprintf(line + n, sizeof line - n, " %08lx", stack[i]);
                found++;
            }
        }
        logmsg("%s\n", line);
    }
}

// Diagnostic for widescreen work: scan the game's global data for values that look like a
// 640x480 viewport (width/height or half of them) in every numeric format the engine might
// use, and log where they sit. Code that reads these addresses is the projection.
#define GAME_DATA_START 0x005e6000
#define GAME_DATA_END   0x00853000

static void scan_viewport_values(const char *why) {
    static const struct { const char *name; DWORD bits; int dbl; } pats[] = {
        {"int 320", 320, 0}, {"int 240", 240, 0}, {"int 640", 640, 0}, {"int 480", 480, 0},
        {"int 319", 319, 0}, {"int 239", 239, 0}, {"int 639", 639, 0}, {"int 479", 479, 0},
        {"float 320", 0x43A00000, 0}, {"float 240", 0x43700000, 0}, {"float 640", 0x44200000, 0}, {"float 480", 0x43F00000, 0},
        {"float 319.5", 0x439FC000, 0}, {"float 239.5", 0x436F8000, 0},
        {"fixed 320", 0x01400000, 0}, {"fixed 240", 0x00F00000, 0}, {"fixed 640", 0x02800000, 0}, {"fixed 480", 0x01E00000, 0},
        {"double 320", 0x40740000, 1}, {"double 240", 0x406E0000, 1}, {"double 640", 0x40840000, 1}, {"double 480", 0x407E0000, 1},
    };
    logmsg("viewport scan (%s):\n", why);
    for (DWORD a = GAME_DATA_START; a < GAME_DATA_END; a += 4) {
        DWORD v = *(DWORD *)a;
        for (int i = 0; i < (int)(sizeof pats / sizeof pats[0]); i++) {
            if (pats[i].dbl) {
                // double: high dword matches and low dword is zero
                if (v == pats[i].bits && a >= GAME_DATA_START + 4 && *(DWORD *)(a - 4) == 0)
                    logmsg("  %08lx %s\n", a - 4, pats[i].name);
            } else if (v == pats[i].bits) {
                // small ints are everywhere; only report them next to a partner value
                if (pats[i].bits < 0x1000) {
                    int partner = 0;
                    for (int d = -16; d <= 16 && !partner; d += 4) {
                        DWORD w = *(DWORD *)(a + d);
                        if (d && (w == 320 || w == 240 || w == 640 || w == 480 || w == 639 || w == 479)) partner = 1;
                    }
                    if (!partner) continue;
                }
                logmsg("  %08lx %s\n", a, pats[i].name);
            }
        }
    }
    logmsg("viewport scan done\n");
}

static DWORD WINAPI delayed_viewport_scan(LPVOID arg) {
    (void)arg;
    Sleep(5000);
    scan_viewport_values("5 s after 3D start");
    return 0;
}

// Global hotkeys, on their own thread so they work even when a wrapper has grabbed the
// screen, mouse and keyboard:
//   Ctrl+Shift+F12  emergency exit
//   Ctrl+Shift+F11  viewport scan (widescreen research)
//   Ctrl+Shift+F10  screenshot to game\screenshots
//   Ctrl+Shift+F9   draw distance: cycle 61 tiles, a middle size and worldtiles
static DWORD WINAPI panic_hotkey_thread(LPVOID arg) {
    (void)arg;
    if (!RegisterHotKey(NULL, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12)) {
        logmsg("panic hotkey Ctrl+Shift+F12 could not be registered (error %lu)\n", GetLastError());
        return 0;
    }
    if (!RegisterHotKey(NULL, 2, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F11))
        logmsg("scan hotkey Ctrl+Shift+F11 could not be registered (error %lu)\n", GetLastError());
    if (!RegisterHotKey(NULL, 3, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F10))
        logmsg("screenshot hotkey Ctrl+Shift+F10 could not be registered (error %lu)\n", GetLastError());
    if (!RegisterHotKey(NULL, 4, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F9))
        logmsg("draw distance hotkey Ctrl+Shift+F9 could not be registered (error %lu)\n", GetLastError());
    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_HOTKEY && m.wParam == 4) {
            drawdist_toggle();
        } else if (m.message == WM_HOTKEY && m.wParam == 3) {
            take_screenshot(game_window, game_dir);
        } else if (m.message == WM_HOTKEY && m.wParam == 2) {
            scan_viewport_values("Ctrl+Shift+F11");
        } else if (m.message == WM_HOTKEY) {
            logmsg("panic hotkey pressed: terminating\n");
            ClipCursor(NULL);
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    return 0;
}

// Log the achieved main-loop rate every 2 seconds, and any single frame that took longer
// than 250 ms (loading, mode switches, stalls).
static void count_frame(void) {
    static LONGLONG window_start, last_frame;
    static int frames;
    LONGLONG now = now_ticks();
    if (!main_thread_id) main_thread_id = GetCurrentThreadId();
    last_frame_seen = now;
    if (!window_start) window_start = now;
    if (last_frame && now - last_frame > qpc_freq.QuadPart / 4)
        logmsg("hitch: %.0f ms since previous frame\n", (now - last_frame) * 1000.0 / qpc_freq.QuadPart);
    last_frame = now;
    frames++;
    if (now - window_start >= qpc_freq.QuadPart * 2) {
        double ms = 1000.0 / qpc_freq.QuadPart / frames;
        logmsg("main loop: %.1f fps, %.1f ms of work per frame (sim %.1f, render %.1f, post %.1f, swap %.1f)\n",
               frames * (double)qpc_freq.QuadPart / (now - window_start), work_ticks * ms, prof_ticks[PROF_SIM] * ms,
               prof_ticks[PROF_RENDER] * ms, prof_ticks[PROF_POST] * ms, prof_ticks[PROF_SWAP] * ms);
        work_ticks = 0;
        memset(prof_ticks, 0, sizeof prof_ticks);
        window_start = now;
        frames = 0;
    }
}

static void remap_wasd(LPMSG m) {
    if (m->message != WM_KEYDOWN && m->message != WM_KEYUP) return;
    WPARAM vk;
    UINT scan;
    switch (m->wParam) {
    case 'W': vk = VK_UP;    scan = 0x48; break;
    case 'S': vk = VK_DOWN;  scan = 0x50; break;
    case 'A': vk = VK_LEFT;  scan = 0x4B; break;
    case 'D': vk = VK_RIGHT; scan = 0x4D; break;
    default: return;
    }
    m->wParam = vk;
    // scan code in bits 16-23, extended-key flag in bit 24 (arrow keys are extended)
    m->lParam = (m->lParam & ~0x01FF0000) | ((LPARAM)scan << 16) | (1 << 24);
}

static BOOL WINAPI hook_PeekMessageA(LPMSG msg, HWND hwnd, UINT min, UINT max, UINT remove) {
    BOOL got = (*(PeekMessageA_t *)PEEK_IAT_SLOT)(msg, hwnd, min, max, remove);
    if (got) {
        if (cfg_wasd) remap_wasd(msg);
    } else {
        opengl_track_palette();
        widescreen_frame();
    }
    if (!got && cfg_fps > 0) {
        // Empty queue in the main loop means the game is about to tick a frame.
        wait_for_next_frame();
        count_frame();
        opengl_frame_tick();
    }
    return got;
}

static int install_mainloop_peek(void) {
    static const BYTE expected[] = {0x8B, 0x3D, 0x2C, 0x3A, 0xC5, 0x00};
    BYTE *site = (BYTE *)MAINLOOP_PEEK_LOAD;
    if (memcmp(site, expected, sizeof expected) != 0) return 0;
    DWORD old;
    VirtualProtect(site, sizeof expected, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xBF;  // mov edi, imm32
    *(DWORD *)(site + 1) = (DWORD)(UINT_PTR)hook_PeekMessageA;
    site[5] = 0x90;
    VirtualProtect(site, sizeof expected, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof expected);
    return 1;
}

// Log when the game switches the Glide (3dfx) renderer on and off.
typedef int(__stdcall *grSstWinOpen_t)(DWORD, int, int, int, int, int, int);
typedef int(__stdcall *grSstWinClose_t)(void);
static grSstWinOpen_t real_grSstWinOpen;
static grSstWinClose_t real_grSstWinClose;

static int __stdcall hook_grSstWinOpen(DWORD hwnd, int res, int refresh, int fmt, int origin, int ncol, int naux) {
    logmsg("glide: grSstWinOpen(hwnd=%08lx res=%d refresh=%d) ...\n", hwnd, res, refresh);
    DWORD t0 = GetTickCount();
    int r = real_grSstWinOpen(hwnd, res, refresh, fmt, origin, ncol, naux);
    logmsg("glide: grSstWinOpen -> %d (%lu ms)\n", r, GetTickCount() - t0);
    static int scanned;
    if (!scanned++ && GetPrivateProfileIntA("fix", "viewportscan", 0, ini_path))
        CloseHandle(CreateThread(NULL, 0, delayed_viewport_scan, NULL, 0, NULL));
    return r;
}

static int __stdcall hook_grSstWinClose(void) {
    DWORD t0 = GetTickCount();
    int r = real_grSstWinClose();
    logmsg("glide: grSstWinClose -> %d (%lu ms)\n", r, GetTickCount() - t0);
    return r;
}

// Other renderer and lifecycle events.
typedef HRESULT(WINAPI *DirectDrawCreate_t)(GUID *, void **, IUnknown *);
typedef HGLRC(WINAPI *wglCreateContext_t)(HDC);
typedef void(WINAPI *ExitProcess_t)(UINT);
typedef void(WINAPI *FatalAppExitA_t)(UINT, LPCSTR);
typedef ATOM(WINAPI *RegisterClassA_t)(const WNDCLASSA *);
static DirectDrawCreate_t real_DirectDrawCreate;
static wglCreateContext_t real_wglCreateContext;
static ExitProcess_t real_ExitProcess;
static FatalAppExitA_t real_FatalAppExitA;
static RegisterClassA_t real_RegisterClassA;

static HRESULT WINAPI hook_DirectDrawCreate(GUID *guid, void **dd, IUnknown *outer) {
    DWORD t0 = GetTickCount();
    HRESULT r = real_DirectDrawCreate(guid, dd, outer);
    logmsg("ddraw: DirectDrawCreate -> %08lx (%lu ms)\n", r, GetTickCount() - t0);
    return r;
}

// The game's 2D projection for OpenGL; logged when it changes (it is set every frame).
typedef void(WINAPI *gluOrtho2D_t)(double, double, double, double);
static gluOrtho2D_t real_gluOrtho2D;

static void WINAPI hook_gluOrtho2D(double l, double r, double b, double t) {
    static double last[4];
    if (l != last[0] || r != last[1] || b != last[2] || t != last[3]) {
        logmsg("opengl: gluOrtho2D(%g, %g, %g, %g)\n", l, r, b, t);
        last[0] = l, last[1] = r, last[2] = b, last[3] = t;
    }
    real_gluOrtho2D(l, r, b, t);
}

// FUN_0053a730 deletes the GL context whenever the game goes to a 2D menu (Esc, the
// graphics dialog, the end-of-game screen) and creates a new one on the way back, relying
// on FUN_004abb30 to upload every texture again. Sometimes the world textures don't come
// back and the city renders black. With persistentgl=1 the first context is kept for the
// whole session: wglDeleteContext leaves it alone and wglCreateContext hands it back, with
// all its textures still loaded.
typedef BOOL(WINAPI *wglDeleteContext_t)(HGLRC);
static wglDeleteContext_t real_wglDeleteContext;
static int cfg_persistent_gl;
static HGLRC kept_context;
static HDC kept_dc;

static HGLRC WINAPI hook_wglCreateContext(HDC dc) {
    if (cfg_persistent_gl && kept_context && dc == kept_dc) {
        logmsg("opengl: wglCreateContext -> %p (kept context reused)\n", (void *)kept_context);
        opengl_context_created(0);
        return kept_context;
    }
    HGLRC r = real_wglCreateContext(dc);
    logmsg("opengl: wglCreateContext -> %p\n", (void *)r);
    if (cfg_persistent_gl && r && !kept_context) kept_context = r, kept_dc = dc;
    opengl_context_created(1);
    return r;
}

static BOOL WINAPI hook_wglDeleteContext(HGLRC ctx) {
    if (cfg_persistent_gl && ctx && ctx == kept_context) {
        logmsg("opengl: wglDeleteContext(%p) skipped, context kept\n", (void *)ctx);
        return TRUE;
    }
    return real_wglDeleteContext(ctx);
}

static void WINAPI hook_ExitProcess(UINT code) {
    logmsg("game called ExitProcess(%u)\n", code);
    real_ExitProcess(code);
}

static void WINAPI hook_FatalAppExitA(UINT action, LPCSTR msg) {
    logmsg("game called FatalAppExit: %s\n", msg ? msg : "(null)");
    real_FatalAppExitA(action, msg);
}

// The game shows its "unrecoverable error" messages in its own UI rather than a Windows
// message box; it loads the text with LoadStringA, so log any error string it loads.
typedef int(WINAPI *LoadStringA_t)(HINSTANCE, UINT, LPSTR, int);
static LoadStringA_t real_LoadStringA;

static int WINAPI hook_LoadStringA(HINSTANCE inst, UINT id, LPSTR buf, int size) {
    int n = real_LoadStringA(inst, id, buf, size);
    if (n > 0 && buf) {
        char lower[256];
        int i;
        for (i = 0; i < n && i < (int)sizeof lower - 1; i++) lower[i] = (char)tolower((unsigned char)buf[i]);
        lower[i] = 0;
        if (strstr(lower, "error") || strstr(lower, "unable to") || strstr(lower, "must quit"))
            logmsg("game loaded error text (string %u): %s\n", id, buf);
    }
    return n;
}

// Window procedure wrapper: logs focus, size and display changes for the game's windows.
#define MAX_WRAPPED_CLASSES 8
static WNDPROC wrapped_procs[MAX_WRAPPED_CLASSES];
static char wrapped_names[MAX_WRAPPED_CLASSES][64];
static int nwrapped;

static LRESULT log_window_message(int idx, HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ACTIVATEAPP: logmsg("window[%s]: WM_ACTIVATEAPP %s\n", wrapped_names[idx], wp ? "active" : "inactive"); break;
    case WM_SIZE: logmsg("window[%s]: WM_SIZE %ux%u type=%u\n", wrapped_names[idx], LOWORD(lp), HIWORD(lp), (unsigned)wp); break;
    case WM_DISPLAYCHANGE: logmsg("window[%s]: WM_DISPLAYCHANGE %ux%u %ubpp\n", wrapped_names[idx], LOWORD(lp), HIWORD(lp), (unsigned)wp); break;
    case WM_CREATE:
        if (!game_window) game_window = w;
        logmsg("window[%s]: WM_CREATE %p\n", wrapped_names[idx], (void *)w); break;
    case WM_CLOSE: logmsg("window[%s]: WM_CLOSE\n", wrapped_names[idx]); break;
    case WM_DESTROY: logmsg("window[%s]: WM_DESTROY\n", wrapped_names[idx]); break;
    }
    return CallWindowProcA(wrapped_procs[idx], w, m, wp, lp);
}

#define WRAPPED_PROC(i) \
    static LRESULT CALLBACK wrapped_proc_##i(HWND w, UINT m, WPARAM wp, LPARAM lp) { return log_window_message(i, w, m, wp, lp); }
WRAPPED_PROC(0) WRAPPED_PROC(1) WRAPPED_PROC(2) WRAPPED_PROC(3)
WRAPPED_PROC(4) WRAPPED_PROC(5) WRAPPED_PROC(6) WRAPPED_PROC(7)
static const WNDPROC wrapper_table[MAX_WRAPPED_CLASSES] = {
    wrapped_proc_0, wrapped_proc_1, wrapped_proc_2, wrapped_proc_3,
    wrapped_proc_4, wrapped_proc_5, wrapped_proc_6, wrapped_proc_7};

static ATOM WINAPI hook_RegisterClassA(const WNDCLASSA *wc) {
    const char *name = HIWORD(wc->lpszClassName) ? wc->lpszClassName : "#atom";
    if (nwrapped >= MAX_WRAPPED_CLASSES || !wc->lpfnWndProc) {
        logmsg("RegisterClassA %s (not wrapped)\n", name);
        return real_RegisterClassA(wc);
    }
    WNDCLASSA copy = *wc;
    int i = nwrapped++;
    wrapped_procs[i] = wc->lpfnWndProc;
    lstrcpynA(wrapped_names[i], name, sizeof wrapped_names[i]);
    copy.lpfnWndProc = wrapper_table[i];
    logmsg("RegisterClassA %s\n", name);
    return real_RegisterClassA(&copy);
}

// Replace an entry in the main module's import table.
static void *patch_iat(const char *dll, const char *func, void *hook) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA((char *)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *addrs = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (lstrcmpA((char *)ibn->Name, func) != 0) continue;
            DWORD old;
            void *orig = (void *)addrs->u1.Function;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            addrs->u1.Function = (UINT_PTR)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void *), old, &old);
            return orig;
        }
    }
    return NULL;
}

// Open/Save dialogs and message boxes appear at the screen's top-left, outside the 4:3 area
// that cnc-ddraw confines the mouse to, and the file dialog repositions itself after it is
// shown. While one of these calls is in progress, a watcher thread centers each visible
// dialog window of the game's thread once and keeps the cursor unclipped.
typedef BOOL(WINAPI *FileDialog_t)(LPOPENFILENAMEA);
typedef int(WINAPI *MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
static FileDialog_t real_GetOpenFileNameA, real_GetSaveFileNameA;
static MessageBoxA_t real_MessageBoxA;

struct dialog_watch {
    DWORD thread;
    volatile LONG stop;
    HWND centered[8];
    int ncentered;
};

static BOOL CALLBACK center_dialog(HWND w, LPARAM lp) {
    struct dialog_watch *dw = (struct dialog_watch *)lp;
    char cls[16];
    if (!IsWindowVisible(w) || !GetClassNameA(w, cls, sizeof cls) || strcmp(cls, "#32770") != 0) return TRUE;
    for (int i = 0; i < dw->ncentered; i++)
        if (dw->centered[i] == w) return TRUE;
    RECT r;
    MONITORINFO mi = {sizeof mi};
    if (GetWindowRect(w, &r) && GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        int x = (mi.rcMonitor.left + mi.rcMonitor.right - (r.right - r.left)) / 2;
        int y = (mi.rcMonitor.top + mi.rcMonitor.bottom - (r.bottom - r.top)) / 2;
        SetWindowPos(w, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE);
        SetForegroundWindow(w);
    }
    if (dw->ncentered < 8) dw->centered[dw->ncentered++] = w;
    return TRUE;
}

static DWORD WINAPI dialog_watcher(LPVOID arg) {
    struct dialog_watch *dw = arg;
    while (!dw->stop) {
        ClipCursor(NULL);
        EnumThreadWindows(dw->thread, center_dialog, (LPARAM)dw);
        Sleep(50);
    }
    return 0;
}

static HANDLE begin_centering(struct dialog_watch *dw) {
    memset(dw, 0, sizeof *dw);
    dw->thread = GetCurrentThreadId();
    return CreateThread(NULL, 0, dialog_watcher, dw, 0, NULL);
}

static void end_centering(struct dialog_watch *dw, HANDLE t) {
    if (!t) return;
    InterlockedExchange(&dw->stop, 1);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static BOOL WINAPI hook_GetOpenFileNameA(LPOPENFILENAMEA ofn) {
    struct dialog_watch dw;
    HANDLE t = begin_centering(&dw);
    BOOL r = real_GetOpenFileNameA(ofn);
    end_centering(&dw, t);
    return r;
}

static BOOL WINAPI hook_GetSaveFileNameA(LPOPENFILENAMEA ofn) {
    struct dialog_watch dw;
    HANDLE t = begin_centering(&dw);
    BOOL r = real_GetSaveFileNameA(ofn);
    end_centering(&dw, t);
    return r;
}

static int WINAPI hook_MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type) {
    logmsg("message box [%s]: %s\n", caption ? caption : "", text ? text : "");
    struct dialog_watch dw;
    HANDLE t = begin_centering(&dw);
    int r = real_MessageBoxA(owner, text, caption, type);
    end_centering(&dw, t);
    logmsg("message box closed: button %d\n", r);
    return r;
}

// Optional diagnostics: logfiles=1 records failed file accesses, logfiles=2 records all of them.
typedef HANDLE(WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HFILE(WINAPI *OpenFile_t)(LPCSTR, LPOFSTRUCT, UINT);
static CreateFileA_t real_CreateFileA;
static OpenFile_t real_OpenFile;

static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disp, DWORD flags, HANDLE tmpl) {
    HANDLE h = real_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    if (h == INVALID_HANDLE_VALUE)
        logmsg("CreateFileA FAILED (error %lu): %s\n", err, name);
    else if (cfg_logfiles >= 2)
        logmsg("CreateFileA ok: %s\n", name);
    SetLastError(err);
    return h;
}

typedef HANDLE(WINAPI *FindFirstFileA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef DWORD(WINAPI *GetFileAttributesA_t)(LPCSTR);
static FindFirstFileA_t real_FindFirstFileA;
static GetFileAttributesA_t real_GetFileAttributesA;

static HANDLE WINAPI hook_FindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA fd) {
    HANDLE h = real_FindFirstFileA(name, fd);
    if (h == INVALID_HANDLE_VALUE || cfg_logfiles >= 2) logmsg("FindFirstFileA %s: %s\n", h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", name);
    return h;
}

static DWORD WINAPI hook_GetFileAttributesA(LPCSTR name) {
    DWORD a = real_GetFileAttributesA(name);
    if (a == INVALID_FILE_ATTRIBUTES || cfg_logfiles >= 2) logmsg("GetFileAttributesA %s: %s\n", a == INVALID_FILE_ATTRIBUTES ? "FAILED" : "ok", name);
    return a;
}

static HFILE WINAPI hook_OpenFile(LPCSTR name, LPOFSTRUCT of, UINT style) {
    HFILE h = real_OpenFile(name, of, style);
    if (h == HFILE_ERROR) logmsg("OpenFile failed: %s\n", name);
    return h;
}

// The game-session destructor (FUN_00411e50) calls through the player-object global at
// 0x6925d4 without a null check; if the player object was already destroyed the game
// crashes while leaving a session. Detour 0x4120dd so the block is skipped when it's null.
#define PLAYER_GLOBAL      0x006925d4
#define GUARD_SITE         0x004120dd  // push 1 / mov ecx,[PLAYER_GLOBAL] / mov [ebp-0x50],esi
#define GUARD_RESUME       0x004120e8  // call 0x560290
#define GUARD_SKIP_TO      0x00412116  // first instruction after the player-object block

static void put_rel32(BYTE *at, DWORD target) {
    *(DWORD *)at = target - ((DWORD)(UINT_PTR)at + 4);
}

static int install_teardown_guard(void) {
    static const BYTE expected[] = {0x6A, 0x01, 0x8B, 0x0D, 0xD4, 0x25, 0x69, 0x00, 0x89, 0x75, 0xB0};
    BYTE *site = (BYTE *)GUARD_SITE;
    if (memcmp(site, expected, sizeof expected) != 0) return 0;  // different exe version

    BYTE *t = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    BYTE *p = t;
    *p++ = 0x83; *p++ = 0x3D; *(DWORD *)p = PLAYER_GLOBAL; p += 4; *p++ = 0x00;  // cmp dword [PLAYER_GLOBAL], 0
    *p++ = 0x0F; *p++ = 0x84; put_rel32(p, GUARD_SKIP_TO); p += 4;                 // je GUARD_SKIP_TO
    memcpy(p, expected, sizeof expected); p += sizeof expected;                     // original instructions
    *p++ = 0xE9; put_rel32(p, GUARD_RESUME); p += 4;                                // jmp GUARD_RESUME

    DWORD old;
    VirtualProtect(site, sizeof expected, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE9;
    put_rel32(site + 1, (DWORD)(UINT_PTR)t);
    memset(site + 5, 0x90, sizeof expected - 5);
    VirtualProtect(site, sizeof expected, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof expected);
    return 1;
}

// No-CD: the disc check (FUN_005322a0) only accepts a location whose drive reports zero
// free space (i.e. a CD), then looks for smk\intro.smk there. The game folder holds a full
// copy of the disc, so make the free-space call at 0x5323c7 return 0 for this check only.
#define NOCD_SITE 0x005323c7  // call FUN_00513120 (disk free bytes)

static int install_nocd(void) {
    BYTE *site = (BYTE *)NOCD_SITE;
    if (site[0] != 0xE8 || *(DWORD *)(site + 1) != 0x00513120 - (NOCD_SITE + 5)) return 0;
    static const BYTE patch[5] = {0xB8, 0x00, 0x00, 0x00, 0x00};  // mov eax, 0
    DWORD old;
    VirtualProtect(site, sizeof patch, PAGE_EXECUTE_READWRITE, &old);
    memcpy(site, patch, sizeof patch);
    VirtualProtect(site, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof patch);
    return 1;
}


// Fades: the game blocks the main loop while it fades the music volume (FUN_0051d350,
// 2000-2500 ms on every screen transition) and while it fades the 256-colour palette
// (FUN_00525fc0). Palette fades are invisible once Glide or the wrappers are drawing, so
// both turn into seconds of black screen. Cap their duration (third stack argument).
#define MUSIC_FADE_FN   0x0051d350  // __thiscall (from, to, duration_ms)
#define PALETTE_FADE_FN 0x00525fc0  // cdecl (colour, palette, duration_ms, steps)

// Detour fn so that the dword argument at [esp+arg_offset] is clamped to max before the
// original code runs. prologue_len bytes of whole, position-independent instructions are
// relocated into the trampoline.
static int install_arg_cap(DWORD fn, const BYTE *expected, int prologue_len, BYTE arg_offset, DWORD max) {
    BYTE *site = (BYTE *)fn;
    if (memcmp(site, expected, prologue_len) != 0) return 0;
    BYTE *t = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    BYTE *p = t;
    *p++ = 0x81; *p++ = 0x7C; *p++ = 0x24; *p++ = arg_offset; *(DWORD *)p = max; p += 4;  // cmp dword [esp+off], max
    *p++ = 0x76; *p++ = 0x08;                                                            // jbe +8
    *p++ = 0xC7; *p++ = 0x44; *p++ = 0x24; *p++ = arg_offset; *(DWORD *)p = max; p += 4;  // mov dword [esp+off], max
    memcpy(p, site, prologue_len); p += prologue_len;                                    // relocated prologue
    *p++ = 0xE9; put_rel32(p, fn + prologue_len); p += 4;                                // jmp back
    DWORD old;
    VirtualProtect(site, prologue_len, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xE9;
    put_rel32(site + 1, (DWORD)(UINT_PTR)t);
    memset(site + 5, 0x90, prologue_len - 5);
    VirtualProtect(site, prologue_len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, prologue_len);
    return 1;
}

static int install_fade_caps(DWORD max_ms) {
    static const BYTE music[] = {0x8B, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x10};    // mov eax,[esp+8] / sub esp,10h
    static const BYTE palette[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};       // mov eax, fs:[0]
    return install_arg_cap(MUSIC_FADE_FN, music, sizeof music, 0x0C, max_ms) &&
           install_arg_cap(PALETTE_FADE_FN, palette, sizeof palette, 0x0C, max_ms);
}

static void load_config(void) {
    char *slash;
    GetModuleFileNameA(NULL, game_dir, MAX_PATH);
    if ((slash = strrchr(game_dir, '\\'))) *slash = 0;
    snprintf(ini_path, sizeof ini_path, "%s\\streets_fix.ini", game_dir);
    cfg_fps = GetPrivateProfileIntA("fix", "fps", cfg_fps, ini_path);
    cfg_wasd = GetPrivateProfileIntA("fix", "wasd", cfg_wasd, ini_path);
    cfg_logfiles = GetPrivateProfileIntA("fix", "logfiles", 0, ini_path);
}

// Record the machine and install details that matter when someone else reports a problem.
static void log_environment(void) {
    OSVERSIONINFOA ov = {sizeof ov};
    typedef LONG(WINAPI *RtlGetVersion_t)(OSVERSIONINFOA *);
    RtlGetVersion_t rgv = (RtlGetVersion_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&ov);
    logmsg("streets_fix session log: %s\n", log_path());
    logmsg("command line: %s\n", GetCommandLineA());
    logmsg("windows %lu.%lu build %lu, game dir %s\n", ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber, game_dir);
    logmsg("primary display %dx%d, %d monitor(s)\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
           GetSystemMetrics(SM_CMONITORS));
    DISPLAY_DEVICEA dev = {sizeof dev};
    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++, dev.cb = sizeof dev)
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) logmsg("gpu: %s\n", dev.DeviceString);
    static const char *files[] = {"Streets.exe", "ddraw.dll", "ddraw.dll.cnc", "ddraw.ini", "glide2x.dll", "dgVoodoo.conf", "streets_fix.ini"};
    for (int i = 0; i < (int)(sizeof files / sizeof files[0]); i++) {
        char p[MAX_PATH + 32];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        snprintf(p, sizeof p, "%s\\%s", game_dir, files[i]);
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa))
            logmsg("file %s: %lu bytes\n", files[i], fa.nFileSizeLow);
        else
            logmsg("file %s: missing\n", files[i]);
    }
    logmsg("config: fps=%d wasd=%d logfiles=%d nocd=%d\n", cfg_fps, cfg_wasd, cfg_logfiles,
           GetPrivateProfileIntA("fix", "nocd", 1, ini_path));
}

// widescreen= in the ini: auto (the primary monitor's shape, default), off, or a ratio such
// as 16:9, 21:9 or 2.37. OpenGL renderer only: Glide and software stay 4:3.
// The software texture mapper (used for the car preview at gas stations and garages)
// keeps the texel offset as 20.12 fixed point and converts it with an unsigned shift. At a
// polygon edge the offset can dip slightly below zero, which reads about 1 MB past the
// texture. On Windows 95 that memory was usually mapped and gave a stray pixel; now it
// faults. Treat a faulting read at these two instructions as a black texel and carry on.
static const struct { DWORD eip; int is_ah; } texel_reads[] = {
    {0x004b7cbc, 1},  // mov ah, [esi]  (8A 26)
    {0x004b8e55, 0},  // mov al, [esi]  (8A 06)
};
static LONG texel_faults;

static LONG CALLBACK texel_guard(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->ExceptionInformation[0] != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    for (int i = 0; i < (int)(sizeof texel_reads / sizeof texel_reads[0]); i++) {
        if (c->Eip != texel_reads[i].eip) continue;
        c->Eax &= texel_reads[i].is_ah ? ~0xff00u : ~0xffu;
        c->Eip += 2;
        if (InterlockedIncrement(&texel_faults) <= 5)
            logmsg("texel guard: stray texture read at %08lx (esi=%08lx) skipped\n", c->Eip - 2, c->Esi);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int install_texel_guard(void) {
    static const BYTE expected[2][2] = {{0x8A, 0x26}, {0x8A, 0x06}};
    for (int i = 0; i < 2; i++)
        if (memcmp((void *)texel_reads[i].eip, expected[i], 2) != 0) return 0;
    // First in line, ahead of the fault logger, so a handled read isn't logged as a crash.
    return AddVectoredExceptionHandler(1, texel_guard) != NULL;
}

// Fixed-point divides FUN_004ab0fb (16.16) and FUN_004ab118 (20.12) trap with an integer
// overflow when the quotient doesn't fit. The car code at 0x59e222 computes a speed as
// (position change) / dt: after a warp or respawn the position jumps thousands of units in
// one frame, and at high frame rates (small dt) that overflows and kills the game. The game
// already zeroes that speed when dt is at its maximum (a hitch), so do the same for a jump.
// Any other caller gets the quotient saturated (they would have crashed before).
#define SPEED_DIVIDE_RET 0x0059e227
static const DWORD fixed_divides[] = {0x004ab114, 0x004ab131};  // idiv ecx (F7 F9)
static LONG divide_faults;

static LONG CALLBACK divide_guard(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    if (c->Eip != fixed_divides[0] && c->Eip != fixed_divides[1]) return EXCEPTION_CONTINUE_SEARCH;
    DWORD ret = *(DWORD *)(c->Ebp + 4);  // both functions keep a standard ebp frame
    DWORD q;
    if (ret == SPEED_DIVIDE_RET) q = 0;
    else q = (((LONG)c->Edx < 0) != ((LONG)c->Ecx < 0)) ? 0x80000001u : 0x7fffffffu;
    if (InterlockedIncrement(&divide_faults) <= 10)
        logmsg("divide guard: overflow at %08lx from %08lx (dividend %08lx:%08lx / %08lx) -> %08lx\n", c->Eip, ret,
               c->Edx, c->Eax, c->Ecx, q);
    c->Eax = q;
    c->Edx = 0;
    c->Eip += 2;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int install_divide_guard(void) {
    for (int i = 0; i < 2; i++)
        if (memcmp((void *)fixed_divides[i], "\xF7\xF9", 2) != 0) return 0;
    return AddVectoredExceptionHandler(1, divide_guard) != NULL;
}

// The frame function FUN_0044cb10 measures the time since the last frame with GetTickCount
// and hands the simulation dt = ms * 65536 / 1000 (0x68ce84, clamped to 100..0x8000):
// physics, AI and pedestrians all integrate with it. GetTickCount only moves every ~15.6 ms,
// so 33 ms frames measure 31 or 47 ms (jerky motion), and above 64 fps most frames measure
// 0, are clamped up to 1.5 ms, and the game runs fast (the "uncapped is too fast" problem).
// Replace that one call with a millisecond clock from QueryPerformanceCounter.
#define FRAME_TIME_CALL 0x0044cb4b  // call dword ptr [GetTickCount]

static DWORD WINAPI precise_ms(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (DWORD)(now.QuadPart * 1000 / qpc_freq.QuadPart);
}

static int install_precise_time(void) {
    static const BYTE expected[6] = {0xFF, 0x15, 0x5C, 0x37, 0xC5, 0x00};  // GetTickCount's import slot
    BYTE *site = (BYTE *)FRAME_TIME_CALL;
    if (memcmp(site, expected, 6) != 0) return 0;
    BYTE patch[6] = {0xE8, 0, 0, 0, 0, 0x90};
    *(DWORD *)(patch + 1) = (DWORD)(UINT_PTR)precise_ms - (FRAME_TIME_CALL + 5);
    DWORD old;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old);
    memcpy(site, patch, 6);
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    return 1;
}

// Frame profile: where the per-frame work goes. The frame function calls the simulation
// FUN_004dcd10, the 3D render FUN_004d6730 and FUN_0049c7a0 (all take no stack arguments,
// some use ecx), and presents through glFlush + SwapBuffers. Each call site is redirected
// to a __fastcall wrapper that passes ecx/edx through and adds the elapsed time.
typedef int(__fastcall *frame_call_t)(void *, void *);

#define PROF_WRAPPER(name, slot, target)                       \
    static int __fastcall name(void *ecx, void *edx) {         \
        LONGLONG t0 = now_ticks();                             \
        int r = ((frame_call_t)(target))(ecx, edx);            \
        prof_ticks[slot] += now_ticks() - t0;                  \
        return r;                                              \
    }
PROF_WRAPPER(prof_sim, PROF_SIM, 0x004dcd10)
PROF_WRAPPER(prof_render, PROF_RENDER, 0x004d6730)
PROF_WRAPPER(prof_post, PROF_POST, 0x0049c7a0)

typedef BOOL(WINAPI *SwapBuffers_t)(HDC);
static SwapBuffers_t real_SwapBuffers;
static BOOL WINAPI hook_SwapBuffers(HDC dc) {
    opengl_before_swap();
    LONGLONG t0 = now_ticks();
    BOOL r = real_SwapBuffers(dc);
    prof_ticks[PROF_SWAP] += now_ticks() - t0;
    return r;
}

static int redirect_call(DWORD site_addr, DWORD target, void *hook) {
    BYTE *site = (BYTE *)site_addr;
    if (site[0] != 0xE8 || site_addr + 5 + *(int *)(site + 1) != target) return 0;
    DWORD old;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(site + 1) = (DWORD)(UINT_PTR)hook - (site_addr + 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 1;
}

static int install_frame_profile(void) {
    return redirect_call(0x0044cb8d, 0x004dcd10, prof_sim) &
           redirect_call(0x0044cca4, 0x004d6730, prof_render) &
           redirect_call(0x0044ccab, 0x0049c7a0, prof_post);
}

// Entering 3D with OpenGL, FUN_0053a730 sets the DirectDraw mode to 640x480x16 (a 1997 GL
// driver needed a hicolor desktop). The game's message boxes ("YOU WON!", the Alt+F4 quit
// confirmation) are 8-bit bitmaps blitted while that 16-bit surface is still up: they come
// out at half width with garbage colours next to a black block. Under cnc-ddraw the GL
// window doesn't depend on the DirectDraw depth, so keep it at 8 bits throughout.
#define GL_DDRAW_DEPTH_PUSH 0x0053a764  // push 0x10 (bpp argument of SetDisplayMode)

static int install_ddraw_8bpp(void) {
    BYTE *site = (BYTE *)GL_DDRAW_DEPTH_PUSH;
    if (site[0] != 0x6A || site[1] != 0x10) return 0;
    DWORD old;
    VirtualProtect(site, 2, PAGE_EXECUTE_READWRITE, &old);
    site[1] = 0x08;
    VirtualProtect(site, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 2);
    return 1;
}

// Water waves. FUN_004dba90 steps the wave height map (+-1 unit on every water cell of one
// parity, from a 32-entry profile) and returns 1 when FUN_004db9f0 must rebuild the grid
// vertex heights. It is meant to be time-gated (an accumulator of the frame time against a
// threshold) but resets the threshold to 0 on every step, so it steps on every call, and
// FUN_004d74c0 calls it twice per frame (simulation and post-render): the waves run at 60
// steps/s at 30 fps and 240 at 120. Step it on real time instead, every wavems.
#define WAVE_STEP_CALL 0x004d75f4  // FUN_004d74c0: call FUN_004dba90
#define WAVE_STEP_FUNC 0x004dba90
static int cfg_wave_ms = 33;
static LONGLONG wave_next;

static int wave_step(void) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    LONGLONG step = freq.QuadPart * cfg_wave_ms / 1000;
    // start, or resync after a pause (menus, loading) instead of catching up
    if (!wave_next || now.QuadPart - wave_next > freq.QuadPart / 4) wave_next = now.QuadPart;
    if (now.QuadPart < wave_next) return 0;
    wave_next += step;
    return ((int (*)(void))WAVE_STEP_FUNC)();
}

static int install_wave_timing(int ms) {
    if (ms <= 0) return -1;
    cfg_wave_ms = ms;
    return redirect_call(WAVE_STEP_CALL, WAVE_STEP_FUNC, wave_step);
}

// Car collision size. FUN_005a15b0 (thiscall: car, position, heading) tests six points of a
// car against the city (FUN_005a1c10: the buildings' meshes): the middle and both corners of
// the front and of the back. They're placed from the vehicle's entry (0x2c bytes, by the
// type at car+0x18) in the size table at *0xa0a080: +0x08 length, +0x1c centre to front,
// +0x20 half width (the back is length - front). Cars clipped buildings they visibly cleared,
// so the test points are pulled in to carhitbox percent, only for this test: the table
// stays as is for everything else that reads it (127 places).
#define WORLD_HIT_FUNC 0x005a15b0
#define VEHICLE_SIZES  (*(BYTE **)0x00a0a080)
static int cfg_hitbox_pct = 100;
static unsigned hitbox_logged;  // vehicle types already logged (bit per type < 32)

static BYTE __fastcall world_hit(BYTE *car, void *edx, int *pos, int heading) {
    typedef BYTE(__fastcall * world_hit_t)(BYTE *, void *, int *, int);
    BYTE *sizes = VEHICLE_SIZES;
    int type = car ? *(int *)(car + 0x18) : -1;
    if (!sizes || type < 0 || type > 255 || cfg_hitbox_pct == 100)
        return ((world_hit_t)WORLD_HIT_FUNC)(car, edx, pos, heading);
    int *e = (int *)(sizes + type * 0x2c), saved[3] = {e[2], e[7], e[8]};
    if (type < 32 && !(hitbox_logged & 1u << type)) {
        hitbox_logged |= 1u << type;
        logmsg("car hitbox: vehicle type %d length %.2f, front %.2f, half width %.2f (units of 64k), tested at %d%%\n",
               type, e[2] / 65536.0, e[7] / 65536.0, e[8] / 65536.0, cfg_hitbox_pct);
    }
    e[2] = (int)((long long)e[2] * cfg_hitbox_pct / 100);
    e[7] = (int)((long long)e[7] * cfg_hitbox_pct / 100);
    e[8] = (int)((long long)e[8] * cfg_hitbox_pct / 100);
    BYTE hit = ((world_hit_t)WORLD_HIT_FUNC)(car, edx, pos, heading);
    e[2] = saved[0], e[7] = saved[1], e[8] = saved[2];
    return hit;
}

static int install_car_hitbox(int pct) {
    static const DWORD sites[] = {0x005a09e7, 0x005a0aab, 0x005a0aed, 0x005a0c1a, 0x005a0f2a};
    if (pct <= 0 || pct == 100) return -1;
    cfg_hitbox_pct = pct < 50 ? 50 : pct > 150 ? 150 : pct;
    int ok = 0;
    for (int i = 0; i < 5; i++) ok += redirect_call(sites[i], WORLD_HIT_FUNC, world_hit);
    return ok == 5;
}

// The game keeps its preferences in streets.cfg, a MIFF file of records (4-byte tag,
// 4-byte size including the 8-byte header, data). It only rewrites the file on a clean
// exit, so a changed setting is easily lost; set ours before the game reads the file.
//  * 'GOFV': draw-distance slider, 0..24 (map grid = slider x 2 + 13 tiles);
//  * 0xffffff02, first dword: 3D resolution, 0 always low, 1 low/high (drops to the
//    half-resolution view when busy), 2 always high.
// The other graphics options (0xffffff03 record: building/ground textures, sky) default on.
static int cfg_record_set(BYTE *buf, DWORD n, HANDLE f, const BYTE tag[4], DWORD size, int want, int *old) {
    for (DWORD i = 0; i + 12 <= n; i++) {
        if (memcmp(buf + i, tag, 4) != 0 || *(DWORD *)(buf + i + 4) != size) continue;
        *old = *(int *)(buf + i + 8);
        if (*old != want) {
            DWORD written = 0;
            SetFilePointer(f, i + 8, NULL, FILE_BEGIN);
            WriteFile(f, &want, 4, &written, NULL);
        }
        return 1;
    }
    return 0;
}

static int ini_choice(const char *key, const char *dflt, const char *const *names, int count, int max) {
    char value[16];
    GetPrivateProfileStringA("fix", key, dflt, value, sizeof value, ini_path);
    if (!_stricmp(value, "game")) return -1;
    for (int i = 0; i < count; i++)
        if (names[i] && !_stricmp(value, names[i])) return i;
    int v = atoi(value);
    return v < 0 ? 0 : v > max ? max : v;
}

// A streets.cfg for when there is none yet. The game only writes the file on a clean exit, so
// a fresh install starts without one, and then the settings above had nothing to change: the
// first run kept the game's default draw distance (a 25-tile window, too small for worldstart
// to apply). These are the preferences from a played install: full volumes, all graphics
// options on, high resolution, draw distance slider 24.
static const BYTE default_cfg[280] = {
    0x4d, 0x49, 0x46, 0x46, 0x02, 0x00, 0x00, 0x00, 0x43, 0x50, 0x54, 0x52, 0x50, 0x52, 0x45, 0x46,
    0x00, 0x00, 0x01, 0x00, 0x18, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00,
    0x30, 0x00, 0x00, 0x00, 0x0a, 0x0a, 0x2a, 0x2a, 0x2a, 0x44, 0x6f, 0x6e, 0x27, 0x74, 0x20, 0x74,
    0x72, 0x79, 0x20, 0x74, 0x6f, 0x20, 0x65, 0x64, 0x69, 0x74, 0x20, 0x74, 0x68, 0x69, 0x73, 0x20,
    0x66, 0x69, 0x6c, 0x65, 0x2a, 0x2a, 0x2a, 0x21, 0x21, 0x21, 0x0a, 0x0a, 0xff, 0xff, 0xff, 0x04,
    0x18, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
    0x10, 0x27, 0x00, 0x00, 0x4f, 0x49, 0x45, 0x52, 0x24, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x02, 0x28, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0x03, 0x18, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x05, 0x2c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0xf8, 0xff, 0x00, 0x00, 0x4e, 0x00,
    0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0xf0, 0xff, 0x00, 0x00, 0x52, 0x00, 0x00, 0x00, 0x3c, 0x00,
    0x00, 0x00, 0xf8, 0xff, 0xee, 0xee, 0xee, 0x04, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x5a, 0x50, 0x41, 0x4d, 0x0c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x47, 0x4f, 0x46, 0x56,
    0x0c, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
};

static void apply_game_settings(void) {
    static const char *const distance_names[25] = {[24] = "max"};
    static const char *const resolution_names[3] = {"low", "lowhigh", "high"};
    int distance = ini_choice("drawdistance", "max", distance_names, 25, 24);
    int resolution = ini_choice("resolution", "high", resolution_names, 3, 2);
    if (distance < 0 && resolution < 0) return;

    char path[MAX_PATH + 16];
    snprintf(path, sizeof path, "%s\\streets.cfg", game_dir);
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
        f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        DWORD written = 0;
        if (f != INVALID_HANDLE_VALUE && WriteFile(f, default_cfg, sizeof default_cfg, &written, NULL) &&
            written == sizeof default_cfg) {
            SetFilePointer(f, 0, NULL, FILE_BEGIN);
            logmsg("game settings: no streets.cfg yet, wrote the default one\n");
        } else if (f != INVALID_HANDLE_VALUE) {
            CloseHandle(f);
            f = INVALID_HANDLE_VALUE;
        }
    }
    if (f == INVALID_HANDLE_VALUE) {
        logmsg("game settings: streets.cfg can't be opened (error %lu), left alone\n", GetLastError());
        return;
    }
    BYTE buf[4096];
    DWORD n = 0;
    ReadFile(f, buf, sizeof buf, &n, NULL);
    int old;
    if (distance >= 0) {
        if (cfg_record_set(buf, n, f, (const BYTE *)"GOFV", 12, distance, &old))
            logmsg("draw distance: slider %d -> %d (%d map tiles)\n", old, distance, distance * 2 + 13);
        else
            logmsg("draw distance: no GOFV record in streets.cfg, left alone\n");
    }
    if (resolution >= 0) {
        static const BYTE tag[4] = {0xFF, 0xFF, 0xFF, 0x02};
        if (cfg_record_set(buf, n, f, tag, 0x28, resolution, &old))
            logmsg("3D resolution: %s -> %s\n", old >= 0 && old <= 2 ? resolution_names[old] : "?", resolution_names[resolution]);
        else
            logmsg("3D resolution: no record in streets.cfg, left alone\n");
    }
    CloseHandle(f);
}

static int install_widescreen(void) {
    char setting[32], *colon;
    GetPrivateProfileStringA("fix", "widescreen", "auto", setting, sizeof setting, ini_path);
    const char *cmd = GetCommandLineA();
    if (!strstr(cmd, "-o2") && !strstr(cmd, "-O2")) {
        logmsg("widescreen: OpenGL renderer not selected, staying 4:3\n");
        return 0;
    }
    double aspect;
    if (!lstrcmpiA(setting, "off") || !strcmp(setting, "0")) {
        aspect = 4.0 / 3.0;
    } else if (!lstrcmpiA(setting, "auto")) {
        int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        aspect = h > 0 ? (double)w / h : 4.0 / 3.0;
        logmsg("widescreen: auto, primary monitor %dx%d\n", w, h);
    } else if ((colon = strchr(setting, ':'))) {
        double h = atof(colon + 1);
        aspect = h > 0 ? atof(setting) / h : 4.0 / 3.0;
    } else {
        aspect = atof(setting);
    }
    return widescreen_install(aspect);
}

#define HOOK(dll, name, var, type, fn) (var = (type)patch_iat(dll, name, (void *)fn))

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst;
    if (reason == DLL_PROCESS_DETACH) {
        logmsg("process exiting (%s)\n", reserved ? "process termination" : "library unload");
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    load_config();
    log_init(game_dir);
    log_environment();
    mods_install(game_dir, GetPrivateProfileIntA("fix", "moose", 1, ini_path));
    QueryPerformanceFrequency(&qpc_freq);
    if (cfg_fps > 0) frame_ticks = qpc_freq.QuadPart / cfg_fps;
    frame_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION_, TIMER_ALL_ACCESS);
    if (!frame_timer) frame_timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    int peek = install_mainloop_peek();
    if (GetPrivateProfileIntA("fix", "stallwatch", 1, ini_path)) CloseHandle(CreateThread(NULL, 0, stall_watchdog, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, panic_hotkey_thread, NULL, 0, NULL));
    apply_game_settings();
    int guard = install_teardown_guard();
    int texel = install_texel_guard();
    int divide = install_divide_guard();
    int dd8 = GetPrivateProfileIntA("fix", "ddraw8bpp", 1, ini_path) ? install_ddraw_8bpp() : -1;
    int world = drawdist_install(GetPrivateProfileIntA("fix", "worldtiles", 61, ini_path),
                                 GetPrivateProfileIntA("fix", "worldstart", 0, ini_path));
    int waves = install_wave_timing(GetPrivateProfileIntA("fix", "wavems", 33, ini_path));
    int hitbox = install_car_hitbox(GetPrivateProfileIntA("fix", "carhitbox", 70, ini_path));
    logmsg("car hitbox: %s\n", hitbox < 0 ? "original size" : hitbox ? "ok" : "FAILED");
    int precise = GetPrivateProfileIntA("fix", "precisetime", 1, ini_path) ? install_precise_time() : -1;
    int profile = GetPrivateProfileIntA("fix", "frameprofile", 1, ini_path) ? install_frame_profile() : -1;
    HOOK("GDI32.dll", "SwapBuffers", real_SwapBuffers, SwapBuffers_t, hook_SwapBuffers);  // profile and the flash
    int nocd = GetPrivateProfileIntA("fix", "nocd", 1, ini_path) ? install_nocd() : -1;
    int fademax = GetPrivateProfileIntA("fix", "fademax", 250, ini_path);
    int fades = fademax > 0 ? install_fade_caps(fademax) : -1;
    HOOK("comdlg32.dll", "GetOpenFileNameA", real_GetOpenFileNameA, FileDialog_t, hook_GetOpenFileNameA);
    HOOK("comdlg32.dll", "GetSaveFileNameA", real_GetSaveFileNameA, FileDialog_t, hook_GetSaveFileNameA);
    HOOK("USER32.dll", "MessageBoxA", real_MessageBoxA, MessageBoxA_t, hook_MessageBoxA);
    HOOK("USER32.dll", "LoadStringA", real_LoadStringA, LoadStringA_t, hook_LoadStringA);
    HOOK("USER32.dll", "RegisterClassA", real_RegisterClassA, RegisterClassA_t, hook_RegisterClassA);
    HOOK("glide2x.dll", "_grSstWinOpen@28", real_grSstWinOpen, grSstWinOpen_t, hook_grSstWinOpen);
    HOOK("glide2x.dll", "_grSstWinClose@0", real_grSstWinClose, grSstWinClose_t, hook_grSstWinClose);
    HOOK("DDRAW.dll", "DirectDrawCreate", real_DirectDrawCreate, DirectDrawCreate_t, hook_DirectDrawCreate);
    HOOK("GLU32.dll", "gluOrtho2D", real_gluOrtho2D, gluOrtho2D_t, hook_gluOrtho2D);
    int wide = install_widescreen();
    int grid = grid_guard_install();
    int gl = opengl_install(patch_iat);
    opengl_set_sprite_depth(GetPrivateProfileIntA("fix", "spritedepth", 1, ini_path));
    opengl_set_extend_hud(GetPrivateProfileIntA("fix", "extendhud", 1, ini_path));
    opengl_set_tex_probe(GetPrivateProfileIntA("fix", "texprobe", 0, ini_path));
    opengl_set_water_ms(GetPrivateProfileIntA("fix", "wateranimms", 67, ini_path));
    opengl_set_hd_water(GetPrivateProfileIntA("fix", "hdwater", 0, ini_path));
    opengl_set_water_drift(GetPrivateProfileIntA("fix", "waterdrift", 4, ini_path));
    if (GetPrivateProfileIntA("fix", "dumpimages", 0, ini_path)) {
        char dump[MAX_PATH + 32];
        snprintf(dump, sizeof dump, "%s\\logs\\dumps", game_dir);
        opengl_set_dump_dir(dump);
        logmsg("image dumps enabled: %s\n", dump);
    }
    {
        char dir[MAX_PATH + 32];
        snprintf(dir, sizeof dir, "%s\\textures\\wide", game_dir);
        opengl_set_wide_dir(dir);
    }
    if (GetPrivateProfileIntA("fix", "hdtextures", 1, ini_path)) {
        char dir[MAX_PATH + 32];
        snprintf(dir, sizeof dir, "%s\\textures\\hd", game_dir);
        opengl_set_hd_dir(dir);
    }
    if (GetPrivateProfileIntA("fix", "texturedump", 0, ini_path)) {
        char dir[MAX_PATH + 32];
        snprintf(dir, sizeof dir, "%s\\textures", game_dir);
        CreateDirectoryA(dir, NULL);
        snprintf(dir, sizeof dir, "%s\\textures\\dump", game_dir);
        opengl_set_texture_census_dir(dir);
        logmsg("texture census enabled: %s\n", dir);
    }
    HOOK("OPENGL32.dll", "wglCreateContext", real_wglCreateContext, wglCreateContext_t, hook_wglCreateContext);
    cfg_persistent_gl = GetPrivateProfileIntA("fix", "persistentgl", 1, ini_path);
    if (cfg_persistent_gl)
        HOOK("OPENGL32.dll", "wglDeleteContext", real_wglDeleteContext, wglDeleteContext_t, hook_wglDeleteContext);
    logmsg("opengl: persistent context %s\n", !cfg_persistent_gl ? "off" : real_wglDeleteContext ? "on" : "FAILED");
    HOOK("KERNEL32.dll", "ExitProcess", real_ExitProcess, ExitProcess_t, hook_ExitProcess);
    HOOK("KERNEL32.dll", "FatalAppExitA", real_FatalAppExitA, FatalAppExitA_t, hook_FatalAppExitA);
    if (cfg_logfiles) {
        HOOK("KERNEL32.dll", "CreateFileA", real_CreateFileA, CreateFileA_t, hook_CreateFileA);
        HOOK("KERNEL32.dll", "OpenFile", real_OpenFile, OpenFile_t, hook_OpenFile);
        HOOK("KERNEL32.dll", "FindFirstFileA", real_FindFirstFileA, FindFirstFileA_t, hook_FindFirstFileA);
        HOOK("KERNEL32.dll", "GetFileAttributesA", real_GetFileAttributesA, GetFileAttributesA_t, hook_GetFileAttributesA);
    }
#define OK(x) ((x) ? "ok" : "FAILED")
    logmsg("patches: mainloop_peek=%s teardown_guard=%s texel_guard=%s divide_guard=%s ddraw8bpp=%s world_tiles=%s wave_timing=%s precise_time=%s frame_profile=%s nocd=%s fade_caps=%s (fademax=%d ms)\n", OK(peek), OK(guard), OK(texel), OK(divide),
           dd8 < 0 ? "off" : OK(dd8), world < 0 ? "61" : OK(world), waves < 0 ? "off" : OK(waves), precise < 0 ? "off" : OK(precise),
           profile < 0 ? "off" : OK(profile && real_SwapBuffers),
           nocd < 0 ? "off" : OK(nocd), fades < 0 ? "off" : OK(fades), fademax);
    logmsg("opengl presentation layer: %s, widescreen %s, grid guard %s\n", OK(gl), wide ? "on" : "off", OK(grid));
    logmsg("hooks: loadstring=%s dialogs=%s messagebox=%s registerclass=%s glide=%s ddraw=%s wgl=%s exit=%s\n",
           OK(real_LoadStringA), OK(real_GetOpenFileNameA && real_GetSaveFileNameA), OK(real_MessageBoxA), OK(real_RegisterClassA),
           OK(real_grSstWinOpen && real_grSstWinClose), OK(real_DirectDrawCreate), OK(real_wglCreateContext),
           OK(real_ExitProcess && real_FatalAppExitA));
    return TRUE;
}
