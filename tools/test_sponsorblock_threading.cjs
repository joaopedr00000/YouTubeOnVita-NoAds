// Regression checks for the native MediaSession threading contract.
// These tests are intentionally static: actual Vita playback still requires
// hardware validation and, ideally, a PSP2 crash dump.
const fs = require('node:fs');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(
  path.join(__dirname,'../experiments/cobalt/vita/port_media_session.cpp'),'utf8'
);
const player = fs.readFileSync(
  path.join(__dirname,'../experiments/cobalt/vita/port_player.cpp'),'utf8'
);
function part(begin,end) {
  const i=source.indexOf(begin);
  const j=source.indexOf(end,i+begin.length);
  assert.ok(i>=0&&j>i, 'Expected sections: '+begin+' → '+end);
  return source.slice(i,j);
}
const worker=part('void* sb_worker(void*)','void sb_update(');
const callback=part('void sb_action_on_main(void*)','bool sb_schedule_action_locked(');
const scheduler=part('bool sb_schedule_action_locked(','void sb_reset_locked()');
const reset=part('void sb_reset_locked()','void* sb_worker(void*)');
const updater=part('void sb_update(','} // namespace');

assert.match(source,/#include "starboard\/event\.h"/);
assert.doesNotMatch(worker,/sb_invoke\s*\(/,
  'Detached SponsorBlock worker must never call Cobalt callbacks');
assert.match(worker,/sb_schedule_action_locked\(kSbSkipAd\)/);
assert.match(worker,/sb_schedule_action_locked\(kSbSeekTo/);
assert.match(scheduler,/SbEventSchedule\(sb_action_on_main,nullptr,0\)/);
assert.match(reset,/sb_clear_action_locked\(\)/);
assert.match(updater,/sb_clear_action_locked\(\)/);
assert.match(callback,/sb_action_generation==sb_generation/);
assert.match(callback,/pthread_mutex_unlock\(&sb_mutex\)/);
assert.match(callback,/if\(invoke\)\s*\{\s*invoke\(action,context\)/,
  'Cobalt action should be called only after releasing sb_mutex on the main loop');
assert.ok(callback.indexOf('pthread_mutex_unlock(&sb_mutex)')<
          callback.indexOf('invoke(action,context)'));
assert.match(callback,/sb_playing && !ad_skip_available/);
assert.match(callback,/g_ytv_adblock_enabled/);
assert.match(source,/30000000ull/,'Throttle event scheduling logs');
assert.match(player,/log_free_memory\("playback_30s"\)/);
assert.match(player,/nowt-last_playback_memory>=30000000ull/);
console.log('SponsorBlock main-thread dispatch and playback memory diagnostics: OK');
