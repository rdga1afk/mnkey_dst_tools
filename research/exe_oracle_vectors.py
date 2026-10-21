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


# ---- batch 2 (#190): the 6 numeric "leaf" functions of combat/movement specs; callees that read game state are replaced by stubs with a known return value ----
def rd(o, a, kind): return {"f": const_f32, "d": const_f64}[kind](o, a)
def u32(o, a): return struct.unpack("<I", o.read(a, 4))[0]
def sweep(lst, n): return [lst[i % len(lst)] for i in range(n)]

def run2(o: Oracle):
    rows = []
    def emit(fn, inp, exe, ref):
        fmt = lambda v: ",".join("%.9g" % float(x) for x in np.atleast_1d(v))
        rows.append((fn, inp, fmt(exe), fmt(ref), "1" if np.array(exe, dtype=F).tobytes() == np.array(ref, dtype=F).tobytes() else "0"))
    obj = HEAP + 0x20000
    VALS = [0.0, 1.0, -1.0, 0.5, 10.0, 45.0, 90.0, 91.0, 100.0, 250.0, -50.0, 1e6, -1e6, 0.001]

    # 140885d80 blockChance(this, p): base = callee 140885190(this,1) [STUB]; piecewise: d = base - p; d<=0: K14 + K18*d, else K1c*d + K14; above T: soft cap; clamp [0, C]
    # K14/K18/K1c live in .data and are 0.0 in the file (filled by the game at run time): the emulator is given explicit test values
    file_consts = (rd(o, 0x142133e14, "f"), rd(o, 0x142133e18, "f"), rd(o, 0x142133e1c, "f"))
    K14, K18, K1c = 30.0, 0.5, 0.8
    o.write(0x142133e14, struct.pack("<fff", K14, K18, K1c))
    T, D1, D2, C = rd(o, 0x141689850, "f"), rd(o, 0x14168a4a8, "d"), rd(o, 0x141723a08, "d"), rd(o, 0x141685034, "f")
    h = o.stub(0x140885190, f32_ret=0.0)
    for base in [0.0, 20.0, 50.0, 100.0, -10.0]:
        o.unstub(h); h = o.stub(0x140885190, f32_ret=base)
        for p in VALS:
            o.run(0x140885d80, ints=[obj], floats=[None, p]); rax = o.xmm0_bits64()   # returned in xmm0 (low = chance, high = excess above T); the decompiler's ulonglong/rax is misleading
            d = F(F(base) - F(p))
            f = F(F(K14) + F(F(K18) * d)) if d <= 0 else F(F(F(K1c) * d) + F(K14))
            f3 = F(0.0)
            if F(T) < f:
                f3 = F(f - F(T)); f = F(np.float64(f3) * np.float64(D1) + np.float64(D2))
            if f <= F(C):
                lo, hi = (0, 0) if f < 0 else (struct.unpack("<I", struct.pack("<f", f))[0], struct.unpack("<I", struct.pack("<f", f3))[0])
            else: lo, hi = struct.unpack("<I", struct.pack("<f", F(C)))[0], 0
            emit("blockChance_140885d80", f"base={base};p={p}", np.array([rax & 0xffffffff, rax >> 32], dtype=np.uint32).view(F), np.array([lo, hi], dtype=np.uint32).view(F))
    o.unstub(h)

    # 140883a80 combatSpeed(this, a, b, flag): t = (S*f88*f34*K1 + b*K2)*K3; lerp(t, lo, hi); r = lerp*a; r > hi -> hi   (S = callee 1406457e0 [STUB])
    c1, c2, c3 = rd(o, 0x141696c8c, "f"), rd(o, 0x141696c90, "f"), rd(o, 0x141682ad8, "f")
    pairs = {0: (rd(o, 0x1416852f0, "f"), rd(o, 0x14168a178, "f")), 1: (rd(o, 0x14168bf24, "f"), rd(o, 0x141696c8c, "f"))}   # flag -> (hi=fVar6, lo=fVar7)
    for S in [0.0, 1.0, 50.0, 100.0]:
        h = o.stub(0x1406457e0, f32_ret=S)
        for f88, f34 in [(1.0, 1.0), (0.5, 2.0), (0.0, 1.0), (3.0, 0.25)]:
            for flag in (0, 1):
                for a, b in [(1.0, 0.0), (1.0, 50.0), (0.5, 100.0), (2.0, 10.0), (10.0, 200.0), (-1.0, 5.0)]:
                    o.write(obj + 0x88, struct.pack("<f", f88)); o.write(obj + 0x34, struct.pack("<f", f34))
                    o.run(0x140883a80, ints=[obj, None, None, flag], floats=[None, a, b]); xr = o.xmm0_f32()
                    hi_, lo_ = pairs[flag]
                    t = F(F(F(F(F(S) * F(f88)) * F(f34)) * F(c1)) + F(F(b) * F(c2))); t = F(t * F(c3))
                    lr = F(F(F(hi_) - F(lo_)) * t) + F(lo_)
                    r = F(lr * F(a))
                    if F(hi_) < r: r = F(hi_)
                    emit("combatSpeed_140883a80", f"S={S};f88={f88};f34={f34};flag={flag};a={a};b={b}", F(xr), r)
        o.unstub(h)

    # 140644b00 bleedAmount(this): flags -> kind; if kind != 4 and [[this+0x10]+200] != 0 and limbSlotState(...)==1 [STUB] : s = f48 + f40; s < 0 ? s * K : 0
    K = rd(o, 0x1416fb710, "f"); ptrA, ptrB = HEAP + 0x21000, HEAP + 0x22000
    for st in (1, 2):
        h = o.stub(0x1400cd450, rax=st)
        for b32 in (0, 1):
            for i8 in (1, 2, 3):
                for i20 in (0, 1):
                    for f48, f40 in [(-1.0, 0.0), (-0.5, -0.5), (1.0, -3.0), (2.0, 1.0), (0.0, 0.0), (-100.0, 50.0)]:
                        for pnull in (0, 1):
                            o.write(obj + 0x32, bytes([b32])); o.write(obj + 8, struct.pack("<i", i8)); o.write(obj + 0x20, struct.pack("<i", i20))
                            o.write(obj + 0x10, struct.pack("<Q", ptrA)); o.write(ptrA + 200, struct.pack("<Q", 0 if pnull else ptrB))
                            o.write(obj + 0x48, struct.pack("<f", f48)); o.write(obj + 0x40, struct.pack("<f", f40))
                            o.run(0x140644b00, ints=[obj]); x = F(o.xmm0_f32())
                            kind = (2 + (i20 != 1)) if i8 == 1 else ((i20 != 1) if i8 == 2 else 4)
                            sm = F(F(f48) + F(f40))
                            ref = F(sm * F(K)) if (b32 == 0 and kind != 4 and not pnull and st == 1 and sm < 0) else F(0.0)
                            emit("bleedAmount_140644b00", f"slotState={st};b32={b32};kind_i8={i8};i20={i20};f48={f48};f40={f40};ptr0={pnull}", x, ref)
        o.unstub(h)

    # 140608320 extraReach(this) = float([[this+400]+0x200]) * K ; 14060cd20 circleDistance(this) = double(f150 + f278) - double(extraReach) * 0.5
    Kr, H = rd(o, 0x14167e8d8, "f"), rd(o, 0x141684440, "d"); st_ = HEAP + 0x23000
    for v200 in sweep(VALS, 24):
        o.write(obj + 400, struct.pack("<Q", st_)); o.write(st_ + 0x200, struct.pack("<f", v200))
        o.run(0x140608320, ints=[obj]); emit("extraReach_140608320", f"f200={v200}", F(o.xmm0_f32()), F(F(v200) * F(Kr)))
    for f150 in [0.0, 1.0, 2.5, -1.0, 100.0]:
        for f278 in [0.0, 0.5, 3.0, -2.0]:
            for v200 in [0.0, 1.0, 4.0, 10.0, -6.0]:
                o.write(obj + 400, struct.pack("<Q", st_)); o.write(st_ + 0x150, struct.pack("<f", f150)); o.write(st_ + 0x200, struct.pack("<f", v200)); o.write(obj + 0x278, struct.pack("<f", f278))
                o.run(0x14060cd20, ints=[obj]); ex = F(F(v200) * F(Kr))
                ref = F(np.float64(F(F(f150) + F(f278))) - np.float64(ex) * np.float64(H))
                emit("circleDistance_14060cd20", f"f150={f150};f278={f278};f200={v200}", F(o.xmm0_f32()), ref)

    # 1401440b0 setMaxSpeed(this, p): [this+0x7c] = float(double(p) * 0.1)   (callee 1409b19a0 [STUB], argument check)
    K2 = rd(o, 0x14168a318, "d"); h = o.stub(0x1409b19a0, f32_ret=0.0)
    for p in sweep(VALS + [3.4e38, -3.4e38, 1e-30, 80.0, 123.456], 40):
        o.write(obj + 0x7c, b"\xaa" * 4); o.run(0x1401440b0, ints=[obj], floats=[None, p])
        emit("setMaxSpeed_1401440b0", f"p={p}", F(struct.unpack("<f", o.read(obj + 0x7c, 4))[0]), F(np.float64(F(p)) * np.float64(K2)))
    o.unstub(h)
    return rows, dict(K14_file_value=file_consts, K14_test=K14, K18=K18, K1c=K1c, T=T, D1=D1, D2=D2, C=C, c1=c1, c2=c2, c3=c3, K=K, Kr=Kr, H=H, K2=K2, pairs=pairs)


# ---- batch 3 (#190): the last two leaf functions of the combat spec ----
def run3(o: Oracle):
    rows = []
    def emit(fn, inp, exe, ref): rows.append((fn, inp, str(exe), str(ref), "1" if exe == ref else "0"))
    obj = HEAP + 0x30000
    # 1400cd450 limbSlotState(this, i) = u32 [this + 0x10 + 4*i]   (i is a signed 32-bit int)
    o.write(obj + 0x10, b"".join(struct.pack("<I", 0x1000 + k * 0x111) for k in range(16)))
    for i in [0, 1, 2, 5, 15, 3, 7, 11]:
        o.run(0x1400cd450, ints=[obj, i]); emit("limbSlotState_1400cd450", f"i={i}", o.rax() & 0xffffffff, 0x1000 + i * 0x111)
    # 140657590 partForSlot(this, slot): parts = ptr array [this+0x1a0], count [this+0x198]; kind(part): [p+8]==1 -> 2+([p+0x20]!=1), ==2 -> ([p+0x20]!=1), else 4; first part with kind == slot, else 0
    arr, parts = HEAP + 0x31000, HEAP + 0x32000
    layouts = [[(1, 1), (2, 1), (2, 0), (3, 0)], [(2, 0)], [(1, 0), (1, 1)], [(5, 0), (9, 1), (1, 1)], []]
    def kind(a, b): return (2 + (b != 1)) if a == 1 else ((b != 1) if a == 2 else 4)
    for li, lay in enumerate(layouts):
        o.write(obj + 0x198, struct.pack("<I", len(lay))); o.write(obj + 0x1a0, struct.pack("<Q", arr))
        for k, (a, b) in enumerate(lay):
            p = parts + k * 0x100
            o.write(arr + 8 * k, struct.pack("<Q", p)); o.write(p + 8, struct.pack("<i", a)); o.write(p + 0x20, struct.pack("<i", b))
        for slot in range(0, 6):
            o.run(0x140657590, ints=[obj, slot])
            ref = 0
            for k, (a, b) in enumerate(lay):
                if kind(a, b) == slot: ref = parts + k * 0x100; break
            emit("partForSlot_140657590", f"layout={li};slot={slot}", o.rax(), ref)
    return rows
