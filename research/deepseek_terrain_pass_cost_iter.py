#!/usr/bin/env python3
"""
deepseek_terrain_pass_cost_iter.py -- ROUND 4+: iterative search for the MECHANISM behind the
terrain G-buffer cost that does NOT depend on triangle density (density x10 gave no GPU-time
change in 3 independent measurements) but vanishes with scissor 1x1 / no draw, and for the
best fix. Up to --rounds rounds, stops after 3 consecutive "stable" evaluations.
Per round: L1 queries (deepseek-v4-pro, effort max) -> path verification (gh) -> L3 deep dive
on real code -> evaluation + follow-up queries. State saved every round.
Output: docs/research/TERRAIN_PASS_COST_ITER_DEEPSEEK_RESEARCH.md (+ .raw.json)
"""
import argparse, json, re, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deepseek_terrain_alternatives_search as base  # noqa: E402
from _deepseek_common import read_api_key  # noqa: E402

base.DEEP_ASK = (
    "Below is REAL source code from a public repository ({repo}:{path}). Using ONLY this code (not memory), "
    "answer for a terrain render pass on Intel Gen9/Mesa ANV via SDL_GPU/Vulkan: what does it do that costs "
    "GPU time independent of triangle count -- depth/HiZ operations and when they trigger, barriers/layout "
    "transitions per render pass, render-pass begin/end flushes, clear costs, early-Z/overdraw handling, "
    "draw-order sorting, pixel dispatch behaviour? Report (1) mechanism and the exact function/condition that "
    "triggers it; (2) numbers/thresholds in code; (3) QUOTE up to 3 short snippets (max 2 lines each). If the "
    "file does not answer, say exactly 'NOT RELEVANT' and why in one line.\n\n=== {repo}:{path} ===\n{code}\n"
)

ROOT = Path(__file__).resolve().parents[2]
OUT_MD = ROOT / "docs/research/TERRAIN_PASS_COST_ITER_DEEPSEEK_RESEARCH.md"
OUT_RAW = OUT_MD.with_suffix(".raw.json")

CONTEXT = (
    "Context: SDL_GPU (SDL3, Vulkan backend) renderer on Intel HD 520 (Gen9, Mesa ANV), GPU clock floats "
    "300-1000 MHz, ~1366x706. A heightmap terrain G-buffer pass (instanced 16x16-quad tiles, ~2M triangles, "
    "vertex shader fetches height/normal textures) writes RGBA32F (position + packed normal) with a trivial "
    "fragment shader and its own D32_FLOAT depth; then depth is copied to the scene depth; a fullscreen resolve "
    "pass samples both. The test view is mostly sky with a band of terrain. FACTS (fence-synced GPU ms, ABBA): "
    "(1) terrain pass costs ~11.5 ms of a ~33 ms frame; (2) triangle DENSITY has no detectable effect: "
    "flat LOD depth 3->2, GEOCLIPMAP at ~206K triangles, and a sweep 3328->352 nodes all within +-4% noise "
    "(closed, do not propose LOD/clipmap/triangle reduction); (3) scissor 1x1 on the pass (all vertices still "
    "processed, no pixels) -4.7 ms; (4) drawing zero nodes -11.5 ms; (5) constant-output fragment shader "
    "-2.0 ms; (6) RGBA8 instead of RGBA32F target ~0; (7) drawing the same nodes a SECOND time into the same "
    "pass (every pixel fails early depth test, no FS, no colour write) +6.5 ms; (8) node-data upload ~0, depth "
    "copy ~0.4 ms; (9) a separate depth-only terrain pass into the shared depth costs +7.1 ms. Possible "
    "confound: DVFS (clock not pinned). WORKING HYPOTHESES to test or refute: A) cost scales with covered "
    "PIXEL FRAGMENTS including heavy overdraw of a heightfield seen at grazing angles (independent of triangle "
    "density), with nodes submitted in row-major zone order, not front-to-back; B) Gen9 depth/HiZ behaviour "
    "(D32_FLOAT, HiZ resolve/depth resolve when the depth texture is later sampled, depth clear); C) per-pass "
    "fixed costs from SDL_GPU barriers/layout transitions on Gen9 ANV; D) pixel-shader dispatch inefficiency "
    "for small/thin triangles (SIMD8, 2x2 quads). TARGET: ~1.1 ms. We want the real mechanism and the best "
    "fix, each with real-code or driver-source evidence and a CHEAP discriminating experiment. "
)

SEED = {
    "S1": ("Overdraw of heightfield terrain and draw order (front-to-back) on early-Z/HiZ hardware",
           "Open-source engines/papers/blogs measuring or fixing OVERDRAW of terrain seen at grazing angles: "
           "sorting terrain tiles front-to-back, depth pre-pass trade-offs, HiZ behaviour, how much a "
           "row-major (unsorted) draw order costs vs sorted, per-tile distance sorting in the engine (Terrain3D, "
           "O3DE, OpenMW, Godot, Wicked, bevy_terrain). Repo + files with the sort/order code and any numbers."),
    "S2": ("Intel Gen9 (ANV/i965) depth: HiZ, depth resolve, D32_FLOAT, depth clear costs",
           "Mesa ANV/iris source and docs: HiZ ops (HIZ_OP_DEPTH_RESOLVE, HIZ_OP_HIZ_RESOLVE), when they run "
           "(depth texture later sampled by a shader, layout transitions), cost of D32_FLOAT vs D16/D24 on Gen9, "
           "depth clear/fast clear, Z compression. Give exact files/functions in src/intel/vulkan and "
           "src/intel/isl and what triggers a resolve."),
    "S3": ("SDL_GPU Vulkan backend barriers and layout transitions per render pass",
           "libsdl-org/SDL src/gpu/vulkan/SDL_gpu_vulkan.c: how barriers/image layout transitions are issued "
           "when a texture is a render target then sampled (and depth textures), whether pipeline barriers are "
           "conservative (ALL_COMMANDS), per-pass fixed overhead, known performance issues on Intel/Mesa in the "
           "SDL issue tracker. Exact functions and the barrier flags used."),
    "S4": ("Pixel shader dispatch and early-Z rejection throughput on Intel Gen9",
           "Gen9 rasterizer/pixel-dispatch characteristics: pixels per clock for early-Z rejected vs shaded "
           "fragments, cost of thin/small triangles (2x2 quad waste, SIMD8/16/32 dispatch), why a pass that "
           "rejects every pixel by depth can still cost milliseconds, Intel optimization guides and Mesa "
           "comments/issues. Quote numbers/conclusions."),
    "S5": ("Render-pass boundary and render-target format/clear costs on Gen9 tile-less IMR",
           "Costs of starting/ending a render pass on Intel integrated GPUs (cache flushes, PIPE_CONTROL "
           "stalls, render-target cache flush, CCS/MCS resolves), clearing RGBA32F and D32 targets at ~1366x706, "
           "load/store ops; how engines minimise pass count on iGPUs. Repo + files / driver code / guide."),
    "S6": ("Discriminating experiments: separating overdraw, Z-test, FS dispatch and pass overhead",
           "Published methodology to separate raster/Z/early-Z/overdraw cost from vertex/FS cost on a GPU "
           "pass using only ablations (scissor, rasterizerDiscard, colour write mask off, depth func ALWAYS/"
           "NEVER/EQUAL, front-to-back vs back-to-front, overdraw heat map via additive blend/stencil count). "
           "Concrete recipes and which Vulkan states to flip. Repo + files / blog."),
}

EVAL = (
    CONTEXT + "\n\nBelow is ALL evidence gathered so far: previous evaluation (if any), (A) recalled answers "
    "(may be wrong) and (B) analyses of REAL code/driver sources. Tasks: 1) State the MECHANISM most consistent "
    "with ALL nine facts (explain each fact, flag which hypotheses A-D survive and which are refuted) and "
    "your confidence. 2) The BEST FIX for it with evidence rows and a rough effect estimate on our numbers. "
    "3) The cheapest DISCRIMINATING experiment(s) (concrete Vulkan/SDL_GPU state flips) that would confirm "
    "or kill the leading hypothesis. 4) Is the conclusion STABLE (would more search plausibly change it)? "
    "5) Up to 6 follow-up queries targeting the remaining gaps (name concrete repos/files/papers). Finish "
    "with ONE fenced ```json block: "
    '{"best": "...", "stable": true|false, "followups": [{"id": "F1", "title": "...", "prompt": "..."}]}\n\n'
)


def run_q(key, qid, title, ask):
    prompt = (CONTEXT + "\n\nTask " + qid + ": " + title + "\n" + ask +
              "\n\nFormat: a short list; each item = repo, license, files/functions, what it does, confidence "
              "(HIGH/MED/LOW). End with a section 'Not found / unknown'.")
    text, finish, usage = base.call_deepseek(key, prompt)
    return qid, {"title": title, "answer": text, "finish": finish, "usage": usage}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=40)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--deep-limit", type=int, default=35)
    args = ap.parse_args()
    key = read_api_key()
    t0 = time.time()
    results, deep, verdict, evals = {}, [], {}, []
    seen_files = set()
    queue = dict(SEED)
    stable_streak = 0
    for rnd in range(1, args.rounds + 1):
        print(f"=== round {rnd}: {len(queue)} queries ===", flush=True)
        with ThreadPoolExecutor(max_workers=args.workers) as ex:
            futs = {ex.submit(run_q, key, q, t, p): q for q, (t, p) in queue.items()}
            for f in as_completed(futs):
                q = futs[f]
                try:
                    _, r = f.result(); r["round"] = rnd; results[q] = r
                    print(f"[{q}] done {time.time()-t0:.0f}s finish={r['finish']}", flush=True)
                except Exception as e:
                    results[q] = {"title": queue[q][0], "answer": f"(FAILED: {e})", "finish": "error", "usage": {}, "round": rnd}
                    print(f"[{q}] FAILED {e}", flush=True)
        claims, repos = set(), set()
        for q in queue:
            c, rp = base.extract_claims(results[q]["answer"]); claims.update(c); repos.update(rp)
        for a, b, v in base.verify(sorted(claims)):
            verdict[(a, b)] = v
        targets = [(r, p) for (r, p), v in verdict.items() if v == "VERIFIED" and (r, p) not in seen_files]
        for repo in sorted({r for r, _ in targets} | repos):
            tree = base.repo_tree(repo)
            if tree:
                for p in base.keyword_files(repo, tree, 6):
                    if (repo, p) not in seen_files and (repo, p) not in targets:
                        targets.append((repo, p))
        targets = targets[: args.deep_limit]
        seen_files.update(targets)
        print(f"round {rnd}: verified {sum(1 for v in verdict.values() if v=='VERIFIED')}/{len(verdict)}; L3 on {len(targets)} files", flush=True)
        with ThreadPoolExecutor(max_workers=args.workers) as ex:
            for f in [ex.submit(base.deep_one, key, r, p) for r, p in targets]:
                try: deep.append(f.result())
                except Exception as e: print("deep fail", e, flush=True)
        l1 = "\n\n".join(f"[{q}] {r['title']}\n{r['answer'][:4000]}" for q, r in results.items() if r.get("round", 0) >= rnd - 1)
        l3 = "\n\n".join(f"### {r}:{p}\n{a[:2000]}" for r, p, a in deep if "NOT RELEVANT" not in a[:200])[-150000:]
        prev = ("=== PREVIOUS EVALUATION ===\n" + evals[-1][:12000] + "\n\n") if evals else ""
        text, _, _ = base.call_deepseek(key, EVAL + prev + "=== (A) ===\n" + l1 + "\n\n=== (B) ===\n" + l3)
        evals.append(text)
        OUT_RAW.write_text(json.dumps({"L1": results, "L3": deep, "EVALS": evals,
                                       "verdict": {f"{a}:{b}": v for (a, b), v in verdict.items()}}, ensure_ascii=False, indent=1))
        m = re.findall(r"```json\s*(\{.*?\})\s*```", text, re.S)
        meta = {}
        if m:
            try: meta = json.loads(m[-1])
            except Exception: meta = {}
        print(f"round {rnd} best: {str(meta.get('best'))[:160]} stable={meta.get('stable')}", flush=True)
        stable_streak = stable_streak + 1 if meta.get("stable") else 0
        fu = meta.get("followups") or []
        if stable_streak >= 3 or not fu or rnd == args.rounds:
            break
        queue = {f"R{rnd}{x.get('id','F')}": (x.get("title", "follow-up"), x.get("prompt", "")) for x in fu[:6] if x.get("prompt")}

    md = ["# Terrain: deep iterative search for the best alternative (DeepSeek, machine-verified paths)\n",
          "Generated by `tools/research/deepseek_terrain_deep_iter.py`. Model text is UNTRUSTED; only "
          "VERIFIED paths were confirmed to exist via `gh api`.\n",
          f"**Paths: {len(verdict)} claimed, {sum(1 for v in verdict.values() if v=='VERIFIED')} VERIFIED; rounds run: {len(evals)}**\n"]
    for i, e in enumerate(evals, 1):
        md.append(f"\n---\n## Evaluation after round {i}\n{e}\n")
    md.append("\n---\n## Verified paths\n")
    md += [f"- `{a}` : `{b}`" for (a, b), v in sorted(verdict.items()) if v == "VERIFIED"]
    md.append("\n## L3 deep dive (real code)\n")
    for r, p, a in sorted(deep):
        if "NOT RELEVANT" not in a[:200]:
            md.append(f"### `{r}:{p}`\n{a}\n")
    for q, r in results.items():
        md.append(f"\n---\n## L1 / {q} (round {r.get('round')}). {r['title']}\n{r['answer']}\n")
    OUT_MD.write_text("\n".join(md) + "\n")
    print(f"wrote {OUT_MD}", flush=True)


if __name__ == "__main__":
    main()
