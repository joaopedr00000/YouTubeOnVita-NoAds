/*
 * PS Vita Starboard 15 lifecycle event dispatcher + honest optional APIs.
 * Dispatches Start -> Blur -> Conceal -> Freeze -> Stop on the main thread.
 * Does not execute Cobalt until bootstrap explicitly registers a callback.
 */
#include "starboard/system.h"
#include "starboard/log.h"
#include "starboard/event.h"
#include "starboard/thread.h"
#include "starboard/decode_target.h"
#include "starboard/ui_navigation.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <pthread.h>
#include <cstdint>
#include <cstdio>

namespace {
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
SbEventHandleCallback handler=nullptr;
pthread_t owner={};
int current=0; // 0=uninitialized, 1=Started, 2=Blurred, 3=Concealed,4=Frozen,5=Stopped
int target=0;
int exit_code=0;
void request(int stage){
  pthread_mutex_lock(&mutex);
  if(target<stage)target=stage;
  pthread_mutex_unlock(&mutex);
}
void send_event(SbEventType type,void* data){
  SbEventHandleCallback callback=nullptr;
  pthread_mutex_lock(&mutex);
  callback=handler;
  pthread_mutex_unlock(&mutex);
  if(!callback)return;
  SbEvent event={};
  event.type=type;
  event.timestamp=static_cast<int64_t>(sceKernelGetSystemTimeWide());
  event.data=data;
  callback(&event);
}
}

extern "C" {
// Main-thread dispatch of platform events (input) to Cobalt.
void ytv_vita_lifecycle_send(SbEventType type,void* data){send_event(type,data);}
bool ytv_vita_lifecycle_begin(SbEventHandleCallback callback){
  if(!callback)return false;
  pthread_mutex_lock(&mutex);
  if(handler || current!=0){
    pthread_mutex_unlock(&mutex);return false;
  }
  owner=pthread_self();
  handler=callback;
  current=1;
  if(target<1)target=1;
  pthread_mutex_unlock(&mutex);
  // Cobalt command-line bootstrap expects argc>=1 and argv[0]. Optional extra
  // switches (one per line, e.g. --vmodule=...) come from
  // ux0:data/youtubevita-noads/cobalt_args.txt; nothing is added when it is absent.
  static char app_name[]="youtubevita";
  static char arg_buffer[2048];
  static char* args[17]={app_name,nullptr};
  int argc=1;
  {
    int fd=sceIoOpen("ux0:data/youtubevita-noads/cobalt_args.txt",SCE_O_RDONLY,0);
    if(fd>=0){
      int n=sceIoRead(fd,arg_buffer,sizeof(arg_buffer)-1);
      sceIoClose(fd);
      if(n>0){
        arg_buffer[n]=0;
        char* p=arg_buffer;
        while(*p && argc<16){
          while(*p=='\r'||*p=='\n'||*p==' ')*p++=0;
          if(!*p)break;
          args[argc++]=p;
          while(*p && *p!='\r' && *p!='\n')p++;
        }
        for(int i=1;i<argc;i++){
          char line[300];
          snprintf(line,sizeof(line),"YTV_START_ARG %d %s\n",i,args[i]);
          SbLogRaw(line);
        }
      }
    }
  }
  args[argc]=nullptr;
  SbEventStartData info={};
  info.argument_count=argc;
  info.argument_values=args;
  info.link=nullptr;
  send_event(kSbEventTypeStart,&info);
  return true;
}
int ytv_vita_lifecycle_pump(int max_events){
  if(max_events<=0 || max_events>8)return 0;
  pthread_mutex_lock(&mutex);
  bool allowed=handler!=nullptr && pthread_equal(owner,pthread_self());
  pthread_mutex_unlock(&mutex);
  if(!allowed)return -1;
  int calls=0;
  while(calls<max_events){
    pthread_mutex_lock(&mutex);
    if(current>=target || current>=5){
      pthread_mutex_unlock(&mutex);break;
    }
    current++;
    int next=current;
    pthread_mutex_unlock(&mutex);
    SbEventType type=next==2?kSbEventTypeBlur:
                     next==3?kSbEventTypeConceal:
                     next==4?kSbEventTypeFreeze:kSbEventTypeStop;
    send_event(type,nullptr);
    ++calls;
  }
  return calls;
}
bool ytv_vita_lifecycle_should_exit(int* code){
  pthread_mutex_lock(&mutex);
  bool exited=current==5;
  if(exited && code)*code=exit_code;
  pthread_mutex_unlock(&mutex);
  return exited;
}
void SbSystemRequestConceal(){request(3);}
void SbSystemRequestFreeze(){request(4);}
void SbSystemRequestStop(int code){
  char line[112];
  snprintf(line,sizeof(line),"YTV_LIFECYCLE_REQUEST_STOP code=%d\n",code);
  SbLogRaw(line);
  pthread_mutex_lock(&mutex);
  exit_code=code;
  if(target<5)target=5;
  pthread_mutex_unlock(&mutex);
}
// Optional symbolization/profiling: do not fake a captured thread context.
bool SbThreadContextGetPointer(SbThreadContext context,
                                SbThreadContextProperty property,void** out){
  (void)context;(void)property;(void)out;return false;
}
SbThreadSampler SbThreadSamplerCreate(SbThread thread){
  (void)thread;return kSbThreadSamplerInvalid;
}
void SbThreadSamplerDestroy(SbThreadSampler sampler){(void)sampler;}
SbThreadContext SbThreadSamplerFreeze(SbThreadSampler sampler){
  (void)sampler;return kSbThreadContextInvalid;
}
bool SbThreadSamplerThaw(SbThreadSampler sampler){
  (void)sampler;return false;
}
// Native UI focus engine is not implemented. Cobalt uses actual Vita
// gamepad SbInputData from pad_adapter.cpp, not a simulated focus tree.
bool SbUiNavGetInterface(SbUiNavInterface* out){
  (void)out;return false;
}
// SbDecodeTargetRelease/GetInfo live in port_player.cpp.
}

