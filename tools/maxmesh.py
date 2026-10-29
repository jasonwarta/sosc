"""Read Maxis composite mesh files (game/GEO/SIM3D*.MAX), the format documented in
https://github.com/CahootsMalone/maxis-mesh-stuff/blob/master/Info/Maxis-Mesh-Format.md

Each object (OBJX): 124-byte header (size-12, vertex count, face count, XZ radius, name,
ID), then vertices (3 x int32; the first is the origin), then FACE records:
tag, size, nverts u16, flags u16, light u16, unk u32, type u8, texture u8, atlas u8,
nverts x u16 vertex index, nverts x (u32 u, u32 v) in 16.16.

usage: python tools/maxmesh.py list [texture-index]      objects (that use the texture)
       python tools/maxmesh.py obj <file> <offset> <out.obj>   export one object
"""
import struct
import sys
import os

GEO = os.path.join(os.path.dirname(__file__), "..", "game", "GEO")
FILES = ["SIM3D1.MAX", "SIM3D2.MAX", "SIM3D3.MAX"]


def objects(data):
    count = struct.unpack_from("<I", data, 841)[0]
    table = struct.unpack_from("<I", data, 845)[0]
    for e in range(1, count + 1):
        addr = struct.unpack_from("<I", data, table + 53 * e + 17)[0]
        yield addr, parse(data, addr)


def parse(data, at):
    assert data[at:at + 4] == b"OBJX", hex(at)
    size, nv, nf = struct.unpack_from("<IHH", data, at + 4)
    radius = struct.unpack_from("<i", data, at + 16)[0]
    name = data[at + 24:at + 112].split(b"\0")[0].decode("latin1")
    oid = struct.unpack_from("<I", data, at + 120)[0]
    p = at + 124
    verts = [struct.unpack_from("<3i", data, p + 12 * i) for i in range(nv)]
    p += 12 * nv
    faces = []
    for _ in range(nf):
        assert data[p:p + 4] == b"FACE", hex(p)
        rsize, n, flags, light, unk, ftype, tex, atlas = struct.unpack_from("<IHHHIBBB", data, p + 4)
        idx = struct.unpack_from(f"<{n}H", data, p + 21)
        uv = struct.unpack_from(f"<{2 * n}i", data, p + 21 + 2 * n) if p + 21 + 2 * n + 8 * n <= p + rsize else ()
        faces.append(dict(n=n, flags=flags, light=light, unk=unk, type=ftype, tex=tex, atlas=atlas, idx=idx,
                          uv=uv, at=p, size=rsize))
        p += rsize
    return dict(name=name, id=oid, radius=radius, verts=verts, faces=faces, size=size, end=p)


def export_obj(obj, path):
    with open(path, "w") as f:
        for x, y, z in obj["verts"]:
            f.write(f"v {x / 262144:.4f} {y / 262144:.4f} {z / 262144:.4f}\n")
        vt = 1
        for fc in obj["faces"]:
            if fc["n"] < 3:
                continue
            if fc["uv"]:
                for i in range(fc["n"]):
                    f.write(f"vt {fc['uv'][2 * i] / 65536:.5f} {1 - fc['uv'][2 * i + 1] / 65536:.5f}\n")
                f.write("f " + " ".join(f"{i + 1}/{vt + k}" for k, i in enumerate(fc["idx"])) + "\n")
                vt += fc["n"]
            else:
                f.write("f " + " ".join(str(i + 1) for i in fc["idx"]) + "\n")


if __name__ == "__main__":
    if sys.argv[1] == "list":
        want = int(sys.argv[2]) if len(sys.argv) > 2 else None
        for fn in FILES:
            data = open(os.path.join(GEO, fn), "rb").read()
            for addr, o in objects(data):
                texs = sorted({fc["tex"] for fc in o["faces"] if fc["type"] in (13, 18)})
                if want is None or want in texs:
                    types = sorted({fc["type"] for fc in o["faces"]})
                    print(f"{fn} @{addr:#x} id {o['id']} '{o['name']}' {len(o['verts'])}v {len(o['faces'])}f "
                          f"r {o['radius']} types {types} tex {texs}")
    elif sys.argv[1] == "obj":
        data = open(os.path.join(GEO, sys.argv[2]), "rb").read()
        export_obj(parse(data, int(sys.argv[3], 0)), sys.argv[4])
