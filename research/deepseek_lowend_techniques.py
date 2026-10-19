#!/usr/bin/env python3
"""
deepseek_lowend_techniques.py -- #191: render techniques for weak iGPUs, one DeepSeek request per question.
Input  : only our own text -- the questions below and a short description of our frame taken from docs/research/PASS_COSTS_2026-10.md (sections 1-4, our measurements). No source files, no Kenshi data.
Output : docs/research/LOWEND_TECHNIQUES_2026-10.md (answers marked SOURCE-MODEL / CHECK: the model's claims are unverified).
Budget : hard limit in USD from the ACTUAL usage fields of every response; the constant prefix (system text + frame description) is byte-identical in all requests (cache hit), the question comes last.
Usage  : python3 tools/research/deepseek_lowend_techniques.py [--budget-usd 2] [--dry-run]
"""
import argparse
import datetime
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-v4-pro"
OUT = REPO / "docs" / "research" / "LOWEND_TECHNIQUES_2026-10.md"
PASS_COSTS = REPO / "docs" / "research" / "PASS_COSTS_2026-10.md"

SYSTEM = (
    "You are a senior real-time rendering engineer who ships open-world games on weak integrated GPUs. "
    "Answer concisely and concretely, in English. For every technique give: the idea; a known game or engine that used it (name it only if you are sure, otherwise say so); the expected gain; the risks for the image; the complexity for the frame described. "
    "Mark every factual claim you are not certain about with 'CHECK'. Do not invent paper titles, numbers or API names."
)

QUESTIONS = [
    ("1. Pre-blended terrain texture (virtual texturing / clipmap / adaptive VT)",
     "Pre-blended ground texture: virtual texturing, clipmaps, adaptive virtual textures (Rage / id Tech 5, Far Cry 4, Battlefield and others). Page sizes, how the cache is updated around the camera, memory on 2-4 GB of shared RAM of an iGPU. Our own earlier attempts at a texture-space ground cache cost as much to update as live shading (update cost ~ camera speed x texel count); say what the shipped systems did differently."),
    ("2. Temporal reuse / reprojection",
     "Temporal reuse and reprojection of shaded pixels, like our 'once per 8 / 16 frames' history. How others remove the artefacts: invalidation by motion, edge highlighting, different update periods for near and far ground, checkerboard / interleaved patterns, disocclusion handling."),
    ("3. Cheap lighting and fog on Intel Gen9",
     "Cheap lighting and fog on Intel Gen9: per-vertex evaluation, LUTs, half resolution, exp2-based fog, moving frame constants out of the pixel shader."),
    ("4. Gen9 specifics",
     "Intel Gen9 specifics: SIMD8 vs SIMD16 compilation, register pressure, spills, practical rules for fragment shaders (what makes the compiler fall back to SIMD8, how to keep GRF usage low, uniform-branch and loop costs, texture-fetch latency hiding)."),
    ("5. Other techniques for open worlds on weak iGPUs",
     "Other techniques for open worlds on weak iGPUs: impostors, HLOD, variable shading rate without hardware VRS (quad-based, checkerboard, interleaved), resolution scaling in motion, depth-based early-outs."),
]


def frame_description():
    text = PASS_COSTS.read_text()
    a = text.index("## 1.")
    b = text.index("## 5.") if "## 5." in text else len(text)
    return text[a:b].strip()


def call(key, system, user, max_tokens, effort="medium"):
    body = json.dumps({"model": MODEL, "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
                       "max_tokens": max_tokens, "reasoning_effort": effort}).encode()
    req = urllib.request.Request(API_URL, data=body, method="POST", headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    last = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=900) as r:
                d = json.loads(r.read())
                ch = d["choices"][0]
                text = (ch["message"].get("content") or "").strip()
                if ch.get("finish_reason") == "length" or not text:
                    text += "\n\n[TRUNCATED: finish_reason=length; the reasoning used the token limit, raise --max-tokens]"
                return text, d.get("usage", {})
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget-usd", type=float, default=2.0)
    ap.add_argument("--in-rate", type=float, default=1.74)
    ap.add_argument("--out-rate", type=float, default=3.48)
    ap.add_argument("--cache-rate", type=float, default=0.0145)
    ap.add_argument("--max-tokens", type=int, default=24000)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    prefix = ("Our frame (Intel HD 520, 1920x1080, target 99 FPS on the landscape route = 10.1 ms p99, today ~15-19 ms). "
              "Measurements of our own, GPU ms per pass and the breakdown of the ground resolve pass:\n\n" + frame_description() + "\n\n"
              "Constraints: C++17 engine on SDL_GPU (Vulkan, Mesa ANV), one colour attachment per pipeline in our HAL, no hardware VRS, 2-4 GB shared RAM, "
              "the ground is shaded in a full-screen resolve pass from an 8-byte G-buffer; the view is a free flying camera over a large terrain.\n\nQuestion:\n")
    worst = lambda p, q: (len(SYSTEM) + len(p) + len(q)) / 3.0 * a.in_rate / 1e6 + a.max_tokens * a.out_rate / 1e6
    if a.dry_run:
        print(f"prefix {len(prefix)} chars, {len(QUESTIONS)} questions, worst case ${sum(worst(prefix, q[1]) for q in QUESTIONS):.2f}")
        return
    key = read_api_key()
    spent = 0.0
    out = [f"# Low-end rendering techniques (DeepSeek, {datetime.date.today()})", "",
           "Source: DeepSeek `%s`, one request per question; the input was only our own questions and the frame measurements of `PASS_COSTS_2026-10.md` §1-4 (no source code, no Kenshi data). "
           "**Every claim below is SOURCE-MODEL: unverified; items marked CHECK must be verified before they go into an OPT_CANDIDATES card.**" % MODEL, ""]
    for title, q in QUESTIONS:
        if spent + worst(prefix, q) > a.budget_usd:
            out += [f"## {title}", "", "NOT RUN: the budget would be exceeded.", ""]
            continue
        try:
            text, usage = call(key, SYSTEM, prefix + q, a.max_tokens)
        except RuntimeError as e:
            out += [f"## {title}", "", f"ERROR: {e}", ""]
            continue
        hit = usage.get("prompt_cache_hit_tokens", 0)
        miss = usage.get("prompt_cache_miss_tokens", usage.get("prompt_tokens", 0) - hit)
        cost = hit * a.cache_rate / 1e6 + miss * a.in_rate / 1e6 + usage.get("completion_tokens", 0) * a.out_rate / 1e6
        spent += cost
        print(f"{title[:40]}: ${cost:.3f} (cache hit {hit}/{hit + miss} prompt tokens), total ${spent:.3f}", flush=True)
        out += [f"## {title}", "", f"*SOURCE-MODEL, unverified. Cost ${cost:.3f}, prompt cache hit {hit}/{hit + miss} tokens.*", "", text, ""]
    out += ["", f"Total spent: ${spent:.3f} of ${a.budget_usd:.2f}."]
    OUT.write_text("\n".join(out) + "\n")
    print(f"written {OUT}, spent ${spent:.3f}")


if __name__ == "__main__":
    main()
