/*
 * Optional MediaSession bridge for native YouTubeVita downloads.
 * A media artwork URL from the active Cobalt session may contain an EXACT
 * YouTube video ID as the /vi/<11-char-id>/ thumbnail path. Only trusted
 * YouTube thumbnail hosts are accepted; never infer IDs from titles.
 */
#include "starboard/extension/media_session.h"
#include "starboard/log.h"
#include "starboard/event.h"
#include <cstdio>
#include <cstring>
#include <cctype>

extern "C" bool ytv_native_download_set_video(const char*);
extern "C" bool ytv_native_download_set_artwork(const char*,const char*);
extern "C" bool ytv_native_download_artwork_valid(const char*);
extern "C" void ytv_native_download_clear_video();


/* SponsorBlock: read-only, bounded HTTPS lookup and MediaSession seek.
 * Network runs off the Cobalt thread; no raw SbPlayerSeek or ad-stream manipulation.
 */
#include <curl/curl.h>
#include <sodium.h>
#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <algorithm>

extern "C" { int g_ytv_sponsorblock_mode = 1; }
extern "C" int g_ytv_adblock_enabled;
namespace {
struct SbSegment { double start, end; bool attempted; };
struct JsonToken { char type; size_t begin, end, next; };
// Minimal bounded JSON reader; tokens retain exact subtree boundaries.
struct SbJson {
  const std::string& s;
  size_t p=0;
  std::vector<JsonToken> t;
  void ws(){while(p<s.size() && isspace((unsigned char)s[p]))++p;}
  bool value(unsigned depth=0){
    ws();
    if(depth>24 || p>=s.size() || t.size()>=16000)return false;
    const char c=s[p]; const size_t i=t.size(), b=p++;
    t.push_back({c,b,0,0});
    if(c=='"' ){
      bool closed=false;
      while(p<s.size()){
        char x=s[p++];
        if(x=='"'){closed=true;break;}
        if((unsigned char)x<32)return false;
        if(x=='\\'){if(p==s.size())return false;++p;}
      }
      if(!closed)return false;
    }else if(c=='{' || c=='['){
      const char close=c=='{'?'}':']';
      ws();
      if(p<s.size() && s[p]==close)++p;
      else for(;;){
        ws();
        if(c=='{'){
          if(p>=s.size() || s[p]!='"' || !value(depth+1))return false;
          ws(); if(p>=s.size() || s[p++]!=':')return false;
        }
        if(!value(depth+1))return false;
        ws(); if(p>=s.size())return false;
        char x=s[p++]; if(x==close)break; if(x!=',')return false;
      }
    }else{
      while(p<s.size() && !isspace((unsigned char)s[p]) && s[p]!=',' && s[p]!=']' && s[p]!='}')++p;
    }
    t[i].end=p;t[i].next=t.size();return true;
  }
  bool equal(size_t i,const char* v)const{
    return i<t.size() && t[i].type=='"' && t[i].end-t[i].begin==strlen(v)+2 &&
           s.compare(t[i].begin+1,strlen(v),v)==0;
  }
  size_t field(size_t i,const char* k)const{
    if(i>=t.size()||t[i].type!='{')return t.size();
    for(size_t j=i+1;j<t[i].next;){
      size_t v=t[j].next; if(v>=t[i].next)return t.size();
      if(equal(j,k))return v;
      j=t[v].next;
    }
    return t.size();
  }
  bool number(size_t i,double& out)const{
    if(i>=t.size() || !(t[i].type=='-' || isdigit((unsigned char)t[i].type)))return false;
    const std::string v=s.substr(t[i].begin,t[i].end-t[i].begin);
    char* e=nullptr;out=strtod(v.c_str(),&e);
    return e==v.c_str()+v.size() && std::isfinite(out);
  }
};
size_t sb_write(char* ptr,size_t size,size_t count,void* user){
  auto& s=*static_cast<std::string*>(user);
  if(size && count>262144/size)return 0;
  const size_t n=size*count;
  if(n>262144-s.size())return 0;
  s.append(ptr,n);return n;
}
std::vector<SbSegment> sb_lookup(const std::string& id,int mode){
  unsigned char hash[crypto_hash_sha256_BYTES];
  if(crypto_hash_sha256(hash,reinterpret_cast<const unsigned char*>(id.data()),id.size())!=0)return {};
  char prefix[5];snprintf(prefix,sizeof(prefix),"%02x%02x",hash[0],hash[1]);
  std::string url=std::string("https://sponsor.ajay.app/api/skipSegments/")+prefix+
    "?categories=%5B%22sponsor%22";
  if(mode==2)url+="%2C%22selfpromo%22%2C%22interaction%22%2C%22intro%22%2C%22outro%22";
  url+="%5D&actionTypes=%5B%22skip%22%5D";
  CURL* c=curl_easy_init();if(!c)return {};
  std::string body;
  curl_easy_setopt(c,CURLOPT_URL,url.c_str());
  curl_easy_setopt(c,CURLOPT_CAINFO,"app0:cacert.pem");
  curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"https");
  curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,0L);
  curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,4L);
  curl_easy_setopt(c,CURLOPT_TIMEOUT,8L);
  curl_easy_setopt(c,CURLOPT_USERAGENT,"YouTubeOnVita-NoAds/SponsorBlock");
  curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,sb_write);
  curl_easy_setopt(c,CURLOPT_WRITEDATA,&body);
  CURLcode result=curl_easy_perform(c);
  long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
  curl_easy_cleanup(c);
  if(result!=CURLE_OK || status!=200)return {};
  SbJson j{body};if(!j.value())return {};
  j.ws();if(j.p!=body.size() || j.t.empty() || j.t[0].type!='[')return {};
  std::vector<SbSegment> segments;
  for(size_t i=1;i<j.t[0].next;i=j.t[i].next){
    if(!j.equal(j.field(i,"videoID"),id.c_str()))continue;
    size_t a=j.field(i,"segments");
    if(a>=j.t.size()||j.t[a].type!='[')continue;
    for(size_t k=a+1;k<j.t[a].next && segments.size()<128;k=j.t[k].next){
      const size_t cat=j.field(k,"category");
      if(!j.equal(cat,"sponsor") && !(mode==2 &&
         (j.equal(cat,"selfpromo")||j.equal(cat,"interaction")||j.equal(cat,"intro")||j.equal(cat,"outro"))))continue;
      if(!j.equal(j.field(k,"actionType"),"skip"))continue;
      size_t s=j.field(k,"segment"); double start,end;
      if(s>=j.t.size() || j.t[s].type!='[' || j.t[s].next!=s+3 ||
         !j.number(s+1,start)||!j.number(s+2,end)||start<0||end<=start||end>86400*7)continue;
      segments.push_back({start,end,false});
    }
    break;
  }
  std::sort(segments.begin(),segments.end(),[](const SbSegment& a,const SbSegment& b){return a.start<b.start;});
  // Merge overlapping segments so we never seek backwards or skip twice.
  std::vector<SbSegment> merged;
  for(const auto& s:segments){
    if(!merged.empty() && s.start<=merged.back().end)
      merged.back().end=std::max(merged.back().end,s.end);
    else merged.push_back(s);
  }
  return merged;
}
pthread_mutex_t sb_mutex=PTHREAD_MUTEX_INITIALIZER;
std::string sb_id;
uint64_t sb_generation=0,sb_at=0;
double sb_pos=0,sb_duration=0,sb_rate=0;
bool sb_playing=false,sb_worker_started=false;
bool ad_skip_available=false,ad_skip_requested=false;
void* sb_context=nullptr;
CobaltExtensionMediaSessionInvokeActionCallback sb_invoke=nullptr;

// MediaSession actions must be dispatched from Starboard's event-loop thread.
// The network/timer worker only QUEUES them. It must never invoke Cobalt while
// holding sb_mutex or from a detached pthread (reentrancy and lifetime risks).
enum SbActionKind { kSbNoAction=0, kSbSkipAd=1, kSbSeekTo=2 };
SbActionKind sb_action=kSbNoAction;
double sb_action_seek=0.0;
uint64_t sb_action_generation=0;
SbEventId sb_action_event=0;

void sb_clear_action_locked(){
  if(sb_action_event)SbEventCancel(sb_action_event);
  sb_action_event=0;sb_action=kSbNoAction;
  sb_action_seek=0.0;sb_action_generation=0;
}
void sb_action_on_main(void*){
  CobaltExtensionMediaSessionInvokeActionCallback invoke=nullptr;
  void* context=nullptr;
  CobaltExtensionMediaSessionActionDetails action;
  SbActionKind kind=kSbNoAction;
  pthread_mutex_lock(&sb_mutex);
  kind=sb_action;
  const bool valid=sb_action_generation==sb_generation && sb_invoke &&
    ((kind==kSbSkipAd &&
      __atomic_load_n(&g_ytv_adblock_enabled,__ATOMIC_RELAXED) &&
      ad_skip_available) ||
     (kind==kSbSeekTo &&
      __atomic_load_n(&g_ytv_sponsorblock_mode,__ATOMIC_RELAXED)>0 &&
      sb_playing && !ad_skip_available && sb_action_seek<=sb_duration));
  if(valid){
    invoke=sb_invoke;
    context=sb_context;
    CobaltExtensionMediaSessionActionDetailsInit(&action,
      kind==kSbSkipAd?kCobaltExtensionMediaSessionActionSkipad:
                      kCobaltExtensionMediaSessionActionSeekto);
    if(kind==kSbSeekTo)action.seek_time=sb_action_seek;
  }
  // This callback is running: the event is already removed from the queue.
  sb_action_event=0;sb_action=kSbNoAction;sb_action_seek=0.0;sb_action_generation=0;
  pthread_mutex_unlock(&sb_mutex);

  // Runs on the Vita main event loop, OUTSIDE sb_mutex. Callbacks may
  // synchronously update MediaSession or unregister themselves.
  if(invoke){
    invoke(action,context);
    SbLogRaw(kind==kSbSkipAd?"YTV_ADBLOCK skipad dispatched on main thread\n":
                              "YTV_SPONSORBLOCK seek dispatched on main thread\n");
  }
}
bool sb_schedule_action_locked(SbActionKind kind,double seek=0.0){
  if(sb_action_event || !sb_invoke || kind==kSbNoAction)return false;
  sb_action=kind;sb_action_seek=seek;sb_action_generation=sb_generation;
  sb_action_event=SbEventSchedule(sb_action_on_main,nullptr,0);
  if(!sb_action_event){
    sb_action=kSbNoAction;sb_action_seek=0.0;sb_action_generation=0;
    // A full Starboard event queue must not cause unbounded disk logging.
    static uint64_t last_failure_log=0;
    const uint64_t now=sceKernelGetSystemTimeWide();
    if(now-last_failure_log>30000000ull){
      last_failure_log=now;
      SbLogRaw("YTV_SPONSORBLOCK action_schedule_failed\n");
    }
    return false;
  }
  return true;
}
void sb_reset_locked(){
  sb_clear_action_locked();
  sb_id.clear();++sb_generation;sb_playing=false;sb_at=0;
  ad_skip_requested=false;
}
void* sb_worker(void*){
  uint64_t loaded=~uint64_t(0);
  int loaded_mode=-1;
  std::vector<SbSegment> segs;
  for(;;){
    pthread_mutex_lock(&sb_mutex);
    const int mode=__atomic_load_n(&g_ytv_sponsorblock_mode,__ATOMIC_RELAXED);
    const uint64_t generation=sb_generation;
    const std::string id=sb_id;
    if(loaded!=generation || loaded_mode!=mode){
      pthread_mutex_unlock(&sb_mutex);
      segs=mode && !id.empty()?sb_lookup(id,mode):std::vector<SbSegment>{};
      loaded=generation;loaded_mode=mode;
      continue;
    }
    // Only request the supported MediaSession action when the frontend exposes it.
    if(__atomic_load_n(&g_ytv_adblock_enabled,__ATOMIC_RELAXED) &&
       ad_skip_available && !ad_skip_requested && sb_invoke){
      if(sb_schedule_action_locked(kSbSkipAd)){
        ad_skip_requested=true;
        SbLogRaw("YTV_ADBLOCK skipad scheduled\n");
      }
    }
    if(mode && sb_playing && sb_invoke && sb_at && !id.empty() && !ad_skip_available){
      const uint64_t now=sceKernelGetSystemTimeWide();
      const double pos=sb_pos+(now-sb_at)*0.000001*sb_rate;
      for(auto& s:segs){
        if(s.attempted || pos<s.start || pos>=s.end-0.15 ||
           s.end>sb_duration+0.05)continue;
        if(sb_schedule_action_locked(kSbSeekTo,std::min(s.end,sb_duration))){
          s.attempted=true;
          SbLogRaw("YTV_SPONSORBLOCK seek scheduled\n");
        }
        break;
      }
    }
    pthread_mutex_unlock(&sb_mutex);
    sceKernelDelayThread(mode?200000:500000);
  }
  return nullptr;
}
void sb_update(const char* id,const CobaltExtensionMediaSessionState& state){
  pthread_mutex_lock(&sb_mutex);
  ad_skip_available=state.actual_playback_state!=kCobaltExtensionMediaSessionNone &&
    state.available_actions[kCobaltExtensionMediaSessionActionSkipad];
  if(!ad_skip_available)ad_skip_requested=false;
  if(!id || !*id){if(!sb_id.empty())sb_reset_locked();}
  else{
    if(sb_id!=id){
      sb_clear_action_locked();ad_skip_requested=false;
      sb_id=id;++sb_generation;
    }
    sb_pos=state.current_playback_position/1000000.0;
    sb_duration=state.duration/1000000.0;
    sb_rate=state.actual_playback_rate;
    sb_at=sceKernelGetSystemTimeWide();
    sb_playing=state.actual_playback_state==kCobaltExtensionMediaSessionPlaying &&
      state.has_position_state && state.available_actions[kCobaltExtensionMediaSessionActionSeekto] &&
      sb_pos>=0 && sb_duration>0 && std::isfinite(sb_rate) && sb_rate>0 && sb_rate<=4;
  }
  if(!sb_worker_started && (ad_skip_available || !sb_id.empty())){
    pthread_t worker;
    if(pthread_create(&worker,nullptr,sb_worker,nullptr)==0){
      pthread_detach(worker);sb_worker_started=true;
    }
  }
  pthread_mutex_unlock(&sb_mutex);
}
} // namespace

namespace {
bool video_from_artwork(const char* src,char output[12]){
  if(!src || strncmp(src,"https://",8)!=0)return false;
  const char*host=src+8;
  const char*slash=strchr(host,'/');
  if(!slash || slash-host>100)return false;
  char name[110]={};
  memcpy(name,host,(size_t)(slash-host));
  bool trusted=false;
  if(!strcmp(name,"img.youtube.com") || !strcmp(name,"i.ytimg.com"))
    trusted=true;
  if(name[0]=='i' && name[1]>='0' && name[1]<='9' &&
     !strcmp(name+2,".ytimg.com"))
    trusted=true;
  if(!trusted)return false;
  const char*id=nullptr;
  if(!strncmp(slash,"/vi/",4))id=slash+4;
  else if(!strncmp(slash,"/vi_webp/",9))id=slash+9;
  if(!id)return false;
  for(int j=0;j<11;j++){
    const unsigned char c=(unsigned char)id[j];
    if(!c || (!isalnum(c) && c!='_' && c!='-'))return false;
    output[j]=(char)c;
  }
  output[11]=0;
  if(id[11]!='/' && id[11]!='?' && id[11]!='#' && id[11]!=0)
    return false;
  return true;
}
void on_update(CobaltExtensionMediaSessionState state){
  static int last_state=-1;
  const int playback=(int)state.actual_playback_state;
  if(last_state!=playback){
    char buf[100];
    snprintf(buf,sizeof(buf),"YTV_MEDIA_SESSION_STATE %d\n",playback);
    SbLogRaw(buf);
    last_state=playback;
  }
  if(state.actual_playback_state==kCobaltExtensionMediaSessionNone){
    ytv_native_download_clear_video();
    sb_update(nullptr,state);
    return;
  }
  if(!state.metadata){ytv_native_download_clear_video();sb_update(nullptr,state);return;}
  if(!state.metadata->artwork || !state.metadata->artwork_count){
    ytv_native_download_clear_video();
    sb_update(nullptr,state);
    return;
  }
  const size_t count=state.metadata->artwork_count>24?24:
                     state.metadata->artwork_count;
  static bool announced=false;
  if(!announced){
    char msg[110];
    snprintf(msg,sizeof(msg),"YTV_MEDIA_SESSION_METADATA artwork_count=%u\n",
             (unsigned)count);
    SbLogRaw(msg);
    announced=true;
  }
  // Find the exact active video ID without inferring it from title or artist.
  // The artwork array may also contain a different "music cover" variant.
  char active_id[12]={};
  for(size_t i=0;i<count;i++){
    char candidate[12]={};
    if(video_from_artwork(state.metadata->artwork[i].src,candidate)){
      memcpy(active_id,candidate,sizeof(active_id));
      break;
    }
  }
  if(!active_id[0]){
    ytv_native_download_clear_video();
    sb_update(nullptr,state);
    return;
  }
  sb_update(active_id,state);
  if(ytv_native_download_set_video(active_id)){
    SbLogRaw("YTV_MEDIA_SESSION_VIDEO_ID_EXACT\n");
    // Cobalt's MediaSession artwork is the art supplied to the music player.
    // Prefer the best-resolution rendition from that SAME session over
    // looking up the generic hqdefault.jpg video thumbnail.
    int best=-1;
    unsigned long best_area=0;
    for(size_t i=0;i<count;i++){
      const CobaltExtensionMediaImage& art=state.metadata->artwork[i];
      if(!ytv_native_download_artwork_valid(art.src))continue;
      int w=0,h=0;
      if(art.size)sscanf(art.size,"%dx%d",&w,&h);
      const unsigned long area=(w>0 && h>0 && w<=4096 && h<=4096)?
                                (unsigned long)w*h:1ul;
      if(best<0 || area>best_area){best=(int)i;best_area=area;}
    }
    if(best>=0){
      ytv_native_download_set_artwork(
        active_id,state.metadata->artwork[best].src);
    }
  }
}
void register_callbacks(void* context,CobaltExtensionMediaSessionInvokeActionCallback invoke,
                        CobaltExtensionMediaSessionUpdatePlatformPlaybackStateCallback){
  pthread_mutex_lock(&sb_mutex);
  sb_reset_locked();ad_skip_available=ad_skip_requested=false;sb_context=context;sb_invoke=invoke;
  pthread_mutex_unlock(&sb_mutex);
  SbLogRaw("YTV_MEDIA_SESSION_CALLBACK_REGISTERED\n");
}
void destroy_callbacks(){
  pthread_mutex_lock(&sb_mutex);
  sb_reset_locked();ad_skip_available=ad_skip_requested=false;sb_context=nullptr;sb_invoke=nullptr;
  pthread_mutex_unlock(&sb_mutex);
  ytv_native_download_clear_video();
  SbLogRaw("YTV_MEDIA_SESSION_CALLBACK_DESTROYED\n");
}
void update_state(CobaltExtensionMediaSessionPlaybackState){}
const CobaltExtensionMediaSessionApi kApi={
  kCobaltExtensionMediaSessionName,1,on_update,register_callbacks,
  destroy_callbacks,update_state
};
} // namespace
extern "C" const void* ytv_media_session_extension(){
  return &kApi;
}
