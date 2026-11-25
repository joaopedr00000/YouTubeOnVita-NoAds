const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const context = vm.createContext({});
const source = fs.readFileSync(path.join(__dirname, 'cobalt_adblock_preload.js'), 'utf8');
vm.runInContext(source, context);
vm.runInContext(source, context); // idempotent install
function parse(input) {
  const code = 'JSON.stringify(JSON.parse(' + JSON.stringify(JSON.stringify(input)) + '))';
  return JSON.parse(vm.runInContext(code, context));
}
const video = { responseContext: {}, playabilityStatus: {status:'OK'},
  adPlacements:[{ad:1}], playerAds:[{}], adSlots:[{}],
  streamingData:{formats:[{url:'https://example.invalid/video'}]},
  videoDetails:{videoId:'example'} };
assert.deepEqual(parse(video), {...video, adPlacements:[],playerAds:[],adSlots:[]});
const browse = {responseContext:{},contents:[{adSlotRenderer:{}},
  {videoRenderer:{title:'Normal video'}},{promotedVideoRenderer:{}},
  {command:{reelWatchEndpoint:{adClientParams:{isAd:true}}}},
  {command:{reelWatchEndpoint:{adClientParams:{isAd:false}}}}]};
assert.deepEqual(parse(browse).contents,[browse.contents[1],browse.contents[4]]);
assert.deepEqual(parse({someProperty:{adPlacements:[1]}}),{someProperty:{adPlacements:[1]}});
assert.deepEqual(parse(null), null);
assert.equal(vm.runInContext('JSON.parse("{\\"adPlacements\\":[1]}", (k,v)=>v).adPlacements.length', context),1);
assert.throws(()=>vm.runInContext('JSON.parse("{")', context), /JSON|property name|Unexpected/);
console.log('Cobalt ad filter: all synthetic fixtures passed');
