"""Build a low-poly bull moose to replace the cow (object 505 'Line01' in SIM3D1.MAX).

The moose keeps the cow's conventions: metres x 262144 units, feet on y=0, head towards -z,
x across; every face is type 13 (dedicated texture) with texture 168, the cow's slot in
sim3d.bmp. Parts are lofts of cross-sections (body, neck and head, legs, dewlap, antler beams),
thin solids (antler palms and points, ears) and a flat double-sided tail.

UVs: everything but the antlers is projected from the side (z -> u, y -> v) into the top
SIDE_V of the texture; the antlers are projected onto their palm plane into the band below.
A moose texture painted from the side therefore lands where it should; both flanks share it.

usage: python tools/make_moose.py preview <out-dir>   writes moose.obj and moose_uv.png (layout guide)
       python tools/make_moose.py install             puts the moose in game/GEO/SIM3D1.MAX in place
                                                      of the cow (original kept as SIM3D1.MAX.orig)
       python tools/make_moose.py patch <original SIM3D1.MAX> <out>   writes the patch the
                                                      fix DLL applies (used by tools/package.py)
"""
import math
import os
import shutil
import struct
import sys

import numpy as np

UNIT = 262144
SIDE_V = 0.72  # texture rows [0, SIDE_V) hold the side view; [SIDE_V + 0.02, 1] the antler palm

verts = []  # metres
faces = []  # (vertex indices, part)
centers = {}  # face number -> centre of its closed loft, to turn the face outward


def add_vert(p):
    verts.append(np.asarray(p, float))
    return len(verts) - 1


def ring(center, half_w, top, bottom, n=6, vertical=False, sideways=False):
    """Cross-section around center (x, y, z): half width across x, and the extent above and
    below (y) for a part running along z, or in front and behind (z) for a vertical one
    (legs). A sideways part (running along x, the antler beams) is half_w deep along z."""
    cx, cy, cz = center
    pts = []
    for k in range(n):
        a = 2 * math.pi * k / n
        x = math.sin(a) * half_w
        c = math.cos(a)
        c = c * (top if c > 0 else bottom)
        if sideways:
            pts.append(add_vert((cx, cy + c, cz + x)))
        else:
            pts.append(add_vert((cx + x, cy, cz - c) if vertical else (cx + x, cy + c, cz)))
    return pts


def loft(rings, part, cap_start=True, cap_end=True):
    """Join consecutive rings with quads. Each quad is turned outward about the centre of its
    own segment (the two rings it joins): on a long, bent part such as a leg the centre of the
    whole part can lie to the side of a face and turn it inward, and the game culls faces
    that face away, so the leg would vanish from some angles."""
    center = np.mean([verts[k] for r in rings for k in r], axis=0)
    n = len(rings[0])
    for a, b in zip(rings, rings[1:]):
        seg = np.mean([verts[k] for k in a + b], axis=0)
        for k in range(n):
            centers[len(faces)] = seg
            faces.append(([a[k], a[(k + 1) % n], b[(k + 1) % n], b[k]], part))
    for r, cap in ((rings[0], cap_start), (rings[-1], cap_end)):
        if cap:
            centers[len(faces)] = center
            faces.append((list(r), part))


def flat(points, part):
    """A flat piece seen from both sides."""
    idx = [add_vert(p) for p in points]
    faces.append((idx, part + "+"))
    faces.append((idx[::-1], part + "-"))


# Body: rump (+z) to chest (-z), the shoulder hump highest. (z, centre y, half width, top, bottom)
body = [(0.95, 1.30, 0.22, 0.20, 0.30), (0.80, 1.32, 0.34, 0.24, 0.40), (0.30, 1.30, 0.38, 0.26, 0.44),
        (-0.20, 1.38, 0.40, 0.38, 0.46), (-0.55, 1.42, 0.36, 0.40, 0.44), (-0.75, 1.40, 0.28, 0.30, 0.36)]
loft([ring((0, y, z), w, t, b) for z, y, w, t, b in body], "body")

# Neck and head: from the chest, forward and down to the long head and its big, blunt,
# overhanging muzzle (wider and deeper than the skull, not tapering to a point).
head = [(-0.50, 1.50, 0.26, 0.26, 0.34), (-0.78, 1.58, 0.24, 0.22, 0.30), (-1.00, 1.62, 0.18, 0.17, 0.22),
        (-1.22, 1.62, 0.15, 0.14, 0.17), (-1.42, 1.53, 0.14, 0.12, 0.18), (-1.60, 1.46, 0.16, 0.13, 0.21),
        (-1.72, 1.42, 0.15, 0.12, 0.20), (-1.78, 1.40, 0.10, 0.08, 0.14)]
loft([ring((0, y, z), w, t, b) for z, y, w, t, b in head], "head")

# Legs: four-sided lofts that start inside the body (so they join it with no gap), widen
# into the shoulder or thigh where they leave the body, then knee/hock, cannon and hoof.
for side in (-1, 1):
    for z, knee_dz in ((-0.55, -0.05), (0.72, 0.10)):
        x = side * 0.22
        segs = [((x * 0.8, 1.30, z), 0.12, 0.16), ((x, 0.98, z), 0.11, 0.15),
                ((x, 0.62, z + knee_dz), 0.06, 0.07), ((x, 0.10, z), 0.045, 0.05), ((x, 0.0, z - 0.03), 0.06, 0.06)]
        loft([ring(p, w, d, d, n=4, vertical=True) for p, w, d in segs], "legs", cap_start=False)

# Dewlap ("bell") under the throat, ears, tail.
# The dewlap (bell) is a hairy flap hanging from the throat: thick where it joins, tapering
# to a tuft, hanging a little back. A narrow cone, flattened side to side, starting inside the head.
loft([ring((0, 1.50, -1.24), 0.045, 0.08, 0.08, n=6, vertical=True),
      ring((0, 1.36, -1.22), 0.04, 0.065, 0.065, n=6, vertical=True),
      ring((0, 1.22, -1.19), 0.03, 0.04, 0.04, n=6, vertical=True),
      ring((0, 1.10, -1.16), 0.012, 0.015, 0.015, n=6, vertical=True)], "dewlap", cap_start=False)
flat([(-0.05, 1.42, 0.95), (0.05, 1.42, 0.95), (0, 1.15, 1.02)], "tail")

# Antlers. Each has a short thick beam straight out of the top of the skull, a small brow palm
# reaching forward with its points forward, and a big main palm (the "paddle") sweeping out and
# back behind it, past a notch (the bay). The palms are dished like hands held palm up: low
# where they leave the beam, rising to a rim of points. Spread about 1.5 m. Palms and points
# are solid, ANTLER_T thick, and both palms start at the end of the beam.
ANTLER_T = 0.05


def closed(face_list, part, points):
    """Faces of one closed, roughly round solid: wound outward later, about its centre."""
    center = np.mean([verts[k] for k in points], axis=0)
    for f in face_list:
        centers[len(faces)] = center
        faces.append((f, part))


def facing_away(face, opposite, part):
    """A face of a thin solid, wound outward later about the centre of the face across the
    solid from it. The centre of a thin, wide solid lies almost in the plane of its big faces,
    which makes it no use for telling their outside from their inside."""
    centers[len(faces)] = np.mean([verts[k] for k in opposite], axis=0)
    faces.append((face, part))


def slab(outline, center, part="antlers"):
    """A dished plate: the closed outline fanned to a centre point, as a top and a bottom
    surface ANTLER_T apart with side walls round the outline. Returns (top, bottom) outlines."""
    up = np.array([0, ANTLER_T / 2, 0])
    top = [add_vert(np.array(q) + up) for q in outline]
    bot = [add_vert(np.array(q) - up) for q in outline]
    ct, cb = add_vert(np.array(center) + up), add_vert(np.array(center) - up)
    n = len(outline)
    for i in range(n):
        j = (i + 1) % n
        facing_away([ct, top[i], top[j]], [cb, bot[i], bot[j]], part)
        facing_away([cb, bot[j], bot[i]], [ct, top[i], top[j]], part)
        facing_away([top[i], bot[i], bot[j], top[j]], [ct, cb], part)
    return top, bot


def point(top, bot, i, j, tip, part="antlers"):
    """A solid point (a four-sided pyramid) on the slab edge between outline corners i and j."""
    t = add_vert(tip)
    a, b, c, d = top[i], top[j], bot[j], bot[i]
    for face, opposite in (([a, b, t], [c, d, t]), ([c, d, t], [a, b, t]), ([b, c, t], [d, a, t]),
                           ([d, a, t], [b, c, t])):
        facing_away(face, opposite, part)


for s in (-1, 1):
    def p(x, y, z):
        return (s * x, y, z)

    # The beam starts at the middle of the skull, below its top, so it grows out of the head.
    beam_end = p(0.26, 1.83, -1.12)
    loft([ring(p(0.0, 1.68, -1.13), 0.05, 0.05, 0.05, n=6, sideways=True),
          ring(beam_end, 0.045, 0.045, 0.045, n=6, sideways=True)],
         "antlers", cap_start=False)
    # main palm: from the beam end out along the front edge, round the outer corner, along the
    # back and in again to the beam
    main = [beam_end, p(0.50, 1.94, -1.10), p(0.68, 2.02, -1.02), p(0.76, 2.06, -0.86),
            p(0.66, 2.02, -0.70), p(0.46, 1.94, -0.68), p(0.30, 1.86, -0.88)]
    top, bot = slab(main, p(0.44, 1.88, -0.92))
    centre = np.array(p(0.44, 1.90, -0.92))
    for i in range(1, 6):  # points along the outer and back rim, out and up
        a, b = np.array(main[i]), np.array(main[i + 1])
        out = (a + b) / 2 - centre
        out[1] = 0
        point(top, bot, i, i + 1, (a + b) / 2 + out / np.linalg.norm(out) * 0.15 + np.array([0, 0.12, 0]))
    # brow palm: forward and a little out from the beam end, two points facing forward
    brow = [beam_end, p(0.40, 1.86, -1.22), p(0.38, 1.90, -1.40), p(0.26, 1.86, -1.36)]
    top, bot = slab(brow, p(0.32, 1.86, -1.28))
    point(top, bot, 1, 2, p(0.48, 1.98, -1.48))
    point(top, bot, 2, 3, p(0.32, 1.98, -1.56))

    # Ear, behind and below the antler: a solid leaf, broad in the middle and 4 cm thick,
    # from inside the head out, up and back to a point.
    base = [add_vert(q) for q in (p(0.12, 1.74, -1.06), p(0.11, 1.80, -1.00), p(0.12, 1.74, -0.94),
                                  p(0.14, 1.68, -1.00))]
    mid = [add_vert(q) for q in (p(0.24, 1.80, -1.04), p(0.23, 1.83, -0.96), p(0.24, 1.80, -0.88),
                                 p(0.25, 1.77, -0.96))]
    tip = add_vert(p(0.36, 1.86, -0.90))
    for i in range(4):  # each face against the one across the ear from it
        o = (i + 2) % 4
        facing_away([base[i], base[(i + 1) % 4], mid[(i + 1) % 4], mid[i]],
                    [base[o], base[(o + 1) % 4], mid[(o + 1) % 4], mid[o]], "ears")
        facing_away([mid[i], mid[(i + 1) % 4], tip], [mid[o], mid[(o + 1) % 4], tip], "ears")


def uvs():
    """Per-face UVs (u, v in 0..1, v down from the top of the texture)."""
    V = np.array(verts)
    body_parts = [i for i, (_, p) in enumerate(faces) if not p.startswith("antlers")]
    used = sorted({k for i in body_parts for k in faces[i][0]})
    zmin, zmax = V[used, 2].min(), V[used, 2].max()
    ymax = V[used, 1].max()
    scale = min(1 / (zmax - zmin), SIDE_V / ymax)
    ant = sorted({k for f, p in faces if p.startswith("antlers") for k in f})
    ax = np.abs(V[ant, 0])
    out = []
    for f, p in faces:
        if p.startswith("antlers"):
            # palm plane, seen from above: |x| across, z down the band
            pts = [((abs(V[k, 0]) - ax.min()) / (ax.max() - ax.min()),
                    SIDE_V + 0.02 + (V[k, 2] - V[ant, 2].min()) / np.ptp(V[ant, 2]) * (1 - SIDE_V - 0.03))
                   for k in f]
        else:
            pts = [((V[k, 2] - zmin) * scale, (ymax - V[k, 1]) * scale) for k in f]
        out.append(pts)
    return out


def write_obj(path):
    with open(path, "w") as f:
        for v in verts:
            f.write(f"v {v[0]:.4f} {v[1]:.4f} {v[2]:.4f}\n")
        t = 1
        for (idx, _), uv in zip(faces, uvs()):
            for u, v in uv:
                f.write(f"vt {u:.5f} {1 - v:.5f}\n")
            f.write("f " + " ".join(f"{i + 1}/{t + k}" for k, i in enumerate(idx)) + "\n")
            t += len(idx)


def write_uv_guide(path, size=512):
    from PIL import Image, ImageDraw
    img = Image.new("RGB", (size, size), (255, 255, 255))
    d = ImageDraw.Draw(img)
    colors = {"body": (150, 110, 70), "head": (120, 90, 60), "legs": (90, 80, 70), "dewlap": (110, 80, 50),
              "ears": (130, 100, 70), "tail": (100, 70, 50), "antlers": (200, 180, 140)}
    order = sorted(range(len(faces)), key=lambda i: -np.mean([verts[k][0] for k in faces[i][0]]))
    all_uv = uvs()
    for i in order:
        part = faces[i][1].rstrip("+-")
        d.polygon([(u * size, v * size) for u, v in all_uv[i]], fill=colors[part], outline=(0, 0, 0))
    img.save(path)


def orient():
    """Loft faces wind so that (v1 - v0) x (v2 - v0) points outward, as the cow's do."""
    for i, c in centers.items():
        idx, part = faces[i]
        p = [verts[k] for k in idx]
        if np.cross(p[1] - p[0], p[2] - p[0]) @ (np.mean(p, axis=0) - c) < 0:
            faces[i] = (idx[::-1], part)


GEO = os.path.join(os.path.dirname(__file__), "..", "game", "GEO", "SIM3D1.MAX")
COW_ID, TEXTURE = 505, 168


def object_bytes(cow_header, origin):
    """The moose as an OBJX record, keeping the cow's header fields, origin and ID."""
    vs = [origin] + [tuple(int(round(c * UNIT)) for c in v) for v in verts]
    radius = int(max(math.hypot(x - origin[0], z - origin[2]) for x, _, z in vs[1:]))
    body = b"".join(struct.pack("<3i", *v) for v in vs)
    for n, ((idx, _), uv) in enumerate(zip(faces, uvs())):
        rec = struct.pack("<HHHIBBB", len(idx), 8194, 0, n, 13, TEXTURE, 0)
        rec += struct.pack(f"<{len(idx)}H", *[k + 1 for k in idx])
        rec += b"".join(struct.pack("<2i", int(u * 65536), int((1 - v) * 65536)) for u, v in uv)
        body += b"FACE" + struct.pack("<I", 8 + len(rec)) + rec
    head = bytearray(cow_header)
    name = b"moose".ljust(88, b"\0")
    head[24:112] = name
    struct.pack_into("<HHi", head, 8, len(vs), len(faces), 0)
    struct.pack_into("<i", head, 16, radius)
    obj = bytes(head) + body
    struct.pack_into("<I", head, 4, len(obj) - 12)
    return bytes(head) + body, len(vs), len(faces), sum(len(f) for f, _ in faces)


def moose_file(original):
    """SIM3D1.MAX with the moose in place of the cow. Returns (data, at, old_len, new_len)."""
    sys.path.insert(0, os.path.dirname(__file__))
    import maxmesh
    data = bytearray(original)
    count, table, dup = struct.unpack_from("<III", data, 841)
    entry = next(e for e in range(1, count + 1)
                 if struct.unpack_from("<I", data, dup + 36 * (e - 1))[0] == COW_ID)
    at = struct.unpack_from("<I", data, table + 53 * entry + 17)[0]
    cow = maxmesh.parse(bytes(data), at)
    new, nv, nf, rendered = object_bytes(data[at:at + 124], cow["verts"][0])
    old_len = cow["end"] - at
    delta = len(new) - old_len
    old_rendered = sum(f["n"] for f in cow["faces"])
    data[at:at + old_len] = new
    struct.pack_into("<I", data, 4, len(data))  # DIRC file size
    struct.pack_into("<I", data, 833, struct.unpack_from("<I", data, 833)[0] + delta)  # GEOM size
    for e in range(1, count + 1):  # object addresses after the moose move by delta
        a = struct.unpack_from("<I", data, table + 53 * e + 17)[0]
        if a > at:
            struct.pack_into("<I", data, table + 53 * e + 17, a + delta)
        d = dup + 36 * (e - 1)
        a = struct.unpack_from("<I", data, d + 4)[0]
        if a > at:
            struct.pack_into("<I", data, d + 4, a + delta)
    # per-object counts: rendered vertices, faces, unique vertices (offsets from the entry)
    for base, offs in ((table + 53 * entry, (29, 41, 45)), (dup + 36 * (entry - 1), (12, 24, 28))):
        for off, value in zip(offs, (rendered, nf, nv)):
            struct.pack_into("<I", data, base + off, value)
    for off, d in ((29, rendered - old_rendered), (41, nf - len(cow["faces"])), (45, nv - len(cow["verts"]))):
        struct.pack_into("<I", data, table + off, struct.unpack_from("<I", data, table + off)[0] + d)  # file totals
    check = maxmesh.parse(bytes(data), at)
    print(f"moose: {nv} vertices, {nf} faces, {len(new)} bytes ({delta:+d}), "
          f"radius {struct.unpack_from('<i', data, at + 16)[0]}, parsed back: '{check['name']}'")
    return bytes(data), at, old_len, len(new)


def install():
    orig = GEO + ".orig"
    if not os.path.exists(orig):
        shutil.copy(GEO, orig)
    data = moose_file(open(orig, "rb").read())[0]
    open(GEO, "wb").write(data)
    print("installed in", GEO)


def fnv64(b):
    h = 0xCBF29CE484222325
    for c in b:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def make_patch(original):
    """A patch the fix DLL applies on first launch (fix/mods.c), so the release carries only the
    moose, not the game's models. Format (little-endian):
      "SOSCPAT1", u32 original size, u64 FNV-1a of the original, u32 patched size,
      u64 FNV-1a of the patched file, u32 at, u32 old_len, u32 new_len, new_len bytes
      (the moose replacing original[at:at+old_len]), u32 fixup count, then per fixup
      u32 offset (in the patched file), u32 length, bytes (changed header and table fields)."""
    patched, at, old_len, new_len = moose_file(original)
    spliced = original[:at] + patched[at:at + new_len] + original[at + old_len:]
    assert len(spliced) == len(patched)
    fixups, i = [], 0
    while i < len(patched):
        if spliced[i] != patched[i]:
            j = i
            while j < len(patched) and (spliced[j] != patched[j] or j - i < 4):
                j += 1
            fixups.append((i, patched[i:j]))
            i = j
        else:
            i += 1
    body = b"SOSCPAT1" + struct.pack("<IQIQIII", len(original), fnv64(original), len(patched), fnv64(patched),
                                     at, old_len, new_len) + patched[at:at + new_len]
    body += struct.pack("<I", len(fixups)) + b"".join(struct.pack("<II", o, len(d)) + d for o, d in fixups)
    print(f"patch: {len(body)} bytes, {len(fixups)} fixups")
    return body


if __name__ == "__main__":
    orient()
    if sys.argv[1] == "preview":
        out = sys.argv[2]
        os.makedirs(out, exist_ok=True)
        write_obj(os.path.join(out, "moose.obj"))
        write_uv_guide(os.path.join(out, "moose_uv.png"))
        print(f"{len(verts)} vertices, {len(faces)} faces")
    elif sys.argv[1] == "install":
        install()
    elif sys.argv[1] == "patch":
        open(sys.argv[3], "wb").write(make_patch(open(sys.argv[2], "rb").read()))
