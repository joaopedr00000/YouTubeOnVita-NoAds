// No-build regression check for the v0.0.2 fork data isolation.
// Usage: node tools/test_fork_data_isolation.cjs
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const root = path.resolve(__dirname,'..');
const files = [
  "experiments/cobalt/vita/port_addons.cpp",
  "experiments/cobalt/vita/port_audio_sink.cpp",
  "experiments/cobalt/vita/port_downloads.cpp",
  "experiments/cobalt/vita/port_files.cpp",
  "experiments/cobalt/vita/port_graphics.cpp",
  "experiments/cobalt/vita/port_input.cpp",
  "experiments/cobalt/vita/port_lifecycle.cpp",
  "experiments/cobalt/vita/port_misc_utility.cpp",
  "experiments/cobalt/vita/port_native_download.cpp",
  "experiments/cobalt/vita/port_osk.cpp",
  "experiments/cobalt/vita/port_player.cpp",
  "experiments/cobalt/vita/port_storage.cpp",
  "experiments/cobalt/vita/port_system.cpp",
  "experiments/cobalt/vita/relocate_elf.c",
  "experiments/cobalt/vita/main_test.cpp",
  "experiments/cobalt/vita/platform15.cpp",
  "experiments/cobalt/vita/gn_overlay/starboard/vita/armv7/port_files.cpp",
  "experiments/cobalt/vita/gn_overlay/starboard/vita/armv7/port_storage.cpp",
  "experiments/cobalt/vita/gn_overlay/starboard/vita/armv7/platform15.cpp"
];
const legacy = /ux0:data\/youtubevita(?!-noads)(?:\/|["']|$)/;
for (const filename of files) {
  const source = fs.readFileSync(path.join(root,filename),'utf8');
  assert.ok(source.includes('ux0:data/youtubevita-noads'), filename+': missing fork data root');
  assert.ok(!legacy.test(source), filename+': still references original app data directory');
}
const storage = fs.readFileSync(path.join(root,'experiments/cobalt/vita/port_storage.cpp'),'utf8');
assert.ok(storage.includes('"YouTubeVita-NoAds/storage/v1"'), 'fork storage needs distinct encryption key domain');
const addon = fs.readFileSync(path.join(root,'experiments/cobalt/vita/port_addons.cpp'),'utf8');
assert.ok(addon.includes('ux0:picture/YouTubeVita-NoAds'), 'screenshots should be kept separate too');
console.log('PASS: isolated data root in '+files.length+' runtime/test files');
