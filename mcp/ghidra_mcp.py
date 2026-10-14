#!/usr/bin/env python3
"""Thin read-only Ghidra MCP server (stdio, JSON-RPC 2.0), for reading facts from a Ghidra project copy.

No network code, no indexing, no scripts: PyGhidra (shipped with Ghidra 12) opens one program of a project READ-ONLY; every tool only reads.
Run with the venv python that has pyghidra + jpype1 (installed offline from Ghidra's own wheels), ideally under `unshare -rn` (no network):

  claude mcp add ghidra -- unshare -rn /home/rdga1/rdga1prj/tools/ghidra-mcp-venv/bin/python tools/mcp/ghidra_mcp.py \
      --project /home/rdga1/rdga1prj/ghidra_mcp/kenshi_mcp/KenshiProj.gpr --program kenshi_x64.exe

Tools: decompile(target), disasm(addr, n), xrefs_to(addr), xrefs_from(addr), find_functions(regex), find_strings(regex), read_bytes(addr, n).
Errors are one line "ERROR: ...". The JVM starts on the first tool call (about 12 s), not on initialize.
"""
import argparse, json, os, re, sys

# stdout is the protocol channel: keep a private copy of it and send everything else (JVM, native libraries) to stderr
_OUT = os.fdopen(os.dup(1), "w", buffering=1, encoding="utf-8")
os.dup2(2, 1)

GHIDRA_FLATPAK = os.path.expanduser("~/.local/share/flatpak/app/org.ghidra_sre.Ghidra/current/active/files")
MAX_CHARS = 14000

_prog = None
_decomp = None


def _log(msg):
    print("[ghidra_mcp] " + msg, file=sys.stderr, flush=True)


def _open(args):
    """Starts the JVM and opens the program read-only (once)."""
    global _prog, _decomp
    if _prog is not None:
        return
    install = os.environ.get("GHIDRA_INSTALL_DIR") or GHIDRA_FLATPAK + "/lib/ghidra"
    java_home = os.environ.get("JAVA_HOME") or GHIDRA_FLATPAK + "/jdk"
    os.environ["GHIDRA_INSTALL_DIR"], os.environ["JAVA_HOME"] = install, java_home
    import pyghidra, pyghidra.launcher as launcher
    from pathlib import Path

    # the flatpak's support/launch.properties is a link into the sandbox (/var/config) and does not exist outside it: give the JVM the arguments it would carry
    def jvm_args(self):
        self._java_home = Path(java_home)
        return [f"-Dpyghidra.sys.prefix={sys.prefix}", f"-Dpyghidra.sys.executable={sys.executable}", "-Djava.awt.headless=true", "-Xmx3g",
                "-Djava.system.class.loader=ghidra.GhidraClassLoader", "-Dfile.encoding=UTF8", "-Duser.country=US", "-Duser.language=en",
                "-Duser.name=" + os.environ.get("USER", "rdga1")]  # under `unshare -rn` the uid maps to root; the project is owned by the real user
    launcher.HeadlessPyGhidraLauncher._jvm_args = jvm_args
    pyghidra.start(install_dir=install)
    from ghidra.base.project import GhidraProject
    from ghidra.app.decompiler import DecompInterface
    gpr = Path(args.project)
    proj = GhidraProject.openProject(str(gpr.parent), gpr.stem, True)
    _prog = proj.openProgram("/", args.program, True)  # readOnly
    if _prog is None:
        raise RuntimeError("program not found in project: " + args.program)
    _decomp = DecompInterface()
    _decomp.openProgram(_prog)
    _log("opened %s (%d functions)" % (_prog.getName(), _prog.getFunctionManager().getFunctionCount()))


def _addr(text):
    t = str(text).strip()
    if re.fullmatch(r"(0x)?[0-9a-fA-F]+", t):
        a = _prog.getAddressFactory().getAddress(t if t.lower().startswith("0x") else "0x" + t)
        if a is not None:
            return a
    raise ValueError("bad address: " + t)


def _clip(text):
    return text if len(text) <= MAX_CHARS else text[:MAX_CHARS] + "\n... [truncated at %d chars]" % MAX_CHARS


def _func_at(a):
    fm = _prog.getFunctionManager()
    return fm.getFunctionContaining(a)


def _where(a):
    f = _func_at(a)
    return ("%s+0x%x" % (f.getName(), a.subtract(f.getEntryPoint()))) if f is not None else "-"


def t_decompile(target, offset=0):
    from ghidra.util.task import ConsoleTaskMonitor
    f = None
    try:
        f = _func_at(_addr(target))
    except ValueError:
        syms = list(_prog.getSymbolTable().getSymbols(str(target)))
        f = _prog.getFunctionManager().getFunctionAt(syms[0].getAddress()) if syms else None
    if f is None:
        return "ERROR: no function at/named " + str(target)
    r = _decomp.decompileFunction(f, 90, ConsoleTaskMonitor())
    if not r.decompileCompleted():
        return "ERROR: decompile failed: " + str(r.getErrorMessage())
    text = "// %s @ %s\n%s" % (f.getName(), f.getEntryPoint(), r.getDecompiledFunction().getC())
    off = max(0, int(offset))
    if off:  # long functions: read them in MAX_CHARS pieces
        piece = text[off:off + MAX_CHARS]
        return "// chars %d..%d of %d\n%s%s" % (off, off + len(piece), len(text), piece, "" if off + MAX_CHARS >= len(text) else "\n... [continue with offset=%d]" % (off + MAX_CHARS))
    return _clip(text)


def t_disasm(addr, n=20):
    n = max(1, min(int(n), 200))
    it = _prog.getListing().getInstructions(_addr(addr), True)
    out = []
    while it.hasNext() and len(out) < n:
        ins = it.next()
        out.append("%s  %-24s %s" % (ins.getAddress(), " ".join("%02x" % (b & 0xFF) for b in ins.getBytes()), ins))
    return "\n".join(out) or "ERROR: no instructions at " + str(addr)


def t_xrefs_to(addr, limit=60):
    out = []
    for r in _prog.getReferenceManager().getReferencesTo(_addr(addr)):
        out.append("%s  %s  in %s" % (r.getFromAddress(), r.getReferenceType(), _where(r.getFromAddress())))
        if len(out) >= int(limit):
            out.append("... [limit]")
            break
    return "\n".join(out) or "(none)"


def t_xrefs_from(addr, limit=60):
    out = []
    for r in _prog.getReferenceManager().getReferencesFrom(_addr(addr)):
        out.append("%s  %s  -> %s" % (r.getFromAddress(), r.getReferenceType(), r.getToAddress()))
        if len(out) >= int(limit):
            break
    return "\n".join(out) or "(none)"


def t_find_functions(regex, limit=60):
    rx = re.compile(regex)
    out = []
    it = _prog.getFunctionManager().getFunctions(True)
    while it.hasNext():
        f = it.next()
        name = f.getName()
        if rx.search(name):
            out.append("%s  %s" % (f.getEntryPoint(), name))
            if len(out) >= int(limit):
                out.append("... [limit]")
                break
    return "\n".join(out) or "(none)"


def t_find_strings(regex, limit=60):
    rx = re.compile(regex)
    out = []
    it = _prog.getListing().getDefinedData(True)
    while it.hasNext():
        d = it.next()
        if not d.hasStringValue():
            continue
        v = str(d.getValue())
        if rx.search(v):
            out.append("%s  %s  (%d refs)" % (d.getAddress(), json.dumps(v[:120]), len(list(d.getReferenceIteratorTo()))))
            if len(out) >= int(limit):
                out.append("... [limit]")
                break
    return "\n".join(out) or "(none)"


def t_read_bytes(addr, n=64):
    n = max(1, min(int(n), 512))
    a = _addr(addr)
    buf = bytearray()
    for i in range(n):
        buf.append(_prog.getMemory().getByte(a.add(i)) & 0xFF)
    return "\n".join("%s  %s" % (a.add(i), " ".join("%02x" % b for b in buf[i:i + 16])) for i in range(0, len(buf), 16))


TOOLS = {
    "decompile": (t_decompile, "Decompile the function containing an address (0x...) or with a name; offset reads a long function in pieces.", {"target": "string", "offset": "integer"}, ["target"]),
    "disasm": (t_disasm, "Disassemble n instructions from an address.", {"addr": "string", "n": "integer"}, ["addr"]),
    "xrefs_to": (t_xrefs_to, "References to an address (from, type, containing function).", {"addr": "string", "limit": "integer"}, ["addr"]),
    "xrefs_from": (t_xrefs_from, "References from an address.", {"addr": "string", "limit": "integer"}, ["addr"]),
    "find_functions": (t_find_functions, "Functions whose name matches a regex.", {"regex": "string", "limit": "integer"}, ["regex"]),
    "find_strings": (t_find_strings, "Defined strings matching a regex, with reference counts.", {"regex": "string", "limit": "integer"}, ["regex"]),
    "read_bytes": (t_read_bytes, "Hex dump of n bytes (max 512) from an address.", {"addr": "string", "n": "integer"}, ["addr"]),
}


def _tools_list():
    return [{"name": k, "description": v[1], "inputSchema": {"type": "object", "properties": {p: {"type": t} for p, t in v[2].items()}, "required": v[3]}} for k, v in TOOLS.items()]


def _reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    _OUT.write(json.dumps(msg) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True, help="path to the .gpr file of a COPY of the Ghidra project")
    ap.add_argument("--program", default="kenshi_x64.exe")
    args = ap.parse_args()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        method, id_ = req.get("method"), req.get("id")
        if id_ is None:  # notification
            continue
        if method == "initialize":
            _reply(id_, {"protocolVersion": (req.get("params") or {}).get("protocolVersion", "2024-11-05"), "capabilities": {"tools": {}},
                         "serverInfo": {"name": "ghidra-readonly", "version": "1.0"}})
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
                _open(args)
                text = TOOLS[name][0](**a)
                err = text.startswith("ERROR:")
            except Exception as e:  # one line, no traceback
                text, err = "ERROR: %s: %s" % (type(e).__name__, str(e).splitlines()[0] if str(e) else ""), True
            _reply(id_, {"content": [{"type": "text", "text": text}], "isError": err})
        else:
            _reply(id_, error={"code": -32601, "message": "method not found: " + str(method)})


if __name__ == "__main__":
    main()
