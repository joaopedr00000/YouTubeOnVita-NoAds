/*
 * Native add-on menu drawn on top of Cobalt (nothing is injected into the
 * YouTube page). Opened with SELECT or a three-finger tap.
 *
 *   - Sleep timer: pauses playback (media Pause key) when it expires and
 *     lets the Vita go to sleep normally.
 *   - Screen off: turns the OLED/LCD off while audio keeps playing; any
 *     button turns it back on.
 *   - Screenshot: saves the current screen as PNG in ux0:picture/YouTubeVita.
 *
 * Also keeps the Vita from dimming/sleeping while a video plays.
 *
 * Drawing: the menu is composed on the CPU into an opaque RGBA panel. When
 * Cobalt is presenting frames, the panel is drawn as a textured quad just
 * before each eglSwapBuffers (GL state is saved and restored). When Cobalt
 * is idle (static page, no swaps), the panel is written straight into the
 * displayed framebuffer, and the pixels underneath are restored on close.
 */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/power.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/rtc.h>
#include <vitaGL.h>
#include <zlib.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <cstdint>
#include <ctime>
#include "starboard/event.h"
#include "starboard/input.h"
#include "starboard/key.h"
#include "starboard/log.h"
#include "starboard/window.h"
#include "addon_font.h"
#include "addon_font_bold.h"
#include "port_download_entries.h"
#include "ytv_adblock_dns.h"

extern "C" void ytv_vita_lifecycle_send(SbEventType type, void* data);
extern "C" SbWindow ytv_vita_current_window();
extern "C" int ytv_download_start_or_cancel();
extern "C" void ytv_download_get_status(char*, size_t);
extern "C" bool ytv_download_config_read(int field, char*, size_t);
extern "C" bool ytv_osk_open_download_config(int field);
extern "C" bool ytv_osk_open_native_video_url();
extern "C" bool ytv_native_download_set_video(const char*);
extern "C" bool ytv_native_download_running();
extern "C" bool ytv_native_download_has_video();
extern "C" int ytv_native_download_start(bool mp3,int quality);
extern "C" bool ytv_native_download_cancel_current();
extern "C" bool ytv_native_download_running();
extern "C" void ytv_native_download_get_status(char*,size_t);
extern "C" bool ytv_native_player_present();
extern "C" void ytv_ui_sound_enable(bool on);  // port_ui_sound.cpp
extern "C" void ytv_ui_sound_set_volume(int pct);
extern "C" volatile int g_ytv_graphics_ext_active; // port_system.cpp
extern "C" void ytv_ui_sound(int kind);
extern volatile int g_ytv_hud_video_h;             // port_player.cpp
extern "C" volatile size_t g_vgl_free_vram, g_vgl_free_ram, g_vgl_free_phy;  // port_graphics.cpp
extern volatile unsigned g_ytv_hud_dropped_total;  // port_player.cpp
// Read atomically in the Starboard DNS worker threads.
extern "C" { int g_ytv_adblock_enabled = 1; }
// Video decoder texture sampler only. 1=legacy bilinear, 0=nearest/clearer.
extern "C" { volatile int g_ytv_video_bilinear_enabled = 1; }
extern "C" { volatile int g_ytv_video_aspect_mode = 0; }
// v0.64 Vertical mode for portrait videos (Shorts): 0 off, 1 turn right
// (clockwise), 2 turn left. Applied by port_player.cpp to new frames.
extern "C" { volatile int g_ytv_video_vertical_mode = 0; }
// v0.64 inverted screen (whole interface turned 180 degrees). Settings page.
extern "C" { volatile int g_ytv_screen_flip = 0; }
extern "C" { volatile int g_ytv_media_volume_pct=100; }
extern "C" bool SbSystemNetworkIsDisconnected();
extern "C" int ytv_network_platform_error_pending();
extern "C" unsigned ytv_network_platform_error_generation();
extern "C" int ytv_network_platform_error_retry();
#if defined(YTV_NATIVE_DOWNLOAD_TEST)
extern "C" int ytv_metadata_selftest();
extern "C" int ytv_download_quality_selftest();
extern "C" int ytv_media_size_selftest();
#endif

namespace {

constexpr int kScreenW = 960, kScreenH = 544;
constexpr int kPanelW = 520, kPanelH = 520;
constexpr int kToastH = 52;
// 50% black backdrop to distinguish the Add-ons menu from the underlying TV image.
constexpr float kMenuBackdropOpacity = 0.50f;

// Native pages instead of modifying the proprietary YouTube TV DOM.
enum Page { kPageNone, kPageHome, kPagePlayer, kPageSettings,
            kPageDisplay, kPageDownload, kPageDownloads, kPageNetwork };
Page g_page = kPageNone;
Page g_player_return = kPageHome;
Page g_downloads_return = kPageHome;
bool g_network_offline=false, g_network_seen_offline=false;
bool g_network_auto_retry_done=false;
uint64_t g_network_online_since=0, g_network_next_prompt=0, g_network_last_check=0;
unsigned g_network_error_seen=0;
int g_quality_idx = 1;            // 360, 480, 720
bool g_audio_only = false;         // MP4 by default
int g_language_pref = 0;           // 0=system, 1=PT, 2=English
bool g_system_pt = false;
// Screen and info page.
const int kDimLevels[] = {0, 20, 35, 50, 65, 80, 90};
constexpr int kDimChoices = sizeof(kDimLevels) / sizeof(kDimLevels[0]);
uint64_t g_media_cfg_dirty_since=0;
int g_dim_idx = 0;          // internal darkening, independent of the backlight
bool g_perf_hud = false;    // FPS / memory / video info in a corner
bool g_clock_hud = false;   // clock and battery in a corner
bool g_ui_sound = true;     // original, gentle navigation cues ON by default
int g_ui_volume = 70;       // percent; persistent UI audio volume
bool g_fhd_cfg = false;     // Full HD choice (takes effect when the app starts)
bool g_fhd_boot = false;    // what this run started with
const int kTimerMinutes[] = {0, 5, 15, 30, 45, 60, 90, 120};
constexpr int kTimerChoices = sizeof(kTimerMinutes) / sizeof(kTimerMinutes[0]);

// Shared state. Written by the main (input) thread, read by the renderer
// thread in ytv_addons_before_swap(); a generation counter publishes a new
// panel image.
volatile bool g_menu_open = false;
volatile int g_toast_until_ms = 0;  // toast visible while now_ms < this
int g_sel = 0;
int g_timer_idx = 0;
uint64_t g_timer_deadline_us = 0;
bool g_timer_announce_pending=false;
bool g_timer_final_alert_played=false;
volatile bool g_timer_countdown_visible=false;
volatile unsigned g_downloads_completed=0;
volatile int g_mp4_fallback_requested=0;
volatile int g_mp4_fallback_actual=0;
volatile unsigned g_mp4_fallback_serial=0;
unsigned g_mp4_fallback_displayed=0;
unsigned g_downloads_completed_notified=0;
bool g_battery_warned=false, g_battery_pending=false;
bool g_screen_off = false;
bool g_shot_pending = false;
uint64_t g_shot_at_us = 0;
uint64_t g_screen_off_at_us = 0;
bool g_input_locked = false;  // screen and buttons ignored; SELECT unlocks
bool g_lr_enabled = true;     // L/R reach YouTube (saved in addons.cfg)
constexpr const char* kAddonCfg = "ux0:data/youtubevita/addons.cfg";
bool g_pt = false;  // Portuguese UI (system or user preference)

// The native input thread composes UI while EGL uploads it. Serializing the
// CPU panel/HUD image with GL texture uploads prevents partially rendered
// rows from flashing on every menu navigation/volume change.
pthread_mutex_t g_overlay_image_lock = PTHREAD_MUTEX_INITIALIZER;
// Current overlay image (panel or toast).
uint8_t* g_img = nullptr;  // kPanelW*kPanelH*4, top row first
volatile int g_img_w = 0, g_img_h = 0;
volatile uint32_t g_img_gen = 0;

// Renderer-thread side.
volatile uint64_t g_last_swap_us = 0;
GLuint g_tex = 0, g_prog = 0;
GLint g_loc_pos = -1, g_loc_uv = -1, g_loc_tex = -1, g_loc_alpha = -1;
GLuint g_tex_hud = 0, g_tex_black = 0;
uint32_t g_hud_tex_gen = 0;
uint32_t g_tex_gen = 0;
bool g_gl_failed = false;

// Idle (CPU) path: what was painted into which display buffer.
void* g_painted_base = nullptr;
uint32_t g_painted_gen = 0;
int g_painted_x = 0, g_painted_y = 0, g_painted_w = 0, g_painted_h = 0;
uint64_t g_painted_after_swap = 0;
uint8_t* g_backup = nullptr;  // saved original pixels under idle overlay
size_t g_backup_capacity = 0;

uint64_t now_us() { return sceKernelGetSystemTimeWide(); }

void log_line(const char* s) { SbLogRaw(s); }

// ---- Drawing ---------------------------------------------------------------
// Everything is composed on the CPU into an RGBA image. Shapes are
// anti-aliased inside the panel; the panel's outer edge keeps binary alpha so
// the idle path can copy pixels without blending.
struct Font { const AddonGlyph* g; const uint8_t* px; int w, h; };
const Font kRegular = {kAddonGlyphs, kAddonFont, kAddonFontW, kAddonFontH};
const Font kBold = {kAddonBoldGlyphs, kAddonBoldFont, kAddonBoldFontW, kAddonBoldFontH};

// Palette (YouTube dark theme).
constexpr uint32_t kBg = 0x181818, kSurface = 0x272727, kSelected = 0x3f3f3f, kDivider = 0x333333;
constexpr uint32_t kText = 0xf1f1f1, kTextDim = 0xaaaaaa, kRed = 0xff0033;
constexpr uint32_t kCrossBlue = 0x8fb4ff, kCircleRed = 0xff7a7a;

// UTF-8 (only Latin-1 code points are drawn) -> glyph indices.
int next_cp(const char*& s) {
  unsigned char c = (unsigned char)*s;
  if (!c) return 0;
  if (c < 0x80) { s++; return c; }
  if ((c & 0xe0) == 0xc0 && s[1]) {
    int cp = ((c & 0x1f) << 6) | ((unsigned char)s[1] & 0x3f);
    s += 2;
    return cp < 256 ? cp : '?';
  }
  s++;
  while ((*s & 0xc0) == 0x80) s++;
  return '?';
}

int text_width(const char* s, const Font& f = kRegular) {
  int w = 0;
  for (int cp; (cp = next_cp(s));) w += f.g[cp].adv;
  return w;
}

struct Canvas { uint8_t* px; int w, h; };

void blend(const Canvas& c, int x, int y, uint32_t rgb, int a) {
  if (x < 0 || y < 0 || x >= c.w || y >= c.h || a <= 0) return;
  if (a > 255) a = 255;
  uint8_t* p = c.px + ((size_t)y * c.w + x) * 4;
  if (!p[3]) return;  // outside the panel shape
  if (p[3] < 255) p[3] = (uint8_t)(p[3] + (255 - p[3]) * a / 255);  // translucent layers (HUD)
  int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  p[0] = (uint8_t)((p[0] * (255 - a) + r * a) / 255);
  p[1] = (uint8_t)((p[1] * (255 - a) + g * a) / 255);
  p[2] = (uint8_t)((p[2] * (255 - a) + b * a) / 255);
}

void draw_text(const Canvas& c, int x, int y, const char* s, uint32_t rgb, const Font& f = kRegular) {
  for (int cp; (cp = next_cp(s));) {
    const AddonGlyph& g = f.g[cp];
    for (int gy = 0; gy < f.h; gy++)
      for (int gx = 0; gx < g.w; gx++) blend(c, x + gx, y + gy, rgb, f.px[gy * f.w + g.x + gx]);
    x += g.adv;
  }
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Signed distance to a rounded rectangle (negative inside).
float round_rect_sd(float px, float py, float x0, float y0, float x1, float y1, float r) {
  float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
  float hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
  float dx = fabsf(px - cx) - hx, dy = fabsf(py - cy) - hy;
  float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
  float inside = dx > dy ? dx : dy;
  return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - r;
}

void fill_round(const Canvas& c, float x0, float y0, float x1, float y1, float r, uint32_t rgb) {
  for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++)
    for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
      float d = round_rect_sd(x + 0.5f, y + 0.5f, x0, y0, x1, y1, r);
      blend(c, x, y, rgb, (int)(clampf(0.5f - d, 0.f, 1.f) * 255.f));
    }
}

void fill_rect(const Canvas& c, int x0, int y0, int x1, int y1, uint32_t rgb) {
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) blend(c, x, y, rgb, 255);
}

// Anti-aliased segment of thickness t.
void line(const Canvas& c, float x0, float y0, float x1, float y1, float t, uint32_t rgb) {
  float minx = (x0 < x1 ? x0 : x1) - t, maxx = (x0 > x1 ? x0 : x1) + t;
  float miny = (y0 < y1 ? y0 : y1) - t, maxy = (y0 > y1 ? y0 : y1) + t;
  float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
  for (int y = (int)miny; y <= (int)maxy; y++)
    for (int x = (int)minx; x <= (int)maxx; x++) {
      float px = x + 0.5f - x0, py = y + 0.5f - y0;
      float k = len2 > 0 ? clampf((px * vx + py * vy) / len2, 0.f, 1.f) : 0.f;
      float dx = px - k * vx, dy = py - k * vy;
      float d = sqrtf(dx * dx + dy * dy) - t * 0.5f;
      blend(c, x, y, rgb, (int)(clampf(0.5f - d, 0.f, 1.f) * 255.f));
    }
}

void ring(const Canvas& c, float cx, float cy, float r, float t, uint32_t rgb) {
  for (int y = (int)(cy - r - t); y <= (int)(cy + r + t); y++)
    for (int x = (int)(cx - r - t); x <= (int)(cx + r + t); x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      float d = fabsf(sqrtf(dx * dx + dy * dy) - r) - t * 0.5f;
      blend(c, x, y, rgb, (int)(clampf(0.5f - d, 0.f, 1.f) * 255.f));
    }
}

void disc(const Canvas& c, float cx, float cy, float r, uint32_t rgb) {
  for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
    for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      float d = sqrtf(dx * dx + dy * dy) - r;
      blend(c, x, y, rgb, (int)(clampf(0.5f - d, 0.f, 1.f) * 255.f));
    }
}

// Opaque panel shape: rounded rectangle with binary outer alpha.
void base_panel(const Canvas& c, uint32_t rgb, int radius) {
  for (int y = 0; y < c.h; y++)
    for (int x = 0; x < c.w; x++) {
      uint8_t* p = c.px + ((size_t)y * c.w + x) * 4;
      float d = round_rect_sd(x + 0.5f, y + 0.5f, 0, 0, (float)c.w, (float)c.h, (float)radius);
      p[0] = (rgb >> 16) & 255; p[1] = (rgb >> 8) & 255; p[2] = rgb & 255;
      p[3] = d <= 0.f ? 255 : 0;
    }
}

// ---- Icons (24x24, centred on cx, cy) --------------------------------------
void icon_timer(const Canvas& c, float cx, float cy, uint32_t col) {
  ring(c, cx, cy + 1, 9, 2.2f, col);
  line(c, cx, cy + 1, cx, cy - 4.5f, 2.2f, col);
  line(c, cx, cy + 1, cx + 4, cy + 3, 2.2f, col);
  line(c, cx - 3, cy - 10, cx + 3, cy - 10, 2.2f, col);
}
void icon_screen_off(const Canvas& c, float cx, float cy, uint32_t col, uint32_t bg) {
  fill_round(c, cx - 11, cy - 8, cx + 11, cy + 7, 2.5f, col);
  fill_round(c, cx - 9, cy - 6, cx + 9, cy + 5, 1.f, bg);
  line(c, cx - 4, cy + 10, cx + 4, cy + 10, 2.2f, col);
  // moon inside the screen
  disc(c, cx - 1, cy - 0.5f, 4.2f, col);
  disc(c, cx + 1.4f, cy - 2.2f, 3.6f, bg);
}
void icon_camera(const Canvas& c, float cx, float cy, uint32_t col, uint32_t bg) {
  fill_round(c, cx - 11, cy - 6, cx + 11, cy + 9, 3.f, col);
  fill_round(c, cx - 5, cy - 9, cx + 4, cy - 4, 1.5f, col);
  disc(c, cx, cy + 1.5f, 5.2f, bg);
  disc(c, cx, cy + 1.5f, 3.2f, col);
}
void icon_lock(const Canvas& c, float cx, float cy, uint32_t col, uint32_t bg) {
  ring(c, cx, cy - 3, 6, 2.4f, col);                       // shackle
  fill_round(c, cx - 9, cy - 2, cx + 9, cy + 10, 2.5f, col);  // body
  disc(c, cx, cy + 3.5f, 2.2f, bg);                        // keyhole
}
void icon_lr(const Canvas& c, float cx, float cy, uint32_t col, uint32_t bg) {
  fill_round(c, cx - 12, cy - 8, cx - 1, cy + 8, 4, col);
  fill_round(c, cx + 1, cy - 8, cx + 12, cy + 8, 4, col);
  line(c, cx - 8, cy - 4, cx - 8, cy + 4, 2.f, bg);   // L
  line(c, cx - 8, cy + 4, cx - 4, cy + 4, 2.f, bg);
  line(c, cx + 5, cy - 4, cx + 5, cy + 4, 2.f, bg);   // R
  line(c, cx + 5, cy - 4, cx + 8, cy - 2, 2.f, bg);
  line(c, cx + 8, cy - 2, cx + 5, cy, 2.f, bg);
  line(c, cx + 5, cy, cx + 8.5f, cy + 4, 2.f, bg);
}
void icon_download(const Canvas& c, float cx, float cy, uint32_t col) {
  line(c, cx, cy - 10, cx, cy + 6, 2.4f, col);
  line(c, cx - 6, cy, cx, cy + 6, 2.4f, col);
  line(c, cx, cy + 6, cx + 6, cy, 2.4f, col);
  line(c, cx - 9, cy + 10, cx + 9, cy + 10, 2.4f, col);
}
void icon_sun(const Canvas& c, float cx, float cy, uint32_t col) {
  disc(c, cx, cy, 4.5f, col);
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159265f / 4;
    line(c, cx + cosf(a) * 7.5f, cy + sinf(a) * 7.5f, cx + cosf(a) * 10.5f, cy + sinf(a) * 10.5f, 2.f, col);
  }
}
void icon_gear(const Canvas& c, float cx, float cy, uint32_t col, uint32_t bg) {
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159265f / 4;
    line(c, cx + cosf(a) * 6, cy + sinf(a) * 6, cx + cosf(a) * 10.5f, cy + sinf(a) * 10.5f, 3.4f, col);
  }
  disc(c, cx, cy, 7.5f, col);
  disc(c, cx, cy, 3.2f, bg);
}
void icon_chart(const Canvas& c, float cx, float cy, uint32_t col) {
  fill_round(c, cx - 10, cy + 2, cx - 5, cy + 10, 1, col);
  fill_round(c, cx - 2.5f, cy - 4, cx + 2.5f, cy + 10, 1, col);
  fill_round(c, cx + 5, cy - 10, cx + 10, cy + 10, 1, col);
}
void icon_close(const Canvas& c, float cx, float cy, uint32_t col) {
  line(c, cx - 7, cy - 7, cx + 7, cy + 7, 2.4f, col);
  line(c, cx + 7, cy - 7, cx - 7, cy + 7, 2.4f, col);
}
void glyph_cross(const Canvas& c, float cx, float cy) {
  line(c, cx - 5, cy - 5, cx + 5, cy + 5, 2.2f, kCrossBlue);
  line(c, cx + 5, cy - 5, cx - 5, cy + 5, 2.2f, kCrossBlue);
}
void glyph_circle(const Canvas& c, float cx, float cy) { ring(c, cx, cy, 6, 2.2f, kCircleRed); }

// Footer hint: icon or key name, then the action.
int hint(const Canvas& c, int x, int y, int key, const char* label) {
  const int cy = y + kAddonFontH / 2;
  if (key == 1) { glyph_cross(c, x + 8, cy); x += 22; }
  else if (key == 2) { glyph_circle(c, x + 8, cy); x += 22; }
  else {
    int kw = text_width("SELECT") + 12;
    fill_round(c, x, cy - 10, x + kw, cy + 10, 10, kSelected);
    draw_text(c, x + 6, y, "SELECT", kTextDim);
    x += kw + 6;
  }
  draw_text(c, x, y, label, kTextDim);
  return x + text_width(label) + 22;
}

const char* T(const char* en, const char* pt) { return g_pt ? pt : en; }

void timer_value(char* out, size_t n) {
  if (g_timer_deadline_us) {
    uint64_t now = now_us();
    int left = g_timer_deadline_us > now ? (int)((g_timer_deadline_us - now) / 60000000ull) + 1 : 0;
    snprintf(out, n, T("%d min left", "faltam %d min"), left);
  } else {
    snprintf(out, n, "%s", T("Off", "Desligado"));
  }
}

// Snapshot only when opened/refreshed: never do disk I/O during a frame.
constexpr int kDownloadsVisible=5;
YtvDownloadEntry g_download_items[48]={};
char g_download_labels[kDownloadsVisible][88]={};
int g_download_count=0, g_download_first=0;
unsigned g_download_seen_revision=0;
int download_visible(){
  const int n=g_download_count-g_download_first;
  return n<=0?0:(n<kDownloadsVisible?n:kDownloadsVisible);
}
void refresh_download_list(){
  const unsigned version=ytv_native_download_revision();
  g_download_count=ytv_native_download_entries(g_download_items,48);
  g_download_seen_revision=version;
  if(g_download_count<0)g_download_count=0;
  if(g_download_first>=g_download_count)g_download_first=0;
  char log[96];snprintf(log,sizeof(log),"YTV_LIFECYCLE_ADDON downloads_list=%d\n",g_download_count);
  log_line(log);
}
const char* download_row_label(int row){
  if(row<0 || row>=download_visible())return "";
  const YtvDownloadEntry& e=g_download_items[g_download_first+row];
  const char* prefix=e.state==1?T("Active: ","Baixando: "):
                     e.state==2?T("Queued: ","Na fila: "):"";
  char* dest=g_download_labels[row];
  const size_t width=32;
  snprintf(dest,88,"%s",prefix);
  size_t used=strlen(dest);
  const unsigned char* u=(const unsigned char*)e.name;
  while(*u && used<width) {
    const size_t step=(*u<0x80)?1:((*u&0xe0)==0xc0?2:((*u&0xf0)==0xe0?3:4));
    if(used+step>width || strlen((const char*)u)<step)break;
    memcpy(dest+used,u,step);used+=step;u+=step;dest[used]=0;
  }
  if(*u)strncat(dest,"...",87-strlen(dest));
  return dest;
}
constexpr int kRowY0 = 62, kRowH = 42, kRowX0 = 14;
constexpr const char* kQualities[] = {"360p", "480p", "720p"};
constexpr const char* kQualityValues[] = {"360", "480", "720"};

int page_count() {
  if (g_page == kPageNetwork) return 3;
  if (g_page == kPagePlayer) return 9;
  if (g_page == kPageDownload) return 5;
  if (g_page == kPageDownloads) return download_visible()+2;
  if (g_page == kPageSettings) return 8;
  if (g_page == kPageDisplay) return 6;
  return 6;
}
constexpr int kVolumeRowExtra=24;
int row_y(int i){
  return kRowY0+i*kRowH+(g_page==kPageDisplay && i>=5?kVolumeRowExtra:0);
}
int row_h(int i){
  return kRowH+(g_page==kPageDisplay && i==4?kVolumeRowExtra:0);
}
int panel_height(){
  return kRowY0+page_count()*kRowH+
         (g_page==kPageDisplay?kVolumeRowExtra:0)+64;
}
const char* menu_title() {
  switch (g_page) {
    case kPageNetwork:
      return g_network_offline ? T("No internet", "Sem internet")
                               : T("Connection error", "Erro de conexão");
    case kPagePlayer: return T("Player settings", "Configurações do player");
    case kPageDownload: return T("Download", "Baixar");
    case kPageDownloads: return "Downloads";
    case kPageSettings: return T("Settings", "Configurações");
    case kPageDisplay: return T("Screen and info", "Tela e informações");
    default: return "Youtube on Vita";
  }
}
const char* item_text(int i) {
  if (g_page == kPageNetwork) {
    const char* options[] = {
      T("Try again", "Tentar novamente"),
      T("Network settings", "Configurações de rede"),
      T("Back", "Voltar")
    };
    return options[i];
  }
  if (g_page == kPagePlayer) {
    const char* labels[] = {
      T("Download", "Baixar"),
      T("Screenshot", "Capturar tela"),
      T("Sleep timer", "Timer para pausar"),
      T("Black Screen", "Tela preta"),
      T("Darken Screen", "Escurecer tela"),
      T("Video filtering", "Filtro de vídeo"),
      T("Aspect Ratio", "Proporção da imagem"),
      T("Vertical (Shorts)", "Vertical (Shorts)"),
      T("Back", "Voltar"),
    };
    return labels[i];
  }
  if (g_page == kPageDownload) {
    const char* labels[] = {
      T("Video (MP4)", "Vídeo (MP4)"),
      T("Audio (MP3)", "Áudio (MP3)"),
      T("View downloads", "Ver downloads"),
      T("Cancel current download", "Cancelar download atual"),
      T("Back", "Voltar"),
    };
    return labels[i];
  }
  if (g_page == kPageDownloads) {
    const int n=download_visible();
    if(i<n)return download_row_label(i);
    if(i==n) {
      if(g_download_count<=kDownloadsVisible)return T("Refresh list", "Atualizar lista");
      return g_download_first+kDownloadsVisible<g_download_count
          ? T("View more", "Ver mais") : T("First page", "Início da lista");
    }
    return T("Back", "Voltar");
  }
  if (g_page == kPageSettings) {
    const char* labels[] = {
      T("AdBlock (partial)", "AdBlock (parcial)"),
      T("Download quality", "Qualidade download"),
      T("Download format", "Formato download"),
      T("L / R buttons", "Botões L / R"),
      T("Lock input", "Bloquear entrada"),
      T("Language", "Idioma"),
      T("Inverted screen", "Tela invertida"),
      T("Back", "Voltar"),
    };
    return labels[i];
  }
  if (g_page == kPageDisplay) {
    const char* labels[] = {
      T("Full HD (beta)", "Full HD (beta)"),
      T("Performance", "Desempenho"),
      T("Clock and battery", "Relógio e bateria"),
      T("Navigation sounds", "Sons de navegação"),
      T("UI sounds volume", "Volume dos sons"),
      T("Back", "Voltar"),
    };
    return labels[i];
  }
  const char* labels[] = {
    T("Player settings", "Configurações do player"),
    T("Downloads", "Downloads"),
    T("Settings", "Configurações"),
    T("Screen and info", "Tela e informações"),
    T("Vita Settings", "Configurações do Vita"),
    T("Close", "Fechar"),
  };
  return labels[i];
}
void icon_item(const Canvas& c, int i, float x, float y, uint32_t col, uint32_t bg) {
  if(g_page == kPageNetwork){
    if(i==0)icon_timer(c,x,y,col);
    else if(i==1)icon_gear(c,x,y,col,bg);
    else icon_close(c,x,y,col);
  } else if (g_page == kPagePlayer) {
    switch(i) {
      case 0: icon_download(c,x,y,col); break;
      case 1: icon_camera(c,x,y,col,bg); break;
      case 2: icon_timer(c,x,y,col); break;
      case 3: icon_screen_off(c,x,y,col,bg); break;
      case 4: icon_sun(c,x,y,col); break;
      case 5: icon_screen_off(c,x,y,col,bg); break;
      case 6: icon_screen_off(c,x,y,col,bg); break;
      case 7: icon_screen_off(c,x,y,col,bg); break;
      default: icon_close(c,x,y,col); break;
    }
  } else if (g_page == kPageDownload) {
    if(i<3) icon_download(c,x,y,col);
    else icon_close(c,x,y,col);
  } else if (g_page == kPageDownloads) {
    if(i<download_visible()) icon_download(c,x,y,col);
    else if(i==download_visible()) icon_timer(c,x,y,col);
    else icon_close(c,x,y,col);
  } else if (g_page == kPageSettings) {
    switch(i) {
      case 0: icon_lock(c,x,y,col,bg); break;
      case 1: icon_download(c,x,y,col); break;
      case 2: icon_download(c,x,y,col); break;
      case 4: icon_lock(c,x,y,col,bg); break;
      case 3: icon_lr(c,x,y,col,bg); break;
      case 5: icon_timer(c,x,y,col); break;
      case 6: icon_screen_off(c,x,y,col,bg); break;
      default: icon_close(c,x,y,col); break;
    }
  } else if (g_page == kPageDisplay) {
    switch(i) {
      case 0: icon_screen_off(c,x,y,col,bg); break;
      case 1: icon_chart(c,x,y,col); break;
      case 2: icon_timer(c,x,y,col); break;
      case 3: icon_lr(c,x,y,col,bg); break;
      case 4: icon_lr(c,x,y,col,bg); break;
      default: icon_close(c,x,y,col); break;
    }
  } else {
    if (i == 0) icon_screen_off(c,x,y,col,bg);
    else if (i == 1) icon_download(c,x,y,col);
    else if (i == 2) icon_lr(c,x,y,col,bg);
    else if (i == 3) icon_sun(c,x,y,col);
    else if (i == 4) icon_gear(c,x,y,col,bg);
    else icon_close(c,x,y,col);
  }
}
const char* item_value(int i, const char* timer, char* scratch, size_t capacity) {
  if(g_page == kPageNetwork && i==0)
    return g_network_offline ? T("Offline","Offline") : T("Connected","Conectado");
  if (g_page == kPagePlayer) {
    if (i == 0) {
      ytv_native_download_get_status(scratch,capacity);
      return scratch;
    }
    if (i == 2) return timer;
    if (i == 7) {
      const int mode=__atomic_load_n(&g_ytv_video_vertical_mode,__ATOMIC_RELAXED);
      if(mode==1)return T("Turn right","Girar à direita");
      if(mode==2)return T("Turn left","Girar à esquerda");
      return T("Off","Desligado");
    }
    if (i == 6) {
      const int mode=__atomic_load_n(&g_ytv_video_aspect_mode,__ATOMIC_RELAXED);
      if(mode==1)return "Zoom";
      if(mode==2)return T("Stretch","Esticar");
      return T("Original","Original");
    }
    if (i == 5) return __atomic_load_n(&g_ytv_video_bilinear_enabled,__ATOMIC_RELAXED)
                         ? "Bilinear" : "Nearest";
    if (i == 4) {
      if (!kDimLevels[g_dim_idx]) return T("Off","Desligado");
      snprintf(scratch,capacity,"%d%%",kDimLevels[g_dim_idx]);
      return scratch;
    }
  }
  if (g_page == kPageDownload) {
    if(i==0) return kQualities[g_quality_idx];
    if(i==1) return "160 kbps";
  }
  if(g_page==kPageDownloads && i==download_visible() &&
      g_download_count>kDownloadsVisible){
    snprintf(scratch,capacity,"%d/%d",
      g_download_first/kDownloadsVisible+1,
      (g_download_count+kDownloadsVisible-1)/kDownloadsVisible);
    return scratch;
  }
  if(g_page==kPageDownloads && i<download_visible()){
    const YtvDownloadEntry& e=g_download_items[g_download_first+i];
    if(e.state==1){
      ytv_native_download_get_status(scratch,capacity);
      return scratch;
    }
    snprintf(scratch,capacity,"%s",e.state==2?T("Waiting","Aguardando"):e.detail);
    return scratch;
  }
  if (g_page == kPageSettings) {
    switch(i) {
      case 0: return __atomic_load_n(&g_ytv_adblock_enabled,__ATOMIC_RELAXED) ? T("On","Ligado") : T("Off","Desligado");
      case 1: return kQualities[g_quality_idx];
      case 2: return g_audio_only ? "MP3" : "MP4";
      case 3: return g_lr_enabled ? T("On","Ligados") : T("Off","Desligados");
      case 5: return g_language_pref == 0 ? T("System","Sistema") : (g_language_pref == 1 ? "PT-BR" : "English");
      case 6: return __atomic_load_n(&g_ytv_screen_flip,__ATOMIC_RELAXED) ? T("On","Ligada") : T("Off","Desligada");
    }
  }
  if (g_page == kPageDisplay) {
    switch(i) {
      case 0:
        if (g_fhd_cfg != g_fhd_boot) return T("Reopen the app","Reabra o app");
        return g_fhd_cfg ? T("On","Ligado") : T("Off","Desligado");
      case 1: return g_perf_hud ? T("On","Ligado") : T("Off","Desligado");
      case 2: return g_clock_hud ? T("On","Ligado") : T("Off","Desligado");
      case 3: return g_ui_sound ? T("On","Ligado") : T("Off","Desligado");
      case 4:
        snprintf(scratch,capacity,"%d%%",g_ui_volume);
        return scratch;
    }
  }
  return nullptr;
}
void compose_panel() {
  pthread_mutex_lock(&g_overlay_image_lock);
  const int h = panel_height();
  Canvas c = {g_img, kPanelW, h};
  base_panel(c, kBg, 16);
  fill_round(c,20,18,46,36,5,kRed);
  for (int i=0;i<6;i++)
    line(c,30,22.5f+i*1.5f,30+(i<3?i:5-i)*2.6f+1,
         22.5f+i*1.5f,1.6f,0xffffff);
  draw_text(c,56,13,menu_title(),kText,kBold);
  {
    SceDateTime t={};
    sceRtcGetCurrentClockLocalTime(&t);
    int batt=scePowerGetBatteryLifePercent();
    char right[32];
    if (batt>=0 && batt<=100)
      snprintf(right,sizeof(right),"%02d:%02d  ·  %d%%",(int)t.hour,(int)t.minute,batt);
    else snprintf(right,sizeof(right),"%02d:%02d",(int)t.hour,(int)t.minute);
    draw_text(c,kPanelW-22-text_width(right),15,right,kTextDim);
  }
  // No revision/subtitle below the YouTube on Vita heading.
  if(g_page==kPageNetwork){
    draw_text(c,25,40,g_network_offline ?
        T("Check Wi-Fi or Airplane mode", "Verifique Wi-Fi ou Modo avião") :
        T("Wi-Fi detected. Retry YouTube", "Wi-Fi detectado. Tente novamente"),
        kTextDim);
  }
  fill_rect(c,20,52,kPanelW-20,53,kDivider);
  char timer[48]={};
  timer_value(timer,sizeof(timer));
  for (int i=0;i<page_count();i++) {
    const int y=row_y(i),rh=row_h(i);
    const bool volume_row=g_page==kPageDisplay && i==4;
    const bool sel=i==g_sel;
    const uint32_t row_bg=sel?kSelected:kBg;
    if (sel) {
      fill_round(c,kRowX0,y+2,kPanelW-kRowX0,y+rh-2,10,kSelected);
      fill_round(c,kRowX0,y+10,kRowX0+4,y+rh-10,2,kRed);
    }
    const float x=kRowX0+30;
    // The volume row has two lines: label and value on top, slider below.
    const float cy=y+(volume_row?kRowH/2.f:rh/2.f);
    icon_item(c,i,x,cy,sel?kText:kTextDim,row_bg);
    const Font& font=sel?kBold:kRegular;
    draw_text(c,kRowX0+56,y+(kRowH-font.h)/2,
              item_text(i),sel?kText:0xdedede,font);
    if(volume_row){
      // Dedicated second line: no overlap with "UI sounds volume" or pill.
      const int x0=kRowX0+62, x1=kPanelW-kRowX0-30;
      const int center=y+kRowH+10;
      fill_round(c,x0,center-3,x1,center+3,3,kSurface);
      const int knob=x0+(x1-x0)*g_ui_volume/100;
      if(knob>x0)fill_round(c,x0,center-3,knob,center+3,3,kRed);
      disc(c,knob,center,6,kText);
    }
    char scratch[48]={};
    const char* value=item_value(i,timer,scratch,sizeof(scratch));
    if (value) {
      // Avoid colliding with long settings labels and the right-hand value pill.
      const int pw=text_width(value)+22;
      const int x1=kPanelW-kRowX0-16,x0=x1-pw;
      fill_round(c,x0,y+8,x1,y+kRowH-8,13,
                 sel?kRed:kSurface);
      draw_text(c,x0+11,y+(kRowH-kAddonFontH)/2,value,kText);
    }
  }
  const int fy=h-38;
  fill_rect(c,20,fy-10,kPanelW-20,fy-9,kDivider);
  int x=22;
  x=hint(c,x,fy,1,T("Select","Escolher"));
  x=hint(c,x,fy,2,T("Back","Voltar"));
  hint(c,x,fy,0,T("Close","Fechar"));
  g_img_w=kPanelW;
  g_img_h=h;
  __sync_add_and_fetch(&g_img_gen,1);
  pthread_mutex_unlock(&g_overlay_image_lock);
}

// icon: 0 none, 1 camera, 2 timer, 3 screen
void compose_toast(const char* text, int icon) {
  pthread_mutex_lock(&g_overlay_image_lock);
  const int w = text_width(text) + (icon ? 76 : 48), h = kToastH;
  Canvas c = {g_img, w > kPanelW ? kPanelW : w, h};
  base_panel(c, kSurface, h / 2);
  int x = 24;
  if (icon) {
    const float cy = h / 2.f;
    if (icon == 1) icon_camera(c, x + 12, cy, kText, kSurface);
    else if (icon == 2) icon_timer(c, x + 12, cy, kText);
    else if (icon == 4) icon_lock(c, x + 12, cy, kText, kSurface);
    else icon_screen_off(c, x + 12, cy, kText, kSurface);
    x += 34;
  }
  draw_text(c, x, (h - kAddonFontH) / 2, text, kText);
  g_img_w = c.w;
  g_img_h = h;
  __sync_add_and_fetch(&g_img_gen, 1);
  pthread_mutex_unlock(&g_overlay_image_lock);
}

bool overlay_visible() {
  return g_menu_open || (int)(now_us() / 1000) < g_toast_until_ms;
}

void show_toast(const char* text, int ms, int icon = 0) {
  if (g_menu_open) return;
  compose_toast(text, icon);
  g_toast_until_ms = (int)(now_us() / 1000) + ms;
}

// ---- Media key -------------------------------------------------------------
void send_key(SbKey key) {
  SbWindow window = ytv_vita_current_window();
  if (!SbWindowIsValid(window)) return;
  for (int i = 0; i < 2; i++) {
    SbInputData e = {};
    e.window = window;
    e.type = i == 0 ? kSbInputEventTypePress : kSbInputEventTypeUnpress;
    e.device_type = kSbInputDeviceTypeKeyboard;
    e.device_id = 0;
    e.key = key;
    ytv_vita_lifecycle_send(kSbEventTypeInput, &e);
  }
}

// ---- Screen off ------------------------------------------------------------
// "Screen off" draws a black frame over everything (GL path every frame,
// CPU path on a static screen) and swallows input until SELECT. The panel is
// NOT turned off: scePowerRequestDisplayOff made the Vita lock, and the lock
// screen suspends the app (video and audio stopped).
uint8_t* g_black_backup = nullptr;   // display buffer under the CPU black frame
void* g_black_base = nullptr;
uint64_t g_black_after_swap = 0;
void black_idle_restore();
void set_screen_off(bool off) {
  if (off == g_screen_off) return;
  g_screen_off = off;
  if(off)ytv_ui_sound(15); // iOS lock cue when screen goes black
  if (!off) black_idle_restore();
  char line[80];
  snprintf(line, sizeof(line), "YTV_LIFECYCLE_ADDON screen_black=%d\n", (int)off);
  log_line(line);
}
// Static screen (no swaps): blacken the displayed buffer once, keep a copy.
void black_idle(uint64_t now) {
  if ((int64_t)(now - g_last_swap_us) < 150000) { g_black_base = nullptr; return; }
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !fb.base) return;
  if (fb.base == g_black_base && g_black_after_swap == g_last_swap_us) return;
  const size_t bytes = (size_t)fb.pitch * kScreenH * 4;
  if (!g_black_backup) g_black_backup = (uint8_t*)malloc(bytes);
  if (g_black_backup) memcpy(g_black_backup, fb.base, bytes);
  memset(fb.base, 0, bytes);
  g_black_base = fb.base;
  g_black_after_swap = g_last_swap_us;
}
void black_idle_restore() {
  if (!g_black_base) return;
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  // Only if no new frame was presented since (that one is not black).
  if (g_black_backup && g_black_after_swap == g_last_swap_us &&
      sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0 && fb.base == g_black_base)
    memcpy(fb.base, g_black_backup, (size_t)fb.pitch * kScreenH * 4);
  g_black_base = nullptr;
}

// ---- Screenshot ------------------------------------------------------------
void be32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
bool write_chunk(SceUID fd, const char* type, const uint8_t* data, uint32_t len) {
  uint8_t hdr[8];
  be32(hdr, len);
  memcpy(hdr + 4, type, 4);
  uLong crc = crc32(0, (const Bytef*)type, 4);
  if (len) crc = crc32(crc, data, len);
  uint8_t tail[4];
  be32(tail, (uint32_t)crc);
  return sceIoWrite(fd, hdr, 8) == 8 && (!len || sceIoWrite(fd, data, len) == (int)len) &&
         sceIoWrite(fd, tail, 4) == 4;
}

// Reads the displayed framebuffer (what the user sees) and saves it as PNG.
void save_screenshot() {
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !fb.base) {
    show_toast(T("Screenshot failed", "Falha na captura"), 2000);
    return;
  }
  const int w = fb.width ? (int)fb.width : kScreenW, h = fb.height ? (int)fb.height : kScreenH;
  const size_t row = (size_t)w * 3 + 1;
  uint8_t* raw = (uint8_t*)malloc(row * h);
  uLongf zcap = compressBound((uLong)(row * h));
  uint8_t* z = (uint8_t*)malloc(zcap);
  bool ok = false;
  char path[96];
  path[0] = 0;
  if (raw && z) {
    for (int y = 0; y < h; y++) {
      const uint32_t* src = (const uint32_t*)fb.base + (size_t)y * fb.pitch;
      uint8_t* dst = raw + row * y;
      dst[0] = 0;  // filter: none
      for (int x = 0; x < w; x++) {
        uint32_t px = src[x];  // A8B8G8R8: R in the low byte
        dst[1 + x * 3] = px & 255;
        dst[2 + x * 3] = (px >> 8) & 255;
        dst[3 + x * 3] = (px >> 16) & 255;
      }
    }
    if (compress2(z, &zcap, raw, (uLong)(row * h), 3) == Z_OK) {
      sceIoMkdir("ux0:picture", 0777);
      sceIoMkdir("ux0:picture/YouTubeVita", 0777);
      SceDateTime t;
      memset(&t, 0, sizeof(t));
      sceRtcGetCurrentClockLocalTime(&t);
      snprintf(path, sizeof(path), "ux0:picture/YouTubeVita/YTV_%04d%02d%02d_%02d%02d%02d.png",
               (int)t.year, (int)t.month, (int)t.day, (int)t.hour, (int)t.minute, (int)t.second);
      SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
      if (fd >= 0) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)w);
        be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;  // 8-bit RGB
        ok = sceIoWrite(fd, sig, 8) == 8 && write_chunk(fd, "IHDR", ihdr, 13) &&
             write_chunk(fd, "IDAT", z, (uint32_t)zcap) && write_chunk(fd, "IEND", nullptr, 0);
        sceIoClose(fd);
        if (!ok) sceIoRemove(path);
      }
    }
  }
  free(raw);
  free(z);
  char line[160];
  snprintf(line, sizeof(line), "YTV_LIFECYCLE_ADDON screenshot ok=%d path=%s\n", (int)ok, path);
  log_line(line);
  show_toast(ok ? T("Screenshot saved to Photos", "Captura salva em Fotos")
                : T("Screenshot failed", "Falha na captura"),
             2200, 1);
}

// ---- Idle path: paint into / restore the displayed framebuffer -------------
void restore_idle_paint() {
  if (!g_painted_base) return;
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  // Only restore when no frame was presented since (that frame has no panel).
  if (g_last_swap_us == g_painted_after_swap &&
      sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0 && fb.base == g_painted_base) {
    for (int y = 0; y < g_painted_h; y++)
      memcpy((uint32_t*)fb.base + (size_t)(g_painted_y + y) * fb.pitch + g_painted_x,
             g_backup + (size_t)y * g_painted_w * 4, (size_t)g_painted_w * 4);
  }
  g_painted_base = nullptr;
}

void idle_paint(uint64_t now) {
  // If Cobalt is presenting frames the normal GL path paints the overlay.
  if ((int64_t)(now-g_last_swap_us)<120000){
    g_painted_base=nullptr;
    return;
  }
  SceDisplayFrameBuf fb={};
  fb.size=sizeof(fb);
  if(sceDisplayGetFrameBuf(&fb,SCE_DISPLAY_SETBUF_NEXTFRAME)<0 || !fb.base)return;
  pthread_mutex_lock(&g_overlay_image_lock);
  const uint32_t gen=g_img_gen;
  const int panel_w=g_img_w,panel_h=g_img_h;
  const int panel_x=(kScreenW-panel_w)/2,panel_y=(kScreenH-panel_h)/2;
  const bool scrim=g_menu_open;
  const int x0=scrim?0:panel_x,y0=scrim?0:panel_y;
  const int w=scrim?kScreenW:panel_w,h=scrim?kScreenH:panel_h;
  const bool same_place=fb.base==g_painted_base &&
      g_last_swap_us==g_painted_after_swap &&
      x0==g_painted_x && y0==g_painted_y &&
      w==g_painted_w && h==g_painted_h;
  if(same_place && gen==g_painted_gen){
    pthread_mutex_unlock(&g_overlay_image_lock);
    return;
  }
  if(!same_place){
    restore_idle_paint();
    const size_t required=(size_t)w*h*4;
    if(g_backup_capacity<required){
      void* expanded=realloc(g_backup,required);
      if(!expanded){
        pthread_mutex_unlock(&g_overlay_image_lock);
        return;
      }
      g_backup=(uint8_t*)expanded;
      g_backup_capacity=required;
    }
    for(int y=0;y<h;y++)
      memcpy(g_backup+(size_t)y*w*4,
             (uint32_t*)fb.base+(size_t)(y0+y)*fb.pitch+x0,
             (size_t)w*4);
  }
  if(scrim){
    // Rebuild from the original backup rather than darkening repeatedly.
    for(int y=0;y<kScreenH;y++){
      uint32_t* dst=(uint32_t*)fb.base+(size_t)y*fb.pitch;
      const uint32_t* src=(const uint32_t*)(g_backup+(size_t)y*kScreenW*4);
      for(int x=0;x<kScreenW;x++)
        dst[x]=(src[x]&0xFF000000u)|((src[x]&0x00FEFEFEu)>>1);
    }
  }
  for(int y=0;y<panel_h;y++){
    uint32_t* dst=(uint32_t*)fb.base+(size_t)(panel_y+y)*fb.pitch+panel_x;
    const uint32_t* src=(const uint32_t*)(g_img+(size_t)y*panel_w*4);
    const uint32_t* under=(const uint32_t*)(g_backup+(size_t)(scrim?panel_y+y:y)*w*4);
    for(int x=0;x<panel_w;x++)
      if(src[x]>>24)dst[x]=src[x];
      else if(!scrim)dst[x]=under[x];
  }
  char line[144];
  snprintf(line,sizeof(line),
           "YTV_ADDON_IDLE_PAINT base=%p gen=%u scrim=%d since_swap_ms=%llu\n",
           fb.base,(unsigned)gen,(int)scrim,
           (unsigned long long)((now-g_last_swap_us)/1000));
  log_line(line);
  g_painted_base=fb.base;
  g_painted_gen=gen;
  g_painted_after_swap=g_last_swap_us;
  g_painted_x=x0;g_painted_y=y0;g_painted_w=w;g_painted_h=h;
  pthread_mutex_unlock(&g_overlay_image_lock);
}

// ---- Menu actions ----------------------------------------------------------
void open_page(Page page) {
  g_toast_until_ms=0;
  g_page=page;
  if(page==kPageDownloads){g_download_first=0;refresh_download_list();}
  g_sel=0;
  g_menu_open=true;
  compose_panel();
  const char* name = page == kPagePlayer ? "player" :
                     page == kPageDownload ? "download_format" :
                     page == kPageDownloads ? "downloads_list" :
                     page == kPageSettings ? "settings" :
                     page == kPageDisplay ? "display" : "home";
  char msg[80];
  snprintf(msg,sizeof(msg),"YTV_LIFECYCLE_ADDON menu=%s\n",name);
  log_line(msg);
}
void open_menu(bool open) {
  if (open) {
    open_page(kPageHome);
  } else if (g_menu_open) {
    g_menu_open=false;
    g_page=kPageNone;
    log_line("YTV_LIFECYCLE_ADDON menu=closed\n");
    if(g_timer_announce_pending && g_timer_deadline_us){
      g_timer_announce_pending=false;
      char message[90]={};
      snprintf(message,sizeof(message),
          g_pt ? "Dormir em %d minutos" : "Sleep in %d minutes",
          kTimerMinutes[g_timer_idx]);
      show_toast(message,2800,2);
      ytv_ui_sound(14); // HourlyChime, only when the Add-ons menu closes
      log_line("YTV_SLEEP_TIMER_ENABLED_TOAST\n");
    }
  }
}
void menu_back() {
  if (g_page == kPageHome) open_menu(false);
  else if (g_page == kPageDownload) open_page(kPagePlayer);
  else if (g_page == kPageDownloads) open_page(g_downloads_return);
  else if (g_page == kPageNetwork) {
    open_menu(false);
    g_network_next_prompt=now_us()+6000000ull;
  }
  else if (g_page == kPagePlayer && g_player_return == kPageNone) open_menu(false);
  else if (g_page == kPagePlayer && g_player_return == kPageSettings)
    open_page(kPageSettings);
  else open_page(kPageHome);
}
void update_language() {
  g_pt = g_language_pref == 0 ? g_system_pt : g_language_pref == 1;
}

void set_timer(int idx) {
  g_timer_idx = (idx + kTimerChoices) % kTimerChoices;
  // DIAGNOSTIC: ux0:data/youtubevita/addon_timer_seconds.txt counts the timer
  // in seconds instead of minutes (tests only).
  static int unit_s = -1;
  if (unit_s < 0) {
    SceIoStat st;
    unit_s = sceIoGetstat("ux0:data/youtubevita/addon_timer_seconds.txt", &st) >= 0 ? 1 : 60;
  }
  g_timer_deadline_us = kTimerMinutes[g_timer_idx]
                            ? now_us() + (uint64_t)kTimerMinutes[g_timer_idx] * unit_s * 1000000ull
                            : 0;
  g_timer_announce_pending=g_timer_deadline_us!=0;
  g_timer_final_alert_played=false;
  g_timer_countdown_visible=false;
  char line[64];
  snprintf(line, sizeof(line), "YTV_LIFECYCLE_ADDON sleep_timer=%dmin\n", kTimerMinutes[g_timer_idx]);
  log_line(line);
}

void save_cfg() {
  sceIoMkdir("ux0:data/youtubevita",0777);
  SceUID fd=sceIoOpen(kAddonCfg,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
  if (fd<0) return;
  char buf[320];
  const int n=snprintf(buf,sizeof(buf),
      "lr=%d\nadblock=%d\nquality=%s\naudio_only=%d\nlanguage=%d\nfhd=%d\ndim=%d\nperf=%d\nclock=%d\nui_sound=%d\nui_volume=%d\nvideo_filter=%s\nvideo_aspect=%d\nmedia_volume=%d\nvideo_vertical=%d\nscreen_flip=%d\n",
      g_lr_enabled?1:0,__atomic_load_n(&g_ytv_adblock_enabled,__ATOMIC_RELAXED),
      kQualityValues[g_quality_idx],g_audio_only?1:0,g_language_pref,
      g_fhd_cfg?1:0,g_dim_idx,g_perf_hud?1:0,g_clock_hud?1:0,g_ui_sound?1:0,g_ui_volume,
      __atomic_load_n(&g_ytv_video_bilinear_enabled,__ATOMIC_RELAXED) ? "bilinear" : "nearest",
       __atomic_load_n(&g_ytv_video_aspect_mode,__ATOMIC_RELAXED),
       __atomic_load_n(&g_ytv_media_volume_pct,__ATOMIC_RELAXED),
       __atomic_load_n(&g_ytv_video_vertical_mode,__ATOMIC_RELAXED),
       __atomic_load_n(&g_ytv_screen_flip,__ATOMIC_RELAXED));
  if (n>0 && n<(int)sizeof(buf)) sceIoWrite(fd,buf,n);
  sceIoClose(fd);
}
void load_cfg() {
  SceUID fd=sceIoOpen(kAddonCfg,SCE_O_RDONLY,0);
  if (fd<0) return;
  char buf[320]={};
  const int n=sceIoRead(fd,buf,sizeof(buf)-1);
  sceIoClose(fd);
  if (n<=0) return;
  if (strstr(buf,"lr=0")) g_lr_enabled=false;
  __atomic_store_n(&g_ytv_adblock_enabled,ytv_adblock_config_enabled(buf,(size_t)n),__ATOMIC_RELAXED);
  if (strstr(buf,"quality=360")) g_quality_idx=0;
  if (strstr(buf,"quality=480")) g_quality_idx=1;
  if (strstr(buf,"quality=720")) g_quality_idx=2;
  if (strstr(buf,"audio_only=1")) g_audio_only=true;
  if (strstr(buf,"language=1")) g_language_pref=1;
  if (strstr(buf,"language=2")) g_language_pref=2;
  if (strstr(buf,"fhd=1")) g_fhd_cfg=true;
  if (const char* d=strstr(buf,"dim=")) { int v=atoi(d+4); if (v>=0 && v<kDimChoices) g_dim_idx=v; }
  if (strstr(buf,"perf=1")) g_perf_hud=true;
  if (strstr(buf,"clock=1")) g_clock_hud=true;
  if (strstr(buf,"ui_sound=1")) g_ui_sound=true;
  if (strstr(buf,"ui_sound=0")) g_ui_sound=false;
  if (const char* v=strstr(buf,"ui_volume=")){
    const int n=atoi(v+10);
    if(n>=0 && n<=100)g_ui_volume=n;
  }
  if(const char* v=strstr(buf,"video_aspect=")){
    const int mode=atoi(v+13);
    if(mode>=0 && mode<=2)__atomic_store_n(&g_ytv_video_aspect_mode,mode,__ATOMIC_RELAXED);
  }
  if (strstr(buf,"screen_flip=1")) __atomic_store_n(&g_ytv_screen_flip,1,__ATOMIC_RELAXED);
  if(const char* v=strstr(buf,"video_vertical=")){
    const int mode=atoi(v+15);
    if(mode>=0 && mode<=2)__atomic_store_n(&g_ytv_video_vertical_mode,mode,__ATOMIC_RELAXED);
  }
  if(const char* v=strstr(buf,"media_volume=")){
    const int volume=atoi(v+13);
    if(volume>=0 && volume<=100)__atomic_store_n(&g_ytv_media_volume_pct,volume,__ATOMIC_RELAXED);
  }
  if (strstr(buf,"video_filter=nearest"))
    __atomic_store_n(&g_ytv_video_bilinear_enabled,0,__ATOMIC_RELAXED);
}

void set_lr(bool on) {
  g_lr_enabled = on;
  save_cfg();
  char line[48];
  snprintf(line, sizeof(line), "YTV_LIFECYCLE_ADDON lr_buttons=%d\n", (int)on);
  log_line(line);
}

void toggle_adblock() {
  const int next=!__atomic_load_n(&g_ytv_adblock_enabled,__ATOMIC_RELAXED);
  __atomic_store_n(&g_ytv_adblock_enabled,next,__ATOMIC_RELAXED);
  save_cfg();
  log_line(next?"YTV_ADBLOCK enabled (limited DNS filter)\n":"YTV_ADBLOCK disabled\n");
}
void adjust_item(int delta) {
  if (g_page==kPagePlayer && g_sel==2) {
    set_timer(g_timer_idx+delta);
  } else if (g_page==kPagePlayer && g_sel==4) {
    g_dim_idx=(g_dim_idx+kDimChoices+delta)%kDimChoices;
    save_cfg();
    char msg[64];
    snprintf(msg,sizeof(msg),"YTV_LIFECYCLE_ADDON dim_percent=%d\n",kDimLevels[g_dim_idx]);
    log_line(msg);
  } else if (g_page==kPagePlayer && g_sel==6) {
    const int mode=(__atomic_load_n(&g_ytv_video_aspect_mode,__ATOMIC_RELAXED)+3+delta)%3;
    __atomic_store_n(&g_ytv_video_aspect_mode,mode,__ATOMIC_RELAXED);
    save_cfg();
    char msg[96];snprintf(msg,sizeof(msg),"YTV_LIFECYCLE_VIDEO_ASPECT mode=%s\n",
      mode==1?"zoom":mode==2?"stretch":"original");
    log_line(msg);
  } else if (g_page==kPagePlayer && g_sel==7) {
    const int mode=(__atomic_load_n(&g_ytv_video_vertical_mode,__ATOMIC_RELAXED)+3+delta)%3;
    __atomic_store_n(&g_ytv_video_vertical_mode,mode,__ATOMIC_RELAXED);
    save_cfg();
    char msg[80];snprintf(msg,sizeof(msg),"YTV_LIFECYCLE_VIDEO_VERTICAL mode=%s\n",
      mode==1?"cw":mode==2?"ccw":"off");
    log_line(msg);
  } else if (g_page==kPagePlayer && g_sel==5) {
    const bool enabled=!__atomic_load_n(&g_ytv_video_bilinear_enabled,__ATOMIC_RELAXED);
    __atomic_store_n(&g_ytv_video_bilinear_enabled,(int)enabled,__ATOMIC_RELAXED);
    save_cfg();
    // Apply only to new video targets, not to currently bound GL textures.
    // The previous live mutation path correlated with a real Vita crash.
    log_line(enabled?"YTV_LIFECYCLE_VIDEO_FILTER mode=bilinear apply=next_video\n":
                     "YTV_LIFECYCLE_VIDEO_FILTER mode=nearest apply=next_video\n");
  } else if (g_page==kPageSettings) {
    switch(g_sel) {
      case 0: toggle_adblock(); break;
      case 1: g_quality_idx=(g_quality_idx+3+delta)%3;save_cfg();break;
      case 2: g_audio_only=!g_audio_only;save_cfg();break;
      case 3: set_lr(!g_lr_enabled);break;
      case 5: g_language_pref=(g_language_pref+3+delta)%3;
              update_language();save_cfg();break;
      case 6: {
        const int on=!__atomic_load_n(&g_ytv_screen_flip,__ATOMIC_RELAXED);
        __atomic_store_n(&g_ytv_screen_flip,on,__ATOMIC_RELAXED);
        save_cfg();
        log_line(on?"YTV_LIFECYCLE_SCREEN_FLIP on=1\n":"YTV_LIFECYCLE_SCREEN_FLIP on=0\n");
        break;
      }
    }
  } else if (g_page==kPageDisplay) {
    char msg[64];
    switch(g_sel) {
      case 0: g_fhd_cfg=!g_fhd_cfg;save_cfg();
              snprintf(msg,sizeof(msg),"YTV_LIFECYCLE_ADDON fhd_next_start=%d\n",(int)g_fhd_cfg);
              log_line(msg);break;
      case 1: g_perf_hud=!g_perf_hud;save_cfg();break;
      case 2: g_clock_hud=!g_clock_hud;save_cfg();break;
      case 3: g_ui_sound=!g_ui_sound;ytv_ui_sound_enable(g_ui_sound);save_cfg();
              break;
      case 4:
              g_ui_volume+=10*delta;
              if(g_ui_volume<0)g_ui_volume=0;
              if(g_ui_volume>100)g_ui_volume=100;
              ytv_ui_sound_set_volume(g_ui_volume);
              save_cfg();
              break;
    }
  }
  compose_panel();
}
// The format is chosen HERE, not implicitly from Settings. Confirm with X.
// Existing Settings value is updated too so URL keyboard fallback uses the
// same requested format when MediaSession has no verified video ID.
void begin_selected_download(bool mp3){
  g_audio_only=mp3;
  save_cfg();
#if defined(YTV_NATIVE_DOWNLOAD_TEST)
  // Diagnostic-only override for short reproducible Vita3K downloads.
  char url[128]={};
  SceUID fd=sceIoOpen("ux0:data/youtubevita/native_test_url.txt",SCE_O_RDONLY,0);
  if(fd>=0){
    int n=sceIoRead(fd,url,sizeof(url)-1);
    sceIoClose(fd);
    if(n>0){
      url[n]=0;
      char*e=strpbrk(url,"\r\n");
      if(e)*e=0;
      ytv_native_download_set_video(url);
    }
  }
#endif
  if(!ytv_native_download_has_video()){
    open_menu(false);
    if(!ytv_osk_open_native_video_url())
      show_toast(T("Video URL keyboard unavailable",
                   "Teclado URL indisponível"),2100,0);
    return;
  }
  const int rc=ytv_native_download_start(mp3,
                                         atoi(kQualityValues[g_quality_idx]));
  open_menu(false);
  if(rc==1)
    show_toast(mp3?T("Downloading MP3","Baixando MP3"):
                   T("Downloading MP4","Baixando MP4"),2000,0);
  else if(rc==2)
    show_toast(T("Added to download queue","Adicionado à fila"),2100,0);
  else if(rc==0)
    show_toast(T("Already downloading or queued",
                 "Já está baixando ou na fila"),2100,0);
  else if(rc==-3)
    show_toast(T("Download queue is full","Fila de downloads cheia"),2100,0);
  else
    show_toast(T("Could not start download","Não foi possível baixar"),2100,0);
}

void activate(int item) {
  if(g_page==kPageNetwork){
    switch(item){
      case 0:
        g_network_offline=SbSystemNetworkIsDisconnected();
        if(g_network_offline){
          log_line("YTV_NETWORK_NATIVE_RETRY denied=offline\n");
          compose_panel();
        }else{
          const int result=ytv_network_platform_error_retry();
          g_network_auto_retry_done=true;
          g_network_seen_offline=false;
          g_network_next_prompt=now_us()+10000000ull;
          open_menu(false);
          log_line(result==1?"YTV_NETWORK_NATIVE_RETRY action=cobalt_callback\n":
                              "YTV_NETWORK_NATIVE_RETRY action=connected_event\n");
          show_toast(T("Reconnecting YouTube...", "Reconectando ao YouTube..."),
                     2600,0);
        }
        break;
      case 1: {
        const int result=sceAppMgrLaunchAppByUri(0xFFFFF,"settings_dlg:");
        char line[120];
        snprintf(line,sizeof(line),
                 "YTV_NETWORK_NATIVE_SETTINGS result=0x%x\n",(unsigned)result);
        log_line(line);
        if(result<0)compose_panel();
        break;
      }
      default:menu_back();break;
    }
  } else if (g_page==kPageHome) {
    switch (item) {
      case 0: g_player_return=kPageHome;open_page(kPagePlayer);break;
      case 1: g_downloads_return=kPageHome;open_page(kPageDownloads);break;
      case 2: open_page(kPageSettings);break;
      case 3: open_page(kPageDisplay);break;
      case 4: {
        // The Vita Settings app opens on top; YouTube stays open in the
        // background and comes back from the LiveArea / PS button.
        open_menu(false);
        int r = sceAppMgrLaunchAppByUri(0xFFFFF, "settings_dlg:");
        char msg[64];
        snprintf(msg,sizeof(msg),"YTV_LIFECYCLE_ADDON vita_settings r=0x%x\n",(unsigned)r);
        log_line(msg);
        if (r < 0) show_toast(T("Could not open Vita Settings","Não deu para abrir as Configurações"),2500,0);
        break;
      }
      default: open_menu(false);break;
    }
  } else if (g_page==kPagePlayer) {
    switch(item) {
      case 0: open_page(kPageDownload);break;
      case 1: open_menu(false);g_shot_pending=true;
              g_shot_at_us=now_us()+250000;break;
      case 2: adjust_item(1);break;
      case 3: open_menu(false);
              show_toast(T("Black Screen: SELECT to return",
                           "Tela preta: SELECT para voltar"),1800,3);
              g_screen_off_at_us=now_us()+1800000;break;
      case 4: case 5: case 6: case 7: adjust_item(1);break;
      default: menu_back();break;
    }
  } else if (g_page==kPageDownload) {
    switch(item){
      case 0: begin_selected_download(false);break; // Video MP4
      case 1: begin_selected_download(true);break;  // Audio MP3
      case 2: g_downloads_return=kPageDownload;open_page(kPageDownloads);break;
      case 3: {
        const bool cancelled=ytv_native_download_cancel_current();
        open_menu(false);
        show_toast(cancelled?
                   T("Cancelling current download", "Cancelando download atual"):
                   T("No active download", "Nenhum download em andamento"),
                   2000,0);
        break;
      }
      default: menu_back();break;
    }
  } else if (g_page==kPageDownloads) {
    const int n=download_visible();
    if(item==n){
      if(g_download_count<=kDownloadsVisible)refresh_download_list();
      else {
        g_download_first=(g_download_first+kDownloadsVisible>=g_download_count)
                            ? 0:g_download_first+kDownloadsVisible;
      }
      g_sel=0;
      compose_panel();
      log_line("YTV_LIFECYCLE_ADDON downloads_view_more\n");
    } else if(item==n+1)menu_back();
  } else if (g_page==kPageSettings) {
    switch(item) {
      case 0: case 1: case 2: case 3: case 5: case 6: adjust_item(1);break;
      case 4: open_menu(false);g_input_locked=true;
              ytv_ui_sound(12);  // controls lock
              log_line("YTV_LIFECYCLE_ADDON input_locked=1\n");
              show_toast(T("Input locked. SELECT unlocks",
                           "Entrada bloqueada. SELECT desbloqueia"),2500,4);break;
      default: menu_back();break;
    }
  } else if (g_page==kPageDisplay) {
    if (item>=0 && item<=4) adjust_item(1);
    else menu_back();
  }
}

// ---- On-screen info (HUD) -------------------------------------------------------
// Composed on the main thread once a second, drawn by the GL path every frame.
constexpr int kHudW = 340, kHudH = 180;
uint8_t* g_hud = nullptr;
volatile int g_hud_w = 0, g_hud_h = 0;
volatile uint32_t g_hud_gen = 0;
volatile int g_fps = 0, g_worst_ms = 0;  // written by the renderer thread
int g_dropped_per_s = 0;

void compose_hud() {
  if (!g_hud) return;
  char lines[8][64];
  int n = 0;
  if (g_clock_hud) {
    SceDateTime t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    int batt = scePowerGetBatteryLifePercent();
    bool charging = scePowerIsBatteryCharging() > 0;
    if (batt >= 0 && batt <= 100)
      snprintf(lines[n++], 64, "%02d:%02d   %d%%%s", (int)t.hour, (int)t.minute, batt, charging ? " +" : "");
    else
      snprintf(lines[n++], 64, "%02d:%02d", (int)t.hour, (int)t.minute);
  }
  if (g_perf_hud) {
    SceKernelFreeMemorySizeInfo m;
    memset(&m, 0, sizeof(m));
    m.size = sizeof(m);
    sceKernelGetFreeMemorySize(&m);
    snprintf(lines[n++], 64, "%d fps   %s %d ms", g_fps, T("worst", "pior"), g_worst_ms);
    snprintf(lines[n++], 64, "RAM %d MB   VRAM %d MB", m.size_user >> 20, m.size_cdram >> 20);
    snprintf(lines[n++], 64, "GL: VRAM %d  RAM %d  PHY %d MB", (int)(g_vgl_free_vram >> 20),
             (int)(g_vgl_free_ram >> 20), (int)(g_vgl_free_phy >> 20));
    if (g_ytv_hud_video_h > 0)
      snprintf(lines[n++], 64, "%s %dp   %s %d/s", T("video", "vídeo"), g_ytv_hud_video_h,
               T("dropped", "perdidos"), g_dropped_per_s);
  }
  if (g_timer_countdown_visible && n<8) {
    const uint64_t now=now_us();
    int secs=g_timer_deadline_us>now
      ? (int)((g_timer_deadline_us-now+999999ull)/1000000ull):0;
    if(secs>60)secs=60;
    snprintf(lines[n++],64,g_pt?"Tela preta em %02d s":"Sleep in %02d s",secs);
  }
  if (!n) { g_hud_w = 0; return; }
  int w = 0;
  for (int i = 0; i < n; i++) { int tw = text_width(lines[i]); if (tw > w) w = tw; }
  w += 24;
  if (w > kHudW) w = kHudW;
  const int h = 8 + n * 22;
  pthread_mutex_lock(&g_overlay_image_lock);
  Canvas c = {g_hud, w, h};
  base_panel(c, 0x000000, 10);
  for (int i = 0; i < w * h; i++) if (g_hud[i * 4 + 3]) g_hud[i * 4 + 3] = 170;  // translucent
  for (int i = 0; i < n; i++) draw_text(c, 12, 3 + i * 22, lines[i], i == 0 && g_clock_hud ? kText : 0xdedede);
  g_hud_w = w;
  g_hud_h = h;
  __sync_add_and_fetch(&g_hud_gen, 1);
  pthread_mutex_unlock(&g_overlay_image_lock);
}

}  // namespace

// Set by the player's audio thread whenever it outputs audio.
volatile uint64_t g_ytv_audio_active_us = 0;

extern "C" {

// Safe cross-thread completion signal. The downloader does not touch the
// Add-ons canvas or GPU. One atomic increment per successfully saved MP3/MP4.
void ytv_addons_download_completed(){
  __sync_add_and_fetch(&g_downloads_completed,1u);
}
void ytv_addons_download_quality_fallback(int requested,int actual){
  if(requested<=actual || actual<1 || requested<1)return;
  __atomic_store_n(&g_mp4_fallback_requested,requested,__ATOMIC_RELEASE);
  __atomic_store_n(&g_mp4_fallback_actual,actual,__ATOMIC_RELEASE);
  __sync_add_and_fetch(&g_mp4_fallback_serial,1u);
}

// Main thread, every input poll, before buttons reach Cobalt. Returns true
// when the add-on layer consumed this poll (Cobalt then sees no change).
bool ytv_addons_input(uint32_t previous, uint32_t current) {
  const uint32_t pressed=current & ~previous;
  if (g_input_locked) {
    if (pressed&SCE_CTRL_SELECT) {
      g_input_locked=false;
      log_line("YTV_LIFECYCLE_ADDON input_locked=0\n");
      show_toast(T("Input unlocked","Entrada liberada"),1500,4);
    }
    return pressed!=0;
  }
  if (g_screen_off) {
    if (pressed&SCE_CTRL_SELECT) set_screen_off(false);
    return pressed!=0;
  }
  // SELECT always enters the native Vita Home / Settings hub.
  if (pressed&SCE_CTRL_SELECT) {
    ytv_ui_sound(g_menu_open ? 3 : 6);
    open_menu(!g_menu_open);
    return true;
  }
  // Triangle opens a lightweight context menu ONLY while an SbPlayer exists.
  // It remains available even when playback is paused, and falls through to
  // YouTube on non-player screens (no global Triangle hijack).
  if ((pressed&SCE_CTRL_TRIANGLE) && ytv_native_player_present()) {
    ytv_ui_sound(g_menu_open && (g_page==kPagePlayer || g_page==kPageDownload) ? 3 : 6);
    if (g_menu_open && (g_page==kPagePlayer || g_page==kPageDownload)) {
      menu_back();
    } else {
      g_player_return=g_menu_open?g_page:kPageNone;
      open_page(kPagePlayer);
    }
    return true;
  }
  if (!g_menu_open) return false;
  if (pressed&SCE_CTRL_UP) {g_sel=(g_sel+page_count()-1)%page_count();ytv_ui_sound(1);}
  if (pressed&SCE_CTRL_DOWN) {g_sel=(g_sel+1)%page_count();ytv_ui_sound(1);}
  if (pressed&(SCE_CTRL_LEFT|SCE_CTRL_RIGHT)) {
    const int delta=(pressed&SCE_CTRL_LEFT)?-1:1;
    if(g_page==kPageDownloads && g_download_count>kDownloadsVisible){
      const int pages=(g_download_count+kDownloadsVisible-1)/kDownloadsVisible;
      g_download_first=((g_download_first/kDownloadsVisible+pages+delta)%pages)*kDownloadsVisible;
      g_sel=0;
      ytv_ui_sound(1);
    }
    if ((g_page==kPagePlayer && (g_sel==2 || g_sel==4 || g_sel==5 || g_sel==6 || g_sel==7)) ||
        (g_page==kPageSettings && (g_sel==0 || g_sel==1 ||
                                  g_sel==2 || g_sel==3 || g_sel==5 || g_sel==6)) ||
        (g_page==kPageDisplay && g_sel<=4))
      {adjust_item(delta);ytv_ui_sound(1);}
  }
  if (pressed&SCE_CTRL_CIRCLE) { ytv_ui_sound(3);menu_back();return true; }
  if (pressed&SCE_CTRL_CROSS) { ytv_ui_sound(2);activate(g_sel);return true; }
  if (pressed) compose_panel();
  return true;
}

int ytv_addons_cfg_download_quality() {return atoi(kQualityValues[g_quality_idx]);}
bool ytv_addons_cfg_download_mp3() {return g_audio_only;}

bool ytv_addons_menu_open() { return g_menu_open || g_screen_off || g_input_locked; }
// Menu or toast on screen: Cobalt is asked to keep presenting frames
// (graphics extension in port_system.cpp), so the overlay is always drawn by
// the GL path on top of a fresh frame. The CPU idle path painted pages over
// copies that could already contain the previous page (stacked menus).
bool ytv_addons_wants_frames() {
  return overlay_visible() || g_timer_countdown_visible;
}

void ytv_addons_adjust_media(int volume_delta,int brightness_delta){
  if(g_input_locked || g_screen_off || g_menu_open)return;
  char message[80]={};
  if(volume_delta){
    int volume=__atomic_load_n(&g_ytv_media_volume_pct,__ATOMIC_RELAXED)+volume_delta;
    if(volume<0)volume=0;
    if(volume>100)volume=100;
    __atomic_store_n(&g_ytv_media_volume_pct,volume,__ATOMIC_RELAXED);
    snprintf(message,sizeof(message),g_pt?"Volume: %d%%":"Volume: %d%%",volume);
  }else if(brightness_delta){
    // The VitaSDK exposes no user-mode backlight setter. Reuse the existing
    // native overlay dimming, with non-wrapping steps (original -> 90% dim).
    int idx=g_dim_idx-brightness_delta;
    if(idx<0)idx=0;
    if(idx>=kDimChoices)idx=kDimChoices-1;
    g_dim_idx=idx;
    snprintf(message,sizeof(message),g_pt?"Brilho: %d%%":"Brightness: %d%%",
             100-kDimLevels[g_dim_idx]);
  }else return;
  show_toast(message,1200,0);
  char log[128];snprintf(log,sizeof(log),"YTV_V063_ANALOG_ADJUST %s\n",message);
  log_line(log);
  // Avoid disk writes on every held-stick step; persist only after a pause.
  // save_cfg is scheduled by the main-thread tick below.
  g_media_cfg_dirty_since=now_us();
}
void ytv_addons_toggle() {
  if (g_input_locked || g_screen_off) return;
  ytv_ui_sound(g_menu_open?3:6);
  open_menu(!g_menu_open);
}

// Buttons YouTube must not see (L/R disabled in the menu).
uint32_t ytv_addons_hidden_buttons() {
  return g_lr_enabled ? 0u : (uint32_t)(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
}

// A tap (window pixels) while the menu is open: pick a row or close.
void ytv_addons_tap(float x, float y) {
  if (g_input_locked || g_screen_off || !g_menu_open) return;
  const int px=(int)x-(kScreenW-kPanelW)/2;
  const int py=(int)y-(kScreenH-panel_height())/2;
  if (px<0 || py<0 || px>=kPanelW || py>=panel_height()) {
    ytv_ui_sound(3);
    menu_back();
    return;
  }
  for (int i=0;i<page_count();i++) {
    const int ry=row_y(i),rh=row_h(i);
    if (py>=ry && py<ry+rh) {
      g_sel=i;
      // Physical touch is already sounded once in port_input; don't replay
      // confirm over the focus_change_small cue on the same gesture.
      if (g_page==kPageDisplay && i==4 &&
          py>=ry+kRowH && px>=kRowX0+50){
        const int x0=kRowX0+62,x1=kPanelW-kRowX0-30;
        g_ui_volume=(px-x0)*100/(x1-x0);
        if(g_ui_volume<0)g_ui_volume=0;
        if(g_ui_volume>100)g_ui_volume=100;
        ytv_ui_sound_set_volume(g_ui_volume);
        save_cfg();
        compose_panel();
      } else if (g_page==kPagePlayer && i==2 && px<kPanelW/3)
        adjust_item(-1);
      else if (g_page==kPageSettings &&
               (i==0 || i==1 || i==2 || i==3 || i==5 || i==6) &&
               px<kPanelW/3)
        adjust_item(-1);
      else if (g_page==kPagePlayer && (i==4 || i==5 || i==6 || i==7) && px<kPanelW/3)
        adjust_item(-1);
      else activate(i);
      return;
    }
  }
}

// Full HD is only available on the Full (extended-memory) VPK and takes
// effect after restart. Read the setting before the Add-ons UI initializes.
// Crash guard v0.60: a persistent probe is armed only while an actual 1080p
// decoder is being constructed/used. Simply starting and closing the app
// with Full HD enabled must NEVER trigger the previous-run crash guard.
constexpr const char* kFhdProbe = "ux0:data/youtubevita/fhd_probe";
int g_fhd_state = -1;            // -1 unknown, 0 off, 1 on
bool g_fhd_auto_off = false;
int g_fhd_guard_owners = 0;      // players with a Full HD decoder or attempt
bool g_fhd_probe_present = false;
pthread_mutex_t g_fhd_guard_mutex = PTHREAD_MUTEX_INITIALIZER;
constexpr const char kFhdProbeSignature[]="YTV60FHD\n";
int ytv_addons_cfg_fhd() {
  if (g_fhd_state >= 0) return g_fhd_state;
  g_fhd_state = 0;
  SceUID fd = sceIoOpen(kAddonCfg, SCE_O_RDONLY, 0);
  if (fd < 0) return 0;
  char buf[256] = {};
  int n=sceIoRead(fd, buf, sizeof(buf)-1);
  sceIoClose(fd);
  if (n<=0) return 0;
  if (!strstr(buf, "fhd=1")) {
    // Never leave an old probe behind when the user has chosen Off.
    sceIoRemove(kFhdProbe);
    return 0;
  }
  SceUID probe = sceIoOpen(kFhdProbe,SCE_O_RDONLY,0);
  if(probe>=0){
    char marker[sizeof(kFhdProbeSignature)]={};
    int n=sceIoRead(probe,marker,sizeof(kFhdProbeSignature)-1);
    sceIoClose(probe);
    sceIoRemove(kFhdProbe);
    // Old releases created a ZERO-byte marker merely by starting the app.
    // Treat only versioned probes of actual v0.60 decoder attempts as a
    // previous-run 1080p failure; never auto-disable on a stale v0.59 file.
    if(n==(int)sizeof(kFhdProbeSignature)-1 &&
       memcmp(marker,kFhdProbeSignature,sizeof(kFhdProbeSignature)-1)==0){
      g_fhd_auto_off = true;
      char* at = strstr(buf, "fhd=1");
      at[4] = '0';
      SceUID w=sceIoOpen(kAddonCfg,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
      if(w>=0){sceIoWrite(w,buf,strlen(buf));sceIoClose(w);}
      SbLogRaw("YTV_LIFECYCLE_ADDON fhd=auto_off previous_1080p_probe=1\n");
      return 0;
    }
    SbLogRaw("YTV_LIFECYCLE_ADDON fhd=legacy_probe_ignored\n");
  }
  g_fhd_state = 1;
  SbLogRaw("YTV_LIFECYCLE_ADDON fhd=enabled guard=deferred_until_1080p_decoder\n");
  return 1;
}
// Called on the decoder/player setup thread, NOT per frame.
void ytv_addons_fhd_begin() {
  if(g_fhd_state!=1)return;
  pthread_mutex_lock(&g_fhd_guard_mutex);
  if(g_fhd_guard_owners++ > 0){
    pthread_mutex_unlock(&g_fhd_guard_mutex);
    return;
  }
  sceIoMkdir("ux0:data/youtubevita",0777);
  SceUID fd=sceIoOpen(kFhdProbe,SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC,0666);
  int wrote=fd>=0?sceIoWrite(fd,kFhdProbeSignature,sizeof(kFhdProbeSignature)-1):-1;
  if(fd>=0)sceIoClose(fd);
  g_fhd_probe_present=wrote==(int)sizeof(kFhdProbeSignature)-1;
  if(!g_fhd_probe_present)sceIoRemove(kFhdProbe);
  pthread_mutex_unlock(&g_fhd_guard_mutex);
  SbLogRaw(g_fhd_probe_present ?
      "YTV_LIFECYCLE_ADDON fhd=probe_armed version=60 real_1080p=1\n" :
      "YTV_LIFECYCLE_ADDON fhd=probe_unavailable\n");
}
// On a normal player tear-down, release the guard. Leaving/reopening an app
// must not be classified as a crash if SbPlayerDestroy completed normally.
void ytv_addons_fhd_end() {
  if(g_fhd_state!=1)return;
  pthread_mutex_lock(&g_fhd_guard_mutex);
  if(g_fhd_guard_owners<=0){
    pthread_mutex_unlock(&g_fhd_guard_mutex);
    return;
  }
  if(--g_fhd_guard_owners>0){
    pthread_mutex_unlock(&g_fhd_guard_mutex);
    return;
  }
  if(g_fhd_probe_present){
    sceIoRemove(kFhdProbe);
    g_fhd_probe_present=false;
  }
  pthread_mutex_unlock(&g_fhd_guard_mutex);
  SbLogRaw("YTV_LIFECYCLE_ADDON fhd=normal_decoder_close\n");
}
// The hardware decoded at least thirty 1080p pictures: clear the crash guard
// for this decoder without claiming all 1080p content/bitrates will work.
void ytv_addons_fhd_ok() {
  if(g_fhd_state!=1)return;
  pthread_mutex_lock(&g_fhd_guard_mutex);
  const bool armed=g_fhd_probe_present;
  if(armed){
    sceIoRemove(kFhdProbe);
    g_fhd_probe_present=false;
  }
  pthread_mutex_unlock(&g_fhd_guard_mutex);
  if(armed)SbLogRaw("YTV_LIFECYCLE_ADDON fhd=decoded_30_frames\n");
}

// Load the DNS preference before Cobalt can issue its first request.
void ytv_addons_load_adblock_config() {
  char buf[320]={};
  SceUID fd=sceIoOpen(kAddonCfg,SCE_O_RDONLY,0);
  int n=0;
  if(fd>=0){n=sceIoRead(fd,buf,sizeof(buf)-1);sceIoClose(fd);}
  __atomic_store_n(&g_ytv_adblock_enabled,
      ytv_adblock_config_enabled(buf,n>0?(size_t)n:0),__ATOMIC_RELAXED);
}

// Main thread, every loop iteration.
void ytv_addons_tick() {
  static bool inited = false;
  if (!inited) {
    inited = true;
    int lang = 0;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang) >= 0)
      g_system_pt = lang == SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR || lang == SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT;
    g_img = (uint8_t*)malloc((size_t)kPanelW * kPanelH * 4);
    g_hud = (uint8_t*)malloc((size_t)kHudW * kHudH * 4);
    load_cfg();
    update_language();
    if (g_fhd_auto_off) g_fhd_cfg = false;  // the guard turned it off this start
    g_fhd_boot = g_fhd_cfg;
    ytv_ui_sound_set_volume(g_ui_volume);
    ytv_ui_sound_enable(g_ui_sound);
    if (g_fhd_auto_off)
      show_toast(T("Full HD failed last time: turned off", "Full HD falhou da última vez: desligado"),
                 4000, 0);
  }
  if (!g_img) return;
#if defined(YTV_NATIVE_DOWNLOAD_TEST)
  static bool unit_tests_once=false;
  if(!unit_tests_once){
    unit_tests_once=true;
    char report[120];
    snprintf(report,sizeof(report), "YTV_DOWNLOAD_DIAGNOSTICS metadata=%d quality=%d media_size=%d\n",
      ytv_metadata_selftest(),ytv_download_quality_selftest(),
      ytv_media_size_selftest());
    SbLogRaw(report);
  }
  // Dedicated Vita3K smoke hook, compiled OUT of all public/local normal
  // builds. Only starts when the explicit test URL marker exists.
  static bool test_once=false;
  // Only start the MP3 test when H.264 picture decoding is already active.
  // This deliberately stresses video + download concurrently (the user's
  // artifact report), rather than downloading during the initial menu.
  if(!test_once && now_us()>=12000000ull &&
     ytv_native_player_present() && g_ytv_hud_video_h>0){
    test_once=true;
    char input[128]={};
    SceUID f=sceIoOpen("ux0:data/youtubevita/native_test_url.txt",SCE_O_RDONLY,0);
    if(f>=0){
      int n=sceIoRead(f,input,sizeof(input)-1);
      sceIoClose(f);
      if(n>0){
        input[n]=0;
        char*stop=strpbrk(input,"\r\n");
        if(stop)*stop=0;
        if(ytv_native_download_set_video(input)){
          const int test_height=g_ytv_hud_video_h;
          SceIoStat force_video={};
          const bool video_480=sceIoGetstat(
              "ux0:data/youtubevita/test_force_video480.txt",&force_video)>=0;
          int test_rc=ytv_native_download_start(!video_480,video_480?480:360);
          char line[155];snprintf(line,sizeof(line),
            "YTV_LIFECYCLE_TEST_DOWNLOAD_AUTOSTART rc=%d mp4_480=%d player_active=%d video_h=%d\n",
            test_rc,(int)video_480,(int)ytv_native_player_present(),test_height);
          log_line(line);
        }
      }
    }
  }
#endif
  uint64_t now = now_us();
  if(now-g_network_last_check>=1000000ull){
    g_network_last_check=now;
    const bool offline=SbSystemNetworkIsDisconnected();
    const bool previous=g_network_offline;
    g_network_offline=offline;
    const unsigned generation=ytv_network_platform_error_generation();
    const bool error_pending=ytv_network_platform_error_pending()!=0;
    const bool new_error=generation!=g_network_error_seen;
    if(new_error)g_network_error_seen=generation;
    if(offline){
      g_network_seen_offline=true;
      g_network_online_since=0;
      g_network_auto_retry_done=false;
    }else if(previous || !g_network_online_since)g_network_online_since=now;
    if(!offline && g_network_seen_offline && error_pending &&
       !g_network_auto_retry_done && g_network_online_since &&
       now-g_network_online_since>=2500000ull){
      g_network_auto_retry_done=true;
      int retried=ytv_network_platform_error_retry();
      log_line(retried==1?"YTV_NETWORK_AUTO_RECOVERY callback=positive\n":
                          "YTV_NETWORK_AUTO_RECOVERY no_callback\n");
      if(retried==1 && g_page==kPageNetwork)open_menu(false);
      if(retried==1)g_network_seen_offline=false;
    }
    if((offline || new_error) && !g_menu_open &&
       now>=g_network_next_prompt && now>3500000ull){
      open_page(kPageNetwork);
      g_network_next_prompt=now+7000000ull;
      log_line(offline?"YTV_NETWORK_NATIVE_DIALOG offline\n":
                       "YTV_NETWORK_NATIVE_DIALOG cobalt_load_error\n");
    }else if(g_page==kPageNetwork && previous!=offline){
      compose_panel();
    }
  }
#if defined(YTV_NATIVE_DOWNLOAD_TEST)
  static bool listing_once=false;
  if(!listing_once && now>=24000000ull){
    listing_once=true;
    g_downloads_return=kPageHome;
    open_page(kPageDownloads);
    log_line("YTV_LIFECYCLE_TEST_DOWNLOADS_PAGE_OPENED\n");
  }
#endif
  if(g_media_cfg_dirty_since && now-g_media_cfg_dirty_since>=700000ull){
    g_media_cfg_dirty_since=0;
    save_cfg();
  }
  // Countdown is a compact native HUD for exactly the final 60 seconds.
  // The long 3.06s go_to_sleep_alert sounds only once, ~4s before blackout.
  if(g_timer_deadline_us && now<g_timer_deadline_us){
    const uint64_t left=g_timer_deadline_us-now;
    g_timer_countdown_visible=left<=60000000ull;
    if(left<=4000000ull && !g_timer_final_alert_played){
      g_timer_final_alert_played=true;
      ytv_ui_sound(13);
      log_line("YTV_SLEEP_TIMER_FINAL_ALERT go_to_sleep_alert\n");
    }
  }else{
    g_timer_countdown_visible=false;
  }
  if (g_timer_deadline_us && now >= g_timer_deadline_us) {
    g_timer_deadline_us = 0;
    g_timer_idx = 0;
    g_timer_announce_pending=false;
    log_line("YTV_LIFECYCLE_ADDON sleep_timer=expired pause_black\n");
    send_key(kSbKeyPause);
    // Native Black Screen keeps audio/system alive rather than invoking
    // scePowerRequestDisplayOff, which suspends the app on Vita.
    set_screen_off(true);
    if (g_menu_open) compose_panel();
  }
  // Downloads report completion from the worker using an atomic counter.
  const unsigned downgrade=__atomic_load_n(&g_mp4_fallback_serial,__ATOMIC_ACQUIRE);
  if(downgrade!=g_mp4_fallback_displayed && !overlay_visible()){
    g_mp4_fallback_displayed=downgrade;
    const int requested=__atomic_load_n(&g_mp4_fallback_requested,__ATOMIC_ACQUIRE);
    const int actual=__atomic_load_n(&g_mp4_fallback_actual,__ATOMIC_ACQUIRE);
    char message[104];
    snprintf(message,sizeof(message),
      g_pt ? "%dp indisponível; MP4 em %dp" : "%dp unavailable; MP4 at %dp",
      requested,actual);
    show_toast(message,3600,0);
    log_line("YTV_ADDON_TOAST_DOWNLOAD_QUALITY_DOWNGRADE\n");
  }
  const unsigned completed=__atomic_load_n(&g_downloads_completed,__ATOMIC_ACQUIRE);
  if(completed!=g_downloads_completed_notified && !overlay_visible()){
    g_downloads_completed_notified=completed;
    show_toast(T("Downloads completed","Downloads concluídos"),3300,0);
    log_line("YTV_ADDON_TOAST_DOWNLOADS_COMPLETED\n");
  }
  // Low-battery warning once at <=15%; re-arm only after a recovery above
  // 20% to avoid repeated sounds when battery fluctuates around 15%.
  {
    static uint64_t battery_check=0;
    if(now-battery_check>=5000000ull){
      battery_check=now;
      const int pct=scePowerGetBatteryLifePercent();
      const bool charging=scePowerIsBatteryCharging()>0;
      if(pct>=0 && pct<=15 && !charging && !g_battery_warned){
        g_battery_warned=true;
        g_battery_pending=true;
        log_line("YTV_ADDON_BATTERY_LOW threshold=15\n");
      }else if(charging || pct>20){
        g_battery_warned=false;
      }
    }
    if(g_battery_pending && !overlay_visible()){
      g_battery_pending=false;
      ytv_ui_sound(18);
      show_toast(T("Low battery (15% or less)",
                   "Bateria fraca (15% ou menos)"),3500,0);
    }
  }
  if (g_screen_off_at_us && now >= g_screen_off_at_us) {
    g_screen_off_at_us = 0;
    set_screen_off(true);
  }
  if (g_menu_open) {
    if(g_page==kPageDownloads &&
       ytv_native_download_revision()!=g_download_seen_revision)
      refresh_download_list(); // disk scan on queue transition/completion only
    static uint64_t last_refresh = 0;  // clock, battery, timer minutes
    if (now - last_refresh > 1000000ull) { last_refresh = now; compose_panel(); }
  }
  {
    static uint64_t last_hud = 0;
    static unsigned last_dropped = 0;
    if (now - last_hud >= 1000000ull) {
      unsigned d = g_ytv_hud_dropped_total;
      g_dropped_per_s = last_hud ? (int)(d - last_dropped) : 0;
      last_dropped = d;
      last_hud = now;
      if (g_perf_hud || g_clock_hud || g_timer_countdown_visible)
        compose_hud();
      else g_hud_w = 0;
    }
  }
  // Full HD guard is now scoped to an actual 1080p decoder, not app uptime.
  // Keep the system awake while audio plays: with the screen off only block
  // auto-suspend; otherwise also block dimming.
  static uint64_t last_tick = 0;
  bool playing = g_ytv_audio_active_us && (int64_t)(now - g_ytv_audio_active_us) < 3000000;
  if (playing && now - last_tick > 5000000ull) {
    last_tick = now;
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);  // no dimming, no auto-lock
  }
  // Screenshot after the menu is gone from the screen.
  if (g_shot_pending && now >= g_shot_at_us && !g_painted_base) {
    g_shot_pending = false;
    save_screenshot();
  }
  if (g_screen_off) black_idle(now);
  else if (overlay_visible() &&
           (!g_ytv_graphics_ext_active ||
            // Cobalt sometimes stops submitting any frame after media or
            // navigation transitions while native input/audio still work.
            // Only use the CPU emergency paint once >=1.5 seconds elapsed
            // since the last real swap. Never race an actively drawing GPU.
            (g_last_swap_us &&
             (int64_t)(now-g_last_swap_us)>1500000)))
  {
    static uint64_t last_fallback_log=0;
    if(now-last_fallback_log>5000000ull){
      last_fallback_log=now;
      char line[128];
      snprintf(line,sizeof(line),
        "YTV_ADDON_NO_SWAP_FALLBACK elapsed_ms=%llu page=%d\n",
        (unsigned long long)((now-g_last_swap_us)/1000),(int)g_page);
      log_line(line);
    }
    idle_paint(now);
  }else restore_idle_paint();
  // Emergency CPU paint is reserved for genuinely stalled frame submission.
  // Cobalt continues to own GL and can resume normally after new input.
}

}  // extern "C"

// ---- Renderer thread: GL overlay ------------------------------------------
namespace {
GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

bool init_gl() {
  static const char* vs =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  static const char* fs =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_tex;\n"
      "uniform float u_alpha;\n"
      "void main() { vec4 c = texture2D(u_tex, v_uv); gl_FragColor = vec4(c.rgb, c.a * u_alpha); }\n";
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return false;
  g_prog = glCreateProgram();
  glAttachShader(g_prog, v);
  glAttachShader(g_prog, f);
  glBindAttribLocation(g_prog, 0, "a_pos");
  glBindAttribLocation(g_prog, 1, "a_uv");
  glLinkProgram(g_prog);
  GLint ok = 0;
  glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
  if (!ok) return false;
  g_loc_pos = glGetAttribLocation(g_prog, "a_pos");
  g_loc_uv = glGetAttribLocation(g_prog, "a_uv");
  g_loc_tex = glGetUniformLocation(g_prog, "u_tex");
  g_loc_alpha = glGetUniformLocation(g_prog, "u_alpha");
  glGenTextures(1, &g_tex);
  glGenTextures(1, &g_tex_hud);
  glGenTextures(1, &g_tex_black);
  static const uint8_t black[16] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
  glBindTexture(GL_TEXTURE_2D, g_tex_black);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, g_tex_hud);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kHudW, kHudH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return g_tex != 0 && g_loc_pos >= 0 && g_loc_uv >= 0;
}

struct AttribState {
  GLint enabled, size, type, normalized, stride, buffer;
  void* pointer;
};
void save_attrib(GLuint i, AttribState& a) {
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a.enabled);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a.size);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a.type);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a.normalized);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a.stride);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a.buffer);
  a.pointer = nullptr;
  glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a.pointer);
}
void restore_attrib(GLuint i, const AttribState& a) {
  if (a.size > 0 && (a.buffer || a.pointer)) {
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a.buffer);
    glVertexAttribPointer(i, a.size, (GLenum)a.type, (GLboolean)a.normalized, a.stride, a.pointer);
  }
  if (a.enabled) glEnableVertexAttribArray(i);
  else glDisableVertexAttribArray(i);
}
}  // namespace

// ---- v0.64 Vertical mode: rotated portrait video over the whole screen ----
extern "C" void* ytv_player_vertical_acquire(unsigned tex[3], int* w, int* h, int* tw, int* th);
extern "C" void ytv_player_vertical_release(void* t);
extern "C" bool ytv_osk_active();
void draw_quad(GLuint tex, int x, int y, int w, int h, float us, float vs, float alpha);
namespace {
GLuint g_yuv_prog = 0;
GLint g_yuv_pos = -1, g_yuv_uv = -1;
bool g_yuv_failed = false;
volatile uint32_t g_vertical_drawn_ms = 0;  // last frame the vertical picture was drawn
bool init_yuv_gl() {
  static const char* vs =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  // The planes are GL_ALPHA textures (decoder memory, see port_player.cpp).
  // BT.709, limited range (YouTube's H.264 streams).
  static const char* fs =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_y;\n"
      "uniform sampler2D u_u;\n"
      "uniform sampler2D u_v;\n"
      "void main() {\n"
      "  float y = 1.1644 * (texture2D(u_y, v_uv).a - 0.0627);\n"
      "  float u = texture2D(u_u, v_uv).a - 0.5;\n"
      "  float v = texture2D(u_v, v_uv).a - 0.5;\n"
      "  gl_FragColor = vec4(y + 1.7927 * v, y - 0.2132 * u - 0.5329 * v, y + 2.1124 * u, 1.0);\n"
      "}\n";
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return false;
  g_yuv_prog = glCreateProgram();
  glAttachShader(g_yuv_prog, v);
  glAttachShader(g_yuv_prog, f);
  glBindAttribLocation(g_yuv_prog, 0, "a_pos");
  glBindAttribLocation(g_yuv_prog, 1, "a_uv");
  glLinkProgram(g_yuv_prog);
  GLint ok = 0;
  glGetProgramiv(g_yuv_prog, GL_LINK_STATUS, &ok);
  if (!ok) return false;
  g_yuv_pos = glGetAttribLocation(g_yuv_prog, "a_pos");
  g_yuv_uv = glGetAttribLocation(g_yuv_prog, "a_uv");
  glUseProgram(g_yuv_prog);
  glUniform1i(glGetUniformLocation(g_yuv_prog, "u_y"), 0);
  glUniform1i(glGetUniformLocation(g_yuv_prog, "u_u"), 1);
  glUniform1i(glGetUniformLocation(g_yuv_prog, "u_v"), 2);
  return g_yuv_pos >= 0 && g_yuv_uv >= 0;
}
// Whether the vertical picture should cover the screen this frame.
bool vertical_wanted() {
  const int mode = __atomic_load_n(&g_ytv_video_vertical_mode, __ATOMIC_RELAXED);
  return (mode == 1 || mode == 2) && !g_menu_open && !g_screen_off && !ytv_osk_active();
}
// Draws the frame turned 90 degrees, fitted (aspect kept) to the screen.
// Called with the overlay's GL state already saved. Returns true if drawn.
bool draw_vertical_video() {
  unsigned tex[3];
  int w = 0, h = 0, tw = 0, th = 0;
  void* ref = ytv_player_vertical_acquire(tex, &w, &h, &tw, &th);
  if (!ref) return false;
  bool drawn = false;
  if (!g_yuv_prog && !g_yuv_failed && !init_yuv_gl()) {
    g_yuv_failed = true;
    log_line("YTV_LIFECYCLE_VIDEO_VERTICAL overlay=failed (shader)\n");
  }
  if (g_yuv_prog && !g_yuv_failed && w > 0 && h > 0 && tw > 0 && th > 0) {
    const int mode = __atomic_load_n(&g_ytv_video_vertical_mode, __ATOMIC_RELAXED);
    // Rotated picture is h x w on screen.
    float dw = kScreenW, dh = (float)kScreenW * w / h;
    if (dh > kScreenH) { dh = kScreenH; dw = (float)kScreenH * h / w; }
    const float x0 = -dw / kScreenW, x1 = dw / kScreenW, y0 = -dh / kScreenH, y1 = dh / kScreenH;
    const float us = (float)w / tw, vs = (float)h / th;
    // Screen corners (bottom-left, bottom-right, top-left, top-right) and the
    // source texel each shows. Clockwise: source top edge -> screen right.
    float uv[8];
    if (mode == 1) {
      const float c[8] = {us, vs, us, 0.f, 0.f, vs, 0.f, 0.f};
      memcpy(uv, c, sizeof(c));
    } else {
      const float c[8] = {0.f, 0.f, 0.f, vs, us, 0.f, us, vs};
      memcpy(uv, c, sizeof(c));
    }
    static float verts[16];
    const float pos[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
    for (int i = 0; i < 4; i++) {
      verts[i * 4 + 0] = pos[i * 2]; verts[i * 4 + 1] = pos[i * 2 + 1];
      verts[i * 4 + 2] = uv[i * 2]; verts[i * 4 + 3] = uv[i * 2 + 1];
    }
    // Black bars, then the picture.
    glUseProgram(g_prog);
    draw_quad(g_tex_black, 0, 0, kScreenW, kScreenH, 1.f, 1.f, 1.f);
    glUseProgram(g_yuv_prog);
    glDisable(GL_BLEND);
    // The player's texture filter is used as is: changing parameters of the
    // live video textures correlated with a hardware crash (port_player.cpp).
    for (int i = 2; i >= 0; i--) {
      glActiveTexture(GL_TEXTURE0 + i);
      glBindTexture(GL_TEXTURE_2D, tex[i]);
    }
    glVertexAttribPointer((GLuint)g_yuv_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
    glVertexAttribPointer((GLuint)g_yuv_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEnable(GL_BLEND);
    glUseProgram(g_prog);
    drawn = true;
    __atomic_store_n(&g_vertical_drawn_ms, (uint32_t)(now_us() / 1000), __ATOMIC_RELAXED);
    static int logged_mode = 0;
    if (logged_mode != mode) {
      logged_mode = mode;
      char line[120];
      snprintf(line, sizeof(line), "YTV_LIFECYCLE_VIDEO_VERTICAL overlay=1 dir=%s picture=%dx%d\n",
               mode == 1 ? "cw" : "ccw", w, h);
      log_line(line);
    }
  }
  ytv_player_vertical_release(ref);
  return drawn;
}
}  // namespace

// ---- v0.64 inverted screen ------------------------------------------------
namespace {
GLuint g_flip_fbo = 0, g_flip_tex = 0, g_flip_rb = 0;
bool g_flip_failed = false;
volatile bool g_flip_on = false;  // the frame being drawn goes to g_flip_fbo
bool create_flip_target() {
  GLint old_fb = 0, old_tex = 0, old_rb = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
  glGenTextures(1, &g_flip_tex);
  glBindTexture(GL_TEXTURE_2D, g_flip_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kScreenW, kScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &g_flip_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, g_flip_rb);
  glRenderbufferStorage(GL_RENDERBUFFER, 0x88F0 /*GL_DEPTH24_STENCIL8*/, kScreenW, kScreenH);
  glGenFramebuffers(1, &g_flip_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, g_flip_fbo);
  // Depth/stencil BEFORE the color texture: vitaGL only flags the
  // framebuffer in glFramebufferRenderbuffer and creates the depth/stencil
  // surface when the color attachment is set (libvitaGL disassembly). In the
  // other order Skia's stencil clips failed: text inside rounded boxes was
  // missing.
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, 0x821A /*GL_DEPTH_STENCIL_ATTACHMENT*/,
                            GL_RENDERBUFFER, g_flip_rb);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_flip_tex, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fb);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex);
  glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)old_rb);
  char line[96];
  snprintf(line, sizeof(line), "YTV_LIFECYCLE_SCREEN_FLIP target status=0x%x\n", (unsigned)status);
  log_line(line);
  return status == GL_FRAMEBUFFER_COMPLETE;
}
}  // namespace
// Default framebuffer for Cobalt and the overlay (port_graphics.cpp).
extern "C" unsigned ytv_screen_fbo() { return g_flip_on ? g_flip_fbo : 0; }
// After each swap (GL thread): choose where the next frame is drawn.
extern "C" void ytv_addons_after_swap() {
  const bool want = __atomic_load_n(&g_ytv_screen_flip, __ATOMIC_RELAXED) && !g_flip_failed;
  if (want && !g_flip_fbo && !create_flip_target()) {
    g_flip_failed = true;
    log_line("YTV_LIFECYCLE_SCREEN_FLIP failed (framebuffer)\n");
  }
  const bool on = want && !g_flip_failed && g_flip_fbo;
  if (on == g_flip_on) return;
  // Cobalt keeps its framebuffer binding across frames: move it with us.
  GLint fb = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
  const GLint old_default = g_flip_on ? (GLint)g_flip_fbo : 0;
  g_flip_on = on;
  if (fb == old_default) glBindFramebuffer(GL_FRAMEBUFFER, on ? g_flip_fbo : 0);
}
// Draws the offscreen frame onto the real screen turned 180 degrees.
void draw_flipped_screen() {
  // Corners bottom-left, bottom-right, top-left, top-right. The offscreen
  // texture's first row is the bottom of the frame (GL convention); a 180
  // degree turn shows texel (1-u, 1-v) where the frame had (u, v).
  static float verts[16];
  const float v[16] = {-1.f, -1.f, 1.f, 1.f,   1.f, -1.f, 0.f, 1.f,
                       -1.f,  1.f, 1.f, 0.f,   1.f,  1.f, 0.f, 0.f};
  memcpy(verts, v, sizeof(v));
  glBindTexture(GL_TEXTURE_2D, g_flip_tex);
  if (g_loc_alpha >= 0) glUniform1f(g_loc_alpha, 1.f);
  glVertexAttribPointer((GLuint)g_loc_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
  glVertexAttribPointer((GLuint)g_loc_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Touch (port_input.cpp): rotation of the vertical picture shown right now,
// 0 when it is not on screen (1 clockwise, 2 counter-clockwise).
extern "C" int ytv_addons_vertical_dir() {
  const uint32_t drawn = __atomic_load_n(&g_vertical_drawn_ms, __ATOMIC_RELAXED);
  if (!drawn || (uint32_t)(now_us() / 1000) - drawn > 300) return 0;
  const int mode = __atomic_load_n(&g_ytv_video_vertical_mode, __ATOMIC_RELAXED);
  return mode == 1 || mode == 2 ? mode : 0;
}

// One textured quad in screen pixels (top-left origin); us/vs = texture extent.
void draw_quad(GLuint tex, int x, int y, int w, int h, float us, float vs, float alpha) {
  static float verts[16];
  const float x0 = (float)x / kScreenW * 2.f - 1.f, x1 = (float)(x + w) / kScreenW * 2.f - 1.f;
  const float y1 = 1.f - (float)y / kScreenH * 2.f, y0 = 1.f - (float)(y + h) / kScreenH * 2.f;
  const float v[16] = {x0, y0, 0.f, vs, x1, y0, us, vs, x0, y1, 0.f, 0.f, x1, y1, us, 0.f};
  memcpy(verts, v, sizeof(v));
  glBindTexture(GL_TEXTURE_2D, tex);
  if (g_loc_alpha >= 0) glUniform1f(g_loc_alpha, alpha);
  glVertexAttribPointer((GLuint)g_loc_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
  glVertexAttribPointer((GLuint)g_loc_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

extern "C" void ytv_addons_before_swap() {
  const uint64_t swap_now = now_us();
  {  // frame rate for the HUD
    static uint64_t window_start = 0, prev = 0;
    static int frames = 0, worst = 0;
    if (prev) { int ms = (int)((swap_now - prev) / 1000); if (ms > worst && ms < 1000) worst = ms; }
    prev = swap_now;
    frames++;
    if (!window_start) window_start = swap_now;
    if (swap_now - window_start >= 1000000ull) {
      g_fps = frames; g_worst_ms = worst;
      frames = 0; worst = 0; window_start = swap_now;
    }
  }
  g_last_swap_us = swap_now;
  const int dim = kDimLevels[g_dim_idx];
  const bool hud = (g_perf_hud || g_clock_hud || g_timer_countdown_visible) &&
                   g_hud_w > 0;
  const bool panel = overlay_visible();
  const bool black = g_screen_off;
  const bool vertical = vertical_wanted();
  const bool flip = g_flip_on;
  if (g_gl_failed || !g_img || (!panel && !dim && !hud && !black && !vertical && !flip)) return;
  // Save the state Cobalt/Skia relies on BEFORE init_gl(): initialising the
  // overlay creates textures and changes GL_TEXTURE_BINDING_2D. Capturing state
  // only afterwards restored the overlay texture as Cobalt's texture, causing
  // corrupted thumbnails/textures after visiting Add-ons.
  GLint prog, active, tex0, tex1 = 0, tex2 = 0, abuf, fbo, vp[4], sc[4], bsrc_rgb, bdst_rgb, bsrc_a, bdst_a, beq_rgb, beq_a;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  if (vertical) {  // the YUV draw also uses texture units 1 and 2
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2);
  }
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetIntegerv(GL_SCISSOR_BOX, sc);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beq_a);
  GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST),
            depth = glIsEnabled(GL_DEPTH_TEST), stencil = glIsEnabled(GL_STENCIL_TEST),
            cull = glIsEnabled(GL_CULL_FACE);
  GLboolean color_mask[4] = {GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE};
  glGetBooleanv(GL_COLOR_WRITEMASK,color_mask);
  AttribState a0, a1;
  save_attrib(0, a0);
  save_attrib(1, a1);

  if (!g_prog && !init_gl()) {
    g_gl_failed = true;
    log_line("YTV_LIFECYCLE_ADDON gl_overlay=failed (idle path only)\n");
    goto restore_overlay_state;
  }
  glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  glBindFramebuffer(GL_FRAMEBUFFER, ytv_screen_fbo());
  glViewport(0, 0, kScreenW, kScreenH);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
  glUseProgram(g_prog);
  if (g_loc_tex >= 0) glUniform1i(g_loc_tex, 0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray((GLuint)g_loc_pos);
  glEnableVertexAttribArray((GLuint)g_loc_uv);
  // 0. Vertical mode: the portrait video, rotated, over the whole screen.
  if (vertical && draw_vertical_video()) glActiveTexture(GL_TEXTURE0);
  // 1. Internal darkening: black over the whole frame (not the backlight).
  //    "Black screen" mode: fully black, nothing else drawn on top.
  if (black) draw_quad(g_tex_black, 0, 0, kScreenW, kScreenH, 1.f, 1.f, 1.f);
  else if (dim) draw_quad(g_tex_black, 0, 0, kScreenW, kScreenH, 1.f, 1.f, dim / 100.f);
  // 2. HUD, top-right corner.
  if (hud && !black) {
    pthread_mutex_lock(&g_overlay_image_lock);
    const int w = g_hud_w, h = g_hud_h;
    uint32_t gen = g_hud_gen;
    if (gen != g_hud_tex_gen) {
      g_hud_tex_gen = gen;
      glBindTexture(GL_TEXTURE_2D, g_tex_hud);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_hud);
    }
    pthread_mutex_unlock(&g_overlay_image_lock);
    draw_quad(g_tex_hud, kScreenW - w - 10, 10, w, h, (float)w / kHudW, (float)h / kHudH, 1.f);
  }
  // 3. Backdrop: draw one translucent black quad behind the OPEN menu.
  // Reuses the existing black texture. Toasts are never dimmed.
  if(g_menu_open && !black)
    draw_quad(g_tex_black,0,0,kScreenW,kScreenH,1.f,1.f,
              kMenuBackdropOpacity);
  // 4. Menu / toast, centred.
  if (panel) {
    pthread_mutex_lock(&g_overlay_image_lock);
    const int w = g_img_w, h = g_img_h;
    glBindTexture(GL_TEXTURE_2D, g_tex);
    uint32_t gen = g_img_gen;
    if (gen != g_tex_gen) {
      g_tex_gen = gen;
      static bool allocated = false;
      if (!allocated) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kPanelW, kPanelH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        allocated = true;
      }
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_img);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    pthread_mutex_unlock(&g_overlay_image_lock);
    draw_quad(g_tex, (kScreenW - w) / 2, (kScreenH - h) / 2, w, h, (float)w / kPanelW,
              (float)h / kPanelH, 1.f);
  }

  // 5. Inverted screen: everything above went to the offscreen frame; show
  //    it on the real screen turned 180 degrees.
  if (flip) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    draw_flipped_screen();
  }

  // Restore every state touched by the overlay, including the very first
  // frame when shaders and textures are created.
restore_overlay_state:
  restore_attrib(0, a0);
  restore_attrib(1, a1);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
  glUseProgram((GLuint)prog);
  if (vertical) {
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex2);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex1);
    glActiveTexture(GL_TEXTURE0);
  }
  glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
  glActiveTexture((GLenum)active);
  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)fbo);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glScissor(sc[0], sc[1], sc[2], sc[3]);
  glBlendEquationSeparate((GLenum)beq_rgb, (GLenum)beq_a);
  glBlendFuncSeparate((GLenum)bsrc_rgb, (GLenum)bdst_rgb, (GLenum)bsrc_a, (GLenum)bdst_a);
  glColorMask(color_mask[0],color_mask[1],color_mask[2],color_mask[3]);
  if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
  if (scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
  if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  if (stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
  if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}
