#!/usr/bin/env python3
"""Release asset audit (#201 R1): which files does the binary open, and where does each one come from?

Two passes:
  dynamic  run scenarios under `strace -f -e trace=openat,stat` and collect every regular file opened under the repo (and, separately, under KENSHI_DIR)
  static   string literals in the sources that look like data paths (game/data/..., shaders/..., data/...) for files the scenarios do not reach
Every file is classified by the first matching rule of a rules file (TSV: glob <TAB> class <TAB> generator-or-source <TAB> note):
  ours | third-party | kenshi-derived | dev-only ;  a file that matches no rule is UNKNOWN and the script exits 1.
Output: a Markdown table (default docs/RELEASE_ASSETS.md).

usage: asset_audit.py [--rules docs/release_asset_rules.tsv] [--out docs/RELEASE_ASSETS.md] [--no-run] [--trace-dir temp_/release]
  --no-run  reuse the strace logs already in the trace dir
"""
import argparse, fnmatch, os, re, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KENSHI = os.environ.get("KENSHI_DIR", "/home/rdga1/rdga1prj/Kenshi_test")
SKIP_PREFIX = ("/usr/", "/lib", "/proc/", "/sys/", "/dev/", "/etc/", "/run/", "/var/", "/tmp/", "/opt/", "/bin/", "/sbin/")
STATIC_DIRS = ["game/src", "engine/src", "engine/include", "tools/editor", "scripts"]
DISK_DIRS = ["game/data"]
STATIC_RE = re.compile(r'"((?:game/data|shaders|data|tests/data|tmp_|temp_)/[A-Za-z0-9_./%*+\- ]*)"')

# (name, argv, cwd-relative env) of the runs; the data-dir is the one the gates use
SCENARIOS = [
    ("game_frames", ["build/game/monkey_dust", "--exec", "temp_/release/gate_frames_fast.lua", "--seed", "42", "--fast", "--headless", "--data-dir", "tmp_/scenario_data/x"]),
    ("game_char", ["build/game/monkey_dust", "--exec", "tests/scenarios/kenshi_data_tab.lua", "--seed", "42", "--fast", "--headless", "--data-dir", "tmp_/scenario_data/x"]),
    ("editor_char", ["build/tools/monkey_dust_editor", "--exec", "tests/scenarios/char_pairs.lua", "--seed", "42"]),
]


def prepare(trace_dir):
    trace_dir.mkdir(parents=True, exist_ok=True)
    src = (ROOT / "tests/gates/scenarios/gate_frames.lua").read_text()
    (trace_dir / "gate_frames_fast.lua").write_text(src.replace("local FAST = false", "local FAST = true"))
    (ROOT / "temp_/char_pairs").mkdir(parents=True, exist_ok=True)
    (ROOT / "temp_/char_pairs/.keep").touch()
    (ROOT / "temp_/gates/frames").mkdir(parents=True, exist_ok=True)
    (ROOT / "temp_/gates/frames/.keep").touch()


def run_scenarios(trace_dir):
    prepare(trace_dir)
    env = dict(os.environ, KENSHI_DIR=KENSHI)
    for name, argv in SCENARIOS:
        log = trace_dir / f"{name}.strace"
        cmd = ["strace", "-f", "-qq", "-e", "trace=openat,stat,newfstatat,access", "-o", str(log)] + argv
        print(f"[audit] running {name} ...", flush=True)
        subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)


OPEN_RE = re.compile(r'(?:openat\(\w+, |stat\(|access\()"([^"]+)"[^=]*= (-?\d+)')


def parse_logs(trace_dir):
    """-> (repo files {rel: set(scenario)}, kenshi files {rel: set(scenario)})"""
    repo, kenshi = {}, {}
    for log in sorted(trace_dir.glob("*.strace")):
        name = log.stem
        for line in log.read_text(errors="ignore").splitlines():
            m = OPEN_RE.search(line)
            if not m or m.group(2).startswith("-"):
                continue
            p = m.group(1)
            if not p.startswith("/"):
                p = str(ROOT / p)
            if p.startswith(KENSHI + "/"):
                if os.path.isfile(p): kenshi.setdefault(p[len(KENSHI) + 1:], set()).add(name)
                continue
            if p.startswith(SKIP_PREFIX) or not p.startswith(str(ROOT) + "/"):
                continue
            rel = os.path.relpath(os.path.realpath(p), ROOT)
            if rel.startswith("..") or not os.path.isfile(ROOT / rel):
                continue
            if rel.startswith(("build/", ".git/", "temp_/", "tmp_/scenario_data", "tmp_/md_ctl_logs")):
                continue
            repo.setdefault(rel, set()).add(name)
    return repo, kenshi


def static_paths():
    """string literals that look like data paths -> {literal: first source file}"""
    found = {}
    for d in STATIC_DIRS:
        for f in (ROOT / d).rglob("*"):
            if f.suffix not in (".cpp", ".h", ".hpp", ".cc", ".lua", ".sh", ".py", ".txt") or not f.is_file():
                continue
            try: text = f.read_text(errors="ignore")
            except OSError: continue
            for m in STATIC_RE.finditer(text):
                found.setdefault(m.group(1), str(f.relative_to(ROOT)))
    return found


def load_rules(path):
    rules = []
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        parts += [""] * (4 - len(parts))
        rules.append(tuple(parts[:4]))
    return rules


def classify(rel, rules):
    for glob, cls, src, note in rules:
        if fnmatch.fnmatch(rel, glob):
            return cls, src, note, glob
    return "UNKNOWN", "", "", ""


def tracked_set():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True).stdout.splitlines()
    return set(out)


def size_of(rel):
    try: return (ROOT / rel).stat().st_size
    except OSError: return 0


def fmt_size(n):
    return f"{n / 1048576:.1f} MB" if n >= 1048576 else f"{n / 1024:.0f} KB" if n >= 1024 else f"{n} B"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rules", default=str(ROOT / "docs/release_asset_rules.tsv"))
    ap.add_argument("--out", default=str(ROOT / "docs/RELEASE_ASSETS.md"))
    ap.add_argument("--trace-dir", default=str(ROOT / "temp_/release"))
    ap.add_argument("--no-run", action="store_true")
    a = ap.parse_args()
    trace_dir = Path(a.trace_dir)
    if not a.no_run:
        run_scenarios(trace_dir)
    repo, kenshi = parse_logs(trace_dir)
    stat = static_paths()
    rules = load_rules(a.rules)
    tracked = tracked_set()

    files = {}   # rel -> dict
    for rel, sc in repo.items():
        files[rel] = {"how": "opened by " + ", ".join(sorted(sc)), "static": ""}
    # static literals that name an existing file
    for lit, where in stat.items():
        p = ROOT / lit
        if "%" not in lit and "*" not in lit and p.is_file():
            rel = str(p.relative_to(ROOT))
            if rel not in files and not rel.startswith(("temp_/", "tmp_/")):
                files[rel] = {"how": "named in " + where, "static": where}
    # every file that sits in the data folders, opened or not (the release has to account for all of them)
    for d in DISK_DIRS:
        for f in (ROOT / d).rglob("*"):
            rel = str(f.relative_to(ROOT))
            if f.is_file() and rel not in files and f.name != ".gitignore" and f.name != ".keep":
                files[rel] = {"how": "on disk, not opened in the audited scenarios", "static": ""}

    rows = []
    unknown = []
    for rel in sorted(files):
        cls, src, note, glob = classify(rel, rules)
        rows.append((rel, cls, src, note, size_of(rel), rel in tracked, files[rel]["how"]))
        if cls == "UNKNOWN":
            unknown.append(rel)

    by_class = {}
    for r in rows:
        by_class.setdefault(r[1], []).append(r)

    out = ["# Release assets (generated by tools/release/asset_audit.py — do not edit by hand)", "",
           "Every file the binary opens in the audited scenarios (strace) or names in the sources, with its class. Rules: `docs/release_asset_rules.tsv` (first match wins).", "",
           "Classes: **ours** (our code / art / data), **third-party** (licence named), **kenshi-derived** (converted, baked, cropped or recoloured from install data: generator needed, never committed), **dev-only** (not shipped), UNKNOWN (needs a decision).", ""]
    out += ["| class | files | size | in git |", "|---|---|---|---|"]
    for c in sorted(by_class):
        rs = by_class[c]
        out.append(f"| {c} | {len(rs)} | {fmt_size(sum(r[4] for r in rs))} | {sum(1 for r in rs if r[5])} |")
    out += ["", f"Files the binary read from the Kenshi install (read-only, fine): {len(kenshi)}.", ""]
    out += ["## R3 work list (generator per group)", "", "| generator / source | files | size | in git |", "|---|---|---|---|"]
    gens = {}
    for r in by_class.get("kenshi-derived", []) + by_class.get("unclear", []):
        g = (r[2] or "none yet") if r[1] == "kenshi-derived" else "UNCLEAR — " + (r[2] or "provenance not recorded")
        w = gens.setdefault(g, [0, 0, 0]); w[0] += 1; w[1] += r[4]; w[2] += int(r[5])
    for g, (n, sz, tr) in sorted(gens.items(), key=lambda kv: -kv[1][1]):
        out.append(f"| {g} | {n} | {fmt_size(sz)} | {tr} |")
    out.append("")
    for c in ("UNKNOWN", "kenshi-derived", "third-party", "dev-only", "ours"):
        rs = by_class.get(c, [])
        if not rs:
            continue
        out += [f"## {c} ({len(rs)})", "", "| file | generator / source | note | size | git | how found |", "|---|---|---|---|---|---|"]
        for rel, _, src, note, sz, tr, how in rs:
            out.append(f"| `{rel}` | {src or '—'} | {note or '—'} | {fmt_size(sz)} | {'yes' if tr else 'no'} | {how} |")
        out.append("")
    Path(a.out).write_text("\n".join(out))
    print(f"[audit] {len(rows)} files; " + ", ".join(f"{c} {len(v)}" for c, v in sorted(by_class.items())) + f"; Kenshi install files read: {len(kenshi)}")
    if unknown:
        print(f"[audit] UNKNOWN {len(unknown)} (first 25):")
        for u in unknown[:25]:
            print("   ", u)
        sys.exit(1)


if __name__ == "__main__":
    main()
