#!/usr/bin/env python3
"""
_deepseek_common.py -- shared helpers duplicated byte-identically across
10 (read_api_key) / 5 (_brace_expand) of tools/research/deepseek_*.py
scripts (surgical-simplicity audit, docs/SURGICAL_SIMPLICITY_
AUDIT_2026-09.md §2). Not a research script itself -- has no __main__.
"""
import os
import re
import sys
from pathlib import Path

KEY_FILE = Path("/home/rdga1/rdga1bot-cli-md-deepseek.txt")


def read_api_key() -> str:
    env_key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if env_key:
        return env_key
    if KEY_FILE.exists():
        return KEY_FILE.read_text().strip()
    print(f"ERROR: no DEEPSEEK_API_KEY env var and {KEY_FILE} not found", file=sys.stderr)
    sys.exit(1)


def _brace_expand(pattern: str):
    m = re.search(r"\{([^{}]+)\}", pattern)
    if not m:
        return [pattern]
    options = m.group(1).split(",")
    out = []
    for opt in options:
        out.extend(_brace_expand(pattern[:m.start()] + opt + pattern[m.end():]))
    return out
