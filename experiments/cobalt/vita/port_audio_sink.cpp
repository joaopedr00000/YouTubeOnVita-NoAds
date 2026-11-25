/*
 * Starboard 15 audio sink on the standard PS Vita audio path:
 * one SceAudioOut MAIN port per sink, 48 kHz stereo S16, fed by a dedicated
 * high-priority kernel thread.
 *
 * Fixes for distorted ("estourado") sound seen on hardware:
 *  - the output never stalls: when Cobalt has not provided enough frames the
 *    port receives a block of silence instead of nothing, so the hardware
 *    clock keeps running and partial/irregular blocks do not crackle;
 *  - the feeder thread runs at high priority (it was a default-priority
 *    pthread competing with the browser's busy threads);
 *  - port volume is set explicitly to 0 dB;
 *  - float samples are clamped before S16 conversion;
 *  - the minimum buffer reported to Cobalt is 8192 frames (~170 ms), 16
 *    hardware blocks, so the renderer keeps enough data queued.
 */
#include "starboard/audio_sink.h"
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <cstdio>
#include "starboard/log.h"
#include <pthread.h>
#include <cstdlib>
#include <cstring>
#include <cstdint>

struct SbAudioSinkPrivate {
  int port;
  int channels;
  int frames_capacity;
  int boot_mute_remaining;  // first TV WebAudio jingle only, not custom splash or SbPlayer
  SbMediaAudioSampleType sample_type;
  SbMediaAudioFrameStorageType storage_type;
  SbAudioSinkFrameBuffers source;
  SbAudioSinkUpdateSourceStatusFunc status;
  SbAudioSinkConsumeFramesFunc consume;
  void* context;
  pthread_t thread;  // pthread: Cobalt callbacks run here and take SbMutex
  volatile int stopping;
  uint32_t magic;
};

namespace {
constexpr int kHz = 48000;
constexpr int kBlock = 512;           // frames per sceAudioOutOutput call
constexpr int kMinBuffer = 16 * kBlock;  // ~170 ms: absorbs CPU stalls on hardware
constexpr int kMaxSinks = 4;
constexpr uint32_t kMagic = 0x59544153u;  // "YTAS"
constexpr int kAudioThreadPriority = 64;  // high (lower value = higher)
constexpr int kTvBootMuteFrames = 3 * kHz;  // at most 3 seconds of native TV intro
constexpr uint64_t kTvBootWindowUs = 90ull * 1000000ull;

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// Diagnostic (verbose.txt only): first ~12 s of PCM written to the port,
// raw S16LE stereo 48 kHz, to verify level, rate and channel layout.
SceUID g_dump = -1;
int g_dump_blocks = 0;
bool g_dump_checked = false;
void dump_block(const int16_t* pcm, int index) {
  if (!g_dump_checked) {
    g_dump_checked = true;
    SceIoStat st;
    if (sceIoGetstat("ux0:data/youtubevita-noads/verbose.txt", &st) >= 0) {
      char path[96];
      snprintf(path, sizeof(path), "ux0:data/youtubevita-noads/audio_dump_%d.raw", index);
      g_dump = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    }
  }
  if (g_dump >= 0 && g_dump_blocks < 1125) {  // 1125 * 512 frames = 12 s
    sceIoWrite(g_dump, pcm, kBlock * 4);
    if (++g_dump_blocks == 1125) { sceIoClose(g_dump); g_dump = -1; }
  }
}
SbAudioSink g_sinks[kMaxSinks] = {};
bool g_first_tv_sink_seen = false;
extern "C" bool ytv_native_player_present();

inline int16_t to_s16(float x) {
  if (x > 1.0f) x = 1.0f;
  if (x < -1.0f) x = -1.0f;
  return static_cast<int16_t>(x * 32767.0f);
}

inline int16_t fetch(SbAudioSink s, int index, int channel) {
  int ch = s->channels == 1 ? 0 : channel;
  int idx = index % s->frames_capacity;
  if (s->storage_type == kSbMediaAudioFrameStorageTypeInterleaved) {
    if (s->sample_type == kSbMediaAudioSampleTypeFloat32)
      return to_s16(static_cast<const float*>(s->source[0])[idx * s->channels + ch]);
    return static_cast<const int16_t*>(s->source[0])[idx * s->channels + ch];
  }
  if (s->sample_type == kSbMediaAudioSampleTypeFloat32)
    return to_s16(static_cast<const float*>(s->source[ch])[idx]);
  return static_cast<const int16_t*>(s->source[ch])[idx];
}

int audio_thread(SceSize, void* argp) {
  SbAudioSink s = *static_cast<SbAudioSink*>(argp);
  alignas(64) int16_t pcm[kBlock * 2];  // on this thread's own stack
  // Pre-buffer: after starting or after running dry, wait until ~85 ms are
  // queued before playing again. Alternating short blocks with silence
  // (the old behaviour) cut the waveform repeatedly and sounded harsh.
  const int kResume = kBlock * 8 < s->frames_capacity / 2 ? kBlock * 8 : s->frames_capacity / 2;
  bool buffering = true;
  while (!s->stopping) {
    int frames = 0, offset = 0;
    bool playing = false, eos = false;
    s->status(&frames, &offset, &playing, &eos, s->context);
    if (s->stopping) break;
    if (!playing) {
      // Paused or not started: the port can idle without output.
      buffering = true;
      sceKernelDelayThread(4000);
      continue;
    }
    if (frames > s->frames_capacity) frames = s->frames_capacity;
    if (offset < 0 || offset >= s->frames_capacity) offset = 0;
    if (buffering && !eos && frames < kResume) {
      sceKernelDelayThread(2000);  // keep filling; nothing queued to the port
      continue;
    }
    buffering = false;
    int take = 0;
    if (frames >= kBlock) take = kBlock;
    else if (eos && frames > 0) take = frames;  // final partial block
    else if (!eos) {
      buffering = true;  // ran dry: wait for a cushion instead of crackling
      continue;
    }
    memset(pcm, 0, sizeof(pcm));  // silence fills underruns / tail
    for (int i = 0; i < take; i++) {
      pcm[2 * i] = fetch(s, offset + i, 0);
      pcm[2 * i + 1] = fetch(s, offset + i, 1);
    }
    // The custom splash jingle plays separately on a VOICE port. YouTube's
    // own first WebAudio sink sometimes plays another startup chime AFTER the
    // profile chooser. Silence only the first 3 seconds of that sink; consume
    // the frames normally so its clock and buffer ownership remain correct.
    // Subsequent WebAudio sinks, native Add-ons sounds and SbPlayer audio are
    // unaffected. This is intentionally bounded, not a global audio mute.
    if (take > 0 && s->boot_mute_remaining > 0) {
      int silent = take < s->boot_mute_remaining ? take : s->boot_mute_remaining;
      memset(pcm, 0, (size_t)silent * 2 * sizeof(int16_t));
      s->boot_mute_remaining -= silent;
    }
    // Blocks until the previous block has been played: this is the pacing.
    if (take > 0) dump_block(pcm, s->port);
    if (sceAudioOutOutput(s->port, pcm) < 0) break;
    if (take > 0) s->consume(take, s->context);
  }
  sceAudioOutOutput(s->port, nullptr);  // drain
  return 0;
}
// The sink callbacks are Cobalt code that locks SbMutex/condition variables,
// which park on a per-thread semaphore found through pthread TLS: run them on
// a pthread (raw kernel threads crash inside pthread-embedded on hardware).
void* audio_pthread(void* arg) {
  sceKernelChangeThreadPriority(0, kAudioThreadPriority);
  SbAudioSink s = static_cast<SbAudioSink>(arg);
  audio_thread(sizeof(s), &s);
  return nullptr;
}

bool register_sink(SbAudioSink s) {
  for (auto& slot : g_sinks)
    if (!slot) { slot = s; return true; }
  return false;
}
void unregister_sink(SbAudioSink s) {
  for (auto& slot : g_sinks)
    if (slot == s) slot = nullptr;
}
}  // namespace

extern "C" {
bool SbAudioSinkIsValid(SbAudioSink sink) {
  if (!sink) return false;
  pthread_mutex_lock(&g_lock);
  bool valid = false;
  for (auto slot : g_sinks)
    if (slot == sink) valid = true;
  pthread_mutex_unlock(&g_lock);
  return valid;
}
int SbAudioSinkGetMaxChannels() { return 2; }
int SbAudioSinkGetNearestSupportedSampleFrequency(int hz) {
  return hz > 0 ? kHz : -1;
}
bool SbAudioSinkIsAudioSampleTypeSupported(SbMediaAudioSampleType type) {
  return type == kSbMediaAudioSampleTypeFloat32 ||
         type == kSbMediaAudioSampleTypeInt16Deprecated;
}
bool SbAudioSinkIsAudioFrameStorageTypeSupported(
    SbMediaAudioFrameStorageType storage) {
  return storage == kSbMediaAudioFrameStorageTypeInterleaved ||
         storage == kSbMediaAudioFrameStorageTypePlanar;
}
int SbAudioSinkGetMinBufferSizeInFrames(int channels,
                                        SbMediaAudioSampleType type, int hz) {
  if (channels < 1 || channels > 2 || hz != kHz ||
      !SbAudioSinkIsAudioSampleTypeSupported(type))
    return -1;
  return kMinBuffer;
}
SbAudioSink SbAudioSinkCreate(int channels, int hz, SbMediaAudioSampleType type,
                              SbMediaAudioFrameStorageType storage,
                              SbAudioSinkFrameBuffers source, int capacity,
                              SbAudioSinkUpdateSourceStatusFunc status,
                              SbAudioSinkConsumeFramesFunc consume,
                              void* context) {
  if (channels < 1 || channels > 2 || hz != kHz ||
      !SbAudioSinkIsAudioSampleTypeSupported(type) ||
      !SbAudioSinkIsAudioFrameStorageTypeSupported(storage) ||
      capacity < kBlock || !source || !source[0] ||
      (storage == kSbMediaAudioFrameStorageTypePlanar && channels == 2 &&
       !source[1]) ||
      !status || !consume)
    return kSbAudioSinkInvalid;
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, kBlock, kHz,
                                 SCE_AUDIO_OUT_MODE_STEREO);
  {
    char line[160];
    snprintf(line, sizeof(line),
             "YTV_AUDIO_SINK_CREATE channels=%d hz=%d type=%d storage=%d capacity=%d port=%d\n",
             channels, hz, (int)type, (int)storage, capacity, port);
    SbLogRaw(line);
  }
  if (port < 0) return kSbAudioSinkInvalid;
  int volume[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
  sceAudioOutSetVolume(port,
      (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
      volume);
  SbAudioSink sink = static_cast<SbAudioSink>(calloc(1, sizeof(*sink)));
  if (!sink) { sceAudioOutReleasePort(port); return kSbAudioSinkInvalid; }
  sink->port = port; sink->channels = channels; sink->frames_capacity = capacity;
  sink->sample_type = type; sink->storage_type = storage; sink->source = source;
  sink->status = status; sink->consume = consume; sink->context = context;
  sink->magic = kMagic;
  pthread_mutex_lock(&g_lock);
  bool registered = register_sink(sink);
  // Suppress exactly one early Cobalt WebAudio startup sound. The custom
  // youtube_intro_audio.pcm is a separate native VOICE port and stays audible.
  bool mute_tv_boot = false;
  if (registered && !g_first_tv_sink_seen &&
      sceKernelGetProcessTimeWide() < kTvBootWindowUs &&
      !ytv_native_player_present()) {
    g_first_tv_sink_seen = true;
    sink->boot_mute_remaining = kTvBootMuteFrames;
    mute_tv_boot = true;
  }
  pthread_mutex_unlock(&g_lock);
  if (!registered) {
    free(sink); sceAudioOutReleasePort(port); return kSbAudioSinkInvalid;
  }
  if (mute_tv_boot)
    SbLogRaw("YTV_LIFECYCLE_TV_BOOT_WEBAUDIO_MUTED first_3s=1 custom_splash_unchanged=1\n");
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 0x10000);
  int created = pthread_create(&sink->thread, &attr, audio_pthread, sink);
  pthread_attr_destroy(&attr);
  if (created != 0) {
    pthread_mutex_lock(&g_lock); unregister_sink(sink); pthread_mutex_unlock(&g_lock);
    free(sink); sceAudioOutReleasePort(port); return kSbAudioSinkInvalid;
  }
  return sink;
}
void SbAudioSinkDestroy(SbAudioSink sink) {
  if (!SbAudioSinkIsValid(sink)) return;
  sink->stopping = 1;
  pthread_join(sink->thread, nullptr);  // no callbacks after return
  sceAudioOutReleasePort(sink->port);
  pthread_mutex_lock(&g_lock);
  unregister_sink(sink);
  pthread_mutex_unlock(&g_lock);
  free(sink);
}
}  // extern "C"
