// Session logging for streets_fix.dll.
//
// One file per game session under logs\, written through a single append-only handle so
// our lines, the game's own stderr output and lines from other threads never overwrite
// each other. Faults are logged first-chance from a vectored handler, because the game
// catches most crashes itself and turns them into an "unrecoverable error" message box.
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_KEEP 20

static HANDLE log_handle = INVALID_HANDLE_VALUE;
static DWORD log_start;
static char log_file[MAX_PATH];

const char *log_path(void) { return log_file; }

static void write_raw(const char *s, int len) {
    DWORD written;
    if (log_handle != INVALID_HANDLE_VALUE) WriteFile(log_handle, s, len, &written, NULL);
}

void logmsg(const char *fmt, ...) {
    char buf[2048];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d [%8.3f] ", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, (GetTickCount() - log_start) / 1000.0);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    write_raw(buf, n);
}

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

// Delete all but the newest LOG_KEEP-1 session logs (names sort chronologically).
static void prune_old_logs(const char *dir) {
    char pattern[MAX_PATH], names[256][64];
    int count = 0;
    snprintf(pattern, sizeof pattern, "%s\\session_*.log", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (count < 256 && strlen(fd.cFileName) < 64) strcpy(names[count++], fd.cFileName);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    qsort(names, count, sizeof names[0], cmp_names);
    for (int i = 0; i < count - (LOG_KEEP - 1); i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", dir, names[i]);
        DeleteFileA(path);
    }
}

#define CXX_EXCEPTION 0xE06D7363  // MSVC C++ throw

// Exception codes that are routine and not worth a log entry.
static int is_noise(DWORD code) {
    switch (code) {
    case 0x40010006:  // OutputDebugStringA
    case 0x4001000A:  // OutputDebugStringW
    case 0x406D1388:  // thread naming
    case 0x80000003:  // breakpoint
    case 0x4000001F:  // WOW64 breakpoint
        return 1;
    }
    return 0;
}

// Allow the first few occurrences of each exception code, so a repeated harmless one (the
// game's fixed-point divide overflows) can't crowd out the one that matters.
static int under_limit(DWORD code) {
    static struct { DWORD code; LONG count; } seen[32];
    for (int i = 0; i < 32; i++) {
        if (seen[i].code == code) return InterlockedIncrement(&seen[i].count) <= 5;
        if (seen[i].code == 0 && InterlockedCompareExchange((LONG *)&seen[i].code, (LONG)code, 0) == 0)
            return InterlockedIncrement(&seen[i].count) <= 5;
    }
    return 0;
}

// Type name of a thrown C++ object, from the throw info MSVC passes with the exception
// (ThrowInfo -> CatchableTypeArray -> first CatchableType -> TypeDescriptor name).
static const char *cxx_type_name(EXCEPTION_RECORD *er) {
    if (er->NumberParameters < 3) return "?";
    DWORD *throw_info = (DWORD *)er->ExceptionInformation[2];
    if (!throw_info || IsBadReadPtr(throw_info, 16)) return "?";
    DWORD *types = (DWORD *)throw_info[3];
    if (!types || IsBadReadPtr(types, 8) || types[0] == 0) return "?";
    DWORD *catchable = (DWORD *)types[1];
    if (!catchable || IsBadReadPtr(catchable, 8)) return "?";
    char *descriptor = (char *)catchable[1];
    if (!descriptor || IsBadReadPtr(descriptor, 9)) return "?";
    return descriptor + 8;  // skip vftable pointer and spare field
}

static LONG CALLBACK fault_logger(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (is_noise(code) || !under_limit(code)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    ULONG_PTR *info = ep->ExceptionRecord->ExceptionInformation;
    if (code == CXX_EXCEPTION)
        logmsg("C++ exception thrown: type %s, object %08lx\n", cxx_type_name(ep->ExceptionRecord),
               ep->ExceptionRecord->NumberParameters > 1 ? (unsigned long)info[1] : 0UL);
    HMODULE mod = NULL;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(UINT_PTR)c->Eip, &mod)) {
        GetModuleFileNameA(mod, modname, sizeof modname);
    }
    const char *base = strrchr(modname, '\\');
    logmsg("EXCEPTION %08lx at %08lx (%s+%lx, thread %lu)%s%s %08lx\n", code, c->Eip, base ? base + 1 : modname,
           c->Eip - (DWORD)(UINT_PTR)mod, GetCurrentThreadId(),
           code == EXCEPTION_ACCESS_VIOLATION ? " " : "",
           code == EXCEPTION_ACCESS_VIOLATION ? (info[0] == 8 ? "execute" : info[0] ? "write" : "read") : "",
           code == EXCEPTION_ACCESS_VIOLATION ? (unsigned long)info[1] : 0UL);
    logmsg("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    // Return addresses into Streets.exe found on the stack (a rough call stack).
    char line[600];
    int n = snprintf(line, sizeof line, "  stack:");
    DWORD *sp = (DWORD *)c->Esp;
    for (int i = 0, found = 0; i < 512 && found < 32; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (v >= 0x00401000 && v < 0x005c3000) {
            n += snprintf(line + n, sizeof line - n, " %08lx", v);
            found++;
        }
    }
    logmsg("%s\n", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

void log_init(const char *game_dir) {
    char dir[MAX_PATH];
    SYSTEMTIME t;
    log_start = GetTickCount();
    GetLocalTime(&t);
    snprintf(dir, sizeof dir, "%s\\logs", game_dir);
    CreateDirectoryA(dir, NULL);
    prune_old_logs(dir);
    snprintf(log_file, sizeof log_file, "%s\\session_%04d%02d%02d_%02d%02d%02d.log", dir, t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    log_handle = CreateFileA(log_file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    // The game's C runtime hasn't started yet; it will pick these up as its stdout/stderr,
    // so messages like "Length exception occurred" (printed right before exit) land here.
    if (log_handle != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, log_handle);
        SetStdHandle(STD_ERROR_HANDLE, log_handle);
    }
    AddVectoredExceptionHandler(1, fault_logger);
}
