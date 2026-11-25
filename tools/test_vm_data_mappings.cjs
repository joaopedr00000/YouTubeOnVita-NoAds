// Regression guard for V8 heap mapping permissions on PS Vita.
// For physical hardware proof a prolonged session and a complete core dump
// remain necessary. This test does not execute a PlayStation Vita kernel.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const file = path.join(__dirname,'../experiments/cobalt/vita/port_virtual_memory.cpp');
const source = fs.readFileSync(file,'utf8').replace(/\/\/[^\n]*/g,'');
function part(start,end){
  const a=source.indexOf(start);
  const b=source.indexOf(end,a+start.length);
  assert.ok(a>=0&&b>a,'missing expected function boundary: '+start);
  return source.slice(a,b);
}
const alloc=part('void* SbMemoryMap(', 'bool SbMemoryUnmap(');
const unmap=part('bool SbMemoryUnmap(', 'bool SbMemoryProtect(');
const protect=source.slice(source.indexOf('bool SbMemoryProtect('));
assert.match(alloc,/sceKernelAllocMemBlock\(/);
assert.match(alloc,/SCE_KERNEL_MEMBLOCK_TYPE_USER_RW/);
assert.doesNotMatch(alloc,/kuKernelMemProtect\(/,
  'New data mappings must never change kernel page-table permission');
assert.match(alloc,/validate_flags\(flags,false\)/,
  'SbMemoryMap must reject executable and invalid flags');
assert.match(protect,/m\.exec_used/);
assert.match(protect,/flags&kSbMemoryMapProtectExec/);
assert.match(protect,/kuKernelMemProtect\(address/,
  'JIT executable pages still require actual RX protection');
assert.match(protect,/flags!=0/,
  'Do not revoke memory on decommit; keep V8 data pages mapped');
assert.match(unmap,/sceKernelFreeMemBlock\(/,
  'Unmap must still release whole blocks');
assert.match(source,/kSbMemoryMapProtectReadWrite\|\s*kSbMemoryMapProtectExec/,
  'W+X must remain forbidden');
console.log('PS Vita V8 heap mapping policy and W^X regression guards: PASS');
