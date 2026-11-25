/*
 * Starboard utility APIs using VitaSDK's C library and real system services.
 * Unsupported certification/stack introspection remain explicitly unsupported.
 */
#include <psp2/kernel/threadmgr.h>
#include "starboard/string.h"
#include "starboard/time_zone.h"
#include "starboard/time.h"
#include "starboard/system.h"
#include "starboard/event.h"
#include "starboard/log.h"
#include <psp2/net/netctl.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <cstdio>
#include <cwchar>
#include <cstdarg>
#include <cerrno>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <pthread.h>

namespace {
pthread_mutex_t netctl_mutex=PTHREAD_MUTEX_INITIALIZER;
bool netctl_ready=false;
uint64_t netctl_last_attempt=0;
// Cobalt invokes SbSystemRaisePlatformError from arbitrary threads. Retain
// its retry callback and invoke it on the main loop only, after connectivity
// has returned, to avoid re-entering BrowserModule on the network worker.
pthread_mutex_t platform_error_mu=PTHREAD_MUTEX_INITIALIZER;
SbSystemPlatformErrorCallback platform_retry=nullptr;
void* platform_retry_context=nullptr;
bool platform_error_pending=false;
volatile unsigned platform_error_generation=0;

bool ensure_netctl(){
  if(__atomic_load_n(&netctl_ready,__ATOMIC_ACQUIRE))return true;
  pthread_mutex_lock(&netctl_mutex);
  if(netctl_ready){
    pthread_mutex_unlock(&netctl_mutex);
    return true;
  }
  const uint64_t now=(uint64_t)SbTimeGetMonotonicNow();
  if(netctl_last_attempt && now>=netctl_last_attempt &&
     now-netctl_last_attempt<2000000ull){
    pthread_mutex_unlock(&netctl_mutex);
    return false;
  }
  netctl_last_attempt=now?now:1;
  const int status=sceNetCtlInit();
  int state=SCE_NETCTL_STATE_DISCONNECTED;
  // A different Vita module may have initialized NetCtl already, which can
  // make sceNetCtlInit return an error even though the service is usable.
  const int check=sceNetCtlInetGetState(&state);
  const bool available=status>=0 || check>=0;
  __atomic_store_n(&netctl_ready,available,__ATOMIC_RELEASE);
  pthread_mutex_unlock(&netctl_mutex);
  char line[125];
  snprintf(line,sizeof(line),"YTV_NETWORK_NETCTL_INIT status=0x%x state_query=0x%x ready=%d\n",
           (unsigned)status,(unsigned)check,(int)available);
  SbLogRaw(line);
  return available;
}
bool valid_name(const char* tz) {
  if(!tz)return false;
  size_t n=strlen(tz);
  if(n<3||n>60||!strchr(tz,'/'))return false;
  // Reject POSIX TZ offsets and malicious "path" strings; IANA naming only.
  for(size_t i=0;i<n;i++) {
    char c=tz[i];
    if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
         (c>='0'&&c<='9')||c=='/'||c=='_'||c=='-'||c=='+'))
       return false;
  }
  return true;
}
} // namespace

extern "C" {
int SbStringFormat(char* dst,size_t capacity,const char* fmt,va_list args) {
  if(!fmt || (capacity>0 && !dst))return -1;
  return vsnprintf(dst,capacity,fmt,args);
}
int SbStringFormatWide(wchar_t* dst,size_t capacity,
                       const wchar_t* fmt,va_list args) {
  if(!fmt || !dst || capacity==0)return -1;
  return vswprintf(dst,capacity,fmt,args);
}
int SbStringScan(const char* input,const char* format,va_list args) {
  if(!input||!format)return -1;
  return vsscanf(input,format,args);
}
SbTimeZone SbTimeZoneGetCurrent() {
  // Starboard wants the *standard* (without DST) offset in minutes
  // west of UTC. Vita stores the user-selected timezone in minutes.
  int vita_time_zone=0;
  if(sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_TIME_ZONE,
                                &vita_time_zone)>=0 &&
     vita_time_zone>=-14*60 && vita_time_zone<=14*60){
    // Vita timezone is minutes east of UTC. Reverse for Starboard's
    // minutes-west convention.
    return -vita_time_zone;
  }
  return 0; // safe default in emulators lacking configured time zones
}
const char* SbTimeZoneGetName(){
  // An application configured with a legitimate IANA TZ is preferred;
  // Vita firmware only exposes a numeric timezone, not an IANA city.
  const char* tz=getenv("TZ");
  if(valid_name(tz))return tz;
  // Fixed offset IANA zone: represents offset, NOT regional DST policy.
  int minutes=SbTimeZoneGetCurrent();
  if(minutes==0)return "Etc/UTC";
  static char names[2][32];
  static int state=0;
  // No locale is guessed. 15/30/45 minute offsets are not representable
  // as Etc/GMT(+HH). Use UTC as a conservative fallback for such devices.
  if(minutes%60)return "Etc/UTC";
  int hour=minutes/60;
  int next=__sync_add_and_fetch(&state,1)&1;
  snprintf(names[next],sizeof(names[next]),"Etc/GMT%c%d",
           hour>=0?'+':'-',hour>=0?hour:-hour);
  return names[next];
}
bool SbSystemNetworkIsDisconnected(){
#if defined(YTV_NATIVE_DOWNLOAD_TEST)
  // Diagnostic only. Test harness drives explicit disconnected and connected
  // states independent of the emulator's usually-incomplete SceNetCtl state.
  SceIoStat forced={};
  if(sceIoGetstat("ux0:data/youtubevita-noads/test_force_offline.txt",&forced)>=0)
    return true;
  if(sceIoGetstat("ux0:data/youtubevita-noads/test_force_online.txt",&forced)>=0)
    return false;
#endif
  if(!ensure_netctl())return true;
  int state=SCE_NETCTL_STATE_DISCONNECTED;
  if(sceNetCtlInetGetState(&state)<0)return true;
  return state!=SCE_NETCTL_STATE_CONNECTED;
}
// Native Vita Add-ons overlay polls this state and can retry the Cobalt
// document load. No cookie/cache deletion is needed when Wi-Fi comes back.
int ytv_network_platform_error_pending(){
  pthread_mutex_lock(&platform_error_mu);
  int result=platform_error_pending?1:0;
  pthread_mutex_unlock(&platform_error_mu);
  return result;
}
unsigned ytv_network_platform_error_generation(){
  return __atomic_load_n(&platform_error_generation,__ATOMIC_RELAXED);
}
int ytv_network_platform_error_retry(){
  if(SbSystemNetworkIsDisconnected())return -1;
  pthread_mutex_lock(&platform_error_mu);
  SbSystemPlatformErrorCallback callback=platform_retry;
  void* context=platform_retry_context;
  platform_retry=nullptr;
  platform_retry_context=nullptr;
  platform_error_pending=false;
  pthread_mutex_unlock(&platform_error_mu);
  if(callback){
    SbLogRaw("YTV_NETWORK_NATIVE_RETRY callback=positive\n");
    callback(kSbSystemPlatformErrorResponsePositive,context);
    return 1;
  }
  // No Cobalt callback exists yet: nudge the TV page via Starboard's
  // regular network-connected event. Never claim a document reload here.
  extern void ytv_vita_lifecycle_send(SbEventType type,void* data);
  ytv_vita_lifecycle_send(kSbEventTypeOsNetworkConnected,nullptr);
  SbLogRaw("YTV_NETWORK_NATIVE_RETRY callback=none event=online\n");
  return 0;
}
// Network watcher (main thread, about once per second): reports connection
// changes to Cobalt as kSbEventTypeOsNetworkDisconnected/Connected, which the
// page receives as window "offline"/"online"; YouTube's player then resumes
// after a Wi-Fi drop instead of staying on an error.
int ytv_vita_network_poll(){
  static int last=-1;  // -1 unknown, 0 disconnected, 1 connected
  int now=SbSystemNetworkIsDisconnected()?0:1;
  if(now==last)return 0;
  int previous=last;
  last=now;
  if(previous<0){
    SbLogRaw(now?"YTV_LIFECYCLE_NETWORK initial=online\n":
                 "YTV_LIFECYCLE_NETWORK initial=offline\n");
    return 0;
  }
  extern void ytv_vita_lifecycle_send(SbEventType type,void* data);
  char line[80];
  snprintf(line,sizeof(line),"YTV_LIFECYCLE_NETWORK %s\n",now?"connected":"disconnected");
  SbLogRaw(line);
  ytv_vita_lifecycle_send(now?kSbEventTypeOsNetworkConnected:kSbEventTypeOsNetworkDisconnected,nullptr);
  return 1;
}
int64_t SbSystemGetTotalGPUMemory(){return 0;}
int64_t SbSystemGetUsedGPUMemory(){return 0;}
int SbSystemGetStack(void** out_stack,int stack_size){
  (void)out_stack;(void)stack_size;
  return 0; // no reliable unwind API integrated yet
}
bool SbSystemSymbolize(const void* address,char* out,int size) {
  (void)address;(void)out;(void)size;
  return false; // never fabricate a symbol name
}
bool SbSystemSignWithCertificationSecretKey(
   const uint8_t* message,size_t size,uint8_t* digest,size_t digest_size) {
  (void)message;(void)size;(void)digest;(void)digest_size;
  return false; // unprovisioned and UNcertified PSVita device
}
bool SbSystemRaisePlatformError(SbSystemPlatformErrorType type,
   SbSystemPlatformErrorCallback callback,void* context){
  // Before v0.60 this returned false, leaving Cobalt's BrowserModule
  // retries unhandled. The native overlay now presents an explicit action
  // and resolves the callback on the main event thread when the user retries.
  if(type!=kSbSystemPlatformErrorTypeConnectionError)return false;
  pthread_mutex_lock(&platform_error_mu);
  if(platform_error_pending){
    pthread_mutex_unlock(&platform_error_mu);
    SbLogRaw("YTV_LIFECYCLE_PLATFORM_ERROR duplicate=1\n");
    return false;
  }
  platform_retry=callback;
  platform_retry_context=context;
  platform_error_pending=true;
  __sync_add_and_fetch(&platform_error_generation,1);
  pthread_mutex_unlock(&platform_error_mu);
  char line[128];
  snprintf(line,sizeof(line),"YTV_LIFECYCLE_PLATFORM_ERROR type=%d handler=1 has_callback=%d\n",
           (int)type,callback?1:0);
  SbLogRaw(line);
  return true;
}
// Set by the ELF loader: base address of the official libcobalt.so image.
uintptr_t ytv_cobalt_image_base=0;
__attribute__((noinline)) void SbSystemBreakIntoDebugger(){
  // Cobalt reaches this from LOG(FATAL)/CHECK failures. Record who called it
  // as an ELF offset so the failing check can be found in the binary.
  uintptr_t ra=(uintptr_t)__builtin_return_address(0);
  char line[160];
  snprintf(line,sizeof(line),
           "YTV_BREAK_INTO_DEBUGGER tid=%d ra=%p elf_offset=0x%x\n",
           (int)sceKernelGetThreadId(),(void*)ra,
           ytv_cobalt_image_base && ra>ytv_cobalt_image_base?
             (unsigned)(ra-ytv_cobalt_image_base):0u);
  SbLogRaw(line);
  // Heuristic backtrace: release Cobalt/V8 strip fatal messages ("ignored"),
  // so log stack words that are return addresses into Cobalt's text segment
  // (the preceding instruction is an ARM BL/BLX or a Thumb BL/BLX).
  if(ytv_cobalt_image_base){
    SceKernelThreadInfo info;
    memset(&info,0,sizeof(info));
    info.size=sizeof(info);
    if(sceKernelGetThreadInfo(sceKernelGetThreadId(),&info)>=0 && info.stack){
      uintptr_t sp=(uintptr_t)__builtin_frame_address(0);
      uintptr_t top=(uintptr_t)info.stack+(uintptr_t)info.stackSize;
      const uintptr_t text_lo=ytv_cobalt_image_base+0x818000u;
      const uintptr_t text_hi=ytv_cobalt_image_base+0x1fa0000u;
      int found=0;
      for(uintptr_t p=sp&~3u;p+4<=top && found<40;p+=4){
        uintptr_t v=*(const uint32_t*)p;
        if(v<text_lo+4 || v>=text_hi)continue;
        bool call=false;
        if(v&1u){  // Thumb return address
          uint16_t hi=*(const uint16_t*)((v&~1u)-4);
          call=(hi&0xf800u)==0xf000u;
        }else if((v&3u)==0){
          uint32_t ins=*(const uint32_t*)(v-4);
          call=((ins&0x0f000000u)==0x0b000000u && (ins>>28)!=0xfu) ||
               (ins>>25)==0x7du || (ins&0x0ffffff0u)==0x012fff30u;
        }
        if(!call)continue;
        snprintf(line,sizeof(line),"YTV_BACKTRACE #%d elf_offset=0x%x sp+0x%x\n",
                 found,(unsigned)(v-ytv_cobalt_image_base),(unsigned)(p-sp));
        SbLogRaw(line);
        found++;
      }
    }
  }
  abort(); // per Starboard contract if debugger not attached
}
}

