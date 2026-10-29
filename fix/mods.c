// Content mods shipped in the release as patches and loose files under mods\, installed into
// the game's own files at startup (before the game reads them) with the originals kept as
// *.orig. The release can't carry the game's files themselves, so the moose is a patch
// against the original GEO\SIM3D1.MAX (written by tools/make_moose.py patch).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "mods.h"

static unsigned long long fnv64(const BYTE *p, DWORD n) {
    unsigned long long h = 0xCBF29CE484222325ull;
    for (DWORD i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static BYTE *read_all(const char *path, DWORD *size) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(f, NULL), got = 0;
    BYTE *p = n != INVALID_FILE_SIZE ? malloc(n ? n : 1) : NULL;
    if (p && !(ReadFile(f, p, n, &got, NULL) && got == n)) free(p), p = NULL;
    CloseHandle(f);
    *size = n;
    return p;
}

static int write_all(const char *path, const BYTE *p, DWORD n) {
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);  // files copied off the CD are read-only
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    BOOL ok = WriteFile(f, p, n, &put, NULL) && put == n;
    CloseHandle(f);
    return ok;
}

// Keep the original as <path>.orig (once).
static int back_up(const char *path, const BYTE *p, DWORD n) {
    char orig[MAX_PATH + 8];
    snprintf(orig, sizeof orig, "%s.orig", path);
    if (GetFileAttributesA(orig) != INVALID_FILE_ATTRIBUTES) return 1;
    return write_all(orig, p, n);
}

// Put the original back from <path>.orig.
static int restore(const char *path) {
    char orig[MAX_PATH + 8];
    DWORD n;
    snprintf(orig, sizeof orig, "%s.orig", path);
    BYTE *p = read_all(orig, &n);
    int ok = p && write_all(path, p, n);
    free(p);
    return ok;
}

#pragma pack(push, 1)
typedef struct {
    char magic[8];  // SOSCPAT1
    DWORD orig_size;
    unsigned long long orig_hash;
    DWORD patched_size;
    unsigned long long patched_hash;
    DWORD at, old_len, new_len;
} patch_header;
#pragma pack(pop)

// Apply (on) or undo (!on) a patch to target. Returns a short status for the log.
static const char *apply_patch(const char *target, const char *patch_path, int on) {
    DWORD pn, tn;
    BYTE *patch = read_all(patch_path, &pn);
    if (!patch) return "no patch file";
    const patch_header *h = (const patch_header *)patch;
    if (pn < sizeof *h || memcmp(h->magic, "SOSCPAT1", 8) || sizeof *h + h->new_len + 4 > pn) {
        free(patch);
        return "bad patch file";
    }
    BYTE *t = read_all(target, &tn);
    if (!t) {
        free(patch);
        return "game file missing";
    }
    unsigned long long th = fnv64(t, tn);
    int is_patched = tn == h->patched_size && th == h->patched_hash;
    int is_orig = tn == h->orig_size && th == h->orig_hash;
    const char *status;
    if (!on) {
        status = !is_patched ? "off" : restore(target) ? "removed (original restored)" : "FAILED to restore the original";
    } else if (is_patched) {
        status = "on";
    } else if (!is_orig || h->at + h->old_len > tn) {
        status = "skipped: the game file isn't the version the patch was made for";
    } else {
        BYTE *out = malloc(h->patched_size);
        const BYTE *p = patch + sizeof *h;
        DWORD tail = tn - h->at - h->old_len;
        int ok = out && h->at + h->new_len + tail == h->patched_size;
        if (ok) {
            memcpy(out, t, h->at);
            memcpy(out + h->at, p, h->new_len);
            memcpy(out + h->at + h->new_len, t + h->at + h->old_len, tail);
            p += h->new_len;
            DWORD fixups = *(const DWORD *)p;
            p += 4;
            for (DWORD i = 0; ok && i < fixups; i++) {
                if (p + 8 > patch + pn) { ok = 0; break; }
                DWORD off = ((const DWORD *)p)[0], len = ((const DWORD *)p)[1];
                p += 8;
                if (p + len > patch + pn || off + len > h->patched_size) { ok = 0; break; }
                memcpy(out + off, p, len);
                p += len;
            }
            ok = ok && fnv64(out, h->patched_size) == h->patched_hash;
        }
        status = !ok                                   ? "FAILED: patch result doesn't check out"
               : !back_up(target, t, tn)               ? "FAILED: couldn't back up the original"
               : !write_all(target, out, h->patched_size) ? "FAILED to write"
                                                        : "installed";
        free(out);
    }
    free(t);
    free(patch);
    return status;
}

// Copy (on) or undo (!on) a replacement file.
static const char *apply_file(const char *target, const char *src_path, int on) {
    DWORD sn, tn;
    BYTE *src = read_all(src_path, &sn);
    if (!src) return "no mod file";
    BYTE *t = read_all(target, &tn);
    int same = t && tn == sn && memcmp(t, src, sn) == 0;
    const char *status = !on   ? (!same ? "off" : restore(target) ? "removed (original restored)" : "FAILED to restore the original")
                       : same  ? "on"
                       : t && !back_up(target, t, tn) ? "FAILED: couldn't back up the original"
                       : write_all(target, src, sn)    ? "installed"
                                                       : "FAILED to write";
    free(t);
    free(src);
    return status;
}

void mods_install(const char *game_dir, int moose) {
    static const char *const sounds[] = {"cowhit.WAV", "cowland.WAV"};
    char target[MAX_PATH + 64], src[MAX_PATH + 64];
    snprintf(src, sizeof src, "%s\\mods\\moose", game_dir);
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
        logmsg("mods: no mods\\moose folder, moose not available\n");
        return;
    }
    snprintf(target, sizeof target, "%s\\GEO\\SIM3D1.MAX", game_dir);
    snprintf(src, sizeof src, "%s\\mods\\moose\\SIM3D1.MAX.patch", game_dir);
    logmsg("mods: moose model (GEO\\SIM3D1.MAX): %s\n", apply_patch(target, src, moose));
    for (int i = 0; i < 2; i++) {
        snprintf(target, sizeof target, "%s\\SOUND\\%s", game_dir, sounds[i]);
        snprintf(src, sizeof src, "%s\\mods\\moose\\%s", game_dir, sounds[i]);
        logmsg("mods: moose sound %s: %s\n", sounds[i], apply_file(target, src, moose));
    }
}
