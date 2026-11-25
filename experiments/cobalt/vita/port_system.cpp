/*
 * Starboard 15 system identification and entropy on Sony PlayStation Vita.
 *
 * The platform MUST NOT impersonate a certified YouTube device or Nintendo
 * Switch. Unsupported property IDs return false. Randomness uses the genuine
 * Vita kernel CSPRNG and fails closed rather than using insecure rand().
 */
#include <psp2/io/fcntl.h>
#include "starboard/system.h"
#ifndef YTV_DEVICE_IDENTITY_PS4
#define YTV_DEVICE_IDENTITY_PS4 1  // see SbSystemGetProperty
#endif
#include "starboard/log.h"
#include "starboard/cpu_features.h"
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/sysmodule.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace {
const char* const kLocales[] = {
  "ja_JP", "en_US", "fr_FR", "es_ES", "de_DE",
  "it_IT", "nl_NL", "pt_PT", "ru_RU", "ko_KR",
  "zh_TW", "zh_CN", "fi_FI", "sv_SE", "da_DK",
  "nb_NO", "pl_PL", "pt_BR", "en_GB", "tr_TR"
};
bool get_console_parameter(int id,int* result) {
  if(!result)return false;
  // sceAppUtilSystemParamGetInt works in Vita3K's emulated system.
  // If unavailable, do not infer a user's region from their IP.
  return sceAppUtilSystemParamGetInt(id,result)>=0;
}
bool copy_value(const char* value,char* out,int cap) {
  if(!value || !out || cap<=0)return false;
  size_t len=strlen(value);
  if(len+1>static_cast<size_t>(cap))return false;
  memcpy(out,value,len+1);
  return true;
}
}

extern "C" {
const char* SbSystemGetLocaleId() {
  int id=-1;
  if(get_console_parameter(SCE_SYSTEM_PARAM_ID_LANG,&id) &&
     id>=0 && id<(int)(sizeof(kLocales)/sizeof(kLocales[0])))
    return kLocales[id];
  return "en_US"; // explicit safe default when system locale is unavailable
}

int64_t ytv_measured_total_memory = 0;  // set by the ELF loader
bool SbSystemGetProperty(SbSystemPropertyId id,char* out,int len) {
  const char* property=nullptr;
  switch(id) {
#if YTV_DEVICE_IDENTITY_PS4
    // Device identity presented to services (User-Agent device token).
    // Project owner's decision (2026-11-17): YouTube's /tv serves the
    // "not available on this device" page to "PlayStation Vita" and the
    // TVHTML5 app to "PlayStation 4". Only the identity strings change; no
    // certification scope or signing key is claimed (see below).
    case kSbSystemPropertyBrandName: property="Sony"; break;
    case kSbSystemPropertyModelName: property="PlayStation 4"; break;
    case kSbSystemPropertyModelYear: property="2013"; break;
    case kSbSystemPropertyFirmwareVersion: property="12.00"; break;
    case kSbSystemPropertyFriendlyName: property="PlayStation Vita"; break;
#else
    case kSbSystemPropertyBrandName: property="Sony"; break;
    case kSbSystemPropertyModelName: property="PlayStation Vita"; break;
    case kSbSystemPropertyModelYear: property="2011"; break;
    case kSbSystemPropertyFriendlyName: property="PlayStation Vita"; break;
#endif
    case kSbSystemPropertyPlatformName: property="PS Vita"; break;
    case kSbSystemPropertyDeviceType: property="GAME_CONSOLE"; break;
    // Deliberately DO NOT claim a certification scope, Speech API key,
    // chipset part number, ad ID, or system integrator.
    default: return false;
  }
  return copy_value(property,out,len);
}
bool SbSystemHasCapability(SbSystemCapabilityId id){
  switch(id) {
    case kSbSystemCapabilityReversedEnterAndBack: {
      int button=-1;
      return get_console_parameter(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON,&button) &&
             button==SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
    }
    case kSbSystemCapabilityCanQueryGPUMemoryStats: return false;
    default:return false;
  }
}
extern "C" int ytv_vita_cpu_count();
int SbSystemGetNumberOfProcessors() {
  // Four Cortex-A9 cores; the 4th is usable only with CapUnlocker.
  return ytv_vita_cpu_count();
}
int64_t SbSystemGetTotalCPUMemory() {
  // Budget measured by the loader at process start: the user RAM the system
  // granted under the VPK's param.sfo memory mode (ATTRIBUTE2=12, extended
  // memory, as in DeltaruneVita). Capped at 365 MiB; 365 MiB if unmeasured.
  extern int64_t ytv_measured_total_memory;
  const int64_t cap = 365LL*1024*1024;
  int64_t m = ytv_measured_total_memory;
  return (m > 0 && m < cap) ? m : cap;
}
int64_t SbSystemGetUsedCPUMemory(){
  // VitaSDK provides no total resident-process-memory counter here.
  // Return zero instead of a fake measurement; do not claim it's measured.
  return 0;
}
bool SbCPUFeaturesGet(SbCPUFeatures* out) {
  if(!out)return false;
  memset(out,0,sizeof(*out));
  out->architecture=kSbCPUFeaturesArchitectureArm;
  out->brand="ARM Cortex-A9 (PS Vita)";
  out->icache_line_size=32;
  out->dcache_line_size=32;
  out->has_fpu=true;
  out->arm.implementer=0x41;
  out->arm.architecture_generation=7;
  out->arm.part=0xC09;
  out->arm.has_neon=true;
  // DIAGNOSTIC ONLY: the presence of ux0:data/youtubevita-noads/diag_hide_neon.txt
  // hides NEON from Cobalt (BoringSSL then uses its generic C crypto), to
  // test whether a TLS failure comes from emulator NEON behaviour. Real
  // hardware has NEON; the file must never exist in a normal setup.
  {
    SceIoStat st;
    if(sceIoGetstat("ux0:data/youtubevita-noads/diag_hide_neon.txt",&st)>=0){
      out->arm.has_neon=false;
      SbLogRaw("YTV_DIAG_CPU_FEATURES neon hidden (diagnostic file present)\n");
    }
  }
  out->arm.has_thumb2=true;
  out->arm.has_vfp=true;
  out->arm.has_vfp3=true;
  out->arm.has_vfp3_d32=true;
  // CPU variant, revision and unsupported instruction extensions remain
  // conservative zeros, not guessed from emulator host capabilities.
  return true;
}
void SbSystemGetRandomData(void* output,int bytes){
  if(!output || bytes<=0)return;
  unsigned char* p=static_cast<unsigned char*>(output);
  int offset=0;
  while(offset<bytes){
    int n=bytes-offset>64?64:bytes-offset;
    int status=sceKernelGetRandomNumber(p+offset,(SceSize)n);
    if(status<0){
      memset(p,0,(size_t)bytes);
      SbLogRaw("ERROR: Vita CSPRNG unavailable; refusing insecure randomness\n");
      abort();
    }
    offset+=n;
  }
}
uint64_t SbSystemGetRandomUInt64(){
  uint64_t random=0;
  SbSystemGetRandomData(&random,(int)sizeof(random));
  return random;
}
SbSystemError SbSystemGetLastError() {return errno;}
void SbSystemClearLastError() {errno=0;}
int SbSystemGetErrorString(SbSystemError error,char* out,int size){
  const char* message=strerror(error);
  if(!message)message="Unknown error";
  int len=(int)strlen(message);
  if(out && size>0){
    int n=len<size-1?len:size-1;
    memcpy(out,message,(size_t)n);
    out[n]=0;
  }
  return len;
}
// dev.cobalt.extension.Configuration (v3). Without it Cobalt falls back to
// hard-coded values, notably CobaltEnableJit()==false (V8 --jitless), which
// made all of YouTube's JavaScript run interpreted. Values are Cobalt's
// defaults except where noted for the Vita.
// Low-memory build ("YouTube Lite"): weak, so absent (0) everywhere else.
// File scope on purpose: inside the anonymous namespace the weak symbol
// got internal (mangled) linkage and never resolved (v0.45 Lite).
extern "C" int ytv_lowmem_build __attribute__((weak));
extern "C" bool ytv_lowmem(){return &ytv_lowmem_build && ytv_lowmem_build;}

namespace {
// Optional overrides for performance experiments, read once from
// ux0:data/youtubevita-noads/cobalt_config.txt: "rasterizer=hardware",
// "jit=0", "swap_interval=0".
struct Overrides { char rasterizer[24]; int jit; int swap_interval; int affinity; bool loaded; };
// affinity defaults to 0 since v0.37: with MainWebModule alone on core 0 a
// busy wait there froze the UI on hardware (v0.36 "frezee" log).
#if defined(YTV_V8_JITLESS_DIAGNOSTIC)
constexpr int kDefaultJit = 0;  // diagnostic only; jit=1 in cobalt_config.txt overrides this
#else
constexpr int kDefaultJit = 1;
#endif
Overrides g_ovr = {"direct-gles", kDefaultJit, 1, 0, false};
void load_overrides(){
  if(g_ovr.loaded)return;
  g_ovr.loaded=true;
  int fd=sceIoOpen("ux0:data/youtubevita-noads/cobalt_config.txt",SCE_O_RDONLY,0);
  if(fd<0)return;
  char buf[256]={0};
  int n=sceIoRead(fd,buf,sizeof(buf)-1);
  sceIoClose(fd);
  if(n<=0)return;
  char* line=buf;
  while(line && *line){
    char* next=strchr(line,'\n'); if(next)*next++=0;
    char key[32]={0},val[32]={0};
    if(sscanf(line,"%31[^=]=%31s",key,val)==2){
      if(!strcmp(key,"rasterizer"))snprintf(g_ovr.rasterizer,sizeof(g_ovr.rasterizer),"%s",val);
      else if(!strcmp(key,"jit"))g_ovr.jit=atoi(val);
      else if(!strcmp(key,"swap_interval"))g_ovr.swap_interval=atoi(val);
      else if(!strcmp(key,"affinity"))g_ovr.affinity=atoi(val);
    }
    line=next;
  }
  char msg[128];
  snprintf(msg,sizeof(msg),"YTV_LIFECYCLE_CONFIG rasterizer=%s jit=%d swap_interval=%d affinity=%d\n",
           g_ovr.rasterizer,g_ovr.jit,g_ovr.swap_interval,g_ovr.affinity);
  SbLogRaw(msg);
}
}  // namespace
extern "C" int ytv_cfg_affinity(){load_overrides();return g_ovr.affinity;}
namespace {
const char* cfg_exit_strategy(){return "stop";}
bool cfg_dirty_region_only(){return false;}  // GXM buffers rotate, no preserve
int cfg_swap_interval(){load_overrides();return g_ovr.swap_interval;}
// YouTube's own animated splash from the official Cobalt package
// (app0:web/cobalt/browser/splash_screen/). Was black_splash_screen.html: a
// static black page, i.e. the grey -> black -> UI start-up the user saw when
// no cached splash existed.
// Tried (v0.43): file:///cobalt/browser/splash_screen/youtube_splash_screen.html
// from the official package. It crashed Vita3K at every start (render-target
// creation failed in its GL renderer); kept black until tested on hardware.
const char* cfg_fallback_splash(){return "h5vcc-embedded://black_splash_screen.html";}
bool cfg_enable_quic(){return true;}
int cfg_skia_cache(){return 4*1024*1024;}  // default 4 MiB
int cfg_offscreen_cache(){return -1;}
int cfg_encoded_image_cache(){return 4*1024*1024;} // default 1 MiB
// Thumbnails. 32 MiB let VitaGL's VRAM run down to ~16 MiB free in a 9 min
// session (v0.42 hardware log); with its RAM pool nearly full that ends in
// failed allocations (artifacts).
int cfg_image_cache(){return 14*1024*1024;}
int cfg_local_typeface_cache(){return 8*1024*1024;}
int cfg_remote_typeface_cache(){return 8*1024*1024;} // default 4 MiB
int cfg_mesh_cache(){return 1024*1024;}
int cfg_software_surface_cache(){return 4*1024*1024;}
float cfg_image_cache_video_multiplier(){return 1.0f;}
int cfg_glyph_atlas_w(){return -1;}
int cfg_glyph_atlas_h(){return -1;}
int cfg_js_gc_threshold(){return (ytv_lowmem()?8:16)*1024*1024;}  // default 8 MiB
int cfg_reduce_cpu(){return -1;}
int cfg_reduce_gpu(){return -1;}
bool cfg_gc_zeal(){return false;}
const char* cfg_rasterizer(){load_overrides();return g_ovr.rasterizer;}
bool cfg_enable_jit(){
  load_overrides();
#if defined(YTV_V8_JITLESS_DIAGNOSTIC)
  // Diagnostic binary must remain jitless even if a user configuration
  // from an earlier JIT-on build still contains jit=1.
  return false;
#else
  return g_ovr.jit!=0;  // executable memory through kubridge
#endif
}
const char* cfg_splash_topics(){return "";}
bool cfg_store_compiled_js(){return true;}  // faster start (compiled_js cache)

struct ConfigurationApi {
  const char* name;
  uint32_t version;
  const char* (*CobaltUserOnExitStrategy)();
  bool (*CobaltRenderDirtyRegionOnly)();
  int (*CobaltEglSwapInterval)();
  const char* (*CobaltFallbackSplashScreenUrl)();
  bool (*CobaltEnableQuic)();
  int (*CobaltSkiaCacheSizeInBytes)();
  int (*CobaltOffscreenTargetCacheSizeInBytes)();
  int (*CobaltEncodedImageCacheSizeInBytes)();
  int (*CobaltImageCacheSizeInBytes)();
  int (*CobaltLocalTypefaceCacheSizeInBytes)();
  int (*CobaltRemoteTypefaceCacheSizeInBytes)();
  int (*CobaltMeshCacheSizeInBytes)();
  int (*CobaltSoftwareSurfaceCacheSizeInBytes)();
  float (*CobaltImageCacheCapacityMultiplierWhenPlayingVideo)();
  int (*CobaltSkiaGlyphAtlasWidth)();
  int (*CobaltSkiaGlyphAtlasHeight)();
  int (*CobaltJsGarbageCollectionThresholdInBytes)();
  int (*CobaltReduceCpuMemoryBy)();
  int (*CobaltReduceGpuMemoryBy)();
  bool (*CobaltGcZeal)();
  const char* (*CobaltRasterizerType)();
  bool (*CobaltEnableJit)();
  const char* (*CobaltFallbackSplashScreenTopics)();
  bool (*CobaltCanStoreCompiledJavascript)();
};
const ConfigurationApi kConfigurationApi = {
  "dev.cobalt.extension.Configuration", 3,
  cfg_exit_strategy, cfg_dirty_region_only, cfg_swap_interval,
  cfg_fallback_splash, cfg_enable_quic, cfg_skia_cache, cfg_offscreen_cache,
  cfg_encoded_image_cache, cfg_image_cache, cfg_local_typeface_cache,
  cfg_remote_typeface_cache, cfg_mesh_cache, cfg_software_surface_cache,
  cfg_image_cache_video_multiplier, cfg_glyph_atlas_w, cfg_glyph_atlas_h,
  cfg_js_gc_threshold, cfg_reduce_cpu, cfg_reduce_gpu, cfg_gc_zeal,
  cfg_rasterizer, cfg_enable_jit, cfg_splash_topics, cfg_store_compiled_js,
};
}  // namespace
extern const void* ytv_media_session_extension();
// dev.cobalt.extension.Graphics v1. Cobalt asks for the maximum frame
// interval on every rasterizer tick. Normally -1 (draw only when something
// changed, as before). While the Vita IME dialog is open it must be drawn and
// updated on every swap, so a frame is forced at least every 33 ms: an idle
// page made no swaps and the keyboard froze (v0.48 Lite, Add-ons > IME).
extern "C" bool ytv_osk_active();
extern "C" bool ytv_addons_wants_frames();  // port_addons.cpp: menu/toast visible
extern "C" volatile uint64_t g_ytv_last_input_us; // port_input.cpp

struct YtvGraphicsApi { const char* name; uint32_t version; float (*max_interval)(); };
extern "C" volatile int g_ytv_graphics_ext_active = 0;
float ytv_max_frame_interval(){
  // Request a short burst of frames after any input. A paused/static Cobalt
  // page can otherwise show no visual response after a playlist transition,
  // even when the native touch/navigation sounds and event pump still run.
  const uint64_t now=sceKernelGetSystemTimeWide();
  const uint64_t last=__atomic_load_n(&g_ytv_last_input_us,__ATOMIC_RELAXED);
  const bool wake=last && now>=last && now-last<2200000ull;
  return ytv_osk_active()||ytv_addons_wants_frames()||wake?33.f:-1.f;
}
const YtvGraphicsApi kGraphicsApi={"dev.cobalt.extension.Graphics",1,ytv_max_frame_interval};
const void* SbSystemGetExtension(const char* name){
  if(name && strcmp(name,kGraphicsApi.name)==0){
    g_ytv_graphics_ext_active=1;
    SbLogRaw("YTV_LIFECYCLE_GRAPHICS_EXTENSION requested\n");
    return &kGraphicsApi;
  }
  if(name && strcmp(name,kConfigurationApi.name)==0)return &kConfigurationApi;
  // Exact active media artwork ID bridge; harmless when Cobalt does not use it.
  if(name && strcmp(name,"dev.cobalt.extension.MediaSession")==0){
    SbLogRaw("YTV_MEDIA_SESSION_EXTENSION_REQUESTED\n");
    return ytv_media_session_extension();
  }
  // Vita IME for text fields (port_osk.cpp); null when turned off.
  extern const void* ytv_osk_extension();
  if(name && strcmp(name,"dev.cobalt.extension.OnScreenKeyboard")==0)return ytv_osk_extension();
  return nullptr;
}
bool SbSystemSupportsResume(){return false;}
bool SbSystemIsDebuggerAttached(){return false;}
void SbSystemHideSplashScreen(){} // No native Vita startup splash is shown.
}

