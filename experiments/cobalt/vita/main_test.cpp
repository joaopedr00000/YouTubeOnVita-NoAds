// Real Starboard API 15 call-site test on Vita3K.
// Does not render a fake YouTube interface.
#include "starboard/time.h"
#include "starboard/log.h"
#include "starboard/memory.h"
#include "starboard/system.h"
#include "starboard/cpu_features.h"
#include "starboard/event.h"
#include "starboard/accessibility.h"
#include "starboard/microphone.h"
#include "starboard/speech_synthesis.h"
#include "starboard/drm.h"
#include "starboard/media.h"
#include "starboard/audio_sink.h"
#include "starboard/ui_navigation.h"
#include "starboard/decode_target.h"
#include "starboard/player.h"
#include "starboard/thread.h"
#include <cwchar>
#include "starboard/string.h"
#include "starboard/time_zone.h"
#include "starboard/configuration_constants.h"
#include "starboard/window.h"
#include "starboard/user.h"
#include "starboard/storage.h"
#include "starboard/file.h"
#include "starboard/directory.h"
#include "starboard/mutex.h"
#include "starboard/once.h"
#include "starboard/thread.h"
#include "starboard/condition_variable.h"
#include <pthread.h>
#include "pad_adapter.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

unsigned int sceUserMainThreadStackSize = 1024 * 1024;
unsigned int _newlib_heap_size_user = 16 * 1024 * 1024;

static int errors = 0;
static void check(bool passed, const char* what) {
  char message[180];
  snprintf(message, sizeof(message), "%s %s\n", passed?"PASS":"FAIL", what);
  SbLog(kSbLogPriorityInfo, message);
  if (!passed) errors++;
}

struct SyncTestWorkerArgs {
  SbMutex* mutex;
  SbConditionVariable* cond;
  volatile int* result;
};
static void* sync_worker(void* p) {
  auto* context=static_cast<SyncTestWorkerArgs*>(p);
  if(SbMutexAcquire(context->mutex)==kSbMutexAcquired) {
    *context->result=1;
    SbConditionVariableSignal(context->cond);
    SbMutexRelease(context->mutex);
  }
  return nullptr;
}

static int once_test_count=0;
static void once_test_entry(){++once_test_count;}
static void* starboard_thread_worker(void* context) {
  int* result=static_cast<int*>(context);
  if(result)*result=42;
  return context;
}
extern "C" int ytv_starboard_pump_scheduled(int limit);
static int scheduled_count=0;
static int events_on_main=0;
static pthread_t event_owner={};
static void scheduled_increment(void* data){
  if(data)(*static_cast<int*>(data))++;
  scheduled_count++;
  if(pthread_equal(event_owner,pthread_self()))events_on_main++;
}
struct EventThreadState{
  SbEventId id;
  int wrong_thread_result;
  int* data;
};
static void* worker_schedule_and_wrong_pump(void* arg){
  EventThreadState* s=static_cast<EventThreadState*>(arg);
  s->id=SbEventSchedule(scheduled_increment,s->data,0);
  s->wrong_thread_result=ytv_starboard_pump_scheduled(1);
  return nullptr;
}

struct SoundSource {
  int consumed;
  int status_calls;
};
static void audio_status_test(int*frames,int*offset,bool*playing,bool*eos,void*data){
  SoundSource* s=static_cast<SoundSource*>(data);
  s->status_calls++;
  *frames=2048-s->consumed;
  *offset=s->consumed%2048;
  *playing=true;
  *eos=true;
}
static void audio_consume_test(int frames,void*data){
  SoundSource* s=static_cast<SoundSource*>(data);
  s->consumed+=frames;
}

extern "C" bool ytv_vita_lifecycle_begin(SbEventHandleCallback);
extern "C" int ytv_vita_lifecycle_pump(int);
extern "C" bool ytv_vita_lifecycle_should_exit(int*);
static int native_lifecycle_types[6]={},native_lifecycle_count=0;
static void native_lifecycle_cb(const SbEvent* e){
  if(native_lifecycle_count<6)
    native_lifecycle_types[native_lifecycle_count++]=(int)e->type;
}

struct PlayerTest { int freed; bool presenting; int errors; int needs; };
static PlayerTest g_player_test;
static void player_test_dealloc(SbPlayer,void* c,const void*){static_cast<PlayerTest*>(c)->freed++;}
static void player_test_decoder_status(SbPlayer,void* c,SbMediaType,SbPlayerDecoderState,int){
  static_cast<PlayerTest*>(c)->needs++;
}
static void player_test_status(SbPlayer,void* c,SbPlayerState st,int){
  if(st==kSbPlayerStatePresenting||st==kSbPlayerStateEndOfStream)
    static_cast<PlayerTest*>(c)->presenting=true;
}
static void player_test_error(SbPlayer,void* c,SbPlayerError,const char*){
  static_cast<PlayerTest*>(c)->errors++;
}
int main() {
  sceIoMkdir("ux0:data/youtubevita-noads", 0777);
  SbLogRaw("\nSTART Starboard 15 VitaSDK native API proof\n");
  check(sizeof(void*) == 4, "pointer ABI is 32 bit");
  check(sizeof(SbLogPriority) == 4, "SABI enum size is 4 bytes");
  check(SbTimeGetNow() > 13380163200000000LL, "UTC clock above year 2025");
  SbTimeMonotonic t0 = SbTimeGetMonotonicNow();
  sceKernelDelayThread(40000);
  SbTimeMonotonic t1 = SbTimeGetMonotonicNow();
  check(t1 > t0 && (t1-t0) >= 30000 && (t1-t0) < 1000000,
        "Vita monotonic clock advanced during 40ms delay");
  unsigned char* ptr = static_cast<unsigned char*>(SbMemoryAllocateUnchecked(4096));
  check(ptr != nullptr, "native Starboard heap allocation");
  if (ptr) {
    memset(ptr, 0xA5, 4096);
    bool valid = true;
    for (int i = 0; i < 4096; ++i) if (ptr[i] != 0xA5) valid = false;
    check(valid, "heap is read/write");
    SbMemoryFree(ptr);
  }
  char path[128] = {};
  check(SbSystemGetPath(kSbSystemPathDebugOutputDirectory,path,sizeof(path)),
        "Starboard log path resolved");
  check(strcmp(path,"ux0:data/youtubevita-noads") == 0,
        "Starboard mapped Vita debug-output path");
  SbWindowOptions opts = {};
  SbWindowSetDefaultOptions(&opts);
  check(opts.size.width == 960 && opts.size.height == 544 && !opts.windowed,
        "Starboard fullscreen native Vita display dimensions");
  SbWindow window = SbWindowCreate(nullptr);
  check(SbWindowIsValid(window), "Starboard single fullscreen logical window");
  check(!SbWindowIsValid(SbWindowCreate(nullptr)), "Second logical window rejected");
  SbWindowSize native_size = {};
  check(SbWindowGetSize(window, &native_size) &&
        native_size.width == 960 && native_size.height == 544,
        "Starboard window size query");
  SbInputData events[16] = {};
  int presses = YtvTranslateVitaButtons(0,SCE_CTRL_UP|SCE_CTRL_CROSS,window,events,16);
  check(presses==2 && events[0].key==kSbKeyGamepadDPadUp &&
        events[1].key==kSbKeyGamepad1 &&
        events[0].type==kSbInputEventTypePress,
        "Vita D-pad Up + X translated to real Starboard gamepad events");
  int repeats = YtvTranslateVitaButtons(SCE_CTRL_UP|SCE_CTRL_CROSS,
                                        SCE_CTRL_UP|SCE_CTRL_CROSS,
                                        window, events,16);
  check(repeats==0, "Held buttons do not generate duplicated press events");
  int releases = YtvTranslateVitaButtons(SCE_CTRL_UP|SCE_CTRL_CROSS,
                                         0,window,events,16);
  check(releases==2 && events[0].type==kSbInputEventTypeUnpress &&
        events[1].type==kSbInputEventTypeUnpress,
        "Vita gamepad release events");
  check(SbWindowGetPlatformHandle(window)==window,
        "Starboard supplies its valid native window token to EGL");
  check(SbWindowDestroy(window), "Starboard fullscreen window destroyed");
  // Storage APIs use actual ux0: files; test read/write persistence across close.
  const char* dirpath="ux0:data/youtubevita-noads/sbfiletest";
  const char* filepath="ux0:data/youtubevita-noads/sbfiletest/sample.txt";
  check(SbDirectoryCreate(dirpath),"Starboard Vita creates real filesystem directory");
  check(SbDirectoryCanOpen(dirpath),"Starboard directory opens by path");
  SbFileError io_error=kSbFileErrorFailed;
  bool created=false;
  SbFile f=SbFileOpen(filepath,kSbFileCreateAlways|kSbFileWrite|kSbFileRead,
                      &created,&io_error);
  check(SbFileIsValid(f)&&created&&io_error==kSbFileOk,
        "Starboard file created on Vita ux0 filesystem");
  static const char sample[]="COBALT-STARBOARD-VITA";
  if(f) {
    check(SbFileWrite(f,sample,sizeof(sample)-1)==sizeof(sample)-1,
          "Starboard file writes persistently to ux0");
    check(SbFileSeek(f,kSbFileFromBegin,0)==0,
          "Starboard file seek to beginning");
    char text[sizeof(sample)]={0};
    check(SbFileRead(f,text,sizeof(sample)-1)==sizeof(sample)-1 &&
          strcmp(text,sample)==0,
          "Starboard file reads written bytes correctly");
    SbFileInfo meta={};
    check(SbFileGetInfo(f,&meta)&&meta.size==sizeof(sample)-1 &&
          !meta.is_directory,
          "Starboard file descriptor metadata reflects size");
    check(SbFileClose(f),"Starboard file descriptor closes");
  }
  check(SbFileExists(filepath),"Starboard file exists after closing");
  SbFileInfo pathmeta={};
  check(SbFileGetPathInfo(filepath,&pathmeta)&&
        pathmeta.size==sizeof(sample)-1,
        "Starboard file path metadata remains available");
  SbFile checkfile=SbFileOpen(filepath,kSbFileOpenOnly|kSbFileRead,
                            &created,&io_error);
  char readback[sizeof(sample)]={0};
  check(SbFileIsValid(checkfile)&&!created&&
        SbFileRead(checkfile,readback,sizeof(sample)-1)==sizeof(sample)-1 &&
        strcmp(readback,sample)==0,
        "Starboard file can be reopened from Vita filesystem");
  if(checkfile)SbFileClose(checkfile);
  check(!SbFileIsValid(SbFileOpen("ur0:tai/config.txt",
                    kSbFileOpenOnly|kSbFileRead,&created,&io_error)),
        "Starboard file sandbox refuses access to ur0");
  SbDirectory directory=SbDirectoryOpen(dirpath,&io_error);
  check(SbDirectoryIsValid(directory),"Starboard Vita directory handle opens");
  bool found=false;
  if(directory){
    char entry[256]={0};
    for(int i=0;i<32&&SbDirectoryGetNext(directory,entry,sizeof(entry));i++){
      if(strcmp(entry,"sample.txt")==0)found=true;
    }
  }
  check(found,"Starboard directory enumerates existing Vita file");
  if(directory)check(SbDirectoryClose(directory),"Starboard directory closes");
  {
    // Cobalt passes exactly kSbFileMaxName (its SSL trust store lists
    // <content>/ssl/certs this way); that size must be accepted.
    SbDirectory listing=SbDirectoryOpen(dirpath,&io_error);
    char exact[kSbFileMaxName]={0};
    bool listed=false;
    for(int i=0;listing&&i<32&&SbDirectoryGetNext(listing,exact,sizeof(exact));i++)
      if(strcmp(exact,"sample.txt")==0)listed=true;
    if(listing)SbDirectoryClose(listing);
    check(listed,"directory enumeration accepts a kSbFileMaxName-sized buffer");
  }
  // Real Vita filesystem flush + truncation, exercised only after readback.
  SbFile edit=SbFileOpen(filepath,kSbFileOpenOnly|kSbFileRead|kSbFileWrite,
                        &created,&io_error);
  check(SbFileIsValid(edit),"Starboard editable file reopened");
  if(edit) {
    check(SbFileFlush(edit),"Starboard flushes Vita descriptor");
    check(SbFileTruncate(edit,7),"Starboard truncates real Vita file to 7 bytes");
    SbFileInfo small={};
    SbFileInfo smallpath={};
    bool byfd=SbFileGetInfo(edit,&small);
    bool bypath=SbFileGetPathInfo(filepath,&smallpath);
    char debug_size[160];
    snprintf(debug_size,sizeof(debug_size),"TRUNCATION_SIZE fd=%lld path=%lld byfd=%d bypath=%d\n",
      (long long)small.size,(long long)smallpath.size,byfd,bypath);
    SbLogRaw(debug_size);
    check(byfd&&small.size==7,
          "Starboard truncation changes persistent file size");
    SbFileClose(edit);
  }

  static const char replacement[]="COBALT-STARBOARD-ATOMIC";
  bool atomic=SbFileAtomicReplace(filepath,replacement,sizeof(replacement)-1);
  char replacement_actual[sizeof(replacement)]={0};
  SbFile read_atomic=SbFileOpen(filepath,kSbFileOpenOnly|kSbFileRead,
                                &created,&io_error);
  bool atomic_contents=read_atomic &&
       SbFileRead(read_atomic,replacement_actual,sizeof(replacement)-1)==
             (int)sizeof(replacement)-1 &&
       memcmp(replacement_actual,replacement,sizeof(replacement)-1)==0;
  if(read_atomic)SbFileClose(read_atomic);
  check(atomic && atomic_contents,
        "Starboard atomically replaces an existing Vita cache file");
  check(!SbFileAtomicReplace("ur0:tai/config.txt",replacement,
                              sizeof(replacement)-1),
        "atomic file replacement rejects paths outside application storage");
  check(SbFileDelete(filepath),"Starboard file is deleted from Vita filesystem");
  check(!SbFileExists(filepath),"Starboard deleted file is absent");
  check(SbFileDelete(dirpath),"Starboard empty directory is deleted");
  // Synchronization: test nonrecursive mutex, timed condition and cross-thread signal.
  SbMutex test_mutex=SB_MUTEX_INITIALIZER;
  check(SbMutexCreate(&test_mutex),"Starboard pthread-backed mutex created");
  check(SbMutexAcquire(&test_mutex)==kSbMutexAcquired,
        "Starboard pthread-backed mutex acquired");
  check(SbMutexAcquireTry(&test_mutex)==kSbMutexBusy,
        "Starboard mutex try-acquire reports busy while locked");
  SbConditionVariable test_condition=SB_CONDITION_VARIABLE_INITIALIZER;
  check(SbConditionVariableCreate(&test_condition,&test_mutex),
        "Starboard pthread-backed condition created");
  SbTimeMonotonic start=SbTimeGetMonotonicNow();
  SbConditionVariableResult timed=SbConditionVariableWaitTimed(
      &test_condition,&test_mutex,20000);
  SbTimeMonotonic elapsed=SbTimeGetMonotonicNow()-start;
  check(timed==kSbConditionVariableTimedOut && elapsed>=10000 && elapsed<500000,
        "Starboard timed condition wait releases/reacquires mutex and times out");
  volatile int worker_ready=0;
  SyncTestWorkerArgs params={&test_mutex,&test_condition,&worker_ready};
  pthread_t thread={};
  int spawn=pthread_create(&thread,nullptr,sync_worker,&params);
  check(spawn==0,"Native Vita worker thread created");
  if(spawn==0) {
    SbConditionVariableResult result_signal=
        SbConditionVariableWaitTimed(&test_condition,&test_mutex,500000);
    check(result_signal==kSbConditionVariableSignaled&&worker_ready==1,
          "Native Vita worker signals actual Starboard condition");
    check(SbMutexRelease(&test_mutex),"Starboard mutex released after condition");
    check(pthread_join(thread,nullptr)==0,"Native Vita worker thread joined");
  } else {
    check(SbMutexRelease(&test_mutex),"Starboard mutex released after thread failure");
  }
  check(SbConditionVariableDestroy(&test_condition),
        "Starboard condition destroyed");
  check(SbMutexDestroy(&test_mutex),"Starboard mutex destroyed");
  // Real Starboard memory wrapper proof. VitaSDK heap must preserve data on grow.
  void* malloc_test=SbMemoryAllocate(256);
  check(malloc_test!=nullptr,"Starboard core malloc wrapper allocates Vita heap");
  if(malloc_test) {
    memset(malloc_test,0x5A,256);
    void* grown=SbMemoryReallocate(malloc_test,512);
    bool preserved=grown!=nullptr;
    if(grown) {
      for(int i=0;i<256;i++)if(static_cast<unsigned char*>(grown)[i]!=0x5A)
        preserved=false;
      SbMemoryDeallocate(grown);
    } else {
      SbMemoryDeallocate(malloc_test);
    }
    check(preserved,"Starboard realloc preserves 256 bytes of heap data");
  }
  void* aligned=SbMemoryAllocateAligned(64,512);
  check(aligned!=nullptr && (reinterpret_cast<uintptr_t>(aligned)&63)==0,
        "Starboard native 64-byte aligned allocation");
  SbMemoryDeallocateAligned(aligned);
  check(SbMemoryAllocateAligned(3,100)==nullptr,
        "Starboard invalid alignment rejected");
  SbOnceControl once=SB_ONCE_INITIALIZER;
  check(SbOnce(&once,once_test_entry),"Starboard pthread_once first execution");
  check(SbOnce(&once,once_test_entry) && once_test_count==1,
        "Starboard pthread_once never executes initializer twice");
  check(!SbOnce(nullptr,once_test_entry),
        "Starboard pthread_once rejects invalid control");
  // Native Starboard-thread lifecycle, TLS and sleep regression.
  check(SbThreadGetId()!=kSbThreadInvalidId,
        "Starboard current Vita kernel thread ID is valid");
  SbThread self=SbThreadGetCurrent();
  check(SbThreadIsValid(self) && SbThreadIsEqual(self,SbThreadGetCurrent()),
        "Starboard pthread identity matches current thread");
  int thread_value=0;void* joined_value=nullptr;
  SbThread worker=SbThreadCreate(256*1024,kSbThreadNoPriority,
         kSbThreadNoAffinity,true,"YTWorker",starboard_thread_worker,&thread_value);
  check(SbThreadIsValid(worker),"Starboard Vita worker created via pthread");
  if(worker)check(SbThreadJoin(worker,&joined_value) &&
             joined_value==&thread_value && thread_value==42,
             "Starboard worker joined with correct return and shared data");
  SbTimeMonotonic sleep_start=SbTimeGetMonotonicNow();
  SbThreadSleep(20000);
  SbTimeMonotonic slept=SbTimeGetMonotonicNow()-sleep_start;
  check(slept>=12000 && slept<300000,
        "Starboard thread sleeps for requested ~20ms");
  SbThreadSetName("YTMain");
  char thread_name[32]={};
  SbThreadGetName(thread_name,sizeof(thread_name));
  check(strcmp(thread_name,"YTMain")==0,
        "Starboard thread name stored in native thread-local state");
  SbThreadLocalKey key=SbThreadCreateLocalKey(nullptr);
  check(key!=kSbThreadLocalKeyInvalid,
        "Starboard thread-local key created via pthread");
  if(key){
    check(SbThreadSetLocalValue(key,&thread_value),
          "Starboard pthread TLS value assigned");
    check(SbThreadGetLocalValue(key)==&thread_value,
          "Starboard pthread TLS value retrieved");
    SbThreadDestroyLocalKey(key);
  }
  SbThreadYield();
  // Page-based Vita user-memory API with real kernel permissions.
  void* region=SbMemoryMap(65536,kSbMemoryMapProtectReadWrite,"YTVM");
  check(region!=SB_MEMORY_MAP_FAILED && region!=nullptr &&
        (reinterpret_cast<uintptr_t>(region)&4095)==0,
        "Starboard allocates 64KB of 4KB-aligned Vita virtual memory");
  if(region!=SB_MEMORY_MAP_FAILED && region) {
    static_cast<unsigned char*>(region)[0]=0x5C;
    static_cast<unsigned char*>(region)[4096]=0xA6;
    check(SbMemoryProtect(region,4096,kSbMemoryMapProtectRead),
          "Starboard protects first 4KB virtual page read-only");
    check(static_cast<unsigned char*>(region)[0]==0x5C &&
          static_cast<unsigned char*>(region)[4096]==0xA6,
          "Starboard memory protection preserves content of adjacent pages");
    check(SbMemoryProtect(region,4096,kSbMemoryMapProtectReadWrite),
          "Starboard safely restores page read/write permissions");
    check(!SbMemoryProtect(region,4096,kSbMemoryMapProtectReadWrite|
           kSbMemoryMapProtectExec),"Starboard forbids writable executable pages");
    check(!SbMemoryProtect(static_cast<unsigned char*>(region)+1,4096,
           kSbMemoryMapProtectRead),"Starboard forbids nonaligned protection range");
    static_cast<unsigned char*>(region)[40000]=0x3C;
    check(SbMemoryUnmap(region,32768) &&
          static_cast<unsigned char*>(region)[40000]==0x3C,
          "Starboard partial unmap (munmap trim) keeps the remaining pages intact");
    check(!SbMemoryUnmap(static_cast<unsigned char*>(region)+100,4096),
          "Starboard rejects a non page-aligned unmap");
    check(SbMemoryUnmap(region,65536),"Starboard releases whole 64KB Vita memory block");
    check(!SbMemoryUnmap(region,65536),"Starboard does not free memory block twice");
  }
  {
    // kSbMemoryMapProtectReserved (0): Cobalt's musl allocator reserves with
    // no access and commits through SbMemoryProtect.
    void* reserved=SbMemoryMap(512*1024,kSbMemoryMapProtectReserved,"reserve");
    check(reserved!=SB_MEMORY_MAP_FAILED,
          "Starboard reserves inaccessible address space (flags=0)");
    if(reserved!=SB_MEMORY_MAP_FAILED){
      bool committed=SbMemoryProtect(reserved,512*1024,
                                     kSbMemoryMapProtectReadWrite);
      check(committed,"reserved region becomes read/write via SbMemoryProtect");
      if(committed){
        unsigned char* bytes=static_cast<unsigned char*>(reserved);
        bytes[0]=0x5a;bytes[512*1024-1]=0xa5;
        check(bytes[0]==0x5a && bytes[512*1024-1]==0xa5,
              "committed reserved region stores data");
      }
      check(SbMemoryUnmap(reserved,512*1024),"reserved region unmapped");
    }
  }
  check(SbMemoryMap(0,kSbMemoryMapProtectReadWrite,"BAD")==SB_MEMORY_MAP_FAILED,
        "Starboard rejects zero-sized map");
  SbUser local=SbUserGetCurrent();
  check(SbUserIsValid(local),"Vita exposes a local console user handle");
  SbStorageRecord saved=SbStorageOpenRecord(local,"ytv_settings_test");
  check(SbStorageIsValidRecord(saved),"Vita storage opens persistent record");
  static const char value[]="YOUTUBE-VITA-LOCAL-SETTINGS";
  if(saved){
    check(SbStorageWriteRecord(saved,value,sizeof(value)-1),
          "Vita storage persists a BLOB with temporary-file replacement");
    char contents[64]={};
    check(SbStorageGetRecordSize(saved)==sizeof(value)-1 &&
          SbStorageReadRecord(saved,contents,sizeof(contents))==sizeof(value)-1 &&
          strcmp(value,contents)==0,"Vita storage readback matches saved data");
    check(SbStorageCloseRecord(saved),"Vita storage handle closes");
  }
  saved=SbStorageOpenRecord(local,"ytv_settings_test");
  char contents[64]={};
  check(saved && SbStorageReadRecord(saved,contents,sizeof(contents))==
        sizeof(value)-1 && strcmp(value,contents)==0,
        "Vita storage survives record closing and reopening");
  if(saved)check(SbStorageCloseRecord(saved),"reopened Vita storage handle closes");
  check(SbStorageDeleteRecord(local,"ytv_settings_test"),
        "Vita storage BLOB deleted from ux0");
  {
    // An app closed mid-save leaves .new/.backup files; saving must recover.
    const char* leftovers[]={"ux0:data/youtubevita-noads/storage/ytv_stale_test.dat.new",
                             "ux0:data/youtubevita-noads/storage/ytv_stale_test.dat.backup"};
    for(const char* path:leftovers){
      int f=sceIoOpen(path,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
      if(f>=0){sceIoWrite(f,"old",3);sceIoClose(f);}
    }
    SbStorageRecord stale=SbStorageOpenRecord(local,"ytv_stale_test");
    SbStorageRecord other=SbStorageOpenRecord(local,"ytv_second_test");
    check(stale && other,"two Vita storage records can be open together");
    bool written=stale && SbStorageWriteRecord(stale,"LOGIN",5);
    if(stale)SbStorageCloseRecord(stale);
    if(other){SbStorageCloseRecord(other);SbStorageDeleteRecord(local,"ytv_second_test");}
    stale=SbStorageOpenRecord(local,"ytv_stale_test");
    char back[8]={};
    check(written && stale && SbStorageReadRecord(stale,back,sizeof(back))==5 &&
          memcmp(back,"LOGIN",5)==0,
          "storage saves despite leftovers of an interrupted write");
    if(stale)SbStorageCloseRecord(stale);
    SbStorageDeleteRecord(local,"ytv_stale_test");
  }
  check(!SbStorageIsValidRecord(SbStorageOpenRecord(nullptr,"forbidden")),
        "Vita storage refuses invalid local user");
  check(!SbStorageIsValidRecord(SbStorageOpenRecord(local,"../forbidden")),
        "Vita storage rejects path traversal");
  // Real console hardware/locale/system APIs â€” never misidentify as Switch.
  char property[80]={};
  check(SbSystemGetProperty(kSbSystemPropertyBrandName,property,sizeof(property)) &&
        strcmp(property,"Sony")==0,
        "native system property reports the actual Sony brand");
  // Service-facing model identity is PlayStation 4 by project decision
  // (YouTube /tv refuses "PlayStation Vita"); the friendly name stays Vita.
  check(SbSystemGetProperty(kSbSystemPropertyModelName,property,sizeof(property)) &&
        strcmp(property,"PlayStation 4")==0,
        "service-facing model identity is PlayStation 4");
  check(SbSystemGetProperty(kSbSystemPropertyFriendlyName,property,sizeof(property)) &&
        strcmp(property,"PlayStation Vita")==0,
        "friendly name still reports the actual PlayStation Vita");
  char narrow[3]={'x','x','x'};
  check(!SbSystemGetProperty(kSbSystemPropertyModelName,narrow,sizeof(narrow)) &&
        narrow[0]=='x' && narrow[1]=='x',
        "system property leaves output untouched if capacity is too small");
  check(!SbSystemGetProperty(kSbSystemPropertyCertificationScope,property,
                             sizeof(property)),
        "platform refuses to claim an official YouTube certification");
  const char* locale=SbSystemGetLocaleId();
  check(locale && strlen(locale)>=5 && locale[2]=='_',
        "system returns a usable locale string");
  check(SbSystemGetNumberOfProcessors()==3 || SbSystemGetNumberOfProcessors()==4,
        "system reports 3 user cores (4 with CapUnlocker)");
  check(SbSystemGetTotalCPUMemory()==365LL*1024*1024,
        "system reports the 365MiB extended-memory application budget");
  check(!SbSystemHasCapability(kSbSystemCapabilityCanQueryGPUMemoryStats),
        "system does not falsely claim exact GPU memory telemetry");
  SbCPUFeatures cpu={};
  check(SbCPUFeaturesGet(&cpu) && cpu.architecture==kSbCPUFeaturesArchitectureArm &&
        cpu.arm.architecture_generation==7 && cpu.arm.part==0xC09,
        "Cobalt sees the real ARMv7 Cortex-A9 CPU architecture");
  check(cpu.arm.has_neon && cpu.arm.has_thumb2 && cpu.arm.has_vfp3 && cpu.has_fpu,
        "Cobalt sees ARM Neon/VFP/Thumb2 hardware features");
  unsigned char random_a[160]={},random_b[160]={};
  SbSystemGetRandomData(random_a,sizeof(random_a));
  SbSystemGetRandomData(random_b,sizeof(random_b));
  int nonzero=0;
  for(unsigned i=0;i<sizeof(random_a);i++)nonzero+=(random_a[i]!=0);
  check(nonzero>120 && memcmp(random_a,random_b,sizeof(random_a))!=0,
        "Vita kernel CSPRNG generated distinct 160-byte sequences");
  uint64_t random64_a=SbSystemGetRandomUInt64();
  uint64_t random64_b=SbSystemGetRandomUInt64();
  check(random64_a!=random64_b,
        "Vita kernel CSPRNG generated distinct 64-bit values");
  check(SbSystemGetExtension("nonexistent.youtube.plugin")==nullptr,
        "system refuses to fabricate unsupported runtime extensions");
  check(!SbSystemSupportsResume(),
        "system does not claim an unimplemented suspended-runtime flow");
    // Starboard scheduled events: callbacks ALWAYS on Vita main thread.
  event_owner=pthread_self();
  check(ytv_starboard_pump_scheduled(1)==0,
        "scheduled event pump initialized on native Vita main thread");
  int callback_data=0;
  SbEventId immediate=SbEventSchedule(scheduled_increment,&callback_data,0);
  check(immediate!=0,"scheduled Starboard zero-delay event receives a valid ID");
  check(ytv_starboard_pump_scheduled(1)==1 && callback_data==1,
        "Starboard dispatches immediate callback on Vita main thread");
  SbEventCancel(immediate); // after-fired cancellation is a harmless no-op
  SbEventId canceled=SbEventSchedule(scheduled_increment,&callback_data,10000);
  check(canceled!=0,"Starboard delayed event receives an ID");
  SbEventCancel(canceled);
  sceKernelDelayThread(18000);
  check(ytv_starboard_pump_scheduled(10)==0 && callback_data==1,
        "Starboard cancel prevents delayed callback execution");
  SbEventId timed_id=SbEventSchedule(scheduled_increment,&callback_data,30000);
  check(timed_id!=0 && ytv_starboard_pump_scheduled(10)==0,
        "Starboard does not dispatch delayed callback before due time");
  sceKernelDelayThread(42000);
  check(ytv_starboard_pump_scheduled(10)==1 && callback_data==2,
        "Starboard dispatches delayed callback after monotonic deadline");
  EventThreadState event_worker={0,0,&callback_data};
  pthread_t other={};
  int worker_start=pthread_create(&other,nullptr,
                                  worker_schedule_and_wrong_pump,&event_worker);
  check(worker_start==0,"Vita worker thread can submit a Starboard scheduled event");
  if(worker_start==0){
    check(pthread_join(other,nullptr)==0 && event_worker.id!=0 &&
          event_worker.wrong_thread_result==-1,
          "Starboard event pump refuses to execute callbacks on a worker thread");
    check(ytv_starboard_pump_scheduled(4)==1 && callback_data==3,
          "worker-scheduled callback executes on the actual main thread");
  }
  check(events_on_main==scheduled_count && scheduled_count==3,
        "all Starboard callbacks preserved correct main-thread affinity");
  check(SbEventSchedule(nullptr,nullptr,0)==0,
        "Starboard refuses scheduling a null callback");
  check(kSbFileMaxName==255 && kSbFileMaxPath==1023,
        "Starboard Vita pathname/name limits agree with SceIo bindings");
  check(kSbFileSepChar=='/' && strcmp(kSbFileSepString,"/")==0,
        "Starboard Vita path separators reflect Vita ux0 files");
    check(kSbFileAltSepChar=='\\' &&
          strcmp(kSbFileAltSepString,"\\")==0 &&
          kSbFileAltSepChar!='\0' &&
          kSbFileAltSepChar!=kSbFileSepChar,
          "Vita alternate path separator is nonzero, preventing Cobalt strchr infinite loop");
  check(kSbPathSepChar==';',
        "Starboard path-list separator does not split Vita mount colons");
  check(kSbMemoryPageSize==4096,
        "Starboard page size agrees with kubridge virtual memory implementation");
  check(kSbPreferredRgbaByteOrder==SB_PREFERRED_RGBA_BYTE_ORDER_RGBA,
        "Starboard color order matches vitaGL RGBA buffers");
  check(kSbNetworkReceiveBufferSize==128*1024,
        "Starboard network buffer budget stays within configured port limit");
  check(kSbMaxSystemPathCacheDirectorySize==16*1024*1024,
        "Starboard cache directory uses bounded Vita storage allocation");
  SbAccessibilityTextToSpeechSettings tts={};
  check(SbAccessibilityGetTextToSpeechSettings(&tts) &&
        !tts.has_text_to_speech_setting,
        "Vita reports no system-wide text-to-speech settings");
  SbAccessibilityDisplaySettings display={};
  check(SbAccessibilityGetDisplaySettings(&display) &&
        !display.has_high_contrast_text_setting,
        "Vita reports no system-wide high-contrast text setting");
  SbAccessibilityCaptionSettings captions={};
  check(SbAccessibilityGetCaptionSettings(&captions) &&
        captions.background_color_state==
           kSbAccessibilityCaptionStateUnsupported &&
        !captions.supports_is_enabled,
        "Vita reports unsupported system-level caption preferences");
  check(!SbAccessibilitySetCaptionsEnabled(true),
        "Vita refuses to claim captions were changed system-wide");
  check(!SbSpeechSynthesisIsSupported(),
        "Vita does not advertise an unimplemented speech synthesizer");
  {
    // Built-in microphone (sceAudioIn VOICE, 16 kHz mono S16) for voice search.
    SbMicrophoneInfo mic_info[2]={};
    int mics=SbMicrophoneGetAvailable(mic_info,2);
    check(mics==1 && mic_info[0].max_sample_rate_hz==16000 &&
          SbMicrophoneIsSampleRateSupported(mic_info[0].id,16000) &&
          !SbMicrophoneIsSampleRateSupported(mic_info[0].id,44100),
          "Vita exposes its built-in 16 kHz microphone");
    SbMicrophone mic=mics==1?SbMicrophoneCreate(mic_info[0].id,16000,16000*2):kSbMicrophoneInvalid;
    check(SbMicrophoneIsValid(mic),"microphone capture object created");
    if(mic){
      bool opened=SbMicrophoneOpen(mic);
      int got=0;
      if(opened){
        static int16_t mic_pcm[4096];
        for(int i=0;i<50 && got<=0;i++){sceKernelDelayThread(20000);got=SbMicrophoneRead(mic,mic_pcm,sizeof(mic_pcm));}
      }
      char mic_line[96];
      snprintf(mic_line,sizeof(mic_line),"MIC_TEST opened=%d bytes=%d\n",opened,got);
      SbLogRaw(mic_line);
      check(!opened || got>0,"an opened microphone delivers captured PCM");
      check(SbMicrophoneClose(mic),"microphone closes and releases sceAudioIn");
      SbMicrophoneDestroy(mic);
    }
  }
  check(SbDrmCreateSystem("com.widevine.alpha",nullptr,nullptr,
         nullptr,nullptr,nullptr,nullptr)==kSbDrmSystemInvalid,
        "Vita refuses unsupported Widevine DRM initialization");
  int drm_metrics=-1;
  check(SbDrmGetMetrics(nullptr,&drm_metrics)==nullptr && drm_metrics==0,
        "unsupported DRM returns no fabricated metrics");
  check(SbMediaCanPlayMimeAndKeySystem(
          "video/mp4; codecs=\"avc1.4d401e\"; width=854; height=480; framerate=30","")==
          kSbMediaSupportTypeProbably,
        "media capability accepts H264 Main 480p30 (hardware decoder)");
  check(SbMediaCanPlayMimeAndKeySystem(
          "video/mp4; codecs=\"avc1.64001f\"; width=1280; height=720; framerate=60","")==
          kSbMediaSupportTypeProbably &&
        SbMediaCanPlayMimeAndKeySystem(
          "video/mp4; codecs=\"avc1.4d401e\"; width=426; height=240; framerate=30","")==
          kSbMediaSupportTypeProbably,
        "media capability accepts H264 240p..720p60 (game decoder limit 1280x720)");
  check(SbMediaCanPlayMimeAndKeySystem("video/webm; codecs=\"vp9\"","")==
          kSbMediaSupportTypeNotSupported &&
        SbMediaCanPlayMimeAndKeySystem(
          "video/mp4; codecs=\"avc1.4d401e\"; framerate=9999","")==
          kSbMediaSupportTypeNotSupported &&
        SbMediaCanPlayMimeAndKeySystem("video/mp4; codecs=\"avc1.4d401e\"; eotf=catavision","")==
          kSbMediaSupportTypeNotSupported &&
        SbMediaCanPlayMimeAndKeySystem("video/mp4; codecs=\"avc1.4d401e\"; tunnelmode=true","")==
          kSbMediaSupportTypeNotSupported &&
        SbMediaCanPlayMimeAndKeySystem("video/mp4; codecs=\"avc1.4d401e\"; decode-to-texture=nope","")==
          kSbMediaSupportTypeNotSupported &&
        SbMediaCanPlayMimeAndKeySystem("video/mp4; codecs=\"avc1.4d401e\"; bitrate=2000000000","")==
          kSbMediaSupportTypeNotSupported,
        "media capability refuses VP9 and invalid probe attributes");
  check(SbMediaCanPlayMimeAndKeySystem("audio/mp4; codecs=\"mp4a.40.2\"; channels=2","")==
          kSbMediaSupportTypeProbably &&
        SbMediaCanPlayMimeAndKeySystem("audio/webm; codecs=\"opus\"","")==
          kSbMediaSupportTypeNotSupported,
        "media capability accepts AAC-LC, refuses Opus");
  check(SbMediaCanPlayMimeAndKeySystem("video/mp4","com.widevine.alpha")==
          kSbMediaSupportTypeNotSupported,
        "media capability refuses unimplemented encrypted video");
  SbMediaAudioConfiguration audio_config={};
  check(SbMediaGetAudioOutputCount()==1 &&
        SbMediaGetAudioConfiguration(0,&audio_config) &&
        audio_config.number_of_channels==2 &&
        audio_config.coding_type==kSbMediaAudioCodingTypePcm,
        "Vita media policy identifies stereo PCM output");
  check(!SbMediaGetAudioConfiguration(1,&audio_config),
        "Vita media policy does not invent a second audio device");
  check(SbMediaGetInitialBufferCapacity()==0 &&
        SbMediaGetBufferAllocationUnit()==0 &&
        SbMediaIsBufferPoolAllocateOnDemand(),
        "Vita media buffers allocate on demand to avoid startup OOM");
  check(SbMediaGetAudioBufferBudget()==2*1024*1024 &&
        SbMediaGetVideoBufferBudget(kSbMediaVideoCodecH264,854,480,8)>=12*1024*1024 &&
        SbMediaGetVideoBufferBudget(kSbMediaVideoCodecH264,854,480,8)<=36*1024*1024 &&
        SbMediaGetVideoBufferBudget(kSbMediaVideoCodecH264,1280,720,8)>=20*1024*1024 &&
        SbMediaGetVideoBufferBudget(kSbMediaVideoCodecH264,1280,720,8)<=60*1024*1024,
        "Vita media budgets: per-resolution base, adaptive extra only from free heap");
  check(SbMediaGetProgressiveBufferBudget(kSbMediaVideoCodecH264,960,544,8)>=8*1024*1024 &&
        SbMediaGetMaxBufferCapacity(kSbMediaVideoCodecH264,960,544,8)>
           SbMediaGetAudioBufferBudget()+SbMediaGetVideoBufferBudget(
               kSbMediaVideoCodecH264,960,544,8),
        "Vita progressive/max media buffer policies are internally consistent");
  check(SbMediaGetBufferAlignment()==64 && SbMediaGetBufferPadding()==0,
        "Vita encoded media buffer alignment and padding are defined");
  char formatted[64]={};
  check(SbStringFormatF(formatted,sizeof(formatted),"%s/%03d","VITA",7)==8 &&
        strcmp(formatted,"VITA/007")==0,
        "Starboard snprintf wrapper formats UTF-8 and integers accurately");
  char tiny[4]={'X','X','X','X'};
  check(SbStringFormatF(tiny,sizeof(tiny),"%s","COBALT")==6 &&
        strcmp(tiny,"COB")==0,
        "Starboard snprintf wrapper truncates safely and returns required size");
  wchar_t wide[32]={};
  check(SbStringFormatWideF(wide,32,L"%d FPS",60)==6 &&
        wcscmp(wide,L"60 FPS")==0,
        "Starboard wide printf wrapper formats UTF-32 text");
  int scanned=0;
  char text_segment[10]={};
  check(SbStringScanF("123 test","%d %9s",&scanned,text_segment)==2 &&
        scanned==123 && strcmp(text_segment,"test")==0,
        "Starboard vsscanf wrapper extracts typed fields");
  check(SbTimeZoneGetCurrent()>=-840 && SbTimeZoneGetCurrent()<=840,
        "Vita timezone offset is in valid Starboard minute range");
  const char* tzname=SbTimeZoneGetName();
  check(tzname!=nullptr && strchr(tzname,'/')!=nullptr,
        "Vita timezone provides an IANA-compatible identifier");
  check(SbSystemGetTotalGPUMemory()==0 && SbSystemGetUsedGPUMemory()==0 &&
        !SbSystemHasCapability(kSbSystemCapabilityCanQueryGPUMemoryStats),
        "unmeasured GPU memory statistics are not falsely advertised");
  char certification[32]={};
  check(!SbSystemSignWithCertificationSecretKey(
            (const uint8_t*)"abc",3,(uint8_t*)certification,sizeof(certification)),
        "unprovisioned Vita refuses certified YouTube crypto handshake");
  check(SbSystemGetStack(nullptr,0)==0,
        "unavailable crash unwinder returns no invented stack frames");
  char symbol_buffer[32]="UNMODIFIED";
  check(!SbSystemSymbolize((void*)1,symbol_buffer,sizeof(symbol_buffer)) &&
        strcmp(symbol_buffer,"UNMODIFIED")==0,
        "unsupported Vita symbolizer does not modify caller memory");
  check(!SbSystemRaisePlatformError(
         kSbSystemPlatformErrorTypeConnectionError,nullptr,nullptr),
        "unsupported native network dialog does not claim success");
  bool disconnected=SbSystemNetworkIsDisconnected();
  check(disconnected==SbSystemNetworkIsDisconnected(),
        "native network-state probe returns a defined boolean");
  check(SbAudioSinkGetMaxChannels()==2 &&
        SbAudioSinkGetNearestSupportedSampleFrequency(44100)==48000,
        "real Vita stereo output is resampled to required 48kHz");
  check(SbAudioSinkGetMinBufferSizeInFrames(
           2,kSbMediaAudioSampleTypeFloat32,48000)==8192,
        "Vita sink asks for 16 hardware blocks (~170 ms) of buffering");
  check(SbAudioSinkGetMinBufferSizeInFrames(
           2,kSbMediaAudioSampleTypeFloat32,22050)==-1,
        "Vita refuses unsupported raw 22.05 kHz audio configuration");
  check(SbAudioSinkIsAudioSampleTypeSupported(kSbMediaAudioSampleTypeFloat32) &&
        SbAudioSinkIsAudioFrameStorageTypeSupported(
          kSbMediaAudioFrameStorageTypeInterleaved),
        "Vita sink accepts interleaved float32 PCM conversion");
  static float silent_audio[2048*2]={};
  void* frame_buffers[1]={silent_audio};
  SoundSource audio_test={};
  SbAudioSink sink=SbAudioSinkCreate(
        2,48000,kSbMediaAudioSampleTypeFloat32,
        kSbMediaAudioFrameStorageTypeInterleaved,
        frame_buffers,2048,audio_status_test,audio_consume_test,&audio_test);
  check(SbAudioSinkIsValid(sink),
        "native SceAudioOut sink created with real 48kHz output port");
  if(sink){
    SoundSource second_test={};
    SbAudioSink second=SbAudioSinkCreate(
          2,48000,kSbMediaAudioSampleTypeFloat32,
          kSbMediaAudioFrameStorageTypeInterleaved,
          frame_buffers,2048,audio_status_test,audio_consume_test,&second_test);
    check(SbAudioSinkIsValid(second) && second!=sink,
          "a second concurrent sink gets its own SceAudioOut port");
    if(second){SbAudioSinkDestroy(second);}
    for(int i=0;i<150 && audio_test.consumed<2048;i++)
      sceKernelDelayThread(12000);
    check(audio_test.status_calls>0 && audio_test.consumed==2048,
          "native Vita PCM output consumed all 2048 silent source frames");
    SbAudioSinkDestroy(sink);
    check(!SbAudioSinkIsValid(sink),
          "native PCM sink releases audio port and invalidates handle");
  }
  check(!SbUiNavGetInterface(nullptr),
        "Cobalt uses Vita gamepad events when native UI nav engine absent");
  SbDecodeTargetInfo target_info={};
  check(!SbDecodeTargetGetInfo(kSbDecodeTargetInvalid,&target_info),
        "Vita does not fabricate an unavailable decode texture");
  check(!SbThreadContextGetPointer(
             kSbThreadContextInvalid,kSbThreadContextInstructionPointer,nullptr),
        "unimplemented profiler returns no invented CPU instruction pointer");
  check(!SbThreadSamplerIsValid(SbThreadSamplerCreate(kSbThreadInvalid)),
        "Starboard disables unsafe thread sampling without kernel support");
  check(ytv_vita_lifecycle_begin(native_lifecycle_cb) &&
        native_lifecycle_count==1 &&
        native_lifecycle_types[0]==kSbEventTypeStart,
        "native Vita Starboard dispatches actual Start lifecycle event");
  SbSystemRequestConceal();
  check(ytv_vita_lifecycle_pump(4)==2 &&
        native_lifecycle_types[1]==kSbEventTypeBlur &&
        native_lifecycle_types[2]==kSbEventTypeConceal,
        "native Vita lifecycle transitions through Blur and Conceal");
  SbSystemRequestFreeze();
  check(ytv_vita_lifecycle_pump(4)==1 &&
        native_lifecycle_types[3]==kSbEventTypeFreeze,
        "native Vita Starboard dispatches Freeze only after Conceal");
  SbSystemRequestStop(17);
  int lifecycle_exit=0;
  check(ytv_vita_lifecycle_pump(4)==1 &&
        native_lifecycle_types[4]==kSbEventTypeStop &&
        ytv_vita_lifecycle_should_exit(&lifecycle_exit) &&
        lifecycle_exit==17,
        "native Vita Starboard dispatches Stop and preserves exit status");
  {
    // Real SbPlayer: H264 hardware decode of a generated 854x480 clip.
    SbPlayerCreationParam video_param={};
    video_param.drm_system=kSbDrmSystemInvalid;
    video_param.video_stream_info.codec=kSbMediaVideoCodecH264;
    video_param.video_stream_info.mime="video/mp4; codecs=\"avc1.4d401e\"";
    video_param.video_stream_info.frame_width=854;
    video_param.video_stream_info.frame_height=480;
    video_param.audio_stream_info.codec=kSbMediaAudioCodecNone;
    video_param.output_mode=kSbPlayerOutputModeDecodeToTexture;
    check(SbPlayerGetPreferredOutputMode(&video_param)==
          kSbPlayerOutputModeDecodeToTexture,
          "player prefers decode-to-texture output");
    check(SbPlayerCreate(kSbWindowInvalid,&video_param,
          nullptr,nullptr,nullptr,nullptr,nullptr,nullptr)==kSbPlayerInvalid,
          "player creation without callbacks fails closed");
    static uint8_t clip[64*1024];
    int fd=sceIoOpen("app0:player_test_854x480.h264",SCE_O_RDONLY,0);
    int clip_size=fd>=0?sceIoRead(fd,clip,sizeof(clip)):0;
    if(fd>=0)sceIoClose(fd);
    check(clip_size>1000,"generated H264 test clip readable");
    g_player_test={};
    SbPlayer player=SbPlayerCreate(kSbWindowInvalid,&video_param,
        player_test_dealloc,player_test_decoder_status,player_test_status,
        player_test_error,&g_player_test,nullptr);
    check(SbPlayerIsValid(player),"SceAvcdec-backed H264 player created");
    if(player){
      SbPlayerSeek(player,0,1);
      // Split the Annex-B stream at access unit delimiters (00 00 00 01 09).
      int starts[16],n=0;
      for(int i=0;i+4<clip_size && n<16;i++)
        if(clip[i]==0&&clip[i+1]==0&&clip[i+2]==0&&clip[i+3]==1&&clip[i+4]==9)starts[n++]=i;
      for(int i=0;i<n;i++){
        SbPlayerSampleInfo info={};
        info.type=kSbMediaTypeVideo;
        info.buffer=clip+starts[i];
        info.buffer_size=(i+1<n?starts[i+1]:clip_size)-starts[i];
        info.timestamp=(int64_t)i*33333;
        info.video_sample_info.is_key_frame=i==0;
        SbPlayerWriteSamples(player,kSbMediaTypeVideo,&info,1);
      }
      SbPlayerWriteEndOfStream(player,kSbMediaTypeVideo);
      SbPlayerInfo pinfo={};
      for(int i=0;i<300;i++){
        SbPlayerGetInfo(player,&pinfo);
        if(g_player_test.freed>=6 && pinfo.total_video_frames>=6 &&
           g_player_test.presenting)break;
        sceKernelDelayThread(10000);
      }
      char line[128];
      snprintf(line,sizeof(line),"PLAYER_TEST samples=%d freed=%d decoded=%d presenting=%d errors=%d",
               n,g_player_test.freed,pinfo.total_video_frames,
               g_player_test.presenting,g_player_test.errors);
      SbLogRaw(line);SbLogRaw("\n");
      // Nothing displays frames here (no GL context), so the 6-frame pool
      // (decoder capacity up to 720p) fills and the decoder stops consuming
      // input: backpressure.
      check(n==10 && g_player_test.freed==6 && pinfo.total_video_frames==6,
            "decoder fills its 6-frame pool, frees those samples, then waits");
      check(pinfo.total_video_frames>=1 && pinfo.frame_width==854,
            "SceAvcdec decoded real 854x480 frames");
      check(g_player_test.presenting && g_player_test.errors==0,
            "player prerolled and reached Presenting without errors");
      SbPlayerDestroy(player);
    }
  }
  SbMutex static_mutex=SB_MUTEX_INITIALIZER;
  check(SbMutexAcquire(&static_mutex)==kSbMutexAcquired,
        "Cobalt zero-initialized static SbMutex lazily initializes Vita pthread");
  check(SbMutexRelease(&static_mutex),
        "Cobalt static SbMutex releases without an explicit Create call");
  check(SbMutexDestroy(&static_mutex),
        "Cobalt static SbMutex is cleanly destroyed after lazy initialization");
  SbConditionVariable static_cond=SB_CONDITION_VARIABLE_INITIALIZER;
  check(SbConditionVariableSignal(&static_cond),
        "Cobalt zero-initialized static condition signal uses Vita pthread sentinel");
  check(SbConditionVariableDestroy(&static_cond),
        "Cobalt static condition cleans up after lazy initialization");
  char result[96];
  snprintf(result, sizeof(result), "RESULT %s (%d errors)\n",errors?"FAIL":"PASS",errors);
  SbLogRaw(result);
  sceKernelExitProcess(errors ? 1 : 0);
  return errors;
}

