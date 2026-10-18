#!/usr/bin/env python3
"""
kenshi_fn_index.py -- stage 0 of #188: splits the Ghidra C export (kenshi_x64.exe.c) into functions and builds the call graph. No API calls.

Output (not for the repo, goes to temp_/): kenshi_fn_index.jsonl, one function per line:
  {"name", "addr" (hex of FUN_/thunk_FUN_ names, else null), "start", "end" (1-based lines), "lines", "chars", "callees": [names defined in the file], "callers_n"}
plus kenshi_fn_index_summary.json (file size, function count, total tokens ~ chars / 3.5, cost estimate at the given rate).

Usage: python3 tools/research/kenshi_fn_index.py [--src re/re_/kenshi_re/kenshi_x64.exe.c] [--out temp_/kenshi_fn_index.jsonl] [--in-rate 1.0]
"""
import argparse
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CALL = re.compile(r"([A-Za-z_][\w:~]*)\s*\(")
ADDR = re.compile(r"(?:thunk_)?FUN_([0-9a-fA-F]+)")
SIG_NAME = re.compile(r"([A-Za-z_~][\w:~<>]*)\s*\(")
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "do", "else"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(REPO / "re/re_/kenshi_re/kenshi_x64.exe.c"))
    ap.add_argument("--out", default=str(REPO / "temp_/kenshi_fn_index.jsonl"))
    ap.add_argument("--in-rate", type=float, default=1.0, help="USD per 1M input tokens (conservative upper bound; v4-pro price unverified)")
    a = ap.parse_args()
    src = Path(a.src)
    funcs = []
    cur = None
    prev = ["", ""]  # [line n-2, line n-1] before the current line n
    with src.open(errors="replace") as fh:
        for n, line in enumerate(fh, 1):
            s = line.rstrip("\n")
            if cur is None:
                if s == "{" and prev[1].strip() == "" and prev[0].strip() and "(" in prev[0]:
                    m = SIG_NAME.search(prev[0])
                    name = m.group(1) if m else prev[0].strip()[:80]
                    am = ADDR.search(name)
                    cur = {"name": name, "addr": am.group(1) if am else None, "start": n - 2, "chars": len(prev[0]) + 2, "calls": set()}
                    cur["chars"] += len(s) + 1
            else:
                cur["chars"] += len(s) + 1
                if s == "}":
                    cur["end"] = n
                    funcs.append(cur)
                    cur = None
                else:
                    for m in CALL.finditer(s):
                        nm = m.group(1)
                        if nm not in KEYWORDS:
                            cur["calls"].add(nm)
            prev = [prev[1], s]
    names = {f["name"] for f in funcs}
    callers = {}
    for f in funcs:
        f["callees"] = sorted(c for c in f["calls"] if c in names and c != f["name"])
        for c in f["callees"]:
            callers[c] = callers.get(c, 0) + 1
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    total_chars = 0
    with out.open("w") as fh:
        for f in funcs:
            total_chars += f["chars"]
            fh.write(json.dumps({"name": f["name"], "addr": f["addr"], "start": f["start"], "end": f["end"], "lines": f["end"] - f["start"] + 1,
                                 "chars": f["chars"], "callees": f["callees"], "callers_n": callers.get(f["name"], 0)}) + "\n")
    toks = total_chars / 3.5
    summary = {"file_bytes": src.stat().st_size, "functions": len(funcs), "function_chars": total_chars, "tokens_est": int(toks),
               "full_run_input_usd_at_rate": round(toks * a.in_rate / 1e6, 2), "in_rate_usd_per_1M": a.in_rate}
    out.with_name("kenshi_fn_index_summary.json").write_text(json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
