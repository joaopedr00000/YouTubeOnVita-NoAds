// Source-level checks for Vita V8 mapping behavior. These checks do not
// replace Vita hardware tests or simulate the kernel's TLB implementation.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const src=fs.readFileSync(path.join(__dirname,'../experiments/cobalt/vita/port_virtual_memory.cpp'),'utf8');
const noComments=src.replace(/\/\/[^\n]*/g,'');
const part=(begin,end)=>{const a=noComments.indexOf(begin),b=noComments.indexOf(end,a+begin.length);
  assert.ok(a>=0&&b>a,'missing source boundary: '+begin);return noComments.slice(a,b);};
const map=part('void* SbMemoryMap(','bool SbMemoryUnmap(');
const unmap=part('bool SbMemoryUnmap(','bool SbMemoryProtect(');
const protect=noComments.slice(noComments.indexOf('bool SbMemoryProtect('));
assert.match(map,/sceKernelAllocMemBlock\(/);
assert.match(map,/SCE_KERNEL_MEMBLOCK_TYPE_USER_RW/);
assert.doesNotMatch(map,/kuKernelMemProtect\(/,'Never protect new data mappings');
assert.match(map,/validate_flags\(flags,false\)/);
assert.match(protect,/m\.kernel_pages/,'Track individual kernel-protected pages');
assert.match(protect,/first_touched/,'Protect only touched subranges');
assert.doesNotMatch(protect,/m\.exec_used/,'Block-wide executable flag must be eliminated');
assert.match(protect,/kernel_calls/);
assert.match(protect,/flags!=0/,'Preserve reserved no-op until hardware validation');
assert.match(protect,/kuKernelMemProtect\(address/);
assert.match(protect,/kuKernelMemProtect\(run/);
assert.match(unmap,/free\(m\.kernel_pages\)/);
assert.match(unmap,/sceKernelFreeMemBlock\(/);
assert.match(src,/YTV_VM_TRACE_508K_MAP/);
assert.match(src,/YTV_VM_TRACE_UNMAP/);
assert.match(src,/YTV_VM_TRACE_PROTECT/);
assert.match(src,/\(flags&kSbMemoryMapProtectExec\)\s*&&\s*\(flags&kSbMemoryMapProtectWrite\)/,
  'Reject writable/executable mappings');
console.log('PASS: per-page JIT/kernel protection policy and redacted crash trace');
