#!/usr/bin/env python3
"""exe_oracle.py -- runs one "pure" function of kenshi_x64.exe in a CPU emulator (Unicorn) and prints the result (#190, pilot oracle).

Needs: pefile, unicorn (venv: ~/.venvs/exeoracle). The exe is read from KENSHI_DIR (default /home/rdga1/rdga1prj/Kenshi_test); nothing of it is stored here.
Windows x64 ABI: ints rcx rdx r8 r9, floats xmm0-3, 32 bytes of shadow space, return in rax / xmm0. Memory the function reads outside the image is mapped
as zero pages on demand and reported ("zero-read"): such reads mean the function touches global state / objects that the caller must supply.

usage:  exe_oracle.py call <hexaddr> [--i a,b,..] [--f x,y,..] [--ret f32|f64|i64]   e.g.  exe_oracle.py call 140015b63 --f 0.25,10,20 --ret f32
        exe_oracle.py vectors                                                          runs the built-in vector sets, prints a TSV (name, inputs, exe result, reference, ok)
"""
import os, struct, sys, argparse
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_MEM_READ_UNMAPPED, UC_HOOK_MEM_WRITE_UNMAPPED, UC_HOOK_CODE, UcError
from unicorn.x86_const import *

EXE = os.path.join(os.environ.get("KENSHI_DIR", "/home/rdga1/rdga1prj/Kenshi_test"), "kenshi_x64.exe")
PAGE = 0x1000
STACK_TOP, STACK_SZ = 0x7ff000000000, 0x100000
HEAP = 0x10000000000          # fake objects supplied by the caller
SENTINEL = 0x7fe000000000     # return address: one `ret` lands here and stops the run


def f32(x): return struct.unpack("<I", struct.pack("<f", x))[0]
def f64(x): return struct.unpack("<Q", struct.pack("<d", x))[0]
def r32(b): return struct.unpack("<f", struct.pack("<I", b & 0xffffffff))[0]
def r64(b): return struct.unpack("<d", struct.pack("<Q", b & 0xffffffffffffffff))[0]


class Oracle:
    def __init__(self, path=EXE):
        pe = pefile.PE(path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        size = (pe.OPTIONAL_HEADER.SizeOfImage + PAGE - 1) & ~(PAGE - 1)
        self.uc.mem_map(self.base, size)
        self.uc.mem_write(self.base, pe.header[:pe.OPTIONAL_HEADER.SizeOfHeaders])
        for s in pe.sections:
            data = s.get_data()
            self.uc.mem_write(self.base + s.VirtualAddress, data[:s.Misc_VirtualSize] if s.Misc_VirtualSize else data)
        self.uc.mem_map(STACK_TOP - STACK_SZ, STACK_SZ)
        self.uc.mem_map(SENTINEL, PAGE)
        self.uc.mem_write(SENTINEL, b"\xc3")
        self.uc.mem_map(HEAP, 0x100000)
        self.zero_reads = []
        self.uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, self._fault)

    def _fault(self, uc, access, addr, size, value, ud):
        page = addr & ~(PAGE - 1)
        uc.mem_map(page, PAGE)
        self.zero_reads.append(addr)
        return True

    def run(self, addr, ints=(), floats=(), objs=None, max_insn=100000):
        """ints -> rcx,rdx,r8,r9 (positional slots 0..3); floats -> xmm0..3 as f32 (positional: a float in slot k goes to xmm k, so mix with ints by slot).
        floats given as ('d', x) are doubles."""
        u = self.uc
        self.zero_reads.clear()
        for r in (UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9):
            u.reg_write(r, 0)
        for i, r in enumerate((UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9)):
            if i < len(ints) and ints[i] is not None:
                u.reg_write(r, ints[i] & 0xffffffffffffffff)
        for i, x in enumerate(floats):
            if x is None: continue
            v = f64(x[1]) if isinstance(x, tuple) else f32(x)
            u.reg_write((UC_X86_REG_XMM0, UC_X86_REG_XMM1, UC_X86_REG_XMM2, UC_X86_REG_XMM3)[i], v)
        sp = STACK_TOP - 0x1000
        u.mem_write(sp, struct.pack("<Q", SENTINEL))
        u.reg_write(UC_X86_REG_RSP, sp)
        u.emu_start(addr, SENTINEL, count=max_insn)
        return u

    def xmm0_f32(self): return r32(self.uc.reg_read(UC_X86_REG_XMM0) & 0xffffffff)
    def xmm0_f64(self): return r64(self.uc.reg_read(UC_X86_REG_XMM0) & 0xffffffffffffffff)
    def rax(self): return self.uc.reg_read(UC_X86_REG_RAX)
    def write(self, addr, data): self.uc.mem_write(addr, data)
    def read(self, addr, n): return bytes(self.uc.mem_read(addr, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["call", "vectors"])
    ap.add_argument("addr", nargs="?")
    ap.add_argument("--i", default=""); ap.add_argument("--f", default=""); ap.add_argument("--ret", default="f32")
    a = ap.parse_args()
    o = Oracle()
    if a.cmd == "call":
        ints = [int(x, 0) for x in a.i.split(",") if x]
        flt = [float(x) for x in a.f.split(",") if x]
        o.run(int(a.addr, 16), ints, flt)
        print({"f32": o.xmm0_f32, "f64": o.xmm0_f64, "i64": o.rax}[a.ret]())
        if o.zero_reads: print("zero-read:", [hex(x) for x in o.zero_reads[:8]], file=sys.stderr)
    else:
        import exe_oracle_vectors
        exe_oracle_vectors.run(o)


if __name__ == "__main__":
    main()
