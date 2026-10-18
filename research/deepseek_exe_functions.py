#!/usr/bin/env python3
"""
deepseek_exe_functions.py -- #188 stage runner: sends the seed functions of kenshi_x64.exe.c (Ghidra decompilation, text only) to the DeepSeek API, one request per
function, and stores a structured answer. Model text is UNTRUSTED: a fact is accepted only with its address and after the sample check through Ghidra MCP.

Input : temp_/kenshi_fn_index.jsonl + a seed list (temp_/kenshi_seeds_stage1.jsonl, see kenshi_seed_select.py); the decompiled file stays local, only the function text goes out.
Output: temp_/kenshi_stage1_results.jsonl (one line per function: purpose, inputs/outputs, formulas+constants with the source line, relevant calls, confidence, usage, cost).
Cache : the instruction block is FIRST and byte-identical in every request, the function text last (DeepSeek reuses the common prefix from its cache).
Budget: hard limit in USD from the ACTUAL usage fields of every response (prompt_cache_hit / miss / completion tokens); resumable (functions already in the output are skipped).
        Functions > --big characters are not sent whole unless they are seeds ("why" is not "callee"); a callee that large gets signature + callee list only.

Usage: python3 tools/research/deepseek_exe_functions.py --budget-usd 15 [--workers 8] [--limit N] [--dry-run] [--only-why addr,marker]
"""
import argparse
import json
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "re/re_/kenshi_re/kenshi_x64.exe.c"
API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-v4-pro"

SYSTEM = (
    "You analyse ONE function from a Ghidra decompilation of the game Kenshi (x64 Windows, MSVC, Ogre3D engine). Names like FUN_14xxxxxxx are addresses; param_N, local_N and DAT_14xxxxxxx are Ghidra placeholders. "
    "Answer with ONE JSON object and nothing else, with exactly these keys: "
    '"purpose" (1-3 sentences, plain words), "inputs" (list of {"name","meaning"}), "outputs" (list of {"name","meaning"}), '
    '"formulas" (list of {"what","expression_in_words","constants":[numbers; only a constant that is written in the function text, never the value behind a DAT_14xxxxxxx name: write \"unknown\" for it],"source_line":"the line of the function text the formula comes from, copied"}), '
    '"important_calls" (list of {"callee":"FUN_14xxxxxxx or name","why"}), "system" (one of: movement, squad, npc_behaviour, melee_combat, limb_health, animation, rendering, ui, io, memory, other), '
    '"confidence" (number 0..1). '
    "Rules: say only what the code shows. If something is not in the code write \"unknown\". Never invent field names, formulas or constants; a constant must appear in the text. "
    "Do not write pseudocode or assembly in your answer; describe in words. The function text follows after the marker FUNCTION. After the function text there may be a block CONSTANTS: the raw bytes of the read-only data that the function names as DAT_14xxxxxxx, read from the executable (u32 / f32 / u64 / f64 views of the same bytes: the type is the one the code uses it as). A constant value may be quoted only from that block."
)

EXE = Path("/home/rdga1/rdga1prj/Kenshi_test/kenshi_x64.exe")
DAT_RE = re.compile(r"\b_?DAT_(14[0-9a-fA-F]{6,7})\b")


class RoData:
    """Read-only sections of the PE image: DAT_14xxxxxxx constants are read straight from the exe file (Ghidra image base 0x140000000)."""

    def __init__(self, path=EXE):
        self.blob = path.read_bytes()
        b = self.blob
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        nsec, optsz = struct.unpack_from("<H", b, pe + 6)[0], struct.unpack_from("<H", b, pe + 20)[0]
        self.base = struct.unpack_from("<Q", b, pe + 24 + 24)[0]
        self.secs = []
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            vsz, va, rsz, raw, chars = struct.unpack_from("<IIII", b, o + 8)[0], struct.unpack_from("<I", b, o + 12)[0], struct.unpack_from("<I", b, o + 16)[0], struct.unpack_from("<I", b, o + 20)[0], struct.unpack_from("<I", b, o + 36)[0]
            if not chars & 0x80000000:   # not writable = constant data
                self.secs.append((self.base + va, max(vsz, rsz), raw, rsz))

    def read(self, addr, n=8):
        for va, sz, raw, rsz in self.secs:
            if va <= addr < va + min(sz, rsz) - n:
                return self.blob[raw + addr - va: raw + addr - va + n]
        return None

    def block(self, text, limit=40):
        out, seen = [], set()
        for m in DAT_RE.finditer(text):
            a = int(m.group(1), 16)
            if a in seen or len(seen) >= limit:
                continue
            seen.add(a)
            r = self.read(a)
            if r is None:
                continue
            u32, = struct.unpack("<I", r[:4])
            f32, = struct.unpack("<f", r[:4])
            u64, = struct.unpack("<Q", r)
            f64, = struct.unpack("<d", r)
            out.append(f"DAT_{a:x}: u32=0x{u32:08x} ({u32}) f32={f32:.9g} | u64=0x{u64:016x} f64={f64:.12g}")
        return ("\nCONSTANTS (read-only data of the executable)\n" + "\n".join(out) + "\n") if out else ""


class Budget:
    def __init__(self, limit, in_rate, out_rate, cache_rate):
        self.limit, self.in_rate, self.out_rate, self.cache_rate = limit, in_rate / 1e6, out_rate / 1e6, cache_rate / 1e6
        self.spent, self.reserved, self.lock = 0.0, 0.0, threading.Lock()
        self.hit = self.miss = self.out = 0

    def reserve(self, amount):
        with self.lock:
            if self.spent + self.reserved + amount > self.limit:
                return False
            self.reserved += amount
            return True

    def settle(self, reserved, usage):
        with self.lock:
            self.reserved -= reserved
            hit = usage.get("prompt_cache_hit_tokens", 0)
            miss = usage.get("prompt_cache_miss_tokens", usage.get("prompt_tokens", 0) - hit)
            out = usage.get("completion_tokens", 0)
            cost = hit * self.cache_rate + miss * self.in_rate + out * self.out_rate
            self.spent += cost
            self.hit, self.miss, self.out = self.hit + hit, self.miss + miss, self.out + out
            return cost


def load_text(index_row, src_lines):
    return "".join(src_lines[index_row["start"] - 1:index_row["end"]])


def call(key, prompt, effort, max_tokens):
    body = json.dumps({"model": MODEL, "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}],
                       "max_tokens": max_tokens, "reasoning_effort": effort, "response_format": {"type": "json_object"}}).encode()
    req = urllib.request.Request(API_URL, data=body, method="POST", headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    last = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=900) as r:
                d = json.loads(r.read())
                return d["choices"][0]["message"]["content"].strip(), d.get("usage", {})
        except urllib.error.HTTPError as e:
            last = f"HTTP {e.code}: {e.read().decode(errors='replace')[:200]}"
            if e.code in (429, 500, 502, 503):
                time.sleep(10 * (attempt + 1))
                continue
            break
        except Exception as e:
            last = str(e)
            time.sleep(5 * (attempt + 1))
    raise RuntimeError(last)


def parse(text):
    a, b = text.find("{"), text.rfind("}")
    try:
        return json.loads(text[a:b + 1], strict=False)
    except Exception:
        return {"unparsed": text[:4000]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", default=str(REPO / "temp_/kenshi_fn_index.jsonl"))
    ap.add_argument("--seeds", default=str(REPO / "temp_/kenshi_seeds_stage1.jsonl"))
    ap.add_argument("--out", default=str(REPO / "temp_/kenshi_stage1_results.jsonl"))
    ap.add_argument("--budget-usd", type=float, required=True)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--in-rate", type=float, default=1.74)
    ap.add_argument("--out-rate", type=float, default=3.48)
    ap.add_argument("--cache-rate", type=float, default=0.0145)
    ap.add_argument("--effort", default="low")
    ap.add_argument("--max-tokens", type=int, default=12000)
    ap.add_argument("--big", type=int, default=40000)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--only-why", default="", help="comma list of 'why' prefixes to run (addr, marker, callee)")
    ap.add_argument("--no-consts", action="store_true", help="do not append the DAT_ constants block")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    idx = {}
    for l in open(a.index):
        r = json.loads(l)
        idx[r["name"]] = r
    seeds = [json.loads(l) for l in open(a.seeds)]
    if a.only_why:
        ws = tuple(a.only_why.split(","))
        seeds = [s for s in seeds if s["why"].startswith(ws)]
    out = Path(a.out)
    done = set()
    prior = 0.0
    if out.exists():
        for l in out.read_text().splitlines():
            if l.strip():
                r = json.loads(l)
                done.add(r["name"])
                prior += r.get("cost_usd", 0)
    todo = [s for s in seeds if s["name"] not in done]
    if a.limit:
        todo = todo[:a.limit]
    print(f"seeds {len(seeds)}, done {len(done)}, to do {len(todo)}, spent so far ${prior:.2f}, budget ${a.budget_usd:.2f}")
    budget = Budget(a.budget_usd, a.in_rate, a.out_rate, a.cache_rate)
    budget.spent = prior
    chars = sum(min(s["send_chars"], a.big) for s in todo)
    if a.dry_run:
        toks = chars / 3.5
        print(f"dry-run: ~{int(toks)} input tokens, estimated ${toks * a.in_rate / 1e6 + len(todo) * 700 * a.out_rate / 1e6:.2f} (no cache, 700 output tokens each)")
        return

    key = read_api_key()
    rodata = RoData() if not a.no_consts else None
    src_lines = SRC.read_text(errors="replace").split("\n")
    src_lines = [x + "\n" for x in src_lines]
    lock = threading.Lock()

    def work(s):
        f = idx[s["name"]]
        row = {"name": s["name"], "addr": f["addr"], "why": s["why"]}
        if s.get("summary_only") or f["chars"] > a.big and not s["why"].startswith(("addr", "marker")):
            text = f"[summary only: function is {f['chars']} characters]\nSignature line: {src_lines[f['start'] - 1].strip()}\nCallees defined in the file: {', '.join(f['callees'][:60])}\n"
        else:
            text = load_text(f, src_lines)
            if len(text) > a.big * 2:
                text = text[:a.big * 2] + "\n/* truncated */\n"
        prompt = "FUNCTION\n" + text + (rodata.block(text) if rodata else "")
        worst = len(prompt) / 3.0 / 1e6 * a.in_rate + a.max_tokens / 1e6 * a.out_rate
        if not budget.reserve(worst):
            return None
        try:
            answer, usage = call(key, prompt, a.effort, a.max_tokens)
            cost = budget.settle(worst, usage)
            row.update(result=parse(answer), usage=usage, cost_usd=round(cost, 5))
        except Exception as e:
            budget.settle(worst, {})
            row.update(error=str(e)[:200], cost_usd=0.0)
        return row

    n = 0
    with ThreadPoolExecutor(max_workers=a.workers) as ex, out.open("a") as fh:
        futs = [ex.submit(work, s) for s in todo]
        for fu in as_completed(futs):
            r = fu.result()
            if r is None:
                continue
            with lock:
                fh.write(json.dumps(r, ensure_ascii=False) + "\n")
                fh.flush()
            n += 1
            if n % 50 == 0:
                tot = budget.hit + budget.miss
                print(f"  {n}/{len(todo)}, spent ${budget.spent:.2f}, cache hit {100 * budget.hit / max(tot, 1):.0f}% of input tokens")
    tot = budget.hit + budget.miss
    print(f"done {n}; spent ${budget.spent:.2f} of ${a.budget_usd:.2f}; cache hit {100 * budget.hit / max(tot, 1):.0f}% of input tokens")


if __name__ == "__main__":
    main()
