// Calls the glDrawPixels hook through its assembly thunk the way the game does (stdcall,
// image object in ebx) and checks that the arguments reach the real function intact and
// that control and the stack come back correctly.
#include <windows.h>
#include <stdio.h>
#include "../opengl.h"

static int got_w, got_h, calls;
static unsigned got_fmt, got_type;
static const void *got_px;

static void WINAPI fake_glDrawPixels(int w, int h, unsigned fmt, unsigned type, const void *px) {
    got_w = w, got_h = h, got_fmt = fmt, got_type = type, got_px = px;
    calls++;
}
static void WINAPI fake_void(void) {}

static void *thunk;

static void *fake_patch_iat(const char *dll, const char *func, void *hook) {
    (void)dll;
    if (!strcmp(func, "glDrawPixels")) {
        thunk = hook;
        return (void *)fake_glDrawPixels;
    }
    return (void *)fake_void;
}

int main(void) {
    if (!opengl_install(fake_patch_iat) || !thunk) {
        printf("FAIL: install\n");
        return 1;
    }
    static BYTE pixels[16];
    int esp_before, esp_after, ebx_out;
    // RGBA path (no conversion): arguments must pass straight through.
    __asm__ volatile(
        "movl %%esp, %0\n\t"
        "pushl %5\n\t"            // px
        "pushl $0x1401\n\t"       // type
        "pushl $0x1908\n\t"       // fmt = GL_RGBA
        "pushl $3\n\t"            // h
        "pushl $88\n\t"           // w
        "movl $0x12345678, %%ebx\n\t"
        "call *%3\n\t"
        "movl %%ebx, %2\n\t"
        "movl %%esp, %1\n\t"
        : "=m"(esp_before), "=m"(esp_after), "=m"(ebx_out)
        : "r"(thunk), "m"(thunk), "r"(pixels)
        : "eax", "ecx", "edx", "ebx", "memory");
    int ok = calls == 1 && got_w == 88 && got_h == 3 && got_fmt == 0x1908 && got_type == 0x1401 &&
             got_px == pixels && esp_before == esp_after && ebx_out == 0x12345678;
    printf("%s: calls=%d w=%d h=%d fmt=%04x type=%04x px_ok=%d esp_balanced=%d ebx_preserved=%d\n", ok ? "PASS" : "FAIL",
           calls, got_w, got_h, got_fmt, got_type, got_px == pixels, esp_before == esp_after, ebx_out == 0x12345678);
    return ok ? 0 : 1;
}
