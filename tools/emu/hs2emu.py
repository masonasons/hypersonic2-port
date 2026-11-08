"""Run code from Hypersonic.dll in the Unicorn CPU emulator.

The DLL is mapped at its preferred base (0x10000000), so no relocation is needed. Imports are pointed at stub
addresses; `sprintf` is implemented in Python and any other import raises ImportCalled. Use this to call pure
helper functions (e.g. parameter display text) and get the exact original behaviour.
"""
import re
import struct

import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_FPCW,
                                UC_X86_REG_FS, UC_X86_REG_GDTR, UC_X86_REG_CS, UC_X86_REG_SS,
                                UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_GS)

STACK, STACK_SIZE = 0x00100000, 0x00100000
GDT, TEB = 0x00080000, 0x00090000
STUBS, STUB_SIZE = 0x00300000, 0x1000
SENTINEL = STUBS + 0x700
HEAP, HEAP_SIZE = 0x00400000, 0x04000000  # 64 MB: effects allocate delay lines
BIG_HEAP = 0x20000000  # heaps over 192 MB (up to 1.5 GB)
NATIVE_BASE = STUBS + 0x800  # real x86 replacements for CRT math (x87 in/out), not hooked

# double floor(double): cdecl, arg at [esp+4], result in ST0. Round-down via a temporary control word.
_FLOOR = bytes.fromhex(
    'DD442404'      # fld qword [esp+4]
    '83EC04'        # sub esp,4
    'D93C24'        # fnstcw [esp]
    '668B0424'      # mov ax,[esp]
    '6625FFF3'      # and ax,0xF3FF
    '660D0004'      # or ax,0x0400   (RC = round down)
    '6689442402'    # mov [esp+2],ax
    'D96C2402'      # fldcw [esp+2]
    'D9FC'          # frndint
    'D92C24'        # fldcw [esp]
    '83C404'        # add esp,4
    'C3')           # ret
# _CIfmod: fmod(ST1, ST0) -> ST0.  fxch; l: fprem; fnstsw ax; sahf; jp l; fstp st1; ret
_CIFMOD = bytes.fromhex('D9C9' 'D9F8' 'DFE0' '9E' '7AF9' 'DDD9' 'C3')
# _CIpow: pow(ST1, ST0) -> ST0 (x > 0).  fxch; fyl2x; fld st0; frndint; fxch; fsub st0,st1; f2xm1; fld1;
# faddp; fscale; fstp st1; ret
_CIPOW = bytes.fromhex('D9C9' 'D9F1' 'D9C0' 'D9FC' 'D9C9' 'D8E1' 'D9F0' 'D9E8' 'DEC1' 'D9FD' 'DDD9' 'C3')
NATIVE = {'floor': (0x00, _FLOOR), '_CIfmod': (0x40, _CIFMOD), '_CIpow': (0x60, _CIPOW)}


class ImportCalled(Exception):
    pass


class Emu:
    def __init__(self, dll_path, heap_size=HEAP_SIZE):
        pe = pefile.PE(dll_path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(self.base, size)
        uc.mem_write(self.base, pe.header)
        for s in pe.sections:
            uc.mem_write(self.base + s.VirtualAddress, s.get_data())
        uc.mem_map(STACK, STACK_SIZE)
        uc.mem_map(STUBS, STUB_SIZE)
        # the default heap sits below the DLL (0x10000000); a big one goes above it
        self.heap_base = HEAP if HEAP + heap_size <= self.base else BIG_HEAP
        uc.mem_map(self.heap_base, heap_size)
        self.heap_end = self.heap_base + heap_size
        # Flat 32-bit GDT plus FS -> a minimal TEB, so SEH prologues (mov eax, fs:[0]; mov fs:[0], esp) work.
        # fs:[0] = -1 (end of the exception chain). All segment registers must be reloaded once a GDT is set.
        uc.mem_map(GDT, 0x1000)
        uc.mem_map(TEB, 0x1000)
        uc.mem_write(TEB, struct.pack('<I', 0xFFFFFFFF))

        def desc(base, limit, access, flags):
            return struct.pack('<HHBBBB', limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF, access,
                               ((limit >> 16) & 0xF) | (flags << 4), (base >> 24) & 0xFF)
        gdt = bytes(8) + desc(0, 0xFFFFF, 0x9A, 0xC) + desc(0, 0xFFFFF, 0x92, 0xC) + desc(TEB, 0xFFF, 0x92, 0x4)
        uc.mem_write(GDT, gdt)
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        for reg, sel in ((UC_X86_REG_CS, 8), (UC_X86_REG_SS, 16), (UC_X86_REG_DS, 16), (UC_X86_REG_ES, 16),
                         (UC_X86_REG_GS, 16), (UC_X86_REG_FS, 24)):
            uc.reg_write(reg, sel)
        uc.mem_write(STUBS, b'\xc3' * STUB_SIZE)  # never actually executed: the hook redirects
        self.stub_names = {}
        n = 0
        for imp in pe.DIRECTORY_ENTRY_IMPORT:
            for f in imp.imports:
                name = f.name.decode() if f.name else f'{imp.dll.decode()}#{f.ordinal}'
                if name in NATIVE:
                    addr = NATIVE_BASE + NATIVE[name][0]
                else:
                    addr = STUBS + 0x10 + n * 4
                    self.stub_names[addr] = name
                    n += 1
                uc.mem_write(f.address, struct.pack('<I', addr))
        for off, code in NATIVE.values():
            uc.mem_write(NATIVE_BASE + off, code)
        uc.hook_add(UC_HOOK_CODE, self._stub_hook, begin=STUBS, end=NATIVE_BASE - 1)
        self.uc = uc
        self.heap_top = self.heap_base
        self.sizes = {}
        self.rand_state = 1
        # name -> fn(emu, esp) for imports not handled below. Returns EAX, or (EAX, bytes popped by a stdcall
        # callee). Used by part_oracle for file I/O.
        self.user_stubs = {}
        self.rand_log = None  # set to a list to record (return address, [esp+8]) of every rand() call
        self.code_hooks = {}

    def hook_function(self, addr, fn, ret_pop=0):
        """Replace the function at addr with fn(emu, esp) -> EAX (Python). ret_pop = bytes the callee pops
        (thiscall/stdcall args)."""
        def cb(uc, a, size, _):
            esp = uc.reg_read(UC_X86_REG_ESP)
            eax = fn(self, esp)
            uc.reg_write(UC_X86_REG_EAX, (eax or 0) & 0xffffffff)
            uc.reg_write(UC_X86_REG_EIP, self.u32(esp))
            uc.reg_write(UC_X86_REG_ESP, esp + 4 + ret_pop)
        self.code_hooks[addr] = self.uc.hook_add(UC_HOOK_CODE, cb, begin=addr, end=addr)

    # ---- memory helpers -------------------------------------------------------------------------
    def alloc(self, n, align=16):
        self.heap_top = (self.heap_top + align - 1) & ~(align - 1)
        a = self.heap_top
        self.heap_top += n
        if self.heap_top > self.heap_end:
            raise MemoryError(f'emulator heap exhausted ({self.heap_end - self.heap_base} bytes)')
        self.uc.mem_write(a, b'\0' * n)
        return a

    def reset_heap(self):
        self.heap_top = self.heap_base

    def u32(self, a):
        return struct.unpack('<I', self.uc.mem_read(a, 4))[0]

    def f32(self, a):
        return struct.unpack('<f', self.uc.mem_read(a, 4))[0]

    def f64(self, a):
        return struct.unpack('<d', self.uc.mem_read(a, 8))[0]

    def cstr(self, a, n=512):
        b = bytes(self.uc.mem_read(a, n))
        return b.split(b'\0')[0].decode('latin1')

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

    def wf32(self, a, v):
        self.uc.mem_write(a, struct.pack('<f', v))

    # ---- imports ----------------------------------------------------------------------------------
    def _stub_hook(self, uc, addr, size, _):
        if addr == SENTINEL:
            uc.emu_stop()
            return
        name = self.stub_names.get(addr, f'stub@{addr:#x}')
        esp = uc.reg_read(UC_X86_REG_ESP)
        a1, a2, a3 = (self.u32(esp + 4 * i) for i in (1, 2, 3))
        if name == 'sprintf':
            buf, fmt = a1, self.cstr(a2)
            s = self._format(fmt, esp + 12)
            uc.mem_write(buf, s.encode('latin1') + b'\0')
            uc.reg_write(UC_X86_REG_EAX, len(s))
        elif name in ('??2@YAPAXI@Z', '??_U@YAPAXI@Z', 'malloc'):     # operator new / new[] / malloc
            p = self.alloc(max(a1, 1))
            self.sizes[p] = a1
            uc.reg_write(UC_X86_REG_EAX, p)
        elif name == 'realloc':
            p = self.alloc(max(a2, 1))
            if a1:
                old = self.sizes.get(a1, 0)
                uc.mem_write(p, bytes(uc.mem_read(a1, min(old, a2))))
            self.sizes[p] = a2
            uc.reg_write(UC_X86_REG_EAX, p)
        elif name in ('??3@YAXPAX@Z', '??_V@YAXPAX@Z', 'free'):        # delete / delete[] / free: leak
            pass
        elif name == 'memmove':
            uc.mem_write(a1, bytes(uc.mem_read(a2, a3)))
            uc.reg_write(UC_X86_REG_EAX, a1)
        elif name == 'rand':
            self.rand_state = (self.rand_state * 214013 + 2531011) & 0xffffffff
            if self.rand_log is not None:
                self.rand_log.append((self.u32(esp), self.u32(esp + 8)))  # [esp+8]: caller of the push ecx; call rand wrappers
            uc.reg_write(UC_X86_REG_EAX, (self.rand_state >> 16) & 0x7fff)
        elif name in self.user_stubs:
            r = self.user_stubs[name](self, esp)
            pop = 0
            if isinstance(r, tuple):
                r, pop = r
            uc.reg_write(UC_X86_REG_EAX, (r or 0) & 0xffffffff)
            uc.reg_write(UC_X86_REG_EIP, self.u32(esp))
            uc.reg_write(UC_X86_REG_ESP, esp + 4 + pop)
            return
        else:
            self.import_error = name
            uc.emu_stop()
            return
        uc.reg_write(UC_X86_REG_EIP, self.u32(esp))   # cdecl return: caller cleans the args
        uc.reg_write(UC_X86_REG_ESP, esp + 4)

    def _format(self, fmt, argp):
        out, pos = [], 0
        for m in re.finditer(r'%([-+ #0]*)(\d*)(?:\.(\d+))?(hh|h|ll|l|L|I64)?([diouxXcsfeEgG%])', fmt):
            out.append(fmt[pos:m.start()])
            pos = m.end()
            flags, width, prec, _, conv = m.groups()
            spec = '%' + flags + width + (('.' + prec) if prec is not None else '')
            if conv == '%':
                out.append('%')
            elif conv in 'fFeEgG':
                out.append((spec + conv) % self.f64(argp))
                argp += 8
            elif conv == 's':
                out.append((spec + 's') % self.cstr(self.u32(argp)))
                argp += 4
            elif conv == 'c':
                out.append((spec + 'c') % chr(self.u32(argp) & 0xff))
                argp += 4
            else:
                v = self.u32(argp)
                if conv in 'di':
                    v = v - (1 << 32) if v & 0x80000000 else v
                out.append((spec + {'i': 'd', 'u': 'd'}.get(conv, conv)) % v)
                argp += 4
        out.append(fmt[pos:])
        return ''.join(out)

    # ---- calls ------------------------------------------------------------------------------------
    def call(self, func, args=(), ecx=0, max_insns=2_000_000):
        """Call func(args...) with ECX=ecx (thiscall/cdecl/stdcall all work). Returns EAX.
        Args: int = dword, float = float32 dword; a ('d', x) tuple pushes a double (8 bytes)."""
        uc = self.uc
        esp = STACK + STACK_SIZE - 0x1000
        for a in reversed(args):
            if isinstance(a, tuple) and a[0] == 'd':
                esp -= 8
                uc.mem_write(esp, struct.pack('<d', a[1]))
                continue
            esp -= 4
            if isinstance(a, float):
                uc.mem_write(esp, struct.pack('<f', a))
            else:
                uc.mem_write(esp, struct.pack('<I', a & 0xffffffff))
        esp -= 4
        uc.mem_write(esp, struct.pack('<I', SENTINEL))
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.reg_write(UC_X86_REG_ECX, ecx)
        uc.reg_write(UC_X86_REG_FPCW, 0x027F)  # MSVC default: 53-bit precision, round to nearest
        self.import_error = None
        uc.emu_start(func, SENTINEL, count=max_insns)
        if self.import_error:
            raise ImportCalled(self.import_error)
        if uc.reg_read(UC_X86_REG_EIP) != SENTINEL:
            raise RuntimeError(f'did not return (eip={uc.reg_read(UC_X86_REG_EIP):#x})')
        return uc.reg_read(UC_X86_REG_EAX)
