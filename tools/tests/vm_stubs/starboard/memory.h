#pragma once
#include <cstdint>
#define SB_MEMORY_MAP_FAILED ((void*)-1)
enum SbMemoryMapFlags {
  kSbMemoryMapProtectReserved = 0,
  kSbMemoryMapProtectRead = 1,
  kSbMemoryMapProtectWrite = 2,
  kSbMemoryMapProtectExec = 4,
  kSbMemoryMapProtectReadWrite = 3
};
extern "C" {
void* SbMemoryMap(int64_t size_bytes,int flags,const char* name);
bool SbMemoryUnmap(void* address,int64_t size_bytes);
bool SbMemoryProtect(void* address,int64_t size_bytes,int flags);
}
