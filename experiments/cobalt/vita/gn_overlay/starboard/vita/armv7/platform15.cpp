// Starboard 15 on PlayStation Vita: first actual platform functions.
// This is NOT a browser runtime and implements only the APIs tested here.
#include "starboard/time.h"
#include "starboard/log.h"
#include "starboard/memory.h"
#include "starboard/system.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

namespace {
constexpr char kLogDir[] = "ux0:data/youtubevita-noads";
constexpr char kLogPath[] = "ux0:data/youtubevita-noads/starboard15.log";
constexpr int64_t kEpoch1601To1970Us = 11644473600000000LL;
}

extern "C" {

SbTime SbTimeGetNow() {
  timeval tv = {};
  if (gettimeofday(&tv, nullptr) != 0) return 0;
  return kEpoch1601To1970Us + static_cast<int64_t>(tv.tv_sec) * 1000000LL +
         static_cast<int64_t>(tv.tv_usec);
}

SbTimeMonotonic SbTimeGetMonotonicNow() {
  return static_cast<SbTimeMonotonic>(sceKernelGetSystemTimeWide());
}

bool SbTimeIsTimeThreadNowSupported() { return false; }

SbTimeMonotonic SbTimeGetMonotonicThreadNow() {
  // Conservative monotonic fallback; per-thread CPU usage is not available yet.
  return SbTimeGetMonotonicNow();
}

void SbLogRaw(const char* message) {
  if (!message) return;
  sceIoMkdir(kLogDir, 0777);
  int fd = sceIoOpen(kLogPath, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_APPEND, 0666);
  if (fd >= 0) {
    sceIoWrite(fd, message, static_cast<unsigned int>(strlen(message)));
    sceIoClose(fd);
  }
}

void SbLog(SbLogPriority priority, const char* message) {
  (void)priority;
  SbLogRaw(message);
}

void* SbMemoryAllocateUnchecked(size_t size) { return malloc(size); }
void* SbMemoryReallocateUnchecked(void* memory, size_t size) {
  return realloc(memory, size);
}
void* SbMemoryAllocateAlignedUnchecked(size_t alignment, size_t size) {
  return memalign(alignment, size);
}
void SbMemoryFree(void* memory) { free(memory); }
void SbMemoryFreeAligned(void* memory) { free(memory); }

bool SbSystemGetPath(SbSystemPathId path_id, char* out_path, int path_length) {
  const char* path = nullptr;
  switch (path_id) {
    case kSbSystemPathContentDirectory: path = "app0:"; break;
    case kSbSystemPathCacheDirectory: path = "ux0:data/youtubevita-noads/cache"; break;
    case kSbSystemPathDebugOutputDirectory: path = kLogDir; break;
    case kSbSystemPathTempDirectory: path = "ux0:data/youtubevita-noads/tmp"; break;
    case kSbSystemPathExecutableFile: path = "app0:eboot.bin"; break;
    case kSbSystemPathStorageDirectory: path = "ux0:data/youtubevita-noads/storage"; break;
    default: return false;
  }
  size_t n = strlen(path);
  if (!out_path || path_length <= 0 || n + 1 > static_cast<size_t>(path_length))
    return false;
  memcpy(out_path, path, n + 1);
  return true;
}

}  // extern "C"

