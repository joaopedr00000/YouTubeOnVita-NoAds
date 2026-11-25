// Native menu regression checks (no VitaSDK required)
const fs=require('node:fs'), assert=require('node:assert/strict');
const c=fs.readFileSync('experiments/cobalt/vita/port_addons.cpp','utf8');
const media=fs.readFileSync('experiments/cobalt/vita/port_media_session.cpp','utf8');
for (const str of ['kPageTweaks','T("Tweaks", "Modificações")',
 'case 6: open_page(kPageTweaks);break;',
 'else if (g_page == kPageTweaks) open_page(kPageSettings);',
 'if(g_sel==0) toggle_adblock();','g_ytv_sponsorblock_mode,on,__ATOMIC_RELAXED',
 'if (g_page == kPageTweaks) return 3;']) assert.ok(c.includes(str),str);
assert.ok(media.includes('g_ytv_sponsorblock_mode = 0'), 'SponsorBlock must default to off');
assert.ok(c.includes('adblock=%d') && c.includes('sponsorblock=%d'), 'both preferences persisted');
console.log('PASS: Tweaks menu, toggles, persistence, and opt-in defaults');
