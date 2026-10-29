"""List every instruction that references an address range (memory displacement or immediate).

usage: python tools/xref_range.py <lo_hex> <hi_hex> [<lo_hex> <hi_hex> ...]

Disassembles each function Ghidra found (the "// ==== FUN_xxx @ addr" markers in
analysis/streets.c) from its start to the next function's start, so data between
functions doesn't desync the decoder the way a linear sweep of .text does.
"""
import re
import sys

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

ROOT = __file__.rsplit("tools", 1)[0]


def main():
    ranges = [(int(sys.argv[i], 16), int(sys.argv[i + 1], 16)) for i in range(1, len(sys.argv), 2)]
    starts = sorted({int(m, 16) for m in re.findall(r"^// ==== FUN_([0-9a-f]+) @", open(
        ROOT + "analysis/streets.c", encoding="utf-8", errors="replace").read(), re.M)})
    pe = pefile.PE(ROOT + "game/Streets.exe", fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    for fi, start in enumerate(starts):
        end = starts[fi + 1] if fi + 1 < len(starts) else start + 0x1000
        for ins in md.disasm(image[start - base:end - base], start):
            for op in ins.operands:
                v = op.mem.disp & 0xffffffff if op.type == X86_OP_MEM else (
                    op.imm & 0xffffffff if op.type == X86_OP_IMM else None)
                if v is None:
                    continue
                for lo, hi in ranges:
                    if lo <= v < hi:
                        kind = "mem" if op.type == X86_OP_MEM else "imm"
                        print(f"{ins.address:08x} FUN_{start:08x} {kind} {v:08x}  {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()
