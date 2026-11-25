/*
 * Vita keyboard for text fields (search, sign-in codes...): Cobalt's
 * OnScreenKeyboard extension, answered with the system IME dialog.
 *
 * Cobalt calls Show/Hide/Focus/Blur from its own threads; the IME dialog and
 * every event back to Cobalt run on the main thread (ytv_osk_tick).
 *
 * Optional, off by default (beta): addons.cfg vita_keyboard=1 turns it on
 * at the next start (Cobalt checks the extension once when it starts).
 * Not verified yet that YouTube TV asks for a platform keyboard.
 */
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "starboard/event.h"
#include "starboard/input.h"
#include "starboard/log.h"
#include "starboard/window.h"
#include "starboard/extension/on_screen_keyboard.h"

#ifndef kSbEventOnScreenKeyboardInvalidTicket
#define kSbEventOnScreenKeyboardInvalidTicket (-1)
#endif

extern "C" void ytv_vita_lifecycle_send(SbEventType type, void* data);
extern "C" SbWindow ytv_vita_current_window();
extern "C" bool ytv_download_config_read(int field, char* out, size_t cap);
extern "C" bool ytv_download_config_write(int field, const char* value);
extern "C" bool ytv_native_download_set_video(const char* input);
extern "C" int ytv_native_download_start(bool mp3,int quality);
extern "C" int ytv_addons_cfg_download_quality();
extern "C" bool ytv_addons_cfg_download_mp3();
extern "C" void ytv_ui_sound(int kind); // 16=typing,17=delete

namespace {
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// Requests from Cobalt's threads (ticket >= 0 means pending).
int g_show_ticket = -1, g_hide_ticket = -1, g_focus_ticket = -1, g_blur_ticket = -1;
char g_show_text[512];
bool g_shown = false;      // as far as Cobalt knows
volatile bool g_ime_running = false;
int g_ime_last_length=-1; // reset on each new system IME
int g_native_config_field = -1;  // 0=legacy server, 1=legacy key, 2=native video URL

// IME dialog buffers (UTF-16).
SceWChar16 g_ime_input[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
SceWChar16 g_ime_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
SceWChar16 g_ime_title[64];

void log_line(const char* fmt, int a, int b) {
  char line[128];
  snprintf(line, sizeof(line), fmt, a, b);
  SbLogRaw(line);
}

void utf8_to_utf16(const char* in, SceWChar16* out, size_t cap) {
  size_t n = 0;
  const unsigned char* s = (const unsigned char*)(in ? in : "");
  while (*s && n + 1 < cap) {
    uint32_t cp = *s++;
    if (cp >= 0xc0) {
      int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : 1;
      cp &= (0x3f >> extra);
      while (extra-- && (*s & 0xc0) == 0x80) cp = (cp << 6) | (*s++ & 0x3f);
    }
    if (cp > 0xffff) cp = '?';  // the IME edits the BMP only
    out[n++] = (SceWChar16)cp;
  }
  out[n] = 0;
}
void utf16_to_utf8(const SceWChar16* in, char* out, size_t cap) {
  size_t n = 0;
  for (; *in && n + 4 < cap; in++) {
    uint32_t cp = *in;
    if (cp < 0x80) out[n++] = (char)cp;
    else if (cp < 0x800) { out[n++] = (char)(0xc0 | (cp >> 6)); out[n++] = (char)(0x80 | (cp & 0x3f)); }
    else {
      out[n++] = (char)(0xe0 | (cp >> 12));
      out[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      out[n++] = (char)(0x80 | (cp & 0x3f));
    }
  }
  out[n] = 0;
}

void send_ticket_event(SbEventType type, int ticket) {
  int t = ticket;
  ytv_vita_lifecycle_send(type, &t);
}

// Replaces the field's text with |text| (Cobalt applies "input" events).
void send_text(const char* text) {
  SbWindow window = ytv_vita_current_window();
  if (!SbWindowIsValid(window)) return;
  SbInputData e = {};
  e.window = window;
  e.type = kSbInputEventTypeInput;
  e.device_type = kSbInputDeviceTypeOnScreenKeyboard;
  e.input_text = text;
  e.is_composing = false;
  ytv_vita_lifecycle_send(kSbEventTypeInput, &e);
}

bool start_ime(const char* initial) {
  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  utf8_to_utf16(initial, g_ime_initial, SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1);
  utf8_to_utf16(g_native_config_field == 0 ? "Cobalt server" :
                g_native_config_field == 1 ? "Cobalt API key" :
                g_native_config_field == 2 ? "Video URL" : "YouTube", g_ime_title, 64);
  memset(g_ime_input, 0, sizeof(g_ime_input));
  g_ime_last_length=-1;
  p.supportedLanguages = 0;  // all
  p.languagesForced = SCE_FALSE;
  p.type = SCE_IME_TYPE_DEFAULT;
  p.option = 0;
  p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
  p.maxTextLength = g_native_config_field >= 0 ? 180 : 128;
  p.title = g_ime_title;
  p.initialText = g_ime_initial;
  p.inputTextBuffer = g_ime_input;
  int r = sceImeDialogInit(&p);
  log_line("YTV_LIFECYCLE_OSK ime_init r=0x%x len=%d\n", r, (int)strlen(initial ? initial : ""));
  return r >= 0;
}

// --- Extension entry points (Cobalt threads) ---
void Show(SbWindow, const char* text, int ticket) {
  pthread_mutex_lock(&g_lock);
  g_show_ticket = ticket;
  snprintf(g_show_text, sizeof(g_show_text), "%s", text ? text : "");
  pthread_mutex_unlock(&g_lock);
  log_line("YTV_LIFECYCLE_OSK show ticket=%d len=%d\n", ticket, (int)strlen(text ? text : ""));
}
void Hide(SbWindow, int ticket) {
  pthread_mutex_lock(&g_lock);
  g_hide_ticket = ticket;
  pthread_mutex_unlock(&g_lock);
  log_line("YTV_LIFECYCLE_OSK hide ticket=%d%c\n", ticket, ' ');
}
void Focus(SbWindow, int ticket) {
  pthread_mutex_lock(&g_lock);
  g_focus_ticket = ticket;
  pthread_mutex_unlock(&g_lock);
}
void Blur(SbWindow, int ticket) {
  pthread_mutex_lock(&g_lock);
  g_blur_ticket = ticket;
  pthread_mutex_unlock(&g_lock);
}
void SetBackgroundColor(SbWindow, uint8_t, uint8_t, uint8_t) {}
void SetLightTheme(SbWindow, bool) {}
void UpdateSuggestions(SbWindow, const char*[], int, int) {}
bool IsShown(SbWindow) { return g_shown; }
bool SuggestionsSupported(SbWindow) { return false; }
bool GetBoundingRect(SbWindow, SbWindowRect* r) {
  if (!r) return false;
  // The IME dialog covers the lower part of the 960x544 screen.
  r->x = 0; r->y = 272; r->width = 960; r->height = 272;
  return g_shown;
}
void SetKeepFocus(SbWindow, bool) {}

const CobaltExtensionOnScreenKeyboardApi kApi = {
    kCobaltExtensionOnScreenKeyboardName, 1,
    SetBackgroundColor, SetLightTheme, Show, Hide, Focus, Blur, UpdateSuggestions,
    IsShown, SuggestionsSupported, GetBoundingRect, SetKeepFocus,
};

bool enabled_in_cfg() {
  SceUID fd = sceIoOpen("ux0:data/youtubevita-noads/addons.cfg", SCE_O_RDONLY, 0);
  if (fd < 0) return false;  // off unless turned on (beta)
  char buf[256] = {0};
  sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  return strstr(buf, "vita_keyboard=1") != nullptr;
}
}  // namespace

extern "C" {
// Opens the Vita's system IME from Add-ons without requiring a text file.
bool ytv_osk_open_download_config(int field) {
  if (g_ime_running || g_native_config_field != -1 || (field != 0 && field != 1))
    return false;
  char initial[256] = {};
  if (field == 0) ytv_download_config_read(field, initial, sizeof(initial));
  // Never pre-fill or display an existing API key.
  g_native_config_field = field;
  if (!start_ime(initial)) {
    g_native_config_field = -1;
    return false;
  }
  g_ime_running = true;
  SbLogRaw(field == 0 ? "YTV_DOWNLOAD_SERVER_IME_OPEN\n" : "YTV_DOWNLOAD_KEY_IME_OPEN\n");
  return true;
}
// Native downloader URL input, separate from YouTube search and Cobalt Tools.
bool ytv_osk_open_native_video_url() {
  if(g_ime_running || g_native_config_field != -1)return false;
  g_native_config_field=2;
  if(!start_ime("")){
    g_native_config_field=-1;
    return false;
  }
  g_ime_running=true;
  SbLogRaw("YTV_NATIVE_DOWNLOAD_IME_OPEN\n");
  return true;
}
const void* ytv_osk_extension() {
  static int enabled = -1;
  if (enabled < 0) enabled = enabled_in_cfg() ? 1 : 0;
  return enabled ? &kApi : nullptr;
}

bool ytv_osk_active() { return g_ime_running; }

// Main thread, every loop iteration.
void ytv_osk_tick() {
  int show, hide, focus, blur;
  char text[512];
  pthread_mutex_lock(&g_lock);
  show = g_show_ticket; hide = g_hide_ticket; focus = g_focus_ticket; blur = g_blur_ticket;
  g_show_ticket = g_hide_ticket = g_focus_ticket = g_blur_ticket = -1;
  memcpy(text, g_show_text, sizeof(text));
  pthread_mutex_unlock(&g_lock);

  if (show >= 0) {
    if (!g_ime_running && start_ime(text)) g_ime_running = true;
    g_shown = true;
    send_ticket_event(kSbEventTypeOnScreenKeyboardShown, show);
    send_ticket_event(kSbEventTypeOnScreenKeyboardFocused, show);
  }
  if (focus >= 0) send_ticket_event(kSbEventTypeOnScreenKeyboardFocused, focus);
  if (blur >= 0) send_ticket_event(kSbEventTypeOnScreenKeyboardBlurred, blur);
  if (hide >= 0) {
    if (g_ime_running) { sceImeDialogAbort(); }
    g_shown = false;
    send_ticket_event(kSbEventTypeOnScreenKeyboardHidden, hide);
  }
  if (g_ime_running) {
    // The system IME does not expose per-key callbacks; on the main tick,
    // compare its live UTF-16 buffer length (only cue, no text logging).
    // Some Vita firmware updates the buffer only on ENTER, in which case
    // the cue will play for the confirmed edit, not individual keystrokes.
    int current_length=0;
    while(current_length<SCE_IME_DIALOG_MAX_TEXT_LENGTH &&
          g_ime_input[current_length])++current_length;
    if(g_ime_last_length>=0 && current_length!=g_ime_last_length)
      ytv_ui_sound(current_length<g_ime_last_length?17:16);
    g_ime_last_length=current_length;
    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st == SCE_COMMON_DIALOG_STATUS_FINISHED) {
      SceImeDialogResult res;
      memset(&res, 0, sizeof(res));
      sceImeDialogGetResult(&res);
      if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        char out[SCE_IME_DIALOG_MAX_TEXT_LENGTH * 3 + 1];
        utf16_to_utf8(g_ime_input, out, sizeof(out));
        if (g_native_config_field == 2) {
          const bool ok=ytv_native_download_set_video(out);
          SbLogRaw(ok ? "YTV_NATIVE_DOWNLOAD_ID_ACCEPTED\n" :
                          "YTV_NATIVE_DOWNLOAD_ID_REJECTED\n");
          memset(out,0,sizeof(out));
          if(ok) ytv_native_download_start(ytv_addons_cfg_download_mp3(),
                                            ytv_addons_cfg_download_quality());
        } else if(g_native_config_field >= 0) {
          const int field = g_native_config_field;
          const bool ok = ytv_download_config_write(field, out);
          SbLogRaw(ok ? "YTV_DOWNLOAD_IME_SAVED\n" : "YTV_DOWNLOAD_IME_REJECTED\n");
          memset(out, 0, sizeof(out));
        } else {
          send_text(out);
          log_line("YTV_LIFECYCLE_OSK text len=%d%c\n", (int)strlen(out), ' ');
        }
      }
      sceImeDialogTerm();
      g_ime_last_length=-1;
      g_native_config_field = -1;
      g_ime_running = false;
      if (g_shown) {
        g_shown = false;
        send_ticket_event(kSbEventTypeOnScreenKeyboardBlurred, kSbEventOnScreenKeyboardInvalidTicket);
        send_ticket_event(kSbEventTypeOnScreenKeyboardHidden, kSbEventOnScreenKeyboardInvalidTicket);
      }
    }
  }
}
}  // extern "C"
