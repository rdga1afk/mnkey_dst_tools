#!/usr/bin/env python3
"""Thin read-only RenderDoc MCP server (stdio, JSON-RPC 2.0) over the `renderdoc` Python module of the installed RenderDoc (1.45).

Headless replay of a .rdc file: no GUI, no daemon, no network port, no network code. Use the system python3 that can `import renderdoc`
(the module comes with the RenderDoc packages), ideally under `unshare -rn`:

  claude mcp add renderdoc -- unshare -rn python3 tools/mcp/renderdoc_mcp.py

Tools (read-only): open(path), list_draws(filter, limit, only_draws), draw_state(id), shader_constants(id, stage), pixel(id, x, y, target),
save_texture(id, slot, path, region), diff_draws(a, b), pass_timings(). Errors are one line "ERROR: ...".
"""
import difflib, json, os, re, sys

# stdout is the protocol channel: keep a private copy of it and send everything else (native libraries) to stderr
_OUT = os.fdopen(os.dup(1), "w", buffering=1, encoding="utf-8")
os.dup2(2, 1)

MAX_CHARS = 14000
_rd = None
_cap = None
_ctrl = None
_names = {}
_texs = {}
_actions = {}  # eventId -> (action, depth)
_flat = []     # [(eventId, depth)]


def _mod():
    global _rd
    if _rd is None:
        import renderdoc as rd
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
        _rd = rd
    return _rd


def _clip(text):
    return text if len(text) <= MAX_CHARS else text[:MAX_CHARS] + "\n... [truncated at %d chars]" % MAX_CHARS


def _need():
    if _ctrl is None:
        raise RuntimeError("no capture open: call open(path) first")


def t_open(path):
    global _cap, _ctrl, _names, _texs, _actions, _flat
    rd = _mod()
    if _ctrl is not None:
        _ctrl.Shutdown(); _cap.Shutdown(); _ctrl = _cap = None
    cap = rd.OpenCaptureFile()
    st = cap.OpenFile(os.path.expanduser(path), "", None)
    if not st.OK():
        cap.Shutdown()
        return "ERROR: cannot open: " + st.Message()
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if not res.OK():
        cap.Shutdown()
        return "ERROR: cannot replay: " + res.Message()
    _cap, _ctrl = cap, ctrl
    _names = {r.resourceId: r.name for r in ctrl.GetResources()}
    _texs = {t.resourceId: t for t in ctrl.GetTextures()}
    _actions, _flat = {}, []
    sf = ctrl.GetStructuredFile()

    def walk(a, d):
        _actions[a.eventId] = (a, d, a.GetName(sf))
        _flat.append(a.eventId)
        for c in a.children:
            walk(c, d + 1)
    for r in ctrl.GetRootActions():
        walk(r, 0)
    return "opened %s: %d actions, %d textures, api %s" % (os.path.basename(path), len(_actions), len(_texs), str(ctrl.GetAPIProperties().pipelineType))


def _is_draw(a):
    rd = _mod()
    return bool(a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch))


def t_list_draws(filter="", limit=80, only_draws=False):
    _need()
    rx = re.compile(filter) if filter else None
    out = []
    for eid in _flat:
        a, d, name = _actions[eid]
        if only_draws and not _is_draw(a):
            continue
        if rx and not rx.search(name):
            continue
        extra = (" [%d idx x %d inst]" % (a.numIndices, a.numInstances)) if _is_draw(a) else ""
        out.append("%d %s%s%s" % (eid, "  " * d, name, extra))
        if len(out) >= int(limit):
            out.append("... [limit %d]" % int(limit))
            break
    return "\n".join(out) or "(none)"


def _stage(name):
    rd = _mod()
    return {"vertex": rd.ShaderStage.Vertex, "fragment": rd.ShaderStage.Fragment, "pixel": rd.ShaderStage.Fragment, "compute": rd.ShaderStage.Compute}[str(name).lower()]


def _res(rid):
    return "%s" % _names.get(rid, str(rid))


def _tex(rid):
    t = _texs.get(rid)
    return "%dx%d %s%s" % (t.width, t.height, t.format.Name(), (" x%d" % t.arraysize) if t.arraysize > 1 else "") if t else "-"


def _rid_of(desc):
    return getattr(desc, "resource", None) or getattr(desc, "resourceId", None)


def _path(eid):
    """Marker chain above an event, e.g. DrawScene / DrawTerrainAndProps."""
    chain, depth = [], _actions[eid][1]
    for e in reversed(_flat[:_flat.index(eid)]):
        a, d, name = _actions[e]
        if d < depth:
            chain.append(name)
            depth = d
            if d == 0:
                break
    return " / ".join(reversed(chain)) or "-"


def _state_lines(eid):
    rd = _mod()
    _ctrl.SetFrameEvent(int(eid), True)
    st = _ctrl.GetPipelineState()
    a = _actions.get(int(eid))
    lines = ["event %d: %s%s" % (int(eid), a[2] if a else "?", ("  in " + _path(int(eid))) if a else "")]
    for stage, label in ((rd.ShaderStage.Vertex, "VS"), (rd.ShaderStage.Fragment, "FS"), (rd.ShaderStage.Compute, "CS")):
        sh = st.GetShader(stage)
        if sh != rd.ResourceId.Null():
            refl = st.GetShaderReflection(stage)
            lines.append("%s: %s entry %s, constant blocks %s, resources %d" % (label, _res(sh), st.GetShaderEntryPoint(stage),
                         [c.name for c in refl.constantBlocks] if refl else [], len(refl.readOnlyResources) if refl else 0))
    for i, o in enumerate(st.GetOutputTargets()):
        rid = _rid_of(o)
        if rid and rid != rd.ResourceId.Null():
            lines.append("color target %d: %s (%s)" % (i, _res(rid), _tex(rid)))
    d = _rid_of(st.GetDepthTarget())
    if d and d != rd.ResourceId.Null():
        lines.append("depth target: %s (%s)" % (_res(d), _tex(d)))
    vp = st.GetViewport(0)
    lines.append("viewport %.0f,%.0f %.0fx%.0f" % (vp.x, vp.y, vp.width, vp.height))
    blends = st.GetColorBlends()
    if blends:
        b = blends[0]
        lines.append("blend0: enabled=%s write mask 0x%x" % (b.enabled, b.writeMask))
    ds = st.GetDepthTestState()
    lines.append("depth: test=%s write=%s func=%s" % (ds.depthEnable, ds.depthWrites, ds.depthFunction))
    for stage, label in ((rd.ShaderStage.Vertex, "VS"), (rd.ShaderStage.Fragment, "FS")):
        refl = st.GetShaderReflection(stage)
        if not refl:
            continue
        for u in st.GetReadOnlyResources(stage):
            desc = getattr(u, "descriptor", u)
            rid = _rid_of(desc)
            if rid and rid != rd.ResourceId.Null():
                idx = getattr(getattr(u, "access", None), "index", "?")
                nm = refl.readOnlyResources[idx].name if isinstance(idx, int) and idx < len(refl.readOnlyResources) else ""
                lines.append("%s bind %s %s: %s (%s)" % (label, idx, nm, _res(rid), _tex(rid)))
    return lines


def t_draw_state(id):
    _need()
    return _clip("\n".join(_state_lines(id)))


def _flatten(v, prefix="", out=None):
    out = [] if out is None else out
    if v.members:
        for m in v.members:
            _flatten(m, prefix + v.name + ".", out)
    else:
        cnt = max(1, v.rows) * max(1, v.columns)
        t = str(v.type).lower()  # reflection of our SPIR-V has no field names ("_child0", ...) and often no useful type: floats unless the type says integer
        vals = list(v.value.u32v[:cnt]) if "uint" in t else (list(v.value.s32v[:cnt]) if "sint" in t or t.endswith("int") else list(v.value.f32v[:cnt]))
        out.append("%s%s = %s" % (prefix, v.name, [round(x, 5) if isinstance(x, float) else x for x in vals]))
    return out


def t_shader_constants(id, stage="fragment"):
    _need()
    rd = _mod()
    _ctrl.SetFrameEvent(int(id), True)
    st = _ctrl.GetPipelineState()
    sg = _stage(stage)
    refl = st.GetShaderReflection(sg)
    if not refl:
        return "ERROR: no %s shader at event %s" % (stage, id)
    out = []
    pipe = st.GetComputePipelineObject() if sg == rd.ShaderStage.Compute else st.GetGraphicsPipelineObject()
    for i, cb in enumerate(refl.constantBlocks):
        bind = st.GetConstantBlock(sg, i, 0)
        rid = _rid_of(bind.descriptor) if hasattr(bind, "descriptor") else _rid_of(bind)
        d = bind.descriptor if hasattr(bind, "descriptor") else bind
        vars_ = _ctrl.GetCBufferVariableContents(pipe, st.GetShader(sg), sg, st.GetShaderEntryPoint(sg), i, rid, d.byteOffset, d.byteSize)
        out.append("[block %d %s, %d bytes]" % (i, cb.name, d.byteSize))
        for v in vars_:
            out.extend(_flatten(v))
    return _clip("\n".join(out) or "(no constant blocks)")


def _target(slot, eid):
    rd = _mod()
    _ctrl.SetFrameEvent(int(eid), True)
    st = _ctrl.GetPipelineState()
    s = str(slot)
    if s.startswith("color"):
        outs = st.GetOutputTargets()
        i = int(s[5:] or 0)
        rid = _rid_of(outs[i]) if i < len(outs) else None
    elif s == "depth":
        rid = _rid_of(st.GetDepthTarget())
    else:
        m = re.search(r"(\d+)", s)
        rid = next((r for r in _texs if re.search(r"::%s$" % m.group(1), str(r))), None) if m else None
    if not rid or rid == rd.ResourceId.Null():
        raise ValueError("no such target: " + s)
    return rid


def t_pixel(id, x, y, target=0):
    _need()
    rd = _mod()
    rid = _target("color%s" % target, id)
    v = _ctrl.PickPixel(rid, int(x), int(y), rd.Subresource(0, 0, 0), rd.CompType.Typeless)
    return "event %s color%s (%s) at (%d,%d): %s" % (id, target, _res(rid), int(x), int(y), [round(c, 5) for c in v.floatValue])


def t_save_texture(id, slot, path, region=None):
    _need()
    rd = _mod()
    rid = _target(slot, id)
    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.destType = rd.FileType.PNG
    ts.mip = 0
    ts.slice.sliceIndex = 0
    ts.alpha = rd.AlphaMapping.Discard
    path = os.path.expanduser(path)
    r = _ctrl.SaveTexture(ts, path)
    if hasattr(r, "OK") and not r.OK():
        return "ERROR: save failed: " + r.Message()
    if region:
        from PIL import Image
        x0, y0, x1, y1 = [int(v) for v in (region if isinstance(region, list) else str(region).split(","))]
        Image.open(path).crop((x0, y0, x1, y1)).save(path)
    return "saved %s (%s)%s" % (path, _tex(rid), " cropped to %s" % (region,) if region else "")


def t_diff_draws(a, b):
    _need()
    la, lb = _state_lines(a), _state_lines(b)
    d = [l for l in difflib.unified_diff(la, lb, "event %s" % a, "event %s" % b, lineterm="", n=0) if not l.startswith(("---", "+++", "@@"))]
    return _clip("\n".join(d) or "(identical state)")


def _subtree_ids(a, out):
    out.append(a.eventId)
    for c in a.children:
        _subtree_ids(c, out)
    return out


def t_pass_timings():
    _need()
    rd = _mod()
    if rd.GPUCounter.EventGPUDuration not in _ctrl.EnumerateCounters():
        return "ERROR: GPU duration counter not available for this capture / driver"
    res = _ctrl.FetchCounters([rd.GPUCounter.EventGPUDuration])
    vals = {r.eventId: r.value.d for r in res}
    out = []
    for eid in _flat:
        a, d, name = _actions[eid]
        if d != 0:
            continue
        t = sum(vals.get(e, 0.0) for e in _subtree_ids(a, []))
        if t > 0:
            out.append("%d %s: %.3f ms" % (eid, name, t * 1000))
    return ("per-event GPU duration summed per top-level marker (RenderDoc replay, not the game's own timing; unreliable for sub-5 ms draws on this driver):\n" + "\n".join(out)) if out else "ERROR: no durations returned"


TOOLS = {
    "open": (t_open, "Open a .rdc capture for replay (headless).", {"path": "string"}, ["path"]),
    "list_draws": (t_list_draws, "Action tree with event ids; filter = regex on names, only_draws = draws/dispatches only.", {"filter": "string", "limit": "integer", "only_draws": "boolean"}, []),
    "draw_state": (t_draw_state, "Pipeline state at an event: shaders, bound resources, targets with sizes, viewport, blend, depth.", {"id": "integer"}, ["id"]),
    "shader_constants": (t_shader_constants, "Constant buffer values of a shader stage (vertex|fragment|compute) at an event.", {"id": "integer", "stage": "string"}, ["id"]),
    "pixel": (t_pixel, "Value of a colour target at (x, y) after the event.", {"id": "integer", "x": "integer", "y": "integer", "target": "integer"}, ["id", "x", "y"]),
    "save_texture": (t_save_texture, "Save a target (color0.., depth, or a resource number) as PNG; region = [x0,y0,x1,y1].", {"id": "integer", "slot": "string", "path": "string", "region": "array"}, ["id", "slot", "path"]),
    "diff_draws": (t_diff_draws, "Differences between the pipeline state of two events.", {"a": "integer", "b": "integer"}, ["a", "b"]),
    "pass_timings": (t_pass_timings, "GPU time per top-level marker if RenderDoc gives counters for this driver, else an error line.", {}, []),
}


def _tools_list():
    return [{"name": k, "description": v[1], "inputSchema": {"type": "object", "properties": {p: ({"type": t, "items": {"type": "integer"}} if t == "array" else {"type": t}) for p, t in v[2].items()}, "required": v[3]}} for k, v in TOOLS.items()]


def _reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    _OUT.write(json.dumps(msg) + "\n")


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        method, id_ = req.get("method"), req.get("id")
        if id_ is None:
            continue
        if method == "initialize":
            _reply(id_, {"protocolVersion": (req.get("params") or {}).get("protocolVersion", "2024-11-05"), "capabilities": {"tools": {}},
                         "serverInfo": {"name": "renderdoc-readonly", "version": "1.0"}})
        elif method == "tools/list":
            _reply(id_, {"tools": _tools_list()})
        elif method == "ping":
            _reply(id_, {})
        elif method == "tools/call":
            p = req.get("params") or {}
            name, a = p.get("name"), p.get("arguments") or {}
            try:
                if name not in TOOLS:
                    raise ValueError("unknown tool " + str(name))
                text = TOOLS[name][0](**a)
                err = text.startswith("ERROR:")
            except Exception as e:  # one line, no traceback
                text, err = "ERROR: %s: %s" % (type(e).__name__, str(e).splitlines()[0] if str(e) else ""), True
            _reply(id_, {"content": [{"type": "text", "text": text}], "isError": err})
        else:
            _reply(id_, error={"code": -32601, "message": "method not found: " + str(method)})


if __name__ == "__main__":
    main()
