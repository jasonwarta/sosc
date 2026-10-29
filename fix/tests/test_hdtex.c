// hdtex: index a directory and decode a PNG whose top row is red, bottom row blue. The dump
// writes the game's row 0 as the PNG's bottom row, so row 0 of the result must be blue.
// Then the background preload: after it finishes, a load takes the preloaded pixels, and a
// second load of the same key (already handed out) decodes again.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../hdtex.h"

static int check(unsigned long *px, int w, int h) {
    return px && w == 4 && h == 2 && px[0] == 0xFFFF0000ul && px[4] == 0xFF0000FFul;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int n = hdtex_init(argv[1]);
    int w = 0, h = 0;
    unsigned long *px = hdtex_load(0xab, &w, &h);
    int pass = n == 1 && hdtex_has(0xab) && !hdtex_has(0xac) && check(px, w, h);
    printf("%s: direct n=%d w=%d h=%d row0=%08lx row1=%08lx\n", pass ? "PASS" : "FAIL", n, w, h, px ? px[0] : 0,
           px ? px[4] : 0);
    free(px);

    hdtex_preload_start();
    Sleep(500);  // one tiny PNG: plenty
    int w2 = 0, h2 = 0, w3 = 0, h3 = 0;
    unsigned long *pre = hdtex_load(0xab, &w2, &h2);
    unsigned long *again = hdtex_load(0xab, &w3, &h3);
    int pass2 = check(pre, w2, h2) && check(again, w3, h3) && pre != again;
    printf("%s: preloaded then decoded again\n", pass2 ? "PASS" : "FAIL");
    free(pre), free(again);
    return !(pass && pass2);
}
