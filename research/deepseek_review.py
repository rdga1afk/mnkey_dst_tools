#!/usr/bin/env python3
"""DeepSeek review of the diff of one step (#199 item 5).
Input : a git range (`--range A..B`, or `--commits N` = last N commits) in main / engine / tools, CODE files only (*.cpp *.h *.lua *.glsl *.py CMake).
Output: temp_/review/<step>.md -- findings 🔴 / 🟡 / 🔵 + «what is good»; one summary line on stdout for the step report.
Never sent: Kenshi data / files, re/**, decompiled text, .def / .base / .mod, secrets (hard exclude list below), anything above --chunk-bytes per request (several requests, ≤ --max-chunks).
Budget: ≤ $5 per calendar month (counter temp_/review/ledger.json from the ACTUAL usage fields of the responses); stops at the cap.
Cheaper model by default (deepseek-v4-flash); the rates below are the pro prices, so the counter is an upper bound.
The model's verdict is UNTRUSTED: a finding is fixed only when confirmed by a test or by file:line logic. It does not replace gates / tests.
Usage: python3 tools/research/deepseek_review.py --step F7 [--range HEAD~3..HEAD | --commits 3] [--repo .|engine|tools] [--dry-run]"""
import argparse, datetime, fnmatch, json, subprocess, sys, urllib.error, urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
API_URL = "https://api.deepseek.com/chat/completions"
CODE_GLOBS = ("*.cpp", "*.h", "*.hpp", "*.lua", "*.glsl", "*.frag", "*.vert", "*.comp", "*.py", "CMakeLists.txt", "*.cmake")
FORBIDDEN = ("re/", "docs/research/", "tmp_/", "temp_/", "third_party/", "/vendor/", "Kenshi_test", ".def", ".base", ".mod", ".key", ".env", "secret", "token", "credential")
SECRET_MARKERS = ("API_KEY", "BEGIN PRIVATE KEY", "Bearer ", "ghp_", "sk-")
SYSTEM = ("You are a senior C++17 / GLSL / Lua reviewer of a game engine (SDL_GPU renderer, gaia-ecs, fixed-size arrays in hot paths, no exceptions, MD_LOG_WARNING instead of assert). "
          "You get the diff of one development step. Reply in Markdown with exactly these sections: '## 🔴 Must fix' (real defects: UB, leaks, races, wrong behaviour; cite file:line), "
          "'## 🟡 Should fix' (likely problems, risky assumptions), '## 🔵 Notes' (style, small cleanups), '## What is good'. Only claims you can support from the diff. Empty section = 'none'.")


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True).stdout


def allowed(path):
    p = path.replace("\\", "/")
    return any(fnmatch.fnmatch(Path(p).name, g) for g in CODE_GLOBS) and not any(f.lower() in p.lower() for f in FORBIDDEN)


def month_spent(ledger):
    m = datetime.date.today().strftime("%Y-%m")
    return m, ledger.get(m, 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--step", required=True)
    ap.add_argument("--range"); ap.add_argument("--commits", type=int, default=1)
    ap.add_argument("--repo", default=".")
    ap.add_argument("--model", default="deepseek-v4-flash")
    ap.add_argument("--chunk-bytes", type=int, default=12000); ap.add_argument("--max-chunks", type=int, default=6)
    ap.add_argument("--budget-usd", type=float, default=5.0)
    ap.add_argument("--in-rate", type=float, default=1.74); ap.add_argument("--out-rate", type=float, default=3.48); ap.add_argument("--cache-rate", type=float, default=0.0145)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    repo = (REPO / a.repo).resolve()
    rng = a.range or f"HEAD~{a.commits}..HEAD"
    files = [f for f in git(repo, "diff", "--name-only", rng).splitlines() if allowed(f)]
    skipped = [f for f in git(repo, "diff", "--name-only", rng).splitlines() if f not in files]
    diff = "".join(git(repo, "diff", rng, "--", f) for f in files)
    if any(m in diff for m in SECRET_MARKERS):
        print("REFUSED: the diff contains a secret-looking string; nothing sent"); return 2
    if not diff.strip():
        print(f"review {a.step}: no code files in {rng} (skipped {len(skipped)} non-code / excluded)"); return 0
    # one request per chunk of ≤ --chunk-bytes (a big multi-file diff makes the model reason past the token cap and answer nothing)
    chunks, cur = [], ""
    for f in files:
        part = git(repo, "diff", rng, "--", f)[: a.chunk_bytes]
        if cur and len(cur) + len(part) > a.chunk_bytes:
            chunks.append(cur); cur = ""
        cur += part
    if cur: chunks.append(cur)
    chunks = chunks[: a.max_chunks]
    out_dir = REPO / "temp_" / "review"; out_dir.mkdir(parents=True, exist_ok=True)
    ledger_path = out_dir / "ledger.json"
    ledger = json.loads(ledger_path.read_text()) if ledger_path.exists() else {}
    month, spent = month_spent(ledger)
    worst = sum(len(c) / 3 * a.in_rate / 1e6 + 12000 * a.out_rate / 1e6 for c in chunks)
    print(f"files {len(files)}, {len(chunks)} request(s), skipped {len(skipped)}, month {month} spent ${spent:.3f}, worst case ${worst:.3f}")
    if spent + worst > a.budget_usd:
        print(f"REFUSED: month spent ${spent:.3f} + worst case ${worst:.3f} > cap ${a.budget_usd:.2f}"); return 3
    if a.dry_run:
        print("dry run: nothing sent"); return 0
    key = read_api_key(); texts = []; total = 0.0; hits = tot_in = 0
    for i, c in enumerate(chunks, 1):
        body = json.dumps({"model": a.model, "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": f"Step {a.step}, range {rng}, part {i}/{len(chunks)}\n\n{c}"}], "max_tokens": 12000, "reasoning_effort": "low"}).encode()
        req = urllib.request.Request(API_URL, data=body, method="POST", headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=600) as r:
                d = json.loads(r.read())
        except (urllib.error.URLError, OSError) as e:
            print(f"ERROR: {e}"); return 4
        ch = d["choices"][0]; text = (ch["message"].get("content") or "").strip(); u = d.get("usage", {})
        if not text: text = f"(empty answer, finish_reason={ch.get('finish_reason')}: the reasoning used the token cap)"
        hit = u.get("prompt_cache_hit_tokens", 0); miss = u.get("prompt_cache_miss_tokens", u.get("prompt_tokens", 0))
        cost = hit * a.cache_rate / 1e6 + miss * a.in_rate / 1e6 + u.get("completion_tokens", 0) * a.out_rate / 1e6
        total += cost; hits += hit; tot_in += hit + miss
        ledger[month] = round(ledger.get(month, 0.0) + cost, 5); ledger_path.write_text(json.dumps(ledger))
        texts.append(f"### Part {i}/{len(chunks)}\n\n{text}")
        if ledger[month] >= a.budget_usd:
            texts.append("(monthly cap reached, stopped)"); break
    body_text = "\n\n".join(texts)
    (out_dir / f"{a.step}.md").write_text(f"# DeepSeek review {a.step} ({rng}, {a.model}) -- UNTRUSTED\n\n{body_text}\n")
    print(f"review {a.step}: saved temp_/review/{a.step}.md; findings 🔴 {body_text.count('🔴')} 🟡 {body_text.count('🟡')} 🔵 {body_text.count('🔵')}; cost ${total:.4f} (month ${ledger[month]:.3f} of ${a.budget_usd:.2f}); cache hit {hits}/{tot_in}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
