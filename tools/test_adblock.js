// node tools/test_adblock.js
const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");
const path = require("node:path");
const source = fs.readFileSync(path.join(__dirname,"../experiments/cobalt/vita/adblock/player_filter.js"),"utf8");
function make(host="www.youtube.com",pathname="/tv") {
  const c=vm.createContext({location:{protocol:"https:",hostname:host,pathname}});
  vm.runInContext("window=this; this.Response=class {json(){return Promise.resolve({playabilityStatus:{status:'OK'},streamingData:{formats:[1]},adSlots:[1]});}}; this.XMLHttpRequest=class {get response(){return this.data;}};",c);
  vm.runInContext(source,c);
  return c;
}
(async()=>{
  const c=make();
  const player={playabilityStatus:{status:"OK"},videoDetails:{videoId:"example"},streamingData:{formats:[{itag:18}]},adPlacements:[1],playerAds:[2],adSlots:[3],captions:{tracks:[4]}};
  const plain=JSON.stringify(player);
  const read=(v)=>JSON.parse(vm.runInContext("JSON.stringify(JSON.parse("+JSON.stringify(JSON.stringify(v))+"))",c));
  const expected={...player}; delete expected.adPlacements; delete expected.playerAds; delete expected.adSlots;
  assert.deepEqual(read(player),expected);
  assert.deepEqual(read({playerResponse:player}),{playerResponse:expected});
  assert.deepEqual(read([{playerResponse:player}]),[{playerResponse:expected}]);
  assert.deepEqual(read({adSlots:[1],unrelated:true}),{adSlots:[1],unrelated:true});
  assert.equal(vm.runInContext("JSON.parse('42')",c),42);
  assert.throws(()=>vm.runInContext("JSON.parse('{')",c),/JSON/);
  assert.equal(vm.runInContext("JSON.parse('{\"n\":1}',(k,v)=>k==='n'?2:v).n",c),2);
  vm.runInContext(source,c);
  assert.deepEqual(read(player),expected);
  assert.equal(await vm.runInContext("new Response().json().then(p=>!('adSlots' in p)&&p.streamingData.formats[0]===1)",c),true);
  assert.equal(vm.runInContext("let x=new XMLHttpRequest();x.data={playabilityStatus:{},streamingData:{},playerAds:[1]};x.responseType='json';!('playerAds' in x.response)",c),true);
  for(const other of [make("accounts.google.com"),make("www.youtube.com","/watch"),make("www.youtube.com.evil.test")]) {
    assert.equal(vm.runInContext("JSON.parse("+JSON.stringify(plain)+").adSlots.length",other),1);
  }
  console.log("AdBlock JS fixture tests passed");
})().catch(e=>{console.error(e);process.exitCode=1;});
