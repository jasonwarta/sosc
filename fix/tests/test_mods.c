// mods: against a scratch game folder (argv[1]) holding the original GEO\SIM3D1.MAX and
// SOUND\cow*.WAV, plus mods\moose: install, install again (no change), then remove. Compare
// argv[2] (the expected patched SIM3D1.MAX) after install and argv[3] (the original) after
// removal.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mods.h"

void logmsg(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
}

static int same_file(const char *a, const char *b) {
    FILE *f = fopen(a, "rb"), *g = fopen(b, "rb");
    int same = f && g;
    while (same) {
        int x = fgetc(f), y = fgetc(g);
        if (x != y) same = 0;
        if (x == EOF || y == EOF) break;
    }
    if (f) fclose(f);
    if (g) fclose(g);
    return same;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    char max[MAX_PATH];
    snprintf(max, sizeof max, "%s\\GEO\\SIM3D1.MAX", argv[1]);
    mods_install(argv[1], 1);
    int installed = same_file(max, argv[2]);
    mods_install(argv[1], 1);
    int still = same_file(max, argv[2]);
    mods_install(argv[1], 0);
    int removed = same_file(max, argv[3]);
    int pass = installed && still && removed;
    printf("%s: installed=%d again=%d removed=%d\n", pass ? "PASS" : "FAIL", installed, still, removed);
    return !pass;
}
