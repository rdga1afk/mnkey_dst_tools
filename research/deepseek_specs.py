#!/usr/bin/env python3
"""
deepseek_specs.py -- #188: drafts the system specifications (docs/spec/<system>.md) from the per-function records of deepseek_exe_functions.py.
Input : temp_/s2_final.jsonl (stage results, private) + the function-name sheet (RTTI names) of the reference project. Only RECORDS (purposes, formulas, constants with addresses) go to the
        model, never Kenshi files and never decompiled text. Output: temp_/spec_drafts/<system>.md -- a DRAFT: every fact is checked against Ghidra before it moves to docs/spec/.
Cache : the instruction block is first and identical in every request. Budget: hard limit from the usage fields of the responses.
Usage : python3 tools/research/deepseek_specs.py --budget-usd 10 [--systems movement,squad,npc,combat] [--dry-run]
"""
import argparse
import csv
import json
import re
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _deepseek_common import read_api_key  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SHEET = Path(__import__("os").environ.get("MD_FN_SHEET", ""))   # function-name sheet (RTTI names), a local file of the reference project
API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-v4-pro"

SYSTEMS = {
    "movement": dict(
        title="Рух персонажа та навігація",
        labels={"movement"},
        names=r"CharMovement|AbstractMovementBase|Task_Move|Task_Goto|MoveTo|Walk|Run|Crouch|Speed|NavMesh|PathFind|Navigat|Stagger|Knock|GetUp",
        extra=set(),
        focus="швидкості (ходьба / біг / присід / ухил / вантаж), розворот, зупинка, вхід і вихід пошуку шляху (лише що подається в Detour-аналог і що повертається; внутрішнє Havok не описувати)",
    ),
    "squad": dict(
        title="Загін",
        labels={"squad"},
        names=r"Platoon|Squad|FollowLeader|Formation|Task_Follow|ActivePlatoon|Leader|Task_TravelToMission",
        extra={"1407b5c90"},
        focus="вибір, накази руху, розстановка біля лідера (1407b5c90), слідування",
    ),
    "npc": dict(
        title="NPC: стоять і ходять",
        labels={"npc_behaviour"},
        names=r"^Task_(Idle|Patrol|Wander|Sit|Sleep|GetUp|Crouch|UnCrouch|PatrolTown|Stand|Wait|UseBed|SleepOnFloor)|AITaskSytem|AIPackage|AICharacter|^AI$|idle",
        extra=set(),
        focus="найпростіші Task_*, розклад, idle, вибір наступного завдання",
    ),
    "combat": dict(
        title="Найпростіший бій",
        labels={"melee_combat", "limb_health"},
        names=r"CombatClass|AttackState|BlockState|MedicalSystem|CharStats|Armour|Weapon|Limb|bleed|Knock|Unconsc|attackBonus|blockChance|defendOutcome|attackContest",
        extra=set(),
        focus="атака, влучання, шкода по частинах тіла, блок, KO / смерть",
    ),
}

SYSTEM = (
    "You write a technical specification of ONE subsystem of the game Kenshi (x64 Windows executable), in UKRAINIAN, from per-function records produced earlier by an automatic analysis of the "
    "decompiled code. The records are your ONLY source. Rules: (1) every fact carries the address of its function in the form `адреса: 14xxxxxxx`; (2) a number may be written only if it is in the record "
    "(\"constants\" or the text); a value marked unknown is written as НЕВІДОМО -- never guess; (3) no pseudocode and no assembly: describe in words and, where a formula is given, as a short mathematical "
    "expression with named quantities; (4) a function whose record does not help the subsystem is left out; (5) when two records disagree, say so under НЕВІДОМО. "
    "Structure (Markdown): `# <title>`; `## Короткий опис`; `## Факти` (a table: адреса | ім'я (RTTI, якщо є) | що робить | впевненість | перевірено в Ghidra: ні); "
    "`## Константи` (table: назва/зміст | значення | адреса джерела); `## Формули` (each with its address and the quantities); "
    "`## Тестові вектори` (at least 5 rows `вхід -> очікуваний вихід`, ONLY where a formula and its constants are known; compute the output; otherwise write that none can be built); "
    "`## Розбіжності з Okran` (leave exactly this sentence: `Заповнюється окремо після звірки.`); `## НЕВІДОМО` (bullet list of what the records do not settle). Be compact."
)


def load_names():
    out = {}
    if SHEET.is_file():
        for r in csv.DictReader(open(SHEET), delimiter="\t"):
            if r.get("addr"):
                out[r["addr"].lower().lstrip("0")] = (r.get("name") or "", r.get("ns") or "")
    return out


def pick(cfg, results, names, cap):
    rx = re.compile(cfg["names"], re.I)
    sel = []
    for x in results:
        q = x.get("result", {})
        if "purpose" not in q:
            continue
        a = (x.get("addr") or "").lower().lstrip("0")
        nm, ns = names.get(a, ("", ""))
        hit = (q.get("system") in cfg["labels"]) or bool(rx.search(ns + " " + nm)) or a in cfg["extra"]
        if not hit:
            continue
        conf = q.get("confidence") if isinstance(q.get("confidence"), (int, float)) else 0
        has_f = 1 if q.get("formulas") else 0
        named = 1 if (ns or (nm and not nm.startswith("FUN_"))) else 0
        score = (a in cfg["extra"]) * 10 + named * 2 + has_f * 2 + conf
        sel.append((score, x, nm, ns))
    sel.sort(key=lambda t: -t[0])
    return sel[:cap]


def compact(x, nm, ns):
    q = x["result"]
    rec = {"adr": x["addr"], "name": (ns + "::" if ns and not nm.startswith(ns) else "") + nm if nm else "", "purpose": q.get("purpose", "")[:420], "conf": q.get("confidence")}
    fs = []
    for f in (q.get("formulas") or [])[:6]:
        fs.append({"what": str(f.get("what", ""))[:110], "words": str(f.get("expression_in_words", ""))[:200], "const": f.get("constants")})
    if fs:
        rec["formulas"] = fs
    calls = [c.get("callee") for c in (q.get("important_calls") or [])[:6] if isinstance(c, dict)]
    if calls:
        rec["calls"] = calls
    return rec


def call(key, prompt, effort, max_tokens):
    body = json.dumps({"model": MODEL, "messages": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": prompt}], "max_tokens": max_tokens, "reasoning_effort": effort}).encode()
    req = urllib.request.Request(API_URL, data=body, method="POST", headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=1800) as r:
        d = json.loads(r.read())
    return d["choices"][0]["message"]["content"], d.get("usage", {})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=str(REPO / "temp_/s2_final.jsonl"))
    ap.add_argument("--out", default=str(REPO / "temp_/spec_drafts"))
    ap.add_argument("--systems", default="movement,squad,npc,combat")
    ap.add_argument("--cap", type=int, default=220)
    ap.add_argument("--budget-usd", type=float, required=True)
    ap.add_argument("--effort", default="high")
    ap.add_argument("--max-tokens", type=int, default=32000)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    results = [json.loads(l) for l in open(a.results) if l.strip()]
    names = load_names()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    spent = 0.0
    key = None if a.dry_run else read_api_key()
    for s in a.systems.split(","):
        cfg = SYSTEMS[s]
        sel = pick(cfg, results, names, a.cap)
        recs = [compact(x, nm, ns) for _, x, nm, ns in sel]
        prompt = f"TITLE: {cfg['title']}\nFOCUS: {cfg['focus']}\nRECORDS ({len(recs)}), JSON lines:\n" + "\n".join(json.dumps(r, ensure_ascii=False) for r in recs) + "\n"
        print(f"{s}: {len(recs)} records, ~{len(prompt) // 3} input tokens")
        if a.dry_run:
            continue
        text, u = call(key, prompt, a.effort, a.max_tokens)
        hit, miss = u.get("prompt_cache_hit_tokens", 0), u.get("prompt_cache_miss_tokens", u.get("prompt_tokens", 0))
        cost = hit * 0.0145e-6 + miss * 1.74e-6 + u.get("completion_tokens", 0) * 3.48e-6
        spent += cost
        (out / f"{s}.md").write_text(text + "\n")
        (out / f"{s}.records.jsonl").write_text("\n".join(json.dumps(r, ensure_ascii=False) for r in recs) + "\n")
        print(f"  -> {out / (s + '.md')} ({len(text)} chars), cost ${cost:.3f}, total ${spent:.3f}, cache hit {hit}/{hit + miss}")
        if spent > a.budget_usd:
            print("budget exhausted")
            break


if __name__ == "__main__":
    main()
