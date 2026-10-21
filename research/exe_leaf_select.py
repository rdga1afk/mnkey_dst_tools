#!/usr/bin/env python3
"""exe_leaf_select.py -- automatic selection of "leaf" functions of kenshi_x64.exe that the emulator oracle (exe_oracle.py, #190) can run (no API, no GPU).

For every function of re/re_/ghidra_projects/kenshi/fn_graph.tsv (addr, size, name, -, callees, strings) the machine code is disassembled (capstone) and classified:
  L0  no call at all and no indirect jump/call (a true leaf);          L1  only direct calls to L0 functions (or to tiny thunks of L0);
  X   anything else: indirect call (virtual / function pointer), call to a non-leaf, write to a .data global, TLS access (gs:/fs:), jump table.
Extra columns: n_insn, n_fp (SSE arithmetic), n_int (integer arithmetic), n_rdata (rip-relative reads of .rdata = constants), n_data_r (reads of .data globals = state).
"candidate" = L0/L1, n_fp+n_int >= 2, no .data write, no TLS. A smoke run (--smoke N) executes N random candidates in Unicorn with zero/random objects and reports how many finish cleanly.
Output: TSV (sorted by address) to stdout or --out. Needs: pefile, capstone, unicorn (venv ~/.venvs/exeoracle). The exe is read from KENSHI_DIR.
usage: exe_leaf_select.py [--out docs/research/exe_leaf_candidates.tsv] [--smoke 300] [--max-size 1500]
"""
import argparse, os, struct, random, sys, collections
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM, CS_OP_REG, CS_AC_WRITE
from capstone.x86 import X86_REG_RIP, X86_REG_INVALID

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GRAPH = os.path.join(REPO, "re/re_/ghidra_projects/kenshi/fn_graph.tsv")
EXE = os.path.join(os.environ.get("KENSHI_DIR", "/home/rdga1/rdga1prj/Kenshi_test"), "kenshi_x64.exe")
FP = ("addss", "subss", "mulss", "divss", "addsd", "subsd", "mulsd", "divsd", "sqrtss", "sqrtsd", "minss", "maxss", "minsd", "maxsd", "cvt", "comiss", "comisd",
      "ucomiss", "ucomisd", "mulps", "addps", "subps", "divps", "rsqrt", "rcp", "pmul", "dpps", "andps", "xorps", "roundss", "roundsd")
INT = ("add", "sub", "imul", "mul", "div", "idiv", "shl", "shr", "sar", "lea", "and", "or", "xor", "cmp", "neg", "not", "bt")


def load():
    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData), s.Name.rstrip(b"\0").decode(), s.get_data()) for s in pe.sections]
    return base, secs


def sec_of(secs, a):
    for lo, hi, n, _ in secs:
        if lo <= a < hi: return n
    return None


def classify(md, secs, code_of, addr, size):
    code = code_of(addr, size)
    n = ncall = nind = nfp = nint = nrd = ndr = 0; dcalls = []; bad = ""
    for i in md.disasm(code, addr):
        n += 1; m = i.mnemonic
        if m == "call":
            op = i.operands[0]
            if op.type == CS_OP_IMM: dcalls.append(op.imm); ncall += 1
            else: nind += 1
        elif m == "jmp":
            op = i.operands[0]
            if op.type != CS_OP_IMM: bad = bad or "jmp-ind"
            elif not (addr <= op.imm < addr + size): dcalls.append(op.imm); ncall += 1   # tail call
        elif m in ("int3", "ud2", "syscall"): pass
        if any(m.startswith(p) for p in FP): nfp += 1
        elif m in INT: nint += 1
        for op in i.operands:
            if op.type != CS_OP_MEM: continue
            if op.mem.segment not in (0,) and i.op_str.find("gs:") >= 0 or i.op_str.find("fs:") >= 0: bad = bad or "tls"
            if op.mem.base == X86_REG_RIP:
                s = sec_of(secs, i.address + i.size + op.mem.disp)
                if s == ".rdata": nrd += 1
                elif s in (".data", ".bss"):
                    if op.access & CS_AC_WRITE: bad = bad or "data-write"
                    else: ndr += 1
    return dict(n=n, ncall=ncall, nind=nind, dcalls=dcalls, nfp=nfp, nint=nint, nrd=nrd, ndr=ndr, bad=bad)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out"); ap.add_argument("--smoke", type=int, default=0); ap.add_argument("--max-size", type=int, default=1500)
    ap.add_argument("--seed", type=int, default=1); a = ap.parse_args()
    base, secs = load()
    def code_of(addr, size):
        for lo, hi, n, d in secs:
            if lo <= addr < hi: return d[addr - lo: addr - lo + size]
        return b""
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    fns = []
    for ln in open(GRAPH):
        p = ln.rstrip("\n").split("\t")
        fns.append((int(p[0], 16), int(p[1]), p[2]))
    info = {}
    for addr, size, name in fns:
        if size > a.max_size or size < 6 or sec_of(secs, addr) != ".text":
            info[addr] = None; continue
        info[addr] = classify(md, secs, code_of, addr, size)
    cls = {}
    def kind(addr, depth=0):
        if addr in cls: return cls[addr]
        r = info.get(addr)
        if r is None: cls[addr] = "X"; return "X"
        if r["bad"] or r["nind"]: cls[addr] = "X"; return "X"
        if r["ncall"] == 0: cls[addr] = "L0"; return "L0"
        if depth >= 1: cls[addr] = "X"; return "X"
        cls[addr] = "X" if any(kind(c, depth + 1) != "L0" for c in r["dcalls"]) else "L1"
        return cls[addr]
    rows = []
    for addr, size, name in fns:
        r = info[addr]
        if r is None: continue
        k = kind(addr)
        cand = k in ("L0", "L1") and (r["nfp"] + r["nint"]) >= 2
        rows.append((addr, size, name, k, r["n"], r["nfp"], r["nint"], r["nrd"], r["ndr"], "1" if cand else "0"))
    out = open(a.out, "w") if a.out else sys.stdout
    out.write("addr\tsize\tname\tclass\tn_insn\tn_fp\tn_int\tn_rdata\tn_data_r\tcandidate\n")
    for r in rows: out.write("%x\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\n" % r)
    cnt = collections.Counter(r[3] for r in rows); ncand = sum(1 for r in rows if r[9] == "1")
    print(f"# functions {len(fns)}; scanned (<= {a.max_size} B, .text) {len(rows)}; L0 {cnt['L0']}, L1 {cnt['L1']}, X {cnt['X']}; candidates {ncand}", file=sys.stderr)
    if a.smoke:
        sys.path.insert(0, os.path.dirname(__file__))
        from exe_oracle import Oracle, HEAP, f32
        from unicorn import UcError
        random.seed(a.seed); o = Oracle()
        cands = [r for r in rows if r[9] == "1"]; sample = random.sample(cands, min(a.smoke, len(cands)))
        ok = fault = loop = 0; zr = 0; errs = collections.Counter()
        for r in sample:
            for k in range(0, 0x4000, 8): pass
            o.write(HEAP + 0x1000, bytes(random.getrandbits(8) for _ in range(0x800)))
            for j in range(4): o.write(HEAP + 0x1000 + 0x100 * j + 0x80, struct.pack("<Q", HEAP + 0x1000 + 0x100 * ((j + 1) % 4)))
            try:
                o.run(r[0], ints=[HEAP + 0x1000, HEAP + 0x1100, HEAP + 0x1200, HEAP + 0x1300], floats=[random.uniform(-100, 100) for _ in range(4)], max_insn=3000)
                ok += 1; zr += 1 if o.zero_reads else 0
            except UcError as e:
                errs[str(e)] += 1; fault += 1
        print(f"# smoke: {len(sample)} candidates run: finished {ok} ({100*ok/len(sample):.0f} %), of them with zero-reads {zr}; stopped with error {fault}: {dict(errs)}", file=sys.stderr)


if __name__ == "__main__":
    main()
