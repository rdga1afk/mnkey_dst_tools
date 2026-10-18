#!/usr/bin/env python3
"""
deepseek_audit_triage.py -- pilot (#185 step 4, docs/research/PARALLEL_AGENTS_PLAN_2026-10.md §0): triage static-analysis findings with the DeepSeek API.

Input  : JSON list of findings {"id", "file", "line", "check", "text"} (see audit_collect_findings.py). Only OUR code (engine/, game/, tools/, tests/) is sent;
         third_party/vendor/external/data paths and anything outside the repo are refused. Kenshi files are never read.
Output : docs/audit/TRIAGE_<date>.jsonl (one line per finding: verdict real_bug | false_positive | style, explanation, minimal patch, usage, cost)
         and the summary TRIAGE_<date>.md. Resumes: ids already present in the jsonl are skipped.
Budget : hard limit in USD by the token counter (prompt + completion tokens x the rates below, rates are a conservative upper bound); a request is
         started only if spent + the worst case of all requests in flight + its own worst case fits in the budget.
The model's verdict is UNTRUSTED: apply only real_bug that is confirmed by a test, a sanitizer or file:line logic.

Usage: python3 tools/research/deepseek_audit_triage.py --findings temp_/audit_findings.json [--workers 8] [--budget-usd 10] [--limit N] [--dry-run]
"""
import argparse
import datetime
import json
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
API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-v4-pro"
ALLOWED_TOP = ("engine", "game", "tools", "tests")
FORBIDDEN_PARTS = ("third_party", "vendor", "external", "/data/", "Kenshi")
CONTEXT_LINES = 40

SYSTEM = (
    "You are a senior C++17 reviewer auditing a game engine (SDL_GPU renderer, gaia-ecs, fixed-size arrays in hot paths, no exceptions, MD_LOG_WARNING instead of assert). "
    "You get ONE static-analysis finding and the code around it. Decide whether it is a real defect. Answer with ONE JSON object and nothing else: "
    '{"verdict": "real_bug" | "false_positive" | "style", "confidence": "high" | "medium" | "low", "explanation": "<=3 sentences citing the lines", '
    '"patch": "minimal unified diff (or empty string)"}. '
    "real_bug = wrong behaviour, UB, leak, overflow, uninitialised read, data race or a clearly wasted hot-path cost. style = correct but could be cleaner. "
    "false_positive = the analyzer is wrong or the code is intentional. Never invent code that is not in the snippet."
)


class Budget:
    def __init__(self, limit_usd, in_rate, out_rate):
        self.limit, self.in_rate, self.out_rate = limit_usd, in_rate / 1e6, out_rate / 1e6
        self.spent = 0.0
        self.reserved = 0.0
        self.lock = threading.Lock()

    def worst_case(self, prompt_tokens, max_tokens):
        return prompt_tokens * self.in_rate + max_tokens * self.out_rate

    def reserve(self, amount):
        with self.lock:
            if self.spent + self.reserved + amount > self.limit:
                return False
            self.reserved += amount
            return True

    def settle(self, reserved, usage):
        with self.lock:
            self.reserved -= reserved
            cost = usage.get("prompt_tokens", 0) * self.in_rate + usage.get("completion_tokens", 0) * self.out_rate
            self.spent += cost
            return cost


def snippet(f):
    rel = Path(f["file"])
    if rel.is_absolute():
        try:
            rel = rel.relative_to(REPO)
        except ValueError:
            return None, "outside the repository"
    s = rel.as_posix()
    if not s.startswith(tuple(a + "/" for a in ALLOWED_TOP)) or any(p in "/" + s for p in FORBIDDEN_PARTS):
        return None, "path not allowed"
    path = REPO / s
    if not path.is_file():
        return None, "file not found"
    lines = path.read_text(errors="replace").split("\n")
    a, b = max(0, f["line"] - 1 - CONTEXT_LINES), min(len(lines), f["line"] + CONTEXT_LINES)
    return "\n".join(f"{i + 1:5d}{'>' if i + 1 == f['line'] else ' '}| {lines[i]}" for i in range(a, b)), None


def build_prompt(f, code):
    return f"File: {f['file']}\nFinding: [{f['check']}] line {f['line']}: {f['text']}\n\nCode (the finding line is marked '>'):\n{code}\n"


def call(key, prompt, effort, max_tokens):
    body = json.dumps({"model": MODEL, "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}],
                       "max_tokens": max_tokens, "reasoning_effort": effort}).encode()
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
        except Exception as e:  # network / timeout
            last = str(e)
            time.sleep(5 * (attempt + 1))
    raise RuntimeError(last)


def parse_verdict(text):
    a, b = text.find("{"), text.rfind("}")
    try:
        d = json.loads(text[a:b + 1])
        if d.get("verdict") in ("real_bug", "false_positive", "style"):
            return d
    except Exception:
        pass
    return {"verdict": "unparsed", "confidence": "low", "explanation": text[:300], "patch": ""}


def summary(path_jsonl, path_md, budget):
    rows = [json.loads(l) for l in path_jsonl.read_text().splitlines() if l.strip()]
    by = {}
    for r in rows:
        by.setdefault(r.get("verdict", "error"), []).append(r)
    out = [f"# Audit triage (DeepSeek pilot) -- {datetime.date.today()}", "",
           f"Findings processed: {len(rows)}; spent ${sum(r.get('cost_usd', 0) for r in rows):.3f} of ${budget.limit:.2f} (counter upper bound).", "",
           "| verdict | count |", "|---|---|"]
    out += [f"| {k} | {len(v)} |" for k, v in sorted(by.items())]
    out += ["", "## real_bug (to be confirmed by a test / sanitizer / file:line logic before any patch)", ""]
    for r in by.get("real_bug", []):
        out.append(f"- `{r['file']}:{r['line']}` [{r['check']}] ({r.get('confidence')}) {r.get('explanation', '')}")
    path_md.write_text("\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--findings", required=True)
    ap.add_argument("--out", default=str(REPO / "docs" / "audit" / f"TRIAGE_{datetime.date.today().isoformat()}.jsonl"))
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--budget-usd", type=float, default=10.0)
    ap.add_argument("--in-rate", type=float, default=1.0, help="USD per 1M prompt tokens (conservative upper bound)")
    ap.add_argument("--out-rate", type=float, default=4.0, help="USD per 1M completion tokens incl. reasoning (conservative upper bound)")
    ap.add_argument("--effort", default="medium")
    ap.add_argument("--max-tokens", type=int, default=6000)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    findings = json.load(open(a.findings))
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    done = {json.loads(l)["id"] for l in out.read_text().splitlines() if l.strip()} if out.exists() else set()
    todo = [f for f in findings if f["id"] not in done]
    if a.limit:
        todo = todo[:a.limit]
    budget = Budget(a.budget_usd, a.in_rate, a.out_rate)
    prior = sum(json.loads(l).get("cost_usd", 0) for l in out.read_text().splitlines() if l.strip()) if out.exists() else 0.0
    budget.spent = prior
    print(f"findings {len(findings)}, already done {len(done)}, to do {len(todo)}, spent so far ${prior:.3f}, budget ${a.budget_usd:.2f}")

    jobs = []
    for f in todo:
        code, err = snippet(f)
        if err:
            jobs.append((f, None, err))
        else:
            jobs.append((f, build_prompt(f, code), None))
    if a.dry_run:
        toks = sum(len(p) // 3 for _, p, _ in jobs if p)
        est = toks * a.in_rate / 1e6 + sum(1 for _, p, _ in jobs if p) * 1500 * a.out_rate / 1e6
        print(f"dry-run: ~{toks} prompt tokens, estimated ${est:.2f} (1500 completion tokens per finding assumed); refused: {sum(1 for _, p, e in jobs if e)}")
        return

    key = read_api_key()
    lock = threading.Lock()

    def work(job):
        f, prompt, err = job
        row = {"id": f["id"], "file": f["file"], "line": f["line"], "check": f["check"], "text": f["text"]}
        if err:
            row.update(verdict="skipped", explanation=err, cost_usd=0.0)
            return row
        worst = budget.worst_case(len(prompt) // 2, a.max_tokens)
        if not budget.reserve(worst):
            return None  # budget exhausted: leave for a later resume
        try:
            text, usage = call(key, prompt, a.effort, a.max_tokens)
            cost = budget.settle(worst, usage)
            row.update(parse_verdict(text))
            row.update(usage=usage, cost_usd=round(cost, 5))
        except Exception as e:
            budget.settle(worst, {})
            row.update(verdict="error", explanation=str(e)[:200], cost_usd=0.0)
        return row

    n = 0
    with ThreadPoolExecutor(max_workers=a.workers) as ex, out.open("a") as fh:
        futs = [ex.submit(work, j) for j in jobs]
        for fu in as_completed(futs):
            r = fu.result()
            if r is None:
                continue
            with lock:
                fh.write(json.dumps(r, ensure_ascii=False) + "\n")
                fh.flush()
            n += 1
            if n % 20 == 0:
                print(f"  {n}/{len(jobs)} done, spent ${budget.spent:.3f}")
    summary(out, out.with_suffix(".md"), budget)
    print(f"done: {n} new rows, spent ${budget.spent:.3f} of ${a.budget_usd:.2f}; summary {out.with_suffix('.md')}")


if __name__ == "__main__":
    main()
