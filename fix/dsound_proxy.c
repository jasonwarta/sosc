// dsound.dll - loads streets_fix.dll into Streets.exe without a launcher.
//
// Streets.exe imports DSOUND.dll, and Windows looks for it in the game folder first, so this
// copy is loaded as part of the game's own startup. Every export of the real system
// dsound.dll is passed through under the same name and ordinal. (version.dll doesn't work:
// the compatibility shim Windows applies to the game loads the system copy first.)
//
// On load, the game's entry point is patched to jump to entry_stub, which loads
// streets_fix.dll and then runs the game: the same moment streets_launch.exe injects it, but
// with no remote thread (antivirus flags that as code injection). Other programs that load
// this are left alone.
//
// The renderer comes from the -o option in the shortcut (-o2 OpenGL, -o1 Glide); ddraw.ini's
// renderer is set to match, as streets_launch.exe does.
#include <windows.h>

// Name and ordinal of each export of the system dsound.dll.
#define EXPORTS(X)                                                                          \
    X(DirectSoundCreate, 1) X(DirectSoundEnumerateA, 2) X(DirectSoundEnumerateW, 3)         \
    X(DllCanUnloadNow, 4) X(DllGetClassObject, 5) X(DirectSoundCaptureCreate, 6)            \
    X(DirectSoundCaptureEnumerateA, 7) X(DirectSoundCaptureEnumerateW, 8) X(GetDeviceID, 9) \
    X(DirectSoundFullDuplexCreate, 10) X(DirectSoundCreate8, 11) X(DirectSoundCaptureCreate8, 12)

FARPROC real[13];  // by ordinal
BYTE *entry;
static BYTE orig[5];
static char dir[MAX_PATH];

#define THUNK(name, ord)                                                \
    void p_##name(void) __attribute__((naked));                         \
    void p_##name(void) { __asm__("jmp *_real+" #ord "*4"); }           \
    __asm__(".section .drectve\n"                                       \
            ".ascii \" -export:" #name "=p_" #name ",@" #ord "\"\n"     \
            ".text\n");
EXPORTS(THUNK)

static void trace(const char *msg) {
    char path[MAX_PATH + 32];
    wsprintfA(path, "%s\\logs\\dsound_proxy.log", dir);
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, msg, lstrlenA(msg), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

// Runs at the game's entry point: put the original bytes back, load the fix, continue.
void load_fix(void) {
    DWORD old;
    VirtualProtect(entry, sizeof orig, PAGE_EXECUTE_READWRITE, &old);
    memcpy(entry, orig, sizeof orig);
    VirtualProtect(entry, sizeof orig, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof orig);

    // cnc-ddraw (menus) must use OpenGL when the 3D renderer is OpenGL, so both can share the
    // window; with Glide (dgVoodoo, Direct3D 11) it must not, or 3D stays black.
    char path[MAX_PATH + 32];
    const char *cmd = GetCommandLineA();
    int opengl = strstr(cmd, "-o2") || strstr(cmd, "-O2");
    wsprintfA(path, "%s\\ddraw.ini", dir);
    WritePrivateProfileStringA("Streets", "renderer", opengl ? "opengl" : "auto", path);

    wsprintfA(path, "%s\\streets_fix.dll", dir);
    trace("entry reached, loading streets_fix.dll");
    if (!LoadLibraryA(path))
        MessageBoxA(NULL, "streets_fix.dll could not be loaded; the game runs without fixes.",
                    "Streets of SimCity", MB_ICONWARNING);
}

void entry_stub(void) __attribute__((naked));
void entry_stub(void) { __asm__("pushal\n call _load_fix\n popal\n jmp *_entry"); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    char sys[MAX_PATH + 16];
    GetSystemDirectoryA(sys, MAX_PATH);  // SysWOW64 for this 32-bit process
    lstrcatA(sys, "\\dsound.dll");
    HMODULE sys_dll = LoadLibraryA(sys);
    if (!sys_dll) return FALSE;
    for (int i = 1; i < 13; i++) real[i] = GetProcAddress(sys_dll, MAKEINTRESOURCEA(i));

    char exe[MAX_PATH], *name;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    name = strrchr(exe, '\\');
    if (!name || lstrcmpiA(name + 1, "Streets.exe")) return TRUE;
    *name = 0;
    lstrcpyA(dir, exe);

    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    DWORD old;
    VirtualProtect(entry, sizeof orig, PAGE_EXECUTE_READWRITE, &old);
    memcpy(orig, entry, sizeof orig);
    entry[0] = 0xE9;  // jmp entry_stub
    *(DWORD *)(entry + 1) = (DWORD)((BYTE *)entry_stub - (entry + 5));
    VirtualProtect(entry, sizeof orig, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof orig);
    trace("attached, entry patched");
    return TRUE;
}
