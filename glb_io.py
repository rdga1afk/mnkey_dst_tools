#!/usr/bin/env python3
"""
glb_io.py -- shared GLB (binary glTF) container read/write. Consolidates
3 near-identical reimplementations (morph_gen_b.py, mb_lab_to_bip01.py,
tools/md_mesh_conv/merge_body_morphs.py -- surgical-simplicity audit,
docs/SURGICAL_SIMPLICITY_AUDIT_2026-09.md §2) onto merge_body_morphs.py's
more robust behavior: real error messages (not bare asserts) and a BIN
chunk that's optional on read (some of the 3 originals crashed on a
missing BIN chunk instead of returning empty bin_data). All 3 real call
sites always exercise well-formed GLBs with a real BIN chunk, so this
is added safety, not an observed behavior change. write_glb always
emits the BIN chunk (possibly zero-length) -- 2 of the 3 originals
already did this; the third only omitted it for empty bin_data, which
none of its real callers ever pass.

read_glb returns bin_data as a mutable bytearray, NOT bytes -- this is
load-bearing, not cosmetic: morph_gen_b.py's append_accessor() and
mb_lab_to_bip01.py's append_inv_bind() both do `bin_data += raw`
*without* the caller capturing the return value, relying entirely on
bytearray's in-place __iadd__ extending the SAME object the caller
already holds. Returning immutable bytes here would make that `+=`
silently rebind a local copy instead, and the appended morph/bind-matrix
data would never reach the caller's buffer -- a real corruption bug,
not a style difference (confirmed by reading both call sites before
writing this module, not assumed from the audit's byte-diff finding).

Not a script itself -- has no __main__.
"""
import json
import struct

GLB_MAGIC = 0x46546C67
GLB_VERSION = 2
CHUNK_TYPE_JSON = 0x4E4F534A
CHUNK_TYPE_BIN = 0x004E4942


def _pad4(n: int) -> int:
    return (n + 3) & ~3


def read_glb(path: str):
    """Return (json_dict, bin_bytearray). bin_bytearray is mutable -- see
    module doc comment, callers rely on extending it in place."""
    with open(path, "rb") as fh:
        raw = fh.read()

    if len(raw) < 12:
        raise ValueError(f"{path}: file too short")

    magic, version, _total_len = struct.unpack_from("<III", raw, 0)
    if magic != GLB_MAGIC:
        raise ValueError(f"{path}: bad magic 0x{magic:08X}")
    if version != GLB_VERSION:
        raise ValueError(f"{path}: unsupported glTF version {version}")

    if len(raw) < 20:
        raise ValueError(f"{path}: truncated before JSON chunk")
    c0_len, c0_type = struct.unpack_from("<II", raw, 12)
    if c0_type != CHUNK_TYPE_JSON:
        raise ValueError(f"{path}: chunk 0 is not JSON (got 0x{c0_type:08X})")
    json_start = 20
    json_end = json_start + c0_len
    j = json.loads(raw[json_start:json_end])

    bin_data = bytearray()
    bin_offset = json_end
    if bin_offset + 8 <= len(raw):
        c1_len, c1_type = struct.unpack_from("<II", raw, bin_offset)
        if c1_type == CHUNK_TYPE_BIN:
            bin_data = bytearray(raw[bin_offset + 8: bin_offset + 8 + c1_len])

    return j, bin_data


def write_glb(path: str, j: dict, bin_data: bytes) -> None:
    """Serialise (json_dict, bin_bytes) to a GLB file."""
    json_bytes = json.dumps(j, separators=(",", ":")).encode("utf-8")
    json_pad = _pad4(len(json_bytes)) - len(json_bytes)
    json_chunk = json_bytes + b" " * json_pad

    bin_pad = _pad4(len(bin_data)) - len(bin_data)
    bin_chunk = bytes(bin_data) + b"\x00" * bin_pad

    json_chunk_len = len(json_chunk)
    bin_chunk_len = len(bin_chunk)
    total_len = 12 + 8 + json_chunk_len + 8 + bin_chunk_len

    with open(path, "wb") as fh:
        fh.write(struct.pack("<III", GLB_MAGIC, GLB_VERSION, total_len))
        fh.write(struct.pack("<II", json_chunk_len, CHUNK_TYPE_JSON))
        fh.write(json_chunk)
        fh.write(struct.pack("<II", bin_chunk_len, CHUNK_TYPE_BIN))
        fh.write(bin_chunk)
