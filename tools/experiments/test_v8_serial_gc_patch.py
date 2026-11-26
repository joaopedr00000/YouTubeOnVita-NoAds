#!/usr/bin/env python3
"""Minimal safe-patcher regression: idempotency and pinned source layout."""
import importlib.util
import tempfile
from pathlib import Path

p = Path(__file__).parent / "patch_v8_serial_gc.py"
spec = importlib.util.spec_from_file_location("patch_v8_serial_gc", p)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.EXPECTED_REV == "762d9bfda894dd22cc5d47200f9994c3027514eb"
assert "--single-threaded-gc" in module.REPLACEMENT
with tempfile.TemporaryDirectory() as td:
    root = Path(td)
    location = root / "cobalt/script/v8c"
    location.mkdir(parents=True)
    source = location / "isolate_fellowship.cc"
    original = (
        'void V8FlagsInit() {\n'
        '  std::vector<std::string> kV8CommandLineFlags = {\n'
        + module.ANCHOR + '\n'
        '  if (!configuration::Configuration::GetInstance()->CobaltEnableJit()) {\n'
        '    kV8CommandLineFlags.push_back("--jitless");\n'
        '  }\n'
        '}\n'
    )
    source.write_text(original)
    module.patch(root)
    new = source.read_text()
    assert new.count('"--single-threaded-gc"') == 1
    assert '--jitless' in new
    try:
        module.patch(root)
    except SystemExit as error:
        assert "already-patched" in str(error)
    else:
        raise AssertionError("Second patch unexpectedly succeeded")
print("PASS: GC-only Cobalt source patch, no JIT / default runtime modification")
