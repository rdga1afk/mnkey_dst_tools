#!/usr/bin/env bash
# #192 step 3 oracle (numbers): the pixels the UI changes must lie exactly inside the panels the .layout declares.
# usage: scripts/mygui_layout_check.sh [Layout.layout]   (needs: cmake -DMD_MYGUI=ON build; KENSHI_DIR)
cd "$(dirname "$0")/.." || exit 1
export KENSHI_DIR="${KENSHI_DIR:-/home/rdga1/rdga1prj/Kenshi_test}" MD_WORLD_REGISTRY="${MD_WORLD_REGISTRY:-temp_/md_world_registry.txt}"
L="${1:-Kenshi_CharacterEditor.layout}"; mkdir -p temp_/mygui
MD_MYGUI_LAYOUT="$L" MD_MYGUI_DUMP=temp_/mygui/widgets.tsv tools/md_ctl run tests/scenarios/mygui_layout_with.lua --timeout 280 >/dev/null 2>&1 || { echo "FAIL: run with UI"; exit 1; }
env -u MD_MYGUI_LAYOUT tools/md_ctl run tests/scenarios/mygui_layout_without.lua --timeout 280 >/dev/null 2>&1 || { echo "FAIL: run without UI"; exit 1; }
python3 - <<'PY'
import numpy as np
from PIL import Image
a = np.asarray(Image.open('temp_/mygui/with_ui.png').convert('RGB')).astype(int)
b = np.asarray(Image.open('temp_/mygui/without_ui.png').convert('RGB')).astype(int)
diff = np.abs(a - b).sum(2) > 0
rects = []
for l in open('temp_/mygui/widgets.tsv'):
    if l.startswith('  ') and not l.startswith('    ') and not l.startswith('#'):   # depth 1 = top-level panels
        n, x, y, w, h = l.rstrip('\n').lstrip().split('\t'); rects.append((n, int(x), int(y), int(x) + int(w), min(int(y) + int(h), a.shape[0])))
inside = np.zeros_like(diff)
for n, x0, y0, x1, y1 in rects: inside[y0:y1, x0:x1] = True
out = diff & ~inside
outside = int(out.sum())
if outside:   # the mouse pointer (a 32x32 sprite at the cursor) is UI too and lies outside the panels: allowed when all stray pixels fit in one 32x32 box
    ys, xs = np.where(out)
    if xs.max() - xs.min() < 32 and ys.max() - ys.min() < 32:
        print('stray pixels fit one pointer sprite at', int(xs.min()), int(ys.min())); outside = 0
print('panels from the layout:', [(r[0], r[1], r[2], r[3], r[4]) for r in rects])
print('pixels changed by the UI:', int(diff.sum()), ' outside every declared panel:', outside)
missing = [r[0] for r in rects if not diff[r[2]:r[4], r[1]:r[3]].any()]
print('panels with no drawn pixel:', missing)
ok = outside == 0 and not missing
print('PASS' if ok else 'FAIL'); raise SystemExit(0 if ok else 1)
PY
