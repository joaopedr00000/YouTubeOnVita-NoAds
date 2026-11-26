// Linux host mock for PS Vita mapping calls. No device/hardware semantics
// simulated; this verifies exact spans passed into kuKernelMemProtect.
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>
#include <cstdio>
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>
#include <starboard/memory.h>

namespace {
struct Block {void* data;uint32_t size;};
struct Call {uintptr_t addr;uint32_t size;unsigned flags;int result;};
std::map<SceUID,Block> blocks;
std::vector<Call> calls;
int next_id=12;
int forced_error=0;
}
extern "C" SceUID sceKernelAllocMemBlock(const char*,SceKernelMemBlockType,SceSize size,void*) {
  void* memory=nullptr;
  if(posix_memalign(&memory,4096,size)!=0)return -1;
  const SceUID id=next_id++;
  blocks[id]={memory,size};
  return id;
}
extern "C" int sceKernelGetMemBlockBase(SceUID id,void** out) {
  auto it=blocks.find(id);
  if(it==blocks.end())return -1;
  *out=it->second.data;
  return 0;
}
extern "C" int sceKernelFreeMemBlock(SceUID id) {
  auto it=blocks.find(id);
  if(it==blocks.end())return -1;
  free(it->second.data);
  blocks.erase(it);
  return 0;
}
extern "C" int kuKernelMemProtect(void* addr,SceSize size,unsigned flags) {
  calls.push_back({(uintptr_t)addr,size,flags,forced_error});
  return forced_error;
}
extern "C" void SbLogRaw(const char*) {}
#include "../../experiments/cobalt/vita/port_virtual_memory.cpp"

static void assert_call(size_t i,uintptr_t addr,uint32_t bytes,unsigned perm){
  assert(calls.size()>i);
  assert(calls[i].addr==addr);
  assert(calls[i].size==bytes);
  assert(calls[i].flags==perm);
}
int main() {
  constexpr int RW=kSbMemoryMapProtectReadWrite;
  constexpr int R=kSbMemoryMapProtectRead;
  constexpr int RX=kSbMemoryMapProtectRead|kSbMemoryMapProtectExec;
  constexpr int RWX=kSbMemoryMapProtectRead|kSbMemoryMapProtectWrite|
                    kSbMemoryMapProtectExec;
  void* const m=SbMemoryMap(0x7f000,RW,"508KiB-musl-data");
  assert(m!=SB_MEMORY_MAP_FAILED);
  const uintptr_t b=(uintptr_t)m;
  assert(ytv_memory_map_count()==1);
  assert(ytv_memory_map_bytes()==0x7f000);
  assert(calls.empty());
  // Pure data mappings never invoke kernel-protection code.
  assert(SbMemoryProtect((void*)(b+0x3000),0x10000,RW));
  assert(SbMemoryProtect((void*)(b+0x1000),0x4000,R));
  assert(calls.empty());

  // JIT RX on page 2: one kernel call, and only page 2 is recorded.
  assert(SbMemoryProtect((void*)(b+0x2000),0x1000,RX));
  assert(calls.size()==1);
  assert_call(0,b+0x2000,0x1000,KU_KERNEL_PROT_READ|KU_KERNEL_PROT_EXEC);
  calls.clear();

  // Nonexec request spans 6 pages, but the only changed page is page 2.
  assert(SbMemoryProtect((void*)(b+0x1000),0x6000,RW));
  assert(calls.size()==1);
  assert_call(0,b+0x2000,0x1000,KU_KERNEL_PROT_READ|KU_KERNEL_PROT_WRITE);
  calls.clear();

  // The page-history must survive RX->RW->R->RW.
  assert(SbMemoryProtect((void*)(b+0x2000),0x1000,R));
  assert(SbMemoryProtect((void*)(b+0x2000),0x1000,RW));
  assert(calls.size()==2);
  assert_call(0,b+0x2000,0x1000,KU_KERNEL_PROT_READ);
  assert_call(1,b+0x2000,0x1000,KU_KERNEL_PROT_READ|KU_KERNEL_PROT_WRITE);
  calls.clear();

  // Two JIT spans, only previously-touched contiguous runs are updated.
  assert(SbMemoryProtect((void*)(b+0x7000),0x2000,RX));
  calls.clear();
  assert(SbMemoryProtect((void*)b,0x10000,RW));
  assert(calls.size()==2);
  assert_call(0,b+0x2000,0x1000,KU_KERNEL_PROT_READ|KU_KERNEL_PROT_WRITE);
  assert_call(1,b+0x7000,0x2000,KU_KERNEL_PROT_READ|KU_KERNEL_PROT_WRITE);
  calls.clear();

  // Reserved remains intentionally a no-op in the Vita port.
  assert(SbMemoryProtect((void*)b,0x10000,kSbMemoryMapProtectReserved));
  assert(calls.empty());
  // Failed kernel calls must not mark never-touched pages.
  forced_error=-42;
  assert(!SbMemoryProtect((void*)(b+0x12000),0x1000,RX));
  assert(calls.size()==1 && calls[0].result==-42);
  calls.clear();
  forced_error=0;
  assert(SbMemoryProtect((void*)(b+0x12000),0x1000,RW));
  assert(calls.empty());
  // W+X and unmapped ranges remain rejected.
  assert(!SbMemoryProtect((void*)b,0x1000,RWX));
  assert(!SbMemoryProtect((void*)(b+0x90000),0x1000,RW));
  assert(calls.empty());
  // Partial unmaps are logical; the mapping isn't freed until complete.
  assert(SbMemoryUnmap((void*)(b+0x1000),0x2000));
  assert(!SbMemoryProtect((void*)(b+0x1000),0x1000,RW));
  assert(calls.empty());
  assert(ytv_memory_map_count()==1);
  assert(SbMemoryUnmap((void*)b,0x1000));
  assert(SbMemoryUnmap((void*)(b+0x3000),0x7c000));
  assert(ytv_memory_map_count()==0);
  assert(blocks.empty());

  // Free all bitmap metadata on a direct full unmap, even after JIT work.
  void* const m2=SbMemoryMap(0x4000,RW,"JIT");
  assert(m2!=SB_MEMORY_MAP_FAILED);
  assert(SbMemoryProtect(m2,0x1000,RX));
  assert(SbMemoryUnmap(m2,0x4000));
  assert(blocks.empty());
  std::puts("PASS: page-granular kernel protection, W^X, lifecycle and cleanup");
}
