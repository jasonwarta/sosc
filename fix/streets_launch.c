// streets_launch.exe - starts Streets.exe with streets_fix.dll loaded before the game's code runs.
//
// The process is created suspended, its entry point is patched to a self-jump, and once the
// loader has finished (main thread spinning at the entry point) the DLL is loaded with a
// remote LoadLibraryA. Then the original entry bytes are restored and the game continues.
// Extra command-line arguments are passed through to Streets.exe.
#include <windows.h>
#include <stdio.h>

static void fail(const char *what) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s failed (error %lu)", what, GetLastError());
    MessageBoxA(NULL, buf, "streets_launch", MB_ICONERROR);
    ExitProcess(1);
}

int main(void) {
    char dir[MAX_PATH], exe[MAX_PATH + 16], dll[MAX_PATH + 32], *slash;
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    if ((slash = strrchr(dir, '\\'))) *slash = 0;
    snprintf(exe, sizeof exe, "%s\\Streets.exe", dir);
    snprintf(dll, sizeof dll, "%s\\streets_fix.dll", dir);

    // Pass through our own arguments (everything after argv[0]).
    const char *args = GetCommandLineA();
    if (*args == '"') { args = strchr(args + 1, '"'); args = args ? args + 1 : ""; }
    else { while (*args && *args != ' ') args++; }
    // renderer= in streets_fix.ini picks the game's 3D renderer (-o0 software, -o1 Glide,
    // -o2 OpenGL) unless an -o option was given on the command line.
    char ini[MAX_PATH + 32], renderer[32], opt[8] = "";
    snprintf(ini, sizeof ini, "%s\\streets_fix.ini", dir);
    GetPrivateProfileStringA("fix", "renderer", "", renderer, sizeof renderer, ini);
    if (!strstr(args, "-o") && !strstr(args, "-O")) {
        if (!lstrcmpiA(renderer, "software")) strcpy(opt, " -o0");
        else if (!lstrcmpiA(renderer, "glide")) strcpy(opt, " -o1");
        else if (!lstrcmpiA(renderer, "opengl")) strcpy(opt, " -o2");
    }
    char cmd[MAX_PATH + 1024];
    snprintf(cmd, sizeof cmd, "\"%s\"%s%s", exe, opt, args);

    // cnc-ddraw (menus) must use OpenGL when the game's 3D renderer is OpenGL, so both can
    // share the window; with Glide (dgVoodoo, Direct3D 11) it must not, or 3D stays black.
    char ddraw_ini[MAX_PATH + 32];
    snprintf(ddraw_ini, sizeof ddraw_ini, "%s\\ddraw.ini", dir);
    int opengl = strstr(cmd, "-o2") != NULL;
    WritePrivateProfileStringA("Streets", "renderer", opengl ? "opengl" : "auto", ddraw_ini);

    // Entry point from the PE header on disk (Streets.exe is not ASLR-enabled).
    HANDLE f = CreateFileA(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) fail("open Streets.exe");
    BYTE hdr[4096];
    DWORD got;
    ReadFile(f, hdr, sizeof hdr, &got, NULL);
    CloseHandle(f);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(hdr + ((IMAGE_DOS_HEADER *)hdr)->e_lfanew);
    DWORD entry = nt->OptionalHeader.ImageBase + nt->OptionalHeader.AddressOfEntryPoint;

    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi))
        fail("CreateProcess");

    BYTE orig[2], spin[2] = {0xEB, 0xFE};  // jmp $
    SIZE_T n;
    DWORD old;
    VirtualProtectEx(pi.hProcess, (void *)entry, 2, PAGE_EXECUTE_READWRITE, &old);
    if (!ReadProcessMemory(pi.hProcess, (void *)entry, orig, 2, &n)) fail("read entry");
    WriteProcessMemory(pi.hProcess, (void *)entry, spin, 2, &n);
    FlushInstructionCache(pi.hProcess, (void *)entry, 2);
    ResumeThread(pi.hThread);

    // Wait for the main thread to reach the entry point (loader done).
    CONTEXT ctx;
    for (int i = 0; i < 1000; i++) {
        Sleep(10);
        ctx.ContextFlags = CONTEXT_CONTROL;
        SuspendThread(pi.hThread);
        GetThreadContext(pi.hThread, &ctx);
        if (ctx.Eip == entry) break;
        ResumeThread(pi.hThread);
    }
    if (ctx.Eip != entry) { TerminateProcess(pi.hProcess, 1); fail("waiting for entry point"); }

    void *remote = VirtualAllocEx(pi.hProcess, NULL, sizeof dll, MEM_COMMIT, PAGE_READWRITE);
    WriteProcessMemory(pi.hProcess, remote, dll, strlen(dll) + 1, &n);
    HANDLE t = CreateRemoteThread(pi.hProcess, NULL, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"), remote, 0, NULL);
    if (!t) { TerminateProcess(pi.hProcess, 1); fail("CreateRemoteThread"); }
    WaitForSingleObject(t, INFINITE);
    DWORD loaded = 0;
    GetExitCodeThread(t, &loaded);
    CloseHandle(t);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    if (!loaded) { TerminateProcess(pi.hProcess, 1); fail("LoadLibrary(streets_fix.dll)"); }

    WriteProcessMemory(pi.hProcess, (void *)entry, orig, 2, &n);
    VirtualProtectEx(pi.hProcess, (void *)entry, 2, old, &old);
    FlushInstructionCache(pi.hProcess, (void *)entry, 2);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
