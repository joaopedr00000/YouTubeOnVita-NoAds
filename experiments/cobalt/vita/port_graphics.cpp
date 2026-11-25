/*
 * VitaGL-backed Starboard 15 EGL/GLES function tables.
 * Every EGL slot and every GLES 2.0 core slot is populated with either the
 * real VitaGL function, a real implementation layered on VitaGL, or a
 * function that fails with a proper EGL error. GLES 3.0-only slots remain
 * null because the context is ES 2.0.
 */
#include <vitaGL.h>
#include "starboard/egl.h"
#include "starboard/log.h"
#include "starboard/gles.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <pthread.h>
#include <cstdint>
#include <type_traits>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Vita EGL 1.4 layer on top of VitaGL.
//
// VitaGL's own EGL entry points are a thin facade: eglInitialize() never
// brings up GXM, there is no pbuffer support and the "current" context is
// global instead of per thread. Cobalt's renderer (GraphicsContextEGL) needs:
//   * a working GL implementation after eglInitialize()
//   * eglCreatePbufferSurface() for its "null surface"
//   * eglMakeCurrent() with real per-thread semantics (rasterizer thread vs
//     main thread)
// Everything here either performs the real operation or fails with a proper
// EGL error code. No slot in the Starboard table is left null: a null slot
// makes Cobalt jump to address 0 (the cause of the Start hang investigated
// on 2026-11-17).
// ---------------------------------------------------------------------------

// VitaGL's public header omits this exported EGL function declaration.
extern "C" EGLSurface eglGetCurrentSurface(EGLint readdraw);

#ifndef EGL_BAD_ACCESS
#define EGL_BAD_ACCESS 0x3002
#endif
#ifndef EGL_BAD_ALLOC
#define EGL_BAD_ALLOC 0x3003
#endif
#ifndef EGL_BAD_DISPLAY
#define EGL_BAD_DISPLAY 0x3008
#endif
#ifndef EGL_BAD_MATCH
#define EGL_BAD_MATCH 0x3009
#endif
#ifndef EGL_DRAW
#define EGL_DRAW 0x3059
#endif
#ifndef EGL_READ
#define EGL_READ 0x305A
#endif
#ifndef EGL_VERSION
#define EGL_VERSION 0x3054
#endif
#ifndef GL_LOW_FLOAT
#define GL_LOW_FLOAT 0x8DF0
#define GL_MEDIUM_FLOAT 0x8DF1
#define GL_HIGH_FLOAT 0x8DF2
#define GL_LOW_INT 0x8DF3
#define GL_MEDIUM_INT 0x8DF4
#define GL_HIGH_INT 0x8DF5
#endif
#ifndef GL_RENDERBUFFER_WIDTH
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8D44
#endif
#ifndef GL_DITHER
#define GL_DITHER 0x0BD0
#endif
#ifndef GL_TEXTURE_BINDING_CUBE_MAP
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#endif

extern "C" volatile uint64_t g_ytv_last_input_us;  // port_input.cpp
namespace {

void log_line(const char* text) { SbLogRaw(text); }
// Frame profiler counters (see eglSwapBuffers).
volatile uint64_t g_prof_draw_us = 0;
volatile uint32_t g_prof_draws = 0;
volatile uint64_t g_prof_upload_us = 0;
volatile uint64_t g_prof_frame_first = 0;  // first draw of the current frame
// Heartbeat counters (always collected; summarised every heartbeat).
volatile uint32_t g_hb_frames = 0, g_hb_draws = 0, g_hb_compiles = 0, g_hb_links = 0;
volatile uint64_t g_hb_swap_us = 0, g_hb_raster_us = 0, g_hb_compile_us = 0, g_hb_link_us = 0;
// Frame pacing: intervals between presented frames while animating (gaps over
// 250 ms are idle screen time, not jank).
volatile uint64_t g_hb_prev_swap = 0, g_hb_max_interval_us = 0;
volatile uint32_t g_hb_jank20 = 0, g_hb_jank40 = 0;
volatile uint64_t g_prof_raster_us = 0;    // first draw -> swap, summed
volatile uint64_t g_prof_idle_us = 0;      // previous swap end -> first draw
volatile uint64_t g_prof_last_swap_end = 0;
// Startup-only transition: replace Cobalt's empty/black bootstrap frames with
// #212121. Stop permanently as soon as Cobalt paints a real page; never touch
// video frames, the player's YUV planes, or normal UI frames.
uint32_t g_startup_draws = 0;
bool g_startup_bg_active = true;



// ---- VitaGL / GXM bring-up ------------------------------------------------
pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_vgl_ready = false;
GLboolean g_vgl_fallback = GL_FALSE;

// The VitaGL boot splash is removed at library level: see
// third_party/vitagl-nosplash (VitaGL ed51b21 built with NO_SPLASHSCREEN=1).
// First launch: copy the GXP shaders shipped in app0:/shader_cache (built by
// the same SceShaccCg compiler, keyed by source hash) into VitaGL's cache so
// even the first start skips runtime shader compilation. Existing files win.
void seed_shader_cache() {
  char app_id[16] = {0};
  if (sceAppMgrAppParamGetString(0, 12, app_id, sizeof(app_id)) < 0)
    snprintf(app_id, sizeof(app_id), "YTVCB0001");
  char dst_dir[96];
  snprintf(dst_dir, sizeof(dst_dir), "ux0:data/shader_cache/%s", app_id);
  sceIoMkdir(dst_dir, 0777);
  SceUID d = sceIoDopen("app0:shader_cache");
  if (d < 0) return;
  SceIoDirent e;
  int copied = 0;
  static uint8_t buf[64 * 1024];
  while (sceIoDread(d, &e) > 0) {
    if (SCE_S_ISDIR(e.d_stat.st_mode)) continue;
    char src[160], dst[160];
    snprintf(src, sizeof(src), "app0:shader_cache/%s", e.d_name);
    snprintf(dst, sizeof(dst), "%s/%s", dst_dir, e.d_name);
    SceIoStat st;
    if (sceIoGetstat(dst, &st) >= 0) continue;
    int in = sceIoOpen(src, SCE_O_RDONLY, 0);
    if (in < 0) continue;
    int out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out >= 0) {
      int n;
      while ((n = sceIoRead(in, buf, sizeof(buf))) > 0) sceIoWrite(out, buf, n);
      sceIoClose(out);
      copied++;
    }
    sceIoClose(in);
  }
  sceIoDclose(d);
  char line[96];
  snprintf(line, sizeof(line), "YTV_VGL_SHADER_CACHE_SEEDED %d\n", copied);
  log_line(line);
}

}  // namespace
extern "C" int ytv_addons_cfg_fhd();  // port_addons.cpp (Full HD option)
extern "C" bool ytv_lowmem();         // port_system.cpp (YouTube Lite)
extern "C" { extern volatile size_t g_vgl_free_vram, g_vgl_free_ram, g_vgl_free_phy; }
namespace {
bool ensure_vitagl() {
  pthread_mutex_lock(&g_init_mutex);
  if (!g_vgl_ready) {
    log_line("YTV_VGL_INIT_ENTER\n");
    // vglInitExtended() would hand VitaGL *all* free user RAM minus the
    // threshold. Cobalt still needs SbMemoryMap blocks (V8, caches) after
    // this point, so cap VitaGL's main-RAM pool at 48 MiB and leave the
    // remainder to Starboard. VRAM (CDRAM) is given to VitaGL entirely.
    // VitaGL (HAVE_SHADER_CACHE) only creates ux0:data/shader_cache/<TITLEID>.
    sceIoMkdir("ux0:data/shader_cache", 0777);
    seed_shader_cache();
    SceKernelFreeMemorySizeInfo info = {};
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    // YouTube Lite: 24 MiB. 20 MiB (v0.46-v0.48) left only ~1.3 MiB of the
    // pool free for a whole hardware session.
    const int kRamPool = (ytv_lowmem() ? 24 : 48) * 1024 * 1024;
    int user_free = info.size_user;
    int ram_threshold = user_free > kRamPool ? user_free - kRamPool : 0;
    // Keep physical memory for the H.264 decoder. After ~10 minutes of
    // playback the v0.59 Full hardware log showed only 7 MiB of free CDRAM
    // while VitaGL still had ~40 MiB unused in its own pool; rebalance 8 MiB
    // towards the decoder/next-video allocations. Do not reduce the already
    // validated 40 MiB reserve used by the Full HD option.
    const int cdram_reserve = (ytv_addons_cfg_fhd() && !ytv_lowmem() ? 40 : 28) * 1024 * 1024;
    // VitaGL's per-frame circular vertex pool defaults to 32 MiB taken from
    // its RAM pool, which left that pool ~6.5 MiB free for textures (v0.42
    // log). 12 MiB (4 MiB per frame) is ample for Cobalt; overflows fall back
    // to a normal allocation.
    vglSetCircularPoolSize((ytv_lowmem() ? 8 : 12) * 1024 * 1024);
    g_vgl_fallback = vglInitWithCustomThreshold(
        0x80000, 960, 544, ram_threshold, cdram_reserve, 16 * 1024 * 1024,
        0x7fffffff /* no common-dialog pool, same as vglInitExtended */,
        SCE_GXM_MULTISAMPLE_NONE);
    // v0.64: GL_DYNAMIC_DRAW / GL_STREAM_DRAW buffers must own real memory.
    // VitaGL's default ("scratch") places their contents in the per-frame
    // circular pool, which is handed to another frame 3 swaps later. Any
    // buffer kept across frames (or updated later with glBufferSubData)
    // then points at another frame's vertex data: stretched triangles over
    // thumbnails and text that stay until restart (hardware, lyrics mode
    // with heavy scrolling, logs-psv\v064-artefatos). Without scratch every
    // upload is a normal allocation, freed by the GC 4 frames after its last
    // use, the same path VitaGL uses for static buffers.
    vglSetupScratchMemory(GL_FALSE, GL_FALSE);
    g_vgl_ready = true;
    SceKernelFreeMemorySizeInfo after = {};
    after.size = sizeof(after);
    sceKernelGetFreeMemorySize(&after);
    char line[220];
    snprintf(line, sizeof(line),
             "YTV_VGL_INIT_RETURN fallback=%d user_free_before=%d "
             "user_free_after=%d cdram_before=%d cdram_after=%d\n",
             (int)g_vgl_fallback, info.size_user, after.size_user,
             info.size_cdram, after.size_cdram);
    log_line(line);
    // Until Cobalt presents its first frame (the YouTube splash), show the
    // splash's own grey (#212121, same tone as the LiveArea startup image)
    // instead of black display buffers.
    ::glClearColor(0x21 / 255.f, 0x21 / 255.f, 0x21 / 255.f, 1.f);
    for (int i = 0; i < 2; i++) {
      ::glClear(GL_COLOR_BUFFER_BIT);
      vglSwapBuffers(GL_FALSE);
    }
    ::glClearColor(0.f, 0.f, 0.f, 0.f);
  }
  pthread_mutex_unlock(&g_init_mutex);
  return true;
}

// ---- EGL error state (per thread, as the spec requires) -------------------
thread_local EGLint t_error = EGL_SUCCESS;
EGLBoolean fail(EGLint error) { t_error = error; return EGL_FALSE; }
void ok() { t_error = EGL_SUCCESS; }

// ---- Handles ---------------------------------------------------------------
// VitaGL hands out one display/context/window surface. We learn their values
// from VitaGL itself instead of hard-coding its private enum.
EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_window_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
bool g_initialized = false;

constexpr uintptr_t kPbufferBase = 0x59500000u;
constexpr int kMaxPbuffers = 32;
struct Pbuffer {
  bool used;
  EGLint width;
  EGLint height;
};
Pbuffer g_pbuffers[kMaxPbuffers] = {};
pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;

int pbuffer_index(EGLSurface surface) {
  uintptr_t v = reinterpret_cast<uintptr_t>(surface);
  if (v < kPbufferBase || v >= kPbufferBase + kMaxPbuffers) return -1;
  int index = static_cast<int>(v - kPbufferBase);
  return g_pbuffers[index].used ? index : -1;
}
bool is_window_surface(EGLSurface s) {
  return s != EGL_NO_SURFACE && s == g_window_surface;
}
bool is_valid_surface(EGLSurface s) {
  return is_window_surface(s) || pbuffer_index(s) >= 0;
}
bool is_valid_display(EGLDisplay d) {
  return d != EGL_NO_DISPLAY && d == g_display;
}

// Current-binding state. EGL allows a context to be current on one thread
// only; VitaGL has a single GXM context, so this is enforced for real.
pthread_t g_context_owner = {};
bool g_context_bound = false;
thread_local EGLDisplay t_display = EGL_NO_DISPLAY;
thread_local EGLContext t_context = EGL_NO_CONTEXT;
thread_local EGLSurface t_draw = EGL_NO_SURFACE;
thread_local EGLSurface t_read = EGL_NO_SURFACE;

volatile int g_swap_count = 0;
volatile int g_make_current_count = 0;

// ---- EGL entry points ------------------------------------------------------
SbEglDisplay get_display(SbEglNativeDisplayType display) {
  log_line("YTV_EGL_GET_DISPLAY_ENTER\n");
  EGLDisplay result = ::eglGetDisplay(reinterpret_cast<NativeDisplayType>(display));
  if (result != EGL_NO_DISPLAY) {
    pthread_mutex_lock(&g_state_mutex);
    g_display = result;
    pthread_mutex_unlock(&g_state_mutex);
    ok();
  } else {
    t_error = EGL_BAD_PARAMETER;
  }
  char line[140];
  snprintf(line, sizeof(line), "YTV_EGL_GET_DISPLAY_RETURN %p\n", (void*)result);
  log_line(line);
  return result;
}

EGLBoolean ytv_egl_initialize(EGLDisplay display, EGLint* major, EGLint* minor) {
  log_line("YTV_EGL_INITIALIZE_ENTER\n");
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  ensure_vitagl();
  // VitaGL reports "2.2", but the implemented surface/context/config API is
  // EGL 1.4 (no EGLSync/EGLImage core entry points). Report what is real.
  if (major) *major = 1;
  if (minor) *minor = 4;
  g_initialized = true;
  ok();
  log_line("YTV_EGL_INITIALIZE_RETURN success=1 version=1.4\n");
  return EGL_TRUE;
}

EGLBoolean ytv_egl_terminate(EGLDisplay display) {
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  // GXM stays alive for the process lifetime (vglEnd would invalidate every
  // GL object Cobalt may still reference during shutdown).
  g_initialized = false;
  ok();
  return EGL_TRUE;
}

EGLint ytv_egl_get_error() {
  EGLint e = t_error;
  t_error = EGL_SUCCESS;
  return e;
}

const char* ytv_egl_query_string(EGLDisplay display, EGLint name) {
  if (name == EGL_VERSION) { ok(); return "1.4 VitaGL"; }
  const char* r = ::eglQueryString(display, name);
  if (r) ok(); else t_error = EGL_BAD_PARAMETER;
  return r;
}

EGLContext ytv_egl_create_context(EGLDisplay display, EGLConfig config,
                                  EGLContext share, const EGLint* attrs) {
  log_line("YTV_EGL_CREATE_CONTEXT_ENTER\n");
  if (!is_valid_display(display) || !g_initialized) {
    t_error = EGL_BAD_DISPLAY;
    return EGL_NO_CONTEXT;
  }
  pthread_mutex_lock(&g_state_mutex);
  // VitaGL has exactly one GXM context. A second simultaneous context
  // (e.g. a share-group resource context) cannot be provided.
  if (g_context != EGL_NO_CONTEXT || share != EGL_NO_CONTEXT) {
    pthread_mutex_unlock(&g_state_mutex);
    log_line("YTV_EGL_CREATE_CONTEXT_REFUSED second context\n");
    t_error = EGL_BAD_ALLOC;
    return EGL_NO_CONTEXT;
  }
  EGLContext context = ::eglCreateContext(display, config, share, attrs);
  g_context = context;
  pthread_mutex_unlock(&g_state_mutex);
  ok();
  char line[140];
  snprintf(line, sizeof(line), "YTV_EGL_CREATE_CONTEXT_RETURN %p\n", (void*)context);
  log_line(line);
  return context;
}

EGLBoolean ytv_egl_destroy_context(EGLDisplay display, EGLContext context) {
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  pthread_mutex_lock(&g_state_mutex);
  if (context == EGL_NO_CONTEXT || context != g_context) {
    pthread_mutex_unlock(&g_state_mutex);
    return fail(EGL_BAD_CONTEXT);
  }
  // Per spec destruction is deferred while current; we only forget the
  // handle when no thread holds it.
  if (!g_context_bound) g_context = EGL_NO_CONTEXT;
  pthread_mutex_unlock(&g_state_mutex);
  ok();
  return EGL_TRUE;
}

SbEglSurface create_window(SbEglDisplay display, SbEglConfig config,
                           SbEglNativeWindowType window,
                           const SbEglInt32* attributes) {
  log_line("YTV_EGL_CREATE_SURFACE_ENTER\n");
  if (!is_valid_display(display) || !g_initialized) {
    t_error = EGL_BAD_DISPLAY;
    return EGL_NO_SURFACE;
  }
  if (!window) { t_error = EGL_BAD_PARAMETER; return EGL_NO_SURFACE; }
  pthread_mutex_lock(&g_state_mutex);
  if (g_window_surface != EGL_NO_SURFACE) {
    pthread_mutex_unlock(&g_state_mutex);
    t_error = EGL_BAD_ALLOC;  // only one native display surface exists
    return EGL_NO_SURFACE;
  }
  EGLSurface result = ::eglCreateWindowSurface(display, config,
      reinterpret_cast<void*>(window), attributes);
  g_window_surface = result;
  pthread_mutex_unlock(&g_state_mutex);
  ok();
  char line[140];
  snprintf(line, sizeof(line), "YTV_EGL_CREATE_SURFACE_RETURN %p\n", (void*)result);
  log_line(line);
  return result;
}

EGLSurface ytv_egl_create_pbuffer(EGLDisplay display, EGLConfig config,
                                  const EGLint* attrs) {
  (void)config;
  if (!is_valid_display(display) || !g_initialized) {
    t_error = EGL_BAD_DISPLAY;
    return EGL_NO_SURFACE;
  }
  EGLint width = 0, height = 0;
  for (const EGLint* a = attrs; a && a[0] != EGL_NONE; a += 2) {
    switch (a[0]) {
      case EGL_WIDTH: width = a[1]; break;
      case EGL_HEIGHT: height = a[1]; break;
      case EGL_LARGEST_PBUFFER: break;
      case EGL_TEXTURE_FORMAT:
      case 0x3081: /* EGL_TEXTURE_TARGET */
        // Texture-bindable pbuffers would need eglBindTexImage, which
        // VitaGL cannot provide (no pbuffer colour storage).
        if (a[1] != 0x305C /* EGL_NO_TEXTURE */) {
          t_error = EGL_BAD_MATCH;
          return EGL_NO_SURFACE;
        }
        break;
      default: break;
    }
  }
  if (width < 0 || height < 0 || width > 4096 || height > 4096) {
    t_error = EGL_BAD_PARAMETER;
    return EGL_NO_SURFACE;
  }
  pthread_mutex_lock(&g_state_mutex);
  int slot = -1;
  for (int i = 0; i < kMaxPbuffers; i++)
    if (!g_pbuffers[i].used) { slot = i; break; }
  if (slot < 0) {
    pthread_mutex_unlock(&g_state_mutex);
    t_error = EGL_BAD_ALLOC;
    return EGL_NO_SURFACE;
  }
  g_pbuffers[slot] = {true, width, height};
  pthread_mutex_unlock(&g_state_mutex);
  ok();
  char line[140];
  snprintf(line, sizeof(line), "YTV_EGL_CREATE_PBUFFER %dx%d slot=%d\n",
           (int)width, (int)height, slot);
  log_line(line);
  return reinterpret_cast<EGLSurface>(kPbufferBase + slot);
}

EGLBoolean ytv_egl_destroy_surface(EGLDisplay display, EGLSurface surface) {
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  pthread_mutex_lock(&g_state_mutex);
  int p = pbuffer_index(surface);
  if (p >= 0) {
    g_pbuffers[p].used = false;
    pthread_mutex_unlock(&g_state_mutex);
    ok();
    return EGL_TRUE;
  }
  if (is_window_surface(surface)) {
    g_window_surface = EGL_NO_SURFACE;
    pthread_mutex_unlock(&g_state_mutex);
    ok();
    return EGL_TRUE;
  }
  pthread_mutex_unlock(&g_state_mutex);
  return fail(EGL_BAD_SURFACE);
}

EGLBoolean ytv_egl_query_surface(EGLDisplay display, EGLSurface surface,
                                 EGLint attribute, EGLint* value) {
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  if (!value) return fail(EGL_BAD_PARAMETER);
  pthread_mutex_lock(&g_state_mutex);
  int p = pbuffer_index(surface);
  Pbuffer copy = p >= 0 ? g_pbuffers[p] : Pbuffer{};
  bool window = is_window_surface(surface);
  pthread_mutex_unlock(&g_state_mutex);
  if (p >= 0) {
    if (attribute == EGL_WIDTH) { *value = copy.width; ok(); return EGL_TRUE; }
    if (attribute == EGL_HEIGHT) { *value = copy.height; ok(); return EGL_TRUE; }
    return fail(EGL_BAD_ATTRIBUTE);
  }
  if (!window) return fail(EGL_BAD_SURFACE);
  EGLBoolean r = ::eglQuerySurface(display, surface, attribute, value);
  if (r) ok(); else t_error = EGL_BAD_ATTRIBUTE;
  return r;
}

EGLBoolean ytv_egl_make_current(EGLDisplay display, EGLSurface draw,
                                EGLSurface read, EGLContext context) {
  int n = __sync_add_and_fetch(&g_make_current_count, 1);
  bool trace = n <= 24;
  if (trace) {
    char line[180];
    snprintf(line, sizeof(line),
             "YTV_EGL_MAKE_CURRENT_ENTER n=%d tid=%d draw=%p ctx=%p\n", n,
             (int)sceKernelGetThreadId(), (void*)draw, (void*)context);
    log_line(line);
  }
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  pthread_t self = pthread_self();
  pthread_mutex_lock(&g_state_mutex);
  if (context == EGL_NO_CONTEXT) {
    if (draw != EGL_NO_SURFACE || read != EGL_NO_SURFACE) {
      pthread_mutex_unlock(&g_state_mutex);
      return fail(EGL_BAD_MATCH);
    }
    if (t_context != EGL_NO_CONTEXT && g_context_bound &&
        pthread_equal(g_context_owner, self)) {
      ::glFinish();
      g_context_bound = false;
    }
    t_context = EGL_NO_CONTEXT;
    t_draw = t_read = EGL_NO_SURFACE;
    t_display = EGL_NO_DISPLAY;
    pthread_mutex_unlock(&g_state_mutex);
    ok();
    if (trace) log_line("YTV_EGL_MAKE_CURRENT_RETURN released\n");
    return EGL_TRUE;
  }
  if (context != g_context) {
    pthread_mutex_unlock(&g_state_mutex);
    return fail(EGL_BAD_CONTEXT);
  }
  if (!is_valid_surface(draw) || !is_valid_surface(read)) {
    pthread_mutex_unlock(&g_state_mutex);
    return fail(EGL_BAD_SURFACE);
  }
  if (g_context_bound && !pthread_equal(g_context_owner, self)) {
    pthread_mutex_unlock(&g_state_mutex);
    if (trace) log_line("YTV_EGL_MAKE_CURRENT_RETURN bad_access\n");
    return fail(EGL_BAD_ACCESS);
  }
  EGLBoolean r = ::eglMakeCurrent(display, draw, read, context);
  if (r) {
    g_context_owner = self;
    g_context_bound = true;
    t_context = context;
    t_draw = draw;
    t_read = read;
    t_display = display;
  }
  pthread_mutex_unlock(&g_state_mutex);
  if (r) ok(); else t_error = EGL_BAD_MATCH;
  if (trace) {
    char line[120];
    snprintf(line, sizeof(line), "YTV_EGL_MAKE_CURRENT_RETURN success=%d\n", (int)r);
    log_line(line);
  }
  return r;
}

EGLContext ytv_egl_get_current_context() { ok(); return t_context; }
EGLDisplay ytv_egl_get_current_display() { ok(); return t_display; }
EGLSurface ytv_egl_get_current_surface(EGLint which) {
  if (which == EGL_DRAW) { ok(); return t_draw; }
  if (which == EGL_READ) { ok(); return t_read; }
  t_error = EGL_BAD_PARAMETER;
  return EGL_NO_SURFACE;
}

EGLBoolean ytv_egl_release_thread() {
  if (t_context != EGL_NO_CONTEXT && g_display != EGL_NO_DISPLAY)
    ytv_egl_make_current(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                         EGL_NO_CONTEXT);
  ok();
  return EGL_TRUE;
}

EGLBoolean ytv_egl_wait_client() {
  if (t_context != EGL_NO_CONTEXT) ::glFinish();
  ok();
  return EGL_TRUE;
}
EGLBoolean ytv_egl_wait_native(EGLint engine) {
  (void)engine;  // no native rendering API other than GL exists on Vita
  ok();
  return EGL_TRUE;
}

#if defined(YTV_GL_TRACE)
// Diagnostic: save what the browser rendered into the back buffer (before the
// swap) as a binary PPM, for frames 1-3 and every 64th frame.
void dump_frame(int n) {
  if (!(n <= 3 || (n % 64) == 0)) return;
  const int w = 960, h = 544;
  uint8_t* rgba = static_cast<uint8_t*>(malloc((size_t)w * h * 4));
  if (!rgba) return;
  ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  char path[96];
  snprintf(path, sizeof(path), "ux0:data/youtubevita-noads/frame_%04d.ppm", n);
  int fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) {
    char hdr[32];
    int hl = snprintf(hdr, sizeof(hdr), "P6 %d %d 255\n", w, h);
    sceIoWrite(fd, hdr, hl);
    uint8_t* row = static_cast<uint8_t*>(malloc((size_t)w * 3));
    for (int y = h - 1; row && y >= 0; y--) {  // GL origin is bottom-left
      const uint8_t* src = rgba + (size_t)y * w * 4;
      for (int x = 0; x < w; x++) {
        row[x * 3] = src[x * 4]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + 2];
      }
      sceIoWrite(fd, row, w * 3);
    }
    free(row);
    sceIoClose(fd);
  }
  free(rgba);
}
#endif
extern "C" void ytv_addons_before_swap();
extern "C" void ytv_addons_after_swap();
// VitaGL's own pools (sampled on the GL thread once a second; vglMemFree walks
// VitaGL's heap and must not race with it). When these run dry VitaGL draws
// with missing vertex data / textures: stretched triangles, missing images.
// (VitaGL pool counters: declared at file scope above.)
extern "C" void ytv_player_on_swap();
extern "C" void ytv_native_startup_splash_release(void);
extern "C" bool ytv_osk_active();
EGLBoolean ytv_egl_swap_buffers(EGLDisplay display, EGLSurface surface) {
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  if (pbuffer_index(surface) >= 0) { ok(); return EGL_TRUE; }  // no-op per spec
  if (!is_window_surface(surface)) return fail(EGL_BAD_SURFACE);
  if (t_context == EGL_NO_CONTEXT || t_draw != surface)
    return fail(EGL_BAD_SURFACE);
  uint64_t swap_t0 = sceKernelGetSystemTimeWide();
  if (g_prof_frame_first) {
    g_prof_raster_us += swap_t0 - g_prof_frame_first;
    g_hb_raster_us += swap_t0 - g_prof_frame_first;
  }
  g_prof_frame_first = 0;
  {
    static uint64_t last_mem = 0;
    uint64_t t = sceKernelGetSystemTimeWide();
    if (t - last_mem > 1000000ull) {
      last_mem = t;
      g_vgl_free_vram = vglMemFree(VGL_MEM_VRAM);
      g_vgl_free_ram = vglMemFree(VGL_MEM_RAM);
      g_vgl_free_phy = vglMemFree(VGL_MEM_SLOW);
    }
  }
  if (g_startup_bg_active) {
    // A clearing-only/empty frame is not the YouTube UI. During the handoff
    // from our Lottie scanout to Cobalt, cover only these bootstrap frames.
    // Pure GL clear, no YUV manipulation/readback or per-pixel CPU work.
    const uint32_t draws = g_startup_draws;
    g_startup_draws = 0;
    if (draws <= 2u) {
      GLfloat old_clear[4] = {0.f, 0.f, 0.f, 0.f};
      ::glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
      GLboolean old_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
      ::glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);
      const GLboolean scissor = ::glIsEnabled(GL_SCISSOR_TEST);
      if (scissor) ::glDisable(GL_SCISSOR_TEST);
      ::glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      ::glClearColor(33.f / 255.f, 33.f / 255.f, 33.f / 255.f, 1.f);
      ::glClear(GL_COLOR_BUFFER_BIT);
      ::glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
      ::glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
      if (scissor) ::glEnable(GL_SCISSOR_TEST);
      static int filled = 0;
      if (filled++ == 0) log_line("YTV_BOOT_BG #212121 (Cobalt bootstrap only)\n");
    } else {
      g_startup_bg_active = false;
      char msg[96];
      snprintf(msg, sizeof(msg), "YTV_BOOT_BG complete: first UI frame draws=%u\n", draws);
      log_line(msg);
    }
  }
  ytv_addons_before_swap();  // native add-on menu / toast overlay
  // While the Vita IME dialog is open VitaGL must draw the system dialog too.
  if (ytv_osk_active()) vglSwapBuffers(GL_TRUE);
  else ::eglSwapBuffers(display, surface);
  ytv_addons_after_swap();  // inverted screen: target of the next frame
  ytv_native_startup_splash_release();  // release intro fb AFTER VitaGL swap
  ytv_player_on_swap();  // frees video decode targets the GPU is done with
  g_prof_last_swap_end = sceKernelGetSystemTimeWide();
  g_hb_swap_us += g_prof_last_swap_end - swap_t0;
  g_hb_frames++;
  if (g_hb_prev_swap) {
    uint64_t interval = swap_t0 - g_hb_prev_swap;
    if (interval < 250000) {
      if (interval > g_hb_max_interval_us) g_hb_max_interval_us = interval;
      if (interval > 20000) g_hb_jank20++;
      if (interval > 40000) g_hb_jank40++;
    }
  }
  g_hb_prev_swap = swap_t0;
  {
    // Per-second frame profile (verbose log only): frames presented, time
    // spent inside eglSwapBuffers, in draw calls, in texture uploads, and the
    // number of draw calls.
    static uint64_t window_start = 0, swap_us = 0;
    static int window_frames = 0;
    uint64_t now = sceKernelGetSystemTimeWide();
    swap_us += now - swap_t0;
    if (!window_start) window_start = now;
    window_frames++;
    if (now - window_start >= 1000000) {
      char fps[200];
      snprintf(fps, sizeof(fps),
               "YTV_FPS t=%llums frames=%d window_ms=%llu swap_ms=%llu draw_ms=%llu "
               "draws=%u upload_ms=%llu raster_ms=%llu gap_ms=%llu\n",
               (unsigned long long)(now / 1000), window_frames,
               (unsigned long long)((now - window_start) / 1000),
               (unsigned long long)(swap_us / 1000),
               (unsigned long long)(g_prof_draw_us / 1000), (unsigned)g_prof_draws,
               (unsigned long long)(g_prof_upload_us / 1000),
               (unsigned long long)(g_prof_raster_us / 1000),
               (unsigned long long)(g_prof_idle_us / 1000));
      log_line(fps);
      window_start = now; window_frames = 0; swap_us = 0;
      g_prof_draw_us = 0; g_prof_draws = 0; g_prof_upload_us = 0;
      g_prof_raster_us = 0; g_prof_idle_us = 0;
    }
  }
  {
    uint64_t pressed = g_ytv_last_input_us;
    if (pressed) {
      g_ytv_last_input_us = 0;
      char lat[80];
      snprintf(lat, sizeof(lat), "YTV_INPUT_LATENCY ms=%llu\n",
               (unsigned long long)((sceKernelGetSystemTimeWide() - pressed) / 1000));
      log_line(lat);
    }
  }
  int n = __sync_add_and_fetch(&g_swap_count, 1);
  if (n <= 5 || (n & (n - 1)) == 0) {
    char line[100];
    snprintf(line, sizeof(line), "YTV_EGL_SWAP_BUFFERS n=%d\n", n);
    log_line(line);
  }
  ok();
  return EGL_TRUE;
}

EGLBoolean ytv_egl_surface_attrib(EGLDisplay display, EGLSurface surface,
                                  EGLint attribute, EGLint value) {
  (void)value;
  if (!is_valid_display(display)) return fail(EGL_BAD_DISPLAY);
  if (!is_valid_surface(surface)) return fail(EGL_BAD_SURFACE);
  (void)attribute;
  // GXM display buffers rotate; preserving contents (EGL_BUFFER_PRESERVED)
  // or other surface attributes cannot be honoured by VitaGL.
  return fail(EGL_BAD_MATCH);
}

// Entry points the platform genuinely cannot implement: proper EGL errors.
EGLBoolean ytv_egl_copy_buffers(EGLDisplay, EGLSurface, void*) {
  return fail(EGL_BAD_MATCH);
}
EGLSurface ytv_egl_create_pixmap_surface(EGLDisplay, EGLConfig, void*,
                                         const EGLint*) {
  t_error = EGL_BAD_MATCH;
  return EGL_NO_SURFACE;
}
EGLBoolean ytv_egl_bind_tex_image(EGLDisplay, EGLSurface, EGLint) {
  return fail(EGL_BAD_MATCH);
}
EGLBoolean ytv_egl_release_tex_image(EGLDisplay, EGLSurface, EGLint) {
  return fail(EGL_BAD_MATCH);
}
EGLSurface ytv_egl_create_pbuffer_from_client_buffer(EGLDisplay, EGLenum,
                                                    void*, EGLConfig,
                                                    const EGLint*) {
  t_error = EGL_BAD_PARAMETER;
  return EGL_NO_SURFACE;
}
// EGL 1.5 entry points: we advertise 1.4, so callers must not use them; if
// they still do, they get a defined failure instead of a null jump.
void* ytv_egl_create_sync(EGLDisplay, EGLenum, const intptr_t*) {
  t_error = EGL_BAD_DISPLAY;
  return nullptr;
}
EGLBoolean ytv_egl_destroy_sync(EGLDisplay, void*) {
  return fail(EGL_BAD_PARAMETER);
}
EGLint ytv_egl_client_wait_sync(EGLDisplay, void*, EGLint, uint64_t) {
  t_error = EGL_BAD_PARAMETER;
  return EGL_FALSE;
}
EGLBoolean ytv_egl_get_sync_attrib(EGLDisplay, void*, EGLint, intptr_t*) {
  return fail(EGL_BAD_PARAMETER);
}
void* ytv_egl_create_image(EGLDisplay, EGLContext, EGLenum, void*,
                           const intptr_t*) {
  t_error = EGL_BAD_PARAMETER;
  return nullptr;
}
EGLBoolean ytv_egl_destroy_image(EGLDisplay, void*) {
  return fail(EGL_BAD_PARAMETER);
}
EGLDisplay ytv_egl_get_platform_display(EGLenum, void*, const intptr_t*) {
  t_error = EGL_BAD_PARAMETER;
  return EGL_NO_DISPLAY;
}
EGLSurface ytv_egl_create_platform_window_surface(EGLDisplay, EGLConfig,
                                                  void*, const intptr_t*) {
  t_error = EGL_BAD_PARAMETER;
  return EGL_NO_SURFACE;
}
EGLSurface ytv_egl_create_platform_pixmap_surface(EGLDisplay, EGLConfig,
                                                  void*, const intptr_t*) {
  t_error = EGL_BAD_PARAMETER;
  return EGL_NO_SURFACE;
}
EGLBoolean ytv_egl_wait_sync(EGLDisplay, void*, EGLint) {
  return fail(EGL_BAD_PARAMETER);
}

// ---- GLES 2.0 core entry points VitaGL does not export ---------------------
// Skia's GrGLInterface::validate() rejects an ES2 interface without these.

// Renderbuffer bookkeeping for glGetRenderbufferParameteriv.
struct RenderbufferInfo { GLuint id; GLenum format; GLsizei w, h; };
constexpr int kMaxRenderbuffers = 256;
RenderbufferInfo g_rbos[kMaxRenderbuffers] = {};
GLuint g_bound_rbo = 0;  // GL context is single, so one binding
pthread_mutex_t g_gl_mutex = PTHREAD_MUTEX_INITIALIZER;

// v0.64 inverted screen: while active, Cobalt's default framebuffer (0) is
// an offscreen framebuffer of the Add-ons layer; the overlay turns it 180
// degrees onto the real screen at swap time (port_addons.cpp).
extern "C" unsigned ytv_screen_fbo();
void ytv_gl_bind_framebuffer(GLenum target, GLuint fb) {
  if (fb == 0) fb = ytv_screen_fbo();
  ::glBindFramebuffer(target, fb);
}
// vitaGL places the scissor box of a texture framebuffer with the opposite
// vertical origin from the display (geometry is not affected). Rectangles
// Skia sets for the default framebuffer are mirrored while that framebuffer
// is the inverted-screen target.
inline bool on_screen_fbo() {
  const GLuint screen = ytv_screen_fbo();
  if (!screen) return false;
  GLint bound = 0;
  ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
  return (GLuint)bound == screen;
}
void ytv_gl_scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (on_screen_fbo()) y = 544 - y - h;
  ::glScissor(x, y, w, h);
}
void ytv_gl_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  if (on_screen_fbo()) y = 544 - y - h;
  ::glViewport(x, y, w, h);
}

void ytv_gl_bind_renderbuffer(GLenum target, GLuint rbo) {
  ::glBindRenderbuffer(target, rbo);
  pthread_mutex_lock(&g_gl_mutex);
  g_bound_rbo = rbo;
  pthread_mutex_unlock(&g_gl_mutex);
}
void ytv_gl_renderbuffer_storage(GLenum target, GLenum format, GLsizei w,
                                 GLsizei h) {
  {
    static int n = 0;
    if (n++ < 30) {
      char line[112];
      snprintf(line, sizeof(line), "YTV_LIFECYCLE_GL_RBO %dx%d fmt=0x%x\n", (int)w, (int)h, (unsigned)format);
      log_line(line);
    }
  }
  ::glRenderbufferStorage(target, format, w, h);
  pthread_mutex_lock(&g_gl_mutex);
  if (g_bound_rbo) {
    int slot = -1;
    for (int i = 0; i < kMaxRenderbuffers; i++) {
      if (g_rbos[i].id == g_bound_rbo) { slot = i; break; }
      if (slot < 0 && g_rbos[i].id == 0) slot = i;
    }
    if (slot >= 0) g_rbos[slot] = {g_bound_rbo, format, w, h};
  }
  pthread_mutex_unlock(&g_gl_mutex);
}
void ytv_gl_delete_renderbuffers(GLsizei n, const GLuint* ids) {
  ::glDeleteRenderbuffers(n, ids);
  pthread_mutex_lock(&g_gl_mutex);
  for (GLsizei k = 0; ids && k < n; k++) {
    for (int i = 0; i < kMaxRenderbuffers; i++)
      if (ids[k] && g_rbos[i].id == ids[k]) g_rbos[i] = {};
    if (ids[k] == g_bound_rbo) g_bound_rbo = 0;
  }
  pthread_mutex_unlock(&g_gl_mutex);
}
void ytv_gl_get_renderbuffer_parameteriv(GLenum target, GLenum pname,
                                         GLint* params) {
  (void)target;
  if (!params) return;
  pthread_mutex_lock(&g_gl_mutex);
  RenderbufferInfo info = {};
  for (int i = 0; i < kMaxRenderbuffers; i++)
    if (g_bound_rbo && g_rbos[i].id == g_bound_rbo) info = g_rbos[i];
  pthread_mutex_unlock(&g_gl_mutex);
  switch (pname) {
    case GL_RENDERBUFFER_WIDTH: *params = info.w; break;
    case GL_RENDERBUFFER_HEIGHT: *params = info.h; break;
    case GL_RENDERBUFFER_INTERNAL_FORMAT:
      *params = info.format ? (GLint)info.format : GL_RGBA4;
      break;
    default: *params = 0; break;
  }
}

// Texture parameter bookkeeping for glGetTexParameteriv (used by Cobalt's
// textured mesh renderer) and glTexParameterfv.
struct TextureParams { GLuint id; GLint min, mag, wrap_s, wrap_t; };
constexpr int kMaxTextures = 1024;
TextureParams g_textures[kMaxTextures] = {};

GLuint bound_texture(GLenum target) {
  GLint id = 0;
  ::glGetIntegerv(target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_BINDING_CUBE_MAP
                                                : GL_TEXTURE_BINDING_2D, &id);
  return (GLuint)id;
}
TextureParams* texture_slot(GLuint id, bool create) {
  if (!id) return nullptr;
  TextureParams* free_slot = nullptr;
  for (int i = 0; i < kMaxTextures; i++) {
    if (g_textures[i].id == id) return &g_textures[i];
    if (!free_slot && g_textures[i].id == 0) free_slot = &g_textures[i];
  }
  if (!create || !free_slot) return nullptr;
  *free_slot = {id, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT};
  return free_slot;
}
void record_tex_param(GLenum target, GLenum pname, GLint value) {
  GLuint id = bound_texture(target);
  pthread_mutex_lock(&g_gl_mutex);
  TextureParams* t = texture_slot(id, true);
  if (t) {
    if (pname == GL_TEXTURE_MIN_FILTER) t->min = value;
    else if (pname == GL_TEXTURE_MAG_FILTER) t->mag = value;
    else if (pname == GL_TEXTURE_WRAP_S) t->wrap_s = value;
    else if (pname == GL_TEXTURE_WRAP_T) t->wrap_t = value;
  }
  pthread_mutex_unlock(&g_gl_mutex);
}
void ytv_gl_tex_parameteri(GLenum target, GLenum pname, GLint value) {
  ::glTexParameteri(target, pname, value);
  record_tex_param(target, pname, value);
}
void ytv_gl_tex_parameterf(GLenum target, GLenum pname, GLfloat value) {
  ::glTexParameterf(target, pname, value);
  record_tex_param(target, pname, (GLint)value);
}
void ytv_gl_tex_parameteriv(GLenum target, GLenum pname, const GLint* value) {
  ::glTexParameteriv(target, pname, const_cast<GLint*>(value));
  if (value) record_tex_param(target, pname, value[0]);
}
void ytv_gl_tex_parameterfv(GLenum target, GLenum pname, const GLfloat* value) {
  if (!value) return;
  // All GLES2 texture parameters are scalar.
  ::glTexParameterf(target, pname, value[0]);
  record_tex_param(target, pname, (GLint)value[0]);
}
void ytv_gl_get_tex_parameteriv(GLenum target, GLenum pname, GLint* out) {
  if (!out) return;
  GLuint id = bound_texture(target);
  pthread_mutex_lock(&g_gl_mutex);
  TextureParams* t = texture_slot(id, false);
  TextureParams v = t ? *t : TextureParams{id, GL_NEAREST_MIPMAP_LINEAR,
                                            GL_LINEAR, GL_REPEAT, GL_REPEAT};
  pthread_mutex_unlock(&g_gl_mutex);
  switch (pname) {
    case GL_TEXTURE_MIN_FILTER: *out = v.min; break;
    case GL_TEXTURE_MAG_FILTER: *out = v.mag; break;
    case GL_TEXTURE_WRAP_S: *out = v.wrap_s; break;
    case GL_TEXTURE_WRAP_T: *out = v.wrap_t; break;
    default: break;
  }
}
void ytv_gl_get_tex_parameterfv(GLenum target, GLenum pname, GLfloat* out) {
  if (!out) return;
  GLint v = 0;
  ytv_gl_get_tex_parameteriv(target, pname, &v);
  *out = (GLfloat)v;
}
void ytv_gl_delete_textures(GLsizei n, const GLuint* ids) {
  ::glDeleteTextures(n, ids);
  pthread_mutex_lock(&g_gl_mutex);
  for (GLsizei k = 0; ids && k < n; k++) {
    TextureParams* t = texture_slot(ids[k], false);
    if (t) *t = {};
  }
  pthread_mutex_unlock(&g_gl_mutex);
}

// SGX543MP4 fragment and vertex units execute full IEEE fp32 for every
// precision qualifier, and 32-bit integers.
void ytv_gl_get_shader_precision_format(GLenum shader, GLenum type,
                                        GLint* range, GLint* precision) {
  (void)shader;
  if (!range || !precision) return;
  switch (type) {
    case GL_LOW_FLOAT: case GL_MEDIUM_FLOAT: case GL_HIGH_FLOAT:
      range[0] = 127; range[1] = 127; *precision = 23; break;
    case GL_LOW_INT: case GL_MEDIUM_INT: case GL_HIGH_INT:
      range[0] = 31; range[1] = 30; *precision = 0; break;
    default:
      range[0] = range[1] = 0; *precision = 0; break;
  }
}

// glShaderSource: GLSL ES 1.00 section 3.4 requires implementations to ignore
// unrecognised #pragma directives. Cobalt's direct-GLES rasterizer shaders
// carry "#pragma array name(a, b)" lines, a build-time directive for its own
// generate_shader_impl.py; VitaGL's GLSL preprocessor hangs forever on them
// inside glLinkProgram (reproduced by the graphics test). Remove the pragmas
// the driver must ignore anyway; keep "#pragma STDGL ..." (invariance).
bool is_ignorable_pragma(const char* line, const char* end) {
  const char* p = line;
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  if (p >= end || *p != '#') return false;
  p++;
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  if (end - p < 6 || strncmp(p, "pragma", 6) != 0) return false;
  p += 6;
  while (p < end && (*p == ' ' || *p == '\t')) p++;
  return !(end - p >= 5 && strncmp(p, "STDGL", 5) == 0);
}
volatile int g_pragmas_removed = 0;
void ytv_gl_shader_source(GLuint shader, GLsizei count,
                          const GLchar* const* strings, const GLint* lengths) {
  if (count <= 0 || !strings) {
    ::glShaderSource(shader, count, const_cast<const GLchar**>(strings), lengths);
    return;
  }
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++) {
    if (!strings[i]) continue;
    total += (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(strings[i]);
  }
  char* merged = static_cast<char*>(malloc(total + 1));
  if (!merged) {  // fall back to the unmodified source
    ::glShaderSource(shader, count, const_cast<const GLchar**>(strings), lengths);
    return;
  }
  size_t out = 0;
  for (GLsizei i = 0; i < count; i++) {
    if (!strings[i]) continue;
    size_t n = (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(strings[i]);
    memcpy(merged + out, strings[i], n);
    out += n;
  }
  merged[out] = 0;
  // Filter line by line in place.
  size_t w = 0;
  int removed = 0;
  for (size_t r = 0; r < out;) {
    size_t e = r;
    while (e < out && merged[e] != '\n') e++;
    size_t next = e < out ? e + 1 : e;
    if (is_ignorable_pragma(merged + r, merged + e)) {
      removed++;
      if (e < out) merged[w++] = '\n';  // keep line numbering stable
    } else {
      memmove(merged + w, merged + r, next - r);
      w += next - r;
    }
    r = next;
  }
  merged[w] = 0;
  if (removed) __sync_add_and_fetch(&g_pragmas_removed, removed);
  // DIAGNOSTIC: ux0:data/youtubevita-noads/dump_shaders.txt writes every distinct
  // shader source to ux0:data/youtubevita-noads/shaders/<n>.glsl.
  {
    static int enabled = -1, n = 0;
    if (enabled < 0) {
      SceIoStat st;
      enabled = sceIoGetstat("ux0:data/youtubevita-noads/dump_shaders.txt", &st) >= 0;
      if (enabled) sceIoMkdir("ux0:data/youtubevita-noads/shaders", 0777);
    }
    if (enabled && n < 400) {
      char path[96];
      snprintf(path, sizeof(path), "ux0:data/youtubevita-noads/shaders/%03d.glsl", n++);
      SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
      if (fd >= 0) { sceIoWrite(fd, merged, (SceSize)w); sceIoClose(fd); }
    }
  }
  const GLchar* one = merged;
  GLint len = (GLint)w;
  ::glShaderSource(shader, 1, &one, &len);
  free(merged);
}

// ---- GLES2 pixel-store state ---------------------------------------------
// VitaGL's glPixelStorei only knows GL_UNPACK_ROW_LENGTH and raises
// GL_INVALID_ENUM for the core GLES2 GL_UNPACK_ALIGNMENT/GL_PACK_ALIGNMENT,
// which Cobalt sets before every upload (its GL_CALL then aborts). The state
// is kept here, per GL semantics, and uploads/readbacks whose rows are not
// tightly packed are repacked before/after calling VitaGL (which always
// reads and writes tight rows).
GLint g_unpack_alignment = 4;
GLint g_unpack_row_length = 0;
GLint g_pack_alignment = 4;

int bytes_per_pixel(GLenum format, GLenum type) {
  int channels;
  switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: case 0x1903 /*GL_RED*/: channels = 1; break;
    case GL_LUMINANCE_ALPHA: case 0x8227 /*GL_RG*/: channels = 2; break;
    case GL_RGB: channels = 3; break;
    case GL_RGBA: case 0x80E1 /*GL_BGRA_EXT*/: channels = 4; break;
    default: return 0;
  }
  switch (type) {
    case GL_UNSIGNED_BYTE: return channels;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_FLOAT: return channels * 4;
    case 0x8D61 /*GL_HALF_FLOAT_OES*/: case 0x140B /*GL_HALF_FLOAT*/: return channels * 2;
    default: return 0;
  }
}
size_t align_up(size_t v, GLint a) { return a > 1 ? (v + a - 1) / a * a : v; }

// Returns a tightly packed copy when the client layout differs, else null.
void* repack_upload(GLsizei w, GLsizei h, GLenum format, GLenum type,
                    const void* pixels) {
  if (!pixels || w <= 0 || h <= 0) return nullptr;
  int bpp = bytes_per_pixel(format, type);
  if (!bpp) return nullptr;
  size_t tight = (size_t)w * bpp;
  size_t row_pixels = g_unpack_row_length > 0 ? (size_t)g_unpack_row_length : (size_t)w;
  size_t stride = align_up(row_pixels * bpp, g_unpack_alignment);
  if (stride == tight) return nullptr;
  uint8_t* out = static_cast<uint8_t*>(malloc(tight * (size_t)h));
  if (!out) return nullptr;
  const uint8_t* src = static_cast<const uint8_t*>(pixels);
  for (GLsizei y = 0; y < h; y++) memcpy(out + tight * y, src + stride * y, tight);
  return out;
}

void ytv_gl_pixel_storei(GLenum pname, GLint param) {
  switch (pname) {
    case GL_UNPACK_ALIGNMENT:
    case GL_PACK_ALIGNMENT:
      if (param != 1 && param != 2 && param != 4 && param != 8) return;  // GL_INVALID_VALUE
      (pname == GL_UNPACK_ALIGNMENT ? g_unpack_alignment : g_pack_alignment) = param;
      return;
    case GL_UNPACK_ROW_LENGTH:
      if (param >= 0) g_unpack_row_length = param;
      return;
    default:
      ::glPixelStorei(pname, param);  // VitaGL reports its own error
  }
}
void ytv_gl_get_integerv(GLenum pname, GLint* data) {
  if (!data) return;
  switch (pname) {
    case GL_UNPACK_ALIGNMENT: *data = g_unpack_alignment; return;
    case GL_PACK_ALIGNMENT: *data = g_pack_alignment; return;
    case GL_UNPACK_ROW_LENGTH: *data = g_unpack_row_length; return;
    // Core GLES2 queries VitaGL rejects. VitaGL's glReadPixels fast path is
    // RGBA/UNSIGNED_BYTE for the 8888 display and render targets.
    case 0x8B9A /*GL_IMPLEMENTATION_COLOR_READ_TYPE*/: *data = GL_UNSIGNED_BYTE; return;
    case 0x8B9B /*GL_IMPLEMENTATION_COLOR_READ_FORMAT*/: *data = GL_RGBA; return;
    // VitaGL reports one program-binary format but never names it (its
    // glGetProgramBinary leaves binaryFormat unset), so no usable format
    // exists: report none and let callers compile from source.
    case 0x87FE /*GL_NUM_PROGRAM_BINARY_FORMATS*/: *data = 0; return;
    case 0x87FF /*GL_PROGRAM_BINARY_FORMATS*/: return;
    case GL_FRAMEBUFFER_BINDING: {  // the inverted-screen target reads as 0
      ::glGetIntegerv(pname, data);
      const GLint screen = (GLint)ytv_screen_fbo();
      if (screen && *data == screen) *data = 0;
      return;
    }
    default: ::glGetIntegerv(pname, data);
  }
}
// GL_DITHER is a core GLES2 capability that VitaGL rejects. Dithering is
// optional per spec and has no effect on 8-bit-per-channel targets; keep the
// state so glIsEnabled answers correctly.
GLboolean g_dither_enabled = GL_TRUE;  // GLES2 initial value
void ytv_gl_enable(GLenum cap) {
  if (cap == GL_DITHER) { g_dither_enabled = GL_TRUE; return; }
  ::glEnable(cap);
}
void ytv_gl_disable(GLenum cap) {
  if (cap == GL_DITHER) { g_dither_enabled = GL_FALSE; return; }
  ::glDisable(cap);
}
GLboolean ytv_gl_is_enabled(GLenum cap) {
  if (cap == GL_DITHER) return g_dither_enabled;
  return ::glIsEnabled(cap);
}
// DIAGNOSTIC (verbose log): internal format of each texture, to report the
// formats Cobalt/Skia attach as render targets.
static uint16_t g_diag_tex_fmt[4096];
void ytv_gl_tex_image_2d(GLenum target, GLint level, GLint ifmt, GLsizei w,
                         GLsizei h, GLint border, GLenum format, GLenum type,
                         const void* pixels) {
  if (level == 0) {
    GLint bound = 0;
    ::glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    if (bound > 0 && bound < 4096) g_diag_tex_fmt[bound] = (uint16_t)ifmt;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) {
      static int odd = 0;
      if (odd++ < 20) {
        char line[128];
        snprintf(line, sizeof(line), "YTV_LIFECYCLE_GL_ODD_TEXTURE %dx%d ifmt=0x%x data=%d\n", (int)w,
                 (int)h, (unsigned)ifmt, pixels != nullptr);
        log_line(line);
      }
    }
    static uint32_t seen[24];
    static int nseen = 0;
    uint32_t key = ((uint32_t)ifmt & 0xffff) << 16 ^ ((uint32_t)format & 0xffff) << 4 ^ ((uint32_t)type & 0xffff);
    bool known = false;
    for (int i = 0; i < nseen; i++) known |= seen[i] == key;
    if (!known && nseen < 24) {
      seen[nseen++] = key;
      char line[128];
      snprintf(line, sizeof(line), "YTV_GL_TEX_FORMAT ifmt=0x%x format=0x%x type=0x%x %dx%d data=%d\n",
               (unsigned)ifmt, (unsigned)format, (unsigned)type, (int)w, (int)h, pixels != nullptr);
      log_line(line);
    }
  }
  void* packed = repack_upload(w, h, format, type, pixels);
  uint64_t t0 = sceKernelGetSystemTimeWide();
  ::glTexImage2D(target, level, ifmt, w, h, border, format, type,
                 packed ? packed : pixels);
  g_prof_upload_us += sceKernelGetSystemTimeWide() - t0;
  free(packed);
}
void ytv_gl_tex_sub_image_2d(GLenum target, GLint level, GLint x, GLint y,
                             GLsizei w, GLsizei h, GLenum format, GLenum type,
                             const void* pixels) {
  void* packed = repack_upload(w, h, format, type, pixels);
  if (format == GL_ALPHA && pixels) {
    // DIAGNOSTIC (verbose): content of glyph-atlas uploads.
    static int logged = 0;
    if (logged < 60) {
      logged++;
      const uint8_t* p = static_cast<const uint8_t*>(packed ? packed : pixels);
      int zeros = 0, full = 0, mid = 0;
      for (int i = 0; i < w * h; i++) {
        if (p[i] == 0) zeros++; else if (p[i] == 255) full++; else mid++;
      }
      char line[160];
      snprintf(line, sizeof(line),
               "YTV_GL_A8_UPLOAD at=%d,%d %dx%d zero=%d full=%d mid=%d rowlen=%d align=%d\n",
               (int)x, (int)y, (int)w, (int)h, zeros, full, mid, g_unpack_row_length,
               g_unpack_alignment);
      log_line(line);
    }
  }
  uint64_t t0 = sceKernelGetSystemTimeWide();
  ::glTexSubImage2D(target, level, x, y, w, h, format, type,
                    packed ? packed : pixels);
  g_prof_upload_us += sceKernelGetSystemTimeWide() - t0;
  free(packed);
}
void ytv_gl_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                        GLenum type, void* data) {
  int bpp = bytes_per_pixel(format, type);
  size_t tight = (size_t)(w > 0 ? w : 0) * bpp;
  size_t stride = align_up(tight, g_pack_alignment);
  if (!data || !bpp || h <= 0 || stride == tight) {
    ::glReadPixels(x, y, w, h, format, type, data);
    return;
  }
  // VitaGL writes tight rows: read tight, then spread rows to the stride
  // (bottom-up so the in-place move never overwrites unread rows).
  ::glReadPixels(x, y, w, h, format, type, data);
  uint8_t* p = static_cast<uint8_t*>(data);
  for (GLsizei r = h - 1; r > 0; r--) memmove(p + stride * r, p + tight * r, tight);
}

// Not implementable on VitaGL: report once, never silently pretend.
volatile int g_unsupported_reported = 0;
void report_unsupported(int bit, const char* name) {
  int prev = __sync_fetch_and_or(&g_unsupported_reported, bit);
  if (prev & bit) return;
  char line[120];
  snprintf(line, sizeof(line), "YTV_GLES_UNSUPPORTED %s\n", name);
  log_line(line);
}
void ytv_gl_blend_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  // Only matters for GL_CONSTANT_COLOR blend factors, which VitaGL's GXM
  // blend setup cannot express.
  if (r != 0.f || g != 0.f || b != 0.f || a != 0.f)
    report_unsupported(1, "glBlendColor(non-zero)");
}
void ytv_gl_compressed_tex_sub_image_2d(GLenum, GLint, GLint, GLint, GLsizei,
                                        GLsizei, GLenum, GLsizei,
                                        const void*) {
  report_unsupported(2, "glCompressedTexSubImage2D");
}
void ytv_gl_sample_coverage(GLfloat, GLboolean) {
  report_unsupported(4, "glSampleCoverage");
}
void ytv_gl_detach_shader(GLuint, GLuint) {
  // VitaGL links shaders by reference; detaching has no GXM equivalent.
  // Shader objects stay alive until glDeleteShader/glDeleteProgram.
}
void ytv_gl_validate_program(GLuint) {}
GLboolean ytv_gl_is_buffer(GLuint id) { return id != 0 ? GL_TRUE : GL_FALSE; }
GLboolean ytv_gl_is_shader(GLuint id) {
  if (!id) return GL_FALSE;
  GLint type = 0;
  ::glGetShaderiv(id, GL_SHADER_TYPE, &type);
  return type == GL_VERTEX_SHADER || type == GL_FRAGMENT_SHADER;
}

// Logs every extension lookup (Skia asks for each advertised extension's
// entry points; one null pointer invalidates the whole GrGLInterface).
volatile int g_proc_queries = 0;
// GL_EXT_debug_marker: VitaGL advertises the extension and exports the
// push/pop entry points but not glInsertEventMarkerEXT. Markers are
// annotations for debuggers only and have no rendering effect by spec.
void ytv_gl_insert_event_marker(GLsizei, const GLchar*) {}

struct ProcOverride { const char* name; void* fn; };
void* override_proc(const char* name) {
  static const ProcOverride table[] = {
    {"glShaderSource", reinterpret_cast<void*>(&ytv_gl_shader_source)},
    {"glEnable", reinterpret_cast<void*>(&ytv_gl_enable)},
    {"glDisable", reinterpret_cast<void*>(&ytv_gl_disable)},
    {"glIsEnabled", reinterpret_cast<void*>(&ytv_gl_is_enabled)},
    {"glPixelStorei", reinterpret_cast<void*>(&ytv_gl_pixel_storei)},
    {"glGetIntegerv", reinterpret_cast<void*>(&ytv_gl_get_integerv)},
    {"glTexImage2D", reinterpret_cast<void*>(&ytv_gl_tex_image_2d)},
    {"glTexSubImage2D", reinterpret_cast<void*>(&ytv_gl_tex_sub_image_2d)},
    {"glReadPixels", reinterpret_cast<void*>(&ytv_gl_read_pixels)},
    {"glInsertEventMarkerEXT", reinterpret_cast<void*>(&ytv_gl_insert_event_marker)},
    // EGL entry points resolve to this layer, never to VitaGL's facade.
    {"eglGetCurrentDisplay", reinterpret_cast<void*>(&ytv_egl_get_current_display)},
    {"eglGetCurrentContext", reinterpret_cast<void*>(&ytv_egl_get_current_context)},
    {"eglGetCurrentSurface", reinterpret_cast<void*>(&ytv_egl_get_current_surface)},
    {"eglMakeCurrent", reinterpret_cast<void*>(&ytv_egl_make_current)},
    {"eglGetError", reinterpret_cast<void*>(&ytv_egl_get_error)},
    {"eglQueryString", reinterpret_cast<void*>(&ytv_egl_query_string)},
    {"eglQuerySurface", reinterpret_cast<void*>(&ytv_egl_query_surface)},
    {"eglSwapBuffers", reinterpret_cast<void*>(&ytv_egl_swap_buffers)},
    {"eglCreatePbufferSurface", reinterpret_cast<void*>(&ytv_egl_create_pbuffer)},
    {"eglDestroySurface", reinterpret_cast<void*>(&ytv_egl_destroy_surface)},
    {"eglReleaseThread", reinterpret_cast<void*>(&ytv_egl_release_thread)},
    // Same bookkeeping wrappers as the Starboard GLES table.
    {"glTexParameteri", reinterpret_cast<void*>(&ytv_gl_tex_parameteri)},
    {"glTexParameterf", reinterpret_cast<void*>(&ytv_gl_tex_parameterf)},
    {"glTexParameteriv", reinterpret_cast<void*>(&ytv_gl_tex_parameteriv)},
    {"glTexParameterfv", reinterpret_cast<void*>(&ytv_gl_tex_parameterfv)},
    {"glGetTexParameteriv", reinterpret_cast<void*>(&ytv_gl_get_tex_parameteriv)},
    {"glDeleteTextures", reinterpret_cast<void*>(&ytv_gl_delete_textures)},
    {"glBindRenderbuffer", reinterpret_cast<void*>(&ytv_gl_bind_renderbuffer)},
    {"glRenderbufferStorage", reinterpret_cast<void*>(&ytv_gl_renderbuffer_storage)},
    {"glDeleteRenderbuffers", reinterpret_cast<void*>(&ytv_gl_delete_renderbuffers)},
    {"glGetRenderbufferParameteriv", reinterpret_cast<void*>(&ytv_gl_get_renderbuffer_parameteriv)},
    {"glGetShaderPrecisionFormat", reinterpret_cast<void*>(&ytv_gl_get_shader_precision_format)},
  };
  if (!name) return nullptr;
  for (const ProcOverride& o : table)
    if (strcmp(o.name, name) == 0) return o.fn;
  return nullptr;
}

void* ytv_egl_get_proc_address(const char* name) {
  void* p = override_proc(name);
  if (!p) p = reinterpret_cast<void*>(::eglGetProcAddress(name));
  int n = __sync_add_and_fetch(&g_proc_queries, 1);
  if (n <= 400) {
    char line[160];
    snprintf(line, sizeof(line), "YTV_EGL_PROC %s %s\n", name ? name : "(null)",
             p ? "ok" : "MISSING");
    log_line(line);
  }
  if (p) ok(); else t_error = EGL_SUCCESS;  // NULL is a valid "not found"
  return p;
}

// ---- Optional GL call trace (diagnostic builds only: YTV_GL_TRACE) --------
// Each line is written synchronously so the last entry before a freeze or
// emulator crash identifies the GL operation that triggered it.
#if defined(YTV_GL_TRACE)
volatile int g_gl_trace_n = 0;
bool gl_trace_on(int* out) {
  int n = __sync_add_and_fetch(&g_gl_trace_n, 1);
  *out = n;
  return n <= 3000;
}
#define GLT(fmt, ...) do { int n_; if (gl_trace_on(&n_)) { char l_[200]; \
  snprintf(l_, sizeof(l_), "YTV_GL %d tid=%d " fmt "\n", n_, \
  (int)sceKernelGetThreadId(), __VA_ARGS__); log_line(l_); } } while (0)
void t_compile_shader(GLuint s) {
  GLT("glCompileShader %u", s);
  ::glCompileShader(s);
  GLint ok = 0; ::glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  GLT("glCompileShader %u status=%d", s, ok);
  if (!ok) {
    char info[512] = {0}; GLsizei len = 0;
    ::glGetShaderInfoLog(s, sizeof(info) - 1, &len, info);
    log_line("YTV_GL_SHADER_LOG "); log_line(info); log_line("\n");
  }
}
void t_link_program(GLuint p) {
  GLT("glLinkProgram %u", p);
  ::glLinkProgram(p);
  GLint ok = 0; ::glGetProgramiv(p, GL_LINK_STATUS, &ok);
  GLT("glLinkProgram %u status=%d", p, ok);
}
void t_shader_source(GLuint sh, GLsizei count, const GLchar* const* str,
                     const GLint* len) {
  GLT("glShaderSource %u parts=%d", sh, count);
  for (GLsizei i = 0; str && i < count; i++) {
    const char* src = str[i];
    if (!src) continue;
    size_t n = len && len[i] >= 0 ? (size_t)len[i] : strlen(src);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "YTV_GL_SRC_BEGIN %u.%d\n", sh, (int)i);
    log_line(hdr);
    for (size_t off = 0; off < n; off += 900) {
      char chunk[904];
      size_t c = n - off < 900 ? n - off : 900;
      memcpy(chunk, src + off, c);
      chunk[c] = 0;
      log_line(chunk);
    }
    log_line("\nYTV_GL_SRC_END\n");
  }
  ytv_gl_shader_source(sh, count, str, len);
}
void t_clear_color(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  GLT("glClearColor %.3f %.3f %.3f %.3f", (double)r, (double)g, (double)b, (double)a);
  ::glClearColor(r, g, b, a);
}
void t_viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  GLT("glViewport %d %d %d %d", x, y, w, h); ytv_gl_viewport(x, y, w, h);
}
void t_scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  GLT("glScissor %d %d %d %d", x, y, w, h); ytv_gl_scissor(x, y, w, h);
}
void t_enable(GLenum c) { GLT("glEnable 0x%x", c); ytv_gl_enable(c); }
void t_disable(GLenum c) { GLT("glDisable 0x%x", c); ytv_gl_disable(c); }
void t_blend_func(GLenum s_, GLenum d) { GLT("glBlendFunc 0x%x 0x%x", s_, d); ::glBlendFunc(s_, d); }
void t_blend_func_sep(GLenum a, GLenum b, GLenum c, GLenum d) {
  GLT("glBlendFuncSeparate 0x%x 0x%x 0x%x 0x%x", a, b, c, d); ::glBlendFuncSeparate(a, b, c, d);
}
void t_color_mask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  GLT("glColorMask %d %d %d %d", r, g, b, a); ::glColorMask(r, g, b, a);
}
void t_uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
  GLT("glUniform4f loc=%d %.3f %.3f %.3f %.3f", l, (double)a, (double)b, (double)c, (double)d);
  ::glUniform4f(l, a, b, c, d);
}
void t_uniform4fv(GLint l, GLsizei n, const GLfloat* v) {
  GLT("glUniform4fv loc=%d n=%d %.3f %.3f %.3f %.3f", l, n, v ? (double)v[0] : 0.0,
      v ? (double)v[1] : 0.0, v ? (double)v[2] : 0.0, v ? (double)v[3] : 0.0);
  ::glUniform4fv(l, n, v);
}
void t_uniform_mat3(GLint l, GLsizei n, GLboolean t, const GLfloat* v) {
  GLT("glUniformMatrix3fv loc=%d t=%d [%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f]", l, t,
      (double)v[0], (double)v[1], (double)v[2], (double)v[3], (double)v[4], (double)v[5],
      (double)v[6], (double)v[7], (double)v[8]);
  ::glUniformMatrix3fv(l, n, t, v);
}
GLint t_get_uniform_location(GLuint p, const GLchar* n) {
  GLint r = ::glGetUniformLocation(p, n);
  GLT("glGetUniformLocation prog=%u %s -> %d", p, n ? n : "?", r);
  return r;
}
void t_vertex_attrib_pointer(GLuint i, GLint sz, GLenum ty, GLboolean nz, GLsizei st, const void* ptr) {
  GLT("glVertexAttribPointer idx=%u size=%d type=0x%x norm=%d stride=%d off=%p", i, sz, ty, nz, st, ptr);
  ::glVertexAttribPointer(i, sz, ty, nz, st, ptr);
}
void t_bind_buffer(GLenum t, GLuint b) { GLT("glBindBuffer 0x%x %u", t, b); ::glBindBuffer(t, b); }
void t_buffer_data(GLenum t, GLsizeiptr sz, const void* d, GLenum u) {
  GLT("glBufferData 0x%x size=%ld data=%p", t, (long)sz, d); ::glBufferData(t, sz, d, u);
}
void t_bind_attrib(GLuint p, GLuint i, const GLchar* n) {
  GLT("glBindAttribLocation prog=%u %u %s", p, i, n ? n : "?"); ::glBindAttribLocation(p, i, n);
}
void t_use_program(GLuint p) { GLT("glUseProgram %u", p); ::glUseProgram(p); }
void t_draw_arrays(GLenum m, GLint f, GLsizei c) {
  GLT("glDrawArrays mode=%u first=%d count=%d", m, f, c);
  ::glDrawArrays(m, f, c);
  GLT("glDrawArrays done err=0x%x", ::glGetError());
}
void t_draw_elements(GLenum m, GLsizei c, GLenum t, const void* i) {
  GLT("glDrawElements mode=%u count=%d type=0x%x idx=%p", m, c, t, i);
  ::glDrawElements(m, c, t, i);
  GLT("glDrawElements done err=0x%x", ::glGetError());
}
void t_clear(GLbitfield b) { GLT("glClear 0x%x", b); ::glClear(b); GLT("glClear done %d", 0); }
void t_bind_fb(GLenum t, GLuint f) { GLT("glBindFramebuffer %u", f); ytv_gl_bind_framebuffer(t, f); }
void t_fb_tex(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) {
  GLT("glFramebufferTexture2D att=0x%x tex=%u", a, tex);
  ::glFramebufferTexture2D(t, a, tt, tex, l);
}
GLenum t_check_fb(GLenum t) {
  GLenum r = ::glCheckFramebufferStatus(t);
  GLT("glCheckFramebufferStatus 0x%x", r);
  return r;
}
void t_tex_image(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b,
                 GLenum f, GLenum ty, const void* p) {
  GLT("glTexImage2D tgt=0x%x %dx%d ifmt=0x%x fmt=0x%x type=0x%x data=%p pending_err=0x%x", t, w, h, ifmt, f, ty, p, ::glGetError());
  ytv_gl_tex_image_2d(t, l, ifmt, w, h, b, f, ty, p);
  GLT("glTexImage2D done err=0x%x", ::glGetError());
}
void t_read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void* p) {
  GLT("glReadPixels %d,%d %dx%d", x, y, w, h);
  ytv_gl_read_pixels(x, y, w, h, f, t, p);
  GLT("glReadPixels done %d", 0);
}
void t_finish() { GLT("glFinish %d", 0); ::glFinish(); GLT("glFinish done %d", 0); }
void t_flush() { GLT("glFlush %d", 0); ::glFlush(); }
#endif


SbGlesInterface gl_api = {};
pthread_once_t once = PTHREAD_ONCE_INIT;

#define SET_EGL(field, fn) \
  egl_api.field = reinterpret_cast<decltype(egl_api.field)>(fn)
#define SET_GL(field, fn) \
  gl_api.field = reinterpret_cast<decltype(gl_api.field)>(fn)

// ---- Render-target diagnostics (verbose log only) --------------------------
void diag_fb_tex(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level) {
  static int seen[16] = {0};
  static int n = 0;
  uint16_t fmt = tex < 4096 ? g_diag_tex_fmt[tex] : 0;
  bool known = false;
  for (int i = 0; i < n; i++) if (seen[i] == fmt) known = true;
  if (!known && n < 16) {
    seen[n++] = fmt;
    char line[120];
    snprintf(line, sizeof(line), "YTV_GL_FBO_ATTACH tex=%u internal_format=0x%x attachment=0x%x\n",
             tex, (unsigned)fmt, (unsigned)attachment);
    log_line(line);
  }
  ::glFramebufferTexture2D(target, attachment, textarget, tex, level);
}
GLenum diag_check_fb(GLenum target) {
  GLenum r = ::glCheckFramebufferStatus(target);
  if (r != GL_FRAMEBUFFER_COMPLETE) {
    static int count = 0;
    if (count++ < 20) {
      char line[80];
      snprintf(line, sizeof(line), "YTV_GL_FBO_INCOMPLETE status=0x%x\n", (unsigned)r);
      log_line(line);
    }
  }
  return r;
}

// ---- Lightweight frame profiler (logged only with verbose.txt) -----------
inline uint64_t prof_now() { return sceKernelGetSystemTimeWide(); }
inline void prof_frame_mark(uint64_t t) {
  if (!g_prof_frame_first) {
    g_prof_frame_first = t;
    if (g_prof_last_swap_end) g_prof_idle_us += t - g_prof_last_swap_end;
  }
}
void prof_compile_shader(GLuint sh) {
  uint64_t t = prof_now(); ::glCompileShader(sh);
  g_hb_compile_us += prof_now() - t; g_hb_compiles++;
}
void prof_link_program(GLuint pr) {
  uint64_t t = prof_now(); ::glLinkProgram(pr);
  g_hb_link_us += prof_now() - t; g_hb_links++;
}
void prof_draw_arrays(GLenum m, GLint f, GLsizei c) {
  uint64_t t = prof_now(); prof_frame_mark(t); g_hb_draws++;
  if (g_startup_bg_active) ++g_startup_draws;
  ::glDrawArrays(m, f, c);
  g_prof_draw_us += prof_now() - t; g_prof_draws++;
}
// ---- v0.64: opaque black boxes behind text --------------------------------
// With the player open (end screen, suggestions, video title) the YouTube
// page gives each line of text an opaque black background. Cobalt draws them
// as solid rectangles (its a_position/a_color program, blending off, colour
// exactly 0,0,0,1, one quad per text line) together with the other opaque
// rectangles of the frame. Seen in Vita3K: the 960x544 page background, then
// 12 boxes of 14-21 px height under the titles and metadata lines.
// Those quads become degenerate triangles before the draw; the text itself
// (drawn afterwards from Cobalt's text atlas) is untouched. The vertex and
// index data are read from a CPU copy kept at upload time.
struct BufCopy { GLuint id; uint8_t* p; size_t n; };
BufCopy g_buf_copies[64];
int g_buf_next = 0;
BufCopy* buf_copy(GLuint id, bool create) {
  if (!id) return nullptr;
  for (auto& e : g_buf_copies) if (e.id == id) return &e;
  if (!create) return nullptr;
  BufCopy* e = &g_buf_copies[g_buf_next++ % 64];
  free(e->p);
  e->id = id; e->p = nullptr; e->n = 0;
  return e;
}
GLuint bound_buffer(GLenum target) {
  GLint b = 0;
  ::glGetIntegerv(target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING
                                                    : GL_ARRAY_BUFFER_BINDING, &b);
  return (GLuint)b;
}
// v0.64 diagnostic: dynamic/stream buffers used again in a later frame. With
// VitaGL scratch memory (disabled above) exactly these uses read or wrote
// another frame's data. Logged a few times, so the hardware log shows whether
// the page does it.
}  // namespace
extern "C" uint32_t vgl_framecount;
namespace {
struct DynBuf { GLuint id; uint32_t frame; };
DynBuf g_dyn_bufs[256];
int g_dyn_logged = 0;
DynBuf* dyn_buf(GLuint id, bool create) {
  if (!id) return nullptr;
  DynBuf* slot = &g_dyn_bufs[(id >> 4) & 255];
  if (slot->id == id) return slot;
  if (!create) return nullptr;
  slot->id = id;
  return slot;
}
void dyn_check(GLuint id, const char* what) {
  DynBuf* d = dyn_buf(id, false);
  if (!d || g_dyn_logged >= 6) return;
  const uint32_t age = vgl_framecount - d->frame;
  if (age < 3) return;
  g_dyn_logged++;
  char line[112];
  snprintf(line, sizeof(line), "YTV_GL_DYNAMIC_BUFFER_REUSE %s age=%u frames" "\n", what, (unsigned)age);
  log_line(line);
}
void ytv_gl_buffer_data(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
  {
    const GLuint id = bound_buffer(target);
    if (usage == GL_DYNAMIC_DRAW || usage == GL_STREAM_DRAW) {
      if (DynBuf* d = dyn_buf(id, true)) d->frame = vgl_framecount;
    } else if (DynBuf* d = dyn_buf(id, false)) {
      d->id = 0;
    }
  }
  if (BufCopy* e = buf_copy(bound_buffer(target), true)) {
    free(e->p);
    e->p = nullptr; e->n = 0;
    if (size > 0 && size <= (1 << 20)) {
      e->p = static_cast<uint8_t*>(malloc((size_t)size));
      if (e->p) {
        e->n = (size_t)size;
        if (data) memcpy(e->p, data, (size_t)size); else memset(e->p, 0, (size_t)size);
      }
    }
  }
  ::glBufferData(target, size, data, usage);
}
void ytv_gl_buffer_sub_data(GLenum target, GLintptr off, GLsizeiptr size, const void* data) {
  dyn_check(bound_buffer(target), "subdata");
  BufCopy* e = buf_copy(bound_buffer(target), false);
  if (e && e->p) {
    if (data && off >= 0 && (size_t)(off + size) <= e->n) memcpy(e->p + off, data, (size_t)size);
    else { free(e->p); e->p = nullptr; e->n = 0; }  // copy no longer valid
  }
  ::glBufferSubData(target, off, size, data);
}
// 0 unknown, 1 Cobalt's solid colour program, 2 anything else.
uint8_t g_prog_kind[1024];
GLint g_solid_pos[1024], g_solid_col[1024];
// Draws without the text-background quads; false = draw normally.
bool draw_without_text_boxes(GLenum mode, GLsizei count, GLenum type, const void* indices) {
  if (mode != GL_TRIANGLES || count < 6 || count % 6 || count > 6 * 512 ||
      type != GL_UNSIGNED_SHORT || ::glIsEnabled(GL_BLEND))
    return false;
  GLint pr = 0;
  ::glGetIntegerv(GL_CURRENT_PROGRAM, &pr);
  if (pr <= 0 || pr >= 1024) return false;
  if (!g_prog_kind[pr]) {
    const GLint lp = ::glGetAttribLocation((GLuint)pr, "a_position");
    const GLint lc = ::glGetAttribLocation((GLuint)pr, "a_color");
    const GLint lt = ::glGetAttribLocation((GLuint)pr, "a_texcoord");
    g_prog_kind[pr] = lp >= 0 && lc >= 0 && lt < 0 ? 1 : 2;
    g_solid_pos[pr] = lp; g_solid_col[pr] = lc;
  }
  if (g_prog_kind[pr] != 1) return false;
  GLint fb = 0;
  ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
  if (fb != 0 && (GLuint)fb != ytv_screen_fbo()) return false;  // offscreen targets untouched
  const GLuint lp = (GLuint)g_solid_pos[pr], lc = (GLuint)g_solid_col[pr];
  GLint pb = 0, ps = 0, pt = 0, pst = 0, cb = 0, cs = 0, ct = 0, cst = 0;
  void* po = nullptr;
  void* co = nullptr;
  ::glGetVertexAttribiv(lp, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &pb);
  ::glGetVertexAttribiv(lp, GL_VERTEX_ATTRIB_ARRAY_SIZE, &ps);
  ::glGetVertexAttribiv(lp, GL_VERTEX_ATTRIB_ARRAY_TYPE, &pt);
  ::glGetVertexAttribiv(lp, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &pst);
  ::glGetVertexAttribPointerv(lp, GL_VERTEX_ATTRIB_ARRAY_POINTER, &po);
  ::glGetVertexAttribiv(lc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &cb);
  ::glGetVertexAttribiv(lc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &cs);
  ::glGetVertexAttribiv(lc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &ct);
  ::glGetVertexAttribiv(lc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &cst);
  ::glGetVertexAttribPointerv(lc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &co);
  if (pt != GL_FLOAT || ps < 2 || ct != GL_UNSIGNED_BYTE || cs != 4) return false;
  const GLuint eb = bound_buffer(GL_ELEMENT_ARRAY_BUFFER);
  BufCopy* vc = buf_copy((GLuint)pb, false);
  BufCopy* cc = buf_copy((GLuint)cb, false);
  BufCopy* ic = buf_copy(eb, false);
  if (!vc || !vc->p || !cc || !cc->p || !ic || !ic->p) return false;
  const size_t ioff = (size_t)(uintptr_t)indices;
  if (ioff + (size_t)count * 2 > ic->n) return false;
  const size_t pstride = pst ? (size_t)pst : (size_t)ps * 4, cstride = cst ? (size_t)cst : 4;
  static uint16_t idx[6 * 512];
  memcpy(idx, ic->p + ioff, (size_t)count * 2);
  int removed = 0;
  for (int q = 0; q < count; q += 6) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    bool black = true;
    for (int k = 0; k < 6 && black; k++) {
      const size_t vo = (size_t)(uintptr_t)po + idx[q + k] * pstride;
      const size_t ko = (size_t)(uintptr_t)co + idx[q + k] * cstride;
      if (vo + 8 > vc->n || ko + 4 > cc->n) return false;
      float v[2];
      memcpy(v, vc->p + vo, sizeof(v));
      const uint8_t* c = cc->p + ko;
      black = c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 255;
      x0 = v[0] < x0 ? v[0] : x0; x1 = v[0] > x1 ? v[0] : x1;
      y0 = v[1] < y0 ? v[1] : y0; y1 = v[1] > y1 ? v[1] : y1;
    }
    const float h = y1 - y0, w = x1 - x0;
    if (black && h >= 8.f && h <= 40.f && w >= 4.f && w <= 900.f) {
      for (int k = 1; k < 6; k++) idx[q + k] = idx[q];  // degenerate
      removed++;
    }
  }
  if (!removed) return false;
  static int logged = 0;
  if (logged < 3) {
    logged++;
    char line[96];
    snprintf(line, sizeof(line), "YTV_LIFECYCLE_TEXT_BOXES removed=%d of %d quads\n", removed, count / 6);
    log_line(line);
  }
  ::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  ::glDrawElements(mode, count, type, idx);
  ::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eb);
  return true;
}
void prof_draw_elements(GLenum m, GLsizei c, GLenum ty, const void* i) {
  uint64_t t = prof_now(); prof_frame_mark(t); g_hb_draws++;
  if (g_startup_bg_active) ++g_startup_draws;
  if (g_dyn_logged < 6) {
    dyn_check(bound_buffer(GL_ELEMENT_ARRAY_BUFFER), "draw_indices");
    dyn_check(bound_buffer(GL_ARRAY_BUFFER), "draw_vertices");
  }
  if (!draw_without_text_boxes(m, c, ty, i)) ::glDrawElements(m, c, ty, i);
  g_prof_draw_us += prof_now() - t; g_prof_draws++;
}
SbEglInterface egl_api = {};

#if defined(YTV_GL_TRACE)
// Checks glGetError() after every GLES call (diagnostic builds only) and logs
// the first calls that raise an error. The error is consumed by the check.
volatile int g_gl_err_logged = 0;
template <typename T, T Fn> struct Checked;
template <typename R, typename... A, R (*Fn)(A...)>
struct Checked<R (*)(A...), Fn> {
  static const char* name;
  static R call(A... a) {
    if constexpr (std::is_void<R>::value) {
      Fn(a...);
      report(first(a...));
    } else {
      R r = Fn(a...);
      report(first(a...));
      return r;
    }
  }
  static unsigned first() { return 0; }
  template <typename F, typename... Rest>
  static unsigned first(F f, Rest...) {
    if constexpr (std::is_integral<F>::value) return (unsigned)f; else return 0;
  }
  static void report(unsigned arg0) {
    GLenum e = ::glGetError();
    if (e != GL_NO_ERROR && __sync_add_and_fetch(&g_gl_err_logged, 1) <= 200) {
      char l[140];
      snprintf(l, sizeof(l), "YTV_GL_ERROR %s(0x%x) -> 0x%x tid=%d\n", name, arg0, e,
               (int)sceKernelGetThreadId());
      log_line(l);
    }
  }
};
template <typename R, typename... A, R (*Fn)(A...)>
const char* Checked<R (*)(A...), Fn>::name = "?";
#define CHECKED(fn) Checked<decltype(&fn), &fn>
void install_checked_gl() {
  CHECKED(::glActiveTexture)::name = "glActiveTexture"; SET_GL(glActiveTexture, &CHECKED(::glActiveTexture)::call);
  CHECKED(::glAttachShader)::name = "glAttachShader"; SET_GL(glAttachShader, &CHECKED(::glAttachShader)::call);
  CHECKED(::glBindAttribLocation)::name = "glBindAttribLocation"; SET_GL(glBindAttribLocation, &CHECKED(::glBindAttribLocation)::call);
  CHECKED(::glBindBuffer)::name = "glBindBuffer"; SET_GL(glBindBuffer, &CHECKED(::glBindBuffer)::call);
  CHECKED(ytv_gl_bind_framebuffer)::name = "glBindFramebuffer"; SET_GL(glBindFramebuffer, &CHECKED(ytv_gl_bind_framebuffer)::call);
  CHECKED(ytv_gl_bind_renderbuffer)::name = "glBindRenderbuffer"; SET_GL(glBindRenderbuffer, &CHECKED(ytv_gl_bind_renderbuffer)::call);
  CHECKED(::glBindTexture)::name = "glBindTexture"; SET_GL(glBindTexture, &CHECKED(::glBindTexture)::call);
  CHECKED(::glBlendEquation)::name = "glBlendEquation"; SET_GL(glBlendEquation, &CHECKED(::glBlendEquation)::call);
  CHECKED(::glBlendEquationSeparate)::name = "glBlendEquationSeparate"; SET_GL(glBlendEquationSeparate, &CHECKED(::glBlendEquationSeparate)::call);
  CHECKED(::glBlendFunc)::name = "glBlendFunc"; SET_GL(glBlendFunc, &CHECKED(::glBlendFunc)::call);
  CHECKED(::glBlendFuncSeparate)::name = "glBlendFuncSeparate"; SET_GL(glBlendFuncSeparate, &CHECKED(::glBlendFuncSeparate)::call);
  CHECKED(::glBufferData)::name = "glBufferData"; SET_GL(glBufferData, &CHECKED(::glBufferData)::call);
  CHECKED(::glBufferSubData)::name = "glBufferSubData"; SET_GL(glBufferSubData, &CHECKED(::glBufferSubData)::call);
  CHECKED(::glCheckFramebufferStatus)::name = "glCheckFramebufferStatus"; SET_GL(glCheckFramebufferStatus, &CHECKED(::glCheckFramebufferStatus)::call);
  CHECKED(::glClear)::name = "glClear"; SET_GL(glClear, &CHECKED(::glClear)::call);
  CHECKED(::glClearColor)::name = "glClearColor"; SET_GL(glClearColor, &CHECKED(::glClearColor)::call);
  CHECKED(::glClearDepthf)::name = "glClearDepthf"; SET_GL(glClearDepthf, &CHECKED(::glClearDepthf)::call);
  CHECKED(::glClearStencil)::name = "glClearStencil"; SET_GL(glClearStencil, &CHECKED(::glClearStencil)::call);
  CHECKED(::glColorMask)::name = "glColorMask"; SET_GL(glColorMask, &CHECKED(::glColorMask)::call);
  CHECKED(::glCompileShader)::name = "glCompileShader"; SET_GL(glCompileShader, &CHECKED(::glCompileShader)::call);
  CHECKED(::glCompressedTexImage2D)::name = "glCompressedTexImage2D"; SET_GL(glCompressedTexImage2D, &CHECKED(::glCompressedTexImage2D)::call);
  CHECKED(::glCopyTexImage2D)::name = "glCopyTexImage2D"; SET_GL(glCopyTexImage2D, &CHECKED(::glCopyTexImage2D)::call);
  CHECKED(::glCopyTexSubImage2D)::name = "glCopyTexSubImage2D"; SET_GL(glCopyTexSubImage2D, &CHECKED(::glCopyTexSubImage2D)::call);
  CHECKED(::glCreateProgram)::name = "glCreateProgram"; SET_GL(glCreateProgram, &CHECKED(::glCreateProgram)::call);
  CHECKED(::glCreateShader)::name = "glCreateShader"; SET_GL(glCreateShader, &CHECKED(::glCreateShader)::call);
  CHECKED(::glCullFace)::name = "glCullFace"; SET_GL(glCullFace, &CHECKED(::glCullFace)::call);
  CHECKED(::glDeleteBuffers)::name = "glDeleteBuffers"; SET_GL(glDeleteBuffers, &CHECKED(::glDeleteBuffers)::call);
  CHECKED(::glDeleteFramebuffers)::name = "glDeleteFramebuffers"; SET_GL(glDeleteFramebuffers, &CHECKED(::glDeleteFramebuffers)::call);
  CHECKED(::glDeleteProgram)::name = "glDeleteProgram"; SET_GL(glDeleteProgram, &CHECKED(::glDeleteProgram)::call);
  CHECKED(ytv_gl_delete_renderbuffers)::name = "glDeleteRenderbuffers"; SET_GL(glDeleteRenderbuffers, &CHECKED(ytv_gl_delete_renderbuffers)::call);
  CHECKED(::glDeleteShader)::name = "glDeleteShader"; SET_GL(glDeleteShader, &CHECKED(::glDeleteShader)::call);
  CHECKED(ytv_gl_delete_textures)::name = "glDeleteTextures"; SET_GL(glDeleteTextures, &CHECKED(ytv_gl_delete_textures)::call);
  CHECKED(::glDepthFunc)::name = "glDepthFunc"; SET_GL(glDepthFunc, &CHECKED(::glDepthFunc)::call);
  CHECKED(::glDepthMask)::name = "glDepthMask"; SET_GL(glDepthMask, &CHECKED(::glDepthMask)::call);
  CHECKED(::glDepthRangef)::name = "glDepthRangef"; SET_GL(glDepthRangef, &CHECKED(::glDepthRangef)::call);
  CHECKED(ytv_gl_disable)::name = "glDisable"; SET_GL(glDisable, &CHECKED(ytv_gl_disable)::call);
  CHECKED(::glDisableVertexAttribArray)::name = "glDisableVertexAttribArray"; SET_GL(glDisableVertexAttribArray, &CHECKED(::glDisableVertexAttribArray)::call);
  CHECKED(::glDrawArrays)::name = "glDrawArrays"; SET_GL(glDrawArrays, &CHECKED(::glDrawArrays)::call);
  CHECKED(::glDrawElements)::name = "glDrawElements"; SET_GL(glDrawElements, &CHECKED(::glDrawElements)::call);
  CHECKED(ytv_gl_enable)::name = "glEnable"; SET_GL(glEnable, &CHECKED(ytv_gl_enable)::call);
  CHECKED(::glEnableVertexAttribArray)::name = "glEnableVertexAttribArray"; SET_GL(glEnableVertexAttribArray, &CHECKED(::glEnableVertexAttribArray)::call);
  CHECKED(::glFinish)::name = "glFinish"; SET_GL(glFinish, &CHECKED(::glFinish)::call);
  CHECKED(::glFlush)::name = "glFlush"; SET_GL(glFlush, &CHECKED(::glFlush)::call);
  CHECKED(::glFramebufferRenderbuffer)::name = "glFramebufferRenderbuffer"; SET_GL(glFramebufferRenderbuffer, &CHECKED(::glFramebufferRenderbuffer)::call);
  CHECKED(::glFramebufferTexture2D)::name = "glFramebufferTexture2D"; SET_GL(glFramebufferTexture2D, &CHECKED(::glFramebufferTexture2D)::call);
  CHECKED(::glFrontFace)::name = "glFrontFace"; SET_GL(glFrontFace, &CHECKED(::glFrontFace)::call);
  CHECKED(::glGenBuffers)::name = "glGenBuffers"; SET_GL(glGenBuffers, &CHECKED(::glGenBuffers)::call);
  CHECKED(::glGenerateMipmap)::name = "glGenerateMipmap"; SET_GL(glGenerateMipmap, &CHECKED(::glGenerateMipmap)::call);
  CHECKED(::glGenFramebuffers)::name = "glGenFramebuffers"; SET_GL(glGenFramebuffers, &CHECKED(::glGenFramebuffers)::call);
  CHECKED(::glGenRenderbuffers)::name = "glGenRenderbuffers"; SET_GL(glGenRenderbuffers, &CHECKED(::glGenRenderbuffers)::call);
  CHECKED(::glGenTextures)::name = "glGenTextures"; SET_GL(glGenTextures, &CHECKED(::glGenTextures)::call);
  CHECKED(::glGetActiveAttrib)::name = "glGetActiveAttrib"; SET_GL(glGetActiveAttrib, &CHECKED(::glGetActiveAttrib)::call);
  CHECKED(::glGetActiveUniform)::name = "glGetActiveUniform"; SET_GL(glGetActiveUniform, &CHECKED(::glGetActiveUniform)::call);
  CHECKED(::glGetAttachedShaders)::name = "glGetAttachedShaders"; SET_GL(glGetAttachedShaders, &CHECKED(::glGetAttachedShaders)::call);
  CHECKED(::glGetAttribLocation)::name = "glGetAttribLocation"; SET_GL(glGetAttribLocation, &CHECKED(::glGetAttribLocation)::call);
  CHECKED(::glGetBooleanv)::name = "glGetBooleanv"; SET_GL(glGetBooleanv, &CHECKED(::glGetBooleanv)::call);
  CHECKED(::glGetBufferParameteriv)::name = "glGetBufferParameteriv"; SET_GL(glGetBufferParameteriv, &CHECKED(::glGetBufferParameteriv)::call);
  CHECKED(::glGetFloatv)::name = "glGetFloatv"; SET_GL(glGetFloatv, &CHECKED(::glGetFloatv)::call);
  CHECKED(::glGetFramebufferAttachmentParameteriv)::name = "glGetFramebufferAttachmentParameteriv"; SET_GL(glGetFramebufferAttachmentParameteriv, &CHECKED(::glGetFramebufferAttachmentParameteriv)::call);
  CHECKED(ytv_gl_get_integerv)::name = "glGetIntegerv"; SET_GL(glGetIntegerv, &CHECKED(ytv_gl_get_integerv)::call);
  CHECKED(::glGetProgramiv)::name = "glGetProgramiv"; SET_GL(glGetProgramiv, &CHECKED(::glGetProgramiv)::call);
  CHECKED(::glGetProgramInfoLog)::name = "glGetProgramInfoLog"; SET_GL(glGetProgramInfoLog, &CHECKED(::glGetProgramInfoLog)::call);
  CHECKED(::glGetShaderiv)::name = "glGetShaderiv"; SET_GL(glGetShaderiv, &CHECKED(::glGetShaderiv)::call);
  CHECKED(::glGetShaderInfoLog)::name = "glGetShaderInfoLog"; SET_GL(glGetShaderInfoLog, &CHECKED(::glGetShaderInfoLog)::call);
  CHECKED(::glGetShaderSource)::name = "glGetShaderSource"; SET_GL(glGetShaderSource, &CHECKED(::glGetShaderSource)::call);
  CHECKED(::glGetString)::name = "glGetString"; SET_GL(glGetString, &CHECKED(::glGetString)::call);
  CHECKED(::glGetUniformLocation)::name = "glGetUniformLocation"; SET_GL(glGetUniformLocation, &CHECKED(::glGetUniformLocation)::call);
  CHECKED(::glGetVertexAttribfv)::name = "glGetVertexAttribfv"; SET_GL(glGetVertexAttribfv, &CHECKED(::glGetVertexAttribfv)::call);
  CHECKED(::glGetVertexAttribiv)::name = "glGetVertexAttribiv"; SET_GL(glGetVertexAttribiv, &CHECKED(::glGetVertexAttribiv)::call);
  CHECKED(::glGetVertexAttribPointerv)::name = "glGetVertexAttribPointerv"; SET_GL(glGetVertexAttribPointerv, &CHECKED(::glGetVertexAttribPointerv)::call);
  CHECKED(::glHint)::name = "glHint"; SET_GL(glHint, &CHECKED(::glHint)::call);
  CHECKED(ytv_gl_is_enabled)::name = "glIsEnabled"; SET_GL(glIsEnabled, &CHECKED(ytv_gl_is_enabled)::call);
  CHECKED(::glIsFramebuffer)::name = "glIsFramebuffer"; SET_GL(glIsFramebuffer, &CHECKED(::glIsFramebuffer)::call);
  CHECKED(::glIsProgram)::name = "glIsProgram"; SET_GL(glIsProgram, &CHECKED(::glIsProgram)::call);
  CHECKED(::glIsRenderbuffer)::name = "glIsRenderbuffer"; SET_GL(glIsRenderbuffer, &CHECKED(::glIsRenderbuffer)::call);
  CHECKED(::glIsTexture)::name = "glIsTexture"; SET_GL(glIsTexture, &CHECKED(::glIsTexture)::call);
  CHECKED(::glLineWidth)::name = "glLineWidth"; SET_GL(glLineWidth, &CHECKED(::glLineWidth)::call);
  CHECKED(::glLinkProgram)::name = "glLinkProgram"; SET_GL(glLinkProgram, &CHECKED(::glLinkProgram)::call);
  CHECKED(ytv_gl_pixel_storei)::name = "glPixelStorei"; SET_GL(glPixelStorei, &CHECKED(ytv_gl_pixel_storei)::call);
  CHECKED(::glPolygonOffset)::name = "glPolygonOffset"; SET_GL(glPolygonOffset, &CHECKED(::glPolygonOffset)::call);
  CHECKED(ytv_gl_read_pixels)::name = "glReadPixels"; SET_GL(glReadPixels, &CHECKED(ytv_gl_read_pixels)::call);
  CHECKED(::glReleaseShaderCompiler)::name = "glReleaseShaderCompiler"; SET_GL(glReleaseShaderCompiler, &CHECKED(::glReleaseShaderCompiler)::call);
  CHECKED(ytv_gl_renderbuffer_storage)::name = "glRenderbufferStorage"; SET_GL(glRenderbufferStorage, &CHECKED(ytv_gl_renderbuffer_storage)::call);
  CHECKED(ytv_gl_scissor)::name = "glScissor"; SET_GL(glScissor, &CHECKED(ytv_gl_scissor)::call);
  CHECKED(::glShaderBinary)::name = "glShaderBinary"; SET_GL(glShaderBinary, &CHECKED(::glShaderBinary)::call);
  CHECKED(ytv_gl_shader_source)::name = "glShaderSource"; SET_GL(glShaderSource, &CHECKED(ytv_gl_shader_source)::call);
  CHECKED(::glStencilFunc)::name = "glStencilFunc"; SET_GL(glStencilFunc, &CHECKED(::glStencilFunc)::call);
  CHECKED(::glStencilFuncSeparate)::name = "glStencilFuncSeparate"; SET_GL(glStencilFuncSeparate, &CHECKED(::glStencilFuncSeparate)::call);
  CHECKED(::glStencilMask)::name = "glStencilMask"; SET_GL(glStencilMask, &CHECKED(::glStencilMask)::call);
  CHECKED(::glStencilMaskSeparate)::name = "glStencilMaskSeparate"; SET_GL(glStencilMaskSeparate, &CHECKED(::glStencilMaskSeparate)::call);
  CHECKED(::glStencilOp)::name = "glStencilOp"; SET_GL(glStencilOp, &CHECKED(::glStencilOp)::call);
  CHECKED(::glStencilOpSeparate)::name = "glStencilOpSeparate"; SET_GL(glStencilOpSeparate, &CHECKED(::glStencilOpSeparate)::call);
  CHECKED(ytv_gl_tex_image_2d)::name = "glTexImage2D"; SET_GL(glTexImage2D, &CHECKED(ytv_gl_tex_image_2d)::call);
  CHECKED(ytv_gl_tex_parameterf)::name = "glTexParameterf"; SET_GL(glTexParameterf, &CHECKED(ytv_gl_tex_parameterf)::call);
  CHECKED(ytv_gl_tex_parameteri)::name = "glTexParameteri"; SET_GL(glTexParameteri, &CHECKED(ytv_gl_tex_parameteri)::call);
  CHECKED(ytv_gl_tex_parameteriv)::name = "glTexParameteriv"; SET_GL(glTexParameteriv, &CHECKED(ytv_gl_tex_parameteriv)::call);
  CHECKED(ytv_gl_tex_sub_image_2d)::name = "glTexSubImage2D"; SET_GL(glTexSubImage2D, &CHECKED(ytv_gl_tex_sub_image_2d)::call);
  CHECKED(::glUniform1f)::name = "glUniform1f"; SET_GL(glUniform1f, &CHECKED(::glUniform1f)::call);
  CHECKED(::glUniform1fv)::name = "glUniform1fv"; SET_GL(glUniform1fv, &CHECKED(::glUniform1fv)::call);
  CHECKED(::glUniform1i)::name = "glUniform1i"; SET_GL(glUniform1i, &CHECKED(::glUniform1i)::call);
  CHECKED(::glUniform1iv)::name = "glUniform1iv"; SET_GL(glUniform1iv, &CHECKED(::glUniform1iv)::call);
  CHECKED(::glUniform2f)::name = "glUniform2f"; SET_GL(glUniform2f, &CHECKED(::glUniform2f)::call);
  CHECKED(::glUniform2fv)::name = "glUniform2fv"; SET_GL(glUniform2fv, &CHECKED(::glUniform2fv)::call);
  CHECKED(::glUniform2i)::name = "glUniform2i"; SET_GL(glUniform2i, &CHECKED(::glUniform2i)::call);
  CHECKED(::glUniform2iv)::name = "glUniform2iv"; SET_GL(glUniform2iv, &CHECKED(::glUniform2iv)::call);
  CHECKED(::glUniform3f)::name = "glUniform3f"; SET_GL(glUniform3f, &CHECKED(::glUniform3f)::call);
  CHECKED(::glUniform3fv)::name = "glUniform3fv"; SET_GL(glUniform3fv, &CHECKED(::glUniform3fv)::call);
  CHECKED(::glUniform3i)::name = "glUniform3i"; SET_GL(glUniform3i, &CHECKED(::glUniform3i)::call);
  CHECKED(::glUniform3iv)::name = "glUniform3iv"; SET_GL(glUniform3iv, &CHECKED(::glUniform3iv)::call);
  CHECKED(::glUniform4f)::name = "glUniform4f"; SET_GL(glUniform4f, &CHECKED(::glUniform4f)::call);
  CHECKED(::glUniform4fv)::name = "glUniform4fv"; SET_GL(glUniform4fv, &CHECKED(::glUniform4fv)::call);
  CHECKED(::glUniform4i)::name = "glUniform4i"; SET_GL(glUniform4i, &CHECKED(::glUniform4i)::call);
  CHECKED(::glUniform4iv)::name = "glUniform4iv"; SET_GL(glUniform4iv, &CHECKED(::glUniform4iv)::call);
  CHECKED(::glUniformMatrix2fv)::name = "glUniformMatrix2fv"; SET_GL(glUniformMatrix2fv, &CHECKED(::glUniformMatrix2fv)::call);
  CHECKED(::glUniformMatrix3fv)::name = "glUniformMatrix3fv"; SET_GL(glUniformMatrix3fv, &CHECKED(::glUniformMatrix3fv)::call);
  CHECKED(::glUniformMatrix4fv)::name = "glUniformMatrix4fv"; SET_GL(glUniformMatrix4fv, &CHECKED(::glUniformMatrix4fv)::call);
  CHECKED(::glUseProgram)::name = "glUseProgram"; SET_GL(glUseProgram, &CHECKED(::glUseProgram)::call);
  CHECKED(::glVertexAttrib1f)::name = "glVertexAttrib1f"; SET_GL(glVertexAttrib1f, &CHECKED(::glVertexAttrib1f)::call);
  CHECKED(::glVertexAttrib1fv)::name = "glVertexAttrib1fv"; SET_GL(glVertexAttrib1fv, &CHECKED(::glVertexAttrib1fv)::call);
  CHECKED(::glVertexAttrib2f)::name = "glVertexAttrib2f"; SET_GL(glVertexAttrib2f, &CHECKED(::glVertexAttrib2f)::call);
  CHECKED(::glVertexAttrib2fv)::name = "glVertexAttrib2fv"; SET_GL(glVertexAttrib2fv, &CHECKED(::glVertexAttrib2fv)::call);
  CHECKED(::glVertexAttrib3f)::name = "glVertexAttrib3f"; SET_GL(glVertexAttrib3f, &CHECKED(::glVertexAttrib3f)::call);
  CHECKED(::glVertexAttrib3fv)::name = "glVertexAttrib3fv"; SET_GL(glVertexAttrib3fv, &CHECKED(::glVertexAttrib3fv)::call);
  CHECKED(::glVertexAttrib4f)::name = "glVertexAttrib4f"; SET_GL(glVertexAttrib4f, &CHECKED(::glVertexAttrib4f)::call);
  CHECKED(::glVertexAttrib4fv)::name = "glVertexAttrib4fv"; SET_GL(glVertexAttrib4fv, &CHECKED(::glVertexAttrib4fv)::call);
  CHECKED(::glVertexAttribPointer)::name = "glVertexAttribPointer"; SET_GL(glVertexAttribPointer, &CHECKED(::glVertexAttribPointer)::call);
  CHECKED(ytv_gl_viewport)::name = "glViewport"; SET_GL(glViewport, &CHECKED(ytv_gl_viewport)::call);
  CHECKED(::glReadBuffer)::name = "glReadBuffer"; SET_GL(glReadBuffer, &CHECKED(::glReadBuffer)::call);
  CHECKED(::glDrawRangeElements)::name = "glDrawRangeElements"; SET_GL(glDrawRangeElements, &CHECKED(::glDrawRangeElements)::call);
  CHECKED(::glGenQueries)::name = "glGenQueries"; SET_GL(glGenQueries, &CHECKED(::glGenQueries)::call);
  CHECKED(::glDeleteQueries)::name = "glDeleteQueries"; SET_GL(glDeleteQueries, &CHECKED(::glDeleteQueries)::call);
  CHECKED(::glBeginQuery)::name = "glBeginQuery"; SET_GL(glBeginQuery, &CHECKED(::glBeginQuery)::call);
  CHECKED(::glEndQuery)::name = "glEndQuery"; SET_GL(glEndQuery, &CHECKED(::glEndQuery)::call);
  CHECKED(::glUnmapBuffer)::name = "glUnmapBuffer"; SET_GL(glUnmapBuffer, &CHECKED(::glUnmapBuffer)::call);
  CHECKED(::glBlitFramebuffer)::name = "glBlitFramebuffer"; SET_GL(glBlitFramebuffer, &CHECKED(::glBlitFramebuffer)::call);
  CHECKED(::glMapBufferRange)::name = "glMapBufferRange"; SET_GL(glMapBufferRange, &CHECKED(::glMapBufferRange)::call);
  CHECKED(::glFlushMappedBufferRange)::name = "glFlushMappedBufferRange"; SET_GL(glFlushMappedBufferRange, &CHECKED(::glFlushMappedBufferRange)::call);
  CHECKED(::glBindVertexArray)::name = "glBindVertexArray"; SET_GL(glBindVertexArray, &CHECKED(::glBindVertexArray)::call);
  CHECKED(::glDeleteVertexArrays)::name = "glDeleteVertexArrays"; SET_GL(glDeleteVertexArrays, &CHECKED(::glDeleteVertexArrays)::call);
  CHECKED(::glGenVertexArrays)::name = "glGenVertexArrays"; SET_GL(glGenVertexArrays, &CHECKED(::glGenVertexArrays)::call);
  CHECKED(::glBindBufferRange)::name = "glBindBufferRange"; SET_GL(glBindBufferRange, &CHECKED(::glBindBufferRange)::call);
  CHECKED(::glBindBufferBase)::name = "glBindBufferBase"; SET_GL(glBindBufferBase, &CHECKED(::glBindBufferBase)::call);
  CHECKED(::glGetStringi)::name = "glGetStringi"; SET_GL(glGetStringi, &CHECKED(::glGetStringi)::call);
  CHECKED(::glGetUniformBlockIndex)::name = "glGetUniformBlockIndex"; SET_GL(glGetUniformBlockIndex, &CHECKED(::glGetUniformBlockIndex)::call);
  CHECKED(::glUniformBlockBinding)::name = "glUniformBlockBinding"; SET_GL(glUniformBlockBinding, &CHECKED(::glUniformBlockBinding)::call);
  CHECKED(::glDrawArraysInstanced)::name = "glDrawArraysInstanced"; SET_GL(glDrawArraysInstanced, &CHECKED(::glDrawArraysInstanced)::call);
  CHECKED(::glDrawElementsInstanced)::name = "glDrawElementsInstanced"; SET_GL(glDrawElementsInstanced, &CHECKED(::glDrawElementsInstanced)::call);
  CHECKED(::glGenSamplers)::name = "glGenSamplers"; SET_GL(glGenSamplers, &CHECKED(::glGenSamplers)::call);
  CHECKED(::glDeleteSamplers)::name = "glDeleteSamplers"; SET_GL(glDeleteSamplers, &CHECKED(::glDeleteSamplers)::call);
  CHECKED(::glBindSampler)::name = "glBindSampler"; SET_GL(glBindSampler, &CHECKED(::glBindSampler)::call);
  CHECKED(::glSamplerParameteri)::name = "glSamplerParameteri"; SET_GL(glSamplerParameteri, &CHECKED(::glSamplerParameteri)::call);
  CHECKED(::glSamplerParameterf)::name = "glSamplerParameterf"; SET_GL(glSamplerParameterf, &CHECKED(::glSamplerParameterf)::call);
  CHECKED(::glVertexAttribDivisor)::name = "glVertexAttribDivisor"; SET_GL(glVertexAttribDivisor, &CHECKED(::glVertexAttribDivisor)::call);
  CHECKED(::glGetProgramBinary)::name = "glGetProgramBinary"; SET_GL(glGetProgramBinary, &CHECKED(::glGetProgramBinary)::call);
  CHECKED(::glProgramBinary)::name = "glProgramBinary"; SET_GL(glProgramBinary, &CHECKED(::glProgramBinary)::call);
  CHECKED(ytv_gl_blend_color)::name = "glBlendColor"; SET_GL(glBlendColor, &CHECKED(ytv_gl_blend_color)::call);
  CHECKED(ytv_gl_compressed_tex_sub_image_2d)::name = "glCompressedTexSubImage2D"; SET_GL(glCompressedTexSubImage2D, &CHECKED(ytv_gl_compressed_tex_sub_image_2d)::call);
  CHECKED(ytv_gl_detach_shader)::name = "glDetachShader"; SET_GL(glDetachShader, &CHECKED(ytv_gl_detach_shader)::call);
  CHECKED(ytv_gl_get_renderbuffer_parameteriv)::name = "glGetRenderbufferParameteriv"; SET_GL(glGetRenderbufferParameteriv, &CHECKED(ytv_gl_get_renderbuffer_parameteriv)::call);
  CHECKED(ytv_gl_get_shader_precision_format)::name = "glGetShaderPrecisionFormat"; SET_GL(glGetShaderPrecisionFormat, &CHECKED(ytv_gl_get_shader_precision_format)::call);
  CHECKED(ytv_gl_get_tex_parameterfv)::name = "glGetTexParameterfv"; SET_GL(glGetTexParameterfv, &CHECKED(ytv_gl_get_tex_parameterfv)::call);
  CHECKED(ytv_gl_get_tex_parameteriv)::name = "glGetTexParameteriv"; SET_GL(glGetTexParameteriv, &CHECKED(ytv_gl_get_tex_parameteriv)::call);
  CHECKED(ytv_gl_is_buffer)::name = "glIsBuffer"; SET_GL(glIsBuffer, &CHECKED(ytv_gl_is_buffer)::call);
  CHECKED(ytv_gl_is_shader)::name = "glIsShader"; SET_GL(glIsShader, &CHECKED(ytv_gl_is_shader)::call);
  CHECKED(ytv_gl_sample_coverage)::name = "glSampleCoverage"; SET_GL(glSampleCoverage, &CHECKED(ytv_gl_sample_coverage)::call);
  CHECKED(ytv_gl_tex_parameterfv)::name = "glTexParameterfv"; SET_GL(glTexParameterfv, &CHECKED(ytv_gl_tex_parameterfv)::call);
  CHECKED(ytv_gl_validate_program)::name = "glValidateProgram"; SET_GL(glValidateProgram, &CHECKED(ytv_gl_validate_program)::call);
}
#endif

void set_interfaces() {
  SET_EGL(eglChooseConfig, &::eglChooseConfig);
  SET_EGL(eglCopyBuffers, &ytv_egl_copy_buffers);
  SET_EGL(eglCreateContext, &ytv_egl_create_context);
  SET_EGL(eglCreatePbufferSurface, &ytv_egl_create_pbuffer);
  SET_EGL(eglCreatePixmapSurface, &ytv_egl_create_pixmap_surface);
  egl_api.eglCreateWindowSurface = &create_window;
  SET_EGL(eglDestroyContext, &ytv_egl_destroy_context);
  SET_EGL(eglDestroySurface, &ytv_egl_destroy_surface);
  SET_EGL(eglGetConfigAttrib, &::eglGetConfigAttrib);
  SET_EGL(eglGetConfigs, &::eglGetConfigs);
  SET_EGL(eglGetCurrentDisplay, &ytv_egl_get_current_display);
  SET_EGL(eglGetCurrentSurface, &ytv_egl_get_current_surface);
  egl_api.eglGetDisplay = &get_display;
  SET_EGL(eglGetError, &ytv_egl_get_error);
  SET_EGL(eglGetProcAddress, &ytv_egl_get_proc_address);
  SET_EGL(eglInitialize, &ytv_egl_initialize);
  SET_EGL(eglMakeCurrent, &ytv_egl_make_current);
  SET_EGL(eglQueryContext, &::eglQueryContext);
  SET_EGL(eglQueryString, &ytv_egl_query_string);
  SET_EGL(eglQuerySurface, &ytv_egl_query_surface);
  SET_EGL(eglSwapBuffers, &ytv_egl_swap_buffers);
  SET_EGL(eglTerminate, &ytv_egl_terminate);
  SET_EGL(eglWaitGL, &ytv_egl_wait_client);
  SET_EGL(eglWaitNative, &ytv_egl_wait_native);
  SET_EGL(eglBindTexImage, &ytv_egl_bind_tex_image);
  SET_EGL(eglReleaseTexImage, &ytv_egl_release_tex_image);
  SET_EGL(eglSurfaceAttrib, &ytv_egl_surface_attrib);
  SET_EGL(eglSwapInterval, &::eglSwapInterval);
  SET_EGL(eglBindAPI, &::eglBindAPI);
  SET_EGL(eglQueryAPI, &::eglQueryAPI);
  SET_EGL(eglCreatePbufferFromClientBuffer,
          &ytv_egl_create_pbuffer_from_client_buffer);
  SET_EGL(eglReleaseThread, &ytv_egl_release_thread);
  SET_EGL(eglWaitClient, &ytv_egl_wait_client);
  SET_EGL(eglGetCurrentContext, &ytv_egl_get_current_context);
  SET_EGL(eglCreateSync, &ytv_egl_create_sync);
  SET_EGL(eglDestroySync, &ytv_egl_destroy_sync);
  SET_EGL(eglClientWaitSync, &ytv_egl_client_wait_sync);
  SET_EGL(eglGetSyncAttrib, &ytv_egl_get_sync_attrib);
  SET_EGL(eglCreateImage, &ytv_egl_create_image);
  SET_EGL(eglDestroyImage, &ytv_egl_destroy_image);
  SET_EGL(eglGetPlatformDisplay, &ytv_egl_get_platform_display);
  SET_EGL(eglCreatePlatformWindowSurface,
          &ytv_egl_create_platform_window_surface);
  SET_EGL(eglCreatePlatformPixmapSurface,
          &ytv_egl_create_platform_pixmap_surface);
  SET_EGL(eglWaitSync, &ytv_egl_wait_sync);
  SET_GL(glActiveTexture, &::glActiveTexture);
  SET_GL(glAttachShader, &::glAttachShader);
  SET_GL(glBindAttribLocation, &::glBindAttribLocation);
  SET_GL(glBindBuffer, &::glBindBuffer);
  SET_GL(glBindFramebuffer, &ytv_gl_bind_framebuffer);
  SET_GL(glBindRenderbuffer, &ytv_gl_bind_renderbuffer);
  SET_GL(glBindTexture, &::glBindTexture);
  SET_GL(glBlendEquation, &::glBlendEquation);
  SET_GL(glBlendEquationSeparate, &::glBlendEquationSeparate);
  SET_GL(glBlendFunc, &::glBlendFunc);
  SET_GL(glBlendFuncSeparate, &::glBlendFuncSeparate);
  SET_GL(glBufferData, &ytv_gl_buffer_data);
  SET_GL(glBufferSubData, &ytv_gl_buffer_sub_data);
  SET_GL(glCheckFramebufferStatus, &diag_check_fb);
  SET_GL(glClear, &::glClear);
  SET_GL(glClearColor, &::glClearColor);
  SET_GL(glClearDepthf, &::glClearDepthf);
  SET_GL(glClearStencil, &::glClearStencil);
  SET_GL(glColorMask, &::glColorMask);
  SET_GL(glCompileShader, &prof_compile_shader);
  SET_GL(glCompressedTexImage2D, &::glCompressedTexImage2D);
  SET_GL(glCopyTexImage2D, &::glCopyTexImage2D);
  SET_GL(glCopyTexSubImage2D, &::glCopyTexSubImage2D);
  SET_GL(glCreateProgram, &::glCreateProgram);
  SET_GL(glCreateShader, &::glCreateShader);
  SET_GL(glCullFace, &::glCullFace);
  SET_GL(glDeleteBuffers, &::glDeleteBuffers);
  SET_GL(glDeleteFramebuffers, &::glDeleteFramebuffers);
  SET_GL(glDeleteProgram, &::glDeleteProgram);
  SET_GL(glDeleteRenderbuffers, &ytv_gl_delete_renderbuffers);
  SET_GL(glDeleteShader, &::glDeleteShader);
  SET_GL(glDeleteTextures, &ytv_gl_delete_textures);
  SET_GL(glDepthFunc, &::glDepthFunc);
  SET_GL(glDepthMask, &::glDepthMask);
  SET_GL(glDepthRangef, &::glDepthRangef);
  SET_GL(glDisable, &ytv_gl_disable);
  SET_GL(glDisableVertexAttribArray, &::glDisableVertexAttribArray);
  SET_GL(glDrawArrays, &prof_draw_arrays);
  SET_GL(glDrawElements, &prof_draw_elements);
  SET_GL(glEnable, &ytv_gl_enable);
  SET_GL(glEnableVertexAttribArray, &::glEnableVertexAttribArray);
  SET_GL(glFinish, &::glFinish);
  SET_GL(glFlush, &::glFlush);
  SET_GL(glFramebufferRenderbuffer, &::glFramebufferRenderbuffer);
  SET_GL(glFramebufferTexture2D, &diag_fb_tex);
  SET_GL(glFrontFace, &::glFrontFace);
  SET_GL(glGenBuffers, &::glGenBuffers);
  SET_GL(glGenerateMipmap, &::glGenerateMipmap);
  SET_GL(glGenFramebuffers, &::glGenFramebuffers);
  SET_GL(glGenRenderbuffers, &::glGenRenderbuffers);
  SET_GL(glGenTextures, &::glGenTextures);
  SET_GL(glGetActiveAttrib, &::glGetActiveAttrib);
  SET_GL(glGetActiveUniform, &::glGetActiveUniform);
  SET_GL(glGetAttachedShaders, &::glGetAttachedShaders);
  SET_GL(glGetAttribLocation, &::glGetAttribLocation);
  SET_GL(glGetBooleanv, &::glGetBooleanv);
  SET_GL(glGetBufferParameteriv, &::glGetBufferParameteriv);
  SET_GL(glGetError, &::glGetError);
  SET_GL(glGetFloatv, &::glGetFloatv);
  SET_GL(glGetFramebufferAttachmentParameteriv, &::glGetFramebufferAttachmentParameteriv);
  SET_GL(glGetIntegerv, &ytv_gl_get_integerv);
  SET_GL(glGetProgramiv, &::glGetProgramiv);
  SET_GL(glGetProgramInfoLog, &::glGetProgramInfoLog);
  SET_GL(glGetShaderiv, &::glGetShaderiv);
  SET_GL(glGetShaderInfoLog, &::glGetShaderInfoLog);
  SET_GL(glGetShaderSource, &::glGetShaderSource);
  SET_GL(glGetString, &::glGetString);
  SET_GL(glGetUniformLocation, &::glGetUniformLocation);
  SET_GL(glGetVertexAttribfv, &::glGetVertexAttribfv);
  SET_GL(glGetVertexAttribiv, &::glGetVertexAttribiv);
  SET_GL(glGetVertexAttribPointerv, &::glGetVertexAttribPointerv);
  SET_GL(glHint, &::glHint);
  SET_GL(glIsEnabled, &ytv_gl_is_enabled);
  SET_GL(glIsFramebuffer, &::glIsFramebuffer);
  SET_GL(glIsProgram, &::glIsProgram);
  SET_GL(glIsRenderbuffer, &::glIsRenderbuffer);
  SET_GL(glIsTexture, &::glIsTexture);
  SET_GL(glLineWidth, &::glLineWidth);
  SET_GL(glLinkProgram, &prof_link_program);
  SET_GL(glPixelStorei, &ytv_gl_pixel_storei);
  SET_GL(glPolygonOffset, &::glPolygonOffset);
  SET_GL(glReadPixels, &ytv_gl_read_pixels);
  SET_GL(glReleaseShaderCompiler, &::glReleaseShaderCompiler);
  SET_GL(glRenderbufferStorage, &ytv_gl_renderbuffer_storage);
  SET_GL(glScissor, &ytv_gl_scissor);
  SET_GL(glShaderBinary, &::glShaderBinary);
  SET_GL(glShaderSource, &ytv_gl_shader_source);
  SET_GL(glStencilFunc, &::glStencilFunc);
  SET_GL(glStencilFuncSeparate, &::glStencilFuncSeparate);
  SET_GL(glStencilMask, &::glStencilMask);
  SET_GL(glStencilMaskSeparate, &::glStencilMaskSeparate);
  SET_GL(glStencilOp, &::glStencilOp);
  SET_GL(glStencilOpSeparate, &::glStencilOpSeparate);
  SET_GL(glTexImage2D, &ytv_gl_tex_image_2d);
  SET_GL(glTexParameterf, &ytv_gl_tex_parameterf);
  SET_GL(glTexParameteri, &ytv_gl_tex_parameteri);
  SET_GL(glTexParameteriv, &ytv_gl_tex_parameteriv);
  SET_GL(glTexSubImage2D, &ytv_gl_tex_sub_image_2d);
  SET_GL(glUniform1f, &::glUniform1f);
  SET_GL(glUniform1fv, &::glUniform1fv);
  SET_GL(glUniform1i, &::glUniform1i);
  SET_GL(glUniform1iv, &::glUniform1iv);
  SET_GL(glUniform2f, &::glUniform2f);
  SET_GL(glUniform2fv, &::glUniform2fv);
  SET_GL(glUniform2i, &::glUniform2i);
  SET_GL(glUniform2iv, &::glUniform2iv);
  SET_GL(glUniform3f, &::glUniform3f);
  SET_GL(glUniform3fv, &::glUniform3fv);
  SET_GL(glUniform3i, &::glUniform3i);
  SET_GL(glUniform3iv, &::glUniform3iv);
  SET_GL(glUniform4f, &::glUniform4f);
  SET_GL(glUniform4fv, &::glUniform4fv);
  SET_GL(glUniform4i, &::glUniform4i);
  SET_GL(glUniform4iv, &::glUniform4iv);
  SET_GL(glUniformMatrix2fv, &::glUniformMatrix2fv);
  SET_GL(glUniformMatrix3fv, &::glUniformMatrix3fv);
  SET_GL(glUniformMatrix4fv, &::glUniformMatrix4fv);
  SET_GL(glUseProgram, &::glUseProgram);
  SET_GL(glVertexAttrib1f, &::glVertexAttrib1f);
  SET_GL(glVertexAttrib1fv, &::glVertexAttrib1fv);
  SET_GL(glVertexAttrib2f, &::glVertexAttrib2f);
  SET_GL(glVertexAttrib2fv, &::glVertexAttrib2fv);
  SET_GL(glVertexAttrib3f, &::glVertexAttrib3f);
  SET_GL(glVertexAttrib3fv, &::glVertexAttrib3fv);
  SET_GL(glVertexAttrib4f, &::glVertexAttrib4f);
  SET_GL(glVertexAttrib4fv, &::glVertexAttrib4fv);
  SET_GL(glVertexAttribPointer, &::glVertexAttribPointer);
  SET_GL(glViewport, &ytv_gl_viewport);
  SET_GL(glReadBuffer, &::glReadBuffer);
  SET_GL(glDrawRangeElements, &::glDrawRangeElements);
  SET_GL(glGenQueries, &::glGenQueries);
  SET_GL(glDeleteQueries, &::glDeleteQueries);
  SET_GL(glBeginQuery, &::glBeginQuery);
  SET_GL(glEndQuery, &::glEndQuery);
  SET_GL(glUnmapBuffer, &::glUnmapBuffer);
  SET_GL(glBlitFramebuffer, &::glBlitFramebuffer);
  SET_GL(glMapBufferRange, &::glMapBufferRange);
  SET_GL(glFlushMappedBufferRange, &::glFlushMappedBufferRange);
  SET_GL(glBindVertexArray, &::glBindVertexArray);
  SET_GL(glDeleteVertexArrays, &::glDeleteVertexArrays);
  SET_GL(glGenVertexArrays, &::glGenVertexArrays);
  SET_GL(glBindBufferRange, &::glBindBufferRange);
  SET_GL(glBindBufferBase, &::glBindBufferBase);
  SET_GL(glGetStringi, &::glGetStringi);
  SET_GL(glGetUniformBlockIndex, &::glGetUniformBlockIndex);
  SET_GL(glUniformBlockBinding, &::glUniformBlockBinding);
  SET_GL(glDrawArraysInstanced, &::glDrawArraysInstanced);
  SET_GL(glDrawElementsInstanced, &::glDrawElementsInstanced);
  SET_GL(glGenSamplers, &::glGenSamplers);
  SET_GL(glDeleteSamplers, &::glDeleteSamplers);
  SET_GL(glBindSampler, &::glBindSampler);
  SET_GL(glSamplerParameteri, &::glSamplerParameteri);
  SET_GL(glSamplerParameterf, &::glSamplerParameterf);
  SET_GL(glVertexAttribDivisor, &::glVertexAttribDivisor);
  SET_GL(glGetProgramBinary, &::glGetProgramBinary);
  SET_GL(glProgramBinary, &::glProgramBinary);
#if defined(YTV_GL_TRACE)
  install_checked_gl();
  SET_GL(glCompileShader, &t_compile_shader);
  SET_GL(glLinkProgram, &t_link_program);
  SET_GL(glUseProgram, &t_use_program);
  SET_GL(glShaderSource, &t_shader_source);
  SET_GL(glClearColor, &t_clear_color);
  SET_GL(glViewport, &t_viewport);
  SET_GL(glScissor, &t_scissor);
  SET_GL(glEnable, &t_enable);
  SET_GL(glDisable, &t_disable);
  SET_GL(glBlendFunc, &t_blend_func);
  SET_GL(glBlendFuncSeparate, &t_blend_func_sep);
  SET_GL(glColorMask, &t_color_mask);
  SET_GL(glUniform4f, &t_uniform4f);
  SET_GL(glUniform4fv, &t_uniform4fv);
  SET_GL(glUniformMatrix3fv, &t_uniform_mat3);
  SET_GL(glGetUniformLocation, &t_get_uniform_location);
  SET_GL(glVertexAttribPointer, &t_vertex_attrib_pointer);
  SET_GL(glBindBuffer, &t_bind_buffer);
  SET_GL(glBufferData, &t_buffer_data);
  SET_GL(glBindAttribLocation, &t_bind_attrib);
  SET_GL(glDrawArrays, &t_draw_arrays);
  SET_GL(glDrawElements, &t_draw_elements);
  SET_GL(glClear, &t_clear);
  SET_GL(glBindFramebuffer, &t_bind_fb);
  SET_GL(glFramebufferTexture2D, &t_fb_tex);
  SET_GL(glCheckFramebufferStatus, &t_check_fb);
  SET_GL(glTexImage2D, &t_tex_image);
  SET_GL(glReadPixels, &t_read_pixels);
  SET_GL(glFinish, &t_finish);
  SET_GL(glFlush, &t_flush);
#endif
  // GLES 2.0 core entry points VitaGL lacks (see above).
  SET_GL(glBlendColor, &ytv_gl_blend_color);
  SET_GL(glCompressedTexSubImage2D, &ytv_gl_compressed_tex_sub_image_2d);
  SET_GL(glDetachShader, &ytv_gl_detach_shader);
  SET_GL(glGetRenderbufferParameteriv, &ytv_gl_get_renderbuffer_parameteriv);
  SET_GL(glGetShaderPrecisionFormat, &ytv_gl_get_shader_precision_format);
  SET_GL(glGetTexParameterfv, &ytv_gl_get_tex_parameterfv);
  SET_GL(glGetTexParameteriv, &ytv_gl_get_tex_parameteriv);
  SET_GL(glIsBuffer, &ytv_gl_is_buffer);
  SET_GL(glIsShader, &ytv_gl_is_shader);
  SET_GL(glSampleCoverage, &ytv_gl_sample_coverage);
  SET_GL(glTexParameterfv, &ytv_gl_tex_parameterfv);
  SET_GL(glValidateProgram, &ytv_gl_validate_program);
  // GLES 3.0-only entry points stay null: VitaGL reports an ES 2 context,
  // so Cobalt/Skia must not call them.
}
} // namespace
extern "C" {
const SbEglInterface* SbGetEglInterface() {
  SbLogRaw("YTV_PLATFORM_GET_EGL\n");
  pthread_once(&once,set_interfaces);
  return &egl_api;
}
const SbGlesInterface* SbGetGlesInterface() {
  SbLogRaw("YTV_PLATFORM_GET_GLES\n");
  pthread_once(&once,set_interfaces);
  return &gl_api;
}
// True when this thread has the (single) GL context current: the player
// uploads video frames only from the rasterizer thread.
bool ytv_gl_context_is_current() { return t_context != EGL_NO_CONTEXT; }
// Shared with standalone graphics tests: brings up GXM exactly once.
GLboolean ytv_vitagl_ensure_initialized() {
  ensure_vitagl();
  return g_vgl_fallback;
}
}

// Loader heartbeat: GL work since the previous call (frames presented, time
// in eglSwapBuffers, first-draw->swap time, draw calls, shader compiles and
// links with their time). Diagnostic; costs a few counters per call.
extern "C" { volatile size_t g_vgl_free_vram = 0, g_vgl_free_ram = 0, g_vgl_free_phy = 0; }
extern "C" void ytv_gl_stats(char* out, int n) {
  snprintf(out, n, "GL_STATS frames=%u swap_ms=%llu raster_ms=%llu draws=%u "
           "compiles=%u compile_ms=%llu links=%u link_ms=%llu max_frame_ms=%llu "
           "over20ms=%u over40ms=%u vgl_vram_kb=%u vgl_ram_kb=%u vgl_phy_kb=%u",
           (unsigned)g_hb_frames, (unsigned long long)(g_hb_swap_us / 1000),
           (unsigned long long)(g_hb_raster_us / 1000), (unsigned)g_hb_draws,
           (unsigned)g_hb_compiles, (unsigned long long)(g_hb_compile_us / 1000),
           (unsigned)g_hb_links, (unsigned long long)(g_hb_link_us / 1000),
           (unsigned long long)(g_hb_max_interval_us / 1000), (unsigned)g_hb_jank20,
           (unsigned)g_hb_jank40, (unsigned)(g_vgl_free_vram >> 10), (unsigned)(g_vgl_free_ram >> 10),
           (unsigned)(g_vgl_free_phy >> 10));
  g_hb_max_interval_us = 0; g_hb_jank20 = g_hb_jank40 = 0;
  g_hb_frames = g_hb_draws = g_hb_compiles = g_hb_links = 0;
  g_hb_swap_us = g_hb_raster_us = g_hb_compile_us = g_hb_link_us = 0;
}
