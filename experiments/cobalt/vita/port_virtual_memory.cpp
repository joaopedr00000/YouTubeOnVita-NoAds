/*
 * Starboard 15 Vita virtual-memory mapping on sceKernelMemBlock/kubridge.
 *
 * Tracks page-aligned subranges within unsplittable Vita memory blocks.
 * Partial unmaps remain physically backed until all pages are released.
 * W^X enforced. Executable permission requires kubridge.
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
  // Logical released pages; partial unmaps do not remove physical Vita pages.
  uint8_t* released;
  uint32_t released_pages;
  // Bitmap of individual pages on which kernel protection succeeded.
  // Never touch unrelated data pages just because a neighbor became JIT RX.
  // Retain history after RX->R->RW so later RW transitions are not skipped.
  uint8_t* kernel_pages;
  uint32_t kernel_page_count;
  uint32_t map_id;
};
Mapping maps[kMaxMaps]={};
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
bool validate_flags(int flags,bool allow_exec) {
  const int known=kSbMemoryMapProtectRead|
                  kSbMemoryMapProtectWrite|kSbMemoryMapProtectExec;
  // flags==0 is kSbMemoryMapProtectReserved: address space that is not
  // accessible until SbMemoryProtect() (Cobalt's musl allocator relies on it).
  // Vita cannot reserve without committing; the port currently keeps
  // reserved pages RW (known Starboard compatibility limitation).
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
volatile int g_vm_trace_lines=0;
void vm_log(const char* fmt,...) __attribute__((format(printf,1,2)));
volatile int g_vm_fail_lines=0;
void vm_log(const char* fmt,...){
  // Failures (YTV_LIFECYCLE_VM_*) have their own budget: the 400-line cap
  // used to be exhausted by routine map/unmap lines, hiding later failures.
  if(strstr(fmt,"YTV_VM_TRACE")){
    if(__sync_add_and_fetch(&g_vm_trace_lines,1)>1800)return;
  } else if(strstr(fmt,"LIFECYCLE")){
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
  // Three November 2026 Vita dumps show a read-side translation fault,
  // DFSR=7, at Cobalt +0x95a194 within different live 0x7f000 memblocks.
  // This is not proof of a permission fault or a confirmed fix.
  // Nothing in SbMemoryMap can request executable permission.

  pthread_mutex_lock(&lock);
  int index=-1;
  for(int i=0;i<kMaxMaps;i++)if(!maps[i].base){index=i;break;}
  if(index>=0)maps[index]={base,uid,(uint64_t)size_bytes,size,nullptr,0,nullptr,0,(uint32_t)num};
  pthread_mutex_unlock(&lock);
  if(index<0) {
    vm_log("YTV_LIFECYCLE_VM_MAP_FAIL n=%d reason=table_full\n",num);
    sceKernelFreeMemBlock(uid);
    return SB_MEMORY_MAP_FAILED;
  }
  vm_log("YTV_VM_MAP n=%d base=%p size=%u flags=%d\n",num,base,(unsigned)size,flags);
  if(size==0x7f000u)
    vm_log("YTV_VM_TRACE_508K_MAP id=%u uid=0x%x base=%p bytes=%u flags=%d\n",
           (unsigned)num,(unsigned)uid,base,(unsigned)size,flags);
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
  const uint32_t map_id=m.map_id;
  const SceUID memid=m.memid;
  const uintptr_t map_base=(uintptr_t)m.base;
  const uint64_t map_size=m.aligned;
  uint32_t repeated_releases=0;
  uint32_t total_pages=(uint32_t)(m.aligned/kPageSize);
  uint32_t first=(uint32_t)((begin-(uintptr_t)m.base)/kPageSize);
  uint32_t count=(uint32_t)(rounded/kPageSize);
  int result=0;
  if(first==0 && count==total_pages && m.released_pages==0) {
    result=sceKernelFreeMemBlock(m.memid);  // whole mapping at once
    if(result>=0){free(m.released);free(m.kernel_pages);m={};}
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
        } else {
          ++repeated_releases;
        }
      }
      if(m.released_pages==total_pages) {
        result=sceKernelFreeMemBlock(m.memid);
        if(result>=0){free(m.released);free(m.kernel_pages);m={};}
      }
    }
  }
  const uint32_t released_after=m.released_pages;
  pthread_mutex_unlock(&lock);
  vm_log("YTV_VM_UNMAP %p size=%lld pages=%u+%u/%u result=0x%x\n",address,
         (long long)size_bytes,first,count,total_pages,(unsigned)result);
  if(map_size==0x7f000u || repeated_releases)
    vm_log("YTV_VM_TRACE_UNMAP id=%u uid=0x%x base=%p addr=%p pages=%u+%u/%u released=%u duplicates=%u result=0x%x\n",
           map_id,(unsigned)memid,(void*)map_base,address,first,count,total_pages,
           released_after,repeated_releases,(unsigned)result);
  return result>=0;
}
bool SbMemoryProtect(void* address,int64_t size_bytes,int flags) {
  if(!address || size_bytes<=0 || !validate_flags(flags,true)){
    vm_log("YTV_LIFECYCLE_VM_PROTECT_REJECT %p size=%lld flags=%d\n",
           address,(long long)size_bytes,flags);
    return false;
  }
  const uintptr_t begin=(uintptr_t)address;
  if(begin&(kPageSize-1))return false;
  uint64_t rounded=0;
  if(!safe_align(size_bytes,&rounded))return false;
  pthread_mutex_lock(&lock);
  bool within=false;
  bool released_overlap=false;
  int result=-1;
  unsigned kernel_calls=0;
  uint32_t map_id=0;
  SceUID memid=0;
  uintptr_t map_base=0;
  uint32_t kernel_pages=0;
  for(int i=0;i<kMaxMaps;i++) {
    Mapping& m=maps[i];
    if(!m.base)continue;
    const uintptr_t b=(uintptr_t)m.base;
    if(begin>=b && (uint64_t)(begin-b)+rounded<=m.aligned) {
      within=true;
      map_id=m.map_id;
      memid=m.memid;
      map_base=b;
      const uint32_t first=(uint32_t)((begin-b)/kPageSize);
      const uint32_t count=(uint32_t)(rounded/kPageSize);
      const uint32_t total_pages=(uint32_t)(m.aligned/kPageSize);
      if(m.released) {
        for(uint32_t p=first;p<first+count;p++) {
          if(m.released[p/8]&(1u<<(p%8))) {
            within=false;
            released_overlap=true;
            break;
          }
        }
      }
      if(!within)break;
      result=0;
      if(flags&kSbMemoryMapProtectExec) {
        // Record a page ONLY after the kernel protection call succeeds.
        if(!m.kernel_pages) {
          m.kernel_pages=(uint8_t*)calloc((total_pages+7)/8,1);
          if(!m.kernel_pages) {result=-2;break;}
        }
        ++kernel_calls;
        result=kuKernelMemProtect(address,(SceSize)rounded,prot(flags));
        if(result>=0) {
          for(uint32_t p=first;p<first+count;p++) {
            const uint8_t bit=(uint8_t)(1u<<(p%8));
            if(!(m.kernel_pages[p/8]&bit)) {
              m.kernel_pages[p/8]|=bit;
              ++m.kernel_page_count;
            }
          }
        }
      } else if(flags!=0 && m.kernel_pages) {
        // Reprotect ONLY pages previously changed by the kernel, never
        // unrelated neighboring heap/data pages in this same memblock.
        uint32_t p=first;
        while(p<first+count) {
          if(!(m.kernel_pages[p/8]&(1u<<(p%8)))) {++p;continue;}
          const uint32_t first_touched=p++;
          while(p<first+count && (m.kernel_pages[p/8]&(1u<<(p%8)))) ++p;
          ++kernel_calls;
          void* run=(void*)(b+(uintptr_t)first_touched*kPageSize);
          const SceSize run_size=(SceSize)((p-first_touched)*kPageSize);
          result=kuKernelMemProtect(run,run_size,prot(flags));
          if(result<0)break;
        }
      }
      // Reserved (flags==0) remains a no-op, as in the previous version.
      kernel_pages=m.kernel_page_count;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  if(result<0)
    vm_log("YTV_LIFECYCLE_VM_PROTECT_FAIL %p size=%lld flags=%d within=%d released=%d err=0x%x\n",
           address,(long long)size_bytes,flags,within?1:0,
           released_overlap?1:0,(unsigned)result);
  if(kernel_calls)
    vm_log("YTV_VM_TRACE_PROTECT id=%u uid=0x%x block=%p range=%p+%u flags=%d calls=%u touched_pages=%u result=0x%x\n",
           map_id,(unsigned)memid,(void*)map_base,address,(unsigned)rounded,
           flags,kernel_calls,kernel_pages,(unsigned)result);
  return within && result>=0;
}
} // extern C

