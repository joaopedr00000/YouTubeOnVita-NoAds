#!/usr/bin/env python3
"""Experimental Cobalt 25 V8 GC concurrency change, NEVER patch official release.
Only modifies cobalt/script/v8c/isolate_fellowship.cc in a pinned source checkout.
"""
from pathlib import Path
import sys

EXPECTED_REV = "762d9bfda894dd22cc5d47200f9994c3027514eb"
ANCHOR = '      "--novalidate_asm",\\n  };'.replace('\\n', '\n')
REPLACEMENT = (
    '      "--novalidate_asm",\n'
    '      "--single-threaded-gc",  // Vita experiment: no V8 background GC\n'
    '  };'
)
MARKER = '"--single-threaded-gc"'

def patch(path: Path):
    source = path / "cobalt" / "script" / "v8c" / "isolate_fellowship.cc"
    if not source.is_file():
        raise SystemExit(f"missing expected Cobalt 25 source: {source}")
    code = source.read_text(encoding="utf-8")
    if code.count(ANCHOR) != 1 or MARKER in code:
        raise SystemExit("unrecognized or already-patched V8 flags; refusing update")
    if 'kV8CommandLineFlags.push_back("--jitless")' not in code:
        raise SystemExit("JIT handling changed: abort source patch")
    new_code = code.replace(ANCHOR, REPLACEMENT, 1)
    source.write_text(new_code, encoding="utf-8")
    print("V8_SERIAL_GC_PATCH_APPLIED flag=--single-threaded-gc")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: python3 tools/experiments/patch_v8_serial_gc.py COBALT_ROOT")
    patch(Path(sys.argv[1]))
