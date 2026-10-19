#!/usr/bin/env python3
"""
deepseek_reimpl_lessons.py -- #192: lessons of open re-implementations of old games, one DeepSeek request per project.
Input  : only our own text (the questions and 5 lines about our project). No source files, no Kenshi data.
Output : docs/research/REIMPL_LESSONS_2026-10.md (answers marked SOURCE-MODEL / CHECK; the model's claims are unverified).
Budget : hard limit in USD from the ACTUAL usage fields of every response; the constant prefix is byte-identical in all requests.
Usage  : python3 tools/research/deepseek_reimpl_lessons.py [--budget-usd 1] [--dry-run]
"""
import argparse
import datetime
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402
from deepseek_lowend_techniques import call  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "docs" / "research" / "REIMPL_LESSONS_2026-10.md"

SYSTEM = (
    "You are an engineer who knows open-source re-implementations of old games well. Answer concisely and concretely, in English. "
    "Mark every fact you are not certain about with 'CHECK'; never invent numbers, dates, team sizes or repository facts -- say 'unknown' instead."
)

PROJECTS = ["OpenGothic", "OpenMW", "OpenRCT2", "OpenLoco", "devilutionX", "N64 decompilations (sm64, OoT)", "xoreos", "GemRB", "Arx Libertatis", "OpenXcom"]

PREFIX = (
    "Our project: a native Linux engine that renders and runs the world of Kenshi (original game: Ogre, Havok, MyGUI, Windows exe) from the player's own installation; "
    "no game files are shipped. One developer plus AI agents, target Intel HD 520 at 1920x1080, 60 FPS. "
    "Behaviour is verified against the original exe by decompilation notes and tests. "
    "Goal: learn what open re-implementations of other games did, and what applies to us.\n\n"
    "For the project below answer in this order:\n"
    "1. How they verified fidelity to the original: tests, comparison with the original exe, function replacement, compatibility autotests.\n"
    "2. What turned out longest and most expensive: years, parts of the game.\n"
    "3. How testing and bug reports were organised: community, crash reports, bots.\n"
    "4. Team size and pace: people, years to a playable version.\n"
    "5. What applies to us (one developer + AI, HD 520, Kenshi on Ogre / Havok / MyGUI).\n\nProject: "
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--budget-usd", type=float, default=1.0)
    ap.add_argument("--in-rate", type=float, default=1.74)
    ap.add_argument("--out-rate", type=float, default=3.48)
    ap.add_argument("--cache-rate", type=float, default=0.0145)
    ap.add_argument("--max-tokens", type=int, default=16000)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    worst = (len(SYSTEM) + len(PREFIX)) / 3.0 * a.in_rate / 1e6 + a.max_tokens * a.out_rate / 1e6
    if a.dry_run:
        print(f"{len(PROJECTS)} projects, worst case per request ${worst:.2f}")
        return
    key = read_api_key()
    spent = 0.0
    out = [f"# Lessons of open re-implementations (DeepSeek, {datetime.date.today()})", "",
           "Source: DeepSeek, one request per project; the input was only our own questions and a five-line description of our project. "
           "**Every claim is SOURCE-MODEL: unverified; CHECK items must be verified before use.**", ""]
    for p in PROJECTS:
        if spent + worst > a.budget_usd:
            out += [f"## {p}", "", "NOT RUN: the budget would be exceeded.", ""]
            continue
        try:
            text, usage = call(key, SYSTEM, PREFIX + p, a.max_tokens)
        except RuntimeError as e:
            out += [f"## {p}", "", f"ERROR: {e}", ""]
            continue
        hit = usage.get("prompt_cache_hit_tokens", 0)
        miss = usage.get("prompt_cache_miss_tokens", usage.get("prompt_tokens", 0) - hit)
        cost = hit * a.cache_rate / 1e6 + miss * a.in_rate / 1e6 + usage.get("completion_tokens", 0) * a.out_rate / 1e6
        spent += cost
        print(f"{p}: ${cost:.3f}, total ${spent:.3f}", flush=True)
        out += [f"## {p}", "", f"*SOURCE-MODEL, unverified. Cost ${cost:.3f}.*", "", text, ""]
    out += ["## Top-5 techniques for us", "", "(to be written by the agent after reading the sections above; each item marked SOURCE-MODEL / CHECK)", "",
            f"Total spent: ${spent:.3f} of ${a.budget_usd:.2f}."]
    OUT.write_text("\n".join(out) + "\n")
    print(f"written {OUT}, spent ${spent:.3f}")


if __name__ == "__main__":
    main()
