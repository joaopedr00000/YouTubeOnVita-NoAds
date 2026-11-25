#!/usr/bin/env python3
"""Prepare a source checkout of Cobalt 25.lts.50 for an opt-in experiment.
Does not patch, build or replace the shipped Evergreen binary.
"""
import argparse
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("checkout", type=Path)
args = p.parse_args()
target = args.checkout / "cobalt/browser/web_module.cc"
source = target.read_text(encoding="utf-8")
script = (Path(__file__).resolve().parents[1] / "experiments/cobalt/vita/adblock/player_filter.js").read_text(encoding="utf-8")
anchor = "  DCHECK(web_context_->GetWindowOrWorkerGlobalScope()->IsWindow());"
marker = "// YTV_ADBLOCK_DOCUMENT_START"
if marker in source:
    raise SystemExit("Already patched")
if source.count(anchor) != 1 or ')YTVAD"' in script:
    raise SystemExit("Unexpected Cobalt source; refusing to patch")
block = r'''
  // YTV_ADBLOCK_DOCUMENT_START
  // Run in the actual document's world, before its queued load starts.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch("ytv-adblock") &&
      data.initial_url.SchemeIs("https") &&
      data.initial_url.host() == "www.youtube.com" &&
      (data.initial_url.path() == "/tv" ||
       data.initial_url.path().compare(0, 4, "/tv/") == 0)) {
    bool succeeded = false;
    web_context_->script_runner()->Execute(
        R"YTVAD(''' + script + r''')YTVAD",
        base::SourceLocation("ytv_adblock.js", 1, 1),
        true /* mute_errors */, &succeeded);
    if (!succeeded) LOG(WARNING) << "YTV AdBlock script did not initialize";
  }
'''
target.write_text(source.replace(anchor, block + anchor), encoding="utf-8")
print("Prepared source experiment; rebuild and revalidate loader ABI before use.")
