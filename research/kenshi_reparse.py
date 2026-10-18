#!/usr/bin/env python3
"""
kenshi_reparse.py -- #188: repairs the stage results of deepseek_exe_functions.py locally (no API call): bare hex literals in "constants" (0x250 -> "0x250"), trailing commas,
and, if the JSON is still broken, keeps at least the "purpose" and "system" fields (marked salvaged_partial). Idempotent; rows that parse are left alone.
Usage: python3 tools/research/kenshi_reparse.py temp_/kenshi_stage2_results.jsonl
"""
import json
import re
import sys


def quote_hex(s):
    """Wrap bare hex literals (outside JSON strings only) in quotes."""
    out, i, n, ins = [], 0, len(s), False
    while i < n:
        c = s[i]
        if ins:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(s[i + 1]); i += 1
            elif c == '"':
                ins = False
        elif c == '"':
            ins = True; out.append(c)
        else:
            m = re.match(r"-?0x[0-9a-fA-F]+", s[i:]) if c in "-0" else None
            if m:
                out.append('"' + m.group(0) + '"'); i += len(m.group(0)) - 1
            else:
                out.append(c)
        i += 1
    return "".join(out)


def repair(text):
    a, b = text.find("{"), text.rfind("}")
    s = re.sub(r",\s*([}\]])", r"\1", quote_hex(text[a:b + 1]))
    try:
        return json.loads(s, strict=False)
    except Exception:
        pass
    m = re.search(r'"purpose"\s*:\s*"((?:[^"\\]|\\.)*)"', text)
    sy = re.search(r'"system"\s*:\s*"(\w+)"', text)
    if m:
        return {"purpose": m.group(1), "salvaged_partial": True, "system": sy.group(1) if sy else None}
    return None


def main():
    path = sys.argv[1]
    rows = [json.loads(l) for l in open(path) if l.strip()]
    ok = salv = lost = 0
    for r in rows:
        if "unparsed" in r.get("result", {}):
            d = repair(r["result"]["unparsed"])
            if d is None:
                lost += 1
            else:
                r["result"] = d
                if d.get("salvaged_partial"):
                    salv += 1
                else:
                    ok += 1
    tmp = path + ".tmp"
    open(tmp, "w").write("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows))
    import os
    os.replace(tmp, path)
    print(f"{len(rows)} rows: repaired {ok}, purpose only {salv}, lost {lost}")


if __name__ == "__main__":
    main()
