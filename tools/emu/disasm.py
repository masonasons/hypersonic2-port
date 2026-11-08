"""Disassemble a function (or range) of Hypersonic.dll with float constants resolved.

Usage: python -I disasm.py <Hypersonic.dll> <address hex> [length bytes, default 400]
Each FPU instruction that loads a constant from the DLL gets "; = value". Useful for constants the decompiler hides,
e.g. the multiplier before `call 0x1008e09c` (_ftol, a truncating float->int).
"""
import re
import struct
import sys

import capstone
import pefile


def main():
    dll, addr = sys.argv[1], int(sys.argv[2], 16)
    length = int(sys.argv[3], 0) if len(sys.argv) > 3 else 400
    pe = pefile.PE(dll, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for x in md.disasm(pe.get_data(addr - base, length), addr):
        s = f'{x.address:x}  {x.mnemonic} {x.op_str}'
        m = re.search(r'(dword|qword) ptr \[(0x[0-9a-f]+)\]', x.op_str)
        if m and x.mnemonic.startswith('f'):
            try:
                raw = pe.get_data(int(m.group(2), 16) - base, 8)
                v = struct.unpack('<d' if m.group(1) == 'qword' else '<f', raw[:8 if m.group(1) == 'qword' else 4])[0]
                s += f'    ; = {v!r}'
            except Exception:
                pass
        if x.op_str == '0x1008e09c':
            s += '    ; _ftol (truncate)'
        print(s)
        if x.mnemonic == 'ret' and length <= 400:
            pass


if __name__ == '__main__':
    main()
