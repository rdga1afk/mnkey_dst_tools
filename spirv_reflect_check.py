#!/usr/bin/env python3
"""SPIR-V resource-count reflection prototype (docs/SDL_GPU_ECOSYSTEM_2026-09.md,
dia #2 / Q3): compiles a vert+frag pair with glslangValidator's built-in
reflection dump (-l -q, no new dependency -- spirv-cross/spirv-reflect are
NOT installed in this environment, but glslangValidator already is) and
derives real sampler/uniform-block counts from the compiled SPIR-V, instead
of the hand-filled GpuPipeline::Desc fields the project currently relies on.

This is a standalone CHECK tool, not wired into the build -- proving the
approach works on one real shader pair before deciding whether it's worth
integrating into scripts/compile_shaders.sh or GpuPipeline::Create() itself.

Usage:
    python3 tools/spirv_reflect_check.py <vert.vert> <frag.frag> \\
        --expect-frag-samplers N --expect-frag-uniform-bufs N
"""
import argparse
import re
import subprocess
import sys
import tempfile


def run_reflection(vert_path: str, frag_path: str) -> str:
    with tempfile.NamedTemporaryFile(suffix=".spv") as tmp:
        # -DSPIRV_COMPILE=1 matches scripts/compile_shaders.sh's own glslc
        # invocation -- this project's shaders branch on that define for
        # Vulkan-vs-legacy-GL compat paths (TIER 1 vs TIER 2 there).
        result = subprocess.run(
            ["glslangValidator", "-V", "--target-env", "vulkan1.1",
             "-DSPIRV_COMPILE=1", "-l", "-q", vert_path, frag_path,
             "-o", tmp.name],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            print(result.stdout)
            print(result.stderr, file=sys.stderr)
            sys.exit(f"glslangValidator failed (exit {result.returncode})")
        return result.stdout


# glslangValidator's "Uniform reflection" line format:
#   name: offset X, type Y, size Z, index I, binding B, stages S
# type 8b5e = GL_SAMPLER_2D, 8dc1 = GL_SAMPLER_2D_ARRAY (or similar sampler
# family), 8dd2 = GL_SAMPLER_2D_MULTISAMPLE-adjacent -- anything with a real
# `binding` (not -1) and a sampler-family type is a real descriptor slot.
# index==0/1 with binding==-1 is a UBO MEMBER (lives inside a block), not
# its own binding -- excluded from the sampler count.
UNIFORM_RE = re.compile(
    r"^(\S+): offset (-?\d+), type ([0-9a-f]+), size (\d+), index (-?\d+), "
    r"binding (-?\d+), stages (\d+)$")
BLOCK_RE = re.compile(
    r"^(\S+): offset (-?\d+), type ([0-9a-f]+), size (\d+), index (-?\d+), "
    r"binding (-?\d+), stages (\d+), numMembers (\d+)$")

# Sampler-ish GL type enums that show up as real descriptor bindings in this
# project's shaders (opaque handles, not UBO scalar/vector members).
SAMPLER_TYPES = {"8b5e", "8dc1", "8dd2", "8b60", "8b61"}


def parse_reflection(output: str):
    samplers = []
    blocks = []
    in_uniforms = False
    in_blocks = False
    for line in output.splitlines():
        line = line.strip()
        if line == "Uniform reflection:":
            in_uniforms, in_blocks = True, False
            continue
        if line == "Uniform block reflection:":
            in_uniforms, in_blocks = False, True
            continue
        if line in ("Buffer variable reflection:", "Buffer block reflection:",
                     "Pipeline input reflection:", "Pipeline output reflection:"):
            in_uniforms, in_blocks = False, False
            continue
        if in_uniforms:
            m = UNIFORM_RE.match(line)
            if m:
                name, binding, typ = m.group(1), int(m.group(6)), m.group(3)
                if binding >= 0 and typ in SAMPLER_TYPES:
                    samplers.append((name, binding))
        elif in_blocks:
            m = BLOCK_RE.match(line)
            if m:
                blocks.append((m.group(1), int(m.group(6))))
    return samplers, blocks


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vert")
    ap.add_argument("frag")
    ap.add_argument("--expect-frag-samplers", type=int, default=None,
                     help="value currently hardcoded in the pipeline's "
                          "GpuPipeline::Desc.frag_samplers")
    ap.add_argument("--expect-frag-uniform-bufs", type=int, default=None,
                     help="value currently hardcoded in "
                          "GpuPipeline::Desc.frag_uniform_bufs")
    args = ap.parse_args()

    output = run_reflection(args.vert, args.frag)
    samplers, blocks = parse_reflection(output)

    print(f"=== {args.frag} ===")
    print(f"Real (SPIR-V-derived) fragment samplers: {len(samplers)}")
    for name, binding in sorted(samplers, key=lambda x: x[1]):
        print(f"  binding={binding}: {name}")
    print(f"Real (SPIR-V-derived) fragment uniform blocks: {len(blocks)}")
    for name, binding in sorted(blocks, key=lambda x: x[1]):
        print(f"  binding={binding}: {name}")

    ok = True
    if args.expect_frag_samplers is not None:
        real = len(samplers)
        status = "OK" if real == args.expect_frag_samplers else "MISMATCH"
        if real != args.expect_frag_samplers:
            ok = False
        print(f"\nfrag_samplers: C++ says {args.expect_frag_samplers}, "
              f"SPIR-V has {real} real bindings -- {status}")
        if real < args.expect_frag_samplers:
            print("  (C++ count is HIGHER than reflected -- likely a "
                  "declared-but-unused/dead uniform stripped by the "
                  "compiler, not the dangerous direction, but drift worth "
                  "tracking)")
        elif real > args.expect_frag_samplers:
            print("  (C++ count is LOWER than reflected -- THIS is the "
                  "dangerous direction: SDL_GPU reads garbage/NaN for the "
                  "missing binding)")
    if args.expect_frag_uniform_bufs is not None:
        real = len(blocks)
        status = "OK" if real == args.expect_frag_uniform_bufs else "MISMATCH"
        if real != args.expect_frag_uniform_bufs:
            ok = False
        print(f"frag_uniform_bufs: C++ says {args.expect_frag_uniform_bufs}, "
              f"SPIR-V has {real} real blocks -- {status}")

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
