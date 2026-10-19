#!/usr/bin/env python3
"""
deepseek_cathode_shaders.py -- #191 stage E: what the shaders of Alien: Isolation (Cathode engine) tell us.
Input  : ~/re_work/alien_shaders/summary.txt -- one line per shader (stage, instruction / temp / texture-instruction counts, resource names read from the DXBC headers by carve.py + summary.py, outside the repo), and our frame description (PASS_COSTS sections 1-4).
         No shader code, no Kenshi files.
Output : docs/research/CATHODE_SHADERS_2026-10.md -- facts (numbers) and model answers marked SOURCE-MODEL / CHECK. No shader code in the repo.
Budget : hard USD limit from the actual usage fields.
Usage  : python3 tools/research/deepseek_cathode_shaders.py [--budget-usd 6] [--dry-run]
"""
import argparse
import collections
import datetime
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402
import deepseek_lowend_techniques as lt  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SUMMARY = Path.home() / "re_work" / "alien_shaders" / "summary.txt"
OUT = REPO / "docs" / "research" / "CATHODE_SHADERS_2026-10.md"

SYSTEM = (
    "You are a senior real-time rendering engineer who reverse-engineers shipped games from shader statistics. Answer concisely, in English. "
    "You only see per-shader statistics and resource names (no code). Say what each group of shaders most likely does, and which techniques are visible; "
    "mark every inference with 'INFERRED' and everything you are unsure of with 'CHECK'. Never invent numbers."
)
PACKS = [
    ("DEFERREDSHADERS", "Deferred pack (G-buffer fill, lighting, shadows): which techniques are visible from the resource names (g_smp_gbuffer_rt0..3, spotlight shadow samplers, depth/normal/specular/emissive samplers); how the cost is spread (instruction and texture counts, dynamic flow); what differs between the cheap and the expensive shaders."),
    ("BESPOKESHADERS", "Bespoke pack (special materials, compute shaders CS, geometry shaders GS): what do the compute and geometry shaders do; which materials are expensive; any sign of terrain / foliage / water / sky shaders."),
    ("REQUIREDSHADERS", "Required pack: which shaders are always needed (depth prepass, shadow-map passes, simple fills); the cost of the cheapest depth-only variants."),
    ("POSTPROCESSINGSHADERS", "Post-processing pack: the chain visible from the samplers (frame buffer, bloom, tone-map curve, colour LUT, noise, distortion, quarter-size blur): cost per effect, what is done at reduced resolution."),
]


def aggregate(lines):
    out = []
    by = collections.defaultdict(list)
    for l in lines:
        p = l.split()
        by[(p[0], p[2])].append((int(p[3].split('=')[1]), int(p[5].split('=')[1])))
    for (pack, st), v in sorted(by.items()):
        ins = sorted(x[0] for x in v)
        out.append((pack, st, len(v), ins[len(ins) // 2], ins[-1], sum(x[1] for x in v) / len(v)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget-usd", type=float, default=6.0)
    ap.add_argument("--in-rate", type=float, default=1.74)
    ap.add_argument("--out-rate", type=float, default=3.48)
    ap.add_argument("--cache-rate", type=float, default=0.0145)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    lines = [l for l in SUMMARY.read_text().splitlines() if l.strip()]
    pass_costs = (REPO / "docs/research/PASS_COSTS_2026-10.md").read_text()
    frame = pass_costs[pass_costs.index("## 1."):pass_costs.index("## 5.")] if "## 5." in pass_costs else pass_costs
    prefix = ("Our frame (Intel HD 520, 1080p, goal 99 FPS landscape). Our measurements:\n\n" + frame +
              "\n\nBelow: per-shader statistics of another game's shaders (Alien: Isolation, DX11, ubershader packs). One line per shader: "
              "pack, index, stage, instr = instruction count, temps = temp registers, tex = texture instructions, dynflow = dynamic flow-control count, res = resource names.\n\n" + "\n".join(lines) + "\n\nQuestion:\n")
    if a.dry_run:
        print(f"{len(lines)} shaders, prefix {len(prefix)} chars")
        return
    key = read_api_key()
    m = lt  # call()
    spent = 0.0
    agg = aggregate(lines)
    doc = [f"# Cathode shaders (Alien: Isolation) -- statistics and model reading ({datetime.date.today()})", "",
           "Source: DXBC blobs carved from `DATA/SHADERS/UBERSHADER/*_BIN.PAK` of the owner's install (read-only copy outside the repo, `~/re_work/alien_shaders/`); the statistics chunk (STAT) and resource names (RDEF) were parsed; **no disassembly and no shader code in the repo**. "
           "The model saw only these numbers and names. **Model answers are SOURCE-MODEL: unverified.**", "",
           "## Facts (counted by the agent)", "", "| pack | stage | shaders | median instr | max instr | mean tex instr |", "|---|---|---|---|---|---|"]
    for r in agg:
        doc.append("| %s | %s | %d | %d | %d | %.1f |" % r)
    doc.append("")
    for pack, q in PACKS:
        try:
            text, usage = lt.call(key, SYSTEM, prefix + q, 24000, "high")
        except RuntimeError as e:
            doc += [f"## {pack}", "", f"ERROR: {e}", ""]
            continue
        hit = usage.get("prompt_cache_hit_tokens", 0)
        miss = usage.get("prompt_cache_miss_tokens", usage.get("prompt_tokens", 0) - hit)
        cost = hit * a.cache_rate / 1e6 + miss * a.in_rate / 1e6 + usage.get("completion_tokens", 0) * a.out_rate / 1e6
        spent += cost
        print(f"{pack}: ${cost:.3f}, total ${spent:.3f}", flush=True)
        doc += [f"## {pack}", "", f"*SOURCE-MODEL, unverified. Cost ${cost:.3f}.*", "", text, ""]
        if spent > a.budget_usd:
            break
    q = ("Synthesis for our ground resolve pass: from all four groups, list at most 8 techniques that apply to a terrain/ground resolve and to static props on a weak iGPU, "
         "each with: what is visible in the statistics, why it would be cheaper than ours, risk for the image, hours. Mark INFERRED / CHECK.")
    if spent < a.budget_usd:
        text, usage = lt.call(key, SYSTEM, prefix + q, 24000, "high")
        hit = usage.get("prompt_cache_hit_tokens", 0)
        miss = usage.get("prompt_cache_miss_tokens", usage.get("prompt_tokens", 0) - hit)
        spent += hit * a.cache_rate / 1e6 + miss * a.in_rate / 1e6 + usage.get("completion_tokens", 0) * a.out_rate / 1e6
        doc += ["## Synthesis for our ground and props", "", "*SOURCE-MODEL, unverified.*", "", text, ""]
    doc += [f"Total spent: ${spent:.3f} of ${a.budget_usd:.2f}."]
    OUT.write_text("\n".join(doc) + "\n")
    print(f"written {OUT}, spent ${spent:.3f}")


if __name__ == "__main__":
    main()
