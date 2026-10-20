#!/usr/bin/env python3
"""
kenshi_seed_select.py -- #188 stage 1 preparation (no API): picks the seed functions of the game core and expands them along the call graph.

Seeds (all read from files, nothing is sent anywhere):
  addr   : addresses 0x14... / FUN_14... found in docs/research/EXE_PARITY_MAP.tsv, re/re_docs/kenshi/*.md and docs/research/*.md that are function entries in the index
  marker : functions whose string literals contain game-core marker words (combat, movement, squad, NPC behaviour, health, animation) -- see MARKERS
Expansion: callees of seeds, up to --hops steps (default 2), only functions with <= --max-chars characters; functions larger than --big (default 40000 chars)
are NOT sent whole: they get a "summary-only" flag (signature + callee list), and are sent whole only when they are seeds.
Output: temp_/kenshi_seeds_stage1.jsonl (name, addr, chars, why, hop) and a stats line (functions, tokens ~ chars / 3.5, estimated USD at the given rates).

Usage: python3 tools/research/kenshi_seed_select.py [--index temp_/kenshi_fn_index.jsonl] [--hops 2] [--out-tokens-per-fn 450]
"""
import argparse
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "re/re_/kenshi_re/kenshi_x64.exe.c"
ADDR = re.compile(r"(?:0x|FUN_|thunk_FUN_)(14[0-9a-fA-F]{7})\b")
STR = re.compile(r'"((?:[^"\\\n]|\\.){4,80})"')
MARKERS = {
    "movement": ["pathfind", "navmesh", "walk", "crouch", "sprint", "speed", "waypoint", "movement"],
    "squad": ["squad", "platoon", "formation", "selected", "order", "command"],
    "npc_behaviour": ["idle", "patrol", "wander", "task", "goal", "job", "react", "alarm", "aggress", "flee"],
    "combat": ["attack", "block", "blunt", "pierce", "cut damage", "stagger", "knockout", "unconscious", "parry", "weapon", "martial"],
    "health": ["bleed", "blood", "limb", "hunger", "robot", "health", "unconsc", "death", "dead", "medic"],
    "animation": ["anim", "skeleton", "bone", "blend", "pose"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index", default=str(REPO / "temp_/kenshi_fn_index.jsonl"))
    ap.add_argument("--out", default=str(REPO / "temp_/kenshi_seeds_stage1.jsonl"))
    ap.add_argument("--hops", type=int, default=2)
    ap.add_argument("--max-chars", type=int, default=12000, help="expansion functions larger than this are skipped (not seeds)")
    ap.add_argument("--big", type=int, default=40000)
    ap.add_argument("--in-rate", type=float, default=1.74)
    ap.add_argument("--out-rate", type=float, default=3.48)
    ap.add_argument("--out-tokens-per-fn", type=int, default=450)
    # #188 step 2 (per system, on demand): only the call tree of the given roots, restricted to one module of the exe map (scripts/exe_module_map.py)
    ap.add_argument("--roots", default="", help="comma-separated function entries (14xxxxxxx): ONLY their call tree, no doc / marker seeds")
    ap.add_argument("--module-map", default=str(REPO / "re/re_/kenshi_exe_modules.tsv"))
    ap.add_argument("--only-module", default="game", help="with --roots: follow only callees of this module")
    a = ap.parse_args()

    fns = [json.loads(l) for l in open(a.index)]
    by_name = {f["name"]: f for f in fns}
    by_addr = {f["addr"].lower(): f for f in fns if f["addr"]}
    why = {}

    modmap = {}
    if a.roots:
        for l in open(a.module_map):
            t = l.rstrip("\n").split("\t")
            if len(t) >= 2: modmap[t[0].lower().rjust(9, "0")] = t[1]
        for r in a.roots.split(","):
            f = by_addr.get(r.strip().lower().replace("0x", "").rjust(9, "0"))
            if f: why[f["name"]] = ("root", 0)
            else: print("root %s is not a function entry in the index" % r)

    # 1. address seeds from the docs
    files = [] if a.roots else [REPO / "docs/research/EXE_PARITY_MAP.tsv"] + list((REPO / "re/re_docs/kenshi").glob("*.md")) + list((REPO / "docs/research").glob("*.md"))
    found, missing = set(), set()
    for p in files:
        try:
            for m in ADDR.finditer(p.read_text(errors="replace")):
                ad = m.group(1).lower().rjust(9, "0")
                (found if ad in by_addr else missing).add(ad)
        except OSError:
            pass
    for ad in found:
        why[by_addr[ad]["name"]] = ("addr", 0)
    # addresses that are not entries: the owner is the function with the greatest entry <= the address (Ghidra lists FUN_ entries; max distance 20000 bytes)
    import bisect
    entries = sorted((int(k, 16), v["name"]) for k, v in by_addr.items())
    keys = [e[0] for e in entries]
    owners = 0
    for ad in missing:
        v = int(ad, 16)
        i = bisect.bisect_right(keys, v) - 1
        if i >= 0 and v - keys[i] <= 20000 and entries[i][1] not in why:
            why[entries[i][1]] = ("addr_owner", 0)
            owners += 1

    # 2. string-marker seeds (one pass over the source)
    ranges = sorted((f["start"], f["end"], f["name"]) for f in fns) if not a.roots else []
    ri = 0
    hits = {}
    cur = None
    with (open("/dev/null") if a.roots else SRC.open(errors="replace")) as fh:
        for n, line in enumerate(fh, 1):
            while ri < len(ranges) and ranges[ri][1] < n:
                ri += 1
            if ri >= len(ranges) or n < ranges[ri][0]:
                continue
            if '"' not in line:
                continue
            name = ranges[ri][2]
            for m in STR.finditer(line):
                s = m.group(1).lower()
                for sysname, words in MARKERS.items():
                    if any(w in s for w in words):
                        hits.setdefault(name, set()).add(sysname)
    for name, systems in hits.items():
        if name not in why:
            why[name] = ("marker:" + "+".join(sorted(systems)), 0)

    def in_module(f):   # named functions have no FUN_ address in the index: followed unless the name is plainly a library's
        if f["addr"]:
            return modmap.get(f["addr"].lower().rjust(9, "0")) == a.only_module
        return a.only_module == "game" and not re.match(r"(Ogre::|MyGUI::|hk|std::|boost::|NX|Px|AK)", f["name"])

    # 3. expansion along the call graph
    frontier = set(why)
    for hop in range(1, a.hops + 1):
        nxt = set()
        for nm in frontier:
            for c in by_name[nm]["callees"]:
                cf = by_name.get(c)
                if cf and c not in why and cf["chars"] <= a.max_chars and (not a.roots or in_module(cf)):
                    why[c] = ("callee", hop)
                    nxt.add(c)
        frontier = nxt

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    chars = whole = summary_only = 0
    kinds = {}
    with out.open("w") as fh:
        for nm, (w, hop) in why.items():
            f = by_name[nm]
            big = f["chars"] > a.big
            sendchars = 600 + 40 * len(f["callees"]) if (big and w == "callee") else f["chars"]
            chars += sendchars
            if big and w == "callee":
                summary_only += 1
            else:
                whole += 1
            k = w.split(":")[0] + (f"_hop{hop}" if hop else "")
            kinds[k] = kinds.get(k, 0) + 1
            fh.write(json.dumps({"name": nm, "addr": f["addr"], "chars": f["chars"], "send_chars": sendchars, "why": w, "hop": hop, "summary_only": big and w == "callee"}) + "\n")
    toks = chars / 3.5
    usd = toks * a.in_rate / 1e6 + len(why) * a.out_tokens_per_fn * a.out_rate / 1e6
    print(json.dumps({"addr_seeds_found": len(found), "addr_seeds_not_function_entries": len(missing), "owner_functions_added": owners, "marker_seeds": len(hits), "total_functions": len(why),
                      "whole": whole, "summary_only": summary_only, "by_kind": kinds, "input_tokens_est": int(toks),
                      "est_usd_no_cache": round(usd, 2), "rates": [a.in_rate, a.out_rate, a.out_tokens_per_fn]}, indent=1))


if __name__ == "__main__":
    main()
