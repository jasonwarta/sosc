"""usage: python disasm.py <exe> <start_va_hex> <end_va_hex>"""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
pe = pefile.PE(sys.argv[1], fast_load=True)
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()[start - base:end - base]
for i in Cs(CS_ARCH_X86, CS_MODE_32).disasm(data, start):
    print(f"{i.address:08x}  {i.mnemonic:6} {i.op_str}")
