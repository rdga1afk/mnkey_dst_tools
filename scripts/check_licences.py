#!/usr/bin/env python3
"""check_licences: every vendored third-party folder has a licence file and a row in THIRD_PARTY.md (MIGRATION_PLAN 5.3, CI of tools). Prints one line per folder.
usage: scripts/check_licences.py [--root DIR]   (looks in <root>/third_party, <root>/engine/third_party, <root>/tools/third_party; THIRD_PARTY.md in <root>/engine or <root>)
Exit 1 when a folder has no licence file or is not named in THIRD_PARTY.md."""
import os, sys, glob
root = os.getcwd()
if '--root' in sys.argv: root = sys.argv[sys.argv.index('--root') + 1]
dirs = []
for base in ('third_party', 'engine/third_party', 'tools/third_party'):
    p = os.path.join(root, base)
    if os.path.isdir(p): dirs += [os.path.join(p, d) for d in sorted(os.listdir(p)) if os.path.isdir(os.path.join(p, d))]
tp = ''
for cand in ('engine/THIRD_PARTY.md', 'THIRD_PARTY.md'):
    f = os.path.join(root, cand)
    if os.path.isfile(f): tp += open(f, encoding='utf-8', errors='replace').read().lower()
rc = 0
for d in dirs:
    name = os.path.basename(d)
    has_file = any(glob.glob(os.path.join(d, pat)) for pat in ('LICEN[CS]E*', 'COPYING*', 'LICEN[CS]E*/*', 'license*', 'copying*', 'Licen[cs]e*'))
    if not has_file:   # a nested folder (include/, docs/) is accepted one level down
        has_file = bool(glob.glob(os.path.join(d, '*', 'LICEN[CS]E*')) or glob.glob(os.path.join(d, '*', 'COPYING*')))
    lh = os.path.join(d, 'src', 'lua.h')   # Lua carries its MIT notice in the header of the source, not in a LICENSE file
    if not has_file and os.path.isfile(lh) and 'PUC-Rio' in open(lh, encoding='utf-8', errors='replace').read(): has_file = True
    listed = name.lower() in tp or name.lower().replace('-', '_') in tp or name.lower().replace('_', '-') in tp
    v = 'OK' if has_file and listed else 'FAIL'
    if v == 'FAIL': rc = 1
    print(f'{name}: licence file {"yes" if has_file else "NO"}, in THIRD_PARTY.md {"yes" if listed else "NO"} -> {v}')
sys.exit(rc)
