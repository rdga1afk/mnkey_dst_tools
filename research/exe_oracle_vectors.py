"""Vector sets for exe_oracle.py (#190): three pure functions of kenshi_x64.exe, 100+ points each incl. edge values.
Reference = the formula of docs/spec/movement.md / the Ghidra reading, written in numpy float32/float64 with the constants READ FROM THE EXE (.rdata).
Output TSV: function, inputs, exe_result, reference, equal(bit-exact), spec_literal_const_result (the same formula with the rounded constant of the spec)."""
import itertools, struct
import numpy as np
from exe_oracle import Oracle, HEAP, f32, r32, r64

F = np.float32
def const_f32(o, a): return struct.unpack("<f", o.read(a, 4))[0]
def const_f64(o, a): return struct.unpack("<d", o.read(a, 8))[0]

LERP_T = [0.0, 0.25, 0.5, 1.0, -1.0, 2.0, 1e-7, 0.999999, 100.0, -0.5]
LERP_AB = [(0.0, 1.0), (10.0, 20.0), (20.0, 10.0), (-5.0, 5.0), (0.0, 0.0), (1e6, -1e6), (0.1, 0.9)]
VEC = [0.0, 1.0, -1.0, 0.1, 10.0, 123.456, -987.65, 1e6, -1e6, 3.4e38, 1e-30, 147456.0]


def run(o: Oracle, out=None):
    rows = []
    def emit(fn, inp, exe, ref, lit):
        fmt = lambda v: ",".join("%.9g" % float(x) for x in np.atleast_1d(v))
        rows.append((fn, inp, fmt(exe), fmt(ref), "1" if np.array(exe).tobytes() == np.array(ref).tobytes() else "0", fmt(lit)))

    # 1. 140015b63: lerp(t, a, b) = (b - a) * t + a   (float32)
    for t in LERP_T:
        for a, b in LERP_AB:
            o.run(0x140015b63, floats=[t, a, b]); exe = F(o.xmm0_f32())
            ref = F(F(F(b) - F(a)) * F(t)) + F(a)
            emit("lerp_140015b63", f"t={t};a={a};b={b}", exe, F(ref), F(ref))

    # 2. 140144050: desiredVelocity: [obj+0x30..0x3c] = (x*K, y*K, z*K, 0), K double read from 14168a318 (spec: 0.1)
    K = const_f64(o, 0x14168a318)
    obj, vec = HEAP + 0x1000, HEAP + 0x2000
    for x, y, z in itertools.product(VEC[:7], VEC[:7], VEC[:4]):
        o.write(vec, struct.pack("<fff", x, y, z)); o.write(obj + 0x30, b"\xaa" * 16)
        o.run(0x140144050, ints=[obj, vec])
        exe = struct.unpack("<ffff", o.read(obj + 0x30, 16))
        ref = tuple(F(np.float64(F(v)) * np.float64(K)) for v in (x, y, z)) + (F(0.0),)
        lit = tuple(F(np.float64(F(v)) * np.float64(0.1)) for v in (x, y, z)) + (F(0.0),)
        emit("desiredVelocity_140144050", f"v=({x};{y};{z})", np.array(exe, dtype=F), np.array(ref, dtype=F), np.array(lit, dtype=F))

    # 3. 140883db0: runSpeed(this): B = lerp(this[+0x94]*S, B[+0x4c], B[+0x48]); return (r*0.5+0.5)*B, r = virtual call slot +0x398 of this[+0x10] (stub returns r)
    S = const_f32(o, 0x141682ad8); H = const_f64(o, 0x141684440)
    this, A, B, vt, stub, rbuf = HEAP + 0x10000, HEAP + 0x11000, HEAP + 0x12000, HEAP + 0x13000, HEAP + 0x14000, HEAP + 0x15000
    o.write(stub, b"\x48\xb8" + struct.pack("<Q", rbuf) + b"\xf3\x0f\x10\x00\xc3")   # mov rax, rbuf; movss xmm0,[rax]; ret
    o.write(vt + 0x398, struct.pack("<Q", stub)); o.write(A, struct.pack("<Q", vt))
    o.write(this + 0x10, struct.pack("<Q", A)); o.write(A + 0x2e0, struct.pack("<Q", B))
    for skill, lo, hi, r in itertools.product([0.0, 1.0, 20.0, 50.0, 100.0, 250.0, -10.0], [(10.0, 30.0), (0.0, 0.0), (5.0, 1.0)], [None], [0.0, 0.5, 1.0, -1.0, 2.0]):
        l, h = lo
        o.write(this + 0x94, struct.pack("<f", skill)); o.write(B + 0x4c, struct.pack("<f", l)); o.write(B + 0x48, struct.pack("<f", h)); o.write(rbuf, struct.pack("<f", r))
        o.run(0x140883db0, ints=[this]); exe = F(o.xmm0_f32())
        def calc(s, hh):
            t = F(F(skill) * F(s)); bb = F(F(F(h) - F(l)) * t) + F(l)
            return F((np.float64(F(r)) * np.float64(hh) + np.float64(hh)) * np.float64(bb))
        emit("runSpeed_140883db0", f"skill={skill};min={l};max={h};r={r}", exe, calc(S, H), calc(0.01, 0.5))
    if o.zero_reads: rows.append(("NOTE", "zero-read addresses seen in the last run", ",".join(hex(a) for a in o.zero_reads[:6]), "", "", ""))
    print("function\tinputs\texe\treference(exe consts)\tbit_equal\tspec_literal_consts")
    for r in rows: print("\t".join(r))
    ok = sum(1 for r in rows if r[4] == "1"); tot = sum(1 for r in rows if r[4] in "01" and r[4])
    lit_diff = sum(1 for r in rows if r[4] == "1" and r[2] != r[5])
    print(f"# K(14168a318)={K!r} S(141682ad8)={S!r} H(141684440)={H!r}; bit-equal {ok}/{tot}; spec-literal constants differ from exe on {lit_diff} points", file=__import__("sys").stderr)
