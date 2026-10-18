#!/usr/bin/env python3
"""
audit_collect_findings.py -- merges clang-tidy output, cppcheck output and compiler warnings into the findings JSON that deepseek_audit_triage.py reads
(#185 step 4). Only our code (engine/ game/ tools/ tests/, not third_party / vendor / external); duplicates (same file, line, check) are dropped.

Usage: python3 tools/research/audit_collect_findings.py --tidy temp_/tidy_out.txt --cppcheck temp_/cppcheck.txt --warnings temp_/warn_build.log \
           [--skip-warning-categories -Wsign-conversion,-Wimplicit-int-float-conversion] --out temp_/audit_findings.json
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LINE = re.compile(r"^(/[^:\s]+|[A-Za-z][^:\s]*):(\d+):(\d+): (warning|error): (.*?)(?: \[([^\]]+)\])?$")
CPP = re.compile(r"^([^:\s][^:]*):(\d+):(\d+): \((\w+)\) (.*)$")
SKIP = ("third_party", "vendor", "external", "/build", "temp_/")


def rel(p):
    p = Path(p)
    try:
        return p.resolve().relative_to(REPO).as_posix() if p.is_absolute() else p.as_posix()
    except ValueError:
        return None


def ok(r):
    return r is not None and r.split("/")[0] in ("engine", "game", "tools", "tests") and not any(s in "/" + r for s in SKIP)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tidy")
    ap.add_argument("--cppcheck")
    ap.add_argument("--warnings")
    ap.add_argument("--skip-warning-categories", default="")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    skip_cats = {c for c in a.skip_warning_categories.split(",") if c}
    seen, out = set(), []

    def add(src, f, line, check, text):
        key = (f, line, check)
        if key in seen:
            return
        seen.add(key)
        fid = hashlib.sha1(f"{f}:{line}:{check}".encode()).hexdigest()[:12]
        out.append({"id": fid, "file": f, "line": int(line), "check": f"{src}:{check}", "text": text[:300]})

    for src, path in (("tidy", a.tidy), ("warn", a.warnings)):
        if not path:
            continue
        for l in open(path, errors="replace"):
            m = LINE.match(l.rstrip())
            if not m:
                continue
            f, line, _, _, text, check = m.groups()
            r = rel(f)
            if not ok(r) or (src == "warn" and (check or "") in skip_cats):
                continue
            add(src, r, line, check or "?", text)
    if a.cppcheck:
        for l in open(a.cppcheck, errors="replace"):
            m = CPP.match(l.rstrip())
            if m:
                r = rel(m.group(1))
                if ok(r):
                    add("cppcheck", r, m.group(2), m.group(4), m.group(5))
    json.dump(out, open(a.out, "w"), ensure_ascii=False, indent=0)
    print(f"{len(out)} findings -> {a.out}")


if __name__ == "__main__":
    main()
