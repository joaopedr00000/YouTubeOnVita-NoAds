// Starboard 15 on PlayStation Vita: first actual platform functions.
// This is NOT a browser runtime and implements only the APIs tested here.
#include "starboard/time.h"
#include "starboard/log.h"
#include "starboard/memory.h"
#include "starboard/system.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <pthread.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>

namespace {
constexpr char kLogDir[] = "ux0:data/youtubevita-noads";
constexpr char kLogPath[] = "ux0:data/youtubevita-noads/starboard15.log";
constexpr int64_t kEpoch1601To1970Us = 11644473600000000LL;
pthread_once_t g_log_dir_once=PTHREAD_ONCE_INIT;
bool g_verbose=false;
void create_log_dir_once(){
  sceIoMkdir(kLogDir,0777);
  // Diagnostic YTV_* lines cost an open/write/close on the memory card each;
  // write them only when ux0:data/youtubevita-noads/verbose.txt exists.
  SceIoStat st;
  g_verbose=sceIoGetstat("ux0:data/youtubevita-noads/verbose.txt",&st)>=0;
}
bool always_logged(const char* m){
  return strncmp(m,"YTV_",4)!=0 || strncmp(m,"YTV_BREAK",9)==0 ||
         strncmp(m,"YTV_BACKTRACE",13)==0 || strncmp(m,"YTV_VGL_INIT",12)==0 ||
         strncmp(m,"YTV_LIFECYCLE",13)==0 ||
         // Bounded crash metadata in this experimental branch; no verbose.txt
         // required and no unrelated per-frame/storage verbose logging.
         strncmp(m,"YTV_VM_TRACE_",sizeof("YTV_VM_TRACE_")-1)==0;
}
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
  pthread_once(&g_log_dir_once,create_log_dir_once);
  if(!g_verbose && !always_logged(message))return;
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

// ---- large allocations outside the newlib heap (YouTube Lite) -------------
// The Lite build has a 64 MiB newlib heap. Media segments (hundreds of KiB to
// a few MiB each, SbMemoryAllocate) fragmented it: opening a live stream on
// hardware (v0.48) failed 1-2 MiB allocations with only ~41 MiB in use, and
// Cobalt crashed right after. Large blocks now get their own memblock (from
// the free user memory) and go back to the system when freed.
// A 64-byte header in front of the returned pointer keeps the memblock id.
extern "C" bool ytv_lowmem() __attribute__((weak));
namespace {
constexpr size_t kLargeAlloc = 256 * 1024;
constexpr size_t kLargeHeader = 64;
constexpr uint32_t kLargeMagic = 0x59544C42;  // "YTLB"
struct LargeHeader { uint32_t magic; SceUID uid; uint32_t size; uint32_t block; };
uintptr_t g_heap_lo = 0, g_heap_hi = 0;
// Registry of live large blocks (open addressing on the returned pointer):
// a pointer is only treated as a large block if it is registered here, so a
// wrong heap range can never send a newlib pointer to large_free().
constexpr int kLargeSlots = 2048;
void* volatile g_large_slots[kLargeSlots];
pthread_mutex_t g_large_lock = PTHREAD_MUTEX_INITIALIZER;
inline unsigned slot_of(void* p) { return ((uintptr_t)p >> 12) * 2654435761u % kLargeSlots; }
bool registry_add(void* p) {
  pthread_mutex_lock(&g_large_lock);
  unsigned i = slot_of(p);
  for (int n = 0; n < kLargeSlots; n++, i = (i + 1) % kLargeSlots) {
    if (!g_large_slots[i] || g_large_slots[i] == (void*)1) {
      g_large_slots[i] = p;
      pthread_mutex_unlock(&g_large_lock);
      return true;
    }
  }
  pthread_mutex_unlock(&g_large_lock);
  return false;
}
// Removes p if registered; true when it was.
bool registry_take(void* p, bool remove) {
  pthread_mutex_lock(&g_large_lock);
  unsigned i = slot_of(p);
  for (int n = 0; n < kLargeSlots; n++, i = (i + 1) % kLargeSlots) {
    void* s = g_large_slots[i];
    if (!s) break;
    if (s == p) {
      if (remove) g_large_slots[i] = (void*)1;  // tombstone
      pthread_mutex_unlock(&g_large_lock);
      return true;
    }
  }
  pthread_mutex_unlock(&g_large_lock);
  return false;
}
pthread_once_t g_heap_once = PTHREAD_ONCE_INIT;
// g_large_on: the heap range is known, so large blocks can be told apart.
// g_large_first: Lite sends every large block to its own memblock. Full
// (v0.65) only does so when the newlib heap cannot satisfy the request:
// switching 720p -> 480p -> 720p on hardware failed a 2.1 MiB allocation with
// 57 MiB of the 112 MiB heap in use (fragmentation) and Cobalt copied into
// the NULL it got back (logs-psv\v064, pc=cobalt+0x1729e88 in memcpy), while
// ~80 MiB of user memory outside the heap was free.
bool g_large_on = false;
bool g_large_first = false;
volatile int g_large_rescues = 0;
void heap_range_once() {
  g_large_first = ytv_lowmem && ytv_lowmem();
  g_large_on = true;
  void* probe = malloc(16);
  SceKernelMemBlockInfo info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  if (probe && sceKernelGetMemBlockInfoByAddr(probe, &info) >= 0 && info.mappedBase) {
    g_heap_lo = (uintptr_t)info.mappedBase;
    g_heap_hi = g_heap_lo + info.mappedSize;
  } else {
    g_large_on = false;  // unknown heap range: never route frees elsewhere
  }
  free(probe);
  char line[120];
  snprintf(line, sizeof(line), "YTV_LIFECYCLE_LARGE_ALLOC on=%d first=%d heap=%p-%p\n",
           g_large_on ? 1 : 0, g_large_first ? 1 : 0, (void*)g_heap_lo, (void*)g_heap_hi);
  SbLogRaw(line);
}
// Large blocks go to their own memblock before trying the heap (Lite).
inline bool large_enabled() {
  pthread_once(&g_heap_once, heap_range_once);
  return g_large_on && g_large_first;
}
// Large blocks may use their own memblock when the heap failed (both builds).
inline bool large_fallback() {
  pthread_once(&g_heap_once, heap_range_once);
  return g_large_on;
}
// A pointer outside the newlib heap was returned by large_alloc().
inline bool is_large(void* p) {
  uintptr_t a = (uintptr_t)p;
  if (!p || !g_large_on || (a >= g_heap_lo && a < g_heap_hi)) return false;
  return registry_take(p, false);
}
void* large_alloc(size_t size) {
  uint32_t block = (uint32_t)((size + kLargeHeader + 0xFFF) & ~(size_t)0xFFF);
  SceUID uid = sceKernelAllocMemBlock("YTLargeAlloc", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, block, nullptr);
  if (uid < 0) return nullptr;
  void* base = nullptr;
  if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
    sceKernelFreeMemBlock(uid);
    return nullptr;
  }
  LargeHeader* h = (LargeHeader*)base;
  h->magic = kLargeMagic;
  h->uid = uid;
  h->size = (uint32_t)size;
  h->block = block;
  void* p = (char*)base + kLargeHeader;
  if (!registry_add(p)) {  // registry full: use the newlib heap instead
    sceKernelFreeMemBlock(uid);
    return nullptr;
  }
  return p;
}
void* large_rescue(size_t size, const char* what) {
  void* p = large_alloc(size);
  if (__sync_fetch_and_add(&g_large_rescues, 1) < 8) {
    char line[128];
    snprintf(line, sizeof(line), "YTV_LIFECYCLE_ALLOC_RESCUE %s size=%u ok=%d newlib_used=%d\n", what,
             (unsigned)size, p ? 1 : 0, (int)mallinfo().uordblks);
    SbLogRaw(line);
  }
  return p;
}
LargeHeader* large_header(void* p) {
  LargeHeader* h = (LargeHeader*)((char*)p - kLargeHeader);
  return h->magic == kLargeMagic ? h : nullptr;
}
void large_free(void* p) {
  if (!registry_take(p, true)) return;
  LargeHeader* h = large_header(p);
  if (!h) return;  // not ours: leak rather than corrupt
  SceUID uid = h->uid;
  h->magic = 0;
  sceKernelFreeMemBlock(uid);
}
}  // namespace

void* SbMemoryAllocateUnchecked(size_t size) {
  if (size >= kLargeAlloc && large_enabled()) {
    if (void* p = large_alloc(size)) return p;
  }
  void* p = malloc(size);
  if (!p && size >= kLargeAlloc && large_fallback()) p = large_rescue(size, "allocate");
  return p;
}
void SbMemoryFree(void* memory) {
  if (is_large(memory)) { large_free(memory); return; }
  free(memory);
}
void* SbMemoryReallocateUnchecked(void* memory, size_t size) {
  if (!memory) return SbMemoryAllocateUnchecked(size);
  if (!size) { SbMemoryFree(memory); return nullptr; }
  if (is_large(memory)) {
    LargeHeader* h = large_header(memory);
    size_t old = h ? h->size : 0;
    if (h && size <= h->block - kLargeHeader && size >= kLargeAlloc) {
      h->size = (uint32_t)size;  // still fits its memblock
      return memory;
    }
    void* n = SbMemoryAllocateUnchecked(size);
    if (!n) return nullptr;
    memcpy(n, memory, old < size ? old : size);
    large_free(memory);
    return n;
  }
  if (size >= kLargeAlloc && large_enabled()) {
    if (void* n = large_alloc(size)) {
      size_t old = malloc_usable_size(memory);
      memcpy(n, memory, old < size ? old : size);
      free(memory);
      return n;
    }
  }
  void* r = realloc(memory, size);
  if (!r && size >= kLargeAlloc && large_fallback()) {
    // realloc failed and left memory untouched: move it to a memblock.
    if (void* n = large_rescue(size, "reallocate")) {
      size_t old = malloc_usable_size(memory);
      memcpy(n, memory, old < size ? old : size);
      free(memory);
      return n;
    }
  }
  return r;
}
void* SbMemoryAllocateAlignedUnchecked(size_t alignment, size_t size) {
  // Large blocks are 64-byte aligned (memblock base + 64-byte header).
  if (size >= kLargeAlloc && alignment <= kLargeHeader && large_enabled()) {
    if (void* p = large_alloc(size)) return p;
  }
  void* p = memalign(alignment, size);
  if (!p && size >= kLargeAlloc && alignment <= kLargeHeader && large_fallback())
    p = large_rescue(size, "allocate_aligned");
  return p;
}
void SbMemoryFreeAligned(void* memory) { SbMemoryFree(memory); }

extern bool ytv_runtime_trace_is_enabled();
bool SbSystemGetPath(SbSystemPathId path_id, char* out_path, int path_length) {
  static volatile int npaths=0;
  int iteration=__sync_add_and_fetch(&npaths,1);
  if(ytv_runtime_trace_is_enabled() && iteration<=80){
    char line[160];
    snprintf(line,sizeof(line),"YTV_SYSTEM_GET_PATH_ENTER #%d id=%d len=%d\n",
             iteration,(int)path_id,path_length);
    SbLogRaw(line);
  }
  const char* path = nullptr;
  switch (path_id) {
    case kSbSystemPathContentDirectory: path = "app0:"; break;
    // Evergreen ships an empty fonts.xml and expects platform system fonts.
    // The VPK carries Cobalt's official 'limited' package (see
    // tools/stage_vita_system_fonts.py) under app0:/system_fonts.
    case kSbSystemPathFontDirectory: path = "app0:/system_fonts"; break;
    case kSbSystemPathFontConfigurationDirectory:
      path = "app0:/system_fonts"; break;
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
  if(ytv_runtime_trace_is_enabled() && iteration<=80){
    char line[180];
    snprintf(line,sizeof(line),"YTV_SYSTEM_GET_PATH_OK #%d %s\n",
             iteration,path);
    SbLogRaw(line);
  }
  return true;
}

}  // extern "C"

