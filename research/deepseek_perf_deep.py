#!/usr/bin/env python3
"""
deepseek_perf_deep.py -- #191: deep performance research in stages A-D through DeepSeek.
Input  : only our own source files and documents (no Kenshi files, no decompilation). The constant prefix (docs + hardware) is byte-identical in every request (cache hit).
Output : docs/research/perf_deep_findings.jsonl (stage A), docs/research/PERF_DEEP_2026-10.md (stages B-D), rows in OPT_CANDIDATES.tsv (mark deepseek-19.10).
Budget : hard USD limit from the ACTUAL usage fields; state is kept in temp_/perf_deep/ so a stopped run resumes where it ended.
Usage  : python3 tools/research/deepseek_perf_deep.py [--stage A|B|C|D|all] [--budget-usd 28] [--dry-run]
"""
import argparse
import datetime
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402
import deepseek_lowend_techniques as lt  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
STATE = REPO / "temp_" / "perf_deep"
FINDINGS = REPO / "docs" / "research" / "perf_deep_findings.jsonl"
REPORT = REPO / "docs" / "research" / "PERF_DEEP_2026-10.md"
SPEND = STATE / "spend.json"
CHUNK = 110000  # chars per request; longer files go in parts with absolute line numbers

SRC_DIRS = ["engine/src/render", "engine/src/world", "engine/src/ecs", "engine/src/io", "game/src", "shaders"]
EXT = {".cpp", ".h", ".glsl", ".vert", ".frag", ".comp"}

SYSTEM = (
    "You are a senior engine performance engineer for open-world games on weak integrated GPUs (Intel Gen9) and 2-core CPUs. "
    "Answer in English, concretely. Every estimate is an estimate: give a range, a confidence (low / medium / high) and say how to measure it. "
    "Mark facts you are not sure about with 'CHECK'. Do not invent function names: cite only what is in the text given."
)


def doc(rel, limit=None):
    t = (REPO / rel).read_text()
    return t if limit is None else t[:limit]


def prefix():
    fb = doc("docs/FRAME_BUDGET.md")
    cut = fb.find("\n## 3")
    fb = fb[:cut] if cut > 0 else fb
    return ("HARDWARE: Intel HD 520 (Gen9 GT2, 24 EU, shared RAM, Mesa ANV Vulkan), 2 physical cores / 4 threads (i5-6200U), 1920x1080, goal landscape 99 FPS (10.1 ms), today route p50 ~15 ms / p99 ~19 ms at 50 m/s.\n\n"
            "=== FRAME_BUDGET sections 0-2 ===\n" + fb + "\n\n=== PASS_COSTS ===\n" + doc("docs/research/PASS_COSTS_2026-10.md")
            + "\n\n=== FAILED_APPROACHES (do not propose these again) ===\n" + doc("docs/FAILED_APPROACHES.md")
            + "\n\n=== OPT_CANDIDATES.tsv ===\n" + doc("docs/research/OPT_CANDIDATES.tsv")
            + "\n\n=== LOWEND_TECHNIQUES (shortened) ===\n" + doc("docs/research/LOWEND_TECHNIQUES_2026-10.md", 12000) + "\n\n=== TASK ===\n")


def files():
    out = []
    for d in SRC_DIRS:
        for p in sorted((REPO / d).rglob("*")):
            if p.is_file() and p.suffix in EXT:
                out.append(p)
    return out


def parts(p):
    lines = p.read_text(errors="replace").splitlines()
    res, cur, size, start = [], [], 0, 1
    for i, l in enumerate(lines, 1):
        s = f"{i}: {l}\n"
        if size + len(s) > CHUNK and cur:
            res.append((start, "".join(cur)))
            cur, size, start = [], 0, i
        cur.append(s)
        size += len(s)
    if cur:
        res.append((start, "".join(cur)))
    return res


class Meter:
    def __init__(self, a):
        self.a = a
        self.spent = json.loads(SPEND.read_text())["usd"] if SPEND.exists() else 0.0

    def add(self, usage):
        a = self.a
        hit = usage.get("prompt_cache_hit_tokens", 0)
        miss = usage.get("prompt_cache_miss_tokens", usage.get("prompt_tokens", 0) - hit)
        c = hit * a.cache_rate / 1e6 + miss * a.in_rate / 1e6 + usage.get("completion_tokens", 0) * a.out_rate / 1e6
        self.spent += c
        SPEND.write_text(json.dumps({"usd": self.spent}))
        return c

    def ok(self, reserve):
        return self.spent + reserve <= self.a.budget_usd


def ask(key, m, user, max_tokens, effort="high"):
    text, usage = lt.call(key, SYSTEM, user, max_tokens, effort)
    return text, m.add(usage)


def parse_json_array(text):
    m = re.search(r"\[.*\]", text, re.S)
    if not m:
        return None
    try:
        return json.loads(m.group(0))
    except Exception:
        return None


def stage_a(key, m, pre, a):
    done = set()
    if FINDINGS.exists():
        for l in FINDINGS.read_text().splitlines():
            try:
                done.add((json.loads(l)["_file"], json.loads(l)["_part"]))
            except Exception:
                pass
    fl = files()
    n = 0
    for p in fl:
        rel = str(p.relative_to(REPO))
        for idx, (start, body) in enumerate(parts(p)):
            if (rel, idx) in done:
                continue
            if not m.ok(0.6):
                print(f"A: budget reached at {rel}", flush=True)
                return
            q = (f"Stage A. File {rel} (part {idx}, lines numbered, starting at line {start}). List findings: what costs milliseconds, memory or hitches, and why. "
                 "Return ONLY a JSON array (maybe empty); each item: {\"line\": int, \"what\": str, \"why\": str, \"gain_ms_low\": float, \"gain_ms_high\": float, "
                 "\"p99_effect\": str, \"look_risk\": \"none|near|far|both\", \"effort_h\": float, \"confidence\": \"low|medium|high\"}. "
                 "Skip style issues and anything in FAILED_APPROACHES.\n\n" + body)
            try:
                text, c = ask(key, m, pre + q, 24000, "high")
            except RuntimeError as e:
                print(f"A: {rel} ERROR {e}", flush=True)
                continue
            items = parse_json_array(text)
            with FINDINGS.open("a") as f:
                if items is None:
                    f.write(json.dumps({"_file": rel, "_part": idx, "_error": "unparsed", "raw": text[:2000]}) + "\n")
                for it in items or []:
                    if isinstance(it, dict):
                        it["_file"], it["_part"] = rel, idx
                        f.write(json.dumps(it) + "\n")
            n += 1
            print(f"A {rel}[{idx}]: ${c:.3f}, total ${m.spent:.2f}", flush=True)


SUBSYSTEMS = [
    "Ground (resolve / G-buffer / reuse)", "Sky / water / lighting", "Buildings and props", "Vegetation",
    "CPU frame (submit, culling, ECS)", "Streaming and hitches (p99 at 50 m/s about 18.9 ms)", "Memory (shared RAM of the iGPU)", "Load time",
]


def stage_b(key, m, pre, a):
    findings = FINDINGS.read_text() if FINDINGS.exists() else ""
    cap = 300000
    for s in SUBSYSTEMS:
        out = STATE / ("B_" + re.sub(r"\W+", "_", s) + ".md")
        if out.exists():
            continue
        if not m.ok(1.5):
            print("B: budget reached", flush=True)
            return
        q = (f"Stage B. Subsystem: {s}. Below are the per-file findings of stage A (JSONL). Merge the ones that belong to this subsystem, drop duplicates, and ADD your own ideas "
             "(mark them OWN-IDEA). Output markdown: a table of candidates (title; files:lines; ms p50 range; ms p99 range; look risk; hours) and 5-10 lines of reasoning.\n\n" + findings[:cap])
        text, c = ask(key, m, pre + q, 32000, "high")
        out.write_text(text)
        print(f"B {s}: ${c:.3f}, total ${m.spent:.2f}", flush=True)


def stage_c(key, m, pre, a):
    out = STATE / "C.md"
    if out.exists() or not m.ok(2.0):
        return
    allb = "\n\n".join(p.read_text() for p in sorted(STATE.glob("B_*.md")))
    q = ("Stage C. Rank into ONE list of candidates. For each: ms p50 / p99 (range); confidence; does it change the look (near / far); hours; how to measure; conflicts with FAILED_APPROACHES. "
         "Sort by expected p50 gain per hour, then by confidence. Markdown table, at most 40 rows.\n\n" + allb)
    text, c = ask(key, m, pre + q, 32000, "high")
    out.write_text(text)
    print(f"C: ${c:.3f}, total ${m.spent:.2f}", flush=True)


def stage_d(key, m, pre, a):
    out = STATE / "D.md"
    if out.exists() or not m.ok(3.0):
        return
    c_ = (STATE / "C.md").read_text()
    q = ("Stage D, REVIEWER pass over the top 15 of the ranked list. For each: was this already tried by us (cite FAILED_APPROACHES / OPT_CANDIDATES ids), where is the estimate inflated, which dependencies exist. "
         "Then, for the top 10 after your review, a step-by-step prototype plan of at most 4 hours with an oracle (what number proves it works or fails; threshold set BEFORE the work). "
         "Finally output a TSV block (tab-separated, header-less, marker column 'deepseek-19.10') of the top 10 with columns: id, title, source, licence, target pass, current ms, why it may work, parity, gain estimate, hours, stage, status, task.\n\n" + c_)
    text, c = ask(key, m, pre + q, 32000, "high")
    out.write_text(text)
    print(f"D: ${c:.3f}, total ${m.spent:.2f}", flush=True)


def write_report(m):
    parts_ = [f"# Deep performance research (DeepSeek, {datetime.date.today()})", "",
              "Source: DeepSeek `%s`; input was only our own sources and documents (no Kenshi files, no decompilation). **All numbers are SOURCE-MODEL estimates: unverified; nothing here is implemented or measured.** "
              "Stage A findings: `perf_deep_findings.jsonl`. Spent $%.2f." % (lt.MODEL, m.spent), ""]
    for s in SUBSYSTEMS:
        f = STATE / ("B_" + re.sub(r"\W+", "_", s) + ".md")
        if f.exists():
            parts_ += [f"## B. {s}", "", f.read_text(), ""]
    for tag, title in (("C", "C. Ranked candidates"), ("D", "D. Reviewer pass and prototype plans")):
        f = STATE / f"{tag}.md"
        if f.exists():
            parts_ += [f"## {title}", "", f.read_text(), ""]
    REPORT.write_text("\n".join(parts_) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="all")
    ap.add_argument("--budget-usd", type=float, default=28.0)
    ap.add_argument("--in-rate", type=float, default=1.74)
    ap.add_argument("--out-rate", type=float, default=3.48)
    ap.add_argument("--cache-rate", type=float, default=0.0145)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    STATE.mkdir(parents=True, exist_ok=True)
    pre = prefix()
    if a.dry_run:
        n = sum(len(parts(p)) for p in files())
        print(f"prefix {len(pre)} chars, {len(files())} files, {n} requests in stage A")
        return
    key = read_api_key()
    m = Meter(a)
    if a.stage in ("A", "all"):
        stage_a(key, m, pre, a)
    if a.stage in ("B", "all"):
        stage_b(key, m, pre, a)
    if a.stage in ("C", "all"):
        stage_c(key, m, pre, a)
    if a.stage in ("D", "all"):
        stage_d(key, m, pre, a)
    write_report(m)
    print(f"done, spent ${m.spent:.2f} of ${a.budget_usd:.2f}")


if __name__ == "__main__":
    main()
