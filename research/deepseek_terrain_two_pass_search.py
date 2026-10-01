#!/usr/bin/env python3
"""
deepseek_terrain_two_pass_search.py -- broad open-source prior-art search:
how do public engines/renderers split TERRAIN geometry across passes
(depth prepass vs G-buffer / visibility buffer / forward), with separate vs
shared depth buffers, and what do they do when terrain is vertex/primitive
bound on weak integrated GPUs.

Layers:
  L1 discovery   - N focused DeepSeek (deepseek-v4-pro, reasoning_effort=max) queries, parallel.
  L2 verify      - every GitHub path/URL a model claims is checked against the
                   real repo tree via `gh api` (models invent paths).
  L3 deep dive   - real source of verified files (+ files found by keyword in
                   the real trees of verified repos) is fetched and the model
                   analyses THAT code only, with quoted evidence.
  L4 synthesis   - cross-check L1 claims vs L3 evidence, comparison table and
                   an answer for our architecture; contradictions flagged.
Raw model answers are kept next to the report.

Usage:
  python3 tools/research/deepseek_terrain_two_pass_search.py [--workers 4] [--only A,C] [--deep-limit 30] [--no-deep]
Needs: DEEPSEEK_API_KEY (or the key file used by _deepseek_common) and `gh` logged in.
Output: docs/research/TERRAIN_TWO_PASS_OPEN_SOURCE_DEEPSEEK_RESEARCH.md (+ .raw.json)
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
OUT_MD = ROOT / "docs/research/TERRAIN_TWO_PASS_OPEN_SOURCE_DEEPSEEK_RESEARCH.md"
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
    "Context: a heightmap terrain renderer (SDL_GPU/Vulkan, Intel HD 520 "
    "iGPU, 1080p). Terrain is drawn as instanced grid patches. Pipeline: "
    "terrain G-buffer pass writes only world position + normal (cheap "
    "fragment shader) into its OWN depth buffer; a separate full-screen "
    "resolve pass does the texturing/lighting. Historically a second "
    "depth-only terrain pass wrote terrain depth into the shared scene depth "
    "buffer (so NPCs behind hills are rejected) -- the two passes did not "
    "share depth. Terrain G-buffer cost is large and dominated by geometry "
    "(vertex/primitive) work, not pixel shading. "
)

QUERIES = {
    "A": (
        "Terrain depth prepass + deferred G-buffer in open source",
        "List open-source engines/renderers (any language/API) where terrain "
        "is rendered in a depth-only prepass and then again in a deferred "
        "G-buffer pass. For each: where the terrain draw is issued in both "
        "passes, whether they share ONE depth buffer, depth compare used in "
        "the second pass (EQUAL / LESS_EQUAL / GREATER_EQUAL), and the files.",
    ),
    "B": (
        "Separate depth buffer for the G-buffer pass vs the scene depth",
        "Find any open-source renderer where the G-buffer (or a terrain "
        "offscreen pass) has its OWN depth attachment distinct from the main "
        "scene depth, with a later copy/resolve/composite into scene depth "
        "(e.g. depth copy, depth resolve, depth blit, 'terrain composited "
        "with depth'). Explain why each did it and the cost they report.",
    ),
    "C": (
        "Decoupled / screen-space terrain shading (G-buffer positions only)",
        "Find public implementations (open source or detailed public talks "
        "with source) of terrain where the geometry pass stores only "
        "position/normal/IDs and a later screen-space pass does material "
        "blending: visibility-buffer terrain, 'terrain material resolve', "
        "decoupled shading, projected-decal-per-tile terrain (Total War), "
        "Far Cry 5 / Frostbite / Horizon terrain talks and any open "
        "reimplementation. Give repo+file for code, talk title+year otherwise.",
    ),
    "D": (
        "Terrain implementations in open engines: exact pass structure",
        "For each of: Godot 4 (Terrain3D, HTerrain, built-in), O3DE Terrain "
        "Gem, Wicked Engine, Flax Engine landscape, Stride, Bevy "
        "(bevy_terrain), OGRE-Next Terra, Lumix Engine, Diligent samples, "
        "Granite (Themaister), The Forge, Falcor, Unreal-like open clones "
        "(Piccolo, Overload, Hazel): state precisely the passes terrain goes "
        "through (prepass? shadow? gbuffer? forward? visibility buffer?), and "
        "the files. Skip any you do not actually know.",
    ),
    "E": (
        "Is a terrain depth prepass worth it when the main pass is cheap / VS-bound?",
        "Find public evidence (blog posts, GDC/SIGGRAPH slides, forum threads "
        "with measurements, engine docs, source comments) on whether a depth "
        "prepass pays off when the following pass has a trivial fragment "
        "shader or when the workload is vertex/primitive-bound, especially on "
        "tile-less integrated GPUs (Intel Gen9, AMD APUs). Quote the "
        "conclusion and the measured numbers if they exist.",
    ),
    "F": (
        "Reducing geometry cost of distant terrain (sub-pixel triangles)",
        "What do open-source terrain renderers do about distant terrain "
        "producing sub-pixel / quad-overshading triangles when density is "
        "uniform: distance LOD, geomorphing, clipmaps, GPU-driven culling, "
        "tessellation, mesh shaders, depth-based occlusion/HZB culling of "
        "patches, ray-marched terrain at distance? For each give repo+files "
        "and the reported speedup if any.",
    ),
    "G": (
        "Instanced patch terrain: draw-call/vertex-fetch tricks",
        "Open-source terrain that draws many fixed-size patches with "
        "instancing and no vertex buffer (vertex pulled from heightmap in the "
        "vertex shader). Cover vertex-shader texture-fetch cost on weak GPUs, "
        "index-buffer reuse, per-node data textures/SSBOs, indirect draws, "
        "and any documented pitfalls. Repo + files.",
    ),
    "H": (
        "Terrain depth prepass for NPC/object occlusion specifically",
        "Open-source games/engines that render terrain depth early specifically "
        "so objects/NPCs behind hills are culled (depth prepass, software "
        "occlusion culling with a terrain depth/HiZ, Masked Occlusion Culling "
        "with terrain occluders, hierarchical-Z built from terrain). Repo + "
        "files, and whether they rasterize terrain a second time or reuse the "
        "G-buffer depth (depth copy).",
    ),
}


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
    "Below is REAL source code from a public repository ({repo}:{path}). "
    "Using ONLY this code (not memory), answer for OUR question: how does this code "
    "handle terrain/ground geometry across passes? Report: (1) which pass(es)/draw lists "
    "this file participates in (depth prepass / shadow / G-buffer / forward / visibility); "
    "(2) does it draw terrain more than once per frame, and into which depth attachment(s) "
    "- one shared depth buffer or separate ones; (3) depth compare/stencil/depth-write "
    "state if shown; (4) anything about LOD/instancing/culling that reduces geometry cost; "
    "(5) QUOTE up to 3 short snippets (max 2 lines each) as evidence. If the file does not "
    "answer these, say exactly 'NOT RELEVANT' and why in one line. Do not guess beyond the code.\n\n"
    "=== {repo}:{path} ===\n{code}\n"
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
           "(B) layer-3 analyses of REAL code. Produce: 1) a comparison table (implementation | "
           "terrain passes | shared vs separate depth | terrain drawn how many times | evidence file). "
           "Include ONLY rows supported by (B). 2) A list of layer-1 claims CONTRADICTED or "
           "unsupported by (B). 3) Whether ANY real code shows two independent depth buffers for "
           "terrain prepass vs G-buffer; if none, say so. 4) Concrete, ranked recommendations for "
           "our architecture given a cheap G-buffer fragment shader and geometry-bound cost on an "
           "Intel iGPU, each tied to evidence rows. 5) Open questions that need measurement.\n\n"
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

    md = ["# Terrain two-pass / depth-prepass: open-source prior art (DeepSeek search, machine-verified paths)\n",
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
