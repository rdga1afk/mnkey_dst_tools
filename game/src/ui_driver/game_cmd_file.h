#pragma once
// game_cmd_file.h — live command-file automation channel for the GAME
// binary (monkey_dust), porting tools/editor/editor_cmd_file.cpp's task
// #123 protocol from the standalone editor.
//
// Unlike --exec (game/src/autonomy/scenario_driver.h — one Lua script,
// decided at process launch, run to completion via LuaSystem::StartScenario/
// ResumeScenario), this lets an ALREADY RUNNING, normally-launched game
// instance execute arbitrary Lua commands one at a time, sent from outside
// the process without a relaunch — used by the UiDriver (ui_driver.h) so
// an external agent can click/key-hold/screenshot interactively, each next
// command depending on the previous result.
//
// Protocol (files under tmp_/game_cmd/ — a SEPARATE directory from the
// editor's tmp_/editor_cmd/, so the two binaries never collide), all
// written by the sender via write-to-.tmp + rename to avoid a torn read:
//   cmd.lua     — Lua source of the pending command
//   cmd.seq     — integer, bumped LAST by the sender (signals new work)
//   result.json — {"seq":N,"ok":true|false,"error":"..."}, written by the
//                 game after executing; seq echoes the command's seq.
//
// GameCmdFile_Poll() must be called once per frame from main.cpp's normal
// interactive loop (only meaningful once md.screenshot/md.ui.* etc. are
// registered — see main.cpp's MONKEY_DUST_EDITOR block). Cost when idle is
// a single small-file read (cmd.seq) — no directory scan, no malloc.

void GameCmdFile_Poll();
