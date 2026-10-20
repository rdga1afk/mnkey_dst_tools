#!/usr/bin/env bash
# #192 (a)+(b) behaviour checks, no frames: Kenshi colour palette loaded in full; text input incl. Cyrillic reaches the EditBox, Backspace removes one character.
cd "$(dirname "$0")/.." || exit 1
export KENSHI_DIR="${KENSHI_DIR:-/home/rdga1/rdga1prj/Kenshi_test}" MD_WORLD_REGISTRY="${MD_WORLD_REGISTRY:-temp_/md_world_registry.txt}"
export MD_MYGUI_LAYOUT=Kenshi_CharacterEditor.layout MD_MYGUI_TYPE="Привіт Kenshi"
out=$(tools/md_ctl run tests/scenarios/mygui_layout_with.lua --timeout 280 2>&1 | grep -o 'log: .*' | cut -d' ' -f2)
n=$(grep -c "<Property" "$KENSHI_DIR/data/gui/colours/kenshi_colours.xml")
rc=0
grep -q "colour palette: $n entries" "$out" && echo "PASS palette: $n of $n entries" || { echo "FAIL palette (expected $n)"; rc=1; }
grep -q "NameText after typing 'Привіт Kenshi' + 1 Backspace: 'NAMEПривіт Kensh'" "$out" && echo "PASS text input: Cyrillic + Latin typed, 1 Backspace removed 1 char" || { echo "FAIL text input"; rc=1; }
exit $rc
