#!/usr/bin/env python3
"""
deepseek_terrain_opt_compare_search.py -- prior-art search to COMPARE our terrain
cost-reduction methods with alternatives used by public engines/papers.
Our G-buffer is ~11.5 ms (vertex ~6.5 + pixel ~4.7) vs a target ~1.1 ms.

Layers (same as deepseek_terrain_two_pass_search.py): L1 discovery (deepseek-v4-pro,
reasoning_effort=max), L2 path verification via `gh api`, L3 deep dive on real source,
L4 synthesis (comparison of OUR methods vs alternatives, ranked, with evidence).

Usage:
  python3 tools/research/deepseek_terrain_opt_compare_search.py [--workers 4] [--only A,C] [--deep-limit 30] [--no-deep]
Needs: DEEPSEEK_API_KEY (or the key file used by _deepseek_common) and `gh` logged in.
Output: docs/research/TERRAIN_OPT_COMPARE_DEEPSEEK_RESEARCH.md (+ .raw.json)
"""
import argparse
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-v4-pro"   # /models (2026-10-02): v4-pro + deepseek-flash; "deepseek-reasoner" is a legacy alias
REASONING_EFFORT = "max"    # real param: reasoning_effort (output_config.effort is silently ignored)
MAX_TOKENS = 200000          # includes reasoning tokens; model limit is 393216
ROOT = Path(__file__).resolve().parents[2]
OUT_MD = ROOT / "docs/research/TERRAIN_OPT_COMPARE_DEEPSEEK_RESEARCH.md"
OUT_RAW = OUT_MD.with_suffix(".raw.json")

SYSTEM_PROMPT = (
    "You are a graphics-engine researcher with deep knowledge of public, "
    "open-source renderers and published talks. Answer ONLY with things you "
    "are confident exist. For every implementation you cite give: repository "
    "(owner/name), license if known, the exact file path(s) and function or "
    "shader names, and one sentence on what that code does. If you are not "
    "sure a path exists, write 'UNVERIFIED' next to it -- do NOT invent "
    "paths, URLs, or line numbers. Prefer fewer, correct citations over many "
    "plausible ones. Say plainly when you know of no such implementation."
)

CONTEXT = (
    "Context: a heightmap terrain renderer (SDL_GPU/Vulkan, Intel HD 520 Gen9 iGPU, "
    "1366x706 test view, 60 FPS goal). Our CURRENT method: uniform-density instanced grid "
    "tiles (57.6 m tiles of 16x16 quads = 3.6 m quads, same density everywhere, 3840 visible "
    "nodes ~ 2M triangles), no vertex buffer (vertex pulled from 8193^2 heightmap + normal "
    "textures, mip 0 only, in the vertex shader), one shared index buffer, skirts. A G-buffer "
    "pass writes RGBA32F world position + packed normal with a trivial fragment shader into "
    "its own depth buffer, then depth is copied to the scene depth; a fullscreen resolve pass "
    "does texturing/lighting (~2.5 ms). MEASURED (fence-synced GPU ms, ABBA): terrain G-buffer "
    "~11.5 ms = vertex/primitive part ~6.5 ms (proportional to node count; drawing the same "
    "nodes a second time with all pixels early-Z rejected costs +6.5 ms) + pixel part ~4.7 ms "
    "(constant fragment shader -2.0 ms; RGBA8 instead of RGBA32F target ~0 ms; the remaining "
    "~2.7 ms is rasterization of small triangles / quad overshading / covered-pixel writes). "
    "Target: ~1.1 ms. Candidate ideas of ours: (S1) screen-space-error quadtree LOD with skirts "
    "(Cesium style), (S2) mip chain of height/normal sampled by LOD in the VS, (S7) depth-only "
    "terrain into the shared depth with world position/normal reconstructed in the resolve "
    "pass (no G-buffer colour, no FS, no depth copy), (S3) terrain as occluder in software "
    "occlusion culling. A second independent depth-prepass was measured and rejected (+7.1 ms). "
)

QUERIES = {'A': ('Depth-only terrain + position/normal reconstruction in the shading pass', 'Find open-source renderers/engines or detailed public talks where terrain (or any geometry) is rendered depth-only and the deferred/resolve pass reconstructs world position from depth + inverse view-projection and normals from a heightmap or derivatives, instead of writing a position G-buffer. Report precision problems at long range (D32 float, reversed-Z), how skirts/special pixels are identified, and reported costs vs a position G-buffer. Repo + files, or talk + year.'), 'B': ('Screen-space-error terrain LOD and typical triangle budgets', 'Open-source terrain with LOD by screen-space error (Cesium 3D Tiles/CesiumJS, CDLOD, Chunked LOD / Ulrich, geoclipmaps, Spring/Recoil, osgTerrain, Godot Terrain3D, Unreal Landscape). For each: error threshold in pixels, node/triangle counts per frame, crack handling (skirts vs stitching vs morph), and reported GPU cost, especially on integrated GPUs. Repo + files.'), 'C': ('Published per-frame GPU cost of terrain on integrated GPUs', 'Find any published measurements (blogs, papers, slides, issue threads, benchmarks in repos) of terrain G-buffer/depth/shading cost in milliseconds on Intel integrated GPUs (HD 520/620/Iris), AMD APUs or Steam Deck at ~1080p, and what triangle counts that corresponds to. Include what budget ms they considered normal for terrain.'), 'D': ('Quad overshading and triangle-size targets for terrain', 'What minimum triangle screen size do terrain renderers target to avoid quad overshading and primitive-rate limits (e.g. 8-16 px per edge), how is it enforced (LOD selection, geometry clipmaps, tessellation factor), and what speedups were reported when moving from uniform dense grids to pixel-matched density. Repo + files / talk + year.'), 'E': ('GPU-driven terrain on hardware without mesh shaders (Gen9)', 'Open-source terrain using compute culling, indirect draw, HZB occlusion of patches, or visibility buffers on hardware without mesh shaders; which of these reduce vertex-bound cost for ~1-4k patches; known Intel Gen9/ANV pitfalls (indirect draw, storage buffers in VS, vertex texture fetch). Repo + files.'), 'F': ('Ray-marched / heightfield-raycast terrain at distance vs triangle meshes', 'Open-source or published techniques that replace far terrain triangles with heightfield ray-marching (min/max mip pyramids, relief mapping, cone-step, impostors), hybrid near-mesh/far-raymarch, and measured cost vs meshes at ~1080p on weak GPUs. Repo + files / paper.'), 'G': ('Vertex-texture-fetch cost vs baked vertex data for terrain meshes', 'For terrain whose vertices are fetched from a heightmap in the vertex shader on weak GPUs: reported cost of VTF (mip 0 vs mip chain, two fetches for height+normal), versus pre-baked per-node vertex buffers / compressed meshes / geoclipmap vertex buffers; caches, bandwidth, and measured differences. Repo + files / talk.'), 'H': ('Reduced-resolution or temporally reused terrain G-buffer', 'Techniques that render terrain geometry/G-buffer at reduced resolution, checkerboard, or reuse previous-frame terrain via reprojection/caching (Far Cry 5, Horizon, Total War terrain, Frostbite, sparse/cached terrain tiles), including artifacts on cliffs/edges and measured gains. Repo + files / talk + year.')}


def call_deepseek(api_key, user_prompt):
    body = json.dumps({
        "model": MODEL,
        "messages": [{"role": "system", "content": SYSTEM_PROMPT},
                     {"role": "user", "content": user_prompt}],
        "max_tokens": MAX_TOKENS,
        "reasoning_effort": REASONING_EFFORT,
    }).encode()
    req = urllib.request.Request(API_URL, data=body, method="POST", headers={
        "Authorization": f"Bearer {api_key}", "Content-Type": "application/json"})
    last = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=3600) as resp:
                data = json.loads(resp.read())
                ch = data["choices"][0]
                text = ch["message"]["content"].strip()
                if not text:
                    raise RuntimeError(f"empty (finish={ch.get('finish_reason')})")
                return text, ch.get("finish_reason"), data.get("usage", {})
        except urllib.error.HTTPError as e:
            last = f"HTTP {e.code}: {e.read().decode(errors='replace')[:300]}"
            if e.code in (429, 500, 502, 503):
                time.sleep(15 * (attempt + 1)); continue
            break
        except Exception as e:  # network/timeout
            last = str(e); time.sleep(5 * (attempt + 1))
    raise RuntimeError(f"DeepSeek failed: {last}")


def run_query(api_key, qid):
    title, ask = QUERIES[qid]
    prompt = (CONTEXT + "\n\nTask " + qid + ": " + title + "\n" + ask +
              "\n\nFormat: a short list; each item = repo, license, files/"
              "functions, what it does, confidence (HIGH/MED/LOW). End with a "
              "section 'Not found / unknown'.")
    text, finish, usage = call_deepseek(api_key, prompt)
    return qid, {"title": title, "answer": text, "finish": finish, "usage": usage}


# ── verification of claimed GitHub paths ──────────────────────────────────────
_tree_cache = {}


def repo_tree(repo):
    if repo in _tree_cache:
        return _tree_cache[repo]
    paths = None
    try:
        out = subprocess.run(
            ["gh", "api", f"repos/{repo}/git/trees/HEAD?recursive=1", "--jq", ".tree[].path"],
            capture_output=True, text=True, timeout=120)
        if out.returncode == 0:
            paths = set(out.stdout.splitlines())
    except Exception:
        pass
    _tree_cache[repo] = paths  # None = repo missing / API failure
    return paths


URL_RE = re.compile(r"github\.com/([\w.-]+/[\w.-]+)/(?:blob|tree)/[\w./-]+?/((?:[\w.@+-]+/)*[\w.@+-]+\.[\w]+)")
REPOFILE_RE = re.compile(r"([\w.-]+/[\w.-]+)[\s:`*]+((?:[\w.@+-]+/)+[\w.@+-]+\.(?:cpp|c|h|hpp|hlsl|glsl|vert|frag|azsl|azsli|shader|wgsl|rs|cs|gd|py|slang|comp|md|txt))")


REPO_TOKEN = re.compile(r"`([\w.-]+/[\w.-]+)`")
PATH_TOKEN = re.compile(r"`((?:[\w.@+\- ]+/)+[\w.@+\-]+\.[A-Za-z0-9]{1,6})`")


def extract_claims(text):
    """Returns (claims, repos): claims=(repo,path) pairs; repos=every repo named in 'Repo:' lines/headings."""
    claims, repos = set(), set()
    cur = None
    for line in text.splitlines():
        low = line.lower()
        m = REPO_TOKEN.search(line)
        if m and ("repo" in low or line.lstrip().startswith("#")) and "." not in m.group(1).split("/")[-1][-4:-3] * 0:
            cand = m.group(1)
            if not PATH_TOKEN.fullmatch("`" + cand + "`"):  # a repo, not a file path
                cur = cand
                repos.add(cur)
        for pm in PATH_TOKEN.finditer(line):
            path = pm.group(1).strip()
            if cur and path != cur:
                claims.add((cur, path))
    for m in URL_RE.finditer(text):
        claims.add((m.group(1), m.group(2)))
        repos.add(m.group(1))
    for m in REPOFILE_RE.finditer(text):
        repo, path = m.group(1), m.group(2)
        if repo.count("/") == 1 and not repo.startswith(("http", "www")):
            claims.add((repo, path))
    return sorted(claims), sorted(repos)


def verify(claims):
    rows = []
    for repo, path in claims:
        tree = repo_tree(repo)
        if tree is None:
            rows.append((repo, path, "REPO-NOT-FOUND"))
        elif path in tree:
            rows.append((repo, path, "VERIFIED"))
        else:
            tail = path.split("/")[-1]
            near = [p for p in tree if p.endswith("/" + tail) or p == tail][:2]
            rows.append((repo, path, "PATH-NOT-FOUND" + (f" (same filename at: {', '.join(near)})" if near else "")))
    return rows


# ── L3 deep dive ──────────────────────────────────────────────────────────────
KEYWORDS_PRIMARY = ("terrain", "landscape", "ground", "heightmap", "height_map", "clipmap")
KEYWORDS_PASS = ("depth", "prepass", "pre_pass", "gbuffer", "g_buffer", "deferred", "forward",
                 "visibility", "pass", "render", "shadow", "cull", "lod", "patch", "chunk")
CODE_EXT = (".cpp", ".cc", ".c", ".h", ".hpp", ".hlsl", ".hlsli", ".glsl", ".vert", ".frag", ".comp",
            ".azsl", ".azsli", ".shader", ".wgsl", ".rs", ".cs", ".gd", ".slang", ".json", ".pass")


def keyword_files(repo, tree, limit):
    scored = []
    for p in tree:
        low = p.lower()
        if not low.endswith(CODE_EXT):
            continue
        if any(x in low for x in ("/test", "/third_party", "/thirdparty", "/vendor/", "node_modules", "/docs/")):
            continue
        a = sum(1 for k in KEYWORDS_PRIMARY if k in low)
        b = sum(1 for k in KEYWORDS_PASS if k in low)
        if a and b:
            scored.append((a * 3 + b, p))
    scored.sort(key=lambda t: (-t[0], len(t[1])))
    return [p for _, p in scored[:limit]]


def fetch_file(repo, path, max_chars=60000):
    out = subprocess.run(["gh", "api", f"repos/{repo}/contents/{path}", "--jq", ".content"],
                         capture_output=True, text=True, timeout=120)
    if out.returncode != 0 or not out.stdout.strip():
        return None
    import base64
    try:
        txt = base64.b64decode(out.stdout).decode(errors="replace")
    except Exception:
        return None
    return txt[:max_chars] + ("\n... (truncated)" if len(txt) > max_chars else "")


DEEP_ASK = (
    "Below is REAL source code from a public repository ({repo}:{path}). Using ONLY this code "
    "(not memory), answer: what technique does it use to keep terrain rendering cost low "
    "(LOD selection / error metric, crack handling, culling, instancing, depth/position handling, "
    "reduced resolution, caching)? Report: (1) the technique and its key parameters/thresholds "
    "as written in code; (2) how many nodes/triangles it implies or budgets, if shown; (3) how "
    "depth/position is produced for shading (G-buffer vs reconstruct from depth); (4) QUOTE up to "
    "3 short snippets (max 2 lines each). If the file does not answer, say exactly 'NOT RELEVANT' "
    "and why in one line.\n\n=== {repo}:{path} ===\n{code}\n"
)


def deep_one(api_key, repo, path):
    code = fetch_file(repo, path)
    if not code:
        return repo, path, "(fetch failed)"
    text, _, _ = call_deepseek(api_key, DEEP_ASK.format(repo=repo, path=path, code=code))
    return repo, path, text


def synthesize(api_key, results, deep):
    l1 = "\n\n".join(f"[{q}] {r['title']}\n{r['answer'][:6000]}" for q, r in results.items())
    l3 = "\n\n".join(f"### {repo}:{path}\n{ans[:2500]}" for repo, path, ans in deep
                      if "NOT RELEVANT" not in ans[:200])
    ask = (CONTEXT + "\n\nYou are given (A) layer-1 recalled answers, which may contain errors, and "
           "(B) layer-3 analyses of REAL code. Produce: 1) a comparison table: technique | who uses it "
           "(evidence file) | what it removes (vertex / raster / FS / copy) | expected effect on OUR "
           "numbers (vertex 6.5 ms, pixel 4.7 ms) | risks. Include ONLY rows supported by (B), and a "
           "separate list of literature-only techniques from (A) marked UNVERIFIED. 2) Direct comparison "
           "of each of OUR candidate ideas (S1, S2, S7, S3) against the best alternatives found. 3) "
           "Layer-1 claims contradicted or unsupported by (B). 4) Is ~1.1 ms for terrain G-buffer "
           "plausible on an Intel Gen9 iGPU by ANY technique in evidence? State what the evidence says "
           "the realistic floor is. 5) Ranked recommendations tied to evidence rows. 6) Open questions "
           "that need measurement.\n\n"
           "=== (A) ===\n" + l1 + "\n\n=== (B) ===\n" + l3)
    text, _, _ = call_deepseek(api_key, ask)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--only", default="", help="comma list of query ids, e.g. A,C")
    ap.add_argument("--deep-limit", type=int, default=30, help="max files for layer-3 deep dive")
    ap.add_argument("--no-deep", action="store_true", help="skip layers 3-4")
    ap.add_argument("--reuse-l1", action="store_true", help="reuse saved L1 answers from the .raw.json (no new L1 API cost)")
    args = ap.parse_args()
    ids = [x for x in args.only.split(",") if x] or list(QUERIES)
    key = read_api_key()

    results = {}
    t0 = time.time()
    if args.reuse_l1 and OUT_RAW.exists():
        saved = json.loads(OUT_RAW.read_text())
        results = saved.get("L1", saved)
        ids = [q for q in ids if q in results]
        print(f"reusing saved L1 for {ids}", flush=True)
    todo = [q for q in ids if q not in results]
    with ThreadPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(run_query, key, q): q for q in todo}
        for f in as_completed(futs):
            q = futs[f]
            try:
                _, r = f.result()
                results[q] = r
                print(f"[{q}] done {time.time()-t0:.0f}s tokens={r['usage'].get('total_tokens')} finish={r['finish']}", flush=True)
            except Exception as e:
                results[q] = {"title": QUERIES[q][0], "answer": f"(FAILED: {e})", "finish": "error", "usage": {}}
                print(f"[{q}] FAILED {e}", flush=True)

    OUT_RAW.write_text(json.dumps({"L1": results}, ensure_ascii=False, indent=1))

    all_claims, all_repos = set(), set()
    per_q = {}
    for q, r in results.items():
        c, rp = extract_claims(r["answer"])
        per_q[q] = c
        all_claims.update(c)
        all_repos.update(rp)
    print(f"verifying {len(all_claims)} claimed paths ...", flush=True)
    verdict = {(a, b): v for a, b, v in verify(sorted(all_claims))}

    deep, synth = [], ""
    if not args.no_deep:
        targets = [(r, p) for (r, p), v in verdict.items() if v == "VERIFIED"]
        repos_ok = sorted({r for r, _ in targets} | set(all_repos))
        for repo in repos_ok:  # add files found by keyword in the REAL tree (every repo the models named)
            tree = repo_tree(repo)
            if tree:
                for p in keyword_files(repo, tree, 5):
                    if (repo, p) not in targets:
                        targets.append((repo, p))
        targets = targets[: args.deep_limit]
        print(f"L3 deep dive on {len(targets)} files from {len(repos_ok)} repos ...", flush=True)
        with ThreadPoolExecutor(max_workers=args.workers) as ex:
            futs = [ex.submit(deep_one, key, r, p) for r, p in targets]
            for f in as_completed(futs):
                try:
                    deep.append(f.result())
                except Exception as e:
                    print("deep fail", e, flush=True)
        print("L4 synthesis ...", flush=True)
        try:
            synth = synthesize(key, results, deep)
        except Exception as e:
            synth = f"(synthesis FAILED: {e})"
        OUT_RAW.write_text(json.dumps({"L1": results, "L3": deep, "L4": synth}, ensure_ascii=False, indent=1))

    md = ["# Terrain optimisation: our methods vs alternatives (DeepSeek search, machine-verified paths)\n",
          "Generated by `tools/research/deepseek_terrain_two_pass_search.py`. Model answers are "
          "UNTRUSTED text; only rows marked VERIFIED were confirmed to exist in the repo's "
          "default-branch tree via `gh api`. Existence of a file does NOT confirm the model's "
          "description of it.\n"]
    nver = sum(1 for v in verdict.values() if v == "VERIFIED")
    md.append(f"**Claimed paths: {len(verdict)} - VERIFIED: {nver} - not verified: {len(verdict)-nver}**\n")
    md.append("## Verified paths (all queries)\n")
    for (repo, path), v in sorted(verdict.items()):
        if v == "VERIFIED":
            md.append(f"- `{repo}` : `{path}`")
    md.append("\n## Claimed but NOT verified\n")
    for (repo, path), v in sorted(verdict.items()):
        if v != "VERIFIED":
            md.append(f"- `{repo}` : `{path}` -- {v}")
    if synth:
        md.append("\n---\n## L4. Synthesis (cross-checked against real code)\n")
        md.append(synth)
    if deep:
        md.append("\n---\n## L3. Deep dive: real code analysed\n")
        for repo, path, ans in sorted(deep):
            md.append(f"### `{repo}:{path}`\n{ans}\n")
    for q in ids:
        r = results[q]
        md.append(f"\n---\n## L1 / {q}. {r['title']}\n")
        qc = per_q.get(q, [])
        if qc:
            md.append("Paths cited here: " + ", ".join(
                f"`{a}:{b}` [{'ok' if verdict.get((a, b)) == 'VERIFIED' else 'UNVERIFIED'}]" for a, b in qc) + "\n")
        md.append(r["answer"])
    OUT_MD.write_text("\n".join(md) + "\n")
    print(f"wrote {OUT_MD} ({nver}/{len(verdict)} paths verified)")


if __name__ == "__main__":
    main()
