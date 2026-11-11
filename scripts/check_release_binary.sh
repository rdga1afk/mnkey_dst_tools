#!/bin/bash
# check_release_binary: MIGRATION_PLAN 5.3 — a release binary must not contain Dear ImGui Test Engine (licence), and the link list must not name GPL libraries. usage: scripts/check_release_binary.sh [binary ...]  (default build/game/monkey_dust build/tools/monkey_dust_editor); exit 1 on any hit. The embedded THIRD_PARTY.md names the test engine in a table row (lines starting with "| " are not counted).
cd "$(dirname "$0")/.." || exit 1
bins=("$@"); [ ${#bins[@]} -eq 0 ] && bins=(build/game/monkey_dust build/tools/monkey_dust_editor)
rc=0
for b in "${bins[@]}"; do
  [ -f "$b" ] || { echo "$b: missing (skipped)"; continue; }
  sym=$(nm -C "$b" 2>/dev/null | grep -c 'ImGuiTestEngine\|ImGuiTestContext\|ImGuiTest::')
  str=$(strings "$b" | grep -v '^| ' | grep -c 'Dear ImGui Test Engine\|imgui_te_\|ImGuiTestEngine')
  gpl=$(ldd "$b" 2>/dev/null | grep -ciE 'libreadline|libgpl|libgmp|libbfd')
  v=PASS; { [ "$sym" -gt 0 ] || [ "$str" -gt 0 ] || [ "$gpl" -gt 0 ]; } && { v=FAIL; rc=1; }
  echo "$b: test-engine symbols $sym, strings $str, GPL-class shared libs $gpl -> $v"
done
exit $rc
