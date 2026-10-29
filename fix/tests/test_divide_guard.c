// Maps the game's fixed-point divide FUN_004ab0fb at its real address, calls it with an
// overflowing quotient from a fake car-speed call site and from elsewhere, and checks the
// guard returns 0 and a saturated value respectively instead of crashing.
#include "../streets_fix.c"

static const BYTE divide_fn[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x55, 0x0C, 0x8B, 0xCA, 0x0B, 0xC9,
                                 0x75, 0x02, 0xEB, 0x0A, 0x99, 0x0F, 0xA4, 0xC2, 0x10, 0xC1, 0xE0, 0x10, 0xF7, 0xF9, 0x5D, 0xC3};

static int call_from(BYTE *page, DWORD site, int a, int b) {
    // site: push b; push a; call 0x4ab0fb; add esp,8; ret
    (void)page;
    BYTE *p = (BYTE *)(UINT_PTR)(site - 5 - 5 - 5);
    BYTE *start = p;
    *p++ = 0x68; *(int *)p = b; p += 4;
    *p++ = 0x68; *(int *)p = a; p += 4;
    *p++ = 0xE8; *(int *)p = (int)(0x4ab0fb - (DWORD)(UINT_PTR)(p + 4)); p += 4;
    if ((DWORD)(UINT_PTR)p != site) { printf("FAIL: stub layout %p\n", p); exit(1); }
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08; *p++ = 0xC3;
    return ((int (*)(void))start)();
}

int main(void) {
    // the divide at 0x4ab0fb and the speed call site at 0x59e227 (reservations are 64 KB aligned)
    BYTE *ra = VirtualAlloc((void *)0x4a0000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    BYTE *rb = VirtualAlloc((void *)0x590000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (ra != (BYTE *)0x4a0000 || rb != (BYTE *)0x590000) { printf("FAIL: could not map pages (%lu)\n", GetLastError()); return 1; }
    BYTE *a = ra + 0xb000, *b = rb + 0xe000;
    memset(a, 0xCC, 0x1000);
    memcpy(a + 0xfb, divide_fn, sizeof divide_fn);
    memcpy(a + 0x131, "\xF7\xF9", 2);
    QueryPerformanceFrequency(&qpc_freq);
    if (!install_divide_guard()) { printf("FAIL: install\n"); return 1; }
    int ok_div = call_from(b, 0x59e227 + 0x100, 3 << 16, 2 << 16);     // normal: 3/2 = 1.5
    int speed = call_from(b, 0x59e227, 0x15990000, 0x20c);             // crash case -> 0
    int other = call_from(b, 0x59e227 + 0x200, 0x15990000, 0x20c);     // other caller -> saturate
    int neg = call_from(b, 0x59e227 + 0x300, -0x15990000, 0x20c);
    int pass = ok_div == 0x18000 && speed == 0 && other == 0x7fffffff && neg == (int)0x80000001;
    printf("%s: normal=%x speed=%x other=%x neg=%x faults=%ld\n", pass ? "PASS" : "FAIL", ok_div, speed, other, neg, divide_faults);
    return !pass;
}
