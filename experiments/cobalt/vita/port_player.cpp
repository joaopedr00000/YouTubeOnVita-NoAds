/*
 * Starboard 15 SbPlayer for PS Vita: real H.264 + AAC playback.
 *
 * Cobalt (MSE/DASH) hands the player demuxed elementary samples, so the
 * container-based SceAvPlayer cannot consume them. This player drives the
 * same Vita hardware decoders SceAvPlayer uses internally:
 *   video: SceAvcdec (H.264 Annex-B) -> YUV420 planar frames in physically
 *          contiguous memory -> three GL_ALPHA textures (Y, U, V) uploaded on
 *          the rasterizer thread -> SbDecodeTarget kSbDecodeTargetFormat3PlaneYUVI420
 *          (Cobalt's yuv3 shaders sample .a of each plane);
 *   audio: SceAudiodec AAC (each raw frame wrapped in an ADTS header built
 *          from the AudioSpecificConfig) -> S16 PCM ring -> high-priority
 *          thread -> SceAudioOut BGM port at the stream's own sample rate.
 * Clock: audio frames actually written to the port; video-only streams use
 * the system clock. Supported streams are limited by SbMediaCanPlay* in
 * port_media_policy.cpp (H.264 up to 854x480@30, AAC-LC stereo).
 */
#include "starboard/player.h"
#include "starboard/decode_target.h"
#include "starboard/media.h"
#include "starboard/log.h"
#include "starboard/mutex.h"
#include "starboard/condition_variable.h"
#include <vitaGL.h>
#include "time_stretch.h"
extern volatile uint64_t g_ytv_audio_active_us;  // port_addons.cpp: keep-awake
#include <psp2/videodec.h>
#include <psp2/audiodec.h>
#include <psp2/audioout.h>
#include <psp2/sysmodule.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/gxm.h>
#include <pthread.h>
#include <malloc.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <set>
#include <vector>

// GPU-mapped decoder output pool. Shared by the player and every decode
// target whose textures sample it; freed when the last reference goes.
struct VideoPool {
  volatile int refs;
  SceUID mem;
  void* ptr;
  bool mapped;
};

// Decode targets follow the Starboard contract: Cobalt keeps each target it
// got from SbPlayerGetCurrentFrame (its render-tree images hold the texture
// ids) until it calls SbDecodeTargetRelease, possibly after the player is
// gone. A target owns its textures and a pool reference; the player holds one
// reference to its current target, every GetCurrentFrame adds one.
struct SbDecodeTargetPrivate {
  volatile int refs;
  SbDecodeTargetInfo info;
  GLuint tex[3];
  VideoPool* pool;
};

extern "C" { volatile int g_ytv_player_instances = 0; }
extern "C" bool ytv_native_player_present() {
  return __atomic_load_n(&g_ytv_player_instances, __ATOMIC_RELAXED) > 0;
}
extern "C" bool ytv_gl_context_is_current();
extern "C" { extern volatile int g_ytv_video_bilinear_enabled; }
extern "C" { extern volatile int g_ytv_video_aspect_mode; }
extern "C" { extern volatile int g_ytv_video_vertical_mode; }  // 0 off, 1 cw, 2 ccw
extern "C" { extern volatile int g_ytv_media_volume_pct; }
#include "port_video_crop.h"
// Exported by SceVideodec but missing from the VitaSDK header.
extern "C" int sceAvcdecDecodeFlush(SceAvcdecCtrl* decoder);
// Above 1280x720 pixels SceAvcdec only accepts the non-game entry points
// (the kernel checks the caller's program type). Missing from the headers.
extern "C" int sceVideodecInitLibraryNongameapp(SceUInt32 codec,
                                                 const SceVideodecQueryInitInfoHwAvcdec* info);
extern "C" int ytv_addons_cfg_fhd();  // port_addons.cpp
extern "C" bool ytv_lowmem();         // port_system.cpp (YouTube Lite)
// HUD numbers (port_addons.cpp reads them).
volatile int g_ytv_hud_video_h = 0;
volatile unsigned g_ytv_hud_dropped_total = 0;
volatile bool g_ytv_fhd_decoder_created = false;  // read by the Full HD guard
extern "C" void ytv_addons_fhd_ok();              // port_addons.cpp
extern "C" void ytv_addons_fhd_begin();           // port_addons.cpp
extern "C" void ytv_addons_fhd_end();
extern "C" void ytv_media_reject_video_size(int width,int height);             // port_addons.cpp
extern "C" int sceAvcdecQueryDecoderMemSizeNongameapp(SceUInt32 codec,
    const SceAvcdecQueryDecoderInfo* query, SceAvcdecDecoderInfo* info);
extern "C" int sceAvcdecCreateDecoderNongameapp(SceUInt32 codec, SceAvcdecCtrl* ctrl,
    const SceAvcdecQueryDecoderInfo* query);

namespace {
// Decoder capacity tiers (width/height multiples of 16). The decoder and the
// frame pool are sized for the largest tier that fits in memory, so YouTube's
// adaptive switches (240p <-> 1080p) need no new decoder.
constexpr int kMaxWidth = 1920;
constexpr int kMaxHeight = 1088;
struct Tier { int w, h; };
// v0.64: {480, 864} holds portrait Shorts up to 480x854. Without it no tier
// was tall enough and the player could not be created at all.
constexpr Tier kTiers[] = {{1920, 1088}, {1280, 720}, {864, 480}, {480, 864},
                           {640, 368}, {448, 240}};
// Frames are shown straight from decoder memory (no copy), so the pool also
// holds the frame on screen and those the GPU may still be reading.
constexpr int kFramePool = 6;
constexpr int kGamePixelLimit = 1280 * 720;  // SceAvcdec limit for game apps
int pool_frames(int cap_w, int cap_h) { return cap_w * cap_h > kGamePixelLimit ? 4 : kFramePool; }
// GXM uses queued display buffers. Two frames were insufficient when native
// downloads/MP3 encoding delayed the rasterizer: a decoder slot could be
// reused before a queued draw finished sampling it, causing corrupted pixels.
// Keep one extra presented-frame margin (still within the six-slot pool).
constexpr uint32_t kGpuFramesInFlight = 4;  // submitted Vita frames, not decoder jobs
// This counter advances only after eglSwapBuffers actually submits a frame.
// vglGetFrameNumber is a renderer-internal counter and must not be used for
// synchronizing decoder-plane reuse with the display queue.
extern volatile uint32_t g_swaps;
constexpr int kVideoQueueTarget = 12;
constexpr int kAudioQueueTarget = 24;
constexpr int kAudioGrain = 1024;            // frames per sceAudioOutOutput
constexpr int kPcmRingFrames = 48000 * 2;    // up to ~2 s buffered
constexpr int kAudioThreadPriority = 64;
constexpr int kWorkerThreadPriority = 96;  // above the UI (128), below audio

void plog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void plog(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  SbLogRaw(line);
}

inline int align16(int v) { return (v + 15) & ~15; }
// v0.64: output pitch for SceAvcdec. Every stream that decoded on hardware
// (v0.47-v0.62 logs: 1280, 854->864, 640, 626->640, 960, 568->576, 480, 256,
// 792->800, 600->608) had a 16-aligned width that is also a multiple of 32;
// every one refused with UNSUPPORT_IMAGE_SIZE did not (1190->1200, 720x720,
// 240x426 -> 240). The picture layout now uses a 32-aligned pitch; the
// visible size still comes from the decoder (horizontalSize/verticalSize).
inline int align32(int v) { return (v + 31) & ~31; }

// ---- H.264 SPS (v0.64) -----------------------------------------------------
// Decoders were always created for 5 reference frames. Streams whose SPS asks
// for a larger decoded picture buffer (non-standard sizes such as 1190x720 or
// the 240x426 portrait stream in the v0.62 logs) were refused by SceAvcdec with
// UNSUPPORT_IMAGE_SIZE. The SPS of every key frame is read so the decoder can
// be recreated with the buffer the stream really needs, and so hardware logs
// show what the stream asks for.
struct SpsInfo {
  int profile, level;
  int max_refs;          // max_num_ref_frames
  int dpb;               // VUI max_dec_frame_buffering, or the level's limit
  bool dpb_from_vui;
  int coded_w, coded_h;  // macroblock size (multiples of 16)
  int width, height;     // after cropping
  bool frame_mbs_only;
};
struct BitReader {
  const uint8_t* d; int n; int pos; bool bad;
  int bit() {
    if (pos >= n * 8) { bad = true; return 0; }
    int b = (d[pos >> 3] >> (7 - (pos & 7))) & 1;
    pos++;
    return b;
  }
  uint32_t bits(int k) { uint32_t v = 0; while (k-- > 0) v = (v << 1) | (uint32_t)bit(); return v; }
  uint32_t ue() {
    int zeros = 0;
    while (!bit()) { if (bad || ++zeros > 31) { bad = true; return 0; } }
    return zeros ? ((1u << zeros) - 1 + bits(zeros)) : 0;
  }
  int32_t se() { uint32_t v = ue(); return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2); }
};
void skip_scaling_list(BitReader& b, int size) {
  int last = 8, next = 8;
  for (int j = 0; j < size && !b.bad; j++) {
    if (next) next = (last + b.se() + 256) % 256;
    last = next ? next : last;
  }
}
void skip_hrd(BitReader& b) {
  uint32_t cnt = b.ue();
  b.bits(8);
  for (uint32_t i = 0; i <= cnt && i < 32 && !b.bad; i++) { b.ue(); b.ue(); b.bit(); }
  b.bits(20);
}
int level_max_dpb_mbs(int level) {
  switch (level) {
    case 9: case 10: return 396;
    case 11: return 900;
    case 12: case 13: case 20: return 2376;
    case 21: return 4752;
    case 22: case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40: case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    default: return level > 50 ? 184320 : 0;
  }
}
bool parse_sps_rbsp(const uint8_t* d, int n, SpsInfo* o) {
  BitReader b{d, n, 0, false};
  memset(o, 0, sizeof(*o));
  o->profile = (int)b.bits(8);
  b.bits(8);
  o->level = (int)b.bits(8);
  b.ue();
  int chroma = 1;
  switch (o->profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118:
    case 128: case 138: case 139: case 134: case 135:
      chroma = (int)b.ue();
      if (chroma == 3) b.bit();
      b.ue(); b.ue(); b.bit();
      if (b.bit()) {
        for (int i = 0; i < (chroma != 3 ? 8 : 12) && !b.bad; i++)
          if (b.bit()) skip_scaling_list(b, i < 6 ? 16 : 64);
      }
      break;
    default: break;
  }
  b.ue();  // log2_max_frame_num_minus4
  uint32_t poc = b.ue();
  if (poc == 0) {
    b.ue();
  } else if (poc == 1) {
    b.bit(); b.se(); b.se();
    uint32_t c = b.ue();
    for (uint32_t i = 0; i < c && i < 256 && !b.bad; i++) b.se();
  }
  o->max_refs = (int)b.ue();
  b.bit();
  int wmbs = (int)b.ue() + 1;
  int hmap = (int)b.ue() + 1;
  o->frame_mbs_only = b.bit();
  if (!o->frame_mbs_only) b.bit();
  b.bit();
  int cl = 0, cr = 0, ct = 0, cb = 0;
  if (b.bit()) { cl = (int)b.ue(); cr = (int)b.ue(); ct = (int)b.ue(); cb = (int)b.ue(); }
  if (b.bad || wmbs > 512 || hmap > 512) return false;
  const int hmbs = hmap * (o->frame_mbs_only ? 1 : 2);
  o->coded_w = wmbs * 16;
  o->coded_h = hmbs * 16;
  const int cy = (chroma == 1 ? 2 : 1) * (o->frame_mbs_only ? 1 : 2);
  o->width = o->coded_w - (chroma ? 2 : 1) * (cl + cr);
  o->height = o->coded_h - (chroma ? cy : (o->frame_mbs_only ? 1 : 2)) * (ct + cb);
  int lv = level_max_dpb_mbs(o->level);
  o->dpb = lv ? std::min(16, lv / (wmbs * hmbs)) : 0;
  if (b.bit()) {  // vui_parameters_present_flag
    if (b.bit() && b.bits(8) == 255) b.bits(32);
    if (b.bit()) b.bit();
    if (b.bit()) { b.bits(4); if (b.bit()) b.bits(24); }
    if (b.bit()) { b.ue(); b.ue(); }
    if (b.bit()) { b.bits(32); b.bits(32); b.bit(); }
    bool nal = b.bit(); if (nal) skip_hrd(b);
    bool vcl = b.bit(); if (vcl) skip_hrd(b);
    if (nal || vcl) b.bit();
    b.bit();
    if (b.bit()) {  // bitstream_restriction_flag
      b.bit(); b.ue(); b.ue(); b.ue(); b.ue();
      b.ue();  // max_num_reorder_frames
      uint32_t mdfb = b.ue();
      if (!b.bad && mdfb <= 16) { o->dpb = (int)mdfb; o->dpb_from_vui = true; }
    }
  }
  return true;
}
// Finds an SPS NAL in the first bytes of an Annex B access unit.
bool find_sps(const uint8_t* d, int n, SpsInfo* o) {
  const int lim = std::min(n, 512);
  for (int i = 0; i + 4 < lim; i++) {
    if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
    if ((d[i + 3] & 0x1F) != 7) continue;
    uint8_t rbsp[256];
    int k = 0, zeros = 0;
    for (int j = i + 4; j < n && k < (int)sizeof(rbsp); j++) {
      if (zeros >= 2 && d[j] == 3) { zeros = 0; continue; }  // emulation prevention
      if (zeros >= 2 && d[j] <= 1) break;                     // next start code
      zeros = d[j] == 0 ? zeros + 1 : 0;
      rbsp[k++] = d[j];
    }
    return parse_sps_rbsp(rbsp, k, o);
  }
  return false;
}
// Reference frames to ask the decoder for: what the stream may hold, at least
// the historical 5, at most 16.
int sps_decoder_refs(const SpsInfo& s) {
  return std::max(5, std::min(16, std::max(s.max_refs, s.dpb)));
}

// ---- one-time decoder library initialisation ------------------------------
pthread_once_t g_libs_once = PTHREAD_ONCE_INIT;

// YouTube Lite: 720p (up to 60 fps) with a crash guard. A probe file exists
// while a >480p player is open; if it is still there at the next start, that
// run died with 720p playing, so this run stays at 480p (and removes the
// probe, so the following start tries 720p again).
constexpr const char* kLite720Probe = "ux0:data/youtubevita/lite720_probe";
int g_lite720 = -1;  // -1 unknown, 0 480p max, 1 720p allowed
bool g_lite720_probe_open = false;
bool g_video_lib = false, g_audio_lib = false;
int g_lib_pixels = 0;  // largest picture the initialised video library accepts
bool g_lib_nongame = false;            // library initialised for 1920x1088
int g_lib_refs = 5;                    // reference frames of the game library
int g_lib_fhd_refs = 4;                // reference frames of the 1080p library
volatile bool g_fhd_failed = false;    // the non-game decoder produced no pictures

}  // namespace
extern "C" bool ytv_lowmem_720() {
  if (g_lite720 >= 0) return g_lite720 == 1;
  g_lite720 = 0;
  if (!ytv_lowmem()) return false;
  SceIoStat st;
  if (sceIoGetstat(kLite720Probe, &st) >= 0) {
    sceIoRemove(kLite720Probe);
    plog("YTV_LIFECYCLE_LITE720 fallback=480p (previous run died with 720p open)\n");
    return false;
  }
  g_lite720 = 1;
  plog("YTV_LIFECYCLE_LITE720 enabled=1\n");
  return true;
}
namespace {
void init_libs() {
  sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
  SceVideodecQueryInitInfoHwAvcdec vinit;
  memset(&vinit, 0, sizeof(vinit));
  vinit.size = sizeof(vinit);
  // Game apps may only initialise the AVC decoder up to 1280x720 pixels
  // (the hardware refuses more); v0.23-v0.25 asked for 1920x1088, which can
  // leave no video decoder at all on a real Vita. Largest accepted size wins.
  int v = -1;
  // Full HD (Add-ons, experimental): the non-game library accepts 1920x1088.
  if (ytv_addons_cfg_fhd() && !ytv_lowmem()) {
    // v0.64: 5 reference frames at 1920x1088 is more than H.264 allows at
    // that size (level 4.1/4.2: 4 frames) and v0.62 hardware logs show
    // r=0x80620802 (INVALID_PARAM) for it. Try 4 first, then the old 5.
    vinit.horizontal = 1920;
    vinit.vertical = 1088;
    vinit.numOfStreams = 1;
    for (int refs : {4, 5}) {
      vinit.numOfRefFrames = (uint32_t)refs;
      v = sceVideodecInitLibraryNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &vinit);
      plog("YTV_LIFECYCLE_PLAYER_VIDEODEC_INIT nongame 1920x1088 refs=%d r=0x%x\n", refs,
           (unsigned)v);
      if (v >= 0) { g_lib_pixels = 1920 * 1088; g_lib_nongame = true; g_lib_fhd_refs = refs; break; }
    }
  }
  const Tier lib_sizes[] = {{1280, 720}, {864, 480}};
  for (const Tier& t : lib_sizes) {
    if (g_lib_nongame) break;
    // YouTube Lite plays 480p at most: smaller library and decoder memory.
    if (ytv_lowmem() && !ytv_lowmem_720() && t.w > 864) continue;
    vinit.horizontal = (uint32_t)t.w;
    vinit.vertical = (uint32_t)t.h;
    vinit.numOfStreams = 1;
    // v0.64: room for streams that need more than 5 reference frames
    // (decoders still ask for 5 unless the stream's SPS needs more). 9 is
    // the H.264 level 4.2 buffer at 1280x720; 5 is the old, proven value.
    for (int refs : {9, 5}) {
      vinit.numOfRefFrames = (uint32_t)refs;
      v = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &vinit);
      plog("YTV_LIFECYCLE_PLAYER_VIDEODEC_INIT %dx%d refs=%d r=0x%x\n", t.w, t.h, refs,
           (unsigned)v);
      if (v >= 0) { g_lib_refs = refs; break; }
    }
    if (v >= 0) { g_lib_pixels = t.w * t.h; break; }
  }
  g_video_lib = v >= 0;
  sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC);
  SceAudiodecInitParam ainit;
  memset(&ainit, 0, sizeof(ainit));
  ainit.size = sizeof(ainit.aac);
  ainit.aac.totalStreams = 1;
  int a = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &ainit);
  g_audio_lib = a >= 0;
  plog("YTV_LIFECYCLE_PLAYER_LIBS videodec=0x%x audiodec=0x%x\n", (unsigned)v, (unsigned)a);
}

SceUID alloc_device_memory(const char* name, uint32_t size, void** base) {
  // The decoder engine needs physically contiguous (or CDRAM) memory.
  uint32_t phy = (size + 0xFFFFF) & ~0xFFFFFu;
  SceUID uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,
                                      phy, nullptr);
  if (uid < 0) {
    uint32_t cd = (size + 0x3FFFF) & ~0x3FFFFu;
    uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, cd, nullptr);
  }
  if (uid >= 0 && sceKernelGetMemBlockBase(uid, base) < 0) {
    sceKernelFreeMemBlock(uid);
    uid = -1;
  }
  return uid;
}

struct Sample {
  const void* buffer;
  int size;
  int64_t timestamp;
  int width, height;   // video: coded size of the stream at this sample
};

struct Frame {
  bool used;
  int64_t pts;
  int width, height;   // displayed size
  int pitch, h16;      // layout of this frame (stream size aligned to 16)
  uint8_t* data;       // Y (pitch*h16) then U, V (pitch/2 * h16/2 each)
  int cx, cy, cw, ch;  // v0.64: picture shown (aspect mode crop), even values
  uint32_t retired_at; // vitaGL frame number when it left the screen (0: never shown)
};
// A released frame can be decoded into again once the GPU is done with it.
inline bool frame_free(const Frame& f) {
  if (!f.data || f.used) return false;
  if (f.retired_at == 0) return true;  // never presented to the GPU
  const uint32_t submitted = __atomic_load_n(&g_swaps,__ATOMIC_ACQUIRE);
  return (int32_t)(submitted-f.retired_at) >= 0;
}
}  // namespace

struct SbPlayerPrivate {
  // Cobalt side.
  SbPlayerDeallocateSampleFunc deallocate;
  SbPlayerDecoderStatusFunc decoder_status;
  SbPlayerStatusFunc player_status;
  SbPlayerErrorFunc player_error;
  void* context;
  SbDecodeTargetGraphicsContextProvider* provider;

  bool has_video, has_audio;
  SbMutex mutex;               // native LwMutex/LwCond (port_sync.cpp)
  SbConditionVariable cond;
  volatile bool stopping;
  pthread_t worker;

  // Control.
  int ticket;
  bool seek_pending;
  int64_t seek_to;
  SbPlayerState state;
  bool eos_reported;
  double rate, volume;

  // Input.
  std::deque<Sample> video_in, audio_in;
  bool video_eos_in, audio_eos_in;
  bool video_need_pending, audio_need_pending;

  // Video decode.
  int pitch, height16;          // decoder output layout
  int stream_width, stream_height;
  SceAvcdecCtrl avc;
  bool avc_ready;
  SceUID avc_mem, frame_mem;
  void* frame_pool; uint32_t frame_pool_size; bool frame_pool_mapped;
  int displayed;                // slot bound to the textures, -1 if none
  Frame frames[kFramePool];
  std::multiset<int64_t> pending_pts;
  int total_frames, dropped_frames;
  // Playback statistics (YTV_LIFECYCLE_PLAYER_STATS, always logged).
  int shown_frames, frame_calls, backwards;
  uint32_t serial;  // unique per player (addresses get reused)
  uint64_t decode_us;
  int64_t last_video_pts;
  // Video the hardware decoder refuses (UNSUPPORT_IMAGE_SIZE, e.g. the
  // 720x720 stream of YouTube Music's lyrics view): one retry with a decoder
  // sized exactly for the stream, then audio-only playback.
  int size_errors;
  int fhd_errors;
  bool fhd_guard_active;
  bool fhd_restart_pending;
  bool unsupported_size_restart_pending;
  bool size_retry_done;
  bool video_broken;
  int dec_refs;                 // reference frames the decoder was created for
  SpsInfo sps;                  // last SPS seen (v0.64)
  bool sps_valid;

  // Audio decode/output.
  int channels, sample_rate;
  uint8_t adts_profile, adts_freq_index, adts_channels;
  SceAudiodecCtrl aac;
  SceAudiodecInfo aac_info;
  bool aac_ready;
  uint8_t* es_buffer;
  int16_t* pcm_buffer;
  int16_t* ring;                // stereo S16
  int ring_read, ring_count;
  int port;
  pthread_t audio_thread;
  bool audio_thread_running;
  int64_t audio_base_pts;
  bool audio_base_set;
  uint64_t frames_played;
  // Smooth audio clock: frames_played advances in 1024-frame steps (23 ms);
  // between steps the clock is interpolated with the system timer, capped at
  // one step, and never goes backwards. A stepped clock made 24/30 fps video
  // alternate between 1, 2 and 3 refreshes per frame on hardware.
  uint64_t clock_update_us;
  uint32_t audio_epoch;      // bumped on seek: the audio thread resets its state
  int64_t clock_step_us;
  int64_t last_media;

  // Video-only clock.
  int64_t wall_base_pts;
  uint64_t wall_start_us;
  bool clock_running;

  // GL output (rasterizer thread only).
  VideoPool* pool;               // player's reference
  SbDecodeTargetPrivate* cur;    // player's reference (textures for the stream size)
  int tex_pitch, tex_height;
  int64_t uploaded_pts;
  bool have_target;
};

namespace {
uint64_t now_us() { return sceKernelGetSystemTimeWide(); }

// Media time in microseconds (call with mutex held).
int64_t media_time_locked(SbPlayer p) {
  if (p->state != kSbPlayerStatePresenting && p->state != kSbPlayerStateEndOfStream)
    return p->seek_to;
  if (p->has_audio) {
    if (!p->audio_base_set) return p->seek_to;
    int64_t t = p->audio_base_pts + (int64_t)(p->frames_played * 1000000ull / (uint64_t)p->sample_rate);
    if (p->rate > 0.0 && p->state == kSbPlayerStatePresenting && p->clock_update_us) {
      // clock_step_us is the real duration of the last block; media time
      // advances rate times faster (playback speed).
      int64_t extra = (int64_t)(now_us() - p->clock_update_us);
      if (extra > p->clock_step_us) extra = p->clock_step_us;
      if (extra > 0) t += (int64_t)((double)extra * p->rate);
    }
    if (t < p->last_media) t = p->last_media;
    p->last_media = t;
    return t;
  }
  if (!p->clock_running) return p->wall_base_pts;
  return p->wall_base_pts + (int64_t)((now_us() - p->wall_start_us) * p->rate);
}

// ---- callbacks (never with the mutex held) --------------------------------
void report_state(SbPlayer p, SbPlayerState s, int ticket) {
  if (p->player_status) p->player_status(p, p->context, s, ticket);
}
void need_data(SbPlayer p, SbMediaType type, int ticket) {
  if (p->decoder_status)
    p->decoder_status(p, p->context, type, kSbPlayerDecoderStateNeedsData, ticket);
}
void free_sample(SbPlayer p, const void* buffer) {
  if (p->deallocate) p->deallocate(p, p->context, buffer);
}

// ---- video ----------------------------------------------------------------
void log_free_memory(const char* when) {
  SceKernelFreeMemorySizeInfo m;
  memset(&m, 0, sizeof(m));
  m.size = sizeof(m);
  sceKernelGetFreeMemorySize(&m);
  plog("YTV_LIFECYCLE_PLAYER_MEM %s user=%dKiB cdram=%dKiB phycont=%dKiB\n", when, m.size_user / 1024,
       m.size_cdram / 1024, m.size_phycont / 1024);
}

// Unmaps the frame pool from the GPU and frees it. Call only once the GPU can
// no longer read it (textures deleted after glFinish, or never displayed).
void release_frame_pool(SceUID mem, void* pool, bool mapped) {
  if (mapped && pool) sceGxmUnmapMemory(pool);
  if (mem >= 0) sceKernelFreeMemBlock(mem);
}

void pool_unref(VideoPool* pool) {
  if (pool && __sync_sub_and_fetch(&pool->refs, 1) == 0) {
    release_frame_pool(pool->mem, pool->ptr, pool->mapped);
    delete pool;
  }
}

// Targets nobody references any more wait here for the GPU: deleted on the
// GL thread 4 presented frames later (ytv_player_on_swap), or right away
// after a glFinish (flush_dead_targets).
SbMutex g_dead_mutex = SB_MUTEX_INITIALIZER;
struct DeadTarget { SbDecodeTargetPrivate* t; uint32_t due; };
std::vector<DeadTarget> g_dead;
volatile uint32_t g_swaps = 0;
volatile int g_live_targets = 0, g_dead_count = 0;  // diagnostics

// v0.64 Vertical mode: the decode target of the portrait video being
// watched (a reference is held). The Add-ons overlay draws it rotated over
// the whole screen. Only players with sound qualify: the muted previews of the
// home page never take the screen.
pthread_mutex_t g_vert_lock = PTHREAD_MUTEX_INITIALIZER;
SbPlayer g_vert_owner = nullptr;
SbDecodeTargetPrivate* g_vert_target = nullptr;
int g_vert_w = 0, g_vert_h = 0, g_vert_tw = 0, g_vert_th = 0;
uint64_t g_vert_us = 0;
void target_unref(SbDecodeTargetPrivate* t);
void vertical_publish(SbPlayer p, SbDecodeTargetPrivate* t, int w, int h, int tw, int th);
void vertical_forget(SbPlayer p);
void target_unref(SbDecodeTargetPrivate* t) {
  if (!t || __sync_sub_and_fetch(&t->refs, 1) != 0) return;
  SbMutexAcquire(&g_dead_mutex);
  g_dead.push_back({t, g_swaps + 4});
  g_dead_count = (int)g_dead.size();
  SbMutexRelease(&g_dead_mutex);
}

// GL thread only. all=true: everything queued (the caller did glFinish).
void free_dead_targets(bool all) {
  std::vector<SbDecodeTargetPrivate*> ready;
  SbMutexAcquire(&g_dead_mutex);
  for (size_t i = 0; i < g_dead.size();) {
    if (all || (int32_t)(g_swaps - g_dead[i].due) >= 0) {
      ready.push_back(g_dead[i].t);
      g_dead[i] = g_dead.back();
      g_dead.pop_back();
    } else {
      i++;
    }
  }
  g_dead_count = (int)g_dead.size();
  SbMutexRelease(&g_dead_mutex);
  for (SbDecodeTargetPrivate* t : ready) {
    __sync_sub_and_fetch(&g_live_targets, 1);
    if (t->tex[0]) glDeleteTextures(3, t->tex);
    pool_unref(t->pool);
    delete t;
  }
}

bool dead_targets_pending() {
  SbMutexAcquire(&g_dead_mutex);
  bool any = !g_dead.empty();
  SbMutexRelease(&g_dead_mutex);
  return any;
}

void flush_dead_targets(void*) {
  glFinish();
  free_dead_targets(true);
}

SbDecodeTargetPrivate* new_target(VideoPool* pool, int pitch, int h16) {
  // Creating a new target binds three GL_ALPHA plane textures. The previous
  // implementation left plane V bound, so upload_current saved the wrong
  // "previous" binding and leaked it into Cobalt/Skia's texture cache.
  // Snapshot the caller's binding BEFORE creating or initializing any plane.
  GLint original_binding = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D,&original_binding);
  SbDecodeTargetPrivate* t = new SbDecodeTargetPrivate();
  t->refs = 1;  // the player's
  __sync_add_and_fetch(&g_live_targets, 1);
  t->pool = pool;
  __sync_add_and_fetch(&pool->refs, 1);
  const GLint sampler = __atomic_load_n(&g_ytv_video_bilinear_enabled,__ATOMIC_RELAXED)
                            ? GL_LINEAR : GL_NEAREST;
  glGenTextures(3, t->tex);
  plog("YTV_LIFECYCLE_VIDEO_FILTER_TARGET mode=%s pitch=%d h16=%d\n",
       sampler==GL_NEAREST?"nearest":"bilinear",pitch,h16);
  for (int i = 0; i < 3; i++) {
    int w = i == 0 ? pitch : pitch / 2;
    int h = i == 0 ? h16 : h16 / 2;
    glBindTexture(GL_TEXTURE_2D, t->tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, sampler);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, sampler);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D,(GLuint)original_binding);
  GLint verified_binding=0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D,&verified_binding);
  plog("YTV_LIFECYCLE_VIDEO_TARGET_GL_BINDING_RESTORED before=%d after=%d verified=%d\n",
       (int)original_binding,(int)verified_binding,
       (int)(verified_binding==original_binding));
  return t;
}

bool try_create_decoder(SbPlayer p, int cap_w, int cap_h, int refs = 0) {
  const bool nongame = cap_w * cap_h > kGamePixelLimit;  // a 1080p decoder
  // v0.64: once the non-game library is initialised (Full HD option) the
  // game library is not, so EVERY decoder must use the non-game calls, and
  // none may ask for more reference frames than that library was set up
  // with. v0.64 hardware log: game-API creates after a successful non-game
  // init failed with 0x80620002 at every size, so no video played.
  const bool ng_api = g_lib_nongame;
  if (refs <= 0) refs = nongame ? g_lib_fhd_refs : 5;
  if (ng_api && refs > g_lib_fhd_refs) refs = g_lib_fhd_refs;
  SceAvcdecQueryDecoderInfo query = {(uint32_t)cap_w, (uint32_t)cap_h, (uint32_t)refs};
  SceAvcdecDecoderInfo need;
  memset(&need, 0, sizeof(need));
  if(nongame)ytv_addons_fhd_begin();
  int q = ng_api ? sceAvcdecQueryDecoderMemSizeNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &need)
                 : sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &need);
  if (q < 0 || need.frameMemSize == 0) {
    if(nongame)ytv_addons_fhd_end();
    plog("YTV_LIFECYCLE_PLAYER_FHD query=%dx%d refs=%d r=0x%x required=%u\n",
         cap_w,cap_h,refs,(unsigned)q,(unsigned)need.frameMemSize);
    return false;
  }
  void* mem = nullptr;
  p->avc_mem = alloc_device_memory("ytv_avc_ref", need.frameMemSize, &mem);
  if (p->avc_mem < 0) {
    if(nongame)ytv_addons_fhd_end();
    plog("YTV_LIFECYCLE_PLAYER_FHD ref_alloc_failed required=%u\n",
         (unsigned)need.frameMemSize);
    return false;
  }
  memset(&p->avc, 0, sizeof(p->avc));
  p->avc.frameBuf.pBuf = mem;
  p->avc.frameBuf.size = need.frameMemSize;
  int r = ng_api ? sceAvcdecCreateDecoderNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &p->avc, &query)
                 : sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &p->avc, &query);
  if (r < 0) {
    plog("YTV_LIFECYCLE_PLAYER avcdec create %dx%d refs=%d api=%s failed 0x%x\n", cap_w, cap_h,
         refs, ng_api ? "nongame" : "game", (unsigned)r);
    sceKernelFreeMemBlock(p->avc_mem);
    p->avc_mem = -1;
    if(nongame)ytv_addons_fhd_end();
    return false;
  }
  uint32_t frame_bytes = (uint32_t)(cap_w * cap_h * 3 / 2);
  const int frames = pool_frames(cap_w, cap_h);
  void* pool = nullptr;
  SceUID pool_mem = alloc_device_memory("ytv_avc_frames", frame_bytes * frames, &pool);
  // The GPU samples the planes in place: the whole pool must be GPU-mapped
  // (4 KiB granularity; the block itself is rounded up further).
  uint32_t pool_size = (frame_bytes * frames + 0xFFFu) & ~0xFFFu;
  if (pool_mem < 0 || sceGxmMapMemory(pool, pool_size, SCE_GXM_MEMORY_ATTRIB_READ) < 0) {
    if (pool_mem >= 0) sceKernelFreeMemBlock(pool_mem);
    sceAvcdecDeleteDecoder(&p->avc);
    sceKernelFreeMemBlock(p->avc_mem);
    p->avc_mem = -1;
    if(nongame)ytv_addons_fhd_end();
    plog("YTV_LIFECYCLE_PLAYER_FHD mapped_pool_failed required=%u\n",
         (unsigned)pool_size);
    return false;
  }
  p->frame_mem = pool_mem;
  p->frame_pool = pool;
  p->frame_pool_size = pool_size;
  p->frame_pool_mapped = true;
  p->pool = new VideoPool{1, pool_mem, pool, true};
  for (int i = 0; i < kFramePool; i++) {
    p->frames[i].data = i < frames ? static_cast<uint8_t*>(pool) + (size_t)frame_bytes * i : nullptr;
    p->frames[i].used = false;  // slots beyond the pool have no data: never free
    p->frames[i].retired_at = 0;
  }
  p->pitch = cap_w;
  p->height16 = cap_h;
  p->displayed = -1;
  p->avc_ready = true;
  p->dec_refs = refs;
  p->fhd_guard_active=nongame;
  if (nongame) {
    g_ytv_fhd_decoder_created = true;
    plog("YTV_LIFECYCLE_PLAYER_FHD decoder_created=1 capacity=%dx%d\n",cap_w,cap_h);
  }
  plog("YTV_LIFECYCLE_PLAYER_DECODER capacity=%dx%d refs=%d ref=%uKiB pool=%uKiB\n", cap_w,
       cap_h, refs, (unsigned)(need.frameMemSize / 1024), (unsigned)(pool_size / 1024));
  return true;
}

}  // namespace
// Media policy: whether a 1080p decoder can exist right now (non-game
// decoder accepted by the system and enough device memory for it).
extern "C" bool ytv_player_can_decode_1080p() {
  pthread_once(&g_libs_once, init_libs);
  if (!g_lib_nongame || g_fhd_failed) return false;
  SceAvcdecQueryDecoderInfo query = {1920, 1088, (uint32_t)g_lib_fhd_refs};
  SceAvcdecDecoderInfo need;
  memset(&need, 0, sizeof(need));
  if (sceAvcdecQueryDecoderMemSizeNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &need) < 0 ||
      need.frameMemSize == 0)
    return false;
  SceKernelFreeMemorySizeInfo m;
  memset(&m, 0, sizeof(m));
  m.size = sizeof(m);
  sceKernelGetFreeMemorySize(&m);
  uint32_t pool = 1920u * 1088u * 3u / 2u * 4u;
  uint32_t avail = (uint32_t)m.size_phycont + (uint32_t)m.size_cdram;
  return avail >= need.frameMemSize + pool + (2u << 20);
}
namespace {
void vertical_publish(SbPlayer p, SbDecodeTargetPrivate* t, int w, int h, int tw, int th) {
  if (!t) return;
  SbDecodeTargetPrivate* old = nullptr;
  pthread_mutex_lock(&g_vert_lock);
  if (g_vert_target != t) {
    old = g_vert_target;
    __sync_add_and_fetch(&t->refs, 1);
    g_vert_target = t;
  }
  g_vert_owner = p;
  g_vert_w = w; g_vert_h = h; g_vert_tw = tw; g_vert_th = th;
  g_vert_us = sceKernelGetSystemTimeWide();
  pthread_mutex_unlock(&g_vert_lock);
  if (old) target_unref(old);
}
void vertical_forget(SbPlayer p) {
  SbDecodeTargetPrivate* old = nullptr;
  pthread_mutex_lock(&g_vert_lock);
  if (g_vert_owner == p) {
    old = g_vert_target;
    g_vert_target = nullptr;
    g_vert_owner = nullptr;
  }
  pthread_mutex_unlock(&g_vert_lock);
  if (old) target_unref(old);
}
// Renderer thread (Add-ons overlay): the portrait frame to draw, with a
// reference the caller gives back through ytv_player_vertical_release().
// w/h: visible picture; tw/th: texture (plane 0) size.
extern "C" void* ytv_player_vertical_acquire(unsigned tex[3], int* w, int* h, int* tw, int* th) {
  void* r = nullptr;
  pthread_mutex_lock(&g_vert_lock);
  SbDecodeTargetPrivate* t = g_vert_target;
  SbPlayer p = g_vert_owner;
  // A paused Short keeps its picture; a player that stopped presenting for
  // 2 s (left the page, a muted preview took over) gives the screen back.
  if (t && p && p->volume > 0.01 && sceKernelGetSystemTimeWide() - g_vert_us < 2000000ull) {
    __sync_add_and_fetch(&t->refs, 1);
    for (int i = 0; i < 3; i++) tex[i] = t->tex[i];
    *w = g_vert_w; *h = g_vert_h; *tw = g_vert_tw; *th = g_vert_th;
    r = t;
  }
  pthread_mutex_unlock(&g_vert_lock);
  return r;
}
extern "C" void ytv_player_vertical_release(void* t) {
  if (t) target_unref(static_cast<SbDecodeTargetPrivate*>(t));
}
// Media policy: whether a portrait decoder (480x864, Shorts up to 480x854)
// is accepted by SceAvcdec. Queried once.
extern "C" bool ytv_player_can_decode_portrait() {
  pthread_once(&g_libs_once, init_libs);
  static int cached = -1;
  if (cached < 0) {
    SceAvcdecQueryDecoderInfo query = {480, 864, (uint32_t)(g_lib_nongame ? g_lib_fhd_refs : 5)};
    SceAvcdecDecoderInfo need;
    memset(&need, 0, sizeof(need));
    int q = !g_video_lib ? -1
            : g_lib_nongame ? sceAvcdecQueryDecoderMemSizeNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &need)
                            : sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &need);
    cached = q >= 0 && need.frameMemSize > 0;
    plog("YTV_LIFECYCLE_PLAYER_PORTRAIT query=480x864 r=0x%x ok=%d\n", (unsigned)q, cached);
  }
  return cached == 1;
}
// Largest tier that fits in memory and holds the stream.
bool create_video_decoder(SbPlayer p, int need_w, int need_h) {
  log_free_memory("before_decoder");
  const bool needs_fhd=(int64_t)need_w*need_h>kGamePixelLimit;
  // Critical v0.60 fix: with the Full HD option enabled, the old loop
  // attempted 1920x1088 for EVERY video, even 360p. Only try the nongame
  // decoder for actual >720p streams; preserve the regular decoder for
  // lower resolutions, both in Full and Lite.
  // YouTube Music's square album-art/lyrics player typically emits 480x480
  // H.264. A 1280x720 decoder reserves ~23 MiB for that static picture and
  // competes with MP3 transcoding. Give this shape a compact 864x480 decoder
  // first; preserve the legacy large capacity for regular adaptive videos.
  if(!needs_fhd && need_w<=480 && need_h<=480 && need_h>=400 &&
     g_lib_pixels>=864*480){
    if(try_create_decoder(p,864,480)){
      plog("YTV_LIFECYCLE_PLAYER_LYRICS_COMPACT %dx%d cap=864x480\n",
           need_w,need_h);
      return true;
    }
  }
  for (const Tier& t : kTiers) {
    if (t.w < need_w || t.h < need_h) continue;
    const bool tier_fhd=t.w*t.h>kGamePixelLimit;
    if (tier_fhd && !needs_fhd)continue;
    if (tier_fhd && (!g_lib_nongame || g_fhd_failed))continue;
    if (!tier_fhd && t.w*t.h>g_lib_pixels && !g_lib_nongame)continue;
    if (try_create_decoder(p, t.w, t.h)) return true;
    if(tier_fhd){
      g_fhd_failed=true;
      plog("YTV_LIFECYCLE_PLAYER_FHD decoder_creation_failed cap=%dx%d downgrade=720p\n",
           t.w,t.h);
    }
  }
  return false;
}

void destroy_video_decoder(SbPlayer p) {
  if (p->avc_ready) sceAvcdecDeleteDecoder(&p->avc);
  p->avc_ready = false;
  if(p->fhd_guard_active){
    p->fhd_guard_active=false;
    ytv_addons_fhd_end();
  }
  if (p->avc_mem >= 0) sceKernelFreeMemBlock(p->avc_mem);
  p->avc_mem = -1;
}

// Decodes one sample into a free frame slot. Returns false if no slot.
bool decode_video_sample(SbPlayer p, const Sample& s) {
  if (p->video_broken) return true;  // consumed: audio-only playback
  int slot = -1;
  for (int i = 0; i < kFramePool; i++)
    if (frame_free(p->frames[i])) { slot = i; break; }
  if (slot < 0) return false;
  Frame& f = p->frames[slot];
  // Output layout follows the stream's current size (YouTube switches
  // resolution inside one player); the slot holds up to the decoder capacity.
  const int stream_w=s.width>0?s.width:p->stream_width;
  const int stream_h=s.height>0?s.height:p->stream_height;
  const bool wide720=stream_w==1190 && stream_h==720;
  // Experimental hardware workaround: use the known-valid 1280x720
  // output surface for 1190x720 instead of rejected 1200x720 layout.
  // This is padding only; the compressed H.264 is not transcoded.
  int lw = wide720?1280:align32(stream_w);
  int lh = align16(stream_h);
  if (lw > p->pitch || lh > p->height16) {
    plog("YTV_PLAYER frame %dx%d above capacity %dx%d\n", lw, lh, p->pitch, p->height16);
    return true;  // consumed; cannot be shown with this decoder
  }
  {
    SpsInfo sps;
    if (find_sps(static_cast<const uint8_t*>(s.buffer), s.size, &sps)) {
      if (!p->sps_valid || memcmp(&sps, &p->sps, sizeof(sps)) != 0)
        plog("YTV_LIFECYCLE_PLAYER_SPS profile=%d level=%d refs=%d dpb=%d%s size=%dx%d "
             "coded=%dx%d progressive=%d decoder_refs=%d\n", sps.profile, sps.level,
             sps.max_refs, sps.dpb, sps.dpb_from_vui ? "(vui)" : "(level)", sps.width,
             sps.height, sps.coded_w, sps.coded_h, (int)sps.frame_mbs_only, p->dec_refs);
      p->sps = sps;
      p->sps_valid = true;
    }
  }
  SceAvcdecAu au;
  memset(&au, 0, sizeof(au));
  au.es.pBuf = const_cast<void*>(s.buffer);
  au.es.size = (uint32_t)s.size;
  uint64_t ts = (uint64_t)s.timestamp;
  au.pts.upper = au.dts.upper = (uint32_t)(ts >> 32);
  au.pts.lower = au.dts.lower = (uint32_t)ts;
  SceAvcdecPicture pic;
  memset(&pic, 0, sizeof(pic));
  pic.size = sizeof(pic);
  pic.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_YUV420_RASTER;
  pic.frame.framePitch = (uint32_t)lw;
  pic.frame.frameWidth = (uint32_t)lw;
  pic.frame.frameHeight = (uint32_t)lh;
  pic.frame.pPicture[0] = f.data;
  pic.frame.pPicture[1] = f.data + (size_t)lw * lh;
  if(wide720){
    static bool logged=false;
    if(!logged){logged=true;
      plog("YTV_LIFECYCLE_PLAYER_PADDED_1190 output=1280x720 stream=1190x720\n");
    }
  }
  SceAvcdecPicture* pics[1] = {&pic};
  SceAvcdecArrayPicture array;
  memset(&array, 0, sizeof(array));
  array.numOfElm = 1;
  array.pPicture = pics;
  uint64_t dec_t0 = sceKernelGetSystemTimeWide();
  int r = sceAvcdecDecode(&p->avc, &au, &array);
  p->decode_us += sceKernelGetSystemTimeWide() - dec_t0;
  p->pending_pts.insert(s.timestamp);
  if (r < 0) {
    static int decode_errors = 0;  // always logged (first few): explains silent video
    if (decode_errors++ < 8)
      plog("YTV_LIFECYCLE_PLAYER avcdec decode error 0x%x size=%d layout=%dx%d stream=%dx%d\n",
           (unsigned)r, s.size, lw, lh, p->stream_width, p->stream_height);
    if (p->pitch * p->height16 > kGamePixelLimit && p->total_frames == 0 && ++p->fhd_errors >= 5 &&
        !g_fhd_failed) {
      g_fhd_failed = true;  // CanPlay must reject 1080p for this run.
      p->video_broken = true;
      // Notify Cobalt OUTSIDE p->mutex so the player can be replaced at
      // 720p; merely switching to audio-only leaves a broken video forever.
      p->fhd_restart_pending=true;
      plog("YTV_LIFECYCLE_PLAYER_FHD decode_failed=0x%x restart_at_720p=1\n",
           (unsigned)r);
    }
    if ((unsigned)r == (unsigned)SCE_AVCDEC_ERROR_UNSUPPORT_IMAGE_SIZE && ++p->size_errors >= 3) {
      p->size_errors = 0;
      bool retried = false;
      const int old_refs = p->dec_refs;
      const int want_refs = p->sps_valid ? sps_decoder_refs(p->sps) : old_refs;
      const bool more_refs = want_refs > old_refs;
      if (!p->size_retry_done && (more_refs || lw != p->pitch || lh != p->height16)) {
        // Decoder with the stream's exact (16-aligned) size and, since v0.64,
        // the reference frames its SPS asks for. A smaller capacity keeps the
        // memory of the larger buffer close to what the old decoder used.
        p->size_retry_done = true;
        destroy_video_decoder(p);
        pool_unref(p->pool);  // nothing decoded from it is on screen yet
        p->pool = nullptr;
        p->pending_pts.clear();
        if (more_refs) {
          retried = try_create_decoder(p, lw, lh, want_refs);
          plog("YTV_LIFECYCLE_PLAYER size_retry capacity=%dx%d refs=%d ok=%d\n", lw, lh,
               want_refs, (int)retried);
        }
        if (!retried) {
          retried = try_create_decoder(p, lw, lh);
          plog("YTV_LIFECYCLE_PLAYER size_retry capacity=%dx%d refs=%d ok=%d\n", lw, lh,
               p->dec_refs, (int)retried);
        }
      }
      if (!retried) {
        p->video_broken = true;
        ytv_media_reject_video_size(stream_w,stream_h);
        p->unsupported_size_restart_pending=true;
        plog("YTV_LIFECYCLE_PLAYER_UNSUPPORTED_SIZE %dx%d capability_refresh=1\n",
             p->stream_width,p->stream_height);
      }
    }
    return true;  // sample consumed; the stream may recover at the next key frame
  }
  if (array.numOfOutput > 0 && !p->pending_pts.empty()) {
    // Use the timestamp the decoder attaches to the output picture (the pts
    // of the access unit it came from). Assuming "smallest pending pts" put
    // B-frames out of order on hardware (the image jumped back and forth at
    // full frame rate). Fall back to that guess only if the decoder returns
    // a timestamp that matches no submitted sample.
    int64_t out_pts = (int64_t)(((uint64_t)pic.info.pts.upper << 32) | pic.info.pts.lower);
    auto it = p->pending_pts.find(out_pts);
    if (it == p->pending_pts.end()) {
      static int mismatches = 0;
      if (mismatches++ < 4)
        plog("YTV_LIFECYCLE_PLAYER pts_fallback decoder=%lld\n", (long long)out_pts);
      it = p->pending_pts.begin();
    }
    f.pts = *it;
    p->pending_pts.erase(it);
    f.pitch = lw;
    f.h16 = lh;
    f.width = pic.frame.horizontalSize ? (int)pic.frame.horizontalSize : lw;
    f.height = pic.frame.verticalSize ? (int)pic.frame.verticalSize : lh;
    if (f.width > lw) f.width = lw;
    if (f.height > lh) f.height = lh;
    // v0.64: the aspect-mode crop (and the 1190x720 padding) is applied by
    // the GPU through the decode target's content region. The previous CPU
    // compaction moved ~1.3 MB of mapped decoder memory per frame and caused
    // dropped frames on hardware (1190x720 streams, Zoom/Stretch modes).
    const int mode=__atomic_load_n(&g_ytv_video_aspect_mode,__ATOMIC_RELAXED);
    f.cx=0; f.cy=0; f.cw=f.width&~1; f.ch=f.height&~1;
    if(wide720 || mode!=0){
      const int vw=std::min(f.width,stream_w);
      const int vh=std::min(f.height,stream_h);
      const YtvCropRect crop=ytv_aspect_region(vw,vh,mode);
      if(crop.w>0 && crop.h>0){
        f.cx=crop.x; f.cy=crop.y; f.cw=crop.w; f.ch=crop.h;
        f.width=vw;
        f.height=vh;
        static int last_mode=-1,last_w=0,last_h=0;
        if(last_mode!=mode || last_w!=crop.w || last_h!=crop.h){
          last_mode=mode;last_w=crop.w;last_h=crop.h;
          plog("YTV_LIFECYCLE_VIDEO_ASPECT_FRAME mode=%s source=%dx%d crop=%dx%d xy=%d,%d\n",
             mode==1?"zoom":mode==2?"stretch":"original",
             vw,vh,crop.w,crop.h,crop.x,crop.y);
        }
      }
    }
    f.used = true;
    p->total_frames++;
    g_ytv_hud_video_h = f.height;
    if (p->pitch * p->height16 > kGamePixelLimit && p->total_frames == 30) ytv_addons_fhd_ok();
    if (f.pts > p->last_video_pts) p->last_video_pts = f.pts;
  }
  return true;
}

// ---- audio ----------------------------------------------------------------
const int kAacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                         22050, 16000, 12000, 11025, 8000, 7350};

bool parse_audio_config(SbPlayer p, const SbMediaAudioStreamInfo& info) {
  p->channels = info.number_of_channels > 0 ? info.number_of_channels : 2;
  p->sample_rate = (int)info.samples_per_second;
  int object = 2, freq_index = -1, chan = p->channels;
  if (info.audio_specific_config_size >= 2 && info.audio_specific_config) {
    const uint8_t* c = static_cast<const uint8_t*>(info.audio_specific_config);
    object = c[0] >> 3;
    freq_index = ((c[0] & 7) << 1) | (c[1] >> 7);
    chan = (c[1] >> 3) & 15;
  }
  if (freq_index < 0 || freq_index > 12) {
    freq_index = -1;
    for (int i = 0; i < 13; i++)
      if (kAacRates[i] == p->sample_rate) freq_index = i;
  }
  if (freq_index < 0 || object < 1 || object > 4 || chan < 1 || chan > 2) return false;
  p->sample_rate = kAacRates[freq_index];
  p->channels = chan;
  p->adts_profile = (uint8_t)(object - 1);
  p->adts_freq_index = (uint8_t)freq_index;
  p->adts_channels = (uint8_t)chan;
  return true;
}

bool create_audio_decoder(SbPlayer p) {
  memset(&p->aac, 0, sizeof(p->aac));
  memset(&p->aac_info, 0, sizeof(p->aac_info));
  p->aac_info.aac.size = sizeof(p->aac_info.aac);
  p->aac_info.aac.isAdts = 1;
  p->aac_info.aac.ch = (SceUInt32)p->channels;
  p->aac_info.aac.samplingRate = (SceUInt32)p->sample_rate;
  p->aac_info.aac.isSbr = 0;
  p->aac.size = sizeof(p->aac);
  p->aac.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
  p->aac.pInfo = &p->aac_info;
  int r = sceAudiodecCreateDecoder(&p->aac, SCE_AUDIODEC_TYPE_AAC);
  if (r < 0) {
    plog("YTV_LIFECYCLE_PLAYER audiodec create failed 0x%x\n", (unsigned)r);
    return false;
  }
  p->es_buffer = static_cast<uint8_t*>(memalign(256, 8192));
  p->pcm_buffer = static_cast<int16_t*>(memalign(256, 8192 * 2));
  p->ring = static_cast<int16_t*>(malloc(sizeof(int16_t) * 2 * kPcmRingFrames));
  if (!p->es_buffer || !p->pcm_buffer || !p->ring) return false;
  p->aac_ready = true;
  return true;
}

int ring_space(SbPlayer p) { return kPcmRingFrames - p->ring_count; }

// Decodes one AAC frame into the PCM ring. Returns false if no ring space.
bool decode_audio_sample(SbPlayer p, const Sample& s) {
  if (ring_space(p) < 2048) return false;
  // Cobalt's MP4 parser (Chromium mp4_stream_parser) already converts AAC
  // access units to ADTS; only raw access units get a header added here.
  const uint8_t* in = static_cast<const uint8_t*>(s.buffer);
  bool has_adts = s.size >= 7 && in[0] == 0xFF && (in[1] & 0xF6) == 0xF0;
  int total = has_adts ? s.size : s.size + 7;
  if (total > 8192) return true;  // malformed; drop
  uint8_t* h = p->es_buffer;
  if (has_adts) memcpy(h, in, (size_t)s.size);
  else {
  h[0] = 0xFF;
  h[1] = 0xF1;  // MPEG-4, layer 0, no CRC
  h[2] = (uint8_t)((p->adts_profile << 6) | (p->adts_freq_index << 2) |
                   ((p->adts_channels >> 2) & 1));
  h[3] = (uint8_t)(((p->adts_channels & 3) << 6) | ((total >> 11) & 3));
  h[4] = (uint8_t)((total >> 3) & 0xFF);
  h[5] = (uint8_t)(((total & 7) << 5) | 0x1F);
  h[6] = 0xFC;
  memcpy(h + 7, s.buffer, (size_t)s.size);
  }
  p->aac.pEs = h;
  p->aac.maxEsSize = (SceUInt32)total;
  p->aac.pPcm = p->pcm_buffer;
  p->aac.maxPcmSize = 8192 * 2;
  int r = sceAudiodecDecode(&p->aac);
  if (r < 0) {
    plog("YTV_PLAYER audiodec decode error 0x%x\n", (unsigned)r);
    return true;
  }
  int frames = (int)(p->aac.outputPcmSize / (2 * p->channels));
  {
    static int logged = 0;
    if (logged < 8 || (frames == 0 && logged < 40)) {
      logged++;
      plog("YTV_PLAYER_AAC r=0x%x in=%d used=%u pcm=%u frames=%d\n", (unsigned)r, total,
           (unsigned)p->aac.inputEsSize, (unsigned)p->aac.outputPcmSize, frames);
    }
  }
  if (!p->audio_base_set) {
    p->audio_base_pts = s.timestamp;
    p->audio_base_set = true;
  }
  for (int i = 0; i < frames && p->ring_count < kPcmRingFrames; i++) {
    int w = (p->ring_read + p->ring_count) % kPcmRingFrames;
    int16_t l = p->pcm_buffer[i * p->channels];
    int16_t r2 = p->channels > 1 ? p->pcm_buffer[i * p->channels + 1] : l;
    p->ring[2 * w] = l;
    p->ring[2 * w + 1] = r2;
    p->ring_count++;
  }
  return true;
}

int audio_output_thread(SceSize, void* argp) {
  SbPlayer p = *static_cast<SbPlayer*>(argp);
  alignas(64) int16_t block[kAudioGrain * 2];
  int last_volume = -1;
  // DIAGNOSTIC: with ux0:data/youtubevita/record_audio.txt present, every
  // block sent to the audio port is also appended to rec_player.raw
  // (S16 stereo at the stream rate) so recordings of test runs carry the
  // exact PCM the Vita played. Absent the file, nothing is written.
  SceUID rec = -1;
  {
    SceUID probe = sceIoOpen("ux0:data/youtubevita/record_audio.txt", SCE_O_RDONLY, 0);
    if (probe >= 0) {
      sceIoClose(probe);
      rec = sceIoOpen("ux0:data/youtubevita/rec_player.raw",
                      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    }
  }
  // Playback speed (WSOLA): used whenever rate != 1.
  WsolaStretcher* stretch = new WsolaStretcher();
  double stretch_rate = 1.0;
  uint32_t stretch_epoch = 0;
  while (!p->stopping) {
    SbMutexAcquire(&p->mutex);
    bool playing = p->state == kSbPlayerStatePresenting && p->rate > 0.0 && !p->seek_pending;
    if (playing && p->rate != 1.0) {
      // ---- speed path: pull input into the stretcher, emit two hops ----
      double rate = p->rate;
      if (rate != stretch_rate || p->audio_epoch != stretch_epoch) {
        stretch->Reset();
        stretch_rate = rate;
        stretch_epoch = p->audio_epoch;
      }
      bool draining = p->audio_eos_in && p->audio_in.empty();
      int want = stretch->NeededForHop() + (int)(rate * WsolaStretcher::kHop) + 8 -
                 stretch->Buffered();
      int pull = want > 0 ? want : 0;
      if (pull > p->ring_count) pull = p->ring_count;
      if (pull > stretch->Space()) pull = stretch->Space();
      static int16_t tmp[2 * 4096];
      int pulled = 0;
      while (pulled < pull) {
        int chunk = pull - pulled > 4096 ? 4096 : pull - pulled;
        for (int i = 0; i < chunk; i++) {
          int rd = (p->ring_read + i) % kPcmRingFrames;
          tmp[2 * i] = p->ring[2 * rd];
          tmp[2 * i + 1] = p->ring[2 * rd + 1];
        }
        p->ring_read = (p->ring_read + chunk) % kPcmRingFrames;
        p->ring_count -= chunk;
        stretch->Push(tmp, chunk);
        pulled += chunk;
      }
      int volume = (int)(p->volume * SCE_AUDIO_VOLUME_0DB * __atomic_load_n(&g_ytv_media_volume_pct,__ATOMIC_RELAXED)/100.0);
      SbMutexRelease(&p->mutex);
      if (volume != last_volume) {
        int v[2] = {volume, volume};
        sceAudioOutSetVolume(p->port,
            (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), v);
        last_volume = volume;
      }
      int c1 = stretch->Hop((float)rate, block);
      int c2 = c1 >= 0 ? stretch->Hop((float)rate, block + 2 * WsolaStretcher::kHop) : -1;
      if (c1 < 0 || c2 < 0) {
        (void)draining;  // tail shorter than a frame at EOS is dropped
        SbConditionVariableSignal(&p->cond);
        sceKernelDelayThread(4000);
        continue;
      }
      sceAudioOutOutput(p->port, block);
      g_ytv_audio_active_us = now_us();
      if (rec >= 0) sceIoWrite(rec, block, sizeof(block));
      SbMutexAcquire(&p->mutex);
      if (!p->seek_pending && p->audio_epoch == stretch_epoch) {
        p->frames_played += (uint64_t)(c1 + c2);  // media frames consumed
        p->clock_update_us = now_us();
        p->clock_step_us = (int64_t)kAudioGrain * 1000000 / p->sample_rate;  // real time
      }
      SbMutexRelease(&p->mutex);
      SbConditionVariableSignal(&p->cond);
      continue;
    }
    int avail = p->ring_count;
    bool draining = p->audio_eos_in && p->audio_in.empty();
    int take = 0;
    if (playing && (avail >= kAudioGrain || (draining && avail > 0))) {
      take = avail < kAudioGrain ? avail : kAudioGrain;
      for (int i = 0; i < take; i++) {
        int rd = (p->ring_read + i) % kPcmRingFrames;
        block[2 * i] = p->ring[2 * rd];
        block[2 * i + 1] = p->ring[2 * rd + 1];
      }
      p->ring_read = (p->ring_read + take) % kPcmRingFrames;
      p->ring_count -= take;
    }
    int volume = (int)(p->volume * SCE_AUDIO_VOLUME_0DB * __atomic_load_n(&g_ytv_media_volume_pct,__ATOMIC_RELAXED)/100.0);
    SbMutexRelease(&p->mutex);
    if (volume != last_volume) {
      int v[2] = {volume, volume};
      sceAudioOutSetVolume(p->port,
          (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), v);
      last_volume = volume;
    }
    if (take == 0) {
      SbConditionVariableSignal(&p->cond);
      sceKernelDelayThread(4000);
      continue;
    }
    for (int i = take; i < kAudioGrain; i++) block[2 * i] = block[2 * i + 1] = 0;
    sceAudioOutOutput(p->port, block);
      g_ytv_audio_active_us = now_us();
    if (rec >= 0) sceIoWrite(rec, block, sizeof(block));
    SbMutexAcquire(&p->mutex);
    if (!p->seek_pending) {
      p->frames_played += (uint64_t)take;
      p->clock_update_us = now_us();
      p->clock_step_us = (int64_t)take * 1000000 / p->sample_rate;
    }
    SbMutexRelease(&p->mutex);
    SbConditionVariableSignal(&p->cond);
  }
  sceAudioOutOutput(p->port, nullptr);
  if (rec >= 0) sceIoClose(rec);
  delete stretch;
  return 0;
}
void* audio_output_pthread(void* arg) {
  sceKernelChangeThreadPriority(0, kAudioThreadPriority);
  SbPlayer p = static_cast<SbPlayer>(arg);
  audio_output_thread(sizeof(p), &p);
  return nullptr;
}

// ---- worker ---------------------------------------------------------------
void* worker_main(void* arg) {
  SbPlayer p = static_cast<SbPlayer>(arg);
  // The worker holds p->mutex while it decodes. At the default priority the
  // UI thread (MainWebModule, 128) preempted it during heavy pages, and the
  // audio thread (64) then waited on the mutex: audio stutter whenever the
  // player controls / suggestions were on screen (v0.39 hardware log).
  sceKernelChangeThreadPriority(0, kWorkerThreadPriority);
  std::vector<const void*> to_free;
  while (!p->stopping) {
    bool did_work = false;
    to_free.clear();
    SbMutexAcquire(&p->mutex);

    if (p->seek_pending) {
      for (auto& s : p->video_in) to_free.push_back(s.buffer);
      for (auto& s : p->audio_in) to_free.push_back(s.buffer);
      p->video_in.clear();
      p->audio_in.clear();
      for (int i = 0; i < kFramePool; i++)
        if (i != p->displayed) p->frames[i].used = false;
      p->pending_pts.clear();
      if (p->avc_ready) {
        sceAvcdecDecodeFlush(&p->avc);
      }
      if (p->aac_ready) sceAudiodecClearContext(&p->aac);
      p->ring_read = p->ring_count = 0;
      p->frames_played = 0;
      p->audio_base_set = false;
      p->clock_update_us = 0;
      p->last_media = INT64_MIN;
      p->audio_epoch++;
      p->wall_base_pts = p->seek_to;
      p->clock_running = false;
      p->video_eos_in = p->audio_eos_in = false;
      p->video_need_pending = p->audio_need_pending = false;
      p->eos_reported = false;
      p->last_video_pts = -1;
      p->uploaded_pts = -1;
      p->seek_pending = false;
      p->state = kSbPlayerStatePrerolling;
      int ticket = p->ticket;
      SbMutexRelease(&p->mutex);
      for (auto b : to_free) free_sample(p, b);
      report_state(p, kSbPlayerStatePrerolling, ticket);
      continue;
    }
    if (p->state == kSbPlayerStateInitialized) {
      SbConditionVariableWait(&p->cond, &p->mutex);
      SbMutexRelease(&p->mutex);
      continue;
    }

    int ticket = p->ticket;
    bool ask_video = p->has_video && !p->video_eos_in && !p->video_need_pending &&
                     (int)p->video_in.size() < kVideoQueueTarget;
    bool ask_audio = p->has_audio && !p->audio_eos_in && !p->audio_need_pending &&
                     (int)p->audio_in.size() < kAudioQueueTarget &&
                     p->ring_count < p->sample_rate;  // < 1 s buffered
    if (ask_video) p->video_need_pending = true;
    if (ask_audio) p->audio_need_pending = true;

    // Video: decode one sample if a frame slot is free.
    if (p->has_video && !p->video_in.empty()) {
      Sample s = p->video_in.front();
      bool free_slot = false;
      for (auto& f : p->frames) if (frame_free(f)) free_slot = true;
      if (free_slot || p->video_broken) {
        p->video_in.pop_front();
        decode_video_sample(p, s);
        to_free.push_back(s.buffer);
        did_work = true;
      }
    }
    // Audio: decode while there is ring space.
    for (int n = 0; n < 4 && p->has_audio && !p->audio_in.empty(); n++) {
      Sample s = p->audio_in.front();
      if (!decode_audio_sample(p, s)) break;
      p->audio_in.pop_front();
      to_free.push_back(s.buffer);
      did_work = true;
    }

    // Preroll -> presenting.
    bool start_presenting = false;
    if (p->state == kSbPlayerStatePrerolling) {
      bool video_ready = !p->has_video || p->video_eos_in || p->video_broken;
      for (auto& f : p->frames) if (f.used) video_ready = true;
      bool audio_ready = !p->has_audio || p->ring_count >= p->sample_rate / 4 ||
                         (p->audio_eos_in && p->audio_in.empty());
      if (video_ready && audio_ready) {
        p->state = kSbPlayerStatePresenting;
        p->wall_start_us = now_us();
        p->clock_running = true;
        start_presenting = true;
      }
    }
    {  // playback statistics every 5 s while playing (always logged)
      static uint64_t last_stats = 0;
      static int prev_dec = 0, prev_shown = 0, prev_drop = 0, prev_calls = 0;
      static uint64_t prev_dec_us = 0;
      static uint32_t prev_serial = 0;
      if (prev_serial != p->serial) {  // counters are per player
        prev_serial = p->serial; prev_dec = prev_shown = prev_drop = prev_calls = 0; prev_dec_us = 0;
      }
      uint64_t nowt = now_us();
      if (p->has_video && p->state == kSbPlayerStatePresenting && nowt - last_stats > 5000000) {
        last_stats = nowt;
        int free_slots = 0;
        for (auto& f : p->frames) if (frame_free(f)) free_slots++;
        plog("YTV_LIFECYCLE_PLAYER_STATS media_ms=%lld rate=%.1f decoded=%d shown=%d dropped=%d "
             "frame_calls=%d backwards=%d dec_ms=%llu free=%d vin=%d ring=%d stream=%dx%d targets=%d dead=%d\n",
             (long long)(media_time_locked(p) / 1000), p->rate, p->total_frames - prev_dec,
             p->shown_frames - prev_shown, p->dropped_frames - prev_drop,
             p->frame_calls - prev_calls, p->backwards, (unsigned long long)((p->decode_us - prev_dec_us) / 1000),
             free_slots, (int)p->video_in.size(), p->ring_count, p->stream_width, p->stream_height,
             (int)g_live_targets, (int)g_dead_count);
        prev_dec = p->total_frames; prev_shown = p->shown_frames; prev_drop = p->dropped_frames;
        prev_calls = p->frame_calls; prev_dec_us = p->decode_us;
        // Only every 30 seconds: identify sustained-playback memory pressure
        // without allocating decoder buffers, changing playback or spamming disk.
        static uint64_t last_playback_memory = 0;
        if(nowt-last_playback_memory>=30000000ull){
          last_playback_memory=nowt;
          log_free_memory("playback_30s");
        }
      }
    }
    {  // diagnostic heartbeat (dropped from the log unless verbose.txt)
      static uint64_t last_status = 0;
      uint64_t nowt = now_us();
      if (nowt - last_status > 1000000) {
        last_status = nowt;
        int used = 0;
        for (auto& f : p->frames) if (f.used) used++;
        plog("YTV_PLAYER_STATUS state=%d rate=%.2f vin=%d ain=%d ring=%d frames_used=%d "
             "media=%lld played=%llu veos=%d aeos=%d vneed=%d aneed=%d\n",
             (int)p->state, p->rate, (int)p->video_in.size(), (int)p->audio_in.size(),
             p->ring_count, used, (long long)media_time_locked(p),
             (unsigned long long)p->frames_played, (int)p->video_eos_in,
             (int)p->audio_eos_in, (int)p->video_need_pending, (int)p->audio_need_pending);
      }
    }
    // End of stream.
    bool report_eos = false;
    if (p->state == kSbPlayerStatePresenting && !p->eos_reported) {
      bool video_done = !p->has_video ||
          (p->video_eos_in && p->video_in.empty() &&
           media_time_locked(p) >= p->last_video_pts);
      bool audio_done = !p->has_audio ||
          (p->audio_eos_in && p->audio_in.empty() && p->ring_count == 0);
      if (video_done && audio_done) {
        p->state = kSbPlayerStateEndOfStream;
        p->eos_reported = true;
        report_eos = true;
      }
    }
    if (!did_work && !ask_video && !ask_audio && !start_presenting && !report_eos) {
      SbConditionVariableWaitTimed(&p->cond, &p->mutex, 5000);
    }
    const bool fhd_restart=p->fhd_restart_pending;
    const bool size_restart=p->unsupported_size_restart_pending;
    p->fhd_restart_pending=false;
    p->unsupported_size_restart_pending=false;
    SbMutexRelease(&p->mutex);

    for (auto b : to_free) free_sample(p, b);
    if(fhd_restart && p->player_error){
      plog("YTV_LIFECYCLE_PLAYER_FHD notifying_capability_changed\n");
      p->player_error(p,p->context,kSbPlayerErrorCapabilityChanged,
          "1080p decoder failed: retrying a supported resolution");
    }
    if(size_restart && p->player_error){
      plog("YTV_LIFECYCLE_PLAYER_UNSUPPORTED_SIZE notifying_capability_changed\n");
      p->player_error(p,p->context,kSbPlayerErrorCapabilityChanged,
          "H.264 decoder rejected picture dimensions: reselect lower resolution");
    }
    if (start_presenting) report_state(p, kSbPlayerStatePresenting, ticket);
    if (report_eos) report_state(p, kSbPlayerStateEndOfStream, ticket);
    if (ask_video) need_data(p, kSbMediaTypeVideo, ticket);
    if (ask_audio) need_data(p, kSbMediaTypeAudio, ticket);
  }
  return nullptr;
}

// ---- GL upload (rasterizer thread) ----------------------------------------
struct UploadJob { SbPlayer player; SbDecodeTarget result; };

void upload_current(void* arg) {
  UploadJob* job = static_cast<UploadJob*>(arg);
  SbPlayer p = job->player;
  job->result = kSbDecodeTargetInvalid;
  SbMutexAcquire(&p->mutex);
  p->frame_calls++;
  int64_t t = media_time_locked(p);
  int best = -1;
  for (int i = 0; i < kFramePool; i++) {
    const Frame& f = p->frames[i];
    if (!f.used) continue;
    if (f.pts <= t || p->state == kSbPlayerStatePrerolling) {
      if (best < 0 || f.pts > p->frames[best].pts) best = i;
    }
  }
  // v0.60: video filtering is selected when a NEW decode target is created.
  // Do not mutate Skia/Cobalt-managed GL texture state during playback:
  // the previous runtime sampler toggle was followed by a hardware crash.
  if (best >= 0 && p->frames[best].pts != p->uploaded_pts) {
    Frame& f = p->frames[best];
    // The textures always cover the decoder layout (pitch x h16); the crop
    // is a content region (v0.64).
    const int target_pitch=p->pitch;
    const int target_h=p->height16;
    if (!p->cur || p->cur->pool!=p->pool ||
        p->tex_pitch!=target_pitch || p->tex_height!=target_h) {
      // New size: new textures in a new target. Images Cobalt still holds
      // keep the old one (and its texture ids) until they are released.
      SbDecodeTargetPrivate* old = p->cur;
      p->cur = new_target(p->pool,target_pitch,target_h);
      target_unref(old);
      p->tex_pitch=target_pitch;
      p->tex_height=target_h;
    }
    const uint8_t* y = f.data;
    const uint8_t* u = y + (size_t)f.pitch * f.h16;
    const uint8_t* v = u + (size_t)(f.pitch / 2) * (f.h16 / 2);
    const uint8_t* planes[3] = {y, u, v};
    // Cobalt caches GL binding state across DecodeTarget callbacks. These
    // binds are VIDEO-ONLY and must never leak into the caller's rasterizer
    // state. Previously this function ended with glBindTexture(..., 0)
    // 24-60 times/sec: after navigating videos, Cobalt's texture cache could
    // still think an UI texture was bound while VitaGL was bound to 0.
    GLint previous_texture=0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous_texture);
    // Zero copy: point each texture's GXM descriptor at the decoder plane.
    // vitaGL keeps (and later frees) its own allocation in tex->data.
    // vitaGL creates textures swizzled; decoder planes are linear rasters, so
    // the descriptor is rebuilt as linear (same format, pitch-wide rows).
    for (int i = 0; i < 3; i++) {
      glBindTexture(GL_TEXTURE_2D, p->cur->tex[i]);
      SceGxmTexture* gxm = vglGetGxmTexture(GL_TEXTURE_2D);
      if (!gxm) continue;
      SceGxmTextureFormat fmt = sceGxmTextureGetFormat(gxm);
      // Linear texture covering the decoder's full layout (pitch x h16).
      // (Strided textures with the exact picture size would be ideal, but
      // vitaGL rewrites the mip/filter control bits on every draw, which hold
      // the stride of a strided texture.)
      int w = i == 0 ? f.pitch : f.pitch / 2;
      int h = i == 0 ? f.h16 : f.h16 / 2;
      sceGxmTextureInitLinear(gxm, planes[i], fmt, (unsigned)w, (unsigned)h, 0);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
    // The previous frame leaves the screen; the GPU may read it for a while.
    if (p->displayed >= 0 && p->displayed != best) {
      p->frames[p->displayed].used = false;
      // The GPU may still have multiple queued draws referring to this
      // decoder slot. Defer reuse until 4 REAL buffer submissions occurred.
      p->frames[p->displayed].retired_at =
          __atomic_load_n(&g_swaps,__ATOMIC_ACQUIRE)+kGpuFramesInFlight;
    }
    p->displayed = best;
    p->shown_frames++;
    if (p->uploaded_pts >= 0 && f.pts < p->uploaded_pts) p->backwards++;  // shown out of order
    p->uploaded_pts = f.pts;
    SbDecodeTargetInfo& info = p->cur->info;
    memset(&info, 0, sizeof(info));
    info.format = kSbDecodeTargetFormat3PlaneYUVI420;
    info.is_opaque = true;
    info.width = f.cw;
    info.height = f.ch;
    for (int i = 0; i < 3; i++) {
      SbDecodeTargetInfoPlane& pl = info.planes[i];
      pl.texture = p->cur->tex[i];
      pl.gl_texture_target = GL_TEXTURE_2D;
      pl.gl_texture_format = GL_ALPHA;
      // Cobalt treats texture rows as bottom-up. Without a content region it
      // flips the image itself with a negative-height rect (hardware_
      // rasterizer.cc: y = H, height = -H). A region must do the same, or
      // the video shows upside down (v0.3x note, v0.64 Vita3K 4:3 Zoom test):
      // rows [cy, cy+ch) are given as top = H - cy > bottom = H - (cy+ch).
      const int div = i == 0 ? 1 : 2;
      pl.width = f.pitch / div;
      pl.height = f.h16 / div;
      pl.content_region.left = f.cx / div;
      pl.content_region.right = (f.cx + f.cw) / div;
      pl.content_region.top = (f.h16 - f.cy) / div;
      pl.content_region.bottom = (f.h16 - (f.cy + f.ch)) / div;
    }
    p->have_target = true;
    if (info.height > info.width && p->volume > 0.01)
      vertical_publish(p, p->cur, f.width, f.height, f.pitch, f.h16);
  }
  // Frames older than the one now on screen will never be shown.
  if (best >= 0) {
    for (int i = 0; i < kFramePool; i++) {
      if (i != best && i != p->displayed && p->frames[i].used &&
          p->frames[i].pts <= p->frames[best].pts) {
        p->dropped_frames++;
        g_ytv_hud_dropped_total++;
        p->frames[i].used = false;
        p->frames[i].retired_at = 0;  // never sampled by the GPU
      }
    }
  }
  if (p->have_target && p->cur) {
    __sync_add_and_fetch(&p->cur->refs, 1);  // released by SbDecodeTargetRelease
    job->result = p->cur;
  }
  SbMutexRelease(&p->mutex);
  SbConditionVariableSignal(&p->cond);
}
}  // namespace

extern "C" {

SbPlayerOutputMode SbPlayerGetPreferredOutputMode(const SbPlayerCreationParam* param) {
  (void)param;
  return kSbPlayerOutputModeDecodeToTexture;
}

SbPlayer SbPlayerCreate(SbWindow window, const SbPlayerCreationParam* param,
                        SbPlayerDeallocateSampleFunc deallocate,
                        SbPlayerDecoderStatusFunc decoder_status,
                        SbPlayerStatusFunc player_status,
                        SbPlayerErrorFunc player_error, void* context,
                        SbDecodeTargetGraphicsContextProvider* provider) {
  (void)window;
  if (!param || !decoder_status || !player_status || !deallocate) return kSbPlayerInvalid;
  plog("YTV_PLAYER_CREATE_ENTER video_codec=%d audio_codec=%d dims=%dx%d pending_targets=%d\n",
       (int)param->video_stream_info.codec, (int)param->audio_stream_info.codec,
       (int)param->video_stream_info.frame_width, (int)param->video_stream_info.frame_height,
       (int)g_dead_count);
  if (param->drm_system != kSbDrmSystemInvalid) return kSbPlayerInvalid;
  if (param->output_mode != kSbPlayerOutputModeDecodeToTexture) return kSbPlayerInvalid;
  pthread_once(&g_libs_once, init_libs);
  bool has_video = param->video_stream_info.codec == kSbMediaVideoCodecH264;
  bool has_audio = param->audio_stream_info.codec == kSbMediaAudioCodecAac;
  if (param->video_stream_info.codec != kSbMediaVideoCodecNone && !has_video) return kSbPlayerInvalid;
  if (param->audio_stream_info.codec != kSbMediaAudioCodecNone && !has_audio) return kSbPlayerInvalid;
  if (!has_video && !has_audio) return kSbPlayerInvalid;
  if ((has_video && !g_video_lib) || (has_audio && !g_audio_lib)) return kSbPlayerInvalid;

  SbPlayer p = new SbPlayerPrivate();
  p->deallocate = deallocate;
  p->decoder_status = decoder_status;
  p->player_status = player_status;
  p->player_error = player_error;
  p->context = context;
  p->provider = provider;
  p->has_video = has_video;
  p->has_audio = has_audio;
  SbMutexCreate(&p->mutex);
  SbConditionVariableCreate(&p->cond, &p->mutex);
  p->state = kSbPlayerStateInitialized;
  p->rate = 1.0;
  p->volume = 1.0;
  p->avc_mem = p->frame_mem = -1;
  p->pool = nullptr;
  p->cur = nullptr;
  // Free decoder pools of finished players now (glFinish on the GL thread),
  // so the new decoder finds their CDRAM/phycont.
  if (dead_targets_pending()) {
    // On quick 720p -> 480p switches Cobalt can call SbPlayerCreate while the
    // rasterizer's GL context runner is waiting on the same media pipeline.
    // Running a synchronous GL callback here stalls both threads and the UI.
    // Dead targets are already retired after three presented frames in
    // ytv_player_on_swap(); do not wait for the rasterizer from media thread.
    if (ytv_gl_context_is_current()) {
      flush_dead_targets(nullptr);
    } else {
      plog("YTV_PLAYER_CREATE deferred_GL_cleanup pending=%d swaps=%u\n",
           (int)g_dead_count, (unsigned)g_swaps);
    }
  }
  p->displayed = -1;
  p->port = -1;
  p->audio_thread_running = false;
  p->last_video_pts = -1;
  p->uploaded_pts = -1;
  {
    static volatile uint32_t next_serial = 0;
    p->serial = __sync_add_and_fetch(&next_serial, 1);
  }

  bool ok = true;
  if (has_video) {
    int w = param->video_stream_info.frame_width > 0 ? param->video_stream_info.frame_width : 854;
    int h = param->video_stream_info.frame_height > 0 ? param->video_stream_info.frame_height : 480;
    p->stream_width = w;
    p->stream_height = h;
    if (align16(w) > kMaxWidth || align16(h) > kMaxHeight) ok = false;
    if (ok) ok = create_video_decoder(p,(w==1190 && h==720)?1280:align32(w),align16(h));
    if (!ok) {
      // v0.64: no decoder for this size (e.g. a portrait size the hardware
      // refuses). Stop advertising it so the next attempt picks another one.
      ytv_media_reject_video_size(w, h);
      plog("YTV_LIFECYCLE_PLAYER decoder_unavailable %dx%d rejected=1\n", w, h);
    }
  }
  if (ok && has_audio) {
    ok = parse_audio_config(p, param->audio_stream_info) && create_audio_decoder(p);
    if (ok) {
      p->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, kAudioGrain, p->sample_rate,
                                    SCE_AUDIO_OUT_MODE_STEREO);
      if (p->port < 0 && p->sample_rate == 48000)
        p->port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, kAudioGrain, 48000,
                                      SCE_AUDIO_OUT_MODE_STEREO);
      ok = p->port >= 0;
    }
  }
  plog("YTV_LIFECYCLE_PLAYER_CREATE video=%d %dx%d audio=%d ch=%d rate=%d ok=%d\n", has_video,
       p->stream_width, p->stream_height, has_audio, p->channels, p->sample_rate, ok);
  if (!ok) {
    destroy_video_decoder(p);
    if (p->pool) pool_unref(p->pool);  // never displayed: no GPU reads
    else release_frame_pool(p->frame_mem, p->frame_pool, p->frame_pool_mapped);
    if (p->aac_ready) sceAudiodecDeleteDecoder(&p->aac);
    if (p->port >= 0) sceAudioOutReleasePort(p->port);
    free(p->es_buffer); free(p->pcm_buffer); free(p->ring);
    SbConditionVariableDestroy(&p->cond);
    SbMutexDestroy(&p->mutex);
    delete p;
    return kSbPlayerInvalid;
  }
  pthread_create(&p->worker, nullptr, worker_main, p);
  if (has_audio) {
    // pthread, not a raw kernel thread: it locks p->mutex (pthread-embedded
    // crashes on hardware when used from threads it did not create).
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x10000);
    p->audio_thread_running =
        pthread_create(&p->audio_thread, &attr, audio_output_pthread, p) == 0;
    pthread_attr_destroy(&attr);
  }
  report_state(p, kSbPlayerStateInitialized, SB_PLAYER_INITIAL_TICKET);
  __sync_add_and_fetch(&g_ytv_player_instances, 1);
  if (ytv_lowmem() && p->stream_height > 480 && !g_lite720_probe_open) {
    SceUID fd = sceIoOpen(kLite720Probe, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) { sceIoWrite(fd, "1", 1); sceIoClose(fd); g_lite720_probe_open = true; }
  }
  return p;
}

void SbPlayerDestroy(SbPlayer p) {
  if (!p) return;
  vertical_forget(p);
  plog("YTV_PLAYER_DESTROY_BEGIN id=%u dims=%dx%d pending=%d live=%d\n",
       p->serial, p->stream_width, p->stream_height, (int)g_dead_count,
       (int)g_live_targets);
  __sync_sub_and_fetch(&g_ytv_player_instances, 1);
  if (g_lite720_probe_open && p->stream_height > 480) {
    sceIoRemove(kLite720Probe);
    g_lite720_probe_open = false;
  }
  p->stopping = true;
  SbConditionVariableBroadcast(&p->cond);
  plog("YTV_PLAYER_DESTROY_JOIN_WORKER id=%u\n", p->serial);
  pthread_join(p->worker, nullptr);
  plog("YTV_PLAYER_DESTROY_WORKER_DONE id=%u\n", p->serial);
  if (p->audio_thread_running) {
    plog("YTV_PLAYER_DESTROY_JOIN_AUDIO id=%u\n", p->serial);
    pthread_join(p->audio_thread, nullptr);
    p->audio_thread_running = false;
    plog("YTV_PLAYER_DESTROY_AUDIO_DONE id=%u\n", p->serial);
  }
  for (auto& s : p->video_in) free_sample(p, s.buffer);
  for (auto& s : p->audio_in) free_sample(p, s.buffer);
  destroy_video_decoder(p);
  if (p->aac_ready) sceAudiodecDeleteDecoder(&p->aac);
  if (p->port >= 0) sceAudioOutReleasePort(p->port);
  // The textures sample the frame pool in place. Cobalt may still hold the
  // current target (its last video frame); it and the pool are freed only
  // after the last SbDecodeTargetRelease and the GPU frames that follow.
  target_unref(p->cur);
  p->cur = nullptr;
  pool_unref(p->pool);
  p->pool = nullptr;
  free(p->es_buffer); free(p->pcm_buffer); free(p->ring);
  SbConditionVariableDestroy(&p->cond);
  SbMutexDestroy(&p->mutex);
  plog("YTV_PLAYER_DESTROY_DONE id=%u\n", p->serial);
  delete p;
}

void SbPlayerSeek(SbPlayer p, int64_t timestamp, int ticket) {
  if (!p) return;
  SbMutexAcquire(&p->mutex);
  plog("YTV_PLAYER_SEEK to=%lld ticket=%d state=%d\n", (long long)timestamp, ticket, (int)p->state);
  p->seek_to = timestamp;
  p->ticket = ticket;
  p->seek_pending = true;
  if (p->state == kSbPlayerStateInitialized) p->state = kSbPlayerStatePrerolling;
  SbMutexRelease(&p->mutex);
  SbConditionVariableBroadcast(&p->cond);
}

void SbPlayerWriteSamples(SbPlayer p, SbMediaType type, const SbPlayerSampleInfo* infos,
                          int count) {
  if (!p || !infos || count <= 0) return;
  {
    static int logged_v = 0, logged_a = 0;
    int& n = type == kSbMediaTypeVideo ? logged_v : logged_a;
    if (n < 6 || (n % 100) == 0)
      plog("YTV_PLAYER_WRITE type=%d count=%d ts=%lld size=%d n=%d\n", (int)type, count,
           (long long)infos[0].timestamp, infos[0].buffer_size, n);
    n++;
  }
  SbMutexAcquire(&p->mutex);
  for (int i = 0; i < count; i++) {
    Sample s = {infos[i].buffer, infos[i].buffer_size, infos[i].timestamp, 0, 0};
    if (type == kSbMediaTypeVideo) {
      s.width = infos[i].video_sample_info.stream_info.frame_width;
      s.height = infos[i].video_sample_info.stream_info.frame_height;
    }
    if (type == kSbMediaTypeVideo) p->video_in.push_back(s);
    else p->audio_in.push_back(s);
  }
  if (type == kSbMediaTypeVideo) p->video_need_pending = false;
  else p->audio_need_pending = false;
  SbMutexRelease(&p->mutex);
  SbConditionVariableBroadcast(&p->cond);
}

int SbPlayerGetMaximumNumberOfSamplesPerWrite(SbPlayer p, SbMediaType type) {
  (void)p; (void)type;
  return 1;
}

void SbPlayerWriteEndOfStream(SbPlayer p, SbMediaType type) {
  if (!p) return;
  plog("YTV_PLAYER_EOS_IN type=%d\n", (int)type);
  SbMutexAcquire(&p->mutex);
  if (type == kSbMediaTypeVideo) { p->video_eos_in = true; p->video_need_pending = false; }
  else { p->audio_eos_in = true; p->audio_need_pending = false; }
  SbMutexRelease(&p->mutex);
  SbConditionVariableBroadcast(&p->cond);
}

void SbPlayerSetBounds(SbPlayer p, int z, int x, int y, int w, int h) {
  (void)p; (void)z; (void)x; (void)y; (void)w; (void)h;  // decode-to-texture
}

bool SbPlayerSetPlaybackRate(SbPlayer p, double rate) {
  if (!p) return false;
  {
    // DIAGNOSTIC: ux0:data/youtubevita/force_rate.txt ("1.5") replaces normal
    // speed, to test the time stretcher without YouTube's speed menu.
    static double forced = -1.0;
    if (forced < 0.0) {
      forced = 0.0;
      FILE* f = fopen("ux0:data/youtubevita/force_rate.txt", "r");
      if (f) {
        double v = 0.0;
        if (fscanf(f, "%lf", &v) == 1 && v >= 0.25 && v <= 2.0) forced = v;
        fclose(f);
      }
    }
    if (forced > 0.0 && rate == 1.0) rate = forced;
  }
  plog("YTV_PLAYER_RATE %.2f%s", rate, "\n");
  // 0 = pause; 0.25x .. 2x through the WSOLA stretcher (YouTube's speed menu).
  if (rate != 0.0 && (rate < 0.25 || rate > 2.0)) return false;
  SbMutexAcquire(&p->mutex);
  if (!p->has_audio && p->clock_running) {
    // Re-base the system clock so pausing does not jump.
    int64_t t = media_time_locked(p);
    p->wall_base_pts = t;
    p->wall_start_us = now_us();
  }
  p->rate = rate;
  SbMutexRelease(&p->mutex);
  SbConditionVariableBroadcast(&p->cond);
  return true;
}

void SbPlayerSetVolume(SbPlayer p, double volume) {
  if (!p) return;
  SbMutexAcquire(&p->mutex);
  p->volume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
  plog("YTV_PLAYER_VOLUME %.2f%s", p->volume, "\n");
  SbMutexRelease(&p->mutex);
}

void SbPlayerGetInfo(SbPlayer p, SbPlayerInfo* out) {
  if (!p || !out) return;
  SbMutexAcquire(&p->mutex);
  memset(out, 0, sizeof(*out));
  out->current_media_timestamp = media_time_locked(p);
  out->duration = SB_PLAYER_NO_DURATION;
  const int aspect=__atomic_load_n(&g_ytv_video_aspect_mode,__ATOMIC_RELAXED);
  out->frame_width = (p->has_video && aspect!=0)?960:p->stream_width;
  out->frame_height = (p->has_video && aspect!=0)?544:p->stream_height;
  out->is_paused = p->rate == 0.0;
  out->volume = p->volume;
  out->total_video_frames = p->total_frames;
  out->dropped_video_frames = p->dropped_frames;
  out->playback_rate = p->rate;
  SbMutexRelease(&p->mutex);
}

SbDecodeTarget SbPlayerGetCurrentFrame(SbPlayer p) {
  if (!p || !p->has_video) return kSbDecodeTargetInvalid;
  UploadJob job = {p, kSbDecodeTargetInvalid};
  if (ytv_gl_context_is_current()) {
    upload_current(&job);
  } else if (p->provider && p->provider->gles_context_runner) {
    p->provider->gles_context_runner(p->provider, upload_current, &job);
  }
  return job.result;
}

bool SbPlayerGetAudioConfiguration(SbPlayer p, int index, SbMediaAudioConfiguration* out) {
  if (!p || index != 0 || !out || !p->has_audio) return false;
  memset(out, 0, sizeof(*out));
  out->connector = kSbMediaAudioConnectorUnknown;
  out->latency = 0;
  out->coding_type = kSbMediaAudioCodingTypePcm;
  out->number_of_channels = 2;
  return true;
}

// Called by Cobalt (GL thread) when an image made from the target is gone.
void SbDecodeTargetRelease(SbDecodeTarget target) {
  if (target) target_unref(target);
}

// GL thread, after every presented frame (port_graphics.cpp).
void ytv_player_on_swap() {
  __sync_add_and_fetch(&g_swaps, 1);
  if (dead_targets_pending()) free_dead_targets(false);
}
bool SbDecodeTargetGetInfo(SbDecodeTarget target, SbDecodeTargetInfo* out) {
  if (!target || !out) return false;
  *out = target->info;
  return true;
}
}  // extern "C"
