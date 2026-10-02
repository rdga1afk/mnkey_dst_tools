#!/usr/bin/env python3
"""
deepseek_terrain_deep_iter.py -- ROUND 3: iterative deep search for the BEST terrain
alternative for a ~1.1 ms G-buffer on Intel Gen9, until the best candidate stabilises.

Each round: L1 queries (deepseek-v4-pro, reasoning_effort=max) -> verify claimed paths via
`gh api` -> L3 deep dive on real code (new files only) -> evaluation call that names the
current best candidate, gaps, and generates follow-up queries (JSON). Stops when the model
reports a stable best candidate for 2 consecutive rounds or --rounds is reached.
Output: docs/research/TERRAIN_DEEP_ITER_DEEPSEEK_RESEARCH.md (+ .raw.json, saved every round).

Usage: python3 tools/research/deepseek_terrain_deep_iter.py [--rounds 4] [--workers 4] [--deep-limit 35]
"""
import argparse, json, re, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deepseek_terrain_alternatives_search as base  # noqa: E402
from _deepseek_common import read_api_key  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT_MD = ROOT / "docs/research/TERRAIN_DEEP_ITER_DEEPSEEK_RESEARCH.md"
OUT_RAW = OUT_MD.with_suffix(".raw.json")

CONTEXT = base.CONTEXT + (
    "ROUND 2 FINDINGS: candidate directions were (1) pre-baked quantized per-tile vertex/index buffers "
    "(Cesium QuantizedMeshTerrainData), (2) camera-centered snapped clipmap with constant triangle budget "
    "(Terrain3D terrain_3d.cpp), (3) offline meshlet LOD with locked borders (Bevy). Dead ends: fullscreen "
    "ray-march, depth-only+reconstruction, software occluders. Suspected measurement confounds: DVFS, "
    "per-vertex texelFetch of per-instance data. We want the single BEST alternative (or combination) "
    "with real-code evidence and a cost model, and a way to check it cheaply. "
)

SEED = {
    "S1": ("OpenMW terrain: a real open-world RPG terrain on weak GPUs",
           "OpenMW (components/terrain, apps/openmw/mwrender): quadtree chunks, per-chunk vertex buffers, "
           "LOD by distance, index-buffer stitching, composite maps, view-data culling. Give exact files, how "
           "many vertices per chunk, how cracks between LODs are handled, and anything stated about cost."),
    "S2": ("O3DE Terrain Gem and Wicked/Flax landscape: sector meshes and LOD",
           "O3DE Terrain (Gems/Terrain: TerrainMeshManager, sectors, LOD distance), Wicked Engine terrain, "
           "Flax Landscape: how sector/chunk meshes are built, whether heights come from vertex buffers or "
           "VS texture fetch, how LOD seams are fixed, per-sector vertex counts. Files and key constants."),
    "S3": ("0 A.D. / Spring / other open RTS-RPG terrain with CPU-baked patch VBOs",
           "0 A.D. patch rendering (PatchRData, CPatch), Spring/Recoil map drawer, others that bake per-patch "
           "vertex buffers and do LOD with index stitching: patch size, vertex format (quantised?), how many "
           "patches/triangles per frame, measured performance on integrated GPUs."),
    "S4": ("CDLOD vertex-shader geomorph snapping instead of stitched indices",
           "Open-source CDLOD / geomorph terrains where the VERTEX SHADER snaps edge vertices to the coarser "
           "level (no index variants, no skirts): shader files, the morph formula, per-node uniform data, node "
           "counts, and published cost. Include Strugar's CDLOD sample and any ports."),
    "S5": ("Gen9/ANV vertex stage: instance data via texture vs vertex attribute divisor",
           "Evidence (Mesa/ANV issues, Intel docs, repos with measurements) on cost of per-vertex texelFetch of "
           "per-instance data in the vertex shader vs instance-rate vertex attributes or push constants on "
           "Intel Gen9; vertex cache behaviour for 17x17 instanced grids; primitive/clip rate limits; VS texture "
           "fetch latency. Give numbers or quote conclusions."),
    "S6": ("Terrain triangle budgets in shipped open-world games on iGPUs",
           "Published triangle/vertex budgets and ms costs for terrain in shipped open-world games and open "
           "engines at 720p-1080p on integrated GPUs or consoles with similar throughput (talks, postmortems, "
           "repos). What terrain budget did they target (ms and triangles) and what techniques achieved it?"),
}

EVAL = (
    CONTEXT + "\n\nBelow is ALL evidence gathered so far: (A) recalled answers (may be wrong) and (B) analyses of "
    "REAL code. Tasks: 1) Name the CURRENT BEST alternative (or ordered combination) for getting terrain G-buffer "
    "cost near ~1.1 ms on Gen9, with the real-code evidence rows supporting it and a rough cost model using OUR "
    "measured numbers (vertex ~6.5 ms at 2M tris, pixel ~4.7 ms incl. ~2.7 ms raster floor). 2) State gaps: "
    "what is still unverified and would change the answer. 3) Is the best candidate STABLE (would more search "
    "plausibly change it)? 4) Produce up to 6 follow-up queries targeted at the gaps (each must name concrete "
    "repos/engines/papers to look at). Finish with ONE fenced ```json block: "
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
    ap.add_argument("--rounds", type=int, default=4)
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
        l1 = "\n\n".join(f"[{q}] {r['title']}\n{r['answer'][:5000]}" for q, r in results.items())
        l3 = "\n\n".join(f"### {r}:{p}\n{a[:2200]}" for r, p, a in deep if "NOT RELEVANT" not in a[:200])
        text, _, _ = base.call_deepseek(key, EVAL + "=== (A) ===\n" + l1 + "\n\n=== (B) ===\n" + l3)
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
        if stable_streak >= 2 or not fu or rnd == args.rounds:
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
