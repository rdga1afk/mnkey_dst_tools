#!/usr/bin/env python3
"""
game_cmd_driver.py — reusable live-command driver for an already-running
monkey_dust game process, via tmp_/game_cmd/ (game/src/ui_driver/
game_cmd_file.cpp).

Promoted from a session-local script (originally
tmp_/perf_master/game_cmd_driver.py, written for a perf-measurement task)
into permanent tools/qa/ infrastructure — 2026-08-15 session found this is
the reliable way to self-verify visual terrain/shader bugs (screenshot,
camera positioning) WITHOUT depending on the editor's `--exec` scenario
mode, which reuses the interactive render loop and can silently fail to
ever produce a composited frame if the window is hidden/not yet mapped
(see CLAUDE_CONSTITUTION.md ADR entry for the real root cause found this
session: editor main() never called SDL_ShowWindow()). A normally-launched
game process (no --exec) renders every frame for real regardless of
pending commands, so driving it live via this file-based channel avoids
that whole class of bug.

Protocol (game/src/ui_driver/game_cmd_file.cpp): write cmd.lua, bump
cmd.seq, poll result.json for a matching "seq" field. One Lua chunk per
send() -- no return values cross the file boundary, only ok/error.
get_number() round-trips a value via md.log() + stdout (game Lua has no
`io` global, sandboxed) -- redirect the process's stdout to a file and
tail it after each send().

Requires a MONKEY_DUST_EDITOR=ON build (md.set_camera_pose/md.screenshot/
md.set_editor_open are only registered in that config -- see
game/src/scripting/lua_scenario_api_misc.cpp's #ifdef guards). `build/`
in this repo is such a build by default (CMakeLists.txt's
MONKEY_DUST_EDITOR option defaults ON).

USAGE (as a library — RECOMMENDED path, no F3 UI, precise camera angle):
    from game_cmd_driver import Driver
    d = Driver(exe="build/game/monkey_dust")
    d.launch()
    d.screenshot_orbit("/tmp/out.png", x=12670.0, z=11960.0,
                        yaw_deg=90.0, pitch_deg=5.0, dist=18.0)
    d.shutdown()

USAGE (CLI, orbit path):
    python3 tools/qa/game_cmd_driver.py --screenshot /tmp/out.png \\
        --orbit 12670.0,11960.0,90.0,5.0,18.0

USAGE (fly-cam path — editor_open MUST stay true, see screenshot()'s own
doc comment for why editor_open=false silently discards the camera pose;
the F3 "Scene" tab (default-active) fills the ENTIRE frame when open, not
a small sidebar, so this path only gives a clean shot on a build/session
where the F3 layout's active tab has been switched away from Scene, e.g.
by hand-editing data/editor_config.json's persisted state -- for a fully
scripted, reliable shot use screenshot_orbit() above instead):
    d.screenshot("/tmp/out.png", camera=(10148.7, 40.0, 15657.6, 26.6, 22.0),
                 editor_open=True)
"""
import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_EXE = str(_REPO / "build" / "game" / "monkey_dust")
CMD_DIR = _REPO / "tmp_" / "game_cmd"
SEQ_PATH = CMD_DIR / "cmd.seq"
CMD_PATH = CMD_DIR / "cmd.lua"
RESULT_PATH = CMD_DIR / "result.json"
STDOUT_PATH = CMD_DIR / "game_stdout.log"


class Driver:
    def __init__(self, exe=DEFAULT_EXE, cwd=None):
        self.exe = exe
        self.cwd = cwd or str(_REPO)
        self.proc = None
        self._seq = 0
        self._val_counter = 0
        self._stdout_f = None

    def launch(self, wait_s=25, argv=None, env=None):
        # Stale files from a previous run could let the first send() match
        # an old seq by accident -- start clean (same reasoning as
        # tools/editor/editor_cmd_file.cpp's analogous channel).
        os.makedirs(CMD_DIR, exist_ok=True)
        for p in (SEQ_PATH, RESULT_PATH, STDOUT_PATH):
            if p.exists():
                p.unlink()
        self._stdout_f = open(STDOUT_PATH, "wb")
        popen_env = {**os.environ, **env} if env else None
        self.proc = subprocess.Popen(argv or [self.exe], cwd=self.cwd, env=popen_env,
                                      stdout=self._stdout_f, stderr=subprocess.STDOUT)
        deadline = time.time() + wait_s
        while time.time() < deadline:
            ok, _ = self.send("md.log('driver: connected')", timeout=1.0)
            if ok:
                return True
            time.sleep(0.3)
        return False

    def send(self, lua_code, timeout=5.0):
        self._seq += 1
        with open(CMD_PATH, "w") as f:
            f.write(lua_code)
        with open(SEQ_PATH, "w") as f:
            f.write(str(self._seq))
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with open(RESULT_PATH) as f:
                    content = f.read()
            except FileNotFoundError:
                time.sleep(0.02)
                continue
            m = re.search(r'"seq":(\d+)', content)
            if m and int(m.group(1)) == self._seq:
                return ('"ok":true' in content), content
            time.sleep(0.02)
        return False, "TIMEOUT waiting for seq=%d" % self._seq

    def get_number(self, expr):
        self._val_counter += 1
        marker = "MDVAL:%d:" % self._val_counter
        lua = "md.log('%s' .. tostring(%s))" % (marker, expr)
        ok, content = self.send(lua)
        if not ok:
            return None, content
        try:
            with open(STDOUT_PATH, "r", errors="replace") as f:
                text = f.read()
        except FileNotFoundError:
            return None, "stdout log missing"
        idx = text.rfind(marker)
        if idx < 0:
            return None, "marker not found: %s" % marker
        line_end = text.find("\n", idx)
        val_str = text[idx + len(marker): line_end if line_end >= 0 else None].strip()
        try:
            return float(val_str), None
        except ValueError as e:
            return None, "%s (raw=%r)" % (e, val_str)

    def screenshot(self, out_path, camera=None, editor_open=False, settle_s=1.0):
        """camera: (world_x, world_y, world_z, yaw_deg, pitch_deg) or None to
        leave as-is -- these are ABSOLUTE Kenshi world metres (the same
        "Abs World" reading the F3 Camera panel shows), NOT the local
        tnoff-relative coords md.set_camera_pose expects raw. Routed through
        md.set_camera_pose_world (2026-09-05) specifically so callers of
        this wrapper can never hit the world-vs-local mixup that cost a full
        debugging session before it was fixed at the engine layer instead of
        just documented here.

        WARNING (confirmed live, 2026-09-17): if `camera` is given AND
        editor_open resolves to False, this method is a NO-OP for
        positioning -- game/src/main.cpp's UpdateOrbitCamera() OVERWRITES
        the fly-cam pose with the default player-orbit camera whenever
        editor_open is false, regardless of what set_camera_pose_world just
        set. Passing camera= with the (default) editor_open=False used to
        silently produce a screenshot from the WRONG camera with no error
        of any kind. This method now REFUSES that combination (raises) --
        pass editor_open=True explicitly (accept the F3 "Scene" tab filling
        the frame, see class-level doc) or use screenshot_orbit() instead,
        which was live-verified to give a clean, UI-free, precisely-aimed
        shot without this trap."""
        if camera is not None and not editor_open:
            raise ValueError(
                "screenshot(camera=..., editor_open=False) silently discards the camera "
                "pose (game/src/main.cpp's UpdateOrbitCamera overwrites it every frame "
                "editor_open is false) -- pass editor_open=True, or use screenshot_orbit() "
                "for a UI-free shot with a precisely aimed camera.")
        if camera is not None:
            ok, r = self.send("md.set_camera_pose_world(%s)" % ", ".join(str(c) for c in camera))
            if not ok:
                return False, r
        ok, r = self.send("md.set_editor_open(%s)" % ("true" if editor_open else "false"))
        if not ok:
            return False, r
        time.sleep(settle_s)
        return self.send("md.screenshot(%r)" % str(out_path))

    def screenshot_orbit(self, out_path, x, z, yaw_deg=0.0, pitch_deg=10.0, dist=20.0,
                          settle_s=1.0):
        """RECOMMENDED screenshot path (live-verified 2026-09-17, TIN Etap 2
        Stage 2 boundary-seam investigation) -- no F3 UI, no fly-cam trap.
        Teleports the player to (x, z) (ABSOLUTE Kenshi world metres, same
        convention as md.teleport_player) then points the release build's
        fixed third-person orbit camera (md.set_camera_orbit, main.cpp's
        g_cam_az/el/dist) at the given angle -- this camera is driven
        DIRECTLY, independent of editor_open, so there is no risk of it
        being silently overwritten the way screenshot(camera=...,
        editor_open=False) is (see that method's own warning).

        yaw_deg: degrees clockwise from south (g_cam_az's own convention).
        pitch_deg: elevation above horizon -- use single-digit values
        (5-10) for a near-horizontal grazing shot (e.g. checking a terrain
        zone-boundary seam at ground level), higher for a more top-down
        aerial framing.
        dist: orbit distance in metres from the player.

        Does NOT touch editor_open at all -- the player-orbit camera is the
        default render path when the F3 panel was never opened this
        session, so no ordering trap exists here the way it does for the
        fly-cam path."""
        ok, r = self.send("md.teleport_player(%s, %s)" % (x, z))
        if not ok:
            return False, r
        ok, r = self.send("md.set_camera_orbit(%s, %s, %s)" % (yaw_deg, pitch_deg, dist))
        if not ok:
            return False, r
        time.sleep(settle_s)
        return self.send("md.screenshot(%r)" % str(out_path))

    def shutdown(self):
        if self.proc:
            self.send("md.quit(0)", timeout=1.0)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None
        if self._stdout_f:
            self._stdout_f.close()
            self._stdout_f = None


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return 0.0
    if n % 2 == 1:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2.0


def stdev(xs, m):
    if len(xs) < 2:
        return 0.0
    return (sum((x - m) ** 2 for x in xs) / (len(xs) - 1)) ** 0.5


def _main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=DEFAULT_EXE, help="game executable path")
    ap.add_argument("--screenshot", metavar="PATH", help="capture a screenshot to PATH")
    ap.add_argument("--camera", metavar="x,y,z,yaw,pitch",
                     help="fly-cam: position camera before capture (comma-separated floats). "
                          "Forces the F3 panel open (see Driver.screenshot's own warning) -- "
                          "prefer --orbit for a UI-free shot.")
    ap.add_argument("--orbit", metavar="x,z,yaw_deg,pitch_deg,dist",
                     help="RECOMMENDED: teleport player to (x,z) and aim the UI-free orbit "
                          "camera (comma-separated floats, see Driver.screenshot_orbit)")
    ap.add_argument("--wait", type=float, default=1.0,
                     help="seconds to settle after camera move before capture (default 1.0)")
    args = ap.parse_args()

    if not args.screenshot:
        print("Nothing to do -- pass --screenshot PATH (see module docstring for library usage)",
              file=sys.stderr)
        return 1
    if args.camera and args.orbit:
        print("ERROR: --camera and --orbit are mutually exclusive", file=sys.stderr)
        return 1

    camera = None
    if args.camera:
        parts = [float(x) for x in args.camera.split(",")]
        if len(parts) != 5:
            print("ERROR: --camera needs 5 comma-separated values: x,y,z,yaw,pitch",
                  file=sys.stderr)
            return 1
        camera = tuple(parts)

    orbit = None
    if args.orbit:
        parts = [float(x) for x in args.orbit.split(",")]
        if len(parts) != 5:
            print("ERROR: --orbit needs 5 comma-separated values: x,z,yaw_deg,pitch_deg,dist",
                  file=sys.stderr)
            return 1
        orbit = tuple(parts)

    d = Driver(exe=args.exe)
    print(f"launching {args.exe} ...")
    if not d.launch():
        print("FAILED to connect", file=sys.stderr)
        return 1
    print("connected")
    if orbit is not None:
        ox, oz, oyaw, opitch, odist = orbit
        ok, r = d.screenshot_orbit(args.screenshot, ox, oz, oyaw, opitch, odist,
                                    settle_s=args.wait)
    else:
        ok, r = d.screenshot(args.screenshot, camera=camera, editor_open=camera is not None,
                              settle_s=args.wait)
    print("screenshot:", ok, r)
    d.shutdown()
    if not ok:
        return 1
    if not Path(args.screenshot).exists():
        print(f"ERROR: {args.screenshot} was not created despite ok=true", file=sys.stderr)
        return 1
    print(f"done -> {args.screenshot}")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
