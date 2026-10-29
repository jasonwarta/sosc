"""Launch a 32-bit program under a minimal debugger and log exceptions (WOW64-aware).

usage: python crashcatch.py <log> <exe> [args...]
       python crashcatch.py <log> --attach <process name>   (waits for it to start)
"""
import ctypes as C
import ctypes.wintypes as W
import os
import sys
import time

k32 = C.WinDLL("kernel32", use_last_error=True)

DEBUG_ONLY_THIS_PROCESS = 0x2
EXCEPTION_DEBUG_EVENT, CREATE_PROCESS_DEBUG_EVENT, EXIT_PROCESS_DEBUG_EVENT, LOAD_DLL_DEBUG_EVENT = 1, 3, 5, 6
DBG_CONTINUE, DBG_EXCEPTION_NOT_HANDLED = 0x00010002, 0x80010001
IGNORE = {0x80000003, 0x4000001F, 0x406D1388, 0x40010006}  # breakpoints, thread naming, debug string
CXX_EXC = 0xE06D7363


class STARTUPINFO(C.Structure):
    _fields_ = [("cb", W.DWORD), ("r", W.LPWSTR), ("d", W.LPWSTR), ("t", W.LPWSTR),
                ("x", W.DWORD * 8), ("c", W.WORD), ("c2", W.WORD), ("p", C.c_void_p),
                ("i", W.HANDLE), ("o", W.HANDLE), ("e", W.HANDLE)]


class PROCESS_INFORMATION(C.Structure):
    _fields_ = [("hProcess", W.HANDLE), ("hThread", W.HANDLE), ("pid", W.DWORD), ("tid", W.DWORD)]


class EXCEPTION_RECORD(C.Structure):
    _fields_ = [("code", W.DWORD), ("flags", W.DWORD), ("rec", C.c_void_p), ("addr", C.c_void_p),
                ("nparams", W.DWORD), ("info", C.c_size_t * 15)]


class DEBUG_EVENT(C.Structure):
    class U(C.Union):
        class Exc(C.Structure):
            _fields_ = [("rec", EXCEPTION_RECORD), ("first", W.DWORD)]

        class Load(C.Structure):
            _fields_ = [("hFile", W.HANDLE), ("base", C.c_void_p), ("x", W.DWORD * 2),
                        ("name", C.c_void_p), ("unicode", W.WORD)]

        class Create(C.Structure):
            _fields_ = [("hFile", W.HANDLE), ("hProc", W.HANDLE), ("hThread", W.HANDLE),
                        ("base", C.c_void_p)]

        _fields_ = [("exc", Exc), ("load", Load), ("create", Create), ("pad", C.c_byte * 160)]

    _fields_ = [("code", W.DWORD), ("pid", W.DWORD), ("tid", W.DWORD), ("u", U)]


class WOW64_CONTEXT(C.Structure):
    _fields_ = [("flags", W.DWORD), ("dr", W.DWORD * 6), ("fp", C.c_byte * 112),
                ("gs", W.DWORD), ("fs", W.DWORD), ("es", W.DWORD), ("ds", W.DWORD),
                ("edi", W.DWORD), ("esi", W.DWORD), ("ebx", W.DWORD), ("edx", W.DWORD),
                ("ecx", W.DWORD), ("eax", W.DWORD), ("ebp", W.DWORD), ("eip", W.DWORD),
                ("cs", W.DWORD), ("eflags", W.DWORD), ("esp", W.DWORD), ("ss", W.DWORD),
                ("ext", C.c_byte * 512)]


def main():
    log_path = sys.argv[1]
    log = open(log_path, "w", buffering=1)
    if sys.argv[2] == "--attach":
        pid = wait_for_process(sys.argv[3])
        if not k32.DebugActiveProcess(pid):
            sys.exit(f"DebugActiveProcess failed: {C.get_last_error()}")
        k32.DebugSetProcessKillOnExit(False)
        log.write(f"attached to pid {pid}\n")
        debug_loop(log, sys.argv[3])
        return
    exe, args = os.path.abspath(sys.argv[2]), sys.argv[3:]
    cmd = " ".join([f'"{exe}"'] + args)
    si, pi = STARTUPINFO(), PROCESS_INFORMATION()
    si.cb = C.sizeof(si)
    if not k32.CreateProcessW(None, C.create_unicode_buffer(cmd), None, None, False,
                              DEBUG_ONLY_THIS_PROCESS, None, os.path.dirname(exe), C.byref(si), C.byref(pi)):
        sys.exit(f"CreateProcess failed: {C.get_last_error()}")
    log.write(f"started pid {pi.pid}: {cmd}\n")
    debug_loop(log, os.path.basename(exe))


def wait_for_process(name):
    import subprocess
    for _ in range(600):
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout
        if name.lower() in out.lower():
            return int(out.strip().splitlines()[0].split('","')[1])
        time.sleep(0.1)
    sys.exit(f"{name} did not start")


def debug_loop(log, exe_name):
    modules = []  # (base, name)
    hproc = None
    t0 = time.time()
    ev = DEBUG_EVENT()
    while True:
        k32.WaitForDebugEvent(C.byref(ev), 0xFFFFFFFF)
        status = DBG_CONTINUE
        if ev.code == CREATE_PROCESS_DEBUG_EVENT:
            modules.append((ev.u.create.base or 0, exe_name))
            hproc = ev.u.create.hProc
        elif ev.code == LOAD_DLL_DEBUG_EVENT:
            buf = C.create_unicode_buffer(520)
            k32.GetFinalPathNameByHandleW(ev.u.load.hFile, buf, 520, 0)
            modules.append((ev.u.load.base or 0, os.path.basename(buf.value) or "?"))
        elif ev.code == EXCEPTION_DEBUG_EVENT:
            rec, first = ev.u.exc.rec, ev.u.exc.first
            if rec.code not in IGNORE:
                status = DBG_EXCEPTION_NOT_HANDLED
                if rec.code != CXX_EXC or not first:
                    dump(log, ev, rec, first, hproc, modules, time.time() - t0)
        elif ev.code == EXIT_PROCESS_DEBUG_EVENT:
            log.write(f"exit code {ev.u.pad[0] & 0xff:#x} after {time.time() - t0:.1f}s\n")
            break
        k32.ContinueDebugEvent(ev.pid, ev.tid, status)


def which_module(addr, modules):
    best = None
    for base, name in modules:
        if base <= addr and (best is None or base > best[0]):
            best = (base, name)
    return f"{best[1]}+{addr - best[0]:#x}" if best else "?"


def dump(log, ev, rec, first, hproc, modules, t):
    addr = rec.addr or 0
    info = [hex(rec.info[i]) for i in range(min(rec.nparams, 4))]
    log.write(f"\n[{t:.1f}s] {'first' if first else 'SECOND'}-chance exception {rec.code:#010x} "
              f"at {addr:#010x} ({which_module(addr, modules)}) tid {ev.tid} info {info}\n")
    h = k32.OpenThread(0x001FFFFF, False, ev.tid)
    ctx = WOW64_CONTEXT()
    ctx.flags = 0x10007  # CONTROL|INTEGER|SEGMENTS
    if k32.Wow64GetThreadContext(h, C.byref(ctx)):
        log.write(" ".join(f"{r}={getattr(ctx, r):08x}" for r in
                           ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip")) + "\n")
        stack = (W.DWORD * 64)()
        n = C.c_size_t()
        if k32.ReadProcessMemory(hproc, C.c_void_p(ctx.esp), stack, C.sizeof(stack), C.byref(n)):
            code_like = [f"{v:08x}" for v in stack if 0x401000 <= v < 0x5c3000]
            log.write("stack code ptrs: " + " ".join(code_like[:30]) + "\n")
    k32.CloseHandle(h)


if __name__ == "__main__":
    main()
