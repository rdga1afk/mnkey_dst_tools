-- #192: the same scene without the UI (baseline for scripts/mygui_layout_check.sh)
local CX, CZ = 9647.7, 15038.8
md.wait_ticks(100); md.set_resolution_scale(1.0); md.set_time_of_day(12.0)
for a = 1, 4 do md.place_window_on_largest_display(true); md.wait_ticks(100); md.render_frame(); md.wait_ticks(10) end
md.teleport_player(CX + 120.0, CZ); md.wait_ticks(300)
for i = 1, 700 do md.render_frame(); md.wait_ticks(1) end
md.screenshot("temp_/mygui/without_ui.png"); md.wait_ticks(30)
md.quit(0)
