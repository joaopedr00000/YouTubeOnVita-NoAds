/*
 * Starboard 15 Vita virtual-memory mapping on sceKernelMemBlock/kubridge.
 *
 * Supports single-region allocations, page-aligned protect subranges, and
 * exact-size unmap. Partial unmap and cross-region unmap fail closed.
 * W^X enforced. Executable permission needs kubridge on hardware.
 */
#include "starboard/memory.h"
#include "starboard/log.h"
#include <psp2/kernel/sysmem.h>
#include <kubridge.h>
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>

namespace {
constexpr uint64_t kPageSize=4096u;
// Cobalt's musl allocator maps ~508 KiB chunks; 64 slots capped it at ~32 MiB.
constexpr int kMaxMaps=1024;
constexpr uint64_t kMaximum=128u*1024u*1024u;
struct Mapping {
  void* base;
  SceUID memid;
  uint64_t requested;
  uint64_t aligned;
  // Pages already returned by a partial SbMemoryUnmap (bit set = released).
  // A Vita memblock cannot be split, so released pages are made inaccessible
  // and the memblock is freed once every page has been released.
  uint8_t* released;
  uint32_t released_pages;
  // Set once any range of this mapping was made executable (V8 JIT code
  // space). Only such mappings need real permission changes afterwards.
  bool exec_used;
};
Mapping maps[kMaxMaps]={};
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
bool validate_flags(int flags,bool allow_exec) {
  const int known=kSbMemoryMapProtectRead|
                  kSbMemoryMapProtectWrite|kSbMemoryMapProtectExec;
  // flags==0 is kSbMemoryMapProtectReserved: address space that is not
  // accessible until SbMemoryProtect() (Cobalt's musl allocator relies on it).
  // Vita cannot reserve without committing, so it is backed by a memblock
  // protected KU_KERNEL_PROT_NONE.
  if((flags&~known)!=0)return false;
  if(!allow_exec && (flags&kSbMemoryMapProtectExec))return false;
  if((flags&kSbMemoryMapProtectWrite) && !(flags&kSbMemoryMapProtectRead))
    return false;
  if((flags&kSbMemoryMapProtectExec) &&
     (flags&kSbMemoryMapProtectWrite))return false;
  return true;
}
unsigned prot(int flags) {
  return ((flags&kSbMemoryMapProtectRead)?KU_KERNEL_PROT_READ:0u) |
         ((flags&kSbMemoryMapProtectWrite)?KU_KERNEL_PROT_WRITE:0u) |
         ((flags&kSbMemoryMapProtectExec)?KU_KERNEL_PROT_EXEC:0u);
}
volatile int g_vm_log_lines=0;
void vm_log(const char* fmt,...) __attribute__((format(printf,1,2)));
volatile int g_vm_fail_lines=0;
void vm_log(const char* fmt,...){
  // Failures (YTV_LIFECYCLE_VM_*) have their own budget: the 400-line cap
  // used to be exhausted by routine map/unmap lines, hiding later failures.
  if(strstr(fmt,"LIFECYCLE")){
    if(__sync_add_and_fetch(&g_vm_fail_lines,1)>200)return;
  } else if(__sync_add_and_fetch(&g_vm_log_lines,1)>400)return;
  char line[200];
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(line,sizeof(line),fmt,ap);
  va_end(ap);
  SbLogRaw(line);
}
bool safe_align(int64_t wanted,uint64_t* rounded) {
  if(wanted<=0 || (uint64_t)wanted>kMaximum)return false;
  *rounded=((uint64_t)wanted+kPageSize-1)&~(kPageSize-1);
  return *rounded <= kMaximum;
}
}
extern "C" {
int ytv_memory_map_count(){
  pthread_mutex_lock(&lock);
  int n=0;
  for(int i=0;i<kMaxMaps;i++)if(maps[i].base)n++;
  pthread_mutex_unlock(&lock);
  return n;
}
unsigned ytv_memory_map_bytes(){
  pthread_mutex_lock(&lock);
  uint64_t b=0;
  for(int i=0;i<kMaxMaps;i++)if(maps[i].base)b+=maps[i].aligned;
  pthread_mutex_unlock(&lock);
  return (unsigned)b;
}
void* SbMemoryMap(int64_t size_bytes,int flags,const char *name) {
  static volatile int mapped=0;
  int num=__sync_add_and_fetch(&mapped,1);
  if(num<=70){
    char message[180];
    snprintf(message,sizeof(message),
      "YTV_PLATFORM_MEMORY_MAP n=%d size=%lld flags=%d label=%s\n",
      num,(long long)size_bytes,flags,name?name:"");
    SbLogRaw(message);
  }
  uint64_t size=0;
  if(!safe_align(size_bytes,&size) || !validate_flags(flags,false)){
    vm_log("YTV_LIFECYCLE_VM_MAP_FAIL n=%d reason=args size=%lld flags=%d\n",num,(long long)size_bytes,flags);
    return SB_MEMORY_MAP_FAILED;
  }
  SceUID uid=sceKernelAllocMemBlock("YTStarboardMap",
               SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,(SceSize)size,nullptr);
  if(uid<0){
    vm_log("YTV_LIFECYCLE_VM_MAP_FAIL n=%d reason=alloc err=0x%x size=%u\n",num,(unsigned)uid,(unsigned)size);
    return SB_MEMORY_MAP_FAILED;
  }
  void* base=nullptr;
  if(sceKernelGetMemBlockBase(uid,&base)<0 || !base) {
    sceKernelFreeMemBlock(uid);
    return SB_MEMORY_MAP_FAILED;
  }
  // All non-executable allocations must start RW, INCLUDING read-only
  // requests. The data-mapping branch of SbMemoryProtect() deliberately
  // acknowledges subsequent Read/ReadWrite/Reserved transitions without
  // performing kuKernelMemProtect(): the Vita kernel may invalidate adjacent
  // V8 heap pages during partial protection changes. Mapping initial Read
  // requests as truly RO was therefore inconsistent: a later ReadWrite
  // commit would report success while leaving the page RO in the kernel.
  //
  // Two hardware coredump fragments (November 2026) both show a Cobalt V8
  // Data Abort (stop 0x30004, DFSR=7) on the same object word 0x8c644600,
  // with r0=0x8c644601. This is not proof that initial RO caused the fault,
  // but avoiding ALL initial data-page protection changes eliminates one
  // plausible source of stale/inconsistent page-table state. Only mappings
  // that become JIT EXEC are ever changed via kuKernelMemProtect (below).
  // Nothing in SbMemoryMap can request executable permission.

  pthread_mutex_lock(&lock);
  int index=-1;
  for(int i=0;i<kMaxMaps;i++)if(!maps[i].base){index=i;break;}
  if(index>=0)maps[index]={base,uid,(uint64_t)size_bytes,size,nullptr,0,false};
  pthread_mutex_unlock(&lock);
  if(index<0) {
    vm_log("YTV_LIFECYCLE_VM_MAP_FAIL n=%d reason=table_full\n",num);
    sceKernelFreeMemBlock(uid);
    return SB_MEMORY_MAP_FAILED;
  }
  vm_log("YTV_VM_MAP n=%d base=%p size=%u flags=%d\n",num,base,(unsigned)size,flags);
  return base;
}
bool SbMemoryUnmap(void* address,int64_t size_bytes) {
  // munmap semantics: any page-aligned range inside one mapping may be
  // released (musl/V8 reserve extra space and trim it for alignment).
  if(!address||size_bytes<=0)return false;
  uintptr_t begin=(uintptr_t)address;
  uint64_t rounded=0;
  if((begin&(kPageSize-1)) || !safe_align(size_bytes,&rounded))return false;
  pthread_mutex_lock(&lock);
  int index=-1;
  for(int i=0;i<kMaxMaps;i++) {
    uintptr_t b=(uintptr_t)maps[i].base;
    if(maps[i].base && begin>=b && (uint64_t)(begin-b)+rounded<=maps[i].aligned) {
      index=i;break;
    }
  }
  if(index<0) {
    pthread_mutex_unlock(&lock);
    vm_log("YTV_LIFECYCLE_VM_UNMAP_FAIL %p size=%lld not inside a mapping\n",address,(long long)size_bytes);
    return false;
  }
  Mapping& m=maps[index];
  uint32_t total_pages=(uint32_t)(m.aligned/kPageSize);
  uint32_t first=(uint32_t)((begin-(uintptr_t)m.base)/kPageSize);
  uint32_t count=(uint32_t)(rounded/kPageSize);
  int result=0;
  if(first==0 && count==total_pages && m.released_pages==0) {
    result=sceKernelFreeMemBlock(m.memid);  // whole mapping at once
    if(result>=0){free(m.released);m={};}
  } else {
    if(!m.released) {
      m.released=(uint8_t*)calloc((total_pages+7)/8,1);
      if(!m.released){pthread_mutex_unlock(&lock);return false;}
    }
    // Released pages are NOT protected any more. Hardware crash dumps
    // (v0.25 and v0.29) show a data abort in V8's object-type checks on a
    // page of such a block that was readable again in the dump; the suspected
    // cause is that removing access from a sub-range of a live block
    // (kuKernelMemProtect PROT_NONE) briefly affected neighbouring live pages.
    // Nothing reads memory it released; the block is freed once all of its
    // pages are released.
    result=0;
    {
      for(uint32_t p=first;p<first+count;p++) {
        if(!(m.released[p/8]&(1u<<(p%8)))) {
          m.released[p/8]|=(uint8_t)(1u<<(p%8));
          m.released_pages++;
        }
      }
      if(m.released_pages==total_pages) {
        result=sceKernelFreeMemBlock(m.memid);
        if(result>=0){free(m.released);m={};}
      }
    }
  }
  pthread_mutex_unlock(&lock);
  vm_log("YTV_VM_UNMAP %p size=%lld pages=%u+%u/%u result=0x%x\n",address,
         (long long)size_bytes,first,count,total_pages,(unsigned)result);
  return result>=0;
}
bool SbMemoryProtect(void* address,int64_t size_bytes,int flags) {
  if(flags&kSbMemoryMapProtectExec)
    vm_log("YTV_VM_PROTECT_EXEC %p size=%lld flags=%d\n",address,(long long)size_bytes,flags);
  if(!address || size_bytes<=0 || !validate_flags(flags,true)){
    vm_log("YTV_LIFECYCLE_VM_PROTECT_REJECT %p size=%lld flags=%d\n",address,(long long)size_bytes,flags);
    return false;
  }
  uintptr_t begin=(uintptr_t)address;
  if(begin&(kPageSize-1))return false;
  uint64_t rounded=0;
  if(!safe_align(size_bytes,&rounded))return false;
  pthread_mutex_lock(&lock);
  bool within=false;
  bool kernel=false;  // a real page-table change is needed
  for(int i=0;i<kMaxMaps;i++) {
    Mapping& m=maps[i];
    if(!m.base)continue;
    uintptr_t b=(uintptr_t)m.base;
    if(begin>=b && (uint64_t)(begin-b)+rounded<=m.aligned) {
      within=true;
      // Pages released by a partial unmap no longer exist for the caller.
      if(m.released) {
        uint32_t first=(uint32_t)((begin-b)/kPageSize);
        for(uint32_t p=first;p<first+(uint32_t)(rounded/kPageSize);p++)
          if(m.released[p/8]&(1u<<(p%8))){within=false;break;}
      }
      // Data mappings are RW for their whole life (see SbMemoryMap): commit
      // (R/RW) and decommit (0) requests need no page-table change. Only
      // JIT code mappings switch RW <-> RX for real.
      if(within && (flags&kSbMemoryMapProtectExec)) m.exec_used=true;
      kernel=within && m.exec_used && flags!=0;
      break;
    }
  }
  // Data mappings: every request is acknowledged without touching the page
  // tables (they stay RW). Mappings that held JIT code: R/RW/RX go to the
  // kernel; decommit (0) is acknowledged, the pages stay mapped but unused.
  int result=!within?-1:!kernel?0:kuKernelMemProtect(address,(SceSize)rounded,prot(flags));
  pthread_mutex_unlock(&lock);
  if(result<0)
    vm_log("YTV_LIFECYCLE_VM_PROTECT_FAIL %p size=%lld flags=%d within=%d err=0x%x\n",address,(long long)size_bytes,flags,within,(unsigned)result);
  return result>=0;
}
} // extern C

