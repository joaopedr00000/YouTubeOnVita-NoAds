#!/usr/bin/env python3
"""Prepare a source-only preload patch for the exact Cobalt 25.lts.50 checkout.

This does NOT modify the official precompiled libcobalt.so, and does not
build a runtime. The Vita-compatible custom Evergreen build is a separate task.
"""
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent
source = pathlib.Path(sys.argv[1]) / "cobalt/browser/web_module.cc"
script = (root / "cobalt_adblock_preload.js").read_text(encoding="utf-8")
anchor = (
    "  web_context_->global_environment()->CreateGlobalObject(\n"
    "      window_, web_context_->environment_settings());\n"
)
marker = "// YTV_NOADS_EARLY_PRELOAD"
text = source.read_text(encoding="utf-8")
if marker in text:
    raise SystemExit("Already patched; use a clean Cobalt checkout.")
if text.count(anchor) != 1 or ')YTVNOADS"' in script:
    raise SystemExit("Unexpected source or script; refusing to patch.")
patch = '''
  // YTV_NOADS_EARLY_PRELOAD
  // Only install in the actual YouTube TV document, before its scripts.
  if (data.initial_url.SchemeIs("https") &&
      (data.initial_url.host() == "www.youtube.com" ||
       data.initial_url.host() == "youtube.com") &&
      (data.initial_url.path() == "/tv" ||
       data.initial_url.path().find("/tv/") == 0)) {
    const char kYtvPreload[] = R"YTVNOADS(
''' + script + '''
)YTVNOADS";
    bool preload_ok = false;
    web_context_->script_runner()->Execute(
        kYtvPreload, base::SourceLocation("ytv-adblock-preload.js", 1, 1),
        false, &preload_ok);
    if (!preload_ok) {
      LOG(ERROR) << "YTV_NOADS early preload failed";
    }
  }
'''
source.write_text(text.replace(anchor, anchor + patch, 1), encoding="utf-8")
print("Source patched. A custom Vita-compatible Cobalt build is still required.")
