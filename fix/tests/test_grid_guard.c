// Maps the caller's `call FUN_004b3b00` at 0x4ab91d and a stand-in for FUN_004b3b00 (returns
// 7), installs the grid guard, and checks that in-range polygons reach the function, that a
// polygon with an absurd vertex is skipped with 0, and that the stack stays balanced.
// Link with -Wl,--image-base=0x20000000 so the game's addresses are free.
#include <stdio.h>
#include <windows.h>
#include "../widescreen.h"

static int run_call(int *poly, int flag) {
    // stub at 0x4ab90f: push flag; push poly; nops; (0x4ab91d) call; add esp,8; ret
    BYTE *p = (BYTE *)0x4ab90f;
    *p++ = 0x68; *(int *)p = flag; p += 4;
    *p++ = 0x68; *(int *)p = (int)(UINT_PTR)poly; p += 4;
    // p is now 0x4ab919: four bytes of nop up to the call site
    *p++ = 0x90; *p++ = 0x90; *p++ = 0x90; *p++ = 0x90;
    BYTE *after = (BYTE *)0x4ab922;
    after[0] = 0x83; after[1] = 0xC4; after[2] = 0x08; after[3] = 0xC3;
    int esp_before, esp_after, r;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp_before));
    r = ((int (*)(void))0x4ab90f)();
    __asm__ volatile("mov %%esp, %0" : "=r"(esp_after));
    if (esp_before != esp_after) { printf("FAIL: esp %x -> %x\n", esp_before, esp_after); exit(1); }
    return r;
}

extern int ws_edge_clamps;

// Run the (patched) 7-byte store at `site` with value and index registers set, then a ret
// placed after it; check every other register survives.
static int run_store(DWORD site, int value_is_edx, int value, int index) {
    BYTE *after = (BYTE *)site + 7;
    after[0] = 0xC3;
    int ecx = value_is_edx ? index : value, edx = value_is_edx ? value : index;
    int out_eax, out_ebx, out_ecx, out_edx, out_esi, out_edi;
    __asm__ volatile("push %%ebp\n"
                     "mov $0x11111111, %%eax\n mov $0x22222222, %%ebx\n mov $0x33333333, %%esi\n"
                     "mov $0x44444444, %%edi\n"
                     "call *%[fn]\n"
                     "pop %%ebp\n"
                     : "=a"(out_eax), "=b"(out_ebx), "=S"(out_esi), "=D"(out_edi), "+c"(ecx), "+d"(edx)
                     : [fn] "m"(site)
                     : "memory", "cc");
    out_ecx = ecx, out_edx = edx;
    int ok = out_eax == 0x11111111 && out_ebx == 0x22222222 && out_esi == 0x33333333 && out_edi == 0x44444444 &&
             out_ecx == (value_is_edx ? index : value) && out_edx == (value_is_edx ? value : index);
    if (!ok) { printf("FAIL: registers changed by store at %lx\n", site); exit(1); }
    return 1;
}

int main(void) {
    BYTE *ra = VirtualAlloc((void *)0x4a0000, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    BYTE *rd = VirtualAlloc((void *)0x9b0000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    BYTE *re2 = VirtualAlloc((void *)0x9c0000, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (ra != (BYTE *)0x4a0000 || rd != (BYTE *)0x9b0000 || re2 != (BYTE *)0x9c0000) {
        MEMORY_BASIC_INFORMATION mi;
        VirtualQuery((void *)0x9b0000, &mi, sizeof mi);
        printf("FAIL: could not map pages %p %p %p (9b0000: state %lx base %p size %lx)\n", ra, rd, re2, mi.State,
               mi.AllocationBase, (unsigned long)mi.RegionSize);
        return 1;
    }
    memset(ra, 0xCC, 0x20000);
    static const struct { DWORD site; BYTE b[7]; } stores[] = {
        {0x004b3f0e, {0x89, 0x14, 0x8D, 0xF8, 0x10, 0x9C, 0x00}}, {0x004b3fd8, {0x89, 0x0C, 0x95, 0xF8, 0x10, 0x9C, 0x00}},
        {0x004b40fd, {0x89, 0x14, 0x8D, 0x08, 0xD5, 0x9B, 0x00}}, {0x004b41d0, {0x89, 0x0C, 0x95, 0x08, 0xD5, 0x9B, 0x00}},
    };
    for (int i = 0; i < 4; i++) memcpy((void *)stores[i].site, stores[i].b, 7);
    BYTE *call = (BYTE *)0x4ab91d;
    call[0] = 0xE8;
    *(int *)(call + 1) = 0x4b3b00 - (0x4ab91d + 5);
    memcpy((void *)0x4b3b00, "\xB8\x07\x00\x00\x00\xC3", 6);  // mov eax, 7; ret
    ws_canvas_w = 1144;
    if (!grid_guard_install()) { printf("FAIL: install\n"); return 1; }

    int good_v[15] = {0, 0, 0, 0, 0, (1151 << 12), 487 << 12, 0, 0, 0, -(7 << 12), 100 << 12, 0, 0, 0};
    int good[2] = {3, (int)(UINT_PTR)good_v};
    int bad_v[15] = {10 << 12, 10 << 12, 0, 0, 0, 0x4cfa << 15, 20 << 12, 0, 0, 0, 30 << 12, 30 << 12, 0, 0, 0};
    int bad[2] = {3, (int)(UINT_PTR)bad_v};
    int neg_v[10] = {-(8 << 12), 10 << 12, 0, 0, 0, 10 << 12, 10 << 12, 0, 0, 0};
    int neg[2] = {2, (int)(UINT_PTR)neg_v};
    int edge_v[5] = {1152 << 12, 0, 0, 0, 0};
    int edge[2] = {1, (int)(UINT_PTR)edge_v};

    int rg = run_call(good, 1), rb = run_call(bad, 1), rn = run_call(neg, 0), re = run_call(edge, 0);
    // edge stores: polygon x range 100..200 px
    *(int *)0x9c10e4 = 100 << 12;
    *(int *)0x9c10d4 = 200 << 12;
    int *left = (int *)0x9c10f8, *right = (int *)0x9bd508;
    run_store(0x004b3f0e, 1, 150 << 12, 3);      // inside: stored as is
    run_store(0x004b3fd8, 0, -(90000 << 12), 4);  // far left: clamped to min
    run_store(0x004b40fd, 1, 157000 << 12, 5);    // far right: clamped to max
    run_store(0x004b41d0, 0, 120 << 12, 6);
    int stores_ok = left[3] == 150 << 12 && left[4] == 100 << 12 && right[5] == 200 << 12 && right[6] == 120 << 12 &&
                    ws_edge_clamps == 2;

    int pass = rg == 7 && rb == 0 && rn == 0 && re == 0 && stores_ok;
    printf("%s: good=%d bad=%d negative=%d past_edge=%d edge_stores=%d clamps=%d\n", pass ? "PASS" : "FAIL", rg, rb,
           rn, re, stores_ok, ws_edge_clamps);
    return !pass;
}
